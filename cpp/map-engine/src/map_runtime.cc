#include "zeus/map/map_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
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
using IndexValue = std::pair<BgBox, std::uint32_t>;

struct IndexedSegment {
    EdgeIndex edge = kInvalidEdge;
    Point2d a;
    Point2d b;
    double start_s = 0.0;
};

double normalizeAngle(double angle) {
    while (angle > std::numbers::pi) {
        angle -= 2.0 * std::numbers::pi;
    }
    while (angle < -std::numbers::pi) {
        angle += 2.0 * std::numbers::pi;
    }
    return angle;
}

struct Projection {
    Point2d point;
    double t = 0.0;
    double distance_m = 0.0;
    double heading_rad = 0.0;
};

Projection project(Point2d point, Point2d a, Point2d b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double length_squared = dx * dx + dy * dy;
    const double t = length_squared > 0.0
                         ? std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) /
                                          length_squared,
                                      0.0,
                                      1.0)
                         : 0.0;
    const Point2d projected{a.x + t * dx, a.y + t * dy};
    return {projected, t, distance(point, projected), std::atan2(dy, dx)};
}

}  // namespace

struct MapRuntime::Impl {
    explicit Impl(MapData input) : map(std::move(input)) {
        buildAdjacency();
        buildTurnTransitions();
        buildSpatialIndex();
    }

    void buildAdjacency() {
        outgoing_offsets.assign(map.nodes.size() + 1, 0);
        for (const DirectedEdge& edge : map.edges) {
            if (edge.from >= map.nodes.size()) {
                throw std::runtime_error("cannot load runtime map with invalid edge nodes");
            }
            ++outgoing_offsets[edge.from + 1];
        }
        for (std::size_t i = 1; i < outgoing_offsets.size(); ++i) {
            outgoing_offsets[i] += outgoing_offsets[i - 1];
        }
        outgoing_edges.resize(map.edges.size());
        std::vector<std::uint32_t> cursors = outgoing_offsets;
        for (std::size_t edge_index = 0; edge_index < map.edges.size(); ++edge_index) {
            const NodeIndex from = map.edges[edge_index].from;
            outgoing_edges[cursors[from]++] = static_cast<EdgeIndex>(edge_index);
        }
    }

    void buildSpatialIndex() {
        std::vector<IndexValue> values;
        values.reserve(map.edges.size());
        edge_segment_offsets.assign(map.edges.size() + 1, 0);
        for (std::size_t edge_index = 0; edge_index < map.edges.size(); ++edge_index) {
            const DirectedEdge& edge = map.edges[edge_index];
            const std::uint64_t geometry_end =
                static_cast<std::uint64_t>(edge.geometry_offset) + edge.geometry_count;
            if (edge.geometry_count < 2 || geometry_end > map.geometry_points.size()) {
                throw std::runtime_error("cannot build spatial index for invalid edge geometry");
            }

            edge_segment_offsets[edge_index] = static_cast<std::uint32_t>(segments.size());
            double start_s = 0.0;
            bool has_bounds = false;
            Point2d minimum;
            Point2d maximum;
            for (std::uint32_t directed_segment = 0;
                 directed_segment + 1 < edge.geometry_count;
                 ++directed_segment) {
                const std::uint32_t first_offset = edge.geometry_reversed
                                                       ? edge.geometry_count - 1 - directed_segment
                                                       : directed_segment;
                const std::uint32_t second_offset = edge.geometry_reversed
                                                        ? edge.geometry_count - 2 - directed_segment
                                                        : directed_segment + 1;
                const Point2d a = map.geometry_points[edge.geometry_offset + first_offset];
                const Point2d b = map.geometry_points[edge.geometry_offset + second_offset];
                const double segment_length = distance(a, b);
                if (segment_length <= 1e-9) {
                    continue;
                }
                if (segments.size() >= std::numeric_limits<std::uint32_t>::max()) {
                    throw std::runtime_error("runtime spatial index exceeds 32-bit segment capacity");
                }
                const std::uint32_t segment_index = static_cast<std::uint32_t>(segments.size());
                segments.push_back({static_cast<EdgeIndex>(edge_index), a, b, start_s});
                (void)segment_index;
                if (!has_bounds) {
                    minimum = {std::min(a.x, b.x), std::min(a.y, b.y)};
                    maximum = {std::max(a.x, b.x), std::max(a.y, b.y)};
                    has_bounds = true;
                } else {
                    minimum.x = std::min({minimum.x, a.x, b.x});
                    minimum.y = std::min({minimum.y, a.y, b.y});
                    maximum.x = std::max({maximum.x, a.x, b.x});
                    maximum.y = std::max({maximum.y, a.y, b.y});
                }
                start_s += segment_length;
            }
            edge_segment_offsets[edge_index + 1] =
                static_cast<std::uint32_t>(segments.size());
            if (!has_bounds) {
                throw std::runtime_error("cannot build spatial index for zero-length edge geometry");
            }
            values.emplace_back(
                BgBox(BgPoint(minimum.x, minimum.y), BgPoint(maximum.x, maximum.y)),
                static_cast<std::uint32_t>(edge_index));
        }
        spatial_index = decltype(spatial_index)(values.begin(), values.end());
    }

