#include "zeus/map/geojson_exporter.h"

#include <filesystem>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include "zeus/map/map_validator.h"

namespace zeus::map {
namespace {

struct DatasetCloser {
    void operator()(GDALDataset* dataset) const {
        if (dataset != nullptr) {
            GDALClose(dataset);
        }
    }
};

struct TransformDeleter {
    void operator()(OGRCoordinateTransformation* transform) const {
        OGRCoordinateTransformation::DestroyCT(transform);
    }
};

using DatasetPtr = std::unique_ptr<GDALDataset, DatasetCloser>;
using TransformPtr = std::unique_ptr<OGRCoordinateTransformation, TransformDeleter>;

struct ExportContext {
    OGRSpatialReference wgs84;
    TransformPtr transform;
    DatasetPtr dataset;
};

void createField(OGRLayer& layer, const char* name, OGRFieldType type) {
    OGRFieldDefn field(name, type);
    if (layer.CreateField(&field) != OGRERR_NONE) {
        throw std::runtime_error(std::string("cannot create GeoJSON field: ") + name);
    }
}

ExportContext createExportContext(const MapData& map, const std::string& path) {
    if (map.metadata.runtime_crs_wkt.empty()) {
        throw std::runtime_error("cannot export a map without a runtime CRS");
    }

    GDALAllRegister();
    OGRSpatialReference source;
    if (source.importFromWkt(map.metadata.runtime_crs_wkt.c_str()) != OGRERR_NONE) {
        throw std::runtime_error("runtime map contains an invalid CRS");
    }
    source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    TransformPtr transform(OGRCreateCoordinateTransformation(&source, &wgs84));
    if (!transform) {
        throw std::runtime_error("cannot create runtime-to-WGS84 coordinate transformation");
    }

    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
    if (driver == nullptr) {
        throw std::runtime_error("GDAL GeoJSON driver is unavailable");
    }
    if (std::filesystem::exists(path)) {
        std::filesystem::remove(path);
    }
    DatasetPtr dataset(driver->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    if (!dataset) {
        throw std::runtime_error("cannot create GeoJSON output: " + path);
    }
    return {std::move(wgs84), std::move(transform), std::move(dataset)};
}

bool hasMappableLocation(const ValidationIssue& issue) {
    return issue.has_location;
}

}  // namespace

void GeoJsonExporter::save(const MapData& map, const std::string& path) {
    ExportContext context = createExportContext(map, path);
    OGRLayer* layer =
        context.dataset->CreateLayer("roads", &context.wgs84, wkbLineString, nullptr);
    if (layer == nullptr) {
        throw std::runtime_error("cannot create GeoJSON road layer");
    }
    createField(*layer, "ROAD_ID", OFTString);
    createField(*layer, "SOURCE_ID", OFTString);
    createField(*layer, "CLASS", OFTString);
    createField(*layer, "EDGE_IDS", OFTString);
    createField(*layer, "DIRECTION", OFTString);
    createField(*layer, "LENGTH_M", OFTReal);
    createField(*layer, "SPEED_KPH", OFTReal);
    createField(*layer, "LANES", OFTInteger);
    createField(*layer, "Z_LEVEL", OFTInteger);

    std::unordered_map<PersistentId, std::vector<EdgeIndex>> grouped;
    for (std::size_t i = 0; i < map.edges.size(); ++i) {
        grouped[map.edges[i].road_id].push_back(static_cast<EdgeIndex>(i));
    }

    for (const auto& [road_id, edge_indices] : grouped) {
        const DirectedEdge& edge = map.edges[edge_indices.front()];
        const std::uint64_t geometry_end =
            static_cast<std::uint64_t>(edge.geometry_offset) + edge.geometry_count;
        if (edge.geometry_count < 2 || geometry_end > map.geometry_points.size()) {
            continue;
        }

        OGRLineString line;
        for (std::uint32_t i = 0; i < edge.geometry_count; ++i) {
            const Point2d point = map.geometry_points[edge.geometry_offset + i];
            line.addPoint(point.x, point.y);
        }
        if (line.transform(context.transform.get()) != OGRERR_NONE) {
            throw std::runtime_error("cannot transform road geometry to WGS84");
        }

        std::string edge_ids;
        bool has_forward = false;
        bool has_reverse = false;
        for (std::size_t i = 0; i < edge_indices.size(); ++i) {
            if (i > 0) {
                edge_ids.push_back(',');
            }
            edge_ids += std::to_string(edge_indices[i]);
            has_reverse = has_reverse || map.edges[edge_indices[i]].geometry_reversed;
            has_forward = has_forward || !map.edges[edge_indices[i]].geometry_reversed;
        }
        const char* direction =
            has_forward && has_reverse ? "both" : (has_reverse ? "reverse" : "forward");

        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            OGRFeature::CreateFeature(layer->GetLayerDefn()), &OGRFeature::DestroyFeature);
        feature->SetField("ROAD_ID", std::to_string(road_id).c_str());
        feature->SetField("SOURCE_ID", edge.source_id.c_str());
        feature->SetField("CLASS", edge.road_class.c_str());
        feature->SetField("EDGE_IDS", edge_ids.c_str());
        feature->SetField("DIRECTION", direction);
        feature->SetField("LENGTH_M", edge.length_m);
        feature->SetField("SPEED_KPH", edge.speed_limit_mps * 3.6F);
        feature->SetField("LANES", static_cast<int>(edge.lane_count));
        feature->SetField("Z_LEVEL", edge.z_level);
        feature->SetGeometry(&line);
        if (layer->CreateFeature(feature.get()) != OGRERR_NONE) {
            throw std::runtime_error("cannot write a road feature to GeoJSON");
        }
    }
}

void GeoJsonExporter::saveNodes(const MapData& map, const std::string& path) {
    ExportContext context = createExportContext(map, path);
    OGRLayer* layer =
        context.dataset->CreateLayer("nodes", &context.wgs84, wkbPoint, nullptr);
    if (layer == nullptr) {
        throw std::runtime_error("cannot create GeoJSON node layer");
    }
    createField(*layer, "NODE_ID", OFTString);
    createField(*layer, "NODE_INDEX", OFTInteger);
    createField(*layer, "IN_DEGREE", OFTInteger);
    createField(*layer, "OUT_DEGREE", OFTInteger);

    std::vector<int> incoming(map.nodes.size(), 0);
    std::vector<int> outgoing(map.nodes.size(), 0);
    for (const DirectedEdge& edge : map.edges) {
        if (edge.from < map.nodes.size()) {
            ++outgoing[edge.from];
        }
        if (edge.to < map.nodes.size()) {
            ++incoming[edge.to];
        }
    }

    for (std::size_t index = 0; index < map.nodes.size(); ++index) {
        const Node& node = map.nodes[index];
        OGRPoint point(node.point.x, node.point.y);
        if (point.transform(context.transform.get()) != OGRERR_NONE) {
            throw std::runtime_error("cannot transform topology node to WGS84");
        }
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            OGRFeature::CreateFeature(layer->GetLayerDefn()), &OGRFeature::DestroyFeature);
        feature->SetField("NODE_ID", std::to_string(node.id).c_str());
        feature->SetField("NODE_INDEX", static_cast<int>(index));
        feature->SetField("IN_DEGREE", incoming[index]);
        feature->SetField("OUT_DEGREE", outgoing[index]);
        feature->SetGeometry(&point);
        if (layer->CreateFeature(feature.get()) != OGRERR_NONE) {
            throw std::runtime_error("cannot write a topology node to GeoJSON");
        }
    }
}

