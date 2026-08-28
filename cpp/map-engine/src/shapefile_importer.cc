#include "zeus/map/shapefile_importer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <cpl_conv.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

namespace zeus::map {
namespace {

struct DatasetCloser {
    void operator()(GDALDataset* dataset) const {
        if (dataset != nullptr) {
            GDALClose(dataset);
        }
    }
};

struct GeometryDeleter {
    void operator()(OGRGeometry* geometry) const {
        OGRGeometryFactory::destroyGeometry(geometry);
    }
};

struct TransformDeleter {
    void operator()(OGRCoordinateTransformation* transform) const {
        OGRCoordinateTransformation::DestroyCT(transform);
    }
};

using DatasetPtr = std::unique_ptr<GDALDataset, DatasetCloser>;
using GeometryPtr = std::unique_ptr<OGRGeometry, GeometryDeleter>;
using TransformPtr = std::unique_ptr<OGRCoordinateTransformation, TransformDeleter>;

std::string trim(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool truthy(const std::string& raw) {
    const std::string value = lowercase(trim(raw));
    return value == "1" || value == "true" || value == "yes" || value == "y" ||
           value == "t" || value == "是";
}

void requireShapefileBundle(const std::string& path) {
    const std::filesystem::path source(path);
    if (lowercase(source.extension().string()) != ".shp") {
        return;
    }
    for (const char* extension : {".shx", ".dbf", ".prj"}) {
        bool found = false;
        for (const auto& entry : std::filesystem::directory_iterator(source.parent_path())) {
            const std::filesystem::path candidate = entry.path();
            if (lowercase(candidate.stem().string()) == lowercase(source.stem().string()) &&
                lowercase(candidate.extension().string()) == extension) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error(
                "incomplete Shapefile bundle; missing required sidecar " +
                std::string(extension) + " for " + source.filename().string());
        }
    }
}

Direction parseDirection(const std::string& raw, bool default_bidirectional) {
    const std::string value = lowercase(trim(raw));
    if (value.empty()) {
        return default_bidirectional ? Direction::kBoth : Direction::kForward;
    }
    if (value == "-1" || value == "reverse" || value == "backward" || value == "tf" ||
        value == "r") {
        return Direction::kReverse;
    }
    if (value == "0" || value == "no" || value == "false" || value == "both" ||
        value == "b" || value == "n" || value == "双向") {
        return Direction::kBoth;
    }
    if (value == "1" || value == "yes" || value == "true" || value == "forward" ||
        value == "ft" || value == "f" || value == "y" || value == "单向") {
        return Direction::kForward;
    }
    return default_bidirectional ? Direction::kBoth : Direction::kForward;
}

int fieldIndex(const OGRFeatureDefn& definition, const std::string& name) {
    if (name.empty()) {
        return -1;
    }
    return definition.GetFieldIndex(name.c_str());
}

std::string fieldString(const OGRFeature& feature, int index) {
    if (index < 0 || !feature.IsFieldSetAndNotNull(index)) {
        return {};
    }
    return feature.GetFieldAsString(index);
}

double fieldDouble(const OGRFeature& feature, int index, double fallback) {
    if (index < 0 || !feature.IsFieldSetAndNotNull(index)) {
        return fallback;
    }
    return feature.GetFieldAsDouble(index);
}

int fieldInteger(const OGRFeature& feature, int index, int fallback) {
    if (index < 0 || !feature.IsFieldSetAndNotNull(index)) {
        return fallback;
    }
    return feature.GetFieldAsInteger(index);
}

std::string exportWkt(OGRSpatialReference& spatial_reference) {
    char* raw = nullptr;
    if (spatial_reference.exportToWkt(&raw) != OGRERR_NONE || raw == nullptr) {
        throw std::runtime_error("failed to export coordinate reference system as WKT");
    }
    std::string result(raw);
    CPLFree(raw);
    return result;
}

OGRSpatialReference chooseRuntimeCrs(
    OGRSpatialReference& source,
    OGRLayer& layer,
    const std::string& requested) {
    OGRSpatialReference target;
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    if (!requested.empty()) {
        if (target.SetFromUserInput(requested.c_str()) != OGRERR_NONE) {
            throw std::runtime_error("invalid target CRS: " + requested);
        }
        target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        return target;
    }

    const double linear_units = source.GetLinearUnits();
    if (source.IsProjected() && std::abs(linear_units - 1.0) < 1e-6) {
        return source;
    }

    OGREnvelope extent;
    if (layer.GetExtent(&extent, TRUE) != OGRERR_NONE) {
        throw std::runtime_error("cannot determine layer extent for automatic projected CRS");
    }

    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    TransformPtr to_wgs84(OGRCreateCoordinateTransformation(&source, &wgs84));
    if (!to_wgs84) {
        throw std::runtime_error("cannot transform source CRS to WGS84 for automatic UTM selection");
    }

    double lon = (extent.MinX + extent.MaxX) * 0.5;
    double lat = (extent.MinY + extent.MaxY) * 0.5;
    if (!to_wgs84->Transform(1, &lon, &lat)) {
        throw std::runtime_error("cannot transform layer center to WGS84");
    }

    const int zone = std::clamp(static_cast<int>(std::floor((lon + 180.0) / 6.0)) + 1, 1, 60);
    const int epsg = (lat >= 0.0 ? 32600 : 32700) + zone;
    const std::string definition = "EPSG:" + std::to_string(epsg);
    if (target.SetFromUserInput(definition.c_str()) != OGRERR_NONE) {
        throw std::runtime_error("cannot create automatic UTM CRS " + definition);
    }
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    return target;
}

std::vector<Point2d> linePoints(const OGRLineString& line) {
    std::vector<Point2d> points;
    points.reserve(static_cast<std::size_t>(line.getNumPoints()));
    for (int i = 0; i < line.getNumPoints(); ++i) {
        const Point2d point{line.getX(i), line.getY(i)};
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            continue;
        }
        if (points.empty() || distance(points.back(), point) > 1e-8) {
            points.push_back(point);
        }
    }
    return points;
}

void appendLine(
    const OGRLineString& line,
    const OGRFeature& feature,
    std::string source_id,
    int part,
    int oneway_index,
    int speed_index,
    int lanes_index,
    int class_index,
    int level_index,
    int bridge_index,
    int tunnel_index,
    const ImportOptions& options,
    ImportedRoads& result) {
    std::vector<Point2d> points = linePoints(line);
    if (points.size() < 2 || polylineLength(points) <= 1e-6) {
        result.issues.push_back({
            "INVALID_LINE_GEOMETRY",
            IssueSeverity::kError,
            "Line has fewer than two distinct points or zero length",
            source_id,
            points.empty() ? Point2d{} : points.front(),
            !points.empty(),
        });
        return;
    }

    if (part > 0) {
        source_id += "#part" + std::to_string(part);
    }

    SourceRoad road;
    road.source_id = std::move(source_id);
    road.road_class = fieldString(feature, class_index);
    road.direction = parseDirection(fieldString(feature, oneway_index), options.default_bidirectional);
    road.speed_limit_mps =
        std::max(0.1, fieldDouble(feature, speed_index, options.default_speed_kph)) / 3.6;
    road.lane_count = static_cast<std::uint16_t>(
        std::clamp(fieldInteger(feature, lanes_index, 1), 1, 32));
    road.z_level = static_cast<std::int16_t>(fieldInteger(feature, level_index, 0));
    if (level_index < 0) {
        if (truthy(fieldString(feature, bridge_index))) {
            road.z_level = 1;
        } else if (truthy(fieldString(feature, tunnel_index))) {
            road.z_level = -1;
        }
    }
    road.points = std::move(points);
    result.roads.push_back(std::move(road));
}

std::vector<std::string> splitCommaLine(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t comma = line.find(',', begin);
        fields.push_back(trim(line.substr(
            begin, comma == std::string::npos ? comma : comma - begin)));
        if (comma == std::string::npos) {
            return fields;
        }
        begin = comma + 1;
    }
}

std::vector<SourceTurnTransition> loadTurnTransitions(
    const std::string& path,
    OGRCoordinateTransformation& transform) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open turn restriction file: " + path);
    }
    std::vector<SourceTurnTransition> result;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::vector<std::string> fields = splitCommaLine(line);
        if (fields.size() < 5 || fields.size() > 6) {
            throw std::runtime_error(
                "invalid turn restriction line " + std::to_string(line_number) +
                ": expected from_source_id,via_x,via_y,to_source_id,type[,penalty_s]");
        }
        SourceTurnTransition transition;
        transition.from_source_id = fields[0];
        transition.via_point = {std::stod(fields[1]), std::stod(fields[2])};
        transition.to_source_id = fields[3];
        const std::string kind = lowercase(fields[4]);
        if (kind == "no") {
            transition.kind = SourceTurnKind::kNo;
        } else if (kind == "only") {
            transition.kind = SourceTurnKind::kOnly;
        } else if (kind == "penalty") {
            transition.kind = SourceTurnKind::kPenalty;
            if (fields.size() != 6) {
                throw std::runtime_error(
                    "turn penalty line " + std::to_string(line_number) +
                    " is missing penalty_s");
            }
            transition.penalty_s = std::stof(fields[5]);
        } else {
            throw std::runtime_error(
                "unknown turn restriction type on line " +
                std::to_string(line_number));
        }
        if (transition.from_source_id.empty() || transition.to_source_id.empty() ||
            !std::isfinite(transition.via_point.x) ||
            !std::isfinite(transition.via_point.y) ||
            !std::isfinite(transition.penalty_s) || transition.penalty_s < 0.0F) {
            throw std::runtime_error(
                "invalid turn restriction value on line " +
                std::to_string(line_number));
        }
        double x = transition.via_point.x;
        double y = transition.via_point.y;
        if (!transform.Transform(1, &x, &y)) {
            throw std::runtime_error(
                "cannot transform turn restriction via point on line " +
                std::to_string(line_number));
        }
        transition.via_point = {x, y};
        result.push_back(std::move(transition));
    }
    return result;
}

}  // namespace

