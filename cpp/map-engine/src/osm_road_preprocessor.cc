#include "zeus/map/osm_road_preprocessor.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <cpl_string.h>
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

bool osmTruthy(const std::string& raw) {
    const std::string value = lowercase(trim(raw));
    return !value.empty() && value != "0" && value != "no" && value != "false";
}

int findField(const OGRFeatureDefn& definition, std::string_view name) {
    const std::string expected = lowercase(std::string(name));
    for (int index = 0; index < definition.GetFieldCount(); ++index) {
        const OGRFieldDefn* field = definition.GetFieldDefn(index);
        if (field != nullptr && lowercase(field->GetNameRef()) == expected) {
            return index;
        }
    }
    return -1;
}

std::string fieldString(const OGRFeature& feature, int index) {
    if (index < 0 || !feature.IsFieldSetAndNotNull(index)) {
        return {};
    }
    return trim(feature.GetFieldAsString(index));
}

struct SourceFields {
    int osm_id = -1;
    int name = -1;
    int ref = -1;
    int highway = -1;
    int oneway = -1;
    int maxspeed = -1;
    int maxspeed_forward = -1;
    int maxspeed_backward = -1;
    int lanes = -1;
    int lanes_forward = -1;
    int lanes_backward = -1;
    int bridge = -1;
    int tunnel = -1;
    int layer = -1;
    int junction = -1;
    int access = -1;
    int vehicle = -1;
    int motor_vehicle = -1;
    int motorcar = -1;
    int service = -1;
    int surface = -1;
};

SourceFields sourceFields(const OGRFeatureDefn& definition) {
    return {
        findField(definition, "osm_id"),
        findField(definition, "name"),
        findField(definition, "ref"),
        findField(definition, "highway"),
        findField(definition, "oneway"),
        findField(definition, "maxspeed"),
        findField(definition, "maxspeed:forward"),
        findField(definition, "maxspeed:backward"),
        findField(definition, "lanes"),
        findField(definition, "lanes:forward"),
        findField(definition, "lanes:backward"),
        findField(definition, "bridge"),
        findField(definition, "tunnel"),
        findField(definition, "layer"),
        findField(definition, "junction"),
        findField(definition, "access"),
        findField(definition, "vehicle"),
        findField(definition, "motor_vehicle"),
        findField(definition, "motorcar"),
        findField(definition, "service"),
        findField(definition, "surface"),
    };
}

bool isAlwaysDrivableClass(const std::string& highway) {
    static const std::unordered_set<std::string> classes = {
        "motorway", "motorway_link", "trunk", "trunk_link", "primary",
        "primary_link", "secondary", "secondary_link", "tertiary",
        "tertiary_link", "unclassified", "residential", "living_street", "road",
    };
    return classes.contains(highway);
}

std::optional<std::string> classExclusion(
    const std::string& highway,
    const OsmPreprocessOptions& options) {
    if (highway.empty()) {
        return "missing_highway";
    }
    if (isAlwaysDrivableClass(highway)) {
        return std::nullopt;
    }
    if (highway == "service") {
        return options.include_service ? std::nullopt
                                       : std::optional<std::string>("service_disabled");
    }
    if (highway == "track") {
        return options.include_track ? std::nullopt
                                     : std::optional<std::string>("track_disabled");
    }
    return "non_drivable_class";
}

