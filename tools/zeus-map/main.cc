#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <ogr_spatialref.h>

#include "zeus/map/geojson_exporter.h"
#include "zeus/map/map_builder.h"
#include "zeus/map/map_runtime.h"
#include "zeus/map/map_serializer.h"
#include "zeus/map/map_validator.h"
#include "zeus/map/osm_road_preprocessor.h"
#include "zeus/map/shapefile_importer.h"
#include "zeus/routing/route_exporter.h"
#include "zeus/routing/route_planner.h"
#include "zeus/simulation/playback_exporter.h"
#include "zeus/simulation/simulation_engine.h"

namespace {

using Options = std::unordered_map<std::string, std::string>;

void printUsage() {
    std::cout
        << "Zeus map engine CLI\n\n"
        << "Usage:\n"
        << "  zeus-map inspect <roads.shp|roads.geojson>\n"
        << "  zeus-map preprocess-osm <roads.geojson> --output <clean.geojson>\n"
        << "                          --report <report.json> [options]\n"
        << "  zeus-map import <roads.shp|roads.geojson> --output <map.zmap> [options]\n"
        << "  zeus-map validate <map.zmap>\n"
        << "  zeus-map geojson <map.zmap> --output <roads.geojson>\n"
        << "  zeus-map nodes-geojson <map.zmap> --output <nodes.geojson>\n"
        << "  zeus-map reference-geojson <source> --output <reference.geojson>\n"
        << "  zeus-map issues-geojson <map.zmap> --output <issues.geojson>\n"
        << "  zeus-map query <map.zmap> (--x X --y Y | --lon LON --lat LAT) [options]\n"
        << "  zeus-map pose <map.zmap> --edge INDEX --offset METERS [--lateral METERS]\n"
        << "  zeus-map route <map.zmap> (--lon LON --lat LAT --dest-lon LON --dest-lat LAT\n"
        << "                       | --x X --y Y --dest-x X --dest-y Y) [options]\n"
        << "  zeus-map route-worker <map.zmap>  # framed requests on stdin/stdout\n"
        << "  zeus-map simulate <map.zmap> (--lon LON --lat LAT --dest-lon LON --dest-lat LAT\n"
        << "                          [--count N --spread S] | --od-file FILE) [options]\n\n"
        << "Import options:\n"
        << "  --mapping FILE             key=value mapping file\n"
        << "  --id-field FIELD\n"
        << "  --oneway-field FIELD\n"
        << "  --speed-field FIELD\n"
        << "  --lanes-field FIELD\n"
        << "  --class-field FIELD\n"
        << "  --level-field FIELD\n"
        << "  --bridge-field FIELD\n"
        << "  --tunnel-field FIELD\n"
        << "  --turn-restrictions FILE  source-CRS turn rules CSV\n"
        << "  --target-crs DEFINITION   for example EPSG:32650\n"
        << "  --snap METERS\n"
        << "  --default-speed KPH\n"
        << "  --issues-output FILE      write located validation issues as WGS84 GeoJSON\n\n"
        << "OSM preprocessing options:\n"
        << "  --profile car             currently only the car profile is supported\n"
        << "  --include-service BOOL    include highway=service (default false)\n"
        << "  --include-track BOOL      include highway=track (default false)\n"
        << "  --include-private BOOL    include restricted access roads (default false)\n"
        << "  --min-length METERS       discard shorter geometry (default 2)\n\n"
        << "Query options:\n"
        << "  --heading DEGREES\n"
        << "  --max-distance METERS\n"
        << "  --limit COUNT\n\n"
        << "Route options:\n"
        << "  --algorithm dijkstra|astar|bidijkstra|biastar\n"
        << "  --max-distance METERS      endpoint snap distance\n"
        << "  --output FILE              write the route as WGS84 GeoJSON\n\n"
        << "Simulate options:\n"
        << "  --count N                  vehicles for the single OD pair (default 1)\n"
        << "  --spread SECONDS           linear departure window (default 0)\n"
        << "  --od-file FILE             lon,lat,dest_lon,dest_lat,depart_s[,algorithm] rows\n"
        << "  --algorithm dijkstra|astar|bidijkstra|biastar\n"
        << "  --duration SECONDS         simulation horizon (default 3600)\n"
        << "  --step SECONDS             tick length (default 1)\n"
        << "  --sample-interval SECONDS  trajectory sampling (default 30)\n"
        << "  --controls FILE            time,scope,target,action[,value] rows\n"
        << "  --output FILE              write per-vehicle WGS84 trajectory GeoJSON\n"
        << "  --playback FILE            write the web playback document\n";
}

Options parseOptions(int argc, char** argv, int start) {
    Options result;
    for (int i = start; i < argc; i += 2) {
        const std::string key = argv[i];
        if (!key.starts_with("--") || i + 1 >= argc) {
            throw std::invalid_argument("expected --option value near: " + key);
        }
        result[key.substr(2)] = argv[i + 1];
    }
    return result;
}

std::string required(const Options& options, const std::string& key) {
    const auto found = options.find(key);
    if (found == options.end() || found->second.empty()) {
        throw std::invalid_argument("missing required option --" + key);
    }
    return found->second;
}

void overrideString(
    const Options& options,
    const std::string& key,
    std::string& destination) {
    if (const auto found = options.find(key); found != options.end()) {
        destination = found->second;
    }
}

bool optionBool(const Options& options, const std::string& key, bool fallback) {
    const auto found = options.find(key);
    if (found == options.end()) {
        return fallback;
    }
    std::string value = found->second;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "off") return false;
    throw std::invalid_argument("option --" + key + " expects a boolean value");
}

