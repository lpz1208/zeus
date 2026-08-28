#include "zeus/map/map_builder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>

namespace zeus::map {
namespace {

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;

using BgPoint = bg::model::point<double, 2, bg::cs::cartesian>;
using BgBox = bg::model::box<BgPoint>;
using IndexValue = std::pair<BgBox, std::size_t>;

struct SegmentRef {
    std::size_t road = 0;
    std::size_t segment = 0;
    Point2d a;
    Point2d b;
    double start_s = 0.0;
    double length = 0.0;
};

struct SplitPoint {
    double s = 0.0;
    Point2d point;
};

BgBox makeBox(Point2d a, Point2d b) {
    return {
        BgPoint(std::min(a.x, b.x), std::min(a.y, b.y)),
        BgPoint(std::max(a.x, b.x), std::max(a.y, b.y)),
    };
}

double cross(Point2d a, Point2d b) {
    return a.x * b.y - a.y * b.x;
}

Point2d subtract(Point2d a, Point2d b) {
    return {a.x - b.x, a.y - b.y};
}

Point2d addScaled(Point2d point, Point2d vector, double scale) {
    return {point.x + vector.x * scale, point.y + vector.y * scale};
}

struct SegmentIntersection {
    double first_t = 0.0;
    double second_t = 0.0;
    Point2d point;
};

std::optional<SegmentIntersection> intersectSegments(const SegmentRef& first, const SegmentRef& second) {
    constexpr double kEpsilon = 1e-10;
    const Point2d r = subtract(first.b, first.a);
    const Point2d s = subtract(second.b, second.a);
    const double denominator = cross(r, s);
    if (std::abs(denominator) < kEpsilon) {
        return std::nullopt;
    }

    const Point2d difference = subtract(second.a, first.a);
    double first_t = cross(difference, s) / denominator;
    double second_t = cross(difference, r) / denominator;
    if (first_t < -kEpsilon || first_t > 1.0 + kEpsilon || second_t < -kEpsilon ||
        second_t > 1.0 + kEpsilon) {
        return std::nullopt;
    }

    first_t = std::clamp(first_t, 0.0, 1.0);
    second_t = std::clamp(second_t, 0.0, 1.0);
    return SegmentIntersection{first_t, second_t, addScaled(first.a, r, first_t)};
}

std::vector<double> cumulativeLengths(const std::vector<Point2d>& points) {
    std::vector<double> result(points.size(), 0.0);
    for (std::size_t i = 1; i < points.size(); ++i) {
        result[i] = result[i - 1] + distance(points[i - 1], points[i]);
    }
    return result;
}

Point2d pointAt(
    const std::vector<Point2d>& points,
    const std::vector<double>& cumulative,
    double offset_s) {
    if (offset_s <= 0.0) {
        return points.front();
    }
    if (offset_s >= cumulative.back()) {
        return points.back();
    }
    const auto upper = std::upper_bound(cumulative.begin(), cumulative.end(), offset_s);
    const std::size_t next = static_cast<std::size_t>(upper - cumulative.begin());
    const std::size_t previous = next - 1;
    const double segment_length = cumulative[next] - cumulative[previous];
    const double t = segment_length > 0.0 ? (offset_s - cumulative[previous]) / segment_length : 0.0;
    return {
        points[previous].x + (points[next].x - points[previous].x) * t,
        points[previous].y + (points[next].y - points[previous].y) * t,
    };
}

std::vector<Point2d> extractSubline(
    const SourceRoad& road,
    const std::vector<double>& cumulative,
    double start_s,
    double end_s) {
    constexpr double kEpsilon = 1e-7;
    std::vector<Point2d> result;
    result.push_back(pointAt(road.points, cumulative, start_s));
    for (std::size_t i = 1; i + 1 < road.points.size(); ++i) {
        if (cumulative[i] > start_s + kEpsilon && cumulative[i] < end_s - kEpsilon) {
            result.push_back(road.points[i]);
        }
    }
    const Point2d end = pointAt(road.points, cumulative, end_s);
    if (distance(result.back(), end) > kEpsilon) {
        result.push_back(end);
    }
    return result;
}

struct Cell {
    std::int64_t x = 0;
    std::int64_t y = 0;

