#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace zeus::map {

using PersistentId = std::uint64_t;
using NodeIndex = std::uint32_t;
using EdgeIndex = std::uint32_t;

inline constexpr NodeIndex kInvalidNode = std::numeric_limits<NodeIndex>::max();
inline constexpr EdgeIndex kInvalidEdge = std::numeric_limits<EdgeIndex>::max();

struct Point2d {
    double x = 0.0;
    double y = 0.0;
};

struct BoundingBox {
    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
};

enum class Direction : std::uint8_t {
    kBoth = 0,
    kForward = 1,
    kReverse = 2,
};

struct MapMetadata {
    std::uint32_t format_version = 2;
    std::string source_path;
    std::string source_crs_wkt;
    std::string runtime_crs_wkt;
    double snap_tolerance_m = 0.5;
};

struct Node {
    PersistentId id = 0;
    Point2d point;
};

struct DirectedEdge {
    PersistentId id = 0;
    PersistentId road_id = 0;
    NodeIndex from = kInvalidNode;
    NodeIndex to = kInvalidNode;

    std::uint32_t geometry_offset = 0;
    std::uint32_t geometry_count = 0;
    bool geometry_reversed = false;

    double length_m = 0.0;
    float speed_limit_mps = 0.0F;
    // Number of usable lanes in this directed travel direction. A value of
    // one is the safe fallback for legacy maps and sources without lane tags.
    std::uint16_t lane_count = 1;
    std::int16_t z_level = 0;

    std::string source_id;
    std::string road_class;
};

// An explicit transition in the edge-based routing graph. Missing entries
// mean the turn is allowed with zero additional cost. Prohibited transitions
// model OSM no_*/only_* restrictions after the latter have been expanded;
// penalty_s supports junction and turn-delay costs without synthetic edges.
struct TurnTransition {
    EdgeIndex from_edge = kInvalidEdge;
    EdgeIndex to_edge = kInvalidEdge;
    float penalty_s = 0.0F;
    bool prohibited = false;
};

struct MapData {
    MapMetadata metadata;
    std::vector<Node> nodes;
    std::vector<DirectedEdge> edges;
    std::vector<Point2d> geometry_points;
    std::vector<TurnTransition> turn_transitions;
};

struct VehicleMapPosition {
    EdgeIndex edge = kInvalidEdge;
    double offset_s = 0.0;
    std::int16_t lane_index = 0;
    float lateral_offset_m = 0.0F;
};

struct WorldPose {
    Point2d point;
    double heading_rad = 0.0;
};

struct MapMatchOptions {
    std::size_t max_results = 5;
    double max_distance_m = 100.0;
    bool has_heading = false;
    double heading_rad = 0.0;
    double heading_weight = 5.0;
};

struct MapMatchCandidate {
    EdgeIndex edge = kInvalidEdge;
    double offset_s = 0.0;
    Point2d projected_point;
    double lateral_distance_m = 0.0;
    double heading_difference_rad = 0.0;
    double score = 0.0;
    double confidence = 0.0;
};

enum class IssueSeverity : std::uint8_t {
    kInfo = 0,
    kWarning = 1,
    kError = 2,
    kFatal = 3,
};

struct ValidationIssue {
    std::string code;
    IssueSeverity severity = IssueSeverity::kInfo;
    std::string message;
    std::string source_id;
    Point2d location;
    bool has_location = false;
};

struct ValidationReport {
    std::size_t node_count = 0;
    std::size_t edge_count = 0;
    std::size_t component_count = 0;
    std::size_t largest_component_nodes = 0;
    std::vector<ValidationIssue> issues;

    [[nodiscard]] bool hasFatalErrors() const;
    [[nodiscard]] std::size_t count(IssueSeverity severity) const;
};

[[nodiscard]] double distance(Point2d lhs, Point2d rhs);
[[nodiscard]] double polylineLength(const std::vector<Point2d>& points);
[[nodiscard]] PersistentId stableId(const std::string& value);

}  // namespace zeus::map