    void buildTurnTransitions() {
        for (const TurnTransition& transition : map.turn_transitions) {
            if (transition.from_edge >= map.edges.size() ||
                transition.to_edge >= map.edges.size()) {
                throw std::runtime_error("cannot load runtime map with invalid turn transition");
            }
            const DirectedEdge& from = map.edges[transition.from_edge];
            const DirectedEdge& to = map.edges[transition.to_edge];
            if (from.to != to.from || !std::isfinite(transition.penalty_s) ||
                transition.penalty_s < 0.0F) {
                throw std::runtime_error("cannot load runtime map with inconsistent turn transition");
            }
            const std::uint64_t key =
                (static_cast<std::uint64_t>(transition.from_edge) << 32U) |
                transition.to_edge;
            turn_transitions[key] = transition;
        }
    }

    MapData map;
    std::vector<std::uint32_t> outgoing_offsets;
    std::vector<EdgeIndex> outgoing_edges;
    std::vector<IndexedSegment> segments;
    std::vector<std::uint32_t> edge_segment_offsets;
    bgi::rtree<IndexValue, bgi::quadratic<16>> spatial_index;
    std::unordered_map<std::uint64_t, TurnTransition> turn_transitions;
};

MapRuntime::MapRuntime(MapData map) : impl_(std::make_unique<Impl>(std::move(map))) {}

MapRuntime::~MapRuntime() = default;
MapRuntime::MapRuntime(MapRuntime&&) noexcept = default;
MapRuntime& MapRuntime::operator=(MapRuntime&&) noexcept = default;

const MapData& MapRuntime::data() const {
    return impl_->map;
}

const DirectedEdge& MapRuntime::edge(EdgeIndex edge_index) const {
    if (edge_index >= impl_->map.edges.size()) {
        throw std::out_of_range("edge index is outside the runtime map");
    }
    return impl_->map.edges[edge_index];
}

std::span<const EdgeIndex> MapRuntime::outgoingEdges(NodeIndex node) const {
    if (node >= impl_->map.nodes.size()) {
        throw std::out_of_range("node index is outside the runtime map");
    }
    const std::uint32_t begin = impl_->outgoing_offsets[node];
    const std::uint32_t end = impl_->outgoing_offsets[node + 1];
    return std::span<const EdgeIndex>(impl_->outgoing_edges.data() + begin, end - begin);
}

bool MapRuntime::hasTurnTransitions() const {
    return !impl_->turn_transitions.empty();
}

double MapRuntime::turnPenaltySeconds(EdgeIndex from_edge, EdgeIndex to_edge) const {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(from_edge) << 32U) | to_edge;
    const auto found = impl_->turn_transitions.find(key);
    if (found == impl_->turn_transitions.end()) {
        return 0.0;
    }
    return found->second.prohibited
               ? std::numeric_limits<double>::infinity()
               : static_cast<double>(found->second.penalty_s);
}

