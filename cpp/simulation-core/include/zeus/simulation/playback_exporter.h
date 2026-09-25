#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

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
    struct RouteChange {
        bool changed = false;
        double overlap_ratio = 0.0;
        std::optional<bool> reversed = std::nullopt;
    };

    // Compare at the application boundary, excluding the driven prefix.
    // Overlap is shared / union length of directed-edge intervals; repeated
    // visits count once. Missing/failed records return no comparison.
    [[nodiscard]] static std::optional<RouteChange> compareReroute(
        const zeus::map::MapRuntime& runtime,
        const SimulationResult& result,
        const VehicleRerouteRecord& reroute);

    // Adds per-vehicle A -> B -> A evidence in application order. Same-route
    // applications and failures do not interrupt the sequence. Driven prefixes
    // must still agree with A; missing boundaries/continuity remain unknown.
    [[nodiscard]] static std::vector<std::optional<RouteChange>> compareReroutes(
        const zeus::map::MapRuntime& runtime,
        const SimulationResult& result);

    static void save(
        const zeus::map::MapRuntime& runtime,
        const SimulationResult& result,
        const std::string& path);
};

}  // namespace zeus::simulation
