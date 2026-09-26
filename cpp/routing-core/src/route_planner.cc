#include "zeus/routing/route_planner.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include "zeus/routing/kshortest.h"
#include "zeus/routing/incremental_search.h"
#include "zeus/routing/time_dependent.h"
#include "zeus/routing/landmarks.h"
#include "zeus/routing/contraction_hierarchy.h"
#include "zeus/routing/sequence_search.h"

namespace zeus::routing {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

std::uint64_t pairKey(zeus::map::NodeIndex from, zeus::map::NodeIndex to) {
    return (static_cast<std::uint64_t>(from) << 32) | to;
}

// matchPoint sorts candidates by score with a non-stable sort; ties between
// the two directed twins of a bidirectional road must still resolve
// deterministically, so re-pick here with an explicit ordering.
const zeus::map::MapMatchCandidate* pickDeterministic(
    const std::vector<zeus::map::MapMatchCandidate>& candidates,
    const RoutingOverlay* overlay) {
    const zeus::map::MapMatchCandidate* best = nullptr;
    for (const zeus::map::MapMatchCandidate& candidate : candidates) {
        if (overlay != nullptr && !overlay->edgeEnabled(candidate.edge)) {
            continue;
        }
        if (best == nullptr) {
            best = &candidate;
            continue;
        }
        if (candidate.score != best->score) {
            if (candidate.score < best->score) {
                best = &candidate;
            }
            continue;
        }
        if (candidate.lateral_distance_m != best->lateral_distance_m) {
            if (candidate.lateral_distance_m < best->lateral_distance_m) {
                best = &candidate;
            }
            continue;
        }
        if (candidate.edge < best->edge) {
            best = &candidate;
        }
    }
    return best;
}

struct DirectOption {
    std::size_t start_index = 0;
    std::size_t goal_index = 0;
    double time_s = kInfinity;
    double length_m = 0.0;
};

}  // namespace

RoutePlanner::RoutePlanner(const zeus::map::MapRuntime& runtime) : runtime_(runtime) {
    for (std::size_t i = 0; i < runtime_.data().edges.size(); ++i) {
        const zeus::map::DirectedEdge& edge = runtime_.data().edges[i];
        max_speed_mps_ = std::max(max_speed_mps_, static_cast<double>(edge.speed_limit_mps));
        edges_by_pair_[pairKey(edge.from, edge.to)].push_back(
            static_cast<zeus::map::EdgeIndex>(i));
    }
    incoming_ = buildIncomingAdjacency(runtime_.data());
}

zeus::map::EdgeIndex RoutePlanner::findTwin(zeus::map::EdgeIndex edge_index) const {
    const zeus::map::DirectedEdge& edge = runtime_.edge(edge_index);
    const auto found = edges_by_pair_.find(pairKey(edge.to, edge.from));
    if (found == edges_by_pair_.end()) {
        return zeus::map::kInvalidEdge;
    }
    // Twins share the road id; fall back to the first opposite edge otherwise.
    for (const zeus::map::EdgeIndex candidate : found->second) {
        if (runtime_.edge(candidate).road_id == edge.road_id) {
            return candidate;
        }
    }
    return found->second.front();
}