zeus::map::ImportOptions importOptions(const Options& arguments) {
    zeus::map::ImportOptions options;
    if (const auto mapping = arguments.find("mapping"); mapping != arguments.end()) {
        options = zeus::map::loadImportOptions(mapping->second);
    }
    overrideString(arguments, "id-field", options.id_field);
    overrideString(arguments, "oneway-field", options.oneway_field);
    overrideString(arguments, "speed-field", options.speed_field);
    overrideString(arguments, "lanes-field", options.lanes_field);
    overrideString(arguments, "class-field", options.road_class_field);
    overrideString(arguments, "level-field", options.z_level_field);
    overrideString(arguments, "bridge-field", options.bridge_field);
    overrideString(arguments, "tunnel-field", options.tunnel_field);
    overrideString(arguments, "turn-restrictions", options.turn_restrictions_file);
    overrideString(arguments, "target-crs", options.target_crs);
    if (const auto found = arguments.find("snap"); found != arguments.end()) {
        options.snap_tolerance_m = std::stod(found->second);
    }
    if (const auto found = arguments.find("default-speed"); found != arguments.end()) {
        options.default_speed_kph = std::stod(found->second);
    }
    return options;
}

void printReport(const zeus::map::ValidationReport& report, bool include_issues) {
    std::cout << "nodes=" << report.node_count << '\n'
              << "directed_edges=" << report.edge_count << '\n'
              << "components=" << report.component_count << '\n'
              << "largest_component_nodes=" << report.largest_component_nodes << '\n'
              << "fatal=" << report.count(zeus::map::IssueSeverity::kFatal) << '\n'
              << "errors=" << report.count(zeus::map::IssueSeverity::kError) << '\n'
              << "warnings=" << report.count(zeus::map::IssueSeverity::kWarning) << '\n'
              << "info=" << report.count(zeus::map::IssueSeverity::kInfo) << '\n';
    if (!include_issues) {
        return;
    }
    for (const zeus::map::ValidationIssue& issue : report.issues) {
        std::cout << "issue=" << zeus::map::severityName(issue.severity) << ':' << issue.code
                  << " source=" << (issue.source_id.empty() ? "-" : issue.source_id)
                  << " location=" << issue.location.x << ',' << issue.location.y
                  << " message=\"" << issue.message << "\"\n";
    }
}

