#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "zeus/simulation/simulation_engine.h"
#include "zeus/simulation/simulation_types.h"

namespace zeus::simulation {

// Lightweight state exposed at committed tick boundaries. Version 0 means the
// session has not been reset. Every completed tick and every reset advances the
// version, so an action from an earlier observation cannot pass the guard.
struct SimulationSessionState {
    std::uint64_t tick = 0;
    double simulation_time_s = 0.0;
    std::uint64_t state_version = 0;
    bool ready = false;
    bool paused = true;
    bool finished = false;
    bool cancelled = false;
};

// Stateful tick-boundary adapter around the deterministic SimulationEngine.
// The engine still owns all hot simulation state on one worker thread; callers
// control it through reset/step/runToEnd without exposing mutable internals.
class SimulationSession {
public:
    SimulationSession(
        const SimulationEngine& engine,
        SimulationConfig config,
        std::span<const VehicleDemand> demands,
        std::span<const SimulationControlEvent> controls = {},
        std::span<const JunctionSignalPlan> signal_plans = {});

    ~SimulationSession();

    SimulationSession(const SimulationSession&) = delete;
    SimulationSession& operator=(const SimulationSession&) = delete;
    SimulationSession(SimulationSession&&) = delete;
    SimulationSession& operator=(SimulationSession&&) = delete;

    // Starts a fresh deterministic run and waits until the initial tick-zero
    // barrier is ready. Resetting a completed/paused session invalidates every
    // previously observed state version.
    [[nodiscard]] SimulationSessionState reset();

    // Advances at most `steps` committed ticks and blocks until the next tick
    // boundary or run completion.
    [[nodiscard]] SimulationSessionState step(std::uint64_t steps = 1);

    // Runs without intermediate barriers until completion. Another thread may
    // call pause(), which takes effect at the next tick boundary.
    [[nodiscard]] SimulationSessionState runToEnd();

    // Starts free-running execution and returns immediately. This is the
    // process-boundary form used by session-worker so a later pause/observe
    // command can be serviced while the engine thread keeps advancing.
    [[nodiscard]] SimulationSessionState resume();

    // Advances at most `max_steps` committed ticks and blocks until a decision
    // event fires (agent route invalidated or a periodic scan while an agent
    // drives), the cap is reached, or the run completes.
    [[nodiscard]] SimulationSessionState stepUntilEvent(std::uint64_t max_steps);

    void pause();
    void close();

    [[nodiscard]] SimulationSessionState observe() const;
    // Latest snapshot published by the engine thread at a committed boundary.
    // Empty (tick 0) before the first step.
    [[nodiscard]] TickSnapshot snapshot() const;
    [[nodiscard]] bool hasResult() const;
    [[nodiscard]] SimulationResult result() const;

    // Outcome of a submitted agent action.
    enum class CommitResult {
        kApplied = 0,
        kRejectedClosed,
        kRejectedStaleVersion,
        kRejectedUnknownVehicle,
        kRejectedNotAgent,
    };

    // Queues a route replacement for `vehicle_id` (a vehicle index), applied
    // at the next tick boundary by re-planning deterministically from the
    // live position with `algorithm`. Rejected when the session is closed, the
    // version does not match the current committed state, or the vehicle id is
    // out of range, or the vehicle is not marked agent-controlled.
    [[nodiscard]] CommitResult commitRoute(
        std::uint32_t vehicle_id,
        zeus::routing::Algorithm algorithm,
        std::uint64_t expected_state_version);

    // Version-validated acknowledgement that the agent keeps its route; drops
    // any pending injection for the vehicle. Same rejection rules.
    [[nodiscard]] CommitResult keepRoute(
        std::uint32_t vehicle_id,
        std::uint64_t expected_state_version);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zeus::simulation