RouteResult RoutePlanner::plan(const RouteRequest& request, IncrementalSearch* incremental) const {
    if (!std::isfinite(request.departure_time_s) || request.departure_time_s < 0 ||
        !std::isfinite(request.cost_reference_time_s) || request.cost_reference_time_s < 0)
        throw std::invalid_argument("route times must be finite and nonnegative");
    if (request.speed_schedule && request.algorithm != Algorithm::kTimeDependent)
        throw std::invalid_argument("speed schedule requires tddijkstra");
    const SpeedSchedule empty_schedule;
    const auto& schedule = request.speed_schedule ? *request.speed_schedule : empty_schedule;
    const auto start_time = std::chrono::steady_clock::now();

    RouteResult result;
    result.algorithm = request.algorithm;
    result.effective_algorithm = request.algorithm;

    const zeus::map::MapData& data = runtime_.data();
    if (data.edges.empty() || data.nodes.empty()) {
        result.failure = RouteFailure::kEmptyMap;
        result.message = "runtime map has no navigable edges";
        return result;
    }

    if (request.overlay != nullptr) {
        const RoutingOverlay& overlay = *request.overlay;
        if ((!overlay.edge_enabled.empty() && overlay.edge_enabled.size() != data.edges.size()) ||
            (!overlay.edge_cost_factors.empty() &&
             overlay.edge_cost_factors.size() != data.edges.size())) {
            throw std::invalid_argument("routing overlay size does not match map edges");
        }
        for (const double factor : overlay.edge_cost_factors) {
            if (!std::isfinite(factor) || factor < 1.0) {
                throw std::invalid_argument(
                    "routing edge cost factors must be finite and at least one");
            }
        }
    }

    zeus::map::MapMatchOptions match_options;
    match_options.max_results = std::max<std::size_t>(1, request.max_match_candidates);
    match_options.max_distance_m = request.max_snap_distance_m;
    if (request.overlay != nullptr) {
        match_options.edge_enabled = request.overlay->edge_enabled;
    }

    std::vector<zeus::map::MapMatchCandidate> origin_candidates;
    zeus::map::MapMatchCandidate exact_origin;
    const zeus::map::MapMatchCandidate* origin_match = nullptr;
    if (request.origin_position.has_value()) {
        const RoutePosition& position = *request.origin_position;
        if (position.edge >= data.edges.size() || !std::isfinite(position.offset_s) ||
            position.offset_s < -1e-9 ||
            position.offset_s > data.edges[position.edge].length_m + 1e-9) {
            throw std::invalid_argument("exact route origin is outside its edge");
        }
        exact_origin.edge = position.edge;
        exact_origin.offset_s = std::clamp(
            position.offset_s, 0.0, data.edges[position.edge].length_m);
        exact_origin.confidence = 1.0;
        origin_match = &exact_origin;
    } else {
        origin_candidates = runtime_.matchPoint(request.origin, match_options);
        origin_match = pickDeterministic(origin_candidates, request.overlay);
    }
    if (origin_match == nullptr) {
        result.failure = RouteFailure::kOriginUnmatched;
        result.message = "origin is farther than the snap distance from any road";
        return result;
    }

    std::vector<zeus::map::MapMatchCandidate> destination_candidates;
    zeus::map::MapMatchCandidate exact_destination;
    const zeus::map::MapMatchCandidate* destination_match = nullptr;
    if (request.destination_position.has_value()) {
        const RoutePosition& position = *request.destination_position;
        if (position.edge >= data.edges.size() || !std::isfinite(position.offset_s) ||
            position.offset_s < -1e-9 ||
            position.offset_s > data.edges[position.edge].length_m + 1e-9 ||
            (request.overlay != nullptr && !request.overlay->edgeEnabled(position.edge))) {
            throw std::invalid_argument("exact route destination is unavailable");
        }
        exact_destination.edge = position.edge;
        exact_destination.offset_s = std::clamp(
            position.offset_s, 0.0, data.edges[position.edge].length_m);
        exact_destination.confidence = 1.0;
        destination_match = &exact_destination;
    } else {
        destination_candidates = runtime_.matchPoint(request.destination, match_options);
        destination_match = pickDeterministic(destination_candidates, request.overlay);
    }
    if (destination_match == nullptr) {
        result.failure = RouteFailure::kDestinationUnmatched;
        result.message = "destination is farther than the snap distance from any road";
        return result;
    }

    result.origin = {origin_match->edge, origin_match->offset_s,
                     origin_match->lateral_distance_m, origin_match->confidence};
    result.destination = {destination_match->edge, destination_match->offset_s,
                          destination_match->lateral_distance_m,
                          destination_match->confidence};

    // Expand each endpoint into a forward option plus a reverse option on the
    // twin edge, so a route may leave or arrive against the matched direction.
    SearchQuery query;
    query.algorithm = request.algorithm;
    query.overlay = request.overlay;
    query.record_trace = request.record_trace;

    const auto costFactor = [&](zeus::map::EdgeIndex edge) {
        return request.overlay == nullptr ? 1.0 : request.overlay->edgeCostFactor(edge);
    };

    const auto appendStart = [&](zeus::map::EdgeIndex edge_index) {
        const zeus::map::DirectedEdge& edge = runtime_.edge(edge_index);
        const double offset =
            std::clamp(edge_index == origin_match->edge
                           ? origin_match->offset_s
                           : edge.length_m - origin_match->offset_s,
                       0.0, edge.length_m);
        query.starts.push_back({edge.to, edge_index, offset,
                                (edge.length_m - offset) /
                                        std::max(kMinSpeedMps, static_cast<double>(edge.speed_limit_mps)) *
                                    costFactor(edge_index),
                                edge.length_m - offset});
    };
    const auto appendGoal = [&](zeus::map::EdgeIndex edge_index) {
        const zeus::map::DirectedEdge& edge = runtime_.edge(edge_index);
        const double offset =
            std::clamp(edge_index == destination_match->edge
                           ? destination_match->offset_s
                           : edge.length_m - destination_match->offset_s,
                       0.0, edge.length_m);
        query.goals.push_back({edge.from, edge_index, offset,
                               offset /
                                       std::max(kMinSpeedMps, static_cast<double>(edge.speed_limit_mps)) *
                                   costFactor(edge_index),
                               offset});
    };

    appendStart(origin_match->edge);
    appendGoal(destination_match->edge);
    const zeus::map::EdgeIndex origin_twin = request.origin_position.has_value()
                                                  ? zeus::map::kInvalidEdge
                                                  : findTwin(origin_match->edge);
    const zeus::map::EdgeIndex destination_twin = request.destination_position.has_value()
                                                       ? zeus::map::kInvalidEdge
                                                       : findTwin(destination_match->edge);
    if (origin_twin != zeus::map::kInvalidEdge && origin_twin != origin_match->edge &&
        (request.overlay == nullptr || request.overlay->edgeEnabled(origin_twin))) {
        appendStart(origin_twin);
    }
    if (destination_twin != zeus::map::kInvalidEdge &&
        destination_twin != destination_match->edge &&
        (request.overlay == nullptr || request.overlay->edgeEnabled(destination_twin))) {
        appendGoal(destination_twin);
    }

    if (request.origin_turn_state) (void)SequenceGraph(runtime_).initial(request, query.starts.front().edge);

    // Direct travel along one matched edge, no intersection search needed.
    DirectOption direct;
    for (std::size_t s = 0; s < query.starts.size(); ++s) {
        for (std::size_t g = 0; g < query.goals.size(); ++g) {
            if (query.starts[s].edge != query.goals[g].edge ||
                query.starts[s].offset_s > query.goals[g].offset_s) {
                continue;
            }
            const double length = query.goals[g].offset_s - query.starts[s].offset_s;
            const double time = request.algorithm == Algorithm::kTimeDependent
                ? schedule.duration(runtime_, query.starts[s].edge, length, request.departure_time_s,
                    costFactor(query.starts[s].edge), request.cost_reference_time_s)
                : length / std::max(
                                          kMinSpeedMps,
                                          static_cast<double>(
                                              runtime_.edge(query.starts[s].edge).speed_limit_mps)) *
                                costFactor(query.starts[s].edge);
            if (time < direct.time_s) {
                direct = {s, g, time, length};
            }
        }
    }

    // K-shortest selection assembles its own result (best path plus the
    // full candidate list) and never mixes with the single-path search below.
    if (!runtime_.hasTurnSequences() && request.algorithm == Algorithm::kKShortest && request.k_paths > 1) {
        const KShortestResult selection =
            runKShortestPaths(runtime_, query, max_speed_mps_, request.k_paths);
        const bool use_search =
            selection.found && !selection.paths.empty() &&
            selection.paths.front().total_time_s < direct.time_s;
        if (!use_search && direct.time_s == kInfinity) {
            result.failure = RouteFailure::kUnreachable;
            result.message = "no path connects the matched origin and destination edges";
            result.stats.expanded_nodes = selection.total_expanded_nodes;
            return result;
        }
        if (use_search) {
            result.alternatives.reserve(selection.paths.size());
            for (const KShortestPath& candidate : selection.paths) {
                const SearchEndpoint& start = query.starts[candidate.start_index];
                const SearchEndpoint& goal = query.goals[candidate.goal_index];
                RouteAlternative alternative;
                alternative.path.edges.push_back(start.edge);
                alternative.path.edges.insert(
                    alternative.path.edges.end(), candidate.middle_edges.begin(),
                    candidate.middle_edges.end());
                if (goal.edge != start.edge || !candidate.middle_edges.empty()) {
                    alternative.path.edges.push_back(goal.edge);
                }
                alternative.path.start_offset_m = start.offset_s;
                alternative.path.end_offset_m = goal.offset_s;
                alternative.time_s = candidate.total_time_s;
                alternative.length_m = start.extra_length_m + goal.extra_length_m;
                for (const zeus::map::EdgeIndex edge_index : candidate.middle_edges) {
                    alternative.length_m += runtime_.edge(edge_index).length_m;
                }
                alternative.expanded_nodes = candidate.expanded_nodes;
                result.alternatives.push_back(std::move(alternative));
            }
            result.path = result.alternatives.front().path;
            result.stats.time_s = result.alternatives.front().time_s;
            result.stats.length_m = result.alternatives.front().length_m;
        } else {
            const SearchEndpoint& start = query.starts[direct.start_index];
            const SearchEndpoint& goal = query.goals[direct.goal_index];
            RouteAlternative alternative;
            alternative.path.edges.push_back(start.edge);
            alternative.path.start_offset_m = start.offset_s;
            alternative.path.end_offset_m = goal.offset_s;
            alternative.time_s = direct.time_s;
            alternative.length_m = direct.length_m;
            result.path = alternative.path;
            result.stats.time_s = direct.time_s;
            result.stats.length_m = direct.length_m;
            result.alternatives.push_back(std::move(alternative));
        }
        result.stats.expanded_nodes = selection.total_expanded_nodes;
        if (request.record_trace) {
            result.search_trace = std::move(selection.first_search.trace);
        }
        result.ok = true;
        const auto k_end = std::chrono::steady_clock::now();
        result.stats.compute_ms =
            std::chrono::duration<double, std::milli>(k_end - start_time).count();
        return result;
    }

    SearchOutput search;
    if (runtime_.hasTurnSequences()) {
        result.effective_algorithm = request.algorithm == Algorithm::kTimeDependent ? Algorithm::kTimeDependent : Algorithm::kDijkstra;
        if (result.effective_algorithm != request.algorithm) result.stats.fallback_reason = "via_way_history";
        search = runSequenceSearch(runtime_, query, request, schedule, direct.time_s);
    } else if (request.algorithm == Algorithm::kCH) {
        if (ContractionHierarchy::baseWeights(request.overlay)) {
            bool built = false;
            const auto begin = std::chrono::steady_clock::now();
            std::call_once(ch_once_, [&] {
                hierarchy_ = std::make_shared<const ContractionHierarchy>(runtime_);
                built = true;
            });
            result.stats.ch_reused = !built && hierarchy_->available();
            result.stats.ch_shortcuts = hierarchy_->shortcuts();
            result.stats.ch_core_states = hierarchy_->coreStates();
            result.stats.ch_bytes = hierarchy_->bytes();
            if (built) result.stats.ch_preprocess_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin).count();
            if (hierarchy_->available()) search = hierarchy_->search(query, incoming_, direct.time_s);
            else result.stats.fallback_reason = "ch_base_graph_budget";
        } else result.stats.fallback_reason = "ch_dynamic_weights";
        if (!result.stats.fallback_reason.empty()) {
            result.effective_algorithm = Algorithm::kBidirectionalDijkstra;
            query.algorithm = result.effective_algorithm;
            search = runtime_.hasTurnTransitions()
                ? runTurnAwareBidirectionalSearch(runtime_, incoming_, query, max_speed_mps_, direct.time_s)
                : runBidirectionalSearch(runtime_, incoming_, query, max_speed_mps_, direct.time_s);
        }
    } else if (request.algorithm == Algorithm::kAlt) {
        bool built = false;
        const auto begin = std::chrono::steady_clock::now();
        std::call_once(landmark_once_, [&] {
            landmarks_ = std::make_shared<const LandmarkIndex>(runtime_, incoming_);
            built = true;
        });
        result.stats.landmark_reused = !built;
        result.stats.landmark_count = landmarks_->count();
        result.stats.landmark_bytes = landmarks_->tableBytes();
        if (built) result.stats.landmark_preprocess_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
        search = runAltSearch(runtime_, query, *landmarks_, direct.time_s);
    } else if (request.algorithm == Algorithm::kTimeDependent) {
        search = runTimeDependentSearch(runtime_, query, request, schedule, direct.time_s);
    } else if (isIncremental(request.algorithm)) {
        if (incremental && &incremental->runtime() != &runtime_)
            throw std::invalid_argument("incremental context belongs to another map");
        if (incremental) search = incremental->run(query);
        else { IncrementalSearch cold(runtime_); search = cold.run(query); }
    } else search = runtime_.hasTurnTransitions()
                                    ? (isBidirectional(request.algorithm)
                                          ? runTurnAwareBidirectionalSearch(
                                                runtime_, incoming_, query, max_speed_mps_, direct.time_s)
                                          : runTurnAwareSearch(
                                                runtime_, query, max_speed_mps_, direct.time_s))
                                    : isBidirectional(request.algorithm)
                                          ? runBidirectionalSearch(
                                                runtime_, incoming_, query,
                                                max_speed_mps_, direct.time_s)
                                          : runShortestPathSearch(
                                                runtime_, query, max_speed_mps_,
                                                direct.time_s);

    result.stats.incremental_reused = search.incremental_reused;
    result.stats.updated_edges = search.updated_edges;
    bool use_search = search.found && search.total_time_s < direct.time_s;
    if (!use_search && direct.time_s == kInfinity) {
        result.failure = RouteFailure::kUnreachable;
        result.message = "no path connects the matched origin and destination edges";
        result.stats.expanded_nodes = search.expanded_nodes;
        return result;
    }

    if (use_search) {
        const SearchEndpoint& start = query.starts[search.start_index];
        const SearchEndpoint& goal = query.goals[search.goal_index];
        result.path.edges.push_back(start.edge);
        result.path.edges.insert(
            result.path.edges.end(), search.node_edges.begin(), search.node_edges.end());
        // A searched path traverses the final partial edge even when it is
        // the origin self-loop again. Same-edge direct travel is handled below.
        result.path.edges.push_back(goal.edge);
        result.path.start_offset_m = start.offset_s;
        result.path.end_offset_m = goal.offset_s;
        result.stats.time_s = search.total_time_s;
        result.stats.length_m = start.extra_length_m + goal.extra_length_m;
        for (const zeus::map::EdgeIndex edge_index : search.node_edges) {
            result.stats.length_m += runtime_.edge(edge_index).length_m;
        }
    } else {
        const SearchEndpoint& start = query.starts[direct.start_index];
        const SearchEndpoint& goal = query.goals[direct.goal_index];
        result.path.edges.push_back(start.edge);
        result.path.start_offset_m = start.offset_s;
        result.path.end_offset_m = goal.offset_s;
        result.stats.time_s = direct.time_s;
        result.stats.length_m = direct.length_m;
    }

    result.stats.expanded_nodes = search.expanded_nodes;
    if (request.record_trace) {
        result.search_trace = std::move(search.trace);
    }
    result.ok = true;
    const auto end_time = std::chrono::steady_clock::now();
    result.stats.compute_ms = std::chrono::duration<double, std::milli>(end_time - start_time)
                                  .count();
    return result;
}

}  // namespace zeus::routing
