#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <unistd.h>

#include "zeus/map/geojson_exporter.h"
#include "zeus/map/map_builder.h"
#include "zeus/map/map_runtime.h"
#include "zeus/map/map_serializer.h"
#include "zeus/map/map_validator.h"
#include "zeus/map/osm_road_preprocessor.h"
#include "zeus/map/shapefile_importer.h"

namespace {

struct DatasetCloser {
    void operator()(GDALDataset* dataset) const {
        if (dataset != nullptr) {
            GDALClose(dataset);
        }
    }
};

using DatasetPtr = std::unique_ptr<GDALDataset, DatasetCloser>;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test failure: " + message);
    }
}

void addRoad(
    OGRLayer& layer,
    const std::string& id,
    const std::string& oneway,
    double speed,
    int level,
    const std::vector<zeus::map::Point2d>& points) {
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
        OGRFeature::CreateFeature(layer.GetLayerDefn()), &OGRFeature::DestroyFeature);
    feature->SetField("ID", id.c_str());
    feature->SetField("ONEWAY", oneway.c_str());
    feature->SetField("SPEED", speed);
    feature->SetField("CLASS", "primary");
    feature->SetField("LEVEL", level);

    OGRLineString line;
    for (const auto point : points) {
        line.addPoint(point.x, point.y);
    }
    feature->SetGeometry(&line);
    require(layer.CreateFeature(feature.get()) == OGRERR_NONE, "create SHP road feature");
}

