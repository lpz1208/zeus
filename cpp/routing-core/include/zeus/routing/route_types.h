#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
};

[[nodiscard]] const char* algorithmName(Algorithm algorithm);
[[nodiscard]] bool parseAlgorithm(const std::string& value, Algorithm& algorithm);
[[nodiscard]] bool isBidirectional(Algorithm algorithm);

// Defensive floor for edges whose speed limit is missing or invalid; such
// edges exist in published maps because invalid speeds are errors, not fatals.
inline constexpr double kMinSpeedMps = 0.1;

[[nodiscard]] inline double edgeCostSeconds(const zeus::map::DirectedEdge& edge) {
    return edge.length_m / std::max(kMinSpeedMps, static_cast<double>(edge.speed_limit_mps));
}

struct RouteRequest {
    zeus::map::Point2d origin;
    zeus::map::Point2d destination;
    Algorithm algorithm = Algorithm::kDijkstra;
    double max_snap_distance_m = 100.0;
    std::size_t max_match_candidates = 8;
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
    Algorithm algorithm = Algorithm::kDijkstra;
    RouteEndpointMatch origin;
    RouteEndpointMatch destination;
    RoutePath path;
    RouteStatistics stats;
};

}  // namespace zeus::routing