zeus::map::Point2d wgs84ToRuntime(
    double lon,
    double lat,
    const std::string& runtime_crs_wkt) {
    OGRSpatialReference source;
    source.SetWellKnownGeogCS("WGS84");
    source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReference target;
    if (target.importFromWkt(runtime_crs_wkt.c_str()) != OGRERR_NONE) {
        throw std::runtime_error("runtime map contains an invalid CRS");
    }
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    struct TransformDeleter {
        void operator()(OGRCoordinateTransformation* transform) const {
            OGRCoordinateTransformation::DestroyCT(transform);
        }
    };
    std::unique_ptr<OGRCoordinateTransformation, TransformDeleter> transform(
        OGRCreateCoordinateTransformation(&source, &target));
    if (!transform || !transform->Transform(1, &lon, &lat)) {
        throw std::runtime_error("failed to transform WGS84 query point to runtime CRS");
    }
    return {lon, lat};
}

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

// Transforms WGS84 lon/lat arrays in place into the runtime CRS.
void transformWgs84Batch(
    std::vector<double>& xs,
    std::vector<double>& ys,
    const std::string& runtime_crs_wkt) {
    if (xs.empty()) {
        return;
    }
    OGRSpatialReference source;
    source.SetWellKnownGeogCS("WGS84");
    source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReference target;
    if (target.importFromWkt(runtime_crs_wkt.c_str()) != OGRERR_NONE) {
        throw std::runtime_error("runtime map contains an invalid CRS");
    }
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    struct TransformDeleter {
        void operator()(OGRCoordinateTransformation* transform) const {
            OGRCoordinateTransformation::DestroyCT(transform);
        }
    };
    std::unique_ptr<OGRCoordinateTransformation, TransformDeleter> transform(
        OGRCreateCoordinateTransformation(&source, &target));
    if (!transform ||
        !transform->Transform(static_cast<int>(xs.size()), xs.data(), ys.data())) {
        throw std::runtime_error("failed to transform WGS84 demand points to runtime CRS");
    }
}

int executeRoute(
    const zeus::map::MapRuntime& runtime,
    const zeus::routing::RoutePlanner& planner,
    const Options& options,
    std::ostream& output) {
    zeus::routing::RouteRequest request;
    if (options.contains("lon") && options.contains("lat") &&
        options.contains("dest-lon") && options.contains("dest-lat")) {
        std::vector<double> xs = {
            std::stod(options.at("lon")), std::stod(options.at("dest-lon"))};
        std::vector<double> ys = {
            std::stod(options.at("lat")), std::stod(options.at("dest-lat"))};
        transformWgs84Batch(xs, ys, runtime.data().metadata.runtime_crs_wkt);
        request.origin = {xs[0], ys[0]};
        request.destination = {xs[1], ys[1]};
    } else if (options.contains("x") && options.contains("y") &&
               options.contains("dest-x") && options.contains("dest-y")) {
        request.origin = {std::stod(options.at("x")), std::stod(options.at("y"))};
        request.destination = {
            std::stod(options.at("dest-x")), std::stod(options.at("dest-y"))};
    } else {
        throw std::invalid_argument(
            "route requires --lon/--lat and --dest-lon/--dest-lat"
            " (or --x/--y and --dest-x/--dest-y)");
    }
    if (const auto found = options.find("algorithm"); found != options.end()) {
        if (!zeus::routing::parseAlgorithm(found->second, request.algorithm)) {
            throw std::invalid_argument("unknown routing algorithm: " + found->second);
        }
    }
    if (const auto found = options.find("max-distance"); found != options.end()) {
        request.max_snap_distance_m = std::stod(found->second);
    }

    const zeus::routing::RouteResult result = planner.plan(request);
    output << std::fixed << std::setprecision(3);
    if (!result.ok) {
        output << "route=failed\n"
               << "algorithm=" << zeus::routing::algorithmName(result.algorithm) << '\n'
               << "reason=" << zeus::routing::routeFailureName(result.failure) << '\n'
               << "message=" << result.message << '\n';
        return 3;
    }

    const auto print_match = [&runtime, &output](
                                 const char* label,
                                 const zeus::routing::RouteEndpointMatch& match) {
        const zeus::map::DirectedEdge& edge = runtime.edge(match.edge);
        output << label << ".edge=" << match.edge
               << " road_id=" << edge.road_id
               << " source=" << edge.source_id
               << " offset_s=" << match.offset_s
               << " distance=" << match.lateral_distance_m
               << " confidence=" << match.confidence << '\n';
    };
    output << "route=ok\n"
           << "algorithm=" << zeus::routing::algorithmName(result.algorithm) << '\n';
    print_match("origin", result.origin);
    print_match("dest", result.destination);
    output << "edges=" << result.path.edges.size() << '\n'
           << "length_m=" << result.stats.length_m << '\n'
           << "time_s=" << result.stats.time_s << '\n'
           << "expanded_nodes=" << result.stats.expanded_nodes << '\n'
           << "compute_ms=" << result.stats.compute_ms << '\n';
    if (const auto found_output = options.find("output");
        found_output != options.end() && !found_output->second.empty()) {
        const std::size_t features = zeus::routing::RouteGeoJsonExporter::save(
            runtime.data(), result, found_output->second);
        output << "features=" << features << '\n'
               << "output=" << found_output->second << '\n';
    }
    return 0;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t end = line.find('\t', begin);
        if (end == std::string::npos) {
            fields.push_back(line.substr(begin));
            return fields;
        }
        fields.push_back(line.substr(begin, end - begin));
        begin = end + 1;
    }
}