    bool operator==(const Cell&) const = default;
};

struct CellHash {
    std::size_t operator()(const Cell& cell) const noexcept {
        const std::uint64_t x = static_cast<std::uint64_t>(cell.x);
        const std::uint64_t y = static_cast<std::uint64_t>(cell.y);
        return static_cast<std::size_t>((x * 0x9e3779b185ebca87ULL) ^ (y + 0x9e3779b97f4a7c15ULL));
    }
};

class NodeCluster {
public:
    NodeCluster(double tolerance, std::vector<Node>& nodes)
        : tolerance_(std::max(tolerance, 1e-6)), nodes_(nodes) {}

    NodeIndex findOrCreate(Point2d point) {
        const Cell center = cellFor(point);
        NodeIndex best = kInvalidNode;
        double best_distance = tolerance_;
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                const auto found = cells_.find({center.x + dx, center.y + dy});
                if (found == cells_.end()) {
                    continue;
                }
                for (const NodeIndex candidate : found->second) {
                    const double candidate_distance = distance(nodes_[candidate].point, point);
                    if (candidate_distance <= best_distance) {
                        best = candidate;
                        best_distance = candidate_distance;
                    }
                }
            }
        }
        if (best != kInvalidNode) {
            return best;
        }

        if (nodes_.size() >= std::numeric_limits<NodeIndex>::max()) {
            throw std::runtime_error("map contains too many nodes for 32-bit runtime indices");
        }
        const NodeIndex index = static_cast<NodeIndex>(nodes_.size());
        const std::string key = "node:" + std::to_string(std::llround(point.x * 1000.0)) + ':' +
                                std::to_string(std::llround(point.y * 1000.0));
        nodes_.push_back({stableId(key), point});
        cells_[center].push_back(index);
        return index;
    }

private:
    Cell cellFor(Point2d point) const {
        return {
            static_cast<std::int64_t>(std::floor(point.x / tolerance_)),
            static_cast<std::int64_t>(std::floor(point.y / tolerance_)),
        };
    }

    double tolerance_;
    std::vector<Node>& nodes_;
    std::unordered_map<Cell, std::vector<NodeIndex>, CellHash> cells_;
};

void appendEdge(
    MapData& map,
    const SourceRoad& source,
    std::size_t piece_index,
    PersistentId road_id,
    NodeIndex from,
    NodeIndex to,
    std::uint32_t geometry_offset,
    std::uint32_t geometry_count,
    double length_m,
    bool reversed) {
    if (map.edges.size() >= std::numeric_limits<EdgeIndex>::max()) {
        throw std::runtime_error("map contains too many edges for 32-bit runtime indices");
    }
    DirectedEdge edge;
    edge.id = stableId(
        "edge:" + source.source_id + ':' + std::to_string(piece_index) + (reversed ? ":r" : ":f"));
    edge.road_id = road_id;
    edge.from = from;
    edge.to = to;
    edge.geometry_offset = geometry_offset;
    edge.geometry_count = geometry_count;
    edge.geometry_reversed = reversed;
    edge.length_m = length_m;
    edge.speed_limit_mps = static_cast<float>(source.speed_limit_mps);
    edge.lane_count = std::max<std::uint16_t>(1, source.lane_count);
    edge.z_level = source.z_level;
    edge.source_id = source.source_id;
    edge.road_class = source.road_class;
    map.edges.push_back(std::move(edge));
}

bool sourceMatches(const std::string& candidate, const std::string& wanted) {
    return candidate == wanted ||
           (candidate.size() > wanted.size() &&
            candidate.compare(0, wanted.size(), wanted) == 0 &&
            candidate.compare(wanted.size(), 5, "#part") == 0);
}

void removeOrphanNodes(MapData& map) {
    std::vector<bool> used(map.nodes.size(), false);
    for (const DirectedEdge& edge : map.edges) {
        used[edge.from] = true;
        used[edge.to] = true;
    }

    std::vector<NodeIndex> remap(map.nodes.size(), kInvalidNode);
    std::vector<Node> compacted;
    compacted.reserve(map.nodes.size());
    for (std::size_t i = 0; i < map.nodes.size(); ++i) {
        if (!used[i]) {
            continue;
        }
        remap[i] = static_cast<NodeIndex>(compacted.size());
        compacted.push_back(std::move(map.nodes[i]));
    }
    if (compacted.size() == map.nodes.size()) {
        return;
    }
    for (DirectedEdge& edge : map.edges) {
        edge.from = remap[edge.from];
        edge.to = remap[edge.to];
    }
    map.nodes = std::move(compacted);
}