ImportedRoads ShapefileImporter::importFile(
    const std::string& path,
    const ImportOptions& options) const {
    requireShapefileBundle(path);
    GDALAllRegister();
    DatasetPtr dataset(static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
    if (!dataset) {
        throw std::runtime_error("cannot open vector dataset: " + path);
    }
    if (dataset->GetLayerCount() == 0) {
        throw std::runtime_error("vector dataset has no layers: " + path);
    }

    OGRLayer* layer = dataset->GetLayer(0);
    if (layer == nullptr) {
        throw std::runtime_error("cannot open the first vector layer: " + path);
    }
    const OGRSpatialReference* source_ptr = layer->GetSpatialRef();
    if (source_ptr == nullptr) {
        throw std::runtime_error(
            "source layer has no CRS; declare one in GeoJSON or provide a .prj sidecar");
    }

    OGRSpatialReference source(*source_ptr);
    source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReference target = chooseRuntimeCrs(source, *layer, options.target_crs);
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    TransformPtr transform(OGRCreateCoordinateTransformation(&source, &target));
    if (!transform) {
        throw std::runtime_error("cannot create source-to-runtime coordinate transformation");
    }

    ImportedRoads result;
    result.metadata.source_path = std::filesystem::absolute(path).string();
    result.metadata.source_crs_wkt = exportWkt(source);
    result.metadata.runtime_crs_wkt = exportWkt(target);
    result.metadata.snap_tolerance_m = options.snap_tolerance_m;
    if (lowercase(std::filesystem::path(path).extension().string()) == ".shp") {
        bool has_cpg = false;
        const std::filesystem::path source(path);
        for (const auto& entry : std::filesystem::directory_iterator(source.parent_path())) {
            if (lowercase(entry.path().stem().string()) == lowercase(source.stem().string()) &&
                lowercase(entry.path().extension().string()) == ".cpg") {
                has_cpg = true;
                break;
            }
        }
        if (!has_cpg) {
            result.issues.push_back({
                "MISSING_CPG",
                IssueSeverity::kWarning,
                "Shapefile has no .cpg encoding declaration; verify text attributes",
                {},
                {},
            });
        }
    }

    OGRFeatureDefn* definition = layer->GetLayerDefn();
    if (definition == nullptr) {
        throw std::runtime_error("layer has no feature definition");
    }
    const int id_index = fieldIndex(*definition, options.id_field);
    const int oneway_index = fieldIndex(*definition, options.oneway_field);
    const int speed_index = fieldIndex(*definition, options.speed_field);
    const int lanes_index = fieldIndex(*definition, options.lanes_field);
    const int class_index = fieldIndex(*definition, options.road_class_field);
    const int level_index = fieldIndex(*definition, options.z_level_field);
    const int bridge_index = fieldIndex(*definition, options.bridge_field);
    const int tunnel_index = fieldIndex(*definition, options.tunnel_field);

    layer->ResetReading();
    while (OGRFeature* raw_feature = layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            raw_feature, &OGRFeature::DestroyFeature);
        const std::string fallback_id = std::to_string(feature->GetFID());
        std::string source_id = fieldString(*feature, id_index);
        if (source_id.empty()) {
            source_id = fallback_id;
        }

        OGRGeometry* source_geometry = feature->GetGeometryRef();
        if (source_geometry == nullptr || source_geometry->IsEmpty()) {
            result.issues.push_back({
                "EMPTY_GEOMETRY", IssueSeverity::kError, "Feature has no geometry", source_id, {}});
            continue;
        }

        GeometryPtr geometry(source_geometry->clone());
        if (!geometry || geometry->transform(transform.get()) != OGRERR_NONE) {
            result.issues.push_back({
                "CRS_TRANSFORM_FAILED",
                IssueSeverity::kError,
                "Geometry could not be transformed to runtime CRS",
                source_id,
                {},
                false,
            });
            continue;
        }

        const OGRwkbGeometryType type = wkbFlatten(geometry->getGeometryType());
        if (type == wkbLineString) {
            const auto* line = geometry->toLineString();
            appendLine(
                *line,
                *feature,
                source_id,
                0,
                oneway_index,
                speed_index,
                lanes_index,
                class_index,
                level_index,
                bridge_index,
                tunnel_index,
                options,
                result);
        } else if (type == wkbMultiLineString) {
            const auto* multi = geometry->toMultiLineString();
            for (int part = 0; part < multi->getNumGeometries(); ++part) {
                const auto* line =
                    dynamic_cast<const OGRLineString*>(multi->getGeometryRef(part));
                if (line == nullptr) {
                    continue;
                }
                appendLine(
                    *line,
                    *feature,
                    source_id,
                    part,
                    oneway_index,
                    speed_index,
                    lanes_index,
                    class_index,
                    level_index,
                    bridge_index,
                    tunnel_index,
                    options,
                    result);
            }
        } else {
            result.issues.push_back({
                "UNSUPPORTED_GEOMETRY",
                IssueSeverity::kError,
                "Expected LineString or MultiLineString",
                source_id,
                {},
                false,
            });
        }
    }

    if (result.roads.empty()) {
        throw std::runtime_error("no valid road lines were imported from: " + path);
    }
    if (!options.turn_restrictions_file.empty()) {
        result.turn_transitions = loadTurnTransitions(
            options.turn_restrictions_file, *transform);
    }
    return result;
}

