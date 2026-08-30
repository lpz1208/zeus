#include "zeus/routing/search.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

namespace zeus::routing {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

bool edgeEnabled(const SearchQuery& query, zeus::map::EdgeIndex edge) {
    return query.overlay == nullptr || query.overlay->edgeEnabled(edge);
}

double edgeCost(const SearchQuery& query, zeus::map::EdgeIndex edge_index,
                const zeus::map::DirectedEdge& edge) {
    const double factor = query.overlay == nullptr
                              ? 1.0
                              : query.overlay->edgeCostFactor(edge_index);
    return edgeCostSeconds(edge) * factor;
}

}  // namespace

SearchOutput runShortestPathSearch(
    const zeus::map::MapRuntime& runtime,
    const SearchQuery& query,
    double max_speed_mps,
    double known_best_time_s) {
    SearchOutput output;
    if (query.starts.empty() || query.goals.empty()) {
        return output;
    }

    const zeus::map::MapData& data = runtime.data();
    const std::size_t node_count = data.nodes.size();

    // Goal entry nodes carry a suffix cost (remaining part of the destination
    // edge); keep the cheapest suffix per node.
    std::vector<std::size_t> goal_at(node_count, query.goals.size());
    std::vector<double> goal_suffix(node_count, kInfinity);
    std::vector<zeus::map::Point2d> goal_points;
    goal_points.reserve(query.goals.size());
    for (std::size_t i = 0; i < query.goals.size(); ++i) {
        const SearchEndpoint& goal = query.goals[i];
        if (goal.node >= node_count) {
            continue;
        }
        if (goal.extra_cost_s < goal_suffix[goal.node]) {
            goal_suffix[goal.node] = goal.extra_cost_s;
            goal_at[goal.node] = i;
        }
        goal_points.push_back(data.nodes[goal.node].point);
    }
    if (goal_points.empty()) {
        return output;
    }

    const bool use_heuristic = query.algorithm == Algorithm::kAStar;
    const double heuristic_speed = std::max(kMinSpeedMps, max_speed_mps);
    const auto heuristic = [&](zeus::map::NodeIndex node) {
        if (!use_heuristic) {
            return 0.0;
        }
        double best = kInfinity;
        const zeus::map::Point2d& point = data.nodes[node].point;
        for (const zeus::map::Point2d& goal_point : goal_points) {
            const double dx = point.x - goal_point.x;
            const double dy = point.y - goal_point.y;
            best = std::min(best, std::sqrt(dx * dx + dy * dy) / heuristic_speed);
        }
        return best;
    };

    std::vector<double> dist(node_count, kInfinity);
    std::vector<zeus::map::EdgeIndex> pred_edge(node_count, zeus::map::kInvalidEdge);
    std::vector<std::size_t> start_of(node_count, query.starts.size());
    std::vector<std::uint8_t> closed(node_count, 0);

    using QueueEntry = std::pair<double, zeus::map::NodeIndex>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> queue;

    for (std::size_t i = 0; i < query.starts.size(); ++i) {
        const SearchEndpoint& start = query.starts[i];
        if (start.node >= node_count) {
            continue;
        }
        if (start.extra_cost_s < dist[start.node]) {
            dist[start.node] = start.extra_cost_s;
            start_of[start.node] = i;
            queue.push({start.extra_cost_s + heuristic(start.node), start.node});
        }
    }

    double best_total = known_best_time_s;
    zeus::map::NodeIndex goal_node = zeus::map::kInvalidNode;

    while (!queue.empty()) {
        const auto [f, node] = queue.top();
        queue.pop();
        if (closed[node] != 0) {
            continue;
        }
        // Every remaining entry has f >= this one, and any completion found
        // later costs at least its goal-entry f, so nothing can beat best_total.
        if (f >= best_total) {
            break;
        }
        closed[node] = 1;
        ++output.expanded_nodes;

        if (goal_at[node] != query.goals.size()) {
            const double total = dist[node] + goal_suffix[node];
            if (total < best_total) {
                best_total = total;
                goal_node = node;
                output.found = true;
                output.total_time_s = total;
                output.start_index = start_of[node];
                output.goal_index = goal_at[node];
            }
        }

        for (const zeus::map::EdgeIndex edge_index : runtime.outgoingEdges(node)) {
            if (!edgeEnabled(query, edge_index)) {
                continue;
            }
            const zeus::map::DirectedEdge& edge = runtime.edge(edge_index);
            if (edge.to >= node_count) {
                continue;
            }
            const double candidate = dist[node] + edgeCost(query, edge_index, edge);
            if (candidate >= best_total || candidate >= dist[edge.to]) {
                continue;
            }
            dist[edge.to] = candidate;
            pred_edge[edge.to] = edge_index;
            start_of[edge.to] = start_of[node];
            queue.push({candidate + heuristic(edge.to), edge.to});
        }
    }

    if (!output.found) {
        return output;
    }

    // Walk predecessor edges from the goal entry back to the chosen start.
    zeus::map::NodeIndex node = goal_node;
    const zeus::map::NodeIndex start_node = query.starts[output.start_index].node;
    while (node != start_node) {
        const zeus::map::EdgeIndex edge_index = pred_edge[node];
        if (edge_index == zeus::map::kInvalidEdge) {
            return {};
        }
        output.node_edges.push_back(edge_index);
        node = runtime.edge(edge_index).from;
    }
    std::reverse(output.node_edges.begin(), output.node_edges.end());
    return output;
}