void buildTurnTransitions(BuildResult& result, const ImportedRoads& imported) {
    std::unordered_map<std::uint64_t, std::size_t> transition_by_pair;
    const auto addTransition = [&](EdgeIndex from, EdgeIndex to, bool prohibited,
                                   float penalty_s) {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(from) << 32U) | to;
        const auto found = transition_by_pair.find(key);
        if (found == transition_by_pair.end()) {
            transition_by_pair[key] = result.map.turn_transitions.size();
            result.map.turn_transitions.push_back({from, to, penalty_s, prohibited});
        } else {
            TurnTransition& existing = result.map.turn_transitions[found->second];
            existing.prohibited = existing.prohibited || prohibited;
            existing.penalty_s = std::max(existing.penalty_s, penalty_s);
        }
    };

    const double tolerance = std::max(0.01, imported.metadata.snap_tolerance_m * 2.0);
    for (const SourceTurnTransition& source : imported.turn_transitions) {
        NodeIndex via = kInvalidNode;
        double best_distance = tolerance;
        for (std::size_t i = 0; i < result.map.nodes.size(); ++i) {
            const double candidate = distance(result.map.nodes[i].point, source.via_point);
            if (candidate <= best_distance) {
                best_distance = candidate;
                via = static_cast<NodeIndex>(i);
            }
        }

        std::vector<EdgeIndex> from_edges;
        std::vector<EdgeIndex> to_edges;
        if (via != kInvalidNode) {
            for (std::size_t i = 0; i < result.map.edges.size(); ++i) {
                const DirectedEdge& edge = result.map.edges[i];
                if (edge.to == via && sourceMatches(edge.source_id, source.from_source_id)) {
                    from_edges.push_back(static_cast<EdgeIndex>(i));
                }
                if (edge.from == via && sourceMatches(edge.source_id, source.to_source_id)) {
                    to_edges.push_back(static_cast<EdgeIndex>(i));
                }
            }
        }
        if (via == kInvalidNode || from_edges.empty() || to_edges.empty()) {
            result.issues.push_back({
                "TURN_RESTRICTION_UNRESOLVED",
                IssueSeverity::kWarning,
                "Turn rule could not resolve its from/via/to topology",
                source.from_source_id + "->" + source.to_source_id,
                source.via_point,
                true,
            });
            continue;
        }

        for (const EdgeIndex from : from_edges) {
            if (source.kind == SourceTurnKind::kOnly) {
                for (std::size_t i = 0; i < result.map.edges.size(); ++i) {
                    const DirectedEdge& outgoing = result.map.edges[i];
                    if (outgoing.from == via &&
                        !sourceMatches(outgoing.source_id, source.to_source_id)) {
                        addTransition(from, static_cast<EdgeIndex>(i), true, 0.0F);
                    }
                }
                continue;
            }
            for (const EdgeIndex to : to_edges) {
                addTransition(
                    from, to, source.kind == SourceTurnKind::kNo,
                    source.kind == SourceTurnKind::kPenalty ? source.penalty_s : 0.0F);
            }
        }
    }
}

}  // namespace

