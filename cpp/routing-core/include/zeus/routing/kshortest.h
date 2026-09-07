#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "zeus/map/map_runtime.h"
#include "zeus/map/types.h"

#include "zeus/routing/route_types.h"
#include "zeus/routing/search.h"

namespace zeus::routing {

// Upper bound for requested candidate counts; requests are clamped into
// [1, kMaxKPaths] so an agent cannot turn one plan into a subgraph dump.
inline constexpr int kMaxKPaths = 8;

// One candidate of a k-shortest selection, expressed against the caller's
// base query: the virtual start/goal entries are referenced by index and only
// the driven middle edges are listed, mirroring how RoutePlanner assembles a
// single-search route.
struct KShortestPath {
    std::size_t start_index = 0;  // into the base query's starts
    std::size_t goal_index = 0;   // into the base query's goals
    std::vector<zeus::map::EdgeIndex> middle_edges;
    double total_time_s = 0.0;
    std::uint64_t expanded_nodes = 0;  // search effort of the producing search
};

struct KShortestResult {
    // Candidates ordered by (total_time_s, edge sequence); paths[0] is the
    // plain shortest path. Holds fewer than k entries when the graph does
    // not offer k distinct loopless paths.
    std::vector<KShortestPath> paths;
    // The initial shortest-path search, carrying the settle trace when the
    // base query asked for record_trace. Trace recording applies to this
    // search only; spur searches never record.
    SearchOutput first_search;
    // Search effort across every Dijkstra the selection ran, accepted or not.
    std::uint64_t total_expanded_nodes = 0;
    bool found = false;
};

// Loopless k-shortest paths (Yen) over the same endpoint model the planner
// uses. The spur pseudo-start encodes the root prefix in its extra_cost_s
// (including turn penalties along the root), so each spur search's
// total_time_s is already the full candidate time. Root prefix nodes are
// non-revisitable: spur searches ban every edge into them plus the current
// path's diverging edge, which keeps every candidate loopless while twin
// start/goal endpoints stay selectable. All searches run Dijkstra-type so the
// k results are ordered by true travel time.
[[nodiscard]] KShortestResult runKShortestPaths(
    const zeus::map::MapRuntime& runtime,
    const SearchQuery& base_query,
    double max_speed_mps,
    int k);

}  // namespace zeus::routing