SearchOutput runTurnAwareSearch(
    const zeus::map::MapRuntime& runtime,
    const SearchQuery& query,
    double max_speed_mps,
    double known_best_time_s) {
    SearchOutput output;
    if (query.starts.empty() || query.goals.empty()) {
        return output;
    }

    const zeus::map::MapData& data = runtime.data();
    const std::size_t node_count = data.nodes.size();
    const std::size_t edge_count = data.edges.size();
    std::vector<std::vector<std::size_t>> goals_at(node_count);
    std::vector<zeus::map::Point2d> goal_points;
    for (std::size_t i = 0; i < query.goals.size(); ++i) {
        if (query.goals[i].node < node_count && query.goals[i].edge < edge_count) {
            goals_at[query.goals[i].node].push_back(i);
            goal_points.push_back(data.nodes[query.goals[i].node].point);
        }
    }
    if (goal_points.empty()) {
        return output;
    }

    const bool use_heuristic = query.algorithm == Algorithm::kAStar ||
                               query.algorithm == Algorithm::kBidirectionalAStar;
    const double heuristic_speed = std::max(kMinSpeedMps, max_speed_mps);
    const auto heuristic = [&](zeus::map::NodeIndex node) {
        if (!use_heuristic) {
            return 0.0;
        }
        double best = kInfinity;
        const zeus::map::Point2d& point = data.nodes[node].point;
        for (const zeus::map::Point2d& goal : goal_points) {
            best = std::min(best, std::hypot(point.x - goal.x, point.y - goal.y) /
                                      heuristic_speed);
        }
        return best;
    };

    std::vector<double> dist(edge_count, kInfinity);
    std::vector<zeus::map::EdgeIndex> predecessor(edge_count, zeus::map::kInvalidEdge);
    std::vector<std::size_t> start_of(edge_count, query.starts.size());
    std::vector<std::uint8_t> closed(edge_count, 0);
    using QueueEntry = std::pair<double, zeus::map::EdgeIndex>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> queue;

    for (std::size_t i = 0; i < query.starts.size(); ++i) {
        const SearchEndpoint& start = query.starts[i];
        if (start.edge >= edge_count || start.node >= node_count ||
            data.edges[start.edge].to != start.node) {
            continue;
        }
        if (start.extra_cost_s < dist[start.edge]) {
            dist[start.edge] = start.extra_cost_s;
            start_of[start.edge] = i;
            queue.push({start.extra_cost_s + heuristic(start.node), start.edge});
        }
    }

    double best_total = known_best_time_s;
    zeus::map::EdgeIndex final_state = zeus::map::kInvalidEdge;
    while (!queue.empty()) {
        const auto [f, incoming_edge] = queue.top();
        queue.pop();
        if (closed[incoming_edge] != 0) {
            continue;
        }
        if (f >= best_total) {
            break;
        }
        closed[incoming_edge] = 1;
        ++output.expanded_nodes;
        const zeus::map::NodeIndex node = data.edges[incoming_edge].to;

        for (const std::size_t goal_index : goals_at[node]) {
            const SearchEndpoint& goal = query.goals[goal_index];
            const double turn = runtime.turnPenaltySeconds(incoming_edge, goal.edge);
            const double total = dist[incoming_edge] + turn + goal.extra_cost_s;
            if (total < best_total) {
                best_total = total;
                final_state = incoming_edge;
                output.found = true;
                output.total_time_s = total;
                output.start_index = start_of[incoming_edge];
                output.goal_index = goal_index;
            }
        }

        for (const zeus::map::EdgeIndex next : runtime.outgoingEdges(node)) {
            if (!edgeEnabled(query, next)) {
                continue;
            }
            const zeus::map::DirectedEdge& edge = runtime.edge(next);
            const double turn = runtime.turnPenaltySeconds(incoming_edge, next);
            if (!std::isfinite(turn)) {
                continue;
            }
            const double candidate = dist[incoming_edge] + turn + edgeCost(query, next, edge);
            if (candidate >= best_total || candidate >= dist[next]) {
                continue;
            }
            dist[next] = candidate;
            predecessor[next] = incoming_edge;
            start_of[next] = start_of[incoming_edge];
            queue.push({candidate + heuristic(edge.to), next});
        }
    }

    if (!output.found) {
        return output;
    }
    const zeus::map::EdgeIndex start_edge = query.starts[output.start_index].edge;
    std::size_t guard = 0;
    for (zeus::map::EdgeIndex edge = final_state; edge != start_edge;) {
        if (edge == zeus::map::kInvalidEdge || ++guard > edge_count) {
            return {};
        }
        output.node_edges.push_back(edge);
        edge = predecessor[edge];
    }
    std::reverse(output.node_edges.begin(), output.node_edges.end());
    return output;
}

