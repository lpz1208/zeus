#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zeus/map/types.h"

namespace zeus::map {

struct ImportOptions {
    std::string id_field;
    std::string oneway_field;
    std::string speed_field;
    std::string lanes_field;
    std::string road_class_field;
    std::string z_level_field;
    std::string bridge_field;
    std::string tunnel_field;
    // Optional, source-CRS CSV:
    // from_source_id,via_x,via_y,to_source_id,no|only|penalty[,penalty_s]
    std::string turn_restrictions_file;
    std::string target_crs;

    double default_speed_kph = 40.0;
    double snap_tolerance_m = 0.5;
    bool default_bidirectional = true;
};

struct SourceRoad {
    std::string source_id;
    std::string road_class;
    Direction direction = Direction::kBoth;
    double speed_limit_mps = 40.0 / 3.6;
    std::uint16_t lane_count = 1;
    std::int16_t z_level = 0;
    std::vector<Point2d> points;
};

enum class SourceTurnKind : std::uint8_t {
    kNo = 0,
    kOnly = 1,
    kPenalty = 2,
};

struct SourceTurnTransition {
    std::string from_source_id;
    Point2d via_point;
    std::string to_source_id;
    SourceTurnKind kind = SourceTurnKind::kNo;
    float penalty_s = 0.0F;
};

struct ImportedRoads {
    MapMetadata metadata;
    std::vector<SourceRoad> roads;
    std::vector<SourceTurnTransition> turn_transitions;
    std::vector<ValidationIssue> issues;
};

class ShapefileImporter {
public:
    [[nodiscard]] ImportedRoads importFile(
        const std::string& path,
        const ImportOptions& options) const;

    [[nodiscard]] static std::string inspect(const std::string& path);
};

[[nodiscard]] ImportOptions loadImportOptions(const std::string& path);

}  // namespace zeus::map
