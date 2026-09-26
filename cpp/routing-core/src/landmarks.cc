#include "zeus/routing/landmarks.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <tuple>

namespace zeus::routing {
namespace {
constexpr double inf = std::numeric_limits<double>::infinity();
using Node = zeus::map::NodeIndex;
using Edge = zeus::map::EdgeIndex;
std::vector<double> distances(const zeus::map::MapRuntime& runtime,
    const IncomingAdjacency& incoming, Node landmark, bool reverse) {
    std::vector<double> result(runtime.data().nodes.size(), inf);
    using Entry = std::pair<double, Node>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    result[landmark] = 0;
    queue.emplace(0, landmark);
    while (!queue.empty()) {
        const auto [cost, node] = queue.top(); queue.pop();
        if (cost != result[node]) continue;
        const auto relax = [&](Edge id) {
            const auto& edge = runtime.edge(id);
            const auto next = reverse ? edge.from : edge.to;
            if (next >= result.size()) return;
            const double value = cost + edgeCostSeconds(edge);
            if (value < result[next]) { result[next] = value; queue.emplace(value, next); }
        };
        if (reverse) {
            for (auto i = incoming.offsets[node]; i < incoming.offsets[node + 1]; ++i) relax(incoming.edges[i]);
        } else for (const auto edge : runtime.outgoingEdges(node)) relax(edge);
    }
    return result;
}
// Guard cancellation/accumulation roundoff in differences of long distances.
// ALT search permits reopening, so rounding need not preserve consistency.
double difference(double a, double b) {
    return std::max(0.0, a - b - 1e-10 * std::max({1.0, a, b}));
}
}

LandmarkIndex::LandmarkIndex(const zeus::map::MapRuntime& runtime, const IncomingAdjacency& incoming,
    std::size_t requested, std::size_t budget) : runtime_(runtime), node_count_(runtime.data().nodes.size()) {
    if (node_count_ == 0) return;
    const auto limit = std::min({requested, std::size_t(16), node_count_, budget / sizeof(double) / 2 / node_count_});
    if (limit == 0) return;
    for (const auto& edge : runtime.data().edges)
        if (!std::isfinite(edgeCostSeconds(edge)) || edgeCostSeconds(edge) <= 0)
            throw std::invalid_argument("ALT requires positive finite base costs");
    std::vector<double> nearest(node_count_, inf);
    std::vector<std::uint8_t> chosen(node_count_, 0);
    for (std::size_t i = 0; i < limit; ++i) {
        Node next = zeus::map::kInvalidNode;
        for (Node node = 0; node < node_count_; ++node) {
            if (chosen[node] || (runtime.outgoingEdges(node).empty() && incoming.offsets[node] == incoming.offsets[node + 1])) continue;
            if (next == zeus::map::kInvalidNode || nearest[node] > nearest[next]) next = node;
        }
        if (next == zeus::map::kInvalidNode) break;
        chosen[next] = 1;
        landmarks_.push_back(next);
        from_landmark_.push_back(distances(runtime, incoming, next, false));
        to_landmark_.push_back(distances(runtime, incoming, next, true));
        // Deterministic farthest-point sampling. Disconnected components have
        // infinite distance and are selected before nearer covered nodes.
        for (Node node = 0; node < node_count_; ++node)
            nearest[node] = std::min({nearest[node], from_landmark_.back()[node], to_landmark_.back()[node]});
    }
}

double LandmarkIndex::lowerBound(Node from, Node to) const {
    if (from >= node_count_ || to >= node_count_) throw std::invalid_argument("ALT node outside map");
    double bound = 0;
    for (std::size_t i = 0; i < count(); ++i) {
        const double lf = from_landmark_[i][from], lt = from_landmark_[i][to];
        const double fl = to_landmark_[i][from], tl = to_landmark_[i][to];
        // A finite-to-infinite reachability mismatch proves from cannot reach
        // to; never subtract infinities or silently turn that proof into zero.
        if ((std::isfinite(lf) && !std::isfinite(lt)) || (!std::isfinite(fl) && std::isfinite(tl))) return inf;
        if (std::isfinite(lf) && std::isfinite(lt)) bound = std::max(bound, difference(lt, lf));
        if (std::isfinite(fl) && std::isfinite(tl)) bound = std::max(bound, difference(fl, tl));
    }
    return bound;
}

SearchOutput runAltSearch(const zeus::map::MapRuntime& runtime, const SearchQuery& query,
    const LandmarkIndex& landmarks, double known_best_time_s) {
    if (&landmarks.runtime() != &runtime) throw std::invalid_argument("ALT index belongs to another map");
    const auto count = runtime.data().edges.size();
    std::vector<double> best(count, inf);
    std::vector<Edge> parent(count, zeus::map::kInvalidEdge);
    std::vector<std::size_t> start_of(count, 0);
    // Cache only heuristic evaluations made during this query; no O(L*V)
    // target potential scan. Labels already require O(E) per-query space.
    std::vector<double> heuristic(runtime.data().nodes.size(), -1);
    const auto estimate = [&](Node node) {
        if (heuristic[node] >= 0) return heuristic[node];
        double bound = inf;
        for (const auto& goal : query.goals)
            bound = std::min(bound, landmarks.lowerBound(node, goal.node) + goal.extra_cost_s);
        return heuristic[node] = bound;
    };
    using Entry = std::tuple<double, double, Edge>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    for (std::size_t i = 0; i < query.starts.size(); ++i) {
        const auto& start = query.starts[i];
        if (start.extra_cost_s < best[start.edge]) {
            best[start.edge] = start.extra_cost_s;
            start_of[start.edge] = i;
            queue.emplace(best[start.edge] + estimate(start.node), best[start.edge], start.edge);
        }
    }
    SearchOutput output;
    double incumbent = known_best_time_s;
    Edge final = zeus::map::kInvalidEdge;
    while (!queue.empty()) {
        const auto [f, cost, edge] = queue.top(); queue.pop();
        if (cost != best[edge]) continue;
        if (f >= incumbent) break;
        ++output.expanded_nodes;
        if (query.record_trace && output.trace.size() < 20000)
            output.trace.push_back({static_cast<std::uint32_t>(output.expanded_nodes), runtime.edge(edge).to, f, cost});
        for (std::size_t i = 0; i < query.goals.size(); ++i) {
            const auto& goal = query.goals[i];
            if (runtime.edge(edge).to != goal.node || (query.overlay && !query.overlay->edgeEnabled(goal.edge))) continue;
            const double value = cost + runtime.turnPenaltySeconds(edge, goal.edge) + goal.extra_cost_s;
            if (value < incumbent) { incumbent = value; final = edge; output.goal_index = i; }
        }
        for (const auto next : runtime.outgoingEdges(runtime.edge(edge).to)) {
            if (query.overlay && !query.overlay->edgeEnabled(next)) continue;
            const double value = cost + runtime.turnPenaltySeconds(edge, next) + edgeCostSeconds(runtime.edge(next)) *
                (query.overlay ? query.overlay->edgeCostFactor(next) : 1);
            if (value < best[next] && value < incumbent) {
                best[next] = value;
                parent[next] = edge;
                start_of[next] = start_of[edge];
                queue.emplace(value + estimate(runtime.edge(next).to), value, next);
            }
        }
    }
    if (final == zeus::map::kInvalidEdge) return output;
    output.found = true;
    output.total_time_s = incumbent;
    output.start_index = start_of[final];
    for (Edge at = final; parent[at] != zeus::map::kInvalidEdge; at = parent[at]) output.node_edges.push_back(at);
    std::reverse(output.node_edges.begin(), output.node_edges.end());
    return output;
}
}  // namespace zeus::routing
