#include "session_worker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "zeus/routing/route_planner.h"
#include "zeus/simulation/playback_exporter.h"
#include "zeus/simulation/simulation_engine.h"
#include "zeus/simulation/simulation_session.h"

#include "simulate_io.h"

namespace zeus::cli {
namespace {

// ---------------------------------------------------------------------------
// Minimal JSON writing helpers (style of playback_exporter.cc): no NaN, no
// external dependency, three-decimal floats.
// ---------------------------------------------------------------------------

std::string jsonString(std::string_view value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    out << '"';
    return out.str();
}

std::string jsonNumber(double value) {
    if (!std::isfinite(value)) {
        return "0";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

const char* jsonVehicleState(zeus::simulation::VehicleState state) {
    switch (state) {
        case zeus::simulation::VehicleState::kWaiting: return "waiting";
        case zeus::simulation::VehicleState::kDriving: return "driving";
        case zeus::simulation::VehicleState::kArrived: return "arrived";
        case zeus::simulation::VehicleState::kUnroutable: return "unroutable";
    }
    return "unknown";
}

bool validSessionId(const std::string& id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    for (const unsigned char ch : id) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

struct Candidate {
    std::uint32_t vehicle_id = 0;
    zeus::routing::Algorithm algorithm = zeus::routing::Algorithm::kDijkstra;
    std::uint64_t based_on_state_version = 0;
    double time_s = 0.0;
    double length_m = 0.0;
    std::vector<zeus::map::EdgeIndex> edges;
};

enum class AppliedActionKind : std::uint8_t {
    kCommit = 0,
    kKeep,
};

struct AppliedAction {
    std::uint64_t tick = 0;
    std::uint32_t vehicle_id = 0;
    AppliedActionKind kind = AppliedActionKind::kKeep;
    zeus::routing::Algorithm algorithm = zeus::routing::Algorithm::kDijkstra;
};

struct WorkerSession {
    std::unique_ptr<zeus::simulation::SimulationSession> session;
    std::map<std::string, Candidate> candidates;
    std::uint64_t next_candidate = 1;
    zeus::simulation::SimulationConfig config;
    std::vector<zeus::simulation::VehicleDemand> demands;
    std::vector<zeus::simulation::SimulationControlEvent> controls;
    std::vector<zeus::simulation::JunctionSignalPlan> signals;
    std::vector<AppliedAction> applied_actions;
};

struct WorkerSnapshot {
    zeus::simulation::SimulationConfig config;
    std::vector<zeus::simulation::VehicleDemand> demands;
    std::vector<zeus::simulation::SimulationControlEvent> controls;
    std::vector<zeus::simulation::JunctionSignalPlan> signals;
    std::vector<AppliedAction> applied_actions;
    std::uint64_t tick = 0;
};

class SessionWorker {
public:
    explicit SessionWorker(const zeus::map::MapRuntime& runtime)
        : runtime_(runtime), planner_(runtime), engine_(runtime, planner_) {}

    int run() {
        std::cout << "ZEUS_SESSION_WORKER\t1\n" << std::flush;
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "shutdown") {
                return 0;
            }
            std::ostringstream payload;
            int exit_code = 1;
            // Only strip a trailing CR: tab-separated commands may carry
            // deliberately empty trailing fields.
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            try {
                exit_code = dispatch(line, payload);
            } catch (const std::exception& error) {
                payload.str("");
                payload << "{\"error\": " << jsonString(error.what()) << "}";
                exit_code = 1;
            }
            const std::string text = payload.str();
            std::cout << "ZEUS_SESSION_RESPONSE\t" << exit_code << '\t'
                      << text.size() << '\n';
            std::cout.write(text.data(), static_cast<std::streamsize>(text.size()));
            std::cout << '\n';
            std::cout << std::flush;
        }
        return 0;
    }

private:
    static void writeAlgorithmCapabilities(std::ostringstream& out) {
        out << "[";
        bool first = true;
        for (const zeus::routing::AlgorithmCapability& capability :
             zeus::routing::algorithmCapabilities()) {
            if (!first) {
                out << ", ";
            }
            first = false;
            out << "{\"algorithmId\": "
                << jsonString(zeus::routing::algorithmName(capability.algorithm))
                << ", \"algorithmVersion\": " << jsonString(capability.version)
                << ", \"supportedObjectives\": [\"travel_time\"]"
                << ", \"searchDirection\": "
                << jsonString(capability.search_direction)
                << ", \"supportsDynamicWeights\": "
                << (capability.supports_dynamic_weights ? "true" : "false")
                << ", \"supportsIncrementalRepair\": "
                << (capability.supports_incremental_repair ? "true" : "false")
                << ", \"supportsKCandidates\": "
                << (capability.supports_k_candidates ? "true" : "false")
                << ", \"supportsTimeDependency\": "
                << (capability.supports_time_dependency ? "true" : "false")
                << ", \"deterministic\": "
                << (capability.deterministic ? "true" : "false")
                << ", \"exact\": " << (capability.exact ? "true" : "false")
                << ", \"usesHeuristic\": "
                << (capability.uses_heuristic ? "true" : "false") << "}";
        }
        out << "]";
    }

    [[nodiscard]] WorkerSession& requireSession(const std::string& id) {
        if (!validSessionId(id)) {
            throw std::invalid_argument("invalid session id");
        }
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) {
            throw std::invalid_argument("unknown session: " + id);
        }
        return *found->second;
    }

    [[nodiscard]] static std::uint32_t parseUint(const std::string& raw) {
        const unsigned long long value = std::stoull(raw);
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("value exceeds uint32 range: " + raw);
        }
        return static_cast<std::uint32_t>(value);
    }