std::string effectiveAccess(const OGRFeature& feature, const SourceFields& fields) {
    for (const int index : {
             fields.motorcar, fields.motor_vehicle, fields.vehicle, fields.access}) {
        const std::string value = lowercase(fieldString(feature, index));
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

bool isRestrictedAccess(const std::string& value) {
    return value == "no" || value == "private" || value == "agricultural" ||
           value == "forestry";
}

double defaultSpeedKph(const std::string& highway) {
    if (highway == "motorway" || highway == "motorway_link") return 100.0;
    if (highway == "trunk" || highway == "trunk_link") return 80.0;
    if (highway == "primary" || highway == "primary_link") return 60.0;
    if (highway == "secondary" || highway == "secondary_link") return 50.0;
    if (highway == "tertiary" || highway == "tertiary_link") return 40.0;
    if (highway == "residential" || highway == "unclassified" || highway == "road") {
        return 30.0;
    }
    if (highway == "living_street") return 10.0;
    if (highway == "service") return 20.0;
    if (highway == "track") return 15.0;
    return 30.0;
}

struct NormalizedSpeed {
    double kph = 0.0;
    bool used_default = false;
    bool converted_mph = false;
};

NormalizedSpeed normalizeSpeed(const std::string& raw, const std::string& highway) {
    const std::string value = lowercase(trim(raw));
    if (value == "none") {
        return {120.0, false, false};
    }
    if (value == "walk" || value == "walking_pace") {
        return {5.0, false, false};
    }
    if (value == "cn:urban") return {50.0, false, false};
    if (value == "cn:rural") return {70.0, false, false};
    if (value == "cn:expressway" || value == "cn:motorway") {
        return {120.0, false, false};
    }
    // Conditional or multi-value tags are represented conservatively by the
    // lowest parseable limit. This handles forms such as "50;60" without
    // pretending that their time/vehicle applicability is already modeled.
    double minimum_kph = std::numeric_limits<double>::infinity();
    bool converted_mph = false;
    std::size_t begin_offset = 0;
    while (begin_offset <= value.size()) {
        const std::size_t separator = value.find(';', begin_offset);
        const std::string token = trim(value.substr(
            begin_offset, separator == std::string::npos
                              ? std::string::npos
                              : separator - begin_offset));
        const char* begin = token.c_str();
        char* end = nullptr;
        const double parsed = std::strtod(begin, &end);
        if (end != begin && std::isfinite(parsed) && parsed > 0.0) {
            const bool mph = token.find("mph") != std::string::npos;
            const double kph = parsed * (mph ? 1.609344 : 1.0);
            if (kph >= 1.0 && kph <= 250.0 && kph < minimum_kph) {
                minimum_kph = kph;
                converted_mph = mph;
            }
        }
        if (separator == std::string::npos) {
            break;
        }
        begin_offset = separator + 1;
    }
    if (std::isfinite(minimum_kph)) {
        return {minimum_kph, false, converted_mph};
    }
    return {defaultSpeedKph(highway), true, false};
}

struct NormalizedDirection {
    const char* value = "both";
    bool implied = false;
    bool reverse = false;
};

NormalizedDirection normalizeDirection(
    const std::string& raw,
    const std::string& highway,
    const std::string& junction) {
    const std::string value = lowercase(trim(raw));
    if (value == "-1" || value == "reverse" || value == "backward") {
        return {"reverse", false, true};
    }
    if (value == "yes" || value == "1" || value == "true" || value == "forward") {
        return {"forward", false, false};
    }
    if (value == "no" || value == "0" || value == "false" || value == "both") {
        return {"both", false, false};
    }
    if (junction == "roundabout" || junction == "circular" || highway == "motorway" ||
        highway == "motorway_link") {
        return {"forward", true, false};
    }
    return {"both", false, false};
}

int parseInteger(const std::string& raw, int fallback, int minimum, int maximum) {
    if (raw.empty()) {
        return fallback;
    }
    const char* begin = raw.c_str();
    char* end = nullptr;
    const long value = std::strtol(begin, &end, 10);
    if (end == begin) {
        return fallback;
    }
    return std::clamp(static_cast<int>(value), minimum, maximum);
}

// The canonical `lanes` field is lanes per directed edge, matching the
// runtime capacity model. OSM `lanes` normally describes the total across
// both directions, so bidirectional roads are divided conservatively.
int normalizeLaneCount(
    const std::string& raw,
    const std::string& forward_raw,
    const std::string& backward_raw,
    const std::string& highway,
    const NormalizedDirection& direction) {
    const int forward = parseInteger(forward_raw, 0, 0, 32);
    const int backward = parseInteger(backward_raw, 0, 0, 32);
    if (forward > 0 || backward > 0) {
        if (forward > 0 && backward > 0) {
            return std::min(forward, backward);
        }
        return std::max(forward, backward);
    }
    const int tagged = parseInteger(raw, 0, 0, 32);
    if (tagged > 0) {
        return std::string_view(direction.value) == "both"
                   ? std::max(1, tagged / 2)
                   : tagged;
    }
    if (highway == "motorway" || highway == "trunk" ||
        highway == "primary") {
        return 2;
    }
    return 1;
}

int normalizeLevel(const OGRFeature& feature, const SourceFields& fields) {
    int level = parseInteger(fieldString(feature, fields.layer), 0, -20, 20);
    if (level == 0 && osmTruthy(fieldString(feature, fields.bridge))) {
        level = 1;
    } else if (level == 0 && osmTruthy(fieldString(feature, fields.tunnel))) {
        level = -1;
    }
    return level;
}

void appendLines(const OGRGeometry& geometry, OGRMultiLineString& output) {
    const OGRwkbGeometryType type = wkbFlatten(geometry.getGeometryType());
    if (type == wkbLineString) {
        output.addGeometry(geometry.toLineString());
        return;
    }
    const auto* collection = dynamic_cast<const OGRGeometryCollection*>(&geometry);
    if (collection == nullptr) {
        return;
    }
    for (int index = 0; index < collection->getNumGeometries(); ++index) {
        const OGRGeometry* child = collection->getGeometryRef(index);
        if (child != nullptr) {
            appendLines(*child, output);
        }
    }
}

GeometryPtr linealGeometry(const OGRGeometry& source, bool& collection_converted) {
    const OGRwkbGeometryType type = wkbFlatten(source.getGeometryType());
    if (type == wkbLineString || type == wkbMultiLineString) {
        return GeometryPtr(source.clone());
    }
    if (type != wkbGeometryCollection) {
        return {};
    }
    auto multi = std::make_unique<OGRMultiLineString>();
    appendLines(source, *multi);
    if (multi->getNumGeometries() == 0) {
        return {};
    }
    collection_converted = true;
    return GeometryPtr(multi.release());
}

constexpr double kEarthRadiusM = 6371008.8;

double radians(double degrees) {
    return degrees * std::acos(-1.0) / 180.0;
}

double segmentLengthM(double lon1, double lat1, double lon2, double lat2) {
    const double dlat = radians(lat2 - lat1);
    const double dlon = radians(lon2 - lon1);
    const double lat1_rad = radians(lat1);
    const double lat2_rad = radians(lat2);
    const double a = std::sin(dlat * 0.5) * std::sin(dlat * 0.5) +
                     std::cos(lat1_rad) * std::cos(lat2_rad) *
                         std::sin(dlon * 0.5) * std::sin(dlon * 0.5);
    return 2.0 * kEarthRadiusM * std::asin(std::sqrt(std::clamp(a, 0.0, 1.0)));
}

double lineLengthM(const OGRLineString& line) {
    double length = 0.0;
    for (int index = 1; index < line.getNumPoints(); ++index) {
        length += segmentLengthM(
            line.getX(index - 1), line.getY(index - 1), line.getX(index), line.getY(index));
    }
    return length;
}

double geometryLengthM(const OGRGeometry& geometry) {
    const OGRwkbGeometryType type = wkbFlatten(geometry.getGeometryType());
    if (type == wkbLineString) {
        return lineLengthM(*geometry.toLineString());
    }
    const auto* collection = dynamic_cast<const OGRGeometryCollection*>(&geometry);
    if (collection == nullptr) {
        return 0.0;
    }
    double length = 0.0;
    for (int index = 0; index < collection->getNumGeometries(); ++index) {
        const OGRGeometry* child = collection->getGeometryRef(index);
        if (child != nullptr) {
            length += geometryLengthM(*child);
        }
    }
    return length;
}

std::string geometryKey(
    const OGRGeometry& geometry,
    const std::string& highway,
    std::string_view direction) {
    const std::size_t size = geometry.WkbSize();
    std::string key(size, '\0');
    if (geometry.exportToWkb(wkbNDR, reinterpret_cast<unsigned char*>(key.data())) != OGRERR_NONE) {
        return {};
    }
    key.push_back('\0');
    key += highway;
    key.push_back('\0');
    key += direction;
    return key;
}

void createField(OGRLayer& layer, const char* name, OGRFieldType type) {
    OGRFieldDefn field(name, type);
    if (layer.CreateField(&field) != OGRERR_NONE) {
        throw std::runtime_error(std::string("cannot create OSM output field: ") + name);
    }
}

std::string jsonEscape(std::string_view value) {
    std::ostringstream output;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (ch < 0x20) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<int>(ch) << std::dec;
                } else {
                    output << static_cast<char>(ch);
                }
        }
    }
    return output.str();
}

