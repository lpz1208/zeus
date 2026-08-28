#pragma once

#include <cstddef>
#include <string>

#include "zeus/map/map_runtime.h"

#include "zeus/simulation/simulation_types.h"

namespace zeus::simulation {

// Writes one WGS84 LineString per vehicle with at least two samples.
// Fields: VEHICLE_ID, DEPART_S, ARRIVE_S, TRAVEL_S, DISTANCE_M.
class TrajectoryExporter {
public:
    [[nodiscard]] static std::size_t save(
        const zeus::map::MapRuntime& runtime,
        const SimulationResult& result,
        const std::string& path);
};

// Writes the playback document consumed by the web workbench:
// {"duration_s":…,"step_s":…,"sample_interval_s":…,
//  "vehicles":[{"id":…,"depart_s":…,"arrive_s":…,
//               "samples":[[t,lon,lat],…]}]}
class PlaybackExporter {
public:
    static void save(
        const zeus::map::MapRuntime& runtime,
        const SimulationResult& result,
        const std::string& path);
};

}  // namespace zeus::simulation