int runRouteWorker(const zeus::map::MapRuntime& runtime) {
    const zeus::routing::RoutePlanner planner(runtime);
    std::cout << "ZEUS_ROUTE_WORKER\t1\n" << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "shutdown") {
            return 0;
        }
        std::ostringstream payload;
        int exit_code = 1;
        try {
            const std::vector<std::string> fields = splitTabs(line);
            if (fields.size() != 7) {
                throw std::invalid_argument("route worker request must contain 7 tab fields");
            }
            Options options;
            options["lon"] = fields[0];
            options["lat"] = fields[1];
            options["dest-lon"] = fields[2];
            options["dest-lat"] = fields[3];
            options["algorithm"] = fields[4];
            options["max-distance"] = fields[5];
            if (!fields[6].empty()) {
                options["output"] = fields[6];
            }
            exit_code = executeRoute(runtime, planner, options, payload);
        } catch (const std::exception& error) {
            payload << "error: " << error.what() << '\n';
        }
        const std::string text = payload.str();
        std::cout << "ZEUS_ROUTE_RESPONSE\t" << exit_code << '\t' << text.size() << '\n';
        std::cout.write(text.data(), static_cast<std::streamsize>(text.size()));
        std::cout << std::flush;
    }
    return 0;
}

std::vector<zeus::simulation::SimulationControlEvent> parseSimulationControls(
    const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open simulation controls file: " + path);
    }
    std::vector<zeus::simulation::SimulationControlEvent> controls;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::vector<std::string> fields;
        std::size_t begin = 0;
        while (true) {
            const std::size_t comma = line.find(',', begin);
            if (comma == std::string::npos) {
                fields.push_back(trim(line.substr(begin)));
                break;
            }
            fields.push_back(trim(line.substr(begin, comma - begin)));
            begin = comma + 1;
        }
        if (fields.size() < 4 || fields.size() > 5) {
            throw std::runtime_error(
                "invalid controls line " + std::to_string(line_number) +
                ": expected time_s,vehicle|edge|junction,target_id,action[,value]");
        }
        zeus::simulation::SimulationControlEvent control;
        control.time_s = std::stod(fields[0]);
        const unsigned long long target_id = std::stoull(fields[2]);
        if (target_id > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error(
                "controls target ID exceeds uint32 range on line " +
                std::to_string(line_number));
        }
        control.target_id = static_cast<std::uint32_t>(target_id);
        if (fields[1] == "vehicle") {
            control.scope = zeus::simulation::ControlScope::kVehicle;
        } else if (fields[1] == "edge" || fields[1] == "road") {
            control.scope = zeus::simulation::ControlScope::kEdge;
        } else if (fields[1] == "junction") {
            control.scope = zeus::simulation::ControlScope::kJunction;
        } else {
            throw std::runtime_error(
                "invalid controls scope on line " + std::to_string(line_number));
        }
        if (fields[3] == "hold") {
            control.action = zeus::simulation::ControlAction::kHold;
        } else if (fields[3] == "release") {
            control.action = zeus::simulation::ControlAction::kRelease;
        } else if (fields[3] == "close") {
            control.action = zeus::simulation::ControlAction::kClose;
        } else if (fields[3] == "open") {
            control.action = zeus::simulation::ControlAction::kOpen;
        } else if (fields[3] == "speed_factor") {
            control.action = zeus::simulation::ControlAction::kSetSpeedFactor;
        } else if (fields[3] == "capacity_factor") {
            control.action = zeus::simulation::ControlAction::kSetCapacityFactor;
        } else {
            throw std::runtime_error(
                "invalid controls action on line " + std::to_string(line_number));
        }
        if (fields.size() == 5 && !fields[4].empty()) {
            control.value = std::stod(fields[4]);
        }
        controls.push_back(control);
    }
    return controls;
}

