#include "zeus/map/map_serializer.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace zeus::map {
namespace {

constexpr std::array<char, 8> kMagic{'Z', 'M', 'A', 'P', '0', '0', '0', '1'};
constexpr std::uint32_t kCurrentFormatVersion = 2;

template <typename T>
void writeValue(std::ostream& output, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    output.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!output) {
        throw std::runtime_error("failed while writing runtime map");
    }
}

template <typename T>
T readValue(std::istream& input) {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!input) {
        throw std::runtime_error("runtime map is truncated or corrupt");
    }
    return value;
}

void writeString(std::ostream& output, const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("runtime map string is too large");
    }
    writeValue(output, static_cast<std::uint32_t>(value.size()));
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!output) {
        throw std::runtime_error("failed while writing runtime map string");
    }
}

std::string readString(std::istream& input) {
    const std::uint32_t size = readValue<std::uint32_t>(input);
    constexpr std::uint32_t kMaximumStringSize = 64U * 1024U * 1024U;
    if (size > kMaximumStringSize) {
        throw std::runtime_error("runtime map contains an unreasonable string length");
    }
    std::string value(size, '\0');
    input.read(value.data(), static_cast<std::streamsize>(size));
    if (!input) {
        throw std::runtime_error("runtime map is truncated while reading a string");
    }
    return value;
}

std::uint32_t checkedCount(std::size_t size, const char* label) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string("too many ") + label + " for the v1 runtime map format");
    }
    return static_cast<std::uint32_t>(size);
}

}  // namespace

void MapSerializer::save(const MapData& map, const std::string& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create runtime map: " + path);
    }

    output.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    writeValue(output, kCurrentFormatVersion);
    writeString(output, map.metadata.source_path);
    writeString(output, map.metadata.source_crs_wkt);
    writeString(output, map.metadata.runtime_crs_wkt);
    writeValue(output, map.metadata.snap_tolerance_m);

    writeValue(output, checkedCount(map.nodes.size(), "nodes"));
    writeValue(output, checkedCount(map.edges.size(), "edges"));
    writeValue(output, checkedCount(map.geometry_points.size(), "geometry points"));
    writeValue(output, checkedCount(map.turn_transitions.size(), "turn transitions"));

    for (const Node& node : map.nodes) {
        writeValue(output, node.id);
        writeValue(output, node.point.x);
        writeValue(output, node.point.y);
    }
    for (const DirectedEdge& edge : map.edges) {
        writeValue(output, edge.id);
        writeValue(output, edge.road_id);
        writeValue(output, edge.from);
        writeValue(output, edge.to);
        writeValue(output, edge.geometry_offset);
        writeValue(output, edge.geometry_count);
        writeValue(output, static_cast<std::uint8_t>(edge.geometry_reversed ? 1 : 0));
        writeValue(output, edge.length_m);
        writeValue(output, edge.speed_limit_mps);
        writeValue(output, edge.lane_count);
        writeValue(output, edge.z_level);
        writeString(output, edge.source_id);
        writeString(output, edge.road_class);
    }
    for (const Point2d point : map.geometry_points) {
        writeValue(output, point.x);
        writeValue(output, point.y);
    }
    for (const TurnTransition& transition : map.turn_transitions) {
        writeValue(output, transition.from_edge);
        writeValue(output, transition.to_edge);
        writeValue(output, transition.penalty_s);
        writeValue(output, static_cast<std::uint8_t>(transition.prohibited ? 1 : 0));
    }
}

MapData MapSerializer::load(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open runtime map: " + path);
    }

    std::array<char, 8> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != kMagic) {
        throw std::runtime_error("not a supported Zeus runtime map: " + path);
    }

    MapData map;
    const std::uint32_t file_format_version = readValue<std::uint32_t>(input);
    if (file_format_version != 1 && file_format_version != kCurrentFormatVersion) {
        throw std::runtime_error(
            "unsupported Zeus runtime map version: " + std::to_string(file_format_version));
    }
    map.metadata.format_version = kCurrentFormatVersion;
    map.metadata.source_path = readString(input);
    map.metadata.source_crs_wkt = readString(input);
    map.metadata.runtime_crs_wkt = readString(input);
    map.metadata.snap_tolerance_m = readValue<double>(input);

    const std::uint32_t node_count = readValue<std::uint32_t>(input);
    const std::uint32_t edge_count = readValue<std::uint32_t>(input);
    const std::uint32_t geometry_count = readValue<std::uint32_t>(input);
    const std::uint32_t turn_transition_count =
        file_format_version >= 2 ? readValue<std::uint32_t>(input) : 0;
    map.nodes.reserve(node_count);
    map.edges.reserve(edge_count);
    map.geometry_points.reserve(geometry_count);
    map.turn_transitions.reserve(turn_transition_count);

    for (std::uint32_t i = 0; i < node_count; ++i) {
        Node node;
        node.id = readValue<PersistentId>(input);
        node.point.x = readValue<double>(input);
        node.point.y = readValue<double>(input);
        map.nodes.push_back(node);
    }
    for (std::uint32_t i = 0; i < edge_count; ++i) {
        DirectedEdge edge;
        edge.id = readValue<PersistentId>(input);
        edge.road_id = readValue<PersistentId>(input);
        edge.from = readValue<NodeIndex>(input);
        edge.to = readValue<NodeIndex>(input);
        edge.geometry_offset = readValue<std::uint32_t>(input);
        edge.geometry_count = readValue<std::uint32_t>(input);
        edge.geometry_reversed = readValue<std::uint8_t>(input) != 0;
        edge.length_m = readValue<double>(input);
        edge.speed_limit_mps = readValue<float>(input);
        edge.lane_count =
            file_format_version >= 2 ? readValue<std::uint16_t>(input) : 1;
        edge.z_level = readValue<std::int16_t>(input);
        edge.source_id = readString(input);
        edge.road_class = readString(input);
        map.edges.push_back(std::move(edge));
    }
    for (std::uint32_t i = 0; i < geometry_count; ++i) {
        Point2d point;
        point.x = readValue<double>(input);
        point.y = readValue<double>(input);
        map.geometry_points.push_back(point);
    }
    for (std::uint32_t i = 0; i < turn_transition_count; ++i) {
        TurnTransition transition;
        transition.from_edge = readValue<EdgeIndex>(input);
        transition.to_edge = readValue<EdgeIndex>(input);
        transition.penalty_s = readValue<float>(input);
        transition.prohibited = readValue<std::uint8_t>(input) != 0;
        map.turn_transitions.push_back(transition);
    }
    return map;
}

}  // namespace zeus::map