    [[nodiscard]] static std::uint64_t parseUint64(const std::string& raw) {
        return std::stoull(raw);
    }

    int dispatch(const std::string& line, std::ostringstream& payload) {
        const std::vector<std::string> fields = splitTabs(line);
        const std::string& command = fields.at(0);
        if (command == "tools") {
            if (fields.size() != 1) {
                throw std::invalid_argument("tools requires 1 tab field");
            }
            payload << "{\"registryVersion\": \"routing-tools-v1\", \"algorithms\": ";
            writeAlgorithmCapabilities(payload);
            payload << "}";
            return 0;
        }
        if (command == "reset") {
            return commandReset(fields, payload);
        }
        if (command == "observe") {
            return commandObserve(fields, payload);
        }
        if (command == "agent-observe") {
            return commandAgentObserve(fields, payload);
        }
        if (command == "plan") {
            return commandPlan(fields, payload);
        }
        if (command == "commit") {
            return commandCommit(fields, payload);
        }
        if (command == "keep") {
            return commandKeep(fields, payload);
        }
        if (command == "step") {
            return commandStep(fields, payload);
        }
        if (command == "step_event") {
            return commandStepEvent(fields, payload);
        }
        if (command == "run-to-end") {
            return commandRunToEnd(fields, payload);
        }
        if (command == "resume") {
            return commandResume(fields, payload);
        }
        if (command == "pause") {
            return commandPause(fields, payload);
        }
        if (command == "snapshot") {
            return commandSnapshot(fields, payload);
        }
        if (command == "restore") {
            return commandRestore(fields, payload);
        }
        if (command == "drop-snapshot") {
            return commandDropSnapshot(fields, payload);
        }
        if (command == "result") {
            return commandResult(fields, payload);
        }
        if (command == "close") {
            return commandClose(fields, payload);
        }
        throw std::invalid_argument("unknown session worker command: " + command);
    }

    int commandReset(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 13) {
            throw std::invalid_argument("reset requires 13 tab fields");
        }
        const std::string& session_id = fields[1];
        if (!validSessionId(session_id)) {
            throw std::invalid_argument("invalid session id");
        }
        zeus::simulation::SimulationConfig config;
        config.duration_seconds = std::stod(fields[2]);
        config.step_seconds = std::stod(fields[3]);
        config.sample_interval_seconds = std::stod(fields[4]);
        config.exit_headway_ff_s = std::stod(fields[5]);
        config.exit_headway_jam_s = std::stod(fields[6]);
        config.reroute_interval_seconds = std::stod(fields[7]);
        config.reroute_cost_ratio = std::stod(fields[8]);
        config.min_speed_ratio = std::stod(fields[9]);
        std::vector<zeus::simulation::VehicleDemand> demands;
        if (!fields[10].empty()) {
            demands = buildVehicleDemands(
                parseOdFile(fields[10]), zeus::routing::Algorithm::kDijkstra,
                runtime_.data().metadata.runtime_crs_wkt);
        }
        std::vector<zeus::simulation::SimulationControlEvent> controls;
        if (!fields[11].empty()) {
            controls = parseSimulationControls(fields[11]);
        }
        std::vector<zeus::simulation::JunctionSignalPlan> signals;
        if (!fields[12].empty()) {
            signals = parseSignalPlans(fields[12]);
        }

        auto entry = std::make_unique<WorkerSession>();
        entry->session = std::make_unique<zeus::simulation::SimulationSession>(
            engine_, config, demands, controls, signals);
        const zeus::simulation::SimulationSessionState state = entry->session->reset();
        entry->config = config;
        entry->demands = demands;
        entry->controls = controls;
        entry->signals = signals;