std::size_t GeoJsonExporter::saveReference(
    const std::string& source_path,
    const std::string& output_path) {
    GDALAllRegister();
    DatasetPtr source(static_cast<GDALDataset*>(GDALOpenEx(
        source_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
    if (!source || source->GetLayerCount() == 0) {
        throw std::runtime_error("cannot open reference vector source: " + source_path);
    }
    OGRLayer* source_layer = source->GetLayer(0);
    if (source_layer == nullptr) {
        throw std::runtime_error("reference vector source has no readable layer");
    }
    const OGRSpatialReference* layer_spatial_reference = source_layer->GetSpatialRef();
    if (layer_spatial_reference == nullptr) {
        throw std::runtime_error(
            "reference layer has no CRS; provide GeoJSON WGS84 metadata or a Shapefile .prj");
    }

    OGRSpatialReference source_spatial_reference(*layer_spatial_reference);
    source_spatial_reference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    TransformPtr transform(
        OGRCreateCoordinateTransformation(&source_spatial_reference, &wgs84));
    if (!transform) {
        throw std::runtime_error("cannot transform reference layer to WGS84");
    }

    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
    if (driver == nullptr) {
        throw std::runtime_error("GDAL GeoJSON driver is unavailable");
    }
    if (std::filesystem::exists(output_path)) {
        std::filesystem::remove(output_path);
    }
    DatasetPtr output(driver->Create(output_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    if (!output) {
        throw std::runtime_error("cannot create reference GeoJSON output: " + output_path);
    }
    OGRLayer* output_layer = output->CreateLayer(
        "reference", &wgs84, source_layer->GetGeomType(), nullptr);
    if (output_layer == nullptr) {
        throw std::runtime_error("cannot create reference GeoJSON layer");
    }

    OGRFeatureDefn* source_definition = source_layer->GetLayerDefn();
    for (int index = 0; index < source_definition->GetFieldCount(); ++index) {
        OGRFieldDefn field(source_definition->GetFieldDefn(index));
        if (output_layer->CreateField(&field) != OGRERR_NONE) {
            throw std::runtime_error(
                "cannot preserve reference field: " + std::string(field.GetNameRef()));
        }
    }

    std::size_t written = 0;
    source_layer->ResetReading();
    while (OGRFeature* raw_feature = source_layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> source_feature(
            raw_feature, &OGRFeature::DestroyFeature);
        OGRGeometry* source_geometry = source_feature->GetGeometryRef();
        if (source_geometry == nullptr || source_geometry->IsEmpty()) {
            continue;
        }
        std::unique_ptr<OGRGeometry> geometry(source_geometry->clone());
        if (!geometry || geometry->transform(transform.get()) != OGRERR_NONE) {
            throw std::runtime_error("cannot transform reference feature to WGS84");
        }

        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> output_feature(
            OGRFeature::CreateFeature(output_layer->GetLayerDefn()),
            &OGRFeature::DestroyFeature);
        if (output_feature->SetFrom(source_feature.get(), TRUE) != OGRERR_NONE) {
            throw std::runtime_error("cannot copy reference feature attributes");
        }
        output_feature->SetGeometryDirectly(geometry.release());
        if (output_layer->CreateFeature(output_feature.get()) != OGRERR_NONE) {
            throw std::runtime_error("cannot write reference GeoJSON feature");
        }
        ++written;
    }
    if (written == 0) {
        throw std::runtime_error("reference layer contains no non-empty geometry");
    }
    return written;
}

void GeoJsonExporter::saveIssues(
    const MapData& map,
    const std::vector<ValidationIssue>& issues,
    const std::string& path) {
    ExportContext context = createExportContext(map, path);
    OGRLayer* layer =
        context.dataset->CreateLayer("issues", &context.wgs84, wkbPoint, nullptr);
    if (layer == nullptr) {
        throw std::runtime_error("cannot create GeoJSON issue layer");
    }
    createField(*layer, "ISSUE_INDEX", OFTInteger);
    createField(*layer, "SEVERITY", OFTString);
    createField(*layer, "CODE", OFTString);
    createField(*layer, "SOURCE", OFTString);
    createField(*layer, "MESSAGE", OFTString);
    createField(*layer, "RUNTIME_X", OFTReal);
    createField(*layer, "RUNTIME_Y", OFTReal);

    for (std::size_t index = 0; index < issues.size(); ++index) {
        const ValidationIssue& issue = issues[index];
        if (!hasMappableLocation(issue)) {
            continue;
        }
        OGRPoint point(issue.location.x, issue.location.y);
        if (point.transform(context.transform.get()) != OGRERR_NONE) {
            throw std::runtime_error("cannot transform issue location to WGS84");
        }
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            OGRFeature::CreateFeature(layer->GetLayerDefn()), &OGRFeature::DestroyFeature);
        feature->SetField("ISSUE_INDEX", static_cast<int>(index));
        feature->SetField("SEVERITY", severityName(issue.severity));
        feature->SetField("CODE", issue.code.c_str());
        feature->SetField("SOURCE", issue.source_id.c_str());
        feature->SetField("MESSAGE", issue.message.c_str());
        feature->SetField("RUNTIME_X", issue.location.x);
        feature->SetField("RUNTIME_Y", issue.location.y);
        feature->SetGeometry(&point);
        if (layer->CreateFeature(feature.get()) != OGRERR_NONE) {
            throw std::runtime_error("cannot write an issue feature to GeoJSON");
        }
    }
}

}  // namespace zeus::map
