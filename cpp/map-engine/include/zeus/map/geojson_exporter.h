#pragma once

#include <cstddef>
#include <string>

#include "zeus/map/types.h"

namespace zeus::map {

class GeoJsonExporter {
public:
    static void save(const MapData& map, const std::string& path);
    static void saveNodes(const MapData& map, const std::string& path);
    static std::size_t saveReference(
        const std::string& source_path,
        const std::string& output_path);
    static void saveIssues(
        const MapData& map,
        const std::vector<ValidationIssue>& issues,
        const std::string& path);
};

}  // namespace zeus::map