        out << "{\"sessionId\": " << jsonString(session_id)
            << ", \"tick\": " << state.tick
            << ", \"simulationTimeS\": " << jsonNumber(state.simulation_time_s)
            << ", \"stateVersion\": " << state.state_version
            << ", \"ready\": " << (state.ready ? "true" : "false")
            << ", \"paused\": " << (state.paused ? "true" : "false")
            << ", \"finished\": " << (state.finished ? "true" : "false")
            << ", \"vehicles\": " << demands.size() << ", \"agents\": [";
        bool first = true;
        for (std::size_t i = 0; i < demands.size(); ++i) {
            if (demands[i].agent_controlled) {
                if (!first) {
                    out << ", ";
                }
                first = false;
                out << static_cast<std::uint32_t>(i);
            }
        }
        out << "]}";
        sessions_[session_id] = std::move(entry);
        return 0;
    }

    static void writeStateHeader(
        std::ostringstream& out,
        const zeus::simulation::SimulationSessionState& state,
        const zeus::simulation::TickSnapshot& snapshot) {
        out << "\"tick\": " << state.tick
            << ", \"simulationTimeS\": " << jsonNumber(state.simulation_time_s)
            << ", \"stateVersion\": " << state.state_version
            << ", \"finished\": " << (state.finished ? "true" : "false")
            << ", \"cancelled\": " << (state.cancelled ? "true" : "false")
            << ", \"decisionDue\": " << (snapshot.decision_due ? "true" : "false")
            << ", \"decisionReason\": " << jsonString(snapshot.decision_reason)
            << ", \"agentVehicleIds\": [";
        for (std::size_t i = 0; i < snapshot.agents.size(); ++i) {
            if (i > 0) {
                out << ", ";
            }
            out << snapshot.agents[i].vehicle_id;
        }
        out << "]";
    }

    static void writeSnapshot(
        std::ostringstream& out,
        const zeus::simulation::TickSnapshot& snapshot,
        const std::vector<zeus::map::EdgeIndex>* edge_filter) {
        out << ", \"counts\": {\"arrived\": " << snapshot.arrived
            << ", \"driving\": " << snapshot.driving
            << ", \"waiting\": " << snapshot.waiting
            << ", \"unroutable\": " << snapshot.unroutable << "}";
        out << ", \"edges\": [";
        bool first = true;
        for (const zeus::simulation::EdgeTickState& edge : snapshot.edges) {
            if (edge_filter != nullptr &&
                std::find(edge_filter->begin(), edge_filter->end(), edge.edge) ==
                    edge_filter->end()) {
                continue;
            }
            if (!first) {
                out << ", ";
            }
            first = false;
            out << "{\"edgeId\": " << edge.edge
                << ", \"occupancy\": " << edge.occupancy
                << ", \"capacity\": " << edge.effective_capacity
                << ", \"closed\": " << (edge.closed ? "true" : "false")
                << ", \"speedFactor\": " << jsonNumber(edge.speed_factor)
                << ", \"costFactor\": " << jsonNumber(edge.routing_cost_factor)
                << ", \"meanSpeedMps\": " << jsonNumber(edge.mean_speed_mps)
                << "}";
        }
        out << "], \"agents\": [";
        first = true;
        for (const zeus::simulation::AgentVehicleState& agent : snapshot.agents) {
            if (!first) {
                out << ", ";
            }
            first = false;
            out << "{\"vehicleId\": " << agent.vehicle_id
                << ", \"state\": " << jsonString(jsonVehicleState(agent.state))
                << ", \"edgeId\": " << agent.edge
                << ", \"offsetM\": " << jsonNumber(agent.offset_s)
                << ", \"routeId\": " << agent.route_id
                << ", \"destinationEdgeId\": " << agent.destination_edge
                << ", \"remainingEtaS\": " << jsonNumber(agent.remaining_eta_s)
                << ", \"routeInvalidated\": "
                << (agent.route_invalidated ? "true" : "false")
                << ", \"held\": " << (agent.held ? "true" : "false")
                << ", \"remainingEdgeIds\": [";
            for (std::size_t i = 0; i < agent.remaining_edges.size(); ++i) {
                if (i > 0) {
                    out << ", ";
                }
                out << agent.remaining_edges[i];
            }
            out << "]}";
        }
        out << "]";
    }

