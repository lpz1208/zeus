#include "zeus/map/map_validator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zeus::map {
namespace {

class DisjointSet {
public:
    explicit DisjointSet(std::size_t size) : parent_(size), sizes_(size, 1) {
        std::iota(parent_.begin(), parent_.end(), 0);
    }

    std::size_t find(std::size_t value) {
        while (parent_[value] != value) {
            parent_[value] = parent_[parent_[value]];
            value = parent_[value];
        }
        return value;
    }

    void unite(std::size_t lhs, std::size_t rhs) {
        lhs = find(lhs);
        rhs = find(rhs);
        if (lhs == rhs) {
            return;
        }
        if (sizes_[lhs] < sizes_[rhs]) {
            std::swap(lhs, rhs);
        }
        parent_[rhs] = lhs;
        sizes_[lhs] += sizes_[rhs];
    }

private:
    std::vector<std::size_t> parent_;
    std::vector<std::size_t> sizes_;
};

std::uint64_t edgePairKey(NodeIndex from, NodeIndex to) {
    return (static_cast<std::uint64_t>(from) << 32U) | static_cast<std::uint64_t>(to);
}

}  // namespace

const char* severityName(IssueSeverity severity) {
    switch (severity) {
        case IssueSeverity::kInfo:
            return "info";
        case IssueSeverity::kWarning:
            return "warning";
        case IssueSeverity::kError:
            return "error";
        case IssueSeverity::kFatal:
            return "fatal";
    }
    return "unknown";
}