namespace {

using SearchQueue = std::priority_queue<
    std::pair<double, zeus::map::NodeIndex>,
    std::vector<std::pair<double, zeus::map::NodeIndex>>,
    std::greater<std::pair<double, zeus::map::NodeIndex>>>;

// Discards settled stale entries and returns the smallest live key; infinity
// when the queue has none left.
double peekTop(SearchQueue& queue, const std::vector<std::uint8_t>& closed) {
    while (!queue.empty() && closed[queue.top().second] != 0) {
        queue.pop();
    }
    return queue.empty() ? kInfinity : queue.top().first;
}

}  // namespace

IncomingAdjacency buildIncomingAdjacency(const zeus::map::MapData& data) {
    IncomingAdjacency incoming;
    incoming.offsets.assign(data.nodes.size() + 1, 0);
    for (const zeus::map::DirectedEdge& edge : data.edges) {
        if (edge.to >= data.nodes.size()) {
            continue;
        }
        ++incoming.offsets[edge.to + 1];
    }
    for (std::size_t i = 1; i < incoming.offsets.size(); ++i) {
        incoming.offsets[i] += incoming.offsets[i - 1];
    }
    incoming.edges.resize(incoming.offsets.empty() ? 0 : incoming.offsets.back());
    std::vector<std::uint32_t> cursors = incoming.offsets;
    for (std::size_t i = 0; i < data.edges.size(); ++i) {
        const zeus::map::NodeIndex to = data.edges[i].to;
        if (to >= data.nodes.size()) {
            continue;
        }
        incoming.edges[cursors[to]++] = static_cast<zeus::map::EdgeIndex>(i);
    }
    return incoming;
}