std::string ShapefileImporter::inspect(const std::string& path) {
    GDALAllRegister();
    DatasetPtr dataset(static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
    if (!dataset) {
        throw std::runtime_error("cannot open vector dataset: " + path);
    }

    std::ostringstream output;
    output << "dataset=" << std::filesystem::absolute(path).string() << '\n';
    output << "driver=" << dataset->GetDriverName() << '\n';
    output << "layers=" << dataset->GetLayerCount() << '\n';
    for (int i = 0; i < dataset->GetLayerCount(); ++i) {
        OGRLayer* layer = dataset->GetLayer(i);
        if (layer == nullptr) {
            continue;
        }
        output << "layer[" << i << "].name=" << layer->GetName() << '\n';
        output << "layer[" << i << "].features=" << layer->GetFeatureCount(TRUE) << '\n';
        output << "layer[" << i << "].geometry="
               << OGRGeometryTypeToName(layer->GetGeomType()) << '\n';
        if (wkbFlatten(layer->GetGeomType()) == wkbUnknown) {
            // Mixed layers report "Unknown (any)"; count each feature's real
            // geometry type so callers can still classify the layer.
            std::map<OGRwkbGeometryType, GIntBig> geometry_counts;
            layer->ResetReading();
            while (OGRFeature* raw_feature = layer->GetNextFeature()) {
                std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
                    raw_feature, &OGRFeature::DestroyFeature);
                const OGRGeometry* geometry = feature->GetGeometryRef();
                if (geometry == nullptr || geometry->IsEmpty()) {
                    continue;
                }
                ++geometry_counts[wkbFlatten(geometry->getGeometryType())];
            }
            layer->ResetReading();
            if (!geometry_counts.empty()) {
                output << "layer[" << i << "].geometry_counts=";
                bool first_type = true;
                for (const auto& [geometry_type, count] : geometry_counts) {
                    if (!first_type) {
                        output << ',';
                    }
                    first_type = false;
                    output << OGRGeometryTypeToName(geometry_type) << ':' << count;
                }
                output << '\n';
            }
        }
        if (const OGRSpatialReference* srs = layer->GetSpatialRef()) {
            output << "layer[" << i << "].crs=" << srs->GetName() << '\n';
        } else {
            output << "layer[" << i << "].crs=<missing>\n";
        }
        if (OGRFeatureDefn* definition = layer->GetLayerDefn()) {
            for (int field = 0; field < definition->GetFieldCount(); ++field) {
                const OGRFieldDefn* field_definition = definition->GetFieldDefn(field);
                output << "layer[" << i << "].field[" << field << "]="
                       << field_definition->GetNameRef() << ':'
                       << OGRFieldDefn::GetFieldTypeName(field_definition->GetType()) << '\n';
            }
        }
    }
    return output.str();
}

