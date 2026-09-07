#include "zeus/routing/kshortest.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace zeus::routing {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

// Per accepted path, bound the number of spur searches so long routes on
// large maps cannot turn one k-shortest request into thousands of Dijkstras.
std::size_t spurSearchBudget(int k) {
    return std::max<std::size_t>(64, static_cast<std::size_t>(8 * k));
}

double edgeCostWithFactor(const RoutingOverlay* overlay, zeus::map::EdgeIndex index,
                          const zeus::map::DirectedEdge& edge) {
    const double factor =
        overlay == nullptr ? 1.0 : overlay->edgeCostFactor(index);
    return edgeCostSeconds(edge) * factor;
}

// Materialized base overlay plus Yen's banned edges. Banning is expressed on
// edges only: a candidate is loopless when no spur search can enter a root
// prefix node again, so every edge whose head is a root node is disabled.
struct SpurOverlay {
    std::vector<std::uint8_t> enabled;
    std::vector<double> factors;
    RoutingOverlay overlay;

    explicit SpurOverlay(const SearchQuery& base, const zeus::map::MapData& data) {
        const std::size_t edge_count = data.edges.size();
        enabled.assign(edge_count, 1);
        if (base.overlay != nullptr) {
            if (!base.overlay->edge_enabled.empty()) {
                std::copy(base.overlay->edge_enabled.begin(),
                          base.overlay->edge_enabled.end(), enabled.begin());
            }
            if (!base.overlay->edge_cost_factors.empty()) {
                factors.assign(base.overlay->edge_cost_factors.begin(),
                               base.overlay->edge_cost_factors.end());
            }
        }
        overlay.edge_enabled = enabled;
        overlay.edge_cost_factors = factors;
    }

    void ban(zeus::map::EdgeIndex edge) {
        if (edge < enabled.size()) {
            enabled[edge] = 0;
        }
    }

    void banEdgesInto(const std::vector<zeus::map::EdgeIndex>& edges,
                      const zeus::map::MapData& data) {
        std::set<zeus::map::NodeIndex> heads;
        for (const zeus::map::EdgeIndex edge : edges) {
            heads.insert(data.edges[edge].to);
        }
        for (std::size_t edge = 0; edge < data.edges.size(); ++edge) {
            if (heads.count(data.edges[edge].to) != 0) {
                enabled[edge] = 0;
            }
        }
    }
};

// Identity of a candidate for duplicate detection: identical driven edges
// through identical virtual endpoints.
using PathKey = std::tuple<std::size_t, std::size_t, std::vector<zeus::map::EdgeIndex>>;

PathKey keyOf(const KShortestPath& path) {
    return {path.start_index, path.goal_index, path.middle_edges};
}

// Best-first candidate pool. Ordered by time, then edge sequence for
// deterministic tie-breaking across platforms.
struct BEntry {
    double time_s = kInfinity;
    KShortestPath path;
};

struct BEntryLess {
    bool operator()(const BEntry& a, const BEntry& b) const {
        if (a.time_s != b.time_s) {
            return a.time_s < b.time_s;
        }
        return keyOf(a.path) < keyOf(b.path);
    }
};

SearchOutput runDijkstra(const zeus::map::MapRuntime& runtime, const SearchQuery& query,
                         double max_speed_mps, bool turn_aware) {
    // No known-best pruning: every spur search must be free to find any
    // suffix cost, including ones worse than a previously accepted path.
    return turn_aware ? runTurnAwareSearch(runtime, query, max_speed_mps, kInfinity)
                      : runShortestPathSearch(runtime, query, max_speed_mps, kInfinity);
}

KShortestPath pathFromSearch(const SearchOutput& search) {
    KShortestPath path;
    path.start_index = search.start_index;
    path.goal_index = search.goal_index;
    path.middle_edges = search.node_edges;
    path.total_time_s = search.total_time_s;
    path.expanded_nodes = search.expanded_nodes;
    return path;
}

}  // namespace

