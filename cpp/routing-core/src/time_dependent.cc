#include "zeus/routing/time_dependent.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>

namespace zeus::routing {
SpeedSchedule::SpeedSchedule(std::size_t edge_count, std::span<const SpeedChange> changes) {
    if (changes.size() > 100000) throw std::invalid_argument("too many speed changes");
    for (const auto& change : changes) {
        if (change.edge >= edge_count || !std::isfinite(change.time_s) || change.time_s < 0 ||
            !std::isfinite(change.factor) || change.factor < .05 || change.factor > 3)
            throw std::invalid_argument("invalid scheduled speed change");
        profiles_[change.edge].push_back(change);
    }
    for (auto& [edge, profile] : profiles_)
        std::stable_sort(profile.begin(), profile.end(),
            [](const auto& a, const auto& b) { return a.time_s < b.time_s; });
}

double SpeedSchedule::duration(const zeus::map::MapRuntime& runtime,
    zeus::map::EdgeIndex edge, double length, double departure, double overlay, double reference) const {
    const double base_speed = std::max(kMinSpeedMps, double(runtime.edge(edge).speed_limit_mps));
    const auto found = profiles_.find(edge);
    if (found == profiles_.end()) return length / base_speed * overlay;
    const auto& profile = found->second;
    const auto upper = [&](double time) {
        return std::upper_bound(profile.begin(), profile.end(), time,
            [](double value, const SpeedChange& item) { return value < item.time_s; });
    };
    const auto current = upper(reference);
    const double current_factor = current == profile.begin() ? 1 : std::prev(current)->factor;
    // The published overlay already includes the current speed penalty. Keep
    // its density/capacity component fixed, replacing only scheduled speed.
    const double residual = std::max(1.0, overlay / std::max(1.0, 1.0 / current_factor));
    auto next = upper(departure);
    double factor = next == profile.begin() ? 1 : std::prev(next)->factor;
    double elapsed = 0;
    while (next != profile.end()) {
        const double interval = next->time_s - departure;
        const double speed = base_speed * factor / residual;
        if (length <= interval * speed) return elapsed + length / speed;
        length -= interval * speed;
        elapsed += interval;
        departure = next->time_s;
        factor = next->factor;
        ++next;
    }
    return elapsed + length / (base_speed * factor / residual);
}

SearchOutput runTimeDependentSearch(const zeus::map::MapRuntime& runtime,
    const SearchQuery& query, const RouteRequest& request, const SpeedSchedule& schedule,
    double known_best_time_s) {
    using Edge = zeus::map::EdgeIndex;
    constexpr double inf = std::numeric_limits<double>::infinity();
    constexpr Edge none = zeus::map::kInvalidEdge;
    const auto count = runtime.data().edges.size();
    std::vector<double> best(count, inf);
    std::vector<Edge> parent(count, none);
    std::vector<std::size_t> start_index(count, 0);
    std::priority_queue<std::pair<double, Edge>, std::vector<std::pair<double, Edge>>,
                        std::greater<>> queue;
    const auto duration = [&](Edge edge, double length, double elapsed) {
        return schedule.duration(runtime, edge, length, request.departure_time_s + elapsed,
            query.overlay ? query.overlay->edgeCostFactor(edge) : 1, request.cost_reference_time_s);
    };
    for (std::size_t i = 0; i < query.starts.size(); ++i) {
        const auto& start = query.starts[i];
        const double value = duration(start.edge, start.extra_length_m, 0);
        if (value < best[start.edge]) {
            best[start.edge] = value;
            start_index[start.edge] = i;
            queue.emplace(value, start.edge);
        }
    }
    SearchOutput output;
    double incumbent = known_best_time_s;
    Edge final = none;
    while (!queue.empty()) {
        const auto [value, edge] = queue.top(); queue.pop();
        if (value != best[edge]) continue;
        if (value >= incumbent) break;
        ++output.expanded_nodes;
        if (query.record_trace && output.trace.size() < 20000)
            output.trace.push_back({static_cast<std::uint32_t>(output.expanded_nodes),
                runtime.edge(edge).to, value, value});
        for (std::size_t i = 0; i < query.goals.size(); ++i) {
            const auto& goal = query.goals[i];
            if (runtime.edge(edge).to != goal.node || (query.overlay && !query.overlay->edgeEnabled(goal.edge))) continue;
            const double turn = runtime.turnPenaltySeconds(edge, goal.edge);
            if (!std::isfinite(turn)) continue;
            const double arrival = value + turn + duration(goal.edge, goal.extra_length_m, value + turn);
            if (arrival < incumbent) {
                incumbent = arrival;
                final = edge;
                output.goal_index = i;
            }
        }
        for (const auto next : runtime.outgoingEdges(runtime.edge(edge).to)) {
            if (query.overlay && !query.overlay->edgeEnabled(next)) continue;
            const double turn = runtime.turnPenaltySeconds(edge, next);
            if (!std::isfinite(turn)) continue;
            const double arrival = value + turn + duration(next, runtime.edge(next).length_m, value + turn);
            if (arrival < best[next] && arrival < incumbent) {
                best[next] = arrival;
                parent[next] = edge;
                start_index[next] = start_index[edge];
                queue.emplace(arrival, next);
            }
        }
    }
    if (final == none) return output;
    output.found = true;
    output.total_time_s = incumbent;
    output.start_index = start_index[final];
    for (Edge at = final; parent[at] != none; at = parent[at]) output.node_edges.push_back(at);
    std::reverse(output.node_edges.begin(), output.node_edges.end());
    return output;
}
}  // namespace zeus::routing
