#include "zeus/routing/algorithm_lab.h"
#include <cmath>
#include <iomanip>
#include <stdexcept>

namespace zeus::routing {
AlgorithmLab::AlgorithmLab(const zeus::map::MapRuntime& runtime, RouteRequest request)
    : runtime_(runtime), request_(request) {
    request_.algorithm = Algorithm::kDijkstra;
    request_.record_trace = false;
    baseline_ = RoutePlanner(runtime_).plan(request_);
    if (!baseline_.ok && baseline_.failure != RouteFailure::kUnreachable) {
        throw std::invalid_argument(baseline_.message);
    }
    const auto expand = [&](const RouteEndpointMatch& match, bool exact) {
        std::vector<RoutePosition> positions{{match.edge, match.offset_s}};
        if (!exact) {
            const auto& edge = runtime_.edge(match.edge);
            zeus::map::EdgeIndex twin = zeus::map::kInvalidEdge;
            for (auto candidate : runtime_.outgoingEdges(edge.to)) {
                const auto& reverse = runtime_.edge(candidate);
                if (reverse.to != edge.from) continue;
                if (twin == zeus::map::kInvalidEdge) twin = candidate;
                if (reverse.road_id == edge.road_id) { twin = candidate; break; }
            }
            if (twin != zeus::map::kInvalidEdge && twin != match.edge && enabled(twin)) {
                positions.push_back({twin, std::clamp(runtime_.edge(twin).length_m - match.offset_s,
                                                     0.0, runtime_.edge(twin).length_m)});
            }
        }
        return positions;
    };
    starts_ = expand(baseline_.origin, request_.origin_position.has_value());
    goals_ = expand(baseline_.destination, request_.destination_position.has_value());
}
bool AlgorithmLab::enabled(zeus::map::EdgeIndex edge) const {
    return request_.overlay == nullptr || request_.overlay->edgeEnabled(edge);
}
double AlgorithmLab::seconds(zeus::map::EdgeIndex edge, double length) const {
    const auto factor = request_.overlay == nullptr ? 1.0 : request_.overlay->edgeCostFactor(edge);
    return length / std::max(kMinSpeedMps, double(runtime_.edge(edge).speed_limit_mps)) * factor;
}
std::vector<LabTransition> AlgorithmLab::neighbors(int state) const {
    std::vector<LabTransition> next;
    if (state == -2) return next;
    if (state == -1) {
        for (const auto& start : starts_) {
            if (!enabled(start.edge)) continue;
            const auto length = runtime_.edge(start.edge).length_m - start.offset_s;
            next.push_back({int(start.edge), start.edge, seconds(start.edge, length), length});
            for (const auto& goal : goals_) {
                if (goal.edge == start.edge && goal.offset_s >= start.offset_s) {
                    const auto direct = goal.offset_s - start.offset_s;
                    next.push_back({-2, goal.edge, seconds(goal.edge, direct), direct});
                }
            }
        }
    } else {
        if (state < 0 || std::size_t(state) >= runtime_.data().edges.size()) {
            throw std::invalid_argument("unknown routing state");
        }
        if (!enabled(state)) return next;
        const auto node = runtime_.edge(state).to;
        for (auto edge : runtime_.outgoingEdges(node)) {
            const auto turn = runtime_.turnPenaltySeconds(state, edge);
            if (!enabled(edge) || !std::isfinite(turn)) continue;
            const auto length = runtime_.edge(edge).length_m;
            next.push_back({int(edge), edge, turn + seconds(edge, length), length});
        }
        for (const auto& goal : goals_) {
            if (runtime_.edge(goal.edge).from != node || !enabled(goal.edge)) continue;
            const auto turn = runtime_.turnPenaltySeconds(state, goal.edge);
            if (std::isfinite(turn)) {
                next.push_back({-2, goal.edge, turn + seconds(goal.edge, goal.offset_s), goal.offset_s});
            }
        }
    }
    // Multiple virtual-goal connections must resolve identically in user
    // search and validation (zero-length OD or self-loop endpoint twins).
    std::stable_sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
        if (a.state != b.state) return a.state < b.state;
        if (a.cost != b.cost) return a.cost < b.cost;
        return a.edge < b.edge;
    });
    next.erase(std::unique(next.begin(), next.end(), [](const auto& a, const auto& b) {
        return a.state == b.state;
    }), next.end());
    return next;
}
RouteResult AlgorithmLab::validate(const std::vector<int>& states) const {
    if (states.size() < 2 || states.size() > 10000 || states.front() != -1 || states.back() != -2) {
        throw std::invalid_argument("route must contain start and goal and at most 10000 states");
    }
    RouteResult result;
    result.origin = baseline_.origin;
    result.destination = baseline_.destination;
    for (std::size_t i = 1; i < states.size(); ++i) {
        const auto options = neighbors(states[i - 1]);
        const auto found = std::find_if(options.begin(), options.end(), [&](const auto& option) {
            return option.state == states[i];
        });
        if (found == options.end()) {
            throw std::invalid_argument("illegal transition at step " + std::to_string(i) +
                ": " + std::to_string(states[i-1]) + " -> " + std::to_string(states[i]));
        }
        result.path.edges.push_back(found->edge);
        result.stats.length_m += found->length;
        result.stats.time_s += found->cost;
    }
    for (const auto& start : starts_) {
        if (start.edge == result.path.edges.front()) result.path.start_offset_m = start.offset_s;
    }
    for (const auto& goal : goals_) {
        if (goal.edge == result.path.edges.back()) result.path.end_offset_m = goal.offset_s;
    }
    result.stats.compute_ms = 0;
    result.ok = true;
    return result;
}
RouteResult AlgorithmLab::validatePath(const RoutePath& path) const {
    if (path.edges.empty() || path.edges.size() > 9999 ||
        !std::isfinite(path.start_offset_m) || !std::isfinite(path.end_offset_m)) {
        throw std::invalid_argument("invalid exact route path");
    }
    std::vector<int> states{-1};
    for (std::size_t i = 0; i < path.edges.size(); ++i) {
        if (path.edges[i] >= runtime_.data().edges.size()) {
            throw std::invalid_argument("unknown exact route edge");
        }
        if (i + 1 < path.edges.size()) states.push_back(static_cast<int>(path.edges[i]));
    }
    states.push_back(-2);
    auto result = validate(states);
    if (result.path.edges != path.edges ||
        std::abs(result.path.start_offset_m - path.start_offset_m) > 1e-7 ||
        std::abs(result.path.end_offset_m - path.end_offset_m) > 1e-7) {
        throw std::invalid_argument("exact route does not match the live origin and destination");
    }
    return result;
}
void AlgorithmLab::writeContext(std::ostream& out, std::string_view observation) const {
    const auto count = runtime_.data().edges.size();
    if (count > 500000) throw std::invalid_argument("algorithm lab supports at most 500000 directed edges");
    out << std::setprecision(17) << "{\"version\":\"zeus-routing-v1\",\"adjacency\":{";
    std::size_t transitions = 0;
    for (int state = -1; state < int(count); ++state) {
        if (state != -1) out << ',';
        out << '"' << state << "\":[";
        const auto options = neighbors(state);
        transitions += options.size();
        if (transitions > 2000000) throw std::invalid_argument("algorithm graph exceeds transition budget");
        for (std::size_t i = 0; i < options.size(); ++i) {
            const auto& t = options[i];
            if (i) out << ',';
            out << '[' << t.state << ',' << t.edge << ',' << t.cost << ',' << t.length << ']';
        }
        out << ']';
    }
    out << "},\"nodes\":[";
    for (std::size_t i = 0; i < count; ++i) {
        if (i) out << ',';
        out << runtime_.edge(i).to;
    }
    out << "],\"estimates\":[";
    double max_speed = kMinSpeedMps;
    for (std::size_t i = 0; i < count; ++i) {
        if (!enabled(i)) continue;
        const double factor = request_.overlay == nullptr ? 1.0 : request_.overlay->edgeCostFactor(i);
        max_speed = std::max(max_speed,
            std::max(kMinSpeedMps, double(runtime_.edge(i).speed_limit_mps)) / factor);
    }
    std::vector<zeus::map::Point2d> destinations;
    for (const auto& goal : goals_) destinations.push_back(runtime_.worldPose({goal.edge, goal.offset_s}).point);
    for (std::size_t i = 0; i < count; ++i) {
        if (i) out << ',';
        const auto& point = runtime_.data().nodes[runtime_.edge(i).to].point;
        double lower_bound = std::numeric_limits<double>::infinity();
        for (const auto& goal : destinations) lower_bound = std::min(lower_bound, zeus::map::distance(point, goal) / max_speed);
        out << lower_bound;
    }
    out << "],\"baseline\":{\"ok\":" << (baseline_.ok ? "true" : "false")
        << ",\"timeS\":" << baseline_.stats.time_s << ",\"lengthM\":" << baseline_.stats.length_m
        << "},\"observation\":" << observation << '}';
}
}
