#pragma once

#include "zeus/map/shapefile_importer.h"
#include "zeus/map/types.h"

namespace zeus::map {

struct BuildResult {
    MapData map;
    std::vector<ValidationIssue> issues;
};

class MapBuilder {
public:
    [[nodiscard]] BuildResult build(const ImportedRoads& imported) const;
};

}  // namespace zeus::map