KShortestResult runKShortestPaths(
    const zeus::map::MapRuntime& runtime,
    const SearchQuery& base_query,
    double max_speed_mps,
    int k) {
    KShortestResult result;
    if (base_query.starts.empty() || base_query.goals.empty()) {
        return result;
    }
    k = std::clamp(k, 1, kMaxKPaths);

    const zeus::map::MapData& data = runtime.data();
    const bool turn_aware = runtime.hasTurnTransitions();

    SearchQuery initial_query = base_query;
    initial_query.record_trace = base_query.record_trace;
    // Dijkstra-type base search keeps candidate ordering by true travel time
    // even when the request selected a heuristic algorithm.
    initial_query.algorithm = Algorithm::kDijkstra;
    SearchOutput first = runDijkstra(runtime, initial_query, max_speed_mps, turn_aware);
    result.total_expanded_nodes += first.expanded_nodes;
    if (!first.found) {
        return result;
    }
    result.first_search = first;
    result.found = true;
    result.paths.push_back(pathFromSearch(first));

    // Full driven chain of a candidate: virtual start edge + middle edges +
    // virtual goal edge (mirrors the planner's assembly rule).
    const auto fullChain = [&](const KShortestPath& path) {
        std::vector<zeus::map::EdgeIndex> chain;
        chain.push_back(base_query.starts[path.start_index].edge);
        chain.insert(chain.end(), path.middle_edges.begin(), path.middle_edges.end());
        const zeus::map::EdgeIndex goal_edge = base_query.goals[path.goal_index].edge;
        const zeus::map::EdgeIndex start_edge = base_query.starts[path.start_index].edge;
        if (goal_edge != start_edge || !path.middle_edges.empty()) {
            chain.push_back(goal_edge);
        }
        return chain;
    };

    std::set<PathKey> accepted;
    accepted.insert(keyOf(result.paths.front()));
    std::set<BEntry, BEntryLess> pool;

    while (result.paths.size() < static_cast<std::size_t>(k)) {
        const KShortestPath& current = result.paths.back();
        const std::vector<zeus::map::EdgeIndex> chain = fullChain(current);
        SpurOverlay spur_overlay(base_query, data);

        // Cost of driving chain[0..r-1]: the virtual start edge contributes
        // its remaining-traversal cost, later root edges their full cost and
        // the turn penalties between consecutive root edges.
        double prefix_cost = base_query.starts[current.start_index].extra_cost_s;
        double prefix_length = base_query.starts[current.start_index].extra_length_m;
        std::vector<zeus::map::EdgeIndex> root_nodes_source;

        std::size_t searches = 0;
        const std::size_t budget = spurSearchBudget(k);
        for (std::size_t r = 1; r < chain.size() && searches < budget; ++r) {
            const zeus::map::EdgeIndex pseudo_edge = chain[r - 1];
            if (r > 1) {
                const zeus::map::EdgeIndex previous = chain[r - 2];
                if (turn_aware) {
                    const double turn = runtime.turnPenaltySeconds(previous, pseudo_edge);
                    if (std::isfinite(turn)) {
                        prefix_cost += turn;
                    }
                }
                prefix_cost += edgeCostWithFactor(base_query.overlay, pseudo_edge,
                                                  data.edges[pseudo_edge]);
                prefix_length += data.edges[pseudo_edge].length_m;
                root_nodes_source.push_back(previous);
            }

            // Ban re-entry into every node the root has visited (including
            // the spur node itself) plus the current path's diverging edge,
            // so the spur explores genuinely new continuations.
            spur_overlay.banEdgesInto(root_nodes_source, data);
            spur_overlay.banEdgesInto({pseudo_edge}, data);
            if (r < chain.size()) {
                spur_overlay.ban(chain[r]);
            }

            SearchQuery spur_query;
            spur_query.algorithm = Algorithm::kDijkstra;
            spur_query.overlay = &spur_overlay.overlay;
            spur_query.goals = base_query.goals;
            SearchEndpoint pseudo_start;
            pseudo_start.node = data.edges[pseudo_edge].to;
            pseudo_start.edge = pseudo_edge;
            pseudo_start.offset_s = 0.0;
            pseudo_start.extra_cost_s = prefix_cost;
            pseudo_start.extra_length_m = prefix_length;
            spur_query.starts.push_back(pseudo_start);

            ++searches;
            const SearchOutput spur =
                runDijkstra(runtime, spur_query, max_speed_mps, turn_aware);
            result.total_expanded_nodes += spur.expanded_nodes;
            if (!spur.found) {
                continue;
            }

            KShortestPath candidate;
            candidate.start_index = current.start_index;
            candidate.goal_index = spur.goal_index;
            candidate.middle_edges.assign(chain.begin() + 1,
                                          chain.begin() + static_cast<std::ptrdiff_t>(r));
            candidate.middle_edges.insert(candidate.middle_edges.end(),
                                          spur.node_edges.begin(), spur.node_edges.end());
            candidate.total_time_s = spur.total_time_s;
            candidate.expanded_nodes = spur.expanded_nodes;

            const PathKey candidate_key = keyOf(candidate);
            if (accepted.count(candidate_key) != 0) {
                continue;
            }
            pool.insert({candidate.total_time_s, std::move(candidate)});
        }

        if (pool.empty()) {
            break;
        }
        const BEntry best = *pool.begin();
        pool.erase(pool.begin());
        accepted.insert(keyOf(best.path));
        result.paths.push_back(best.path);
    }

    return result;
}

}  // namespace zeus::routing
