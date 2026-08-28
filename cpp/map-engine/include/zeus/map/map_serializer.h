#pragma once

#include <string>

#include "zeus/map/types.h"

namespace zeus::map {

class MapSerializer {
public:
    static void save(const MapData& map, const std::string& path);
    [[nodiscard]] static MapData load(const std::string& path);
};

}  // namespace zeus::map