BuildResult MapBuilder::build(const ImportedRoads& imported) const {
    if (imported.roads.empty()) {
        throw std::invalid_argument("cannot build a map without source roads");
    }

    BuildResult result;
    result.map.metadata = imported.metadata;
    result.issues = imported.issues;

    std::vector<std::vector<double>> cumulative;
    cumulative.reserve(imported.roads.size());
    std::vector<std::vector<SplitPoint>> splits(imported.roads.size());
    std::vector<SegmentRef> segments;
    std::vector<IndexValue> index_values;

    for (std::size_t road_index = 0; road_index < imported.roads.size(); ++road_index) {
        const SourceRoad& road = imported.roads[road_index];
        cumulative.push_back(cumulativeLengths(road.points));
        splits[road_index].push_back({0.0, road.points.front()});
        splits[road_index].push_back({cumulative.back().back(), road.points.back()});

        for (std::size_t segment_index = 0; segment_index + 1 < road.points.size(); ++segment_index) {
            SegmentRef segment;
            segment.road = road_index;
            segment.segment = segment_index;
            segment.a = road.points[segment_index];
            segment.b = road.points[segment_index + 1];
            segment.start_s = cumulative.back()[segment_index];
            segment.length = distance(segment.a, segment.b);
            if (segment.length <= 1e-8) {
                continue;
            }
            index_values.emplace_back(makeBox(segment.a, segment.b), segments.size());
            segments.push_back(segment);
        }
    }

    bgi::rtree<IndexValue, bgi::quadratic<16>> segment_index(index_values.begin(), index_values.end());
    std::vector<IndexValue> candidates;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        candidates.clear();
        segment_index.query(bgi::intersects(makeBox(segments[i].a, segments[i].b)),
                            std::back_inserter(candidates));
        for (const IndexValue& candidate : candidates) {
            const std::size_t j = candidate.second;
            if (j <= i) {
                continue;
            }
            const SegmentRef& first = segments[i];
            const SegmentRef& second = segments[j];
            if (first.road == second.road) {
                const std::size_t difference = first.segment > second.segment
                                                   ? first.segment - second.segment
                                                   : second.segment - first.segment;
                if (difference <= 1) {
                    continue;
                }
            }
            if (imported.roads[first.road].z_level != imported.roads[second.road].z_level) {
                continue;
            }
            const auto intersection = intersectSegments(first, second);
            if (!intersection.has_value()) {
                continue;
            }
            splits[first.road].push_back({
                first.start_s + intersection->first_t * first.length, intersection->point});
            splits[second.road].push_back({
                second.start_s + intersection->second_t * second.length, intersection->point});
        }
    }

    NodeCluster node_cluster(imported.metadata.snap_tolerance_m, result.map.nodes);
    for (std::size_t road_index = 0; road_index < imported.roads.size(); ++road_index) {
        const SourceRoad& road = imported.roads[road_index];
        std::vector<SplitPoint>& road_splits = splits[road_index];
        std::sort(road_splits.begin(), road_splits.end(), [](const SplitPoint& lhs, const SplitPoint& rhs) {
            return lhs.s < rhs.s;
        });

        std::vector<SplitPoint> unique_splits;
        for (const SplitPoint& split : road_splits) {
            if (unique_splits.empty() || std::abs(unique_splits.back().s - split.s) > 1e-6) {
                unique_splits.push_back(split);
            }
        }

        for (std::size_t piece = 0; piece + 1 < unique_splits.size(); ++piece) {
            const SplitPoint& start = unique_splits[piece];
            const SplitPoint& end = unique_splits[piece + 1];
            if (end.s - start.s <= 1e-6) {
                continue;
            }

            std::vector<Point2d> geometry =
                extractSubline(road, cumulative[road_index], start.s, end.s);
            if (geometry.size() < 2) {
                continue;
            }
            const NodeIndex from = node_cluster.findOrCreate(geometry.front());
            const NodeIndex to = node_cluster.findOrCreate(geometry.back());
            if (from == to) {
                result.issues.push_back({
                    "SNAP_COLLAPSED_EDGE",
                    IssueSeverity::kWarning,
                    "Endpoint snapping collapsed a road piece into one node",
                    road.source_id,
                    geometry.front(),
                    true,
                });
                continue;
            }
            geometry.front() = result.map.nodes[from].point;
            geometry.back() = result.map.nodes[to].point;
            const double length_m = polylineLength(geometry);
            if (length_m <= 1e-6) {
                continue;
            }

            const std::uint32_t geometry_offset =
                static_cast<std::uint32_t>(result.map.geometry_points.size());
            const std::uint32_t geometry_count = static_cast<std::uint32_t>(geometry.size());
            result.map.geometry_points.insert(
                result.map.geometry_points.end(), geometry.begin(), geometry.end());

            const PersistentId road_id = stableId(
                "road:" + road.source_id + ':' + std::to_string(piece));
            if (road.direction == Direction::kBoth || road.direction == Direction::kForward) {
                appendEdge(
                    result.map,
                    road,
                    piece,
                    road_id,
                    from,
                    to,
                    geometry_offset,
                    geometry_count,
                    length_m,
                    false);
            }
            if (road.direction == Direction::kBoth || road.direction == Direction::kReverse) {
                appendEdge(
                    result.map,
                    road,
                    piece,
                    road_id,
                    to,
                    from,
                    geometry_offset,
                    geometry_count,
                    length_m,
                    true);
            }
        }
    }

    if (result.map.edges.empty()) {
        throw std::runtime_error("topology build produced no navigable edges");
    }
    // A short road piece may create a new endpoint and then collapse onto that
    // same endpoint under snapping. Such provisional nodes are not topology and
    // must not leak into the serialized graph or validator diagnostics.
    removeOrphanNodes(result.map);
    buildTurnTransitions(result, imported);
    return result;
}

}  // namespace zeus::map