SearchOutput runBidirectionalSearch(
    const zeus::map::MapRuntime& runtime,
    const IncomingAdjacency& incoming,
    const SearchQuery& query,
    double max_speed_mps,
    double known_best_time_s) {
    SearchOutput output;
    if (query.starts.empty() || query.goals.empty()) {
        return output;
    }

    const zeus::map::MapData& data = runtime.data();
    const std::size_t node_count = data.nodes.size();
    if (node_count == 0 || incoming.offsets.size() != node_count + 1) {
        return output;
    }

    std::vector<zeus::map::Point2d> start_points;
    std::vector<zeus::map::Point2d> goal_points;
    start_points.reserve(query.starts.size());
    goal_points.reserve(query.goals.size());
    for (const SearchEndpoint& start : query.starts) {
        if (start.node < node_count) {
            start_points.push_back(data.nodes[start.node].point);
        }
    }
    for (const SearchEndpoint& goal : query.goals) {
        if (goal.node < node_count) {
            goal_points.push_back(data.nodes[goal.node].point);
        }
    }
    if (start_points.empty() || goal_points.empty()) {
        return output;
    }

    // Symmetric Ikeda potential p(v) = (h_t(v) - h_s(v)) / 2, cached per node
    // so both directions observe bitwise-identical values. Start labels carry
    // +p and goal labels -p, so the potential cancels at any meeting point and
    // the best total is tracked in real seconds from the start.
    // Compute Ikeda potentials lazily. The previous implementation scanned
    // every map node for every query even when the search settled only a
    // small corridor; caching on first touch preserves deterministic values
    // while reducing work to the actually explored subgraph.
    std::vector<double> potential_cache(
        node_count, std::numeric_limits<double>::quiet_NaN());
    const auto potential = [&](zeus::map::NodeIndex node) {
        double& cached = potential_cache[node];
        if (!std::isnan(cached)) {
            return cached;
        }
        if (query.algorithm != Algorithm::kBidirectionalAStar) {
            cached = 0.0;
            return cached;
        }
        const double speed = std::max(kMinSpeedMps, max_speed_mps);
        const zeus::map::Point2d& point = data.nodes[node].point;
        double to_goal = kInfinity;
        for (const zeus::map::Point2d& goal_point : goal_points) {
            to_goal = std::min(
                to_goal, std::hypot(point.x - goal_point.x, point.y - goal_point.y) /
                             speed);
        }
        double to_start = kInfinity;
        for (const zeus::map::Point2d& start_point : start_points) {
            to_start = std::min(
                to_start, std::hypot(point.x - start_point.x, point.y - start_point.y) /
                              speed);
        }
        cached = (to_goal - to_start) / 2.0;
        return cached;
    };

    const auto reduced = [&](zeus::map::EdgeIndex edge_index) {
        const zeus::map::DirectedEdge& edge = runtime.edge(edge_index);
        const double weight =
            edgeCost(query, edge_index, edge) + potential(edge.to) - potential(edge.from);
        return weight > 0.0 ? weight : 0.0;
    };

    std::vector<double> dist_f(node_count, kInfinity);
    std::vector<zeus::map::EdgeIndex> pred_f(node_count, zeus::map::kInvalidEdge);
    std::vector<std::size_t> start_of(node_count, query.starts.size());
    std::vector<std::uint8_t> closed_f(node_count, 0);

    std::vector<double> dist_b(node_count, kInfinity);
    std::vector<zeus::map::EdgeIndex> pred_b(node_count, zeus::map::kInvalidEdge);
    std::vector<std::size_t> goal_of(node_count, query.goals.size());
    std::vector<std::uint8_t> closed_b(node_count, 0);

    SearchQueue queue_f;
    SearchQueue queue_b;
    for (std::size_t i = 0; i < query.starts.size(); ++i) {
        const SearchEndpoint& start = query.starts[i];
        if (start.node >= node_count) {
            continue;
        }
        const double label = start.extra_cost_s + potential(start.node);
        if (label < dist_f[start.node]) {
            dist_f[start.node] = label;
            start_of[start.node] = i;
            queue_f.push({label, start.node});
        }
    }
    for (std::size_t j = 0; j < query.goals.size(); ++j) {
        const SearchEndpoint& goal = query.goals[j];
        if (goal.node >= node_count) {
            continue;
        }
        const double label = goal.extra_cost_s - potential(goal.node);
        if (label < dist_b[goal.node]) {
            dist_b[goal.node] = label;
            goal_of[goal.node] = j;
            queue_b.push({label, goal.node});
        }
    }

    struct Meeting {
        zeus::map::EdgeIndex edge = zeus::map::kInvalidEdge;
        zeus::map::NodeIndex u = zeus::map::kInvalidNode;
        zeus::map::NodeIndex v = zeus::map::kInvalidNode;
    };

    double best_total = known_best_time_s;
    Meeting meeting;
    bool found = false;
    const auto updateBest = [&](double candidate, zeus::map::EdgeIndex edge,
                                zeus::map::NodeIndex u, zeus::map::NodeIndex v) {
        if (candidate < best_total) {
            best_total = candidate;
            meeting = {edge, u, v};
            found = true;
        }
    };

    while (true) {
        const double top_f = peekTop(queue_f, closed_f);
        const double top_b = peekTop(queue_b, closed_b);
        if (!(top_f + top_b < best_total)) {
            break;
        }
        if (top_f <= top_b) {
            const zeus::map::NodeIndex node = queue_f.top().second;
            queue_f.pop();
            closed_f[node] = 1;
            ++output.expanded_nodes;
            // Connection detection is decoupled from label improvement: a
            // meeting candidate must be checked even when the relaxation is
            // later skipped.
            if (dist_b[node] < kInfinity) {
                updateBest(dist_f[node] + dist_b[node], zeus::map::kInvalidEdge, node, node);
            }
            for (const zeus::map::EdgeIndex edge_index : runtime.outgoingEdges(node)) {
                if (!edgeEnabled(query, edge_index)) {
                    continue;
                }
                const zeus::map::DirectedEdge& edge = runtime.edge(edge_index);
                if (edge.to >= node_count) {
                    continue;
                }
                const double weight = reduced(edge_index);
                if (dist_b[edge.to] < kInfinity) {
                    updateBest(
                        dist_f[node] + weight + dist_b[edge.to], edge_index, node, edge.to);
                }
                const double candidate = dist_f[node] + weight;
                if (candidate < dist_f[edge.to]) {
                    dist_f[edge.to] = candidate;
                    pred_f[edge.to] = edge_index;
                    start_of[edge.to] = start_of[node];
                    queue_f.push({candidate, edge.to});
                }
            }
        } else {
            const zeus::map::NodeIndex node = queue_b.top().second;
            queue_b.pop();
            closed_b[node] = 1;
            ++output.expanded_nodes;
            if (dist_f[node] < kInfinity) {
                updateBest(dist_f[node] + dist_b[node], zeus::map::kInvalidEdge, node, node);
            }
            for (std::uint32_t k = incoming.offsets[node]; k < incoming.offsets[node + 1]; ++k) {
                const zeus::map::EdgeIndex edge_index = incoming.edges[k];
                if (!edgeEnabled(query, edge_index)) {
                    continue;
                }
                const zeus::map::DirectedEdge& edge = runtime.edge(edge_index);
                if (edge.from >= node_count) {
                    continue;
                }
                const double weight = reduced(edge_index);
                if (dist_f[edge.from] < kInfinity) {
                    updateBest(
                        dist_f[edge.from] + weight + dist_b[node], edge_index, edge.from, node);
                }
                const double candidate = dist_b[node] + weight;
                if (candidate < dist_b[edge.from]) {
                    dist_b[edge.from] = candidate;
                    pred_b[edge.from] = edge_index;
                    goal_of[edge.from] = goal_of[node];
                    queue_b.push({candidate, edge.from});
                }
            }
        }
    }

    if (!found) {
        return output;
    }

    output.found = true;
    output.start_index = start_of[meeting.u];
    output.goal_index = goal_of[meeting.v];
    // Labels may have improved after the meeting was recorded; recompute from
    // the final labels, which match the final predecessor chains.
    output.total_time_s = meeting.edge == zeus::map::kInvalidEdge
                              ? dist_f[meeting.u] + dist_b[meeting.v]
                              : dist_f[meeting.u] + reduced(meeting.edge) +
                                    dist_b[meeting.v];

    // Forward chain: from the meeting head back to the chosen start node.
    std::vector<zeus::map::EdgeIndex> forward_edges;
    zeus::map::NodeIndex node = meeting.u;
    const zeus::map::NodeIndex start_node = query.starts[output.start_index].node;
    std::size_t guard = 0;
    while (node != start_node) {
        const zeus::map::EdgeIndex edge_index = pred_f[node];
        if (edge_index == zeus::map::kInvalidEdge || ++guard > node_count) {
            return {};
        }
        forward_edges.push_back(edge_index);
        node = runtime.edge(edge_index).from;
    }
    std::reverse(forward_edges.begin(), forward_edges.end());

    // Backward chain: from the meeting tail forward to the chosen goal node.
    // pred_b[x] is an edge out of x pointing toward the goal side.
    std::vector<zeus::map::EdgeIndex> backward_edges;
    node = meeting.v;
    const zeus::map::NodeIndex goal_node = query.goals[output.goal_index].node;
    guard = 0;
    while (node != goal_node) {
        const zeus::map::EdgeIndex edge_index = pred_b[node];
        if (edge_index == zeus::map::kInvalidEdge || ++guard > node_count) {
            return {};
        }
        backward_edges.push_back(edge_index);
        node = runtime.edge(edge_index).to;
    }

    output.node_edges = std::move(forward_edges);
    if (meeting.edge != zeus::map::kInvalidEdge) {
        output.node_edges.push_back(meeting.edge);
    }
    output.node_edges.insert(
        output.node_edges.end(), backward_edges.begin(), backward_edges.end());
    return output;
}

}  // namespace zeus::routing
