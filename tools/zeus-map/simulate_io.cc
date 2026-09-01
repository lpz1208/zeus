#include "simulate_io.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>

#include <cpl_conv.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

namespace zeus::cli {
namespace {

std::vector<std::string> splitComma(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t comma = line.find(',', begin);
        if (comma == std::string::npos) {
            fields.push_back(line.substr(begin));
            return fields;
        }
        fields.push_back(line.substr(begin, comma - begin));
        begin = comma + 1;
    }
}

}  // namespace

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
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
        const std::vector<std::string> fields = splitComma(line);
        if (fields.size() < 4 || fields.size() > 5) {
            throw std::runtime_error(
                "invalid controls line " + std::to_string(line_number) +
                ": expected time_s,vehicle|edge|junction,target_id,action[,value]");
        }
        zeus::simulation::SimulationControlEvent control;
        control.time_s = std::stod(trim(fields[0]));
        const unsigned long long target_id = std::stoull(trim(fields[2]));
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
            control.value = std::stod(trim(fields[4]));
        }
        controls.push_back(control);
    }
    return controls;
}

std::vector<zeus::simulation::JunctionSignalPlan> parseSignalPlans(
    const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open signal plans file: " + path);
    }
    std::vector<zeus::simulation::JunctionSignalPlan> plans;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::vector<std::string> fields = splitComma(line);
        if (fields.size() < 8 || fields.size() > 9) {
            throw std::runtime_error(
                "invalid signal line " + std::to_string(line_number) +
                ": expected node_id,phase,green_s,yellow_s,all_red_s,offset_s,from_edge,to_edge[,saturation_flow_vph]");
        }
        const auto checkedId = [&](const std::string& value, const char* name) {
            const unsigned long long parsed = std::stoull(trim(value));
            if (parsed > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error(
                    std::string("signal ") + name + " exceeds uint32 range on line " +
                    std::to_string(line_number));
            }
            return static_cast<std::uint32_t>(parsed);
        };
        const auto node = static_cast<zeus::map::NodeIndex>(checkedId(fields[0], "node ID"));
        const std::uint32_t phase_index = checkedId(fields[1], "phase index");
        if (phase_index >= 1024) {
            throw std::runtime_error("signal phase index is too large on line " +
                                     std::to_string(line_number));
        }
        const double green = std::stod(trim(fields[2]));
        const double yellow = std::stod(trim(fields[3]));
        const double all_red = std::stod(trim(fields[4]));
        const double offset = std::stod(trim(fields[5]));
        const auto from_edge = static_cast<zeus::map::EdgeIndex>(
            checkedId(fields[6], "from edge"));
        const auto to_edge = static_cast<zeus::map::EdgeIndex>(
            checkedId(fields[7], "to edge"));
        const double saturation_flow_vph =
            fields.size() == 9 ? std::stod(trim(fields[8])) : 1800.0;

        auto found = std::find_if(
            plans.begin(), plans.end(),
            [node](const zeus::simulation::JunctionSignalPlan& plan) {
                return plan.node == node;
            });
        if (found == plans.end()) {
            plans.push_back({node, offset, yellow, all_red, {}});
            found = std::prev(plans.end());
        } else if (std::abs(found->offset_seconds - offset) > 1e-9 ||
                   std::abs(found->yellow_seconds - yellow) > 1e-9 ||
                   std::abs(found->all_red_seconds - all_red) > 1e-9) {
            throw std::runtime_error(
                "inconsistent signal plan timing on line " +
                std::to_string(line_number));
        }
        if (found->phases.size() <= phase_index) {
            found->phases.resize(static_cast<std::size_t>(phase_index) + 1);
        }
        zeus::simulation::SignalPhase& phase = found->phases[phase_index];
        if (phase.movements.empty()) {
            phase.green_seconds = green;
            phase.saturation_flow_vph = saturation_flow_vph;
        } else if (std::abs(phase.green_seconds - green) > 1e-9) {
            throw std::runtime_error(
                "inconsistent signal phase green time on line " +
                std::to_string(line_number));
        } else if (std::abs(
                       phase.saturation_flow_vph - saturation_flow_vph) > 1e-9) {
            throw std::runtime_error(
                "inconsistent signal phase saturation flow on line " +
                std::to_string(line_number));
        }
        phase.movements.push_back({from_edge, to_edge});
    }
    return plans;
}

std::vector<OdRow> parseOdFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open od file: " + path);
    }
    std::vector<OdRow> rows;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::vector<std::string> fields = splitComma(line);
        if (fields.size() < 5 || fields.size() > 7) {
            throw std::runtime_error(
                "invalid od file line " + std::to_string(line_number) +
                ": expected lon,lat,dest_lon,dest_lat,depart_s[,algorithm][,agent]");
        }
        OdRow row;
        row.origin_lon = std::stod(trim(fields[0]));
        row.origin_lat = std::stod(trim(fields[1]));
        row.dest_lon = std::stod(trim(fields[2]));
        row.dest_lat = std::stod(trim(fields[3]));
        row.depart_s = std::stod(trim(fields[4]));
        if (fields.size() >= 6) {
            row.algorithm = trim(fields[5]);
        }
        if (fields.size() == 7) {
            const std::string agent = trim(fields[6]);
            row.agent_controlled =
                agent == "1" || agent == "true" || agent == "yes" || agent == "agent";
        }
        rows.push_back(row);
    }
    return rows;
}

std::vector<zeus::simulation::VehicleDemand> buildVehicleDemands(
    const std::vector<OdRow>& rows,
    zeus::routing::Algorithm default_algorithm,
    const std::string& runtime_crs_wkt) {
    std::vector<double> xs;
    std::vector<double> ys;
    xs.reserve(rows.size() * 2);
    ys.reserve(rows.size() * 2);
    for (const OdRow& row : rows) {
        xs.push_back(row.origin_lon);
        ys.push_back(row.origin_lat);
        xs.push_back(row.dest_lon);
        ys.push_back(row.dest_lat);
    }
    transformWgs84Batch(xs, ys, runtime_crs_wkt);

    std::vector<zeus::simulation::VehicleDemand> demands;
    demands.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        zeus::simulation::VehicleDemand demand;
        demand.origin = {xs[2 * i], ys[2 * i]};
        demand.destination = {xs[2 * i + 1], ys[2 * i + 1]};
        demand.depart_time_s = rows[i].depart_s;
        demand.algorithm = default_algorithm;
        if (!rows[i].algorithm.empty() &&
            !zeus::routing::parseAlgorithm(rows[i].algorithm, demand.algorithm)) {
            throw std::invalid_argument("unknown routing algorithm: " + rows[i].algorithm);
        }
        demand.agent_controlled = rows[i].agent_controlled;
        demands.push_back(demand);
    }
    return demands;
}

}  // namespace zeus::cli