std::filesystem::path createFixture(const std::filesystem::path& directory) {
    GDALAllRegister();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    require(driver != nullptr, "ESRI Shapefile GDAL driver is available");

    const std::filesystem::path shp = directory / "roads.shp";
    DatasetPtr dataset(driver->Create(shp.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    require(dataset != nullptr, "create SHP dataset");

    OGRSpatialReference spatial_reference;
    spatial_reference.importFromEPSG(3857);
    spatial_reference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRLayer* layer = dataset->CreateLayer(
        "roads", &spatial_reference, wkbLineString, nullptr);
    require(layer != nullptr, "create roads layer");

    OGRFieldDefn id("ID", OFTString);
    OGRFieldDefn oneway("ONEWAY", OFTString);
    OGRFieldDefn speed("SPEED", OFTReal);
    OGRFieldDefn road_class("CLASS", OFTString);
    OGRFieldDefn level("LEVEL", OFTInteger);
    require(layer->CreateField(&id) == OGRERR_NONE, "create ID field");
    require(layer->CreateField(&oneway) == OGRERR_NONE, "create ONEWAY field");
    require(layer->CreateField(&speed) == OGRERR_NONE, "create SPEED field");
    require(layer->CreateField(&road_class) == OGRERR_NONE, "create CLASS field");
    require(layer->CreateField(&level) == OGRERR_NONE, "create LEVEL field");

    addRoad(*layer, "A", "both", 50.0, 0, {{0.0, 0.0}, {100.0, 0.0}});
    addRoad(*layer, "B", "forward", 30.0, 0, {{50.0, -50.0}, {50.0, 50.0}});
    addRoad(*layer, "C", "both", 40.0, 0, {{100.2, 0.0}, {200.0, 0.0}});
    return shp;
}

std::filesystem::path createReferenceFixture(const std::filesystem::path& directory) {
    GDALAllRegister();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    require(driver != nullptr, "ESRI Shapefile driver is available for reference fixture");

    const std::filesystem::path path = directory / "boundary.shp";
    DatasetPtr dataset(driver->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    require(dataset != nullptr, "create reference polygon dataset");
    OGRSpatialReference spatial_reference;
    spatial_reference.importFromEPSG(3857);
    spatial_reference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRLayer* layer = dataset->CreateLayer(
        "boundary", &spatial_reference, wkbPolygon, nullptr);
    require(layer != nullptr, "create reference polygon layer");
    OGRFieldDefn name("NAME", OFTString);
    require(layer->CreateField(&name) == OGRERR_NONE, "create reference name field");

    OGRLinearRing ring;
    ring.addPoint(0.0, 0.0);
    ring.addPoint(100.0, 0.0);
    ring.addPoint(100.0, 100.0);
    ring.addPoint(0.0, 100.0);
    ring.addPoint(0.0, 0.0);
    OGRPolygon polygon;
    polygon.addRing(&ring);
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
        OGRFeature::CreateFeature(layer->GetLayerDefn()), &OGRFeature::DestroyFeature);
    feature->SetField("NAME", "Test boundary");
    feature->SetGeometry(&polygon);
    require(layer->CreateFeature(feature.get()) == OGRERR_NONE, "create reference feature");
    return path;
}

void runEndToEndTest() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("zeus-map-test-" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::create_directories(directory);
    try {
        const std::filesystem::path shp = createFixture(directory);
        const std::filesystem::path reference_shp = createReferenceFixture(directory);

        const std::filesystem::path reference_geojson = directory / "reference.geojson";
        require(
            zeus::map::GeoJsonExporter::saveReference(
                reference_shp.string(), reference_geojson.string()) == 1,
            "export one reference feature");
        DatasetPtr reference(static_cast<GDALDataset*>(GDALOpenEx(
            reference_geojson.c_str(),
            GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr,
            nullptr,
            nullptr)));
        require(reference != nullptr, "open exported reference GeoJSON");
        OGRLayer* reference_layer = reference->GetLayer(0);
        require(reference_layer->GetFeatureCount(TRUE) == 1, "preserve reference feature count");
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> reference_feature(
            reference_layer->GetNextFeature(), &OGRFeature::DestroyFeature);
        require(reference_feature != nullptr, "read exported reference feature");
        require(
            std::string(reference_feature->GetFieldAsString("NAME")) == "Test boundary",
            "preserve reference attributes");
        const OGREnvelope envelope = [&]() {
            OGREnvelope result;
            reference_feature->GetGeometryRef()->getEnvelope(&result);
            return result;
        }();
        require(envelope.MaxX < 0.001, "transform projected reference data to WGS84");

        zeus::map::ImportOptions options;
        options.id_field = "ID";
        options.oneway_field = "ONEWAY";
        options.speed_field = "SPEED";
        options.road_class_field = "CLASS";
        options.z_level_field = "LEVEL";
        options.snap_tolerance_m = 0.5;
        const std::filesystem::path turns = directory / "turns.csv";
        {
            std::ofstream output(turns);
            output << "# from,via_x,via_y,to,type,penalty_s\n"
                   << "A,50,0,B,penalty,2.5\n";
        }
        options.turn_restrictions_file = turns.string();

        zeus::map::ImportedRoads imported =
            zeus::map::ShapefileImporter().importFile(shp.string(), options);
        require(imported.roads.size() == 3, "import all source roads");
        imported.roads.front().lane_count = 3;

        zeus::map::BuildResult build = zeus::map::MapBuilder().build(imported);
        require(build.map.nodes.size() == 6, "intersection and endpoint snapping produce six nodes");
        require(build.map.edges.size() == 8, "directions and intersection splitting produce eight edges");
        // Two sidecar entries plus six generated U-turn penalties (two at the
        // interior junction, one at each of the four dead-end endpoints).
        require(build.map.turn_transitions.size() == 8,
                "source-CRS turn sidecar resolves both directed approaches");

        const zeus::map::ValidationReport report =
            zeus::map::MapValidator().validate(build.map, build.issues);
        require(!report.hasFatalErrors(), "generated map has no fatal validation issues");
        require(report.component_count == 1, "generated map is weakly connected");

        const std::filesystem::path runtime_path = directory / "roads.zmap";
        zeus::map::MapSerializer::save(build.map, runtime_path.string());
        const std::filesystem::path geojson_path = directory / "roads.geojson";
        zeus::map::GeoJsonExporter::save(build.map, geojson_path.string());
        DatasetPtr geojson(static_cast<GDALDataset*>(GDALOpenEx(
            geojson_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
        require(geojson != nullptr, "open exported GeoJSON");
        require(
            geojson->GetLayer(0)->GetFeatureCount(TRUE) == 5,
            "export one feature per road piece");
        bool found_bidirectional = false;
        bool found_oneway = false;
        geojson->GetLayer(0)->ResetReading();
        while (OGRFeature* feature = geojson->GetLayer(0)->GetNextFeature()) {
            const std::string direction = feature->GetFieldAsString("DIRECTION");
            found_bidirectional = found_bidirectional || direction == "both";
            found_oneway = found_oneway || direction == "forward";
            OGRFeature::DestroyFeature(feature);
        }
        require(found_bidirectional, "export bidirectional road semantics");
        require(found_oneway, "export one-way road direction semantics");
        geojson.reset();

        const std::string geojson_inspection =
            zeus::map::ShapefileImporter::inspect(geojson_path.string());
        require(
            geojson_inspection.find("driver=GeoJSON") != std::string::npos,
            "inspect GeoJSON vector source");
        zeus::map::ImportOptions geojson_options;
        geojson_options.id_field = "ROAD_ID";
        geojson_options.speed_field = "SPEED_KPH";
        geojson_options.road_class_field = "CLASS";
        geojson_options.z_level_field = "Z_LEVEL";
        const zeus::map::ImportedRoads geojson_imported =
            zeus::map::ShapefileImporter().importFile(
                geojson_path.string(), geojson_options);
        require(
            geojson_imported.roads.size() == 5,
            "import all GeoJSON LineString features");

        const std::filesystem::path nodes_path = directory / "nodes.geojson";
        zeus::map::GeoJsonExporter::saveNodes(build.map, nodes_path.string());
        DatasetPtr nodes(static_cast<GDALDataset*>(GDALOpenEx(
            nodes_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
        require(nodes != nullptr, "open exported topology node GeoJSON");
        require(
            nodes->GetLayer(0)->GetFeatureCount(TRUE) ==
                static_cast<GIntBig>(build.map.nodes.size()),
            "export every topology node");

        const std::filesystem::path issues_path = directory / "issues.geojson";
        zeus::map::GeoJsonExporter::saveIssues(build.map, report.issues, issues_path.string());
        DatasetPtr issues(static_cast<GDALDataset*>(GDALOpenEx(
            issues_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
        require(issues != nullptr, "open exported issue GeoJSON");
        std::size_t located_issue_count = 0;
        for (const zeus::map::ValidationIssue& issue : report.issues) {
            located_issue_count += issue.has_location ? 1U : 0U;
        }
        require(located_issue_count > 0, "fixture produces located validation issues");
        require(
            issues->GetLayer(0)->GetFeatureCount(TRUE) ==
                static_cast<GIntBig>(located_issue_count),
            "export only validation issues with real map locations");

        zeus::map::MapData loaded = zeus::map::MapSerializer::load(runtime_path.string());
        require(loaded.nodes.size() == build.map.nodes.size(), "round-trip node count");
        require(loaded.edges.size() == build.map.edges.size(), "round-trip edge count");
        require(loaded.edges.front().lane_count == 3, "round-trip directed lane count");
        require(loaded.turn_transitions.size() == 8 &&
                    std::abs(loaded.turn_transitions.front().penalty_s - 2.5F) < 1e-6,
                "round-trip turn transition");

        zeus::map::MapRuntime runtime(std::move(loaded));
        zeus::map::MapMatchOptions match_options;
        match_options.has_heading = true;
        match_options.heading_rad = 0.0;
        match_options.max_distance_m = 10.0;
        const auto matches = runtime.matchPoint({25.0, 2.0}, match_options);
        require(!matches.empty(), "map match returns a candidate");
        require(runtime.edge(matches.front().edge).source_id == "A", "map match selects road A");
        require(std::abs(matches.front().lateral_distance_m - 2.0) < 1e-6, "map match distance");

        zeus::map::VehicleMapPosition position;
        position.edge = matches.front().edge;
        position.offset_s = matches.front().offset_s;
        const zeus::map::WorldPose pose = runtime.worldPose(position);
        require(std::abs(pose.point.x - 25.0) < 1e-6, "edge offset converts to expected x");
        require(std::abs(pose.point.y) < 1e-6, "edge offset converts to expected y");
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
    std::filesystem::remove_all(directory);
}

void runEdgeLevelMapMatchingTest() {
    zeus::map::MapData map;
    map.nodes.push_back({1, {-10.0, 1.0}});
    map.nodes.push_back({2, {10.0, 1.0}});
    map.nodes.push_back({3, {-10.0, 2.0}});
    map.nodes.push_back({4, {10.0, 2.0}});

    zeus::map::DirectedEdge curved;
    curved.id = 10;
    curved.road_id = 10;
    curved.from = 0;
    curved.to = 1;
    curved.geometry_offset = 0;
    curved.geometry_count = 34;
    curved.length_m = 660.0;
    curved.speed_limit_mps = 10.0F;
    for (int i = 0; i < 34; ++i) {
        map.geometry_points.push_back({i % 2 == 0 ? -10.0 : 10.0, 1.0});
    }
    map.edges.push_back(curved);

    zeus::map::DirectedEdge nearby;
    nearby.id = 11;
    nearby.road_id = 11;
    nearby.from = 2;
    nearby.to = 3;
    nearby.geometry_offset = static_cast<std::uint32_t>(map.geometry_points.size());
    nearby.geometry_count = 2;
    nearby.length_m = 20.0;
    nearby.speed_limit_mps = 10.0F;
    map.geometry_points.push_back({-10.0, 2.0});
    map.geometry_points.push_back({10.0, 2.0});
    map.edges.push_back(nearby);

    zeus::map::MapRuntime runtime(std::move(map));
    zeus::map::MapMatchOptions options;
    options.max_results = 2;
    options.max_distance_m = 5.0;
    const auto matches = runtime.matchPoint({0.0, 0.0}, options);
    require(matches.size() == 2, "one segmented edge cannot crowd out another nearby edge");
    require(matches[0].edge == 0 && matches[1].edge == 1,
            "edge-level range matching returns both roads in distance order");
}

void runGradeSeparatedCrossingTest() {
    zeus::map::ImportedRoads imported;
    imported.metadata.runtime_crs_wkt = "LOCAL_METRIC_TEST_CRS";
    imported.metadata.snap_tolerance_m = 0.5;

    zeus::map::SourceRoad ground;
    ground.source_id = "ground";
    ground.direction = zeus::map::Direction::kBoth;
    ground.z_level = 0;
    ground.points = {{0.0, 0.0}, {100.0, 0.0}};
    imported.roads.push_back(ground);

    zeus::map::SourceRoad bridge;
    bridge.source_id = "bridge";
    bridge.direction = zeus::map::Direction::kBoth;
    bridge.z_level = 1;
    bridge.points = {{50.0, -50.0}, {50.0, 50.0}};
    imported.roads.push_back(bridge);

    const zeus::map::BuildResult build = zeus::map::MapBuilder().build(imported);
    require(build.map.nodes.size() == 4, "grade-separated crossing does not create a shared node");
    require(build.map.edges.size() == 4, "grade-separated roads are not split at crossing");
    const zeus::map::ValidationReport report = zeus::map::MapValidator().validate(build.map);
    require(report.component_count == 2, "grade-separated crossing remains disconnected");
}

void runCollapsedPieceDoesNotLeaveOrphanTest() {
    zeus::map::ImportedRoads imported;
    imported.metadata.runtime_crs_wkt = "LOCAL_METRIC_TEST_CRS";
    imported.metadata.snap_tolerance_m = 0.5;

    zeus::map::SourceRoad normal;
    normal.source_id = "normal";
    normal.direction = zeus::map::Direction::kBoth;
    normal.points = {{0.0, 0.0}, {10.0, 0.0}};
    imported.roads.push_back(normal);

    zeus::map::SourceRoad collapsed;
    collapsed.source_id = "collapsed";
    collapsed.direction = zeus::map::Direction::kBoth;
    collapsed.points = {{100.0, 0.0}, {100.2, 0.0}};
    imported.roads.push_back(collapsed);

    const zeus::map::BuildResult build = zeus::map::MapBuilder().build(imported);
    require(build.map.nodes.size() == 2, "snapping does not retain a provisional orphan node");
    const zeus::map::ValidationReport report = zeus::map::MapValidator().validate(build.map);
    for (const zeus::map::ValidationIssue& issue : report.issues) {
        require(issue.code != "ORPHAN_NODE", "collapsed road piece produces no orphan diagnostic");
    }
}

void runMixedGeometryGeoJsonTest() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("zeus-map-mixed-test-" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::create_directories(directory);
    try {
        const std::filesystem::path geojson_path = directory / "mixed-roads.geojson";
        {
            std::ofstream output(geojson_path);
            output << R"({
"type": "FeatureCollection",
"features": [
  {"type": "Feature", "properties": {"osm_id": "1", "oneway": "yes"},
   "geometry": {"type": "LineString", "coordinates": [[114.0000, 30.5000], [114.0010, 30.5000]]}},
  {"type": "Feature", "properties": {"osm_id": "2"},
   "geometry": {"type": "LineString", "coordinates": [[114.0000, 30.5000], [114.0000, 30.5010]]}},
  {"type": "Feature", "properties": {"osm_id": "3"},
   "geometry": {"type": "MultiLineString", "coordinates": [
     [[114.0010, 30.5000], [114.0020, 30.5000]],
     [[114.0000, 30.5010], [114.0000, 30.5020]]]}}
]})";
        }

        const std::string inspection = zeus::map::ShapefileImporter::inspect(geojson_path.string());
        require(
            inspection.find("geometry_counts=") != std::string::npos,
            "mixed layer reports per-feature geometry counts");
        require(
            inspection.find("Line String:2") != std::string::npos,
            "count LineString features");
        require(
            inspection.find("Multi Line String:1") != std::string::npos,
            "count MultiLineString features");

        zeus::map::ImportOptions options;
        options.id_field = "osm_id";
        options.oneway_field = "oneway";
        const zeus::map::ImportedRoads imported =
            zeus::map::ShapefileImporter().importFile(geojson_path.string(), options);
        require(
            imported.roads.size() == 4,
            "import mixed GeoJSON lines including MultiLineString parts");
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
    std::filesystem::remove_all(directory);
}

void runOsmPreprocessorTest() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("zeus-osm-preprocess-test-" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::create_directories(directory);
    try {
        const std::filesystem::path source = directory / "osm-roads.geojson";
        {
            std::ofstream output(source);
            output << R"({
"type":"FeatureCollection",
"features":[
 {"type":"Feature","properties":{"osm_id":"1","highway":"primary"},"geometry":{"type":"LineString","coordinates":[[114.0,30.5],[114.001,30.5]]}},
 {"type":"Feature","properties":{"osm_id":"2","highway":"footway"},"geometry":{"type":"LineString","coordinates":[[114.0,30.501],[114.001,30.501]]}},
 {"type":"Feature","properties":{"osm_id":"3","highway":"service"},"geometry":{"type":"LineString","coordinates":[[114.0,30.502],[114.001,30.502]]}},
 {"type":"Feature","properties":{"osm_id":"4","highway":"primary","access":"private"},"geometry":{"type":"LineString","coordinates":[[114.0,30.503],[114.001,30.503]]}},
 {"type":"Feature","properties":{"osm_id":"5","highway":"motorway"},"geometry":{"type":"LineString","coordinates":[[114.0,30.504],[114.001,30.504]]}},
 {"type":"Feature","properties":{"osm_id":"6","highway":"residential","maxspeed":"30 mph"},"geometry":{"type":"LineString","coordinates":[[114.0,30.505],[114.001,30.505]]}},
 {"type":"Feature","properties":{"osm_id":"7","highway":"tertiary","junction":"roundabout"},"geometry":{"type":"LineString","coordinates":[[114.0,30.506],[114.001,30.506]]}},
 {"type":"Feature","properties":{"osm_id":"8","highway":"secondary","oneway":"-1"},"geometry":{"type":"LineString","coordinates":[[114.0,30.507],[114.001,30.507]]}},
 {"type":"Feature","properties":{"osm_id":"9","highway":"unclassified"},"geometry":{"type":"GeometryCollection","geometries":[{"type":"LineString","coordinates":[[114.0,30.508],[114.001,30.508]]},{"type":"Point","coordinates":[114.0,30.508]}]}},
 {"type":"Feature","properties":{"osm_id":"10","highway":"primary"},"geometry":{"type":"LineString","coordinates":[[114.0,30.509],[114.000001,30.509]]}},
 {"type":"Feature","properties":{"osm_id":"11","highway":"primary"},"geometry":{"type":"LineString","coordinates":[[114.0,30.5],[114.001,30.5]]}},
 {"type":"Feature","properties":{"osm_id":"12","highway":"track"},"geometry":{"type":"LineString","coordinates":[[114.0,30.510],[114.001,30.510]]}},
 {"type":"Feature","properties":{"osm_id":"13","highway":"secondary","maxspeed":"50;60","lanes":"4","lanes:forward":"3","lanes:backward":"2"},"geometry":{"type":"LineString","coordinates":[[114.0,30.511],[114.001,30.511]]}}
]})";
        }

        const std::filesystem::path cleaned_path = directory / "cleaned.geojson";
        const std::filesystem::path report_path = directory / "report.json";
        zeus::map::OsmPreprocessOptions options;
        const zeus::map::OsmPreprocessReport report =
            zeus::map::OsmRoadPreprocessor().process(
                source.string(), cleaned_path.string(), options);
        zeus::map::OsmRoadPreprocessor::saveReport(
            report,
            options,
            source.string(),
            cleaned_path.string(),
            report_path.string());

        require(report.input_features == 13, "OSM preprocessor counts input features");
        require(report.output_features == 7, "OSM car profile keeps seven drivable roads");
        require(report.filtered_features == 6, "OSM car profile reports six exclusions");
        require(report.excluded_by_reason.at("non_drivable_class") == 1, "exclude footway");
        require(report.excluded_by_reason.at("service_disabled") == 1, "exclude service road");
        require(report.excluded_by_reason.at("track_disabled") == 1, "exclude track");
        require(report.excluded_by_reason.at("access_restricted") == 1, "exclude private road");
        require(report.excluded_by_reason.at("too_short") == 1, "exclude short geometry");
        require(report.excluded_by_reason.at("duplicate_geometry") == 1, "exclude duplicate");
        require(report.geometry_collections_converted == 1, "extract lines from collection");
        require(report.default_speed_applied == 5, "apply class default speeds");
        require(report.mph_speed_converted == 1, "convert mph speed");
        require(report.implied_oneway_applied == 2, "infer motorway and roundabout oneway");
        require(report.reverse_oneway_normalized == 1, "normalize reverse oneway");

        DatasetPtr cleaned(static_cast<GDALDataset*>(GDALOpenEx(
            cleaned_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
        require(cleaned != nullptr, "open cleaned OSM GeoJSON");
        OGRLayer* layer = cleaned->GetLayer(0);
        require(layer != nullptr, "cleaned OSM output has a layer");
        require(layer->GetFeatureCount(TRUE) == 7, "cleaned OSM output feature count");
        bool primary_ok = false;
        bool motorway_ok = false;
        bool mph_ok = false;
        bool roundabout_ok = false;
        bool reverse_ok = false;
        bool collection_ok = false;
        bool multi_speed_ok = false;
        layer->ResetReading();
        while (OGRFeature* raw_feature = layer->GetNextFeature()) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
                raw_feature, &OGRFeature::DestroyFeature);
            const std::string id = feature->GetFieldAsString("road_id");
            const std::string direction = feature->GetFieldAsString("oneway");
            const double speed = feature->GetFieldAsDouble("speed_kph");
            const int lanes = feature->GetFieldAsInteger("lanes");
            if (id == "1") primary_ok = direction == "both" &&
                                             std::abs(speed - 60.0) < 1e-6 && lanes == 2;
            if (id == "5") motorway_ok = direction == "forward" &&
                                               std::abs(speed - 100.0) < 1e-6 && lanes == 2;
            if (id == "6") mph_ok = std::abs(speed - 48.28032) < 1e-5;
            if (id == "7") roundabout_ok = direction == "forward";
            if (id == "8") reverse_ok = direction == "reverse";
            if (id == "9") {
                collection_ok = wkbFlatten(feature->GetGeometryRef()->getGeometryType()) ==
                                wkbMultiLineString;
            }
            if (id == "13") multi_speed_ok =
                std::abs(speed - 50.0) < 1e-6 && lanes == 2;
        }
        require(primary_ok, "normalize primary default speed and bidirectionality");
        require(motorway_ok, "normalize motorway speed and implied oneway");
        require(mph_ok, "write converted mph speed");
        require(roundabout_ok, "write implied roundabout direction");
        require(reverse_ok, "write reverse direction");
        require(collection_ok, "write lineal part of GeometryCollection");
        require(multi_speed_ok, "use the conservative limit from a multi-value maxspeed");

        std::ifstream report_file(report_path);
        const std::string report_json(
            (std::istreambuf_iterator<char>(report_file)), std::istreambuf_iterator<char>());
        require(
            report_json.find("\"output_features\": 7") != std::string::npos,
            "write machine-readable OSM report");

        zeus::map::OsmPreprocessOptions service_options;
        service_options.include_service = true;
        const zeus::map::OsmPreprocessReport service_report =
            zeus::map::OsmRoadPreprocessor().process(
                source.string(), (directory / "with-service.geojson").string(), service_options);
        require(service_report.output_features == 8, "optionally include service roads");
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
    std::filesystem::remove_all(directory);
}

zeus::map::EdgeIndex findEdge(
    const zeus::map::MapData& map,
    double from_x,
    double from_y,
    double to_x,
    double to_y,
    const std::string& source_id) {
    for (std::size_t i = 0; i < map.edges.size(); ++i) {
        const zeus::map::DirectedEdge& edge = map.edges[i];
        const zeus::map::Point2d from = map.nodes[edge.from].point;
        const zeus::map::Point2d to = map.nodes[edge.to].point;
        if (edge.source_id == source_id && std::abs(from.x - from_x) < 1e-6 &&
            std::abs(from.y - from_y) < 1e-6 && std::abs(to.x - to_x) < 1e-6 &&
            std::abs(to.y - to_y) < 1e-6) {
            return static_cast<zeus::map::EdgeIndex>(i);
        }
    }
    return zeus::map::kInvalidEdge;
}

const zeus::map::TurnTransition* findTransition(
    const zeus::map::MapData& map,
    zeus::map::EdgeIndex from,
    zeus::map::EdgeIndex to) {
    for (const zeus::map::TurnTransition& transition : map.turn_transitions) {
        if (transition.from_edge == from && transition.to_edge == to) {
            return &transition;
        }
    }
    return nullptr;
}

// Asymmetric crossroads: a north-south primary road, an east-west residential
// road, and a one-way residential diagonal leaving at 135 degrees.
zeus::map::ImportedRoads penaltyFixtureRoads() {
    zeus::map::ImportedRoads imported;
    imported.metadata.snap_tolerance_m = 0.5;
    const auto road = [](std::string id, std::string road_class,
                         std::vector<zeus::map::Point2d> points,
                         zeus::map::Direction direction) {
        zeus::map::SourceRoad source;
        source.source_id = std::move(id);
        source.road_class = std::move(road_class);
        source.direction = direction;
        source.speed_limit_mps = 20.0;
        source.points = std::move(points);
        return source;
    };
    imported.roads.push_back(road(
        "maj", "primary", {{0.0, -100.0}, {0.0, 100.0}}, zeus::map::Direction::kBoth));
    imported.roads.push_back(road(
        "min", "residential", {{-100.0, 0.0}, {100.0, 0.0}}, zeus::map::Direction::kBoth));
    imported.roads.push_back(road(
        "diag", "residential", {{0.0, 0.0}, {-70.0, 70.0}}, zeus::map::Direction::kForward));
    return imported;
}

void runDefaultTurnPenaltyTest() {
    const zeus::map::BuildResult build = zeus::map::MapBuilder().build(penaltyFixtureRoads());

    const zeus::map::EdgeIndex east_in = findEdge(build.map, -100, 0, 0, 0, "min");
    const zeus::map::EdgeIndex north_in = findEdge(build.map, 0, -100, 0, 0, "maj");
    const zeus::map::EdgeIndex east_out = findEdge(build.map, 0, 0, 100, 0, "min");
    const zeus::map::EdgeIndex west_out = findEdge(build.map, 0, 0, -100, 0, "min");
    const zeus::map::EdgeIndex north_out = findEdge(build.map, 0, 0, 0, 100, "maj");
    const zeus::map::EdgeIndex diag_out = findEdge(build.map, 0, 0, -70, 70, "diag");
    require(east_in != zeus::map::kInvalidEdge && north_in != zeus::map::kInvalidEdge &&
                east_out != zeus::map::kInvalidEdge && west_out != zeus::map::kInvalidEdge &&
                north_out != zeus::map::kInvalidEdge && diag_out != zeus::map::kInvalidEdge,
            "penalty fixture edges resolve by endpoint and source");

    const zeus::map::TurnTransition* uturn = findTransition(build.map, east_in, west_out);
    require(uturn != nullptr && !uturn->prohibited &&
                std::abs(uturn->penalty_s - 5.0F) < 1e-6,
            "u-turn onto the opposite twin costs the turnaround penalty");

    const zeus::map::TurnTransition* to_major = findTransition(build.map, east_in, north_out);
    require(to_major != nullptr && std::abs(to_major->penalty_s - 3.0F) < 1e-6,
            "minor-to-major approach costs the merge penalty");

    const zeus::map::TurnTransition* sharp_left = findTransition(build.map, east_in, diag_out);
    require(sharp_left != nullptr && std::abs(sharp_left->penalty_s - 2.0F) < 1e-6,
            "sharp left turn costs the crossing penalty");

    require(findTransition(build.map, north_in, west_out) == nullptr,
            "plain 90-degree left stays below the sharp-turn threshold");
    require(findTransition(build.map, north_in, east_out) == nullptr,
            "right turn stays free");
    require(findTransition(build.map, east_in, east_out) == nullptr,
            "straight movement stays free");
}

void runTurnPenaltyMergeWithSidecarTest() {
    zeus::map::ImportedRoads lower = penaltyFixtureRoads();
    lower.turn_transitions.push_back(
        {"min", {0.0, 0.0}, "maj", zeus::map::SourceTurnKind::kPenalty, 2.5F});
    const zeus::map::BuildResult lower_build = zeus::map::MapBuilder().build(lower);
    const zeus::map::EdgeIndex east_in = findEdge(lower_build.map, -100, 0, 0, 0, "min");
    const zeus::map::EdgeIndex north_out = findEdge(lower_build.map, 0, 0, 0, 100, "maj");
    const zeus::map::TurnTransition* merged = findTransition(
        lower_build.map, east_in, north_out);
    require(merged != nullptr && std::abs(merged->penalty_s - 3.0F) < 1e-6,
            "sidecar penalty below the generated value merges to the maximum");

    zeus::map::ImportedRoads higher = penaltyFixtureRoads();
    higher.turn_transitions.push_back(
        {"min", {0.0, 0.0}, "maj", zeus::map::SourceTurnKind::kPenalty, 7.0F});
    const zeus::map::BuildResult higher_build = zeus::map::MapBuilder().build(higher);
    const zeus::map::EdgeIndex east_in_high = findEdge(higher_build.map, -100, 0, 0, 0, "min");
    const zeus::map::EdgeIndex north_out_high =
        findEdge(higher_build.map, 0, 0, 0, 100, "maj");
    const zeus::map::TurnTransition* overridden = findTransition(
        higher_build.map, east_in_high, north_out_high);
    require(overridden != nullptr && std::abs(overridden->penalty_s - 7.0F) < 1e-6,
            "sidecar penalty above the generated value wins the merge");
}

}  // namespace

int main() {
    try {
        runEndToEndTest();
        runDefaultTurnPenaltyTest();
        runTurnPenaltyMergeWithSidecarTest();
        runGradeSeparatedCrossingTest();
        runCollapsedPieceDoesNotLeaveOrphanTest();
        runMixedGeometryGeoJsonTest();
        runOsmPreprocessorTest();
        runEdgeLevelMapMatchingTest();
        std::cout << "all map engine tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
