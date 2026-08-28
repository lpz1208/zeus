#pragma once

#include <cstddef>
#include <string>

#include "zeus/map/map_runtime.h"
#include "zeus/map/types.h"

#include "zeus/routing/route_types.h"

namespace zeus::routing {

// Writes a route as WGS84 GeoJSON, one LineString feature per traversed
// directed edge; the first and last features are clipped to the route's start
// and end offsets. Fields: ROAD_ID, SOURCE_ID, CLASS, LENGTH_M, EDGE_INDEX.
class RouteGeoJsonExporter {
public:
    [[nodiscard]] static std::size_t save(
        const zeus::map::MapData& map,
        const RouteResult& result,
        const std::string& path);
};

}  // namespace zeus::routing
