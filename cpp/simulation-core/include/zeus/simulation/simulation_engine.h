#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "zeus/map/map_runtime.h"
#include "zeus/routing/route_planner.h"

#include "zeus/simulation/simulation_types.h"

namespace zeus::simulation {

// Optional control point used by SimulationSession. It is called before each
// tick mutates state, so blocking here freezes simulation time at a committed
// boundary while wall-clock time may continue advancing.
class SimulationRunControl {
public:
    [[nodiscard]] virtual bool waitForTick(
        std::uint64_t next_tick,
        double simulation_time_s) = 0;

    // Called by the engine thread after every committed tick boundary with an
    // immutable snapshot of the resulting state. Default: ignored.
    virtual void publishTickState(const TickSnapshot& snapshot) {
        (void)snapshot;
    }

    // Drained by the engine thread right after the tick barrier opens;
    // returned injections are applied at this boundary. Default: none.
    [[nodiscard]] virtual std::vector<RouteInjection> collectRouteInjections() {
        return {};
    }

    virtual ~SimulationRunControl() = default;
};

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
        std::span<const SimulationControlEvent> controls = {},
        std::span<const JunctionSignalPlan> signal_plans = {},
        SimulationRunControl* run_control = nullptr) const;

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

    [[nodiscard]] SimulationResult run(
        const SimulationConfig& config,
        const std::vector<VehicleDemand>& demands,
        const std::vector<SimulationControlEvent>& controls,
        const std::vector<JunctionSignalPlan>& signal_plans) const {
        return run(config, std::span<const VehicleDemand>(demands),
                   std::span<const SimulationControlEvent>(controls),
                   std::span<const JunctionSignalPlan>(signal_plans));
    }

private:
    const zeus::map::MapRuntime& runtime_;
    const zeus::routing::RoutePlanner& planner_;
};

}  // namespace zeus::simulation