ValidationReport MapValidator::validate(
    const MapData& map,
    std::vector<ValidationIssue> prior_issues) const {
    ValidationReport report;
    report.node_count = map.nodes.size();
    report.edge_count = map.edges.size();
    report.issues = std::move(prior_issues);

    if (map.metadata.runtime_crs_wkt.empty()) {
        report.issues.push_back({
            "MISSING_RUNTIME_CRS",
            IssueSeverity::kFatal,
            "Runtime map has no coordinate reference system",
            {},
            {},
        });
    }
    if (map.nodes.empty()) {
        report.issues.push_back({
            "EMPTY_NODE_SET", IssueSeverity::kFatal, "Map has no topology nodes", {}, {}});
        return report;
    }
    if (map.edges.empty()) {
        report.issues.push_back({
            "EMPTY_EDGE_SET", IssueSeverity::kFatal, "Map has no directed edges", {}, {}});
        return report;
    }

    std::vector<std::size_t> incoming(map.nodes.size(), 0);
    std::vector<std::size_t> outgoing(map.nodes.size(), 0);
    std::vector<std::unordered_set<PersistentId>> incident_roads(map.nodes.size());
    DisjointSet components(map.nodes.size());
    std::unordered_map<std::uint64_t, std::size_t> directed_pairs;

    for (std::size_t edge_index = 0; edge_index < map.edges.size(); ++edge_index) {
        const DirectedEdge& edge = map.edges[edge_index];
        if (edge.from >= map.nodes.size() || edge.to >= map.nodes.size()) {
            report.issues.push_back({
                "INVALID_EDGE_NODE",
                IssueSeverity::kFatal,
                "Directed edge references a nonexistent node",
                edge.source_id,
                {},
            });
            continue;
        }
        ++outgoing[edge.from];
        ++incoming[edge.to];
        incident_roads[edge.from].insert(edge.road_id);
        incident_roads[edge.to].insert(edge.road_id);
        components.unite(edge.from, edge.to);

        if (edge.from == edge.to) {
            report.issues.push_back({
                "SELF_LOOP",
                IssueSeverity::kError,
                "Directed edge starts and ends at the same node",
                edge.source_id,
                map.nodes[edge.from].point,
                true,
            });
        }
        if (!std::isfinite(edge.length_m) || edge.length_m <= 0.0) {
            report.issues.push_back({
                "INVALID_EDGE_LENGTH",
                IssueSeverity::kFatal,
                "Directed edge has a non-positive or non-finite length",
                edge.source_id,
                map.nodes[edge.from].point,
                true,
            });
        }
        if (!std::isfinite(edge.speed_limit_mps) || edge.speed_limit_mps <= 0.0F) {
            report.issues.push_back({
                "INVALID_SPEED_LIMIT",
                IssueSeverity::kError,
                "Directed edge has an invalid speed limit",
                edge.source_id,
                map.nodes[edge.from].point,
                true,
            });
        }
        if (edge.lane_count == 0) {
            report.issues.push_back({
                "INVALID_LANE_COUNT",
                IssueSeverity::kError,
                "Directed edge has zero usable lanes",
                edge.source_id,
                map.nodes[edge.from].point,
                true,
            });
        }
        const std::uint64_t geometry_end =
            static_cast<std::uint64_t>(edge.geometry_offset) + edge.geometry_count;
        if (edge.geometry_count < 2 || geometry_end > map.geometry_points.size()) {
            report.issues.push_back({
                "INVALID_EDGE_GEOMETRY",
                IssueSeverity::kFatal,
                "Directed edge references an invalid geometry range",
                edge.source_id,
                map.nodes[edge.from].point,
                true,
            });
        } else {
            const Point2d geometry_from = edge.geometry_reversed
                                              ? map.geometry_points[geometry_end - 1]
                                              : map.geometry_points[edge.geometry_offset];
            const Point2d geometry_to = edge.geometry_reversed
                                            ? map.geometry_points[edge.geometry_offset]
                                            : map.geometry_points[geometry_end - 1];
            const double tolerance = std::max(1e-4, map.metadata.snap_tolerance_m * 0.1);
            if (distance(geometry_from, map.nodes[edge.from].point) > tolerance ||
                distance(geometry_to, map.nodes[edge.to].point) > tolerance) {
                report.issues.push_back({
                    "GEOMETRY_TOPOLOGY_MISMATCH",
                    IssueSeverity::kError,
                    "Edge geometry endpoints do not match its topology nodes",
                    edge.source_id,
                    geometry_from,
                    true,
                });
            }
        }

        const std::uint64_t pair = edgePairKey(edge.from, edge.to);
        const auto [existing, inserted] = directed_pairs.emplace(pair, edge_index);
        if (!inserted && map.edges[existing->second].road_id == edge.road_id) {
            report.issues.push_back({
                "DUPLICATE_DIRECTED_EDGE",
                IssueSeverity::kWarning,
                "The same road contains duplicate directed topology edges",
                edge.source_id,
                map.nodes[edge.from].point,
                true,
            });
        }
    }

    for (const TurnTransition& transition : map.turn_transitions) {
        if (transition.from_edge >= map.edges.size() ||
            transition.to_edge >= map.edges.size()) {
            report.issues.push_back({
                "INVALID_TURN_EDGE", IssueSeverity::kFatal,
                "Turn transition references a nonexistent edge", {}, {}});
            continue;
        }
        if (map.edges[transition.from_edge].to != map.edges[transition.to_edge].from) {
            report.issues.push_back({
                "DISCONNECTED_TURN", IssueSeverity::kFatal,
                "Turn transition edges do not meet at one topology node",
                map.edges[transition.from_edge].source_id,
                map.nodes[map.edges[transition.from_edge].to].point,
                true,
            });
        }
        if (!std::isfinite(transition.penalty_s) || transition.penalty_s < 0.0F) {
            report.issues.push_back({
                "INVALID_TURN_PENALTY", IssueSeverity::kFatal,
                "Turn transition has a negative or non-finite penalty",
                map.edges[transition.from_edge].source_id,
                map.nodes[map.edges[transition.from_edge].to].point,
                true,
            });
        }
    }

    std::unordered_map<std::size_t, std::size_t> component_sizes;
    for (std::size_t node = 0; node < map.nodes.size(); ++node) {
        const std::size_t degree = incoming[node] + outgoing[node];
        if (degree == 0) {
            report.issues.push_back({
                "ORPHAN_NODE",
                IssueSeverity::kError,
                "Topology node has no incident edges",
                {},
                map.nodes[node].point,
                true,
            });
            continue;
        }
        ++component_sizes[components.find(node)];
        if (incident_roads[node].size() == 1) {
            report.issues.push_back({
                "DANGLING_ENDPOINT",
                IssueSeverity::kWarning,
                "Topology node has only one incident directed edge",
                {},
                map.nodes[node].point,
                true,
            });
        }
        if (incoming[node] == 0) {
            report.issues.push_back({
                "NO_INCOMING_EDGE",
                IssueSeverity::kInfo,
                "Topology node cannot be reached through a directed edge",
                {},
                map.nodes[node].point,
                true,
            });
        }
        if (outgoing[node] == 0) {
            report.issues.push_back({
                "NO_OUTGOING_EDGE",
                IssueSeverity::kInfo,
                "Topology node has no outgoing directed edge",
                {},
                map.nodes[node].point,
                true,
            });
        }
    }

    report.component_count = component_sizes.size();
    for (const auto& [_, size] : component_sizes) {
        report.largest_component_nodes = std::max(report.largest_component_nodes, size);
    }
    if (report.component_count > 1) {
        report.issues.push_back({
            "DISCONNECTED_NETWORK",
            IssueSeverity::kWarning,
            "Road network contains " + std::to_string(report.component_count) +
                " weakly connected components",
            {},
            {},
        });
    }

    return report;
}

}  // namespace zeus::map
