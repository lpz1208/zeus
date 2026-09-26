#include "zeus/routing/incremental_search.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace zeus::routing {
namespace {
constexpr double infinity = std::numeric_limits<double>::infinity();
double metricDistance(zeus::map::Point2d a, zeus::map::Point2d b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}
bool sameGoals(const SearchQuery& a, const SearchQuery& b) {
    if (a.goals.size() != b.goals.size()) return false;
    for (std::size_t i = 0; i < a.goals.size(); ++i) {
        if (a.goals[i].edge != b.goals[i].edge || a.goals[i].node != b.goals[i].node ||
            a.goals[i].offset_s != b.goals[i].offset_s) return false;
    }
    return true;
}
}

IncrementalSearch::IncrementalSearch(const zeus::map::MapRuntime& runtime)
    : runtime_(runtime), incoming_(buildIncomingAdjacency(runtime.data())),
      g_(runtime.data().edges.size() + 1, infinity), rhs_(g_),
      factors_(runtime.data().edges.size(), 1), enabled_(factors_.size(), 1),
      queued_keys_(g_.size(), {infinity, infinity}),
      source_(static_cast<State>(factors_.size())) {
    double scale = infinity;
    for (const auto& edge : runtime.data().edges) {
        if (!std::isfinite(edgeCostSeconds(edge)) || edgeCostSeconds(edge) <= 0)
            throw std::invalid_argument("incremental search requires positive finite traversal costs");
        const double span = metricDistance(runtime.data().nodes[edge.from].point,
                                     runtime.data().nodes[edge.to].point);
        if (!std::isfinite(span)) scale = 0;
        else if (span > 0) scale = std::min(scale, edgeCostSeconds(edge) / span);
    }
    // Derive the bound from actual costs, including maps whose stored lengths
    // differ from projected endpoint distances. Overlays may only increase costs.
    heuristic_scale_ = std::isfinite(scale) ? scale * (1 - 1e-12) : 0;
}

IncrementalSearch::Key IncrementalSearch::key(State state) const {
    const double value = std::min(g_[state], rhs_[state]);
    double h = 0;
    if (query_.algorithm == Algorithm::kDStarLite && state != source_ && heuristic_scale_ > 0)
        h = std::max(0.0, metricDistance(anchor_, runtime_.data().nodes[runtime_.edge(state).to].point) - radius_) * heuristic_scale_;
    return {value + h + km_, value};
}
void IncrementalSearch::enqueue(State state) {
    queued_keys_[state] = key(state);
    queue_.insert({queued_keys_[state], state});
}
double IncrementalSearch::transition(State from, State to) const {
    if (!enabled_[to]) return infinity;
    return runtime_.turnPenaltySeconds(from, to) + edgeCostSeconds(runtime_.edge(to)) * factors_[to];
}
double IncrementalSearch::terminal(State state, std::size_t index) const {
    const auto& goal = query_.goals[index];
    if (!enabled_[goal.edge] || runtime_.edge(state).to != goal.node) return infinity;
    return runtime_.turnPenaltySeconds(state, goal.edge) + goal.extra_cost_s;
}
void IncrementalSearch::update(State state) {
    // Movement changes calculated priorities, but retained entries still carry
    // old keys. Remove by the stored key to keep exactly one entry per state.
    queue_.erase({queued_keys_[state], state});
    double best = infinity;
    if (state == source_) {
        for (const auto& start : query_.starts)
            best = std::min(best, start.extra_cost_s + g_[start.edge]);
    } else {
        for (std::size_t i = 0; i < query_.goals.size(); ++i) best = std::min(best, terminal(state, i));
        for (const auto successor : runtime_.outgoingEdges(runtime_.edge(state).to))
            best = std::min(best, transition(state, successor) + g_[successor]);
    }
    rhs_[state] = best;
    if (g_[state] != rhs_[state]) enqueue(state);
}
void IncrementalSearch::updatePredecessors(State state) {
    if (state == source_) return;
    const auto node = runtime_.edge(state).from;
    for (auto i = incoming_.offsets[node]; i < incoming_.offsets[node + 1]; ++i)
        update(incoming_.edges[i]);
    for (const auto& start : query_.starts) {
        if (start.edge == state) { update(source_); break; }
    }
}