void writeCountMap(
    std::ostream& output,
    const std::map<std::string, std::uint64_t>& values,
    int indent) {
    output << "{";
    if (!values.empty()) output << '\n';
    std::size_t index = 0;
    for (const auto& [key, value] : values) {
        output << std::string(static_cast<std::size_t>(indent + 2), ' ')
               << '"' << jsonEscape(key) << "\": " << value;
        if (++index < values.size()) output << ',';
        output << '\n';
    }
    if (!values.empty()) output << std::string(static_cast<std::size_t>(indent), ' ');
    output << "}";
}

void exclude(OsmPreprocessReport& report, const std::string& reason) {
    ++report.filtered_features;
    ++report.excluded_by_reason[reason];
}

}  // namespace

OsmPreprocessReport OsmRoadPreprocessor::process(
    const std::string& input_path,
    const std::string& output_path,
    const OsmPreprocessOptions& options) const {
    if (!std::isfinite(options.min_length_m) || options.min_length_m < 0.0) {
        throw std::invalid_argument("minimum road length must be a finite non-negative value");
    }

    GDALAllRegister();
    DatasetPtr input(static_cast<GDALDataset*>(GDALOpenEx(
        input_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
    if (!input || input->GetLayerCount() == 0) {
        throw std::runtime_error("cannot open OSM road vector source: " + input_path);
    }
    OGRLayer* input_layer = input->GetLayer(0);
    if (input_layer == nullptr || input_layer->GetLayerDefn() == nullptr) {
        throw std::runtime_error("OSM road vector source has no readable first layer");
    }
    const OGRSpatialReference* source_srs = input_layer->GetSpatialRef();
    if (source_srs == nullptr) {
        throw std::runtime_error("OSM road source has no CRS");
    }

    OGRSpatialReference source(*source_srs);
    source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    TransformPtr transform(OGRCreateCoordinateTransformation(&source, &wgs84));
    if (!transform) {
        throw std::runtime_error("cannot transform OSM road source to WGS84");
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
        throw std::runtime_error("cannot create cleaned OSM road output: " + output_path);
    }
    char** layer_options = nullptr;
    layer_options = CSLSetNameValue(layer_options, "RFC7946", "YES");
    layer_options = CSLSetNameValue(layer_options, "WRITE_BBOX", "YES");
    OGRLayer* output_layer = output->CreateLayer("roads", &wgs84, wkbUnknown, layer_options);
    CSLDestroy(layer_options);
    if (output_layer == nullptr) {
        throw std::runtime_error("cannot create cleaned OSM road layer");
    }

    createField(*output_layer, "road_id", OFTString);
    createField(*output_layer, "name", OFTString);
    createField(*output_layer, "ref", OFTString);
    createField(*output_layer, "road_class", OFTString);
    createField(*output_layer, "oneway", OFTString);
    createField(*output_layer, "speed_kph", OFTReal);
    createField(*output_layer, "lanes", OFTInteger);
    createField(*output_layer, "bridge", OFTString);
    createField(*output_layer, "tunnel", OFTString);
    createField(*output_layer, "z_level", OFTInteger);
    createField(*output_layer, "access", OFTString);
    createField(*output_layer, "service", OFTString);
    createField(*output_layer, "surface", OFTString);
    createField(*output_layer, "osm_highway", OFTString);
    createField(*output_layer, "source_maxspeed", OFTString);
    createField(*output_layer, "length_m", OFTReal);

    const SourceFields fields = sourceFields(*input_layer->GetLayerDefn());
    if (fields.highway < 0) {
        throw std::runtime_error("OSM road source is missing required highway field");
    }

    OsmPreprocessReport report;
    std::unordered_set<std::string> seen_geometries;
    input_layer->ResetReading();
    while (OGRFeature* raw_feature = input_layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            raw_feature, &OGRFeature::DestroyFeature);
        ++report.input_features;

        const std::string highway = lowercase(fieldString(*feature, fields.highway));
        if (const auto reason = classExclusion(highway, options); reason.has_value()) {
            exclude(report, *reason);
            continue;
        }
        const std::string access = effectiveAccess(*feature, fields);
        if (!options.include_private && isRestrictedAccess(access)) {
            exclude(report, "access_restricted");
            continue;
        }

        OGRGeometry* source_geometry = feature->GetGeometryRef();
        if (source_geometry == nullptr || source_geometry->IsEmpty()) {
            exclude(report, "empty_geometry");
            continue;
        }
        GeometryPtr transformed(source_geometry->clone());
        if (!transformed || transformed->transform(transform.get()) != OGRERR_NONE) {
            exclude(report, "crs_transform_failed");
            continue;
        }
        bool collection_converted = false;
        GeometryPtr geometry = linealGeometry(*transformed, collection_converted);
        if (!geometry) {
            exclude(report, "unsupported_geometry");
            continue;
        }
        if (collection_converted) {
            ++report.geometry_collections_converted;
        }

        const double length_m = geometryLengthM(*geometry);
        if (!std::isfinite(length_m) || length_m < options.min_length_m) {
            exclude(report, "too_short");
            continue;
        }

        const std::string junction = lowercase(fieldString(*feature, fields.junction));
        const NormalizedDirection direction = normalizeDirection(
            fieldString(*feature, fields.oneway), highway, junction);
        std::string speed_values = fieldString(*feature, fields.maxspeed);
        for (const int directional : {fields.maxspeed_forward, fields.maxspeed_backward}) {
            const std::string value = fieldString(*feature, directional);
            if (!value.empty()) {
                if (!speed_values.empty()) speed_values.push_back(';');
                speed_values += value;
            }
        }
        const NormalizedSpeed speed = normalizeSpeed(speed_values, highway);

        const std::string key = geometryKey(*geometry, highway, direction.value);
        if (!key.empty() && !seen_geometries.insert(key).second) {
            ++report.duplicate_geometries_removed;
            exclude(report, "duplicate_geometry");
            continue;
        }
        if (direction.implied) ++report.implied_oneway_applied;
        if (direction.reverse) ++report.reverse_oneway_normalized;
        if (speed.used_default) ++report.default_speed_applied;
        if (speed.converted_mph) ++report.mph_speed_converted;

        std::string road_id = fieldString(*feature, fields.osm_id);
        if (road_id.empty()) {
            road_id = std::to_string(feature->GetFID());
        }
        const std::string bridge = osmTruthy(fieldString(*feature, fields.bridge)) ? "yes" : "no";
        const std::string tunnel = osmTruthy(fieldString(*feature, fields.tunnel)) ? "yes" : "no";

        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> cleaned(
            OGRFeature::CreateFeature(output_layer->GetLayerDefn()),
            &OGRFeature::DestroyFeature);
        cleaned->SetField("road_id", road_id.c_str());
        cleaned->SetField("name", fieldString(*feature, fields.name).c_str());
        cleaned->SetField("ref", fieldString(*feature, fields.ref).c_str());
        cleaned->SetField("road_class", highway.c_str());
        cleaned->SetField("oneway", direction.value);
        cleaned->SetField("speed_kph", speed.kph);
        cleaned->SetField("lanes", normalizeLaneCount(
            fieldString(*feature, fields.lanes),
            fieldString(*feature, fields.lanes_forward),
            fieldString(*feature, fields.lanes_backward), highway, direction));
        cleaned->SetField("bridge", bridge.c_str());
        cleaned->SetField("tunnel", tunnel.c_str());
        cleaned->SetField("z_level", normalizeLevel(*feature, fields));
        cleaned->SetField("access", access.c_str());
        cleaned->SetField("service", fieldString(*feature, fields.service).c_str());
        cleaned->SetField("surface", fieldString(*feature, fields.surface).c_str());
        cleaned->SetField("osm_highway", highway.c_str());
        cleaned->SetField("source_maxspeed", fieldString(*feature, fields.maxspeed).c_str());
        cleaned->SetField("length_m", length_m);
        cleaned->SetGeometryDirectly(geometry.release());
        if (output_layer->CreateFeature(cleaned.get()) != OGRERR_NONE) {
            throw std::runtime_error("cannot write cleaned OSM road feature");
        }
        ++report.output_features;
        ++report.output_by_class[highway];
    }

    if (report.output_features == 0) {
        throw std::runtime_error("OSM preprocessing produced no drivable road features");
    }
    return report;
}

void OsmRoadPreprocessor::saveReport(
    const OsmPreprocessReport& report,
    const OsmPreprocessOptions& options,
    const std::string& input_path,
    const std::string& output_path,
    const std::string& report_path) {
    std::ofstream output(report_path);
    if (!output) {
        throw std::runtime_error("cannot create OSM preprocessing report: " + report_path);
    }
    output << "{\n"
           << "  \"input\": \"" << jsonEscape(std::filesystem::absolute(input_path).string())
           << "\",\n"
           << "  \"output\": \"" << jsonEscape(std::filesystem::absolute(output_path).string())
           << "\",\n"
           << "  \"profile\": \"car\",\n"
           << "  \"options\": {\n"
           << "    \"include_service\": " << (options.include_service ? "true" : "false")
           << ",\n"
           << "    \"include_track\": " << (options.include_track ? "true" : "false")
           << ",\n"
           << "    \"include_private\": " << (options.include_private ? "true" : "false")
           << ",\n"
           << "    \"min_length_m\": " << options.min_length_m << "\n"
           << "  },\n"
           << "  \"input_features\": " << report.input_features << ",\n"
           << "  \"output_features\": " << report.output_features << ",\n"
           << "  \"filtered_features\": " << report.filtered_features << ",\n"
           << "  \"normalization\": {\n"
           << "    \"geometry_collections_converted\": "
           << report.geometry_collections_converted << ",\n"
           << "    \"default_speed_applied\": " << report.default_speed_applied << ",\n"
           << "    \"mph_speed_converted\": " << report.mph_speed_converted << ",\n"
           << "    \"implied_oneway_applied\": " << report.implied_oneway_applied << ",\n"
           << "    \"reverse_oneway_normalized\": "
           << report.reverse_oneway_normalized << ",\n"
           << "    \"duplicate_geometries_removed\": "
           << report.duplicate_geometries_removed << "\n"
           << "  },\n"
           << "  \"excluded_by_reason\": ";
    writeCountMap(output, report.excluded_by_reason, 2);
    output << ",\n  \"output_by_class\": ";
    writeCountMap(output, report.output_by_class, 2);
    output << "\n}\n";
}

}  // namespace zeus::map