int run(int argc, char** argv) {
    if (argc < 3) {
        printUsage();
        return 1;
    }

    const std::string command = argv[1];
    const std::string input = argv[2];
    if (command == "inspect") {
        std::cout << zeus::map::ShapefileImporter::inspect(input);
        return 0;
    }

    const Options options = parseOptions(argc, argv, 3);
    if (command == "reference-geojson") {
        const std::string output = required(options, "output");
        const std::size_t features =
            zeus::map::GeoJsonExporter::saveReference(input, output);
        std::cout << "features=" << features << '\n'
                  << "output=" << output << '\n';
        return 0;
    }
    if (command == "preprocess-osm") {
        if (const auto profile = options.find("profile");
            profile != options.end() && profile->second != "car") {
            throw std::invalid_argument(
                "unsupported OSM preprocessing profile: " + profile->second);
        }
        zeus::map::OsmPreprocessOptions preprocess_options;
        preprocess_options.include_service = optionBool(
            options, "include-service", preprocess_options.include_service);
        preprocess_options.include_track = optionBool(
            options, "include-track", preprocess_options.include_track);
        preprocess_options.include_private = optionBool(
            options, "include-private", preprocess_options.include_private);
        if (const auto min_length = options.find("min-length"); min_length != options.end()) {
            preprocess_options.min_length_m = std::stod(min_length->second);
        }
        const std::string output = required(options, "output");
        const std::string report_path = required(options, "report");
        const zeus::map::OsmPreprocessReport report =
            zeus::map::OsmRoadPreprocessor().process(input, output, preprocess_options);
        zeus::map::OsmRoadPreprocessor::saveReport(
            report, preprocess_options, input, output, report_path);
        std::cout << "input_features=" << report.input_features << '\n'
                  << "output_features=" << report.output_features << '\n'
                  << "filtered_features=" << report.filtered_features << '\n'
                  << "default_speed_applied=" << report.default_speed_applied << '\n'
                  << "implied_oneway_applied=" << report.implied_oneway_applied << '\n'
                  << "output=" << output << '\n'
                  << "report=" << report_path << '\n';
        return 0;
    }
    if (command == "import") {
        const std::string output = required(options, "output");
        const zeus::map::ImportedRoads imported =
            zeus::map::ShapefileImporter().importFile(input, importOptions(options));
        zeus::map::BuildResult build = zeus::map::MapBuilder().build(imported);
        const zeus::map::ValidationReport report =
            zeus::map::MapValidator().validate(build.map, std::move(build.issues));
        printReport(report, true);
        if (report.hasFatalErrors()) {
            std::cerr << "map was not written because validation found fatal errors\n";
            return 2;
        }
        zeus::map::MapSerializer::save(build.map, output);
        std::cout << "turn_transitions=" << build.map.turn_transitions.size() << '\n';
        if (const auto issues_output = options.find("issues-output");
            issues_output != options.end()) {
            zeus::map::GeoJsonExporter::saveIssues(
                build.map, report.issues, issues_output->second);
            std::cout << "issues_output=" << issues_output->second << '\n';
        }
        std::cout << "output=" << output << '\n';
        return 0;
    }

    zeus::map::MapData map = zeus::map::MapSerializer::load(input);
    if (command == "validate") {
        const zeus::map::ValidationReport report = zeus::map::MapValidator().validate(map);
        printReport(report, true);
        std::cout << "turn_transitions=" << map.turn_transitions.size() << '\n';
        return report.hasFatalErrors() ? 2 : 0;
    }
    if (command == "geojson") {
        const std::string output = required(options, "output");
        zeus::map::GeoJsonExporter::save(map, output);
        std::cout << "output=" << output << '\n';
        return 0;
    }
    if (command == "nodes-geojson") {
        const std::string output = required(options, "output");
        zeus::map::GeoJsonExporter::saveNodes(map, output);
        std::cout << "output=" << output << '\n';
        return 0;
    }
    if (command == "issues-geojson") {
        const std::string output = required(options, "output");
        const zeus::map::ValidationReport report = zeus::map::MapValidator().validate(map);
        zeus::map::GeoJsonExporter::saveIssues(map, report.issues, output);
        std::cout << "output=" << output << '\n';
        return 0;
    }

    zeus::map::MapRuntime runtime(std::move(map));
    if (command == "route-worker") {
        return runRouteWorker(runtime);
    }
    if (command == "query") {
        zeus::map::Point2d point;
        if (options.contains("x") && options.contains("y")) {
            point = {std::stod(options.at("x")), std::stod(options.at("y"))};
        } else if (options.contains("lon") && options.contains("lat")) {
            point = wgs84ToRuntime(
                std::stod(options.at("lon")),
                std::stod(options.at("lat")),
                runtime.data().metadata.runtime_crs_wkt);
        } else {
            throw std::invalid_argument("query requires --x/--y or --lon/--lat");
        }

        zeus::map::MapMatchOptions match_options;
        if (const auto found = options.find("heading"); found != options.end()) {
            match_options.has_heading = true;
            match_options.heading_rad = std::stod(found->second) * std::acos(-1.0) / 180.0;
        }
        if (const auto found = options.find("max-distance"); found != options.end()) {
            match_options.max_distance_m = std::stod(found->second);
        }
        if (const auto found = options.find("limit"); found != options.end()) {
            match_options.max_results = static_cast<std::size_t>(std::stoul(found->second));
        }

        const auto candidates = runtime.matchPoint(point, match_options);
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "query_runtime_xy=" << point.x << ',' << point.y << '\n';
        std::cout << "matches=" << candidates.size() << '\n';
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            const auto& edge = runtime.edge(candidate.edge);
            std::cout << "match[" << i << "].edge=" << candidate.edge
                      << " road_id=" << edge.road_id
                      << " source=" << edge.source_id
                      << " offset_s=" << candidate.offset_s
                      << " distance=" << candidate.lateral_distance_m
                      << " heading_delta=" << candidate.heading_difference_rad
                      << " confidence=" << candidate.confidence
                      << " projected=" << candidate.projected_point.x << ','
                      << candidate.projected_point.y << '\n';
        }
        return candidates.empty() ? 3 : 0;
    }

    if (command == "pose") {
        zeus::map::VehicleMapPosition position;
        position.edge = static_cast<zeus::map::EdgeIndex>(std::stoul(required(options, "edge")));
        position.offset_s = std::stod(required(options, "offset"));
        if (const auto found = options.find("lateral"); found != options.end()) {
            position.lateral_offset_m = static_cast<float>(std::stod(found->second));
        }
        const zeus::map::WorldPose pose = runtime.worldPose(position);
        std::cout << std::fixed << std::setprecision(3)
                  << "x=" << pose.point.x << '\n'
                  << "y=" << pose.point.y << '\n'
                  << "heading_rad=" << pose.heading_rad << '\n';
        return 0;
    }

    if (command == "route") {
        const zeus::routing::RoutePlanner planner(runtime);
        return executeRoute(runtime, planner, options, std::cout);
    }

    if (command == "simulate") {
        // Demand source: a single OD pair expanded into --count departures
        // spread linearly over --spread seconds, or an od file with one
        // "lon,lat,dest_lon,dest_lat,depart_s[,algorithm]" row per vehicle.
        struct Wgs84Demand {
            double origin_lon = 0.0;
            double origin_lat = 0.0;
            double dest_lon = 0.0;
            double dest_lat = 0.0;
            double depart_s = 0.0;
            std::string algorithm;
        };
        std::vector<Wgs84Demand> wgs84_demands;
        if (const auto found = options.find("od-file"); found != options.end()) {
            std::ifstream input(found->second);
            if (!input) {
                throw std::runtime_error("cannot open od file: " + found->second);
            }
            std::string line;
            std::size_t line_number = 0;
            while (std::getline(input, line)) {
                ++line_number;
                line = trim(line);
                if (line.empty() || line.front() == '#') {
                    continue;
                }
                std::vector<std::string> fields;
                std::size_t start = 0;
                while (true) {
                    const std::size_t comma = line.find(',', start);
                    if (comma == std::string::npos) {
                        fields.push_back(line.substr(start));
                        break;
                    }
                    fields.push_back(line.substr(start, comma - start));
                    start = comma + 1;
                }
                if (fields.size() < 5 || fields.size() > 6) {
                    throw std::runtime_error("invalid od file line " +
                                             std::to_string(line_number) +
                                             ": expected lon,lat,dest_lon,dest_lat,depart_s[,algorithm]");
                }
                Wgs84Demand demand;
                demand.origin_lon = std::stod(trim(fields[0]));
                demand.origin_lat = std::stod(trim(fields[1]));
                demand.dest_lon = std::stod(trim(fields[2]));
                demand.dest_lat = std::stod(trim(fields[3]));
                demand.depart_s = std::stod(trim(fields[4]));
                if (fields.size() == 6) {
                    demand.algorithm = trim(fields[5]);
                }
                wgs84_demands.push_back(demand);
            }
        } else if (options.contains("lon") && options.contains("lat") &&
                   options.contains("dest-lon") && options.contains("dest-lat")) {
            const Wgs84Demand base{
                std::stod(options.at("lon")),
                std::stod(options.at("lat")),
                std::stod(options.at("dest-lon")),
                std::stod(options.at("dest-lat")),
                0.0,
                "",
            };
            const int count = options.contains("count")
                                  ? std::stoi(options.at("count"))
                                  : 1;
            if (count < 1) {
                throw std::invalid_argument("--count must be at least 1");
            }
            const double spread = options.contains("spread")
                                      ? std::stod(options.at("spread"))
                                      : 0.0;
            if (!std::isfinite(spread) || spread < 0.0) {
                throw std::invalid_argument("--spread must be a finite non-negative value");
            }
            for (int i = 0; i < count; ++i) {
                Wgs84Demand demand = base;
                demand.depart_s = spread > 0.0 && count > 1
                                      ? spread * static_cast<double>(i) /
                                            static_cast<double>(count - 1)
                                      : 0.0;
                wgs84_demands.push_back(demand);
            }
        } else {
            throw std::invalid_argument(
                "simulate requires --lon/--lat/--dest-lon/--dest-lat (with optional"
                " --count/--spread) or --od-file FILE");
        }

        zeus::routing::Algorithm algorithm = zeus::routing::Algorithm::kDijkstra;
        if (const auto found = options.find("algorithm"); found != options.end()) {
            if (!zeus::routing::parseAlgorithm(found->second, algorithm)) {
                throw std::invalid_argument("unknown routing algorithm: " + found->second);
            }
        }

        zeus::simulation::SimulationConfig config;
        if (const auto found = options.find("duration"); found != options.end()) {
            config.duration_seconds = std::stod(found->second);
        }
        if (const auto found = options.find("step"); found != options.end()) {
            config.step_seconds = std::stod(found->second);
        }
        if (const auto found = options.find("sample-interval"); found != options.end()) {
            config.sample_interval_seconds = std::stod(found->second);
        }
        std::vector<zeus::simulation::SimulationControlEvent> controls;
        if (const auto found = options.find("controls"); found != options.end()) {
            controls = parseSimulationControls(found->second);
        }

        // Convert every demand point in one batch through a shared transform.
        std::vector<double> xs;
        std::vector<double> ys;
        xs.reserve(wgs84_demands.size() * 2);
        ys.reserve(wgs84_demands.size() * 2);
        for (const Wgs84Demand& demand : wgs84_demands) {
            xs.push_back(demand.origin_lon);
            ys.push_back(demand.origin_lat);
            xs.push_back(demand.dest_lon);
            ys.push_back(demand.dest_lat);
        }
        transformWgs84Batch(xs, ys, runtime.data().metadata.runtime_crs_wkt);

        std::vector<zeus::simulation::VehicleDemand> demands;
        demands.reserve(wgs84_demands.size());
        for (std::size_t i = 0; i < wgs84_demands.size(); ++i) {
            zeus::simulation::VehicleDemand demand;
            demand.origin = {xs[2 * i], ys[2 * i]};
            demand.destination = {xs[2 * i + 1], ys[2 * i + 1]};
            demand.depart_time_s = wgs84_demands[i].depart_s;
            demand.algorithm = algorithm;
            if (!wgs84_demands[i].algorithm.empty() &&
                !zeus::routing::parseAlgorithm(
                    wgs84_demands[i].algorithm, demand.algorithm)) {
                throw std::invalid_argument(
                    "unknown routing algorithm: " + wgs84_demands[i].algorithm);
            }
            demands.push_back(demand);
        }

        const zeus::routing::RoutePlanner planner(runtime);
        const zeus::simulation::SimulationEngine engine(runtime, planner);
        const zeus::simulation::SimulationResult result =
            engine.run(config, demands, controls);

        std::cout << std::fixed << std::setprecision(3);
        if (!result.ok) {
            std::cout << "simulate=failed\n"
                      << "reason=unroutable\n"
                      << "message=" << result.message << '\n';
            return 3;
        }
        std::cout << "simulate=ok\n"
                  << "vehicles=" << result.stats.vehicles_total << '\n'
                  << "arrived=" << result.stats.arrived << '\n'
                  << "unroutable=" << result.stats.unroutable << '\n'
                  << "waiting_at_end=" << result.stats.waiting_at_end << '\n'
                  << "driving_at_end=" << result.stats.driving_at_end << '\n'
                  << "ticks=" << result.stats.ticks_executed << '\n'
                  << "route_plans=" << result.stats.route_plans << '\n'
                  << "control_events=" << result.stats.control_events_applied << '\n'
                  << "vehicle_controls=" << result.stats.vehicle_control_events << '\n'
                  << "edge_controls=" << result.stats.edge_control_events << '\n'
                  << "junction_controls=" << result.stats.junction_control_events << '\n'
                  << "avg_travel_s=" << result.stats.average_travel_s << '\n'
                  << "min_travel_s=" << result.stats.min_travel_s << '\n'
                  << "max_travel_s=" << result.stats.max_travel_s << '\n'
                  << "total_distance_m=" << result.stats.total_distance_m << '\n'
                  << "samples=" << result.stats.sample_count << '\n'
                  << "deadlock=" << (result.stats.deadlock ? 1 : 0) << '\n'
                  << "compute_ms=" << result.stats.compute_ms << '\n';
        if (const auto found_output = options.find("output");
            found_output != options.end()) {
            const std::size_t features = zeus::simulation::TrajectoryExporter::save(
                runtime, result, found_output->second);
            std::cout << "features=" << features << '\n'
                      << "output=" << found_output->second << '\n';
        }
        if (const auto found_playback = options.find("playback");
            found_playback != options.end()) {
            zeus::simulation::PlaybackExporter::save(
                runtime, result, found_playback->second);
            std::cout << "playback=" << found_playback->second << '\n';
        }
        return 0;
    }

    throw std::invalid_argument("unknown command: " + command);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
