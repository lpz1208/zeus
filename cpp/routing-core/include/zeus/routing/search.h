#pragma once

#include <cstdint>
#include <vector>

#include "zeus/map/map_runtime.h"
#include "zeus/map/types.h"

#include "zeus/routing/route_types.h"

namespace zeus::routing {

// A virtual start or goal: a partial traversal of the matched edge plus the
// node the search departs from or arrives at.
struct SearchEndpoint {
    zeus::map::NodeIndex node = zeus::map::kInvalidNode;
    zeus::map::EdgeIndex edge = zeus::map::kInvalidEdge;
    double offset_s = 0.0;
    double extra_cost_s = 0.0;
    double extra_length_m = 0.0;
};

struct SearchQuery {
    std::vector<SearchEndpoint> starts;
    std::vector<SearchEndpoint> goals;
    Algorithm algorithm = Algorithm::kDijkstra;
    const RoutingOverlay* overlay = nullptr;
};

struct SearchOutput {
    bool found = false;
    double total_time_s = 0.0;
    std::size_t start_index = 0;
    std::size_t goal_index = 0;
    std::vector<zeus::map::EdgeIndex> node_edges;
    std::uint64_t expanded_nodes = 0;
};

// Multi-source multi-goal shortest path over the runtime CSR graph. Dijkstra
// and A* share one implementation: the heuristic is euclidean distance to the
// closest goal node divided by max speed, which is admissible and consistent,
// so Dijkstra is simply the h == 0 case. `known_best_time_s` prunes search
// nodes that cannot beat an already known direct combination; pass
// std::numeric_limits<double>::infinity() when there is none.
[[nodiscard]] SearchOutput runShortestPathSearch(
    const zeus::map::MapRuntime& runtime,
    const SearchQuery& query,
    double max_speed_mps,
    double known_best_time_s);

// Edge-state shortest path used when the map carries explicit turn
// transitions. The state is the incoming directed edge, so relaxing the next
// edge can enforce (from_edge,to_edge) prohibitions and penalties. Both
// bidirectional algorithm selections intentionally use this forward search
// until a restriction-safe reverse state graph is introduced.
[[nodiscard]] SearchOutput runTurnAwareSearch(
    const zeus::map::MapRuntime& runtime,
    const SearchQuery& query,
    double max_speed_mps,
    double known_best_time_s);

// Incoming-edge CSR keyed by edge.to, buckets in ascending EdgeIndex order.
// Built once by the caller (RoutePlanner) and reused across requests; edges
// with an invalid `to` node are skipped because MapRuntime only validates
// `from` when it builds the outgoing adjacency.
struct IncomingAdjacency {
    std::vector<std::uint32_t> offsets;
    std::vector<zeus::map::EdgeIndex> edges;
};

[[nodiscard]] IncomingAdjacency buildIncomingAdjacency(const zeus::map::MapData& data);

// Bidirectional variant over outgoing plus incoming adjacency. Uses the
// symmetric Ikeda potential p(v) = (h_t(v) - h_s(v)) / 2 for A* (zero for
// Dijkstra); start labels are initialized with +p, goal labels with -p, so
// the potential cancels exactly at any meeting point and the tracked best
// total is already in real seconds. Terminates once the stale-filtered queue
// tops satisfy top_f + top_b >= best. `expanded_nodes` counts settles on both
// sides.
[[nodiscard]] SearchOutput runBidirectionalSearch(
    const zeus::map::MapRuntime& runtime,
    const IncomingAdjacency& incoming,
    const SearchQuery& query,
    double max_speed_mps,
    double known_best_time_s);

}  // namespace zeus::routing
