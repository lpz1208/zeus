#include "zeus/routing/sequence_search.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace zeus::routing {
SearchOutput runSequenceSearch(const zeus::map::MapRuntime& runtime, const SearchQuery& query,
    const RouteRequest& request, const SpeedSchedule& schedule, double known_best_time_s) {
    constexpr auto none = zeus::map::kInvalidEdge;
    constexpr double inf = std::numeric_limits<double>::infinity();
    SequenceGraph graph(runtime);
    std::vector<double> best(graph.size(), inf);
    std::vector<std::uint32_t> parent(graph.size(), none);
    std::vector<std::size_t> starts(graph.size(), 0);
    using Entry = std::pair<double, std::uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    const auto duration = [&](auto edge, double length, double elapsed) {
        const auto factor = query.overlay ? query.overlay->edgeCostFactor(edge) : 1;
        return request.algorithm == Algorithm::kTimeDependent
            ? schedule.duration(runtime, edge, length, request.departure_time_s + elapsed, factor, request.cost_reference_time_s)
            : length / std::max(kMinSpeedMps, double(runtime.edge(edge).speed_limit_mps)) * factor;
    };
    for (std::size_t i = 0; i < query.starts.size(); ++i) {
        const auto& start = query.starts[i];
        const auto context = graph.initial(request, start.edge);
        if (context == none) continue;
        const auto state = graph.encode(start.edge, context);
        const auto value = duration(start.edge, start.extra_length_m, 0);
        if (value < best[state]) { best[state] = value; starts[state] = i; queue.emplace(value, state); }
    }
    SearchOutput output;
    double incumbent = known_best_time_s;
    auto final = none;
    while (!queue.empty()) {
        const auto [cost, state] = queue.top(); queue.pop();
        if (cost != best[state]) continue;
        if (cost >= incumbent) break;
        const auto edge = graph.edge(state), context = graph.context(state);
        ++output.expanded_nodes;
        if (query.record_trace && output.trace.size() < 20000)
            output.trace.push_back({static_cast<std::uint32_t>(output.expanded_nodes), runtime.edge(edge).to, cost, cost});
        for (std::size_t i = 0; i < query.goals.size(); ++i) {
            const auto& goal = query.goals[i];
            if (runtime.edge(edge).to != goal.node || (query.overlay && !query.overlay->edgeEnabled(goal.edge)) ||
                runtime.advanceTurnState(context, goal.edge) == none) continue;
            const auto turn = runtime.turnPenaltySeconds(edge, goal.edge);
            if (!std::isfinite(turn)) continue;
            const auto value = cost + turn + duration(goal.edge, goal.extra_length_m, cost + turn);
            if (value < incumbent) { incumbent = value; final = state; output.goal_index = i; }
        }
        for (auto next : runtime.outgoingEdges(runtime.edge(edge).to)) {
            if (query.overlay && !query.overlay->edgeEnabled(next)) continue;
            const auto turn = runtime.turnPenaltySeconds(edge, next);
            const auto next_context = runtime.advanceTurnState(context, next);
            if (!std::isfinite(turn) || next_context == none) continue;
            const auto target = graph.encode(next, next_context);
            const auto value = cost + turn + duration(next, runtime.edge(next).length_m, cost + turn);
            if (value < best[target] && value < incumbent) {
                best[target] = value; starts[target] = starts[state]; parent[target] = state;
                queue.emplace(value, target);
            }
        }
    }
    if (final == none) return output;
    output.found = true; output.total_time_s = incumbent; output.start_index = starts[final];
    for (auto state = final; parent[state] != none; state = parent[state]) output.node_edges.push_back(graph.edge(state));
    std::reverse(output.node_edges.begin(), output.node_edges.end());
    return output;
}
}  // namespace zeus::routing
