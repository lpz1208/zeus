#include "zeus/routing/route_exporter.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gdal_priv.h>
#include <ogrsf_frmts.h>

namespace zeus::routing {
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

void createField(OGRLayer& layer, const char* name, OGRFieldType type) {
    OGRFieldDefn field(name, type);
    if (layer.CreateField(&field) != OGRERR_NONE) {
        throw std::runtime_error(std::string("cannot create route GeoJSON field: ") + name);
    }
}

// Returns the edge polyline in traversal order and its cumulative station
// values, so any [start_s, end_s] slice can be interpolated.
std::vector<zeus::map::Point2d> edgePolyline(
    const zeus::map::MapData& map,
    const zeus::map::DirectedEdge& edge) {
    std::vector<zeus::map::Point2d> points;
    if (edge.geometry_count < 2) {
        points = {map.nodes[edge.from].point, map.nodes[edge.to].point};
        return points;
    }
    points.reserve(edge.geometry_count);
    for (std::uint32_t i = 0; i < edge.geometry_count; ++i) {
        const std::uint32_t index =
            edge.geometry_reversed ? edge.geometry_count - 1 - i : i;
        points.push_back(map.geometry_points[edge.geometry_offset + index]);
    }
    return points;
}

std::vector<zeus::map::Point2d> slicePolyline(
    const std::vector<zeus::map::Point2d>& points,
    double start_s,
    double end_s) {
    std::vector<zeus::map::Point2d> slice;
    if (points.size() < 2 || end_s <= start_s) {
        return slice;
    }
    const auto interpolate = [](const zeus::map::Point2d& a, const zeus::map::Point2d& b,
                                double ratio) {
        return zeus::map::Point2d{a.x + (b.x - a.x) * ratio, a.y + (b.y - a.y) * ratio};
    };

    double station = 0.0;
    bool started = false;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const double dx = points[i].x - points[i - 1].x;
        const double dy = points[i].y - points[i - 1].y;
        const double segment = std::sqrt(dx * dx + dy * dy);
        if (segment <= 0.0) {
            continue;
        }
        const double next_station = station + segment;
        if (!started && next_station > start_s) {
            slice.push_back(
                interpolate(points[i - 1], points[i], (start_s - station) / segment));
            started = true;
        }
        if (started) {
            if (next_station >= end_s) {
                slice.push_back(
                    interpolate(points[i - 1], points[i], (end_s - station) / segment));
                break;
            }
            slice.push_back(points[i]);
        }
        station = next_station;
    }
    return slice;
}

}  // namespace

std::size_t RouteGeoJsonExporter::save(
    const zeus::map::MapData& map,
    const RouteResult& result,
    const std::string& path) {
    if (map.metadata.runtime_crs_wkt.empty()) {
        throw std::runtime_error("cannot export a route without a runtime CRS");
    }
    if (!result.ok || result.path.edges.empty()) {
        throw std::runtime_error("cannot export a failed or empty route");
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
    std::unique_ptr<OGRCoordinateTransformation, TransformDeleter> transform(
        OGRCreateCoordinateTransformation(&source, &wgs84));
    if (!transform) {
        throw std::runtime_error("cannot create route-to-WGS84 coordinate transformation");
    }

    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
    if (driver == nullptr) {
        throw std::runtime_error("GDAL GeoJSON driver is unavailable");
    }
    if (std::filesystem::exists(path)) {
        std::filesystem::remove(path);
    }
    std::unique_ptr<GDALDataset, DatasetCloser> dataset(
        driver->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    if (!dataset) {
        throw std::runtime_error("cannot create route GeoJSON output: " + path);
    }
    OGRLayer* layer = dataset->CreateLayer("route", &wgs84, wkbLineString, nullptr);
    if (layer == nullptr) {
        throw std::runtime_error("cannot create route GeoJSON layer");
    }
    createField(*layer, "ROAD_ID", OFTString);
    createField(*layer, "SOURCE_ID", OFTString);
    createField(*layer, "CLASS", OFTString);
    createField(*layer, "LENGTH_M", OFTReal);
    createField(*layer, "EDGE_INDEX", OFTInteger);

    std::size_t written = 0;
    for (std::size_t i = 0; i < result.path.edges.size(); ++i) {
        const zeus::map::DirectedEdge& edge = map.edges[result.path.edges[i]];
        const double start_s = i == 0 ? result.path.start_offset_m : 0.0;
        const double end_s = i == result.path.edges.size() - 1
                                 ? result.path.end_offset_m
                                 : edge.length_m;
        const std::vector<zeus::map::Point2d> slice =
            slicePolyline(edgePolyline(map, edge), start_s, end_s);
        if (slice.size() < 2) {
            continue;
        }

        OGRLineString line;
        for (const zeus::map::Point2d& point : slice) {
            line.addPoint(point.x, point.y);
        }
        if (line.transform(transform.get()) != OGRERR_NONE) {
            throw std::runtime_error("cannot transform route geometry to WGS84");
        }

        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            OGRFeature::CreateFeature(layer->GetLayerDefn()), &OGRFeature::DestroyFeature);
        feature->SetField("ROAD_ID", std::to_string(edge.road_id).c_str());
        feature->SetField("SOURCE_ID", edge.source_id.c_str());
        feature->SetField("CLASS", edge.road_class.c_str());
        feature->SetField("LENGTH_M", end_s - start_s);
        feature->SetField("EDGE_INDEX", static_cast<int>(result.path.edges[i]));
        feature->SetGeometry(&line);
        if (layer->CreateFeature(feature.get()) != OGRERR_NONE) {
            throw std::runtime_error("cannot write a route feature to GeoJSON");
        }
        ++written;
    }
    return written;
}

}  // namespace zeus::routing
