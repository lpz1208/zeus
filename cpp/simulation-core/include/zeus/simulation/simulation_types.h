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
    // Speed floor as a ratio of the free-flow speed under jamming.
    double min_speed_ratio = 0.15;
    // Abort after this many consecutive ticks without any vehicle movement.
    std::uint32_t deadlock_probe_ticks = 300;
};

struct VehicleDemand {
    zeus::map::Point2d origin;      // runtime CRS coordinates
    zeus::map::Point2d destination;
    double depart_time_s = 0.0;
    zeus::routing::Algorithm algorithm = zeus::routing::Algorithm::kDijkstra;
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
    double average_travel_s = 0.0;
    double min_travel_s = 0.0;
    double max_travel_s = 0.0;
    double total_distance_m = 0.0;
    double compute_ms = 0.0;
    bool deadlock = false;
};

struct SimulationResult {
    bool ok = false;
    std::string message;            // set when every demand failed to route
    SimulationConfig config;        // effective values actually used
    SimulationStats stats;
    std::vector<VehicleRecord> vehicles;
    // Route pool shared by vehicles with identical OD and algorithm.
    std::vector<zeus::routing::RoutePath> routes;
    std::vector<AppliedControlEvent> applied_controls;
};

[[nodiscard]] const char* controlScopeName(ControlScope scope);
[[nodiscard]] const char* controlActionName(ControlAction action);

}  // namespace zeus::simulation
