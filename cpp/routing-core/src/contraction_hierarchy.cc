#include "zeus/routing/contraction_hierarchy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>

namespace zeus::routing {
namespace {
using State = zeus::map::EdgeIndex;
constexpr auto invalid = std::numeric_limits<std::uint32_t>::max();
constexpr double inf = std::numeric_limits<double>::infinity();
using Entry = std::pair<double, State>;
using Queue = std::priority_queue<Entry, std::vector<Entry>, std::greater<>>;
}

bool ContractionHierarchy::baseWeights(const RoutingOverlay* overlay) {
    return !overlay ||
        (std::all_of(overlay->edge_enabled.begin(), overlay->edge_enabled.end(), [](auto v) { return v != 0; }) &&
         std::all_of(overlay->edge_cost_factors.begin(), overlay->edge_cost_factors.end(), [](auto v) { return v == 1; }));
}

ContractionHierarchy::ContractionHierarchy(const zeus::map::MapRuntime& runtime)
    : ContractionHierarchy(runtime, Limits{}) {}

ContractionHierarchy::ContractionHierarchy(const zeus::map::MapRuntime& runtime, Limits limits)
    : runtime_(runtime), turn_aware_(runtime.hasTurnTransitions()) {
    const auto n = turn_aware_ ? runtime.data().edges.size() : runtime.data().nodes.size();
    if (n > limits.max_states || n >= invalid) return;
    limits.max_arcs = std::min(limits.max_arcs, static_cast<std::size_t>(invalid - 1));
    // Base turn expansion itself must fit. Never publish an incomplete graph.
    std::size_t potential_arcs = turn_aware_ ? 0 : runtime.data().edges.size();
    if (potential_arcs > limits.max_arcs) return;
    if (turn_aware_) for (const auto& edge : runtime.data().edges) {
        const auto degree = runtime.outgoingEdges(edge.to).size();
        if (degree > limits.max_arcs - potential_arcs) return;
        potential_arcs += degree;
    }
    rank_.assign(n, static_cast<std::uint32_t>(n));
    out_.resize(n); in_.resize(n);
    const auto add = [&](const Arc& arc) {
        // Keep only the cheapest active arc for each ordered pair. Older arc
        // records stay immutable because existing shortcuts may reference
        // them, but dominated parallel arcs must not inflate later products.
        for (auto& previous : out_[arc.from]) {
            if (arcs_[previous].to != arc.to) continue;
            if (arcs_[previous].cost <= arc.cost) return -1;
            const auto old = previous;
            const auto id = static_cast<std::uint32_t>(arcs_.size());
            arcs_.push_back(arc);
            previous = id;
            for (auto& reverse : in_[arc.to]) if (reverse == old) { reverse = id; break; }
            return 0;
        }
        const auto id = static_cast<std::uint32_t>(arcs_.size());
        arcs_.push_back(arc); out_[arc.from].push_back(id); in_[arc.to].push_back(id);
        return 1;
    };
    if (!turn_aware_) {
        for (State edge = 0; edge < runtime.data().edges.size(); ++edge) {
            const auto& road = runtime.edge(edge);
            if (road.from != road.to) add({road.from, road.to, edgeCostSeconds(road), invalid, edge});
        }
    } else for (State from = 0; from < n; ++from) {
        for (const auto to : runtime.outgoingEdges(runtime.edge(from).to)) {
            if (from == to) continue; // Positive self transitions never improve a path.
            const double cost = runtime.turnPenaltySeconds(from, to) + edgeCostSeconds(runtime.edge(to));
            if (std::isfinite(cost)) add({from, to, cost, invalid, to});
        }
    }
    available_ = true;
    core_states_ = n;
    std::vector<std::uint64_t> in_degree(n), out_degree(n), level(n);
    std::vector<std::int64_t> priority(n);
    // Cheap upper estimate of edge difference. Give fill substantially more
    // weight than depth so junctions survive while corridors are contracted.
    const auto score = [&](State v) {
        return 14 * (static_cast<std::int64_t>(in_degree[v] * out_degree[v]) -
                     static_cast<std::int64_t>(in_degree[v] + out_degree[v])) +
               static_cast<std::int64_t>(level[v]);
    };
    std::set<std::pair<std::int64_t, State>> order;
    for (State v = 0; v < n; ++v) {
        in_degree[v] = in_[v].size(); out_degree[v] = out_[v].size();
        priority[v] = score(v); order.emplace(priority[v], v);
    }
    // Stamps avoid clearing an O(E) distance vector for each witness source.
    std::vector<double> distance(n, inf);
    std::vector<std::uint64_t> stamp(n, 0);
    std::uint64_t epoch = 0, work = potential_arcs;
    for (std::uint32_t rank = 0; !order.empty() && work < limits.max_work; ++rank) {
        const State v = order.begin()->second;
        std::vector<std::uint32_t> entering, leaving;
        for (auto id : in_[v]) { ++work; if (rank_[arcs_[id].from] == n) entering.push_back(id); }
        for (auto id : out_[v]) { ++work; if (rank_[arcs_[id].to] == n) leaving.push_back(id); }
        // Bound the local Cartesian product as well as total witness work.
        if (work >= limits.max_work || (leaving.size() && entering.size() > 65536 / leaving.size())) break;
        std::vector<Arc> pending;
        bool budget = false;
        for (auto first : entering) {
            const auto incoming = arcs_[first];
            double cutoff = 0;
            for (auto second : leaving) cutoff = std::max(cutoff, incoming.cost + arcs_[second].cost);
            ++epoch;
            Queue queue;
            distance[incoming.from] = 0; stamp[incoming.from] = epoch; queue.emplace(0, incoming.from);
            std::size_t settled = 0;
            while (!queue.empty() && settled < limits.witness_settles && work < limits.max_work) {
                const auto [cost, at] = queue.top(); queue.pop(); ++work;
                if (cost != distance[at]) continue;
                if (cost > cutoff) break;
                ++settled;
                for (auto id : out_[at]) {
                    ++work;
                    const auto& arc = arcs_[id];
                    if (arc.to == v || rank_[arc.to] != n) continue;
                    const double next = cost + arc.cost;
                    if (next <= cutoff && (stamp[arc.to] != epoch || next < distance[arc.to])) {
                        stamp[arc.to] = epoch; distance[arc.to] = next; queue.emplace(next, arc.to);
                    }
                }
            }
            for (auto second : leaving) {
                ++work;
                const auto outgoing = arcs_[second];
                const double cost = incoming.cost + outgoing.cost;
                if (incoming.from == outgoing.to) continue;
                // A discovered path is a valid witness even before settling.
                // No positive epsilon: skipping a slightly better shortcut
                // would sacrifice exactness on nearly tied paths.
                if (stamp[outgoing.to] == epoch && distance[outgoing.to] <= cost) continue;
                pending.push_back({incoming.from, outgoing.to, cost, first, second});
                if (arcs_.size() + pending.size() > limits.max_arcs) { budget = true; break; }
            }
            if (budget || work >= limits.max_work) { budget = true; break; }
        }
        // All-or-nothing contraction. A budget stop leaves v in the core and
        // preserves every path; no incomplete shortcut set is ever published.
        if (budget) break;
        order.erase(order.begin());
        rank_[v] = rank; --core_states_;
        std::vector<State> neighbors;
        for (auto id : entering) { const auto u = arcs_[id].from; --out_degree[u]; neighbors.push_back(u); }
        for (auto id : leaving) { const auto u = arcs_[id].to; --in_degree[u]; neighbors.push_back(u); }
        for (const auto& arc : pending) {
            const int added = add(arc);
            if (added >= 0) ++shortcuts_;
            if (added > 0) { ++out_degree[arc.from]; ++in_degree[arc.to]; }
        }
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
        for (auto u : neighbors) {
            order.erase({priority[u], u});
            level[u] = std::max(level[u], level[v] + 1);
            priority[u] = score(u); order.emplace(priority[u], u);
        }
    }
}

std::size_t ContractionHierarchy::bytes() const {
    std::size_t size = arcs_.capacity() * sizeof(Arc) + rank_.capacity() * sizeof(std::uint32_t) +
        (out_.capacity() + in_.capacity()) * sizeof(std::vector<std::uint32_t>);
    for (const auto& ids : out_) size += ids.capacity() * sizeof(std::uint32_t);
    for (const auto& ids : in_) size += ids.capacity() * sizeof(std::uint32_t);
    return size;
}

SearchOutput ContractionHierarchy::search(const SearchQuery& query, const IncomingAdjacency& incoming,
                                          double known_best_time_s) const {
    if (!available_) throw std::logic_error("CH index unavailable");
    if (!baseWeights(query.overlay)) throw std::invalid_argument("CH index requires base weights");
    const auto n = rank_.size();
    std::vector<double> forward(n, inf), backward(n, inf);
    std::vector<std::uint32_t> parent(n, invalid), successor(n, invalid);
    std::vector<std::size_t> start(n, 0), goal(n, 0);
    Queue qf, qb;
    for (std::size_t i = 0; i < query.starts.size(); ++i) {
        const auto& s = query.starts[i];
        const State at = turn_aware_ ? s.edge : s.node;
        if (s.extra_cost_s < forward[at]) {
            forward[at] = s.extra_cost_s; start[at] = i; qf.emplace(s.extra_cost_s, at);
        }
    }
    for (std::size_t i = 0; i < query.goals.size(); ++i) {
        const auto& g = query.goals[i];
        if (!turn_aware_) {
            if (g.extra_cost_s < backward[g.node]) {
                backward[g.node] = g.extra_cost_s; goal[g.node] = i; qb.emplace(g.extra_cost_s, g.node);
            }
            continue;
        }
        for (auto j = incoming.offsets[g.node]; j < incoming.offsets[g.node + 1]; ++j) {
            const State edge = incoming.edges[j];
            const double cost = runtime_.turnPenaltySeconds(edge, g.edge) + g.extra_cost_s;
            if (cost < backward[edge]) { backward[edge] = cost; goal[edge] = i; qb.emplace(cost, edge); }
        }
    }
    SearchOutput output;
    double best = known_best_time_s;
    State meeting = invalid;
    const auto consider = [&](State v) {
        if (forward[v] + backward[v] < best) { best = forward[v] + backward[v]; meeting = v; }
    };
    const auto clean = [](Queue& queue, const std::vector<double>& distance) {
        while (!queue.empty() && queue.top().first != distance[queue.top().second]) queue.pop();
    };
    while (true) {
        clean(qf, forward); clean(qb, backward);
        const double f = qf.empty() ? inf : qf.top().first;
        const double b = qb.empty() ? inf : qb.top().first;
        // CH frontiers search different upward graphs. The usual sum-of-minima
        // stop is unsound: each side must independently reach the incumbent.
        if (f >= best && b >= best) break;
        const bool reverse = b < f;
        auto& queue = reverse ? qb : qf;
        auto& distance = reverse ? backward : forward;
        const auto [cost, at] = queue.top(); queue.pop();
        ++output.expanded_nodes;
        if (query.record_trace && output.trace.size() < 20000)
            output.trace.push_back({static_cast<std::uint32_t>(output.expanded_nodes), turn_aware_ ? runtime_.edge(at).to : at, cost, cost});
        consider(at);
        for (auto id : reverse ? in_[at] : out_[at]) {
            const auto& arc = arcs_[id];
            const State next = reverse ? arc.from : arc.to;
            if (rank_[next] < rank_[at]) continue; // Equal ranks are the uncontracted core.
            const double value = cost + arc.cost;
            if (value < distance[next] && value < best) {
                distance[next] = value;
                if (reverse) { successor[next] = id; goal[next] = goal[at]; }
                else { parent[next] = id; start[next] = start[at]; }
                queue.emplace(value, next); consider(next);
            }
        }
    }
    if (meeting == invalid) return output;
    output.found = true; output.total_time_s = best;
    output.start_index = start[meeting]; output.goal_index = goal[meeting];
    std::vector<std::uint32_t> path;
    for (State at = meeting; parent[at] != invalid; at = arcs_[parent[at]].from) path.push_back(parent[at]);
    std::reverse(path.begin(), path.end());
    for (State at = meeting; successor[at] != invalid; at = arcs_[successor[at]].to) path.push_back(successor[at]);
    // Iterative unpacking avoids recursion depth proportional to shortcut nesting.
    std::vector<std::uint32_t> stack;
    for (auto id : path) {
        stack.push_back(id);
        while (!stack.empty()) {
            const auto& arc = arcs_[stack.back()]; stack.pop_back();
            if (arc.left == invalid) output.node_edges.push_back(arc.right);
            else { stack.push_back(arc.right); stack.push_back(arc.left); }
        }
    }
    return output;
}
}  // namespace zeus::routing
