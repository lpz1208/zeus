#pragma once

#include <span>
#include <vector>

#include "zeus/map/map_runtime.h"
#include "zeus/routing/route_planner.h"

#include "zeus/simulation/simulation_types.h"

namespace zeus::simulation {

// Deterministic mesoscopic simulation over a read-only runtime map. Vehicles
// are routed through the RoutePlanner (identical OD and algorithm share one
// pooled route), advance by edge travel time with density-dependent speed,
// and queue at edge entries when the downstream edge is full.
class SimulationEngine {
public:
    SimulationEngine(
        const zeus::map::MapRuntime& runtime,
        const zeus::routing::RoutePlanner& planner);

    [[nodiscard]] SimulationResult run(
        const SimulationConfig& config,
        std::span<const VehicleDemand> demands,
        std::span<const SimulationControlEvent> controls = {}) const;

    [[nodiscard]] SimulationResult run(
        const SimulationConfig& config,
        const std::vector<VehicleDemand>& demands) const {
        return run(config, std::span<const VehicleDemand>(demands), {});
    }

    [[nodiscard]] SimulationResult run(
        const SimulationConfig& config,
        const std::vector<VehicleDemand>& demands,
        const std::vector<SimulationControlEvent>& controls) const {
        return run(config, std::span<const VehicleDemand>(demands),
                   std::span<const SimulationControlEvent>(controls));
    }

private:
    const zeus::map::MapRuntime& runtime_;
    const zeus::routing::RoutePlanner& planner_;
};

}  // namespace zeus::simulation
