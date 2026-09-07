#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "zeus/map/map_runtime.h"
#include "zeus/map/types.h"

namespace zeus::routing {

enum class Algorithm : std::uint8_t {
    kDijkstra = 0,
    kAStar = 1,
    kBidirectionalDijkstra = 2,
    kBidirectionalAStar = 3,
    kKShortest = 4,
};

// Stable metadata exposed to navigation agents. Algorithms remain ordinary
// deterministic C++ functions; this registry describes when an orchestrator
// may select each one without coupling it to search internals.
struct AlgorithmCapability {
    Algorithm algorithm = Algorithm::kDijkstra;
    const char* version = "1";
    const char* search_direction = "forward";
    bool supports_dynamic_weights = true;
    bool supports_incremental_repair = false;
    bool supports_k_candidates = false;
    bool supports_time_dependency = false;
    bool deterministic = true;
    bool exact = true;
    bool uses_heuristic = false;
};

[[nodiscard]] const char* algorithmName(Algorithm algorithm);
[[nodiscard]] bool parseAlgorithm(const std::string& value, Algorithm& algorithm);
[[nodiscard]] bool isBidirectional(Algorithm algorithm);
[[nodiscard]] std::span<const AlgorithmCapability> algorithmCapabilities();
[[nodiscard]] const AlgorithmCapability* algorithmCapability(Algorithm algorithm);

// Defensive floor for edges whose speed limit is missing or invalid; such
// edges exist in published maps because invalid speeds are errors, not fatals.
inline constexpr double kMinSpeedMps = 0.1;

[[nodiscard]] inline double edgeCostSeconds(const zeus::map::DirectedEdge& edge) {
    return edge.length_m / std::max(kMinSpeedMps, static_cast<double>(edge.speed_limit_mps));
}

// Per-request dynamic routing view. Empty spans mean that every edge is
// enabled and has its static map cost. The caller owns the backing arrays for
// the duration of RoutePlanner::plan().
struct RoutingOverlay {
    std::span<const std::uint8_t> edge_enabled;
    std::span<const double> edge_cost_factors;

    [[nodiscard]] bool edgeEnabled(zeus::map::EdgeIndex edge) const {
        return edge_enabled.empty() || edge_enabled[edge] != 0;
    }

    [[nodiscard]] double edgeCostFactor(zeus::map::EdgeIndex edge) const {
        return edge_cost_factors.empty() ? 1.0 : edge_cost_factors[edge];
    }
};

struct RoutePosition {
    zeus::map::EdgeIndex edge = zeus::map::kInvalidEdge;
    double offset_s = 0.0;
};

struct RouteRequest {
    zeus::map::Point2d origin;
    zeus::map::Point2d destination;
    Algorithm algorithm = Algorithm::kDijkstra;
    double max_snap_distance_m = 100.0;
    std::size_t max_match_candidates = 8;
    // Number of route candidates requested from k-shortest selection; clamped
    // to [1, 8]. Values above 1 only take effect for Algorithm::kKShortest.
    int k_paths = 1;
    // Record the settle sequence of the underlying search for visualization.
    bool record_trace = false;
    // Exact directed positions bypass map matching. Dynamic simulation
    // rerouting uses origin_position so a vehicle cannot jump to a nearby or
    // reverse edge when its route changes.
    std::optional<RoutePosition> origin_position;
    std::optional<RoutePosition> destination_position;
    const RoutingOverlay* overlay = nullptr;
};

struct RouteEndpointMatch {
    zeus::map::EdgeIndex edge = zeus::map::kInvalidEdge;
    double offset_s = 0.0;
    double lateral_distance_m = 0.0;
    double confidence = 0.0;
};

struct RoutePath {
    std::vector<zeus::map::EdgeIndex> edges;
    double start_offset_m = 0.0;
    double end_offset_m = 0.0;
};

struct RouteStatistics {
    double length_m = 0.0;
    double time_s = 0.0;
    std::uint64_t expanded_nodes = 0;
    double compute_ms = 0.0;
};

// One candidate of a k-shortest selection. When non-empty,
// RouteResult::alternatives holds every candidate including the best one, so
// alternatives.front() mirrors RouteResult::path.
struct RouteAlternative {
    RoutePath path;
    double time_s = 0.0;
    double length_m = 0.0;
    std::uint64_t expanded_nodes = 0;  // search effort attributed to this path
};

// One settled search state, in settle order. Node-state searches record the
// settled node directly; edge-state searches record edges[edge].to so the
// trace is a uniform node sequence for visualization.
struct SearchTraceStep {
    std::uint32_t order = 0;  // 1-based settle ordinal
    std::uint32_t node = 0;
    double f = 0.0;
    double g = 0.0;
};

enum class RouteFailure : std::uint8_t {
    kNone = 0,
    kEmptyMap,
    kOriginUnmatched,
    kDestinationUnmatched,
    kUnreachable,
};

[[nodiscard]] const char* routeFailureName(RouteFailure failure);

struct RouteResult {
    bool ok = false;
    RouteFailure failure = RouteFailure::kNone;
    std::string message;
    Algorithm algorithm = Algorithm::kDijkstra;            // requested
    // What actually ran: bidirectional selections downgrade to the forward
    // edge-state search on maps that carry turn transitions.
    Algorithm effective_algorithm = Algorithm::kDijkstra;
    RouteEndpointMatch origin;
    RouteEndpointMatch destination;
    RoutePath path;
    RouteStatistics stats;
    // All k-shortest candidates (alternatives.front() == path); empty unless
    // the request asked for more than one path.
    std::vector<RouteAlternative> alternatives;
    // Settle sequence of the search that produced `path`; empty unless the
    // request set record_trace.
    std::vector<SearchTraceStep> search_trace;
};

}  // namespace zeus::routing
