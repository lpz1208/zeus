#pragma once

#include <cmath>
#include "zeus/routing/time_dependent.h"
#include "zeus/simulation/simulation_types.h"

namespace zeus::simulation {
// Controls take effect at the first simulation tick at/after their timestamp.
inline double routingDepartureTime(double time, double step) {
    return std::max(0.0, std::ceil((time - 1e-9) / step) * step);
}
inline zeus::routing::SpeedSchedule routingSpeedSchedule(
    std::size_t edge_count, std::span<const SimulationControlEvent> controls, double step) {
    std::vector<zeus::routing::SpeedChange> changes;
    // Match the engine's chronological, stable application order even when
    // several original timestamps round up to the same simulation tick.
    std::vector<SimulationControlEvent> sorted(controls.begin(), controls.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.time_s < b.time_s; });
    for (const auto& event : sorted)
        if (event.scope == ControlScope::kEdge && event.action == ControlAction::kSetSpeedFactor)
            changes.push_back({event.target_id, routingDepartureTime(event.time_s, step), event.value});
    return zeus::routing::SpeedSchedule(edge_count, changes);
}
}  // namespace zeus::simulation