SearchOutput IncrementalSearch::run(const SearchQuery& query) {
    SearchOutput output;
    if (!isIncremental(query.algorithm)) throw std::invalid_argument("invalid incremental algorithm");
    const auto count = enabled_.size();
    for (const auto& start : query.starts) {
        if (start.edge >= count || runtime_.edge(start.edge).to != start.node ||
            !std::isfinite(start.extra_cost_s) || start.extra_cost_s < 0)
            throw std::invalid_argument("invalid incremental start");
    }
    for (const auto& goal : query.goals) {
        if (goal.edge >= count || runtime_.edge(goal.edge).from != goal.node ||
            !std::isfinite(goal.extra_cost_s) || goal.extra_cost_s < 0)
            throw std::invalid_argument("invalid incremental goal");
    }
    if (query.overlay && ((!query.overlay->edge_enabled.empty() && query.overlay->edge_enabled.size() != count) ||
                          (!query.overlay->edge_cost_factors.empty() && query.overlay->edge_cost_factors.size() != count)))
        throw std::invalid_argument("invalid incremental overlay size");
    // Validate before mutating retained state.
    if (query.overlay) for (const auto factor : query.overlay->edge_cost_factors)
        if (!std::isfinite(factor) || factor < 1) throw std::invalid_argument("invalid incremental cost factor");
    output.incremental_reused = initialized_ && query_.algorithm == query.algorithm && sameGoals(query_, query);
    if (!output.incremental_reused) {
        std::fill(g_.begin(), g_.end(), infinity);
        std::fill(rhs_.begin(), rhs_.end(), infinity);
        queue_.clear();
        km_ = 0;
    }
    if (query.algorithm == Algorithm::kDStarLite && heuristic_scale_ > 0) {
        const auto anchor = query.starts.empty() ? zeus::map::Point2d{} : runtime_.data().nodes[query.starts.front().node].point;
        double radius = 0;
        for (const auto& start : query.starts)
            radius = std::max(radius, metricDistance(anchor, runtime_.data().nodes[start.node].point));
        // h(s) = scale * max(0, metricDistance(anchor,s)-radius). The ball contains
        // every start option, so h=0 on each partial-edge successor and remains
        // consistent on virtual-source arcs, including twin/multi-source snaps.
        // This km increment bounds any decrease in h after the origin moves;
        // old queue keys remain lower bounds and can be refreshed lazily.
        if (output.incremental_reused)
            km_ += (metricDistance(anchor_, anchor) + std::abs(radius_ - radius)) * heuristic_scale_;
        anchor_ = anchor;
        radius_ = radius;
    }
    query_ = query;
    query_.overlay = nullptr;  // No borrowed per-request storage survives the call.
    std::vector<State> changes;
    for (State edge = 0; edge < count; ++edge) {
        const auto enabled = query.overlay == nullptr || query.overlay->edgeEnabled(edge);
        const double factor = query.overlay == nullptr ? 1 : query.overlay->edgeCostFactor(edge);
        if (enabled_[edge] != enabled || factors_[edge] != factor) changes.push_back(edge);
        enabled_[edge] = enabled;
        factors_[edge] = factor;
    }
    output.updated_edges = changes.size();
    // A cost of entering v changes the rhs of predecessor incoming-edge states.
    // Batch all overlay writes before updating those rhs values.
    for (const auto edge : changes) updatePredecessors(edge);
    for (const auto& goal : query_.goals) {
        for (auto i = incoming_.offsets[goal.node]; i < incoming_.offsets[goal.node + 1]; ++i)
            update(incoming_.edges[i]);
    }
    update(source_);
    initialized_ = true;
    while (!queue_.empty() && (queue_.begin()->first <= key(source_) || rhs_[source_] != g_[source_])) {
        const auto [value, state] = *queue_.begin();
        queue_.erase(queue_.begin());
        if (value < key(state)) {
            enqueue(state);
            continue;
        }
        if (g_[state] > rhs_[state]) {
            g_[state] = rhs_[state];
        } else {
            g_[state] = infinity;
            update(state);
        }
        if (state != source_) {
            ++output.expanded_nodes;
            if (query.record_trace && output.trace.size() < 20000)
                output.trace.push_back({static_cast<std::uint32_t>(output.expanded_nodes), runtime_.edge(state).to, value.first, value.second});
        }
        updatePredecessors(state);
    }
    if (!std::isfinite(g_[source_])) return output;
    double best = infinity;
    for (std::size_t i = 0; i < query_.starts.size(); ++i) {
        const auto& start = query_.starts[i];
        const double value = start.extra_cost_s + g_[start.edge];
        if (value < best) { best = value; output.start_index = i; }
    }
    // Find a simple path through optimal transitions. The visited set handles
    // floating-point ties without changing the computed shortest-path labels.
    struct Frame { State state; std::vector<State> successors; std::size_t cursor = 0; };
    const auto frame = [&](State state) {
        Frame result{state, {}, 0};
        for (const auto next : runtime_.outgoingEdges(runtime_.edge(state).to))
            if (std::isfinite(g_[next]) && transition(state, next) + g_[next] == g_[state])
                result.successors.push_back(next);
        return result;
    };
    std::vector<std::uint8_t> visited(count, 0);
    const auto initial = query_.starts[output.start_index].edge;
    std::vector<Frame> stack{frame(initial)};
    visited[initial] = 1;
    while (!stack.empty()) {
        auto& top = stack.back();
        for (std::size_t i = 0; i < query_.goals.size(); ++i) {
            if (terminal(top.state, i) == g_[top.state]) {
                output.goal_index = i;
                output.total_time_s = best;
                output.found = true;
                for (std::size_t j = 1; j < stack.size(); ++j) output.node_edges.push_back(stack[j].state);
                return output;
            }
        }
        if (top.cursor == top.successors.size()) { stack.pop_back(); continue; }
        const auto next = top.successors[top.cursor++];
        if (visited[next]) continue;
        visited[next] = 1;
        stack.push_back(frame(next));
    }
    throw std::logic_error("incremental labels have no matching path");
}
}  // namespace zeus::routing