    int commandObserve(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() < 2 || fields.size() > 3) {
            throw std::invalid_argument("observe requires 2 or 3 tab fields");
        }
        WorkerSession& entry = requireSession(fields[1]);
        std::vector<zeus::map::EdgeIndex> filter;
        const std::vector<zeus::map::EdgeIndex>* filter_ptr = nullptr;
        if (fields.size() == 3 && !fields[2].empty() && fields[2] != "hot" &&
            fields[2] != "all") {
            for (const std::string& raw : splitCommaList(fields[2])) {
                filter.push_back(parseUint(raw));
            }
            filter_ptr = &filter;
        }
        const zeus::simulation::SimulationSessionState state = entry.session->observe();
        const zeus::simulation::TickSnapshot snapshot = entry.session->snapshot();
        out << "{";
        writeStateHeader(out, state, snapshot);
        writeSnapshot(out, snapshot, filter_ptr);
        out << "}";
        return 0;
    }

    [[nodiscard]] static std::vector<std::string> splitCommaList(const std::string& raw) {
        std::vector<std::string> items;
        std::size_t begin = 0;
        while (true) {
            const std::size_t comma = raw.find(',', begin);
            if (comma == std::string::npos) {
                const std::string item = trim(raw.substr(begin));
                if (!item.empty()) {
                    items.push_back(item);
                }
                return items;
            }
            const std::string item = trim(raw.substr(begin, comma - begin));
            if (!item.empty()) {
                items.push_back(item);
            }
            begin = comma + 1;
        }
    }

    [[nodiscard]] const zeus::simulation::AgentVehicleState* findAgent(
        const zeus::simulation::TickSnapshot& snapshot,
        std::uint32_t vehicle_id) const {
        for (const zeus::simulation::AgentVehicleState& agent : snapshot.agents) {
            if (agent.vehicle_id == vehicle_id) {
                return &agent;
            }
        }
        return nullptr;
    }

    int commandAgentObserve(
        const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 3) {
            throw std::invalid_argument("agent-observe requires 3 tab fields");
        }
        WorkerSession& entry = requireSession(fields[1]);
        const std::uint32_t vehicle_id = parseUint(fields[2]);
        const zeus::simulation::SimulationSessionState state = entry.session->observe();
        const zeus::simulation::TickSnapshot snapshot = entry.session->snapshot();
        const zeus::simulation::AgentVehicleState* agent =
            findAgent(snapshot, vehicle_id);
        if (agent == nullptr) {
            throw std::invalid_argument("vehicle is not an agent: " + fields[2]);
        }
        out << "{";
        writeStateHeader(out, state, snapshot);
        out << ", \"vehicleId\": " << agent->vehicle_id
            << ", \"state\": " << jsonString(jsonVehicleState(agent->state))
            << ", \"position\": {\"edgeId\": " << agent->edge
            << ", \"offsetM\": " << jsonNumber(agent->offset_s) << "}"
            << ", \"destinationEdgeId\": " << agent->destination_edge
            << ", \"remainingEtaS\": " << jsonNumber(agent->remaining_eta_s)
            << ", \"routeInvalidated\": "
            << (agent->route_invalidated ? "true" : "false")
            << ", \"remainingEdgeIds\": [";
        for (std::size_t i = 0; i < agent->remaining_edges.size(); ++i) {
            if (i > 0) {
                out << ", ";
            }
            out << agent->remaining_edges[i];
        }
        out << "], \"nearbyRoads\": [";
        // Spatially rank hot edges around the live vehicle position. The
        // observation remains bounded and does not expose the whole network.
        std::vector<const zeus::simulation::EdgeTickState*> nearby_edges;
        if (agent->edge < runtime_.data().edges.size() && !snapshot.edges.empty()) {
            std::vector<std::uint8_t> hot_mask(runtime_.data().edges.size(), 0);
            for (const zeus::simulation::EdgeTickState& edge : snapshot.edges) {
                hot_mask[edge.edge] = 1;
            }
            zeus::map::VehicleMapPosition position;
            position.edge = agent->edge;
            position.offset_s = agent->offset_s;
            zeus::map::MapMatchOptions options;
            options.max_results = 64;
            options.max_distance_m = 2000.0;
            options.edge_enabled = std::span<const std::uint8_t>(hot_mask);
            const auto matches = runtime_.matchPoint(
                runtime_.worldPose(position).point, options);
            for (const zeus::map::MapMatchCandidate& match : matches) {
                const auto found = std::lower_bound(
                    snapshot.edges.begin(), snapshot.edges.end(), match.edge,
                    [](const zeus::simulation::EdgeTickState& edge,
                       zeus::map::EdgeIndex id) { return edge.edge < id; });
                if (found != snapshot.edges.end() && found->edge == match.edge) {
                    nearby_edges.push_back(&*found);
                }
            }
        }
        std::size_t written = 0;
        for (const zeus::simulation::EdgeTickState* edge_ptr : nearby_edges) {
            if (written >= 64) {
                break;
            }
            const zeus::simulation::EdgeTickState& edge = *edge_ptr;
            const zeus::map::DirectedEdge& road = runtime_.edge(edge.edge);
            const double occupancy_ratio =
                edge.effective_capacity > 0
                    ? static_cast<double>(edge.occupancy) /
                          static_cast<double>(edge.effective_capacity)
                    : 0.0;
            const double effective = std::max(
                0.5, static_cast<double>(road.speed_limit_mps) * (1.0 - occupancy_ratio));
            if (written > 0) {
                out << ", ";
            }
            ++written;
            out << "{\"edgeId\": " << edge.edge
                << ", \"speedMps\": "
                << jsonNumber(edge.mean_speed_mps > 0.0
                                  ? edge.mean_speed_mps
                                  : effective)
                << ", \"freeFlowSpeedMps\": "
                << jsonNumber(static_cast<double>(road.speed_limit_mps))
                << ", \"occupancyRatio\": " << jsonNumber(occupancy_ratio)
                << ", \"estimatedTravelTimeS\": " << jsonNumber(road.length_m / effective)
                << ", \"closed\": " << (edge.closed ? "true" : "false")
                << "}";
        }
        out << "], \"activeEvents\": [";
        written = 0;
        for (const zeus::simulation::EdgeTickState& edge : snapshot.edges) {
            if (written >= 64) {
                break;
            }
            const char* type = edge.closed      ? "closed"
                               : edge.routing_cost_factor > 1.0 + 1e-9 ? "congested"
                                                                        : nullptr;
            if (type == nullptr) {
                continue;
            }
            if (written > 0) {
                out << ", ";
            }
            ++written;
            out << "{\"eventId\": " << jsonString("edge-" + std::to_string(edge.edge))
                << ", \"type\": " << jsonString(type)
                << ", \"affectedEdgeIds\": [" << edge.edge << "]}";
        }
        out << "], \"availableAlgorithms\": ";
        writeAlgorithmCapabilities(out);
        out << "}";
        return 0;
    }

    int commandPlan(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 4) {
            throw std::invalid_argument("plan requires 4 tab fields");
        }
        WorkerSession& entry = requireSession(fields[1]);
        const std::uint32_t vehicle_id = parseUint(fields[2]);
        if (vehicle_id >= entry.demands.size()) {
            throw std::invalid_argument("plan vehicle id is out of range");
        }
        zeus::routing::Algorithm algorithm = zeus::routing::Algorithm::kDijkstra;
        if (!zeus::routing::parseAlgorithm(fields[3], algorithm)) {
            throw std::invalid_argument("unknown routing algorithm: " + fields[3]);
        }
        const zeus::simulation::SimulationSessionState state = entry.session->observe();
        const zeus::simulation::TickSnapshot snapshot = entry.session->snapshot();
        const zeus::simulation::AgentVehicleState* agent =
            findAgent(snapshot, vehicle_id);
        if (agent == nullptr) {
            throw std::invalid_argument("vehicle is not an agent: " + fields[2]);
        }

        // Overlay from the live snapshot: closed edges are unroutable, cost
        // factors carry the published congestion weights.
        std::vector<std::uint8_t> edge_enabled(runtime_.data().edges.size(), 1);
        std::vector<double> edge_cost(runtime_.data().edges.size(), 1.0);
        for (const zeus::simulation::EdgeTickState& edge : snapshot.edges) {
            if (edge.edge < edge_enabled.size()) {
                edge_enabled[edge.edge] = edge.closed ? 0 : 1;
                edge_cost[edge.edge] = edge.routing_cost_factor;
            }
        }
        const zeus::routing::RoutingOverlay overlay{edge_enabled, edge_cost};

        zeus::routing::RouteRequest request;
        request.origin = entry.demands[vehicle_id].origin;
        request.destination = entry.demands[vehicle_id].destination;
        request.algorithm = algorithm;
        request.overlay = &overlay;
        request.destination_position = zeus::routing::RoutePosition{
            agent->destination_edge, agent->route_end_offset_m};
        if (agent->state == zeus::simulation::VehicleState::kDriving) {
            request.origin_position =
                zeus::routing::RoutePosition{agent->edge, agent->offset_s};
        }
        const zeus::routing::RouteResult planned = planner_.plan(request);

        const std::string candidate_id = "cand-" + std::to_string(entry.next_candidate++);
        out << "{\"candidateId\": " << jsonString(candidate_id)
            << ", \"vehicleId\": " << vehicle_id
            << ", \"algorithm\": " << jsonString(fields[3]);
        if (!planned.ok) {
            out << ", \"ok\": false, \"reason\": "
                << jsonString(zeus::routing::routeFailureName(planned.failure))
                << ", \"message\": " << jsonString(planned.message) << "}";
            return 3;
        }
        Candidate candidate;
        candidate.vehicle_id = vehicle_id;
        candidate.algorithm = algorithm;
        candidate.based_on_state_version = state.state_version;
        candidate.time_s = planned.stats.time_s;
        candidate.length_m = planned.stats.length_m;
        candidate.edges = planned.path.edges;
        entry.candidates[candidate_id] = candidate;
        out << ", \"effectiveAlgorithm\": "
            << jsonString(zeus::routing::algorithmName(planned.effective_algorithm))
            << ", \"basedOnStateVersion\": " << state.state_version
            << ", \"ok\": true"
            << ", \"timeS\": " << jsonNumber(planned.stats.time_s)
            << ", \"lengthM\": " << jsonNumber(planned.stats.length_m)
            << ", \"expandedNodes\": " << planned.stats.expanded_nodes
            << ", \"edges\": [";
        for (std::size_t i = 0; i < planned.path.edges.size(); ++i) {
            if (i > 0) {
                out << ", ";
            }
            out << planned.path.edges[i];
        }
        out << "]}";
        return 0;
    }

    static void writeCommitResult(
        std::ostringstream& out,
        zeus::simulation::SimulationSession::CommitResult result) {
        const char* reason = "";
        bool accepted = true;
        switch (result) {
            case zeus::simulation::SimulationSession::CommitResult::kApplied:
                reason = "";
                break;
            case zeus::simulation::SimulationSession::CommitResult::kRejectedClosed:
                accepted = false;
                reason = "session_closed";
                break;
            case zeus::simulation::SimulationSession::CommitResult::kRejectedStaleVersion:
                accepted = false;
                reason = "stale_state_version";
                break;
            case zeus::simulation::SimulationSession::CommitResult::kRejectedUnknownVehicle:
                accepted = false;
                reason = "unknown_vehicle";
                break;
            case zeus::simulation::SimulationSession::CommitResult::kRejectedNotAgent:
                accepted = false;
                reason = "vehicle_not_agent_controlled";
                break;
        }
        out << "{\"accepted\": " << (accepted ? "true" : "false")
            << ", \"reason\": " << jsonString(reason)
            << ", \"appliesAtNextTick\": " << (accepted ? "true" : "false") << "}";
    }

    int commandCommit(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 5) {
            throw std::invalid_argument("commit requires 5 tab fields");
        }
        WorkerSession& entry = requireSession(fields[1]);
        const std::uint32_t vehicle_id = parseUint(fields[2]);
        const auto found = entry.candidates.find(fields[3]);
        if (found == entry.candidates.end()) {
            throw std::invalid_argument("unknown candidate: " + fields[3]);
        }
        if (found->second.vehicle_id != vehicle_id) {
            throw std::invalid_argument("candidate belongs to another vehicle");
        }
        const std::uint64_t expected_version = parseUint64(fields[4]);
        // Commit re-plans from the live position with the candidate's
        // algorithm; the stored path itself never crosses the boundary.
        const zeus::simulation::SimulationSessionState state = entry.session->observe();
        const auto result = entry.session->commitRoute(
            vehicle_id, found->second.algorithm, expected_version);
        if (result == zeus::simulation::SimulationSession::CommitResult::kApplied) {
            entry.applied_actions.push_back(
                {state.tick, vehicle_id, AppliedActionKind::kCommit,
                 found->second.algorithm});
        }
        writeCommitResult(out, result);
        return 0;
    }

    int commandKeep(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 4) {
            throw std::invalid_argument("keep requires 4 tab fields");
        }
        WorkerSession& entry = requireSession(fields[1]);
        const std::uint32_t vehicle_id = parseUint(fields[2]);
        const std::uint64_t expected_version = parseUint64(fields[3]);
        const zeus::simulation::SimulationSessionState state = entry.session->observe();
        const auto result = entry.session->keepRoute(vehicle_id, expected_version);
        if (result == zeus::simulation::SimulationSession::CommitResult::kApplied) {
            entry.applied_actions.push_back(
                {state.tick, vehicle_id, AppliedActionKind::kKeep,
                 zeus::routing::Algorithm::kDijkstra});
        }
        writeCommitResult(out, result);
        return 0;
    }

    int commandStep(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 3) {
            throw std::invalid_argument("step requires 3 tab fields");
        }
        WorkerSession& entry = requireSession(fields[1]);
        const std::uint64_t ticks = parseUint64(fields[2]);
        if (ticks == 0) {
            throw std::invalid_argument("step count must be positive");
        }
        const zeus::simulation::SimulationSessionState state = entry.session->step(ticks);
        out << "{";
        writeStateHeader(out, state, entry.session->snapshot());
        out << "}";
        return 0;
    }

    int commandStepEvent(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 3) {
            throw std::invalid_argument("step_event requires 3 tab fields");
        }
        WorkerSession& entry = requireSession(fields[1]);
        const std::uint64_t max_ticks = parseUint64(fields[2]);
        if (max_ticks == 0) {
            throw std::invalid_argument("step_event cap must be positive");
        }
        const zeus::simulation::SimulationSessionState state =
            entry.session->stepUntilEvent(max_ticks);
        out << "{";
        writeStateHeader(out, state, entry.session->snapshot());
        out << "}";
        return 0;
    }

    int commandRunToEnd(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 2) {
            throw std::invalid_argument("run-to-end requires 2 tab fields");
        }
        WorkerSession& entry = requireSession(fields[1]);
        const zeus::simulation::SimulationSessionState state = entry.session->runToEnd();
        out << "{";
        writeStateHeader(out, state, entry.session->snapshot());
        if (entry.session->hasResult()) {
            const zeus::simulation::SimulationResult result = entry.session->result();
            out << ", \"arrived\": " << result.stats.arrived
                << ", \"avgTravelS\": " << jsonNumber(result.stats.average_travel_s)
                << ", \"ticks\": " << result.stats.ticks_executed;
        }
        out << "}";
        return 0;
    }

    int commandResume(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 2) {
            throw std::invalid_argument("resume requires 2 tab fields");
        }
        WorkerSession& entry = requireSession(fields[1]);
        const zeus::simulation::SimulationSessionState state = entry.session->resume();
        // This is an acknowledgement, not an observation: the engine may
        // advance immediately after resume returns, so do not pair the copied
        // state with a separately acquired snapshot.
        out << "{\"accepted\": true, \"tick\": " << state.tick
            << ", \"simulationTimeS\": " << jsonNumber(state.simulation_time_s)
            << ", \"stateVersion\": " << state.state_version
            << ", \"finished\": " << (state.finished ? "true" : "false")
            << "}";
        return 0;
    }

    int commandPause(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 2) {
            throw std::invalid_argument("pause requires 2 tab fields");
        }
        WorkerSession& entry = requireSession(fields[1]);
        entry.session->pause();
        const zeus::simulation::SimulationSessionState state = entry.session->observe();
        out << "{";
        writeStateHeader(out, state, entry.session->snapshot());
        out << "}";
        return 0;
    }

    int commandSnapshot(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 3) {
            throw std::invalid_argument("snapshot requires 3 tab fields");
        }
        WorkerSession& entry = requireSession(fields[1]);
        const std::string& snapshot_id = fields[2];
        if (!validSessionId(snapshot_id)) {
            throw std::invalid_argument("invalid snapshot id");
        }
        if (snapshots_.contains(snapshot_id)) {
            throw std::invalid_argument("snapshot already exists: " + snapshot_id);
        }
        const zeus::simulation::SimulationSessionState state = entry.session->observe();
        if (!state.paused && !state.finished) {
            throw std::invalid_argument(
                "snapshot requires a paused or finished session");
        }
        WorkerSnapshot snapshot;
        snapshot.config = entry.config;
        snapshot.demands = entry.demands;
        snapshot.controls = entry.controls;
        snapshot.signals = entry.signals;
        snapshot.applied_actions = entry.applied_actions;
        snapshot.tick = state.tick;
        snapshots_.emplace(snapshot_id, std::move(snapshot));
        out << "{\"snapshotId\": " << jsonString(snapshot_id)
            << ", \"sourceSessionId\": " << jsonString(fields[1])
            << ", \"tick\": " << state.tick
            << ", \"simulationTimeS\": " << jsonNumber(state.simulation_time_s)
            << ", \"stateVersion\": " << state.state_version
            << ", \"actionCount\": " << entry.applied_actions.size()
            << ", \"storage\": \"process_local_replay\"}";
        return 0;
    }

    int commandRestore(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 3) {
            throw std::invalid_argument("restore requires 3 tab fields");
        }
        const std::string& snapshot_id = fields[1];
        const std::string& session_id = fields[2];
        if (!validSessionId(snapshot_id) || !validSessionId(session_id)) {
            throw std::invalid_argument("invalid snapshot or session id");
        }
        const auto found = snapshots_.find(snapshot_id);
        if (found == snapshots_.end()) {
            throw std::invalid_argument("unknown snapshot: " + snapshot_id);
        }
        if (sessions_.contains(session_id)) {
            throw std::invalid_argument("session already exists: " + session_id);
        }
        const WorkerSnapshot& snapshot = found->second;
        auto entry = std::make_unique<WorkerSession>();
        entry->config = snapshot.config;
        entry->demands = snapshot.demands;
        entry->controls = snapshot.controls;
        entry->signals = snapshot.signals;
        entry->session = std::make_unique<zeus::simulation::SimulationSession>(
            engine_, entry->config, entry->demands, entry->controls, entry->signals);
        zeus::simulation::SimulationSessionState state = entry->session->reset();

        for (const AppliedAction& action : snapshot.applied_actions) {
            if (action.tick > snapshot.tick) {
                throw std::logic_error("snapshot action is beyond target tick");
            }
            if (state.tick < action.tick) {
                state = entry->session->step(action.tick - state.tick);
            }
            if (state.tick != action.tick || state.finished) {
                throw std::logic_error("snapshot replay diverged before action");
            }
            zeus::simulation::SimulationSession::CommitResult result;
            if (action.kind == AppliedActionKind::kCommit) {
                result = entry->session->commitRoute(
                    action.vehicle_id, action.algorithm, state.state_version);
            } else {
                result = entry->session->keepRoute(
                    action.vehicle_id, state.state_version);
            }
            if (result != zeus::simulation::SimulationSession::CommitResult::kApplied) {
                throw std::logic_error("snapshot replay rejected a recorded action");
            }
            entry->applied_actions.push_back(action);
        }
        if (state.tick < snapshot.tick) {
            state = entry->session->step(snapshot.tick - state.tick);
        }
        if (state.tick != snapshot.tick) {
            throw std::logic_error("snapshot replay did not reach target tick");
        }

        const zeus::simulation::TickSnapshot restored_snapshot = entry->session->snapshot();
        out << "{\"sessionId\": " << jsonString(session_id)
            << ", \"snapshotId\": " << jsonString(snapshot_id) << ", ";
        writeStateHeader(out, state, restored_snapshot);
        out << ", \"restored\": true}";
        sessions_.emplace(session_id, std::move(entry));
        return 0;
    }

    int commandDropSnapshot(
        const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 2) {
            throw std::invalid_argument("drop-snapshot requires 2 tab fields");
        }
        if (snapshots_.erase(fields[1]) == 0) {
            throw std::invalid_argument("unknown snapshot: " + fields[1]);
        }
        out << "{\"deleted\": true, \"snapshotId\": "
            << jsonString(fields[1]) << "}";
        return 0;
    }

    int commandResult(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 4) {
            throw std::invalid_argument("result requires 4 tab fields");
        }
        WorkerSession& entry = requireSession(fields[1]);
        if (!entry.session->hasResult()) {
            throw std::invalid_argument("session has no completed result");
        }
        const zeus::simulation::SimulationResult result = entry.session->result();
        if (!fields[2].empty()) {
            static_cast<void>(
                zeus::simulation::TrajectoryExporter::save(runtime_, result, fields[2]));
        }
        if (!fields[3].empty()) {
            static_cast<void>(
                zeus::simulation::PlaybackExporter::save(runtime_, result, fields[3]));
        }
        out << "{\"ok\": true"
            << ", \"arrived\": " << result.stats.arrived
            << ", \"vehicles\": " << result.stats.vehicles_total
            << ", \"avgTravelS\": " << jsonNumber(result.stats.average_travel_s)
            << ", \"ticks\": " << result.stats.ticks_executed
            << ", \"trajectory\": " << jsonString(fields[2])
            << ", \"playback\": " << jsonString(fields[3]) << "}";
        return 0;
    }

    int commandClose(const std::vector<std::string>& fields, std::ostringstream& out) {
        if (fields.size() != 2) {
            throw std::invalid_argument("close requires 2 tab fields");
        }
        WorkerSession& entry = requireSession(fields[1]);
        entry.session->close();
        sessions_.erase(fields[1]);
        out << "{}";
        return 0;
    }

    const zeus::map::MapRuntime& runtime_;
    zeus::routing::RoutePlanner planner_;
    zeus::simulation::SimulationEngine engine_;
    std::unordered_map<std::string, std::unique_ptr<WorkerSession>> sessions_;
    std::unordered_map<std::string, WorkerSnapshot> snapshots_;
};

}  // namespace

int runSessionWorker(const zeus::map::MapRuntime& runtime) {
    return SessionWorker(runtime).run();
}

}  // namespace zeus::cli