ImportOptions loadImportOptions(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open mapping file: " + path);
    }

    ImportOptions options;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error(
                "invalid mapping line " + std::to_string(line_number) + ": expected key=value");
        }
        const std::string key = lowercase(trim(line.substr(0, separator)));
        const std::string value = trim(line.substr(separator + 1));
        if (key == "id_field") options.id_field = value;
        else if (key == "oneway_field") options.oneway_field = value;
        else if (key == "speed_field") options.speed_field = value;
        else if (key == "lanes_field") options.lanes_field = value;
        else if (key == "road_class_field") options.road_class_field = value;
        else if (key == "z_level_field") options.z_level_field = value;
        else if (key == "bridge_field") options.bridge_field = value;
        else if (key == "tunnel_field") options.tunnel_field = value;
        else if (key == "turn_restrictions_file") options.turn_restrictions_file = value;
        else if (key == "target_crs") options.target_crs = value;
        else if (key == "default_speed_kph") options.default_speed_kph = std::stod(value);
        else if (key == "snap_tolerance_m") options.snap_tolerance_m = std::stod(value);
        else if (key == "default_bidirectional") options.default_bidirectional = truthy(value);
        else throw std::runtime_error("unknown mapping key on line " + std::to_string(line_number));
    }
    return options;
}

}  // namespace zeus::map