std::vector<MapMatchCandidate> MapRuntime::matchPoint(
    Point2d point,
    const MapMatchOptions& options) const {
    if (options.max_results == 0 || impl_->segments.empty()) {
        return {};
    }

    std::vector<IndexValue> nearby;
    const double radius = std::max(0.0, options.max_distance_m);
    const BgBox search_box(
        BgPoint(point.x - radius, point.y - radius),
        BgPoint(point.x + radius, point.y + radius));
    impl_->spatial_index.query(bgi::intersects(search_box), std::back_inserter(nearby));

    std::vector<MapMatchCandidate> result;
    result.reserve(nearby.size());
    for (const IndexValue& value : nearby) {
        const EdgeIndex edge_index = value.second;
        MapMatchCandidate best;
        best.score = std::numeric_limits<double>::infinity();
        for (std::uint32_t i = impl_->edge_segment_offsets[edge_index];
             i < impl_->edge_segment_offsets[edge_index + 1]; ++i) {
            const IndexedSegment& segment = impl_->segments[i];
            const Projection projection = project(point, segment.a, segment.b);
            if (projection.distance_m > options.max_distance_m) {
                continue;
            }
            const double heading_difference = options.has_heading
                                                  ? std::abs(normalizeAngle(
                                                        options.heading_rad - projection.heading_rad))
                                                  : 0.0;
            const double heading_cost = options.has_heading
                                            ? options.heading_weight *
                                                  (1.0 - std::cos(heading_difference))
                                            : 0.0;
            const double score = projection.distance_m + heading_cost;
            if (score >= best.score) {
                continue;
            }
            best.edge = edge_index;
            best.offset_s = segment.start_s +
                            projection.t * distance(segment.a, segment.b);
            best.projected_point = projection.point;
            best.lateral_distance_m = projection.distance_m;
            best.heading_difference_rad = heading_difference;
            best.score = score;
        }
        if (best.edge != kInvalidEdge) {
            result.push_back(best);
        }
    }

    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.score < rhs.score;
    });
    if (result.size() > options.max_results) {
        result.resize(options.max_results);
    }
    if (!result.empty()) {
        const double best_score = result.front().score;
        for (MapMatchCandidate& candidate : result) {
            const double relative = std::max(0.0, candidate.score - best_score);
            candidate.confidence = std::exp(-relative / 5.0) *
                                   std::exp(-candidate.lateral_distance_m /
                                            std::max(1.0, options.max_distance_m * 0.25));
        }
    }
    return result;
}

WorldPose MapRuntime::worldPose(const VehicleMapPosition& position) const {
    const DirectedEdge& selected_edge = edge(position.edge);
    const double target = std::clamp(position.offset_s, 0.0, selected_edge.length_m);
    double traversed = 0.0;

    Point2d final_a;
    Point2d final_b;
    double final_t = 1.0;
    bool found_segment = false;
    for (std::uint32_t directed_segment = 0;
         directed_segment + 1 < selected_edge.geometry_count;
         ++directed_segment) {
        const std::uint32_t first_offset = selected_edge.geometry_reversed
                                               ? selected_edge.geometry_count - 1 - directed_segment
                                               : directed_segment;
        const std::uint32_t second_offset = selected_edge.geometry_reversed
                                                ? selected_edge.geometry_count - 2 - directed_segment
                                                : directed_segment + 1;
        const Point2d a = impl_->map.geometry_points[selected_edge.geometry_offset + first_offset];
        const Point2d b = impl_->map.geometry_points[selected_edge.geometry_offset + second_offset];
        const double segment_length = distance(a, b);
        if (segment_length <= 1e-9) {
            continue;
        }
        final_a = a;
        final_b = b;
        found_segment = true;
        if (target <= traversed + segment_length) {
            final_t = std::clamp((target - traversed) / segment_length, 0.0, 1.0);
            break;
        }
        traversed += segment_length;
    }

    if (!found_segment) {
        throw std::runtime_error("edge geometry contains no non-zero segment");
    }

    const double heading = std::atan2(final_b.y - final_a.y, final_b.x - final_a.x);
    Point2d point{
        final_a.x + (final_b.x - final_a.x) * final_t,
        final_a.y + (final_b.y - final_a.y) * final_t,
    };
    point.x += -std::sin(heading) * position.lateral_offset_m;
    point.y += std::cos(heading) * position.lateral_offset_m;
    return {point, heading};
}

}  // namespace zeus::map
