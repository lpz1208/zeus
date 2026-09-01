#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "zeus/map/map_runtime.h"
#include "zeus/map/types.h"
#include "zeus/routing/route_types.h"

namespace zeus::simulation {

// Deterministic mesoscopic simulation configuration. The MVP has no random
// behaviour, so there is deliberately no random seed field.
struct SimulationConfig {
    double step_seconds = 1.0;
    double duration_seconds = 3600.0;
    double sample_interval_seconds = 30.0;
    // Per-edge capacity = max(1, floor(length_m / jam_spacing_m) * lane_count).
    double jam_spacing_m = 7.0;
    // Speed floor as a ratio of the free-flow speed under jamming. The zero
    // default lets a saturated edge actually stop traffic; the engine keeps a
    // numerical 0.01 m/s crawl floor so time steps stay finite.
    double min_speed_ratio = 0.0;
    // Exit headway (SUMO meso style tau): leaving an edge requires this many
    // seconds since the edge's previous exit, interpolated by density between
    // the free-flow and jam values. Arrivals at the destination are exempt:
    // leaving the network consumes no downstream capacity. Both zero disables
    // the gate entirely.
    double exit_headway_ff_s = 1.4;
    double exit_headway_jam_s = 2.0;
    // Periodically rebuild routing costs from live occupancy. Zero disables
    // congestion-driven scans; explicit edge controls still update weights
    // and evaluate affected routes immediately.
    double reroute_interval_seconds = 0.0;
    // An edge becomes a routing event when its cost factor changes by at least
    // this multiplicative ratio since the previous published value.
    double reroute_cost_ratio = 1.25;
    // Abort after this many consecutive ticks without any vehicle movement.
    std::uint32_t deadlock_probe_ticks = 300;
};

struct VehicleDemand {
    zeus::map::Point2d origin;      // runtime CRS coordinates
    zeus::map::Point2d destination;
    double depart_time_s = 0.0;
    zeus::routing::Algorithm algorithm = zeus::routing::Algorithm::kDijkstra;
    // Agent-controlled vehicles are excluded from automatic rerouting: route
    // changes go through the session's decision/commit path instead.
    bool agent_controlled = false;
};

// A signal phase explicitly grants a set of edge-to-edge movements. Vehicles
// may enter the junction only during the green portion of a phase containing
// their movement. Yellow and all-red are plan-wide clearance durations added
// after every phase.
struct SignalMovement {
    zeus::map::EdgeIndex from_edge = zeus::map::kInvalidEdge;
    zeus::map::EdgeIndex to_edge = zeus::map::kInvalidEdge;
};

struct SignalPhase {
    double green_seconds = 30.0;
    std::vector<SignalMovement> movements;
    // Independent discharge rate for each listed movement. This is converted
    // to a minimum crossing headway (3600 / flow) at runtime.
    double saturation_flow_vph = 1800.0;
};

struct JunctionSignalPlan {
    zeus::map::NodeIndex node = zeus::map::kInvalidNode;
    double offset_seconds = 0.0;
    double yellow_seconds = 3.0;
    double all_red_seconds = 1.0;
    std::vector<SignalPhase> phases;
};

enum class ControlScope : std::uint8_t {
    kVehicle = 0,
    kEdge,
    kJunction,
};

enum class ControlAction : std::uint8_t {
    kHold = 0,
    kRelease,
    kClose,
    kOpen,
    kSetSpeedFactor,
    kSetCapacityFactor,
};

// Deterministic control command. Events are applied at the first tick boundary
// whose time is >= time_s; equal-time events preserve input order.
struct SimulationControlEvent {
    double time_s = 0.0;
    ControlScope scope = ControlScope::kVehicle;
    std::uint32_t target_id = 0;  // vehicle id, EdgeIndex, or NodeIndex
    ControlAction action = ControlAction::kHold;
    double value = 1.0;           // used by speed/capacity factor actions
};

struct AppliedControlEvent {
    double requested_time_s = 0.0;
    double effective_time_s = 0.0;
    ControlScope scope = ControlScope::kVehicle;
    std::uint32_t target_id = 0;
    ControlAction action = ControlAction::kHold;
    double value = 1.0;
};

struct VehicleRerouteRecord {
    double time_s = 0.0;
    std::uint32_t vehicle_id = 0;
    std::uint32_t old_route_id = 0;
    std::uint32_t new_route_id = 0;
    bool success = false;
};

enum class VehicleState : std::uint8_t {
    kWaiting = 0,
    kDriving,
    kArrived,
    kUnroutable,
};

struct VehicleSample {
    double t = 0.0;
    zeus::map::EdgeIndex edge = zeus::map::kInvalidEdge;
    double offset_s = 0.0;
};

struct VehicleRecord {
    std::uint32_t id = 0;
    double requested_depart_s = 0.0;
    double actual_depart_s = 0.0;   // NaN until the vehicle enters the network
    double arrive_s = 0.0;          // NaN until arrival
    double traveled_m = 0.0;
    std::uint32_t route_id = 0;     // index into SimulationResult::routes
    std::vector<VehicleSample> samples;
};

struct SimulationStats {
    std::uint64_t vehicles_total = 0;
    std::uint64_t arrived = 0;
    std::uint64_t unroutable = 0;
    std::uint64_t waiting_at_end = 0;
    std::uint64_t driving_at_end = 0;
    std::uint64_t ticks_executed = 0;
    std::uint64_t sample_count = 0;
    std::uint64_t route_plans = 0;
    std::uint64_t control_events_applied = 0;
    std::uint64_t vehicle_control_events = 0;
    std::uint64_t edge_control_events = 0;
    std::uint64_t junction_control_events = 0;
    std::uint64_t reroute_attempts = 0;
    std::uint64_t reroute_succeeded = 0;
    std::uint64_t reroute_failed = 0;
    std::uint64_t signal_plans = 0;
    std::uint64_t signal_phases = 0;
    std::uint64_t signal_wait_events = 0;
    std::uint64_t signal_red_wait_events = 0;
    std::uint64_t signal_saturation_wait_events = 0;
    std::uint64_t signal_movements_passed = 0;
    double average_travel_s = 0.0;
    double min_travel_s = 0.0;
    double max_travel_s = 0.0;
    double total_distance_m = 0.0;
    // Wall time intentionally spent behind a stateful tick barrier. This is
    // reported separately and excluded from compute_ms.
    double barrier_wait_ms = 0.0;
    double compute_ms = 0.0;
    bool deadlock = false;
    // True when a stateful session stopped the run at a tick boundary.
    bool cancelled = false;
};

// Per-edge run summary. Only edges that carried at least one vehicle entry
// are reported. mean_speed_mps integrates distance over the time vehicles
// actually spent on the edge, so a queueing edge shows its crawl speed.
struct EdgeKpi {
    zeus::map::EdgeIndex edge = zeus::map::kInvalidEdge;
    std::uint64_t entries = 0;         // admissions and boundary crossings in
    double vehicle_seconds = 0.0;      // time-integrated occupancy
    double distance_m = 0.0;           // sum of per-tick advances on the edge
    double mean_speed_mps = 0.0;
};

// Live per-edge state exposed through tick snapshots. Only "hot" edges are
// listed: occupied, closed, or carrying non-default control factors.
struct EdgeTickState {
    zeus::map::EdgeIndex edge = zeus::map::kInvalidEdge;
    std::uint32_t occupancy = 0;
    std::uint32_t effective_capacity = 1;
    bool closed = false;
    double speed_factor = 1.0;
    double routing_cost_factor = 1.0;
    double mean_speed_mps = 0.0;
};

// Per-agent slice of the snapshot: enough for a NavigationObservation-style
// view without exposing internal loop state.
struct AgentVehicleState {
    std::uint32_t vehicle_id = 0;
    VehicleState state = VehicleState::kWaiting;
    zeus::map::EdgeIndex edge = zeus::map::kInvalidEdge;  // current edge when driving
    double offset_s = 0.0;
    std::uint32_t route_id = 0;
    zeus::map::EdgeIndex destination_edge = zeus::map::kInvalidEdge;
    double route_end_offset_m = 0.0;
    double remaining_eta_s = 0.0;
    bool route_invalidated = false;
    bool held = false;
    std::vector<zeus::map::EdgeIndex> remaining_edges;
};

// Immutable state published at every committed tick boundary. The session
// fills state_version from its own monotonic counter.
struct TickSnapshot {
    std::uint64_t tick = 0;             // committed boundaries
    double simulation_time_s = 0.0;
    std::uint64_t state_version = 0;
    bool decision_due = false;
    std::string decision_reason;        // "route_invalidated" | "periodic" | ""
    std::vector<EdgeTickState> edges;   // hot edges only
    std::vector<AgentVehicleState> agents;
    std::uint64_t arrived = 0;
    std::uint64_t driving = 0;
    std::uint64_t waiting = 0;
    std::uint64_t unroutable = 0;
};

// A committed agent action queued by the session and drained by the engine at
// the next tick boundary. The route itself is re-planned deterministically
// from the vehicle's live position; only the algorithm choice travels.
struct RouteInjection {
    std::uint32_t vehicle_id = 0;
    zeus::routing::Algorithm algorithm = zeus::routing::Algorithm::kDijkstra;
    std::uint64_t based_on_state_version = 0;
};

struct SimulationResult {
    bool ok = false;
    std::string message;            // set when every demand failed to route
    SimulationConfig config;        // effective values actually used
    SimulationStats stats;
    std::vector<VehicleRecord> vehicles;
    // Per-edge summaries for every edge that carried traffic.
    std::vector<EdgeKpi> edge_kpis;
    // Route pool shared by vehicles with identical OD and algorithm.
    std::vector<zeus::routing::RoutePath> routes;
    std::vector<AppliedControlEvent> applied_controls;
    std::vector<VehicleRerouteRecord> reroutes;
    std::vector<JunctionSignalPlan> signal_plans;
};

[[nodiscard]] const char* controlScopeName(ControlScope scope);
[[nodiscard]] const char* controlActionName(ControlAction action);

}  // namespace zeus::simulation
