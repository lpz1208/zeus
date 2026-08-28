#include "zeus/simulation/simulation_engine.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

#include "zeus/routing/route_types.h"
#include "zeus/simulation/vehicle_store.h"

namespace zeus::simulation {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct RouteKey {
    std::uint64_t origin_x_bits = 0;
    std::uint64_t origin_y_bits = 0;
    std::uint64_t destination_x_bits = 0;
    std::uint64_t destination_y_bits = 0;
    zeus::routing::Algorithm algorithm = zeus::routing::Algorithm::kDijkstra;

    bool operator<(const RouteKey& other) const {
        return std::tie(origin_x_bits, origin_y_bits, destination_x_bits, destination_y_bits,
                        algorithm) <
               std::tie(other.origin_x_bits, other.origin_y_bits,
                        other.destination_x_bits, other.destination_y_bits, other.algorithm);
    }
};

RouteKey routeKey(const VehicleDemand& demand) {
    return RouteKey{
        std::bit_cast<std::uint64_t>(demand.origin.x),
        std::bit_cast<std::uint64_t>(demand.origin.y),
        std::bit_cast<std::uint64_t>(demand.destination.x),
        std::bit_cast<std::uint64_t>(demand.destination.y),
        demand.algorithm,
    };
}

double freeSpeedMps(const zeus::map::DirectedEdge& edge) {
    return std::max(zeus::routing::kMinSpeedMps, static_cast<double>(edge.speed_limit_mps));
}

bool validControlAction(ControlScope scope, ControlAction action) {
    switch (scope) {
        case ControlScope::kVehicle:
            return action == ControlAction::kHold || action == ControlAction::kRelease ||
                   action == ControlAction::kSetSpeedFactor;
        case ControlScope::kEdge:
            return action == ControlAction::kClose || action == ControlAction::kOpen ||
                   action == ControlAction::kSetSpeedFactor ||
                   action == ControlAction::kSetCapacityFactor;
        case ControlScope::kJunction:
            return action == ControlAction::kClose || action == ControlAction::kOpen;
    }
    return false;
}

}  // namespace

const char* controlScopeName(ControlScope scope) {
    switch (scope) {
        case ControlScope::kVehicle: return "vehicle";
        case ControlScope::kEdge: return "edge";
        case ControlScope::kJunction: return "junction";
    }
    return "unknown";
}

const char* controlActionName(ControlAction action) {
    switch (action) {
        case ControlAction::kHold: return "hold";
        case ControlAction::kRelease: return "release";
        case ControlAction::kClose: return "close";
        case ControlAction::kOpen: return "open";
        case ControlAction::kSetSpeedFactor: return "speed_factor";
        case ControlAction::kSetCapacityFactor: return "capacity_factor";
    }
    return "unknown";
}

SimulationEngine::SimulationEngine(
    const zeus::map::MapRuntime& runtime,
    const zeus::routing::RoutePlanner& planner)
    : runtime_(runtime), planner_(planner) {}

SimulationResult SimulationEngine::run(
    const SimulationConfig& config,
    std::span<const VehicleDemand> demands,
    std::span<const SimulationControlEvent> controls) const {
    const auto start_time = std::chrono::steady_clock::now();

    if (!std::isfinite(config.step_seconds) || config.step_seconds <= 0.0 ||
        !std::isfinite(config.duration_seconds) ||
        config.duration_seconds < config.step_seconds ||
        !std::isfinite(config.sample_interval_seconds) ||
        config.sample_interval_seconds <= 0.0 ||
        !std::isfinite(config.jam_spacing_m) || config.jam_spacing_m <= 0.0 ||
        !std::isfinite(config.min_speed_ratio) || config.min_speed_ratio < 0.0 ||
        config.min_speed_ratio > 1.0 || config.deadlock_probe_ticks == 0) {
        throw std::invalid_argument("invalid simulation configuration");
    }

    SimulationResult result;
    result.config = config;
    result.config.sample_interval_seconds =
        std::max(config.sample_interval_seconds, config.step_seconds);

    const zeus::map::MapData& data = runtime_.data();
    const std::size_t edge_count = data.edges.size();

    std::vector<SimulationControlEvent> sorted_controls(controls.begin(), controls.end());
    for (const SimulationControlEvent& control : sorted_controls) {
        if (!std::isfinite(control.time_s) || control.time_s < 0.0 ||
            control.time_s >= result.config.duration_seconds ||
            !validControlAction(control.scope, control.action)) {
            throw std::invalid_argument("invalid simulation control event");
        }
        if ((control.scope == ControlScope::kVehicle && control.target_id >= demands.size()) ||
            (control.scope == ControlScope::kEdge && control.target_id >= edge_count) ||
            (control.scope == ControlScope::kJunction &&
             control.target_id >= data.nodes.size())) {
            throw std::invalid_argument("simulation control target is out of range");
        }
        if (control.action == ControlAction::kSetSpeedFactor &&
            (!std::isfinite(control.value) || control.value < 0.05 || control.value > 3.0)) {
            throw std::invalid_argument("speed factor must be between 0.05 and 3");
        }
        if (control.action == ControlAction::kSetCapacityFactor &&
            (!std::isfinite(control.value) || control.value < 0.05 || control.value > 10.0)) {
            throw std::invalid_argument("capacity factor must be between 0.05 and 10");
        }
    }
    std::stable_sort(
        sorted_controls.begin(), sorted_controls.end(),
        [](const SimulationControlEvent& left, const SimulationControlEvent& right) {
            return left.time_s < right.time_s;
        });

    std::vector<std::uint32_t> capacity(edge_count, 1);
    for (std::size_t i = 0; i < edge_count; ++i) {
        const double jam_slots =
            std::floor(data.edges[i].length_m /
                       result.config.jam_spacing_m) *
            std::max<std::uint16_t>(1, data.edges[i].lane_count);
        capacity[i] = static_cast<std::uint32_t>(std::max(1.0, jam_slots));
    }

    // Route pool: demands with identical OD and algorithm plan exactly once.
    // A cache value of -1 marks a demand that cannot be routed.
    std::map<RouteKey, std::int32_t> route_cache;
    VehicleStore store;
    store.reserve(demands.size());
    result.vehicles.resize(demands.size());
    std::size_t routable = 0;
    for (std::size_t i = 0; i < demands.size(); ++i) {
        const VehicleDemand& demand = demands[i];
        if (!std::isfinite(demand.origin.x) || !std::isfinite(demand.origin.y) ||
            !std::isfinite(demand.destination.x) ||
            !std::isfinite(demand.destination.y) ||
            !std::isfinite(demand.depart_time_s) || demand.depart_time_s < 0.0) {
            throw std::invalid_argument("invalid vehicle demand");
        }
        auto [entry, inserted] = route_cache.try_emplace(routeKey(demand), -2);
        if (entry->second == -2) {
            zeus::routing::RouteRequest request;
            request.origin = demand.origin;
            request.destination = demand.destination;
            request.algorithm = demand.algorithm;
            const zeus::routing::RouteResult planned = planner_.plan(request);
            if (planned.ok) {
                result.routes.push_back(planned.path);
                entry->second = static_cast<std::int32_t>(result.routes.size() - 1);
                ++result.stats.route_plans;
            } else {
                entry->second = -1;
            }
        }
        const bool can_route = entry->second >= 0;
        if (can_route) {
            ++routable;
        }
        store.append(demand, can_route ? static_cast<std::uint32_t>(entry->second) : 0);
        if (!can_route) {
            store.states_[i] = VehicleState::kUnroutable;
        }
        result.vehicles[i].id = store.ids_[i];
        result.vehicles[i].requested_depart_s = demand.depart_time_s;
        result.vehicles[i].actual_depart_s = kNaN;
        result.vehicles[i].arrive_s = kNaN;
        result.vehicles[i].route_id = store.route_ids_[i];
    }

    result.stats.vehicles_total = store.size();
    if (routable == 0) {
        result.stats.unroutable = store.size();
        result.message = "no vehicle demand could be routed on this map";
        return result;
    }

    const double step = result.config.step_seconds;
    const double duration = result.config.duration_seconds;
    const std::uint64_t total_ticks =
        static_cast<std::uint64_t>(std::floor(duration / step));
    double next_periodic_sample_s = result.config.sample_interval_seconds;

    std::vector<std::uint32_t> occupancy(edge_count, 0);
    // Entry reservations and exit releases are buffered separately: an entry
    // reserves capacity immediately (no over-admission within a tick), while
    // a slot only frees after the tick-end commit (no same-tick cascade
    // through a bottleneck).
    std::vector<std::int32_t> delta_in(edge_count, 0);
    std::vector<std::int32_t> delta_out(edge_count, 0);
    std::vector<bool> edge_closed(edge_count, false);
    std::vector<double> edge_speed_factor(edge_count, 1.0);
    std::vector<double> edge_capacity_factor(edge_count, 1.0);
    std::vector<bool> junction_closed(data.nodes.size(), false);

    const auto effectiveCapacity = [&](zeus::map::EdgeIndex edge) {
        const double scaled = std::floor(
            static_cast<double>(capacity[edge]) * edge_capacity_factor[edge]);
        return static_cast<std::uint32_t>(std::max(1.0, scaled));
    };

    const auto recordSample = [&](std::size_t i, double t, zeus::map::EdgeIndex edge,
                                  double offset) {
        if (t - store.last_sample_ts_[i] < 1e-9) {
            return;
        }
        store.last_sample_ts_[i] = t;
        result.vehicles[i].samples.push_back({t, edge, offset});
        ++result.stats.sample_count;
    };

    std::uint64_t stalled_ticks = 0;
    std::uint64_t tick = 0;
    std::size_t control_cursor = 0;
    for (; tick < total_ticks; ++tick) {
        const double now = tick * step;

        bool control_activity = false;
        while (control_cursor < sorted_controls.size() &&
               sorted_controls[control_cursor].time_s <= now + 1e-9) {
            const SimulationControlEvent& control = sorted_controls[control_cursor++];
            switch (control.scope) {
                case ControlScope::kVehicle:
                    if (control.action == ControlAction::kHold) {
                        store.held_[control.target_id] = true;
                    } else if (control.action == ControlAction::kRelease) {
                        store.held_[control.target_id] = false;
                    } else {
                        store.speed_factors_[control.target_id] = control.value;
                    }
                    ++result.stats.vehicle_control_events;
                    break;
                case ControlScope::kEdge:
                    if (control.action == ControlAction::kClose) {
                        edge_closed[control.target_id] = true;
                    } else if (control.action == ControlAction::kOpen) {
                        edge_closed[control.target_id] = false;
                    } else if (control.action == ControlAction::kSetSpeedFactor) {
                        edge_speed_factor[control.target_id] = control.value;
                    } else {
                        edge_capacity_factor[control.target_id] = control.value;
                    }
                    ++result.stats.edge_control_events;
                    break;
                case ControlScope::kJunction:
                    junction_closed[control.target_id] =
                        control.action == ControlAction::kClose;
                    ++result.stats.junction_control_events;
                    break;
            }
            result.applied_controls.push_back({
                control.time_s, now, control.scope, control.target_id,
                control.action, control.value,
            });
            ++result.stats.control_events_applied;
            control_activity = true;
        }

        bool active = false;
        for (std::size_t i = 0; i < store.size(); ++i) {
            if (store.states_[i] == VehicleState::kDriving ||
                (store.states_[i] == VehicleState::kWaiting &&
                 store.requested_departs_[i] < duration - 1e-9)) {
                active = true;
                break;
            }
        }
        if (!active) {
            break;
        }

        bool moved = false;
        std::fill(delta_in.begin(), delta_in.end(), 0);
        std::fill(delta_out.begin(), delta_out.end(), 0);

        // Admit departures first, in stable vehicle-id order. Their reserved
        // slots become part of this tick's read-only density snapshot before
        // any vehicle moves. This avoids giving a simultaneously released
        // fleet one artificial free-flow tick and keeps every car on the same
        // edge subject to the same density.
        for (std::size_t i = 0; i < store.size(); ++i) {
            if (store.states_[i] != VehicleState::kWaiting ||
                store.requested_departs_[i] > now + 1e-9 || store.held_[i]) {
                continue;
            }
            const zeus::routing::RoutePath& route = result.routes[store.route_ids_[i]];
            const zeus::map::EdgeIndex first = route.edges.front();
            if (edge_closed[first] || junction_closed[data.edges[first].from] ||
                occupancy[first] + delta_in[first] >= effectiveCapacity(first)) {
                continue;  // queued before the network entry
            }
            delta_in[first] += 1;
            store.states_[i] = VehicleState::kDriving;
            store.route_indices_[i] = 0;
            store.offsets_[i] = route.start_offset_m;
            store.actual_departs_[i] = now;
            result.vehicles[i].actual_depart_s = now;
            recordSample(i, now, first, route.start_offset_m);
            moved = true;
        }
        for (std::size_t e = 0; e < edge_count; ++e) {
            occupancy[e] += static_cast<std::uint32_t>(delta_in[e]);
            delta_in[e] = 0;
        }

        // Movement phase: occupancy is immutable until every vehicle has
        // advanced. Boundary entries reserve delta_in in id order, and all
        // releases stay in delta_out until the tick-end commit.
        for (std::size_t i = 0; i < store.size(); ++i) {
            if (store.states_[i] != VehicleState::kDriving) {
                continue;
            }
            if (store.held_[i]) {
                continue;
            }
            double remaining = step;
            while (remaining > 1e-12) {
                const zeus::routing::RoutePath& route = result.routes[store.route_ids_[i]];
                const std::uint32_t route_index = store.route_indices_[i];
                const zeus::map::EdgeIndex edge_index = route.edges[route_index];
                const zeus::map::DirectedEdge& edge = runtime_.edge(edge_index);
                const bool at_last_edge = route_index + 1 == route.edges.size();
                const double edge_end =
                    at_last_edge ? route.end_offset_m : edge.length_m;

                // Density as seen by this vehicle excludes itself: a lone car
                // never jams its own edge, and bottleneck serialization is
                // enforced by the entry admission instead.
                const double density_ratio =
                    (static_cast<double>(occupancy[edge_index]) - 1.0) /
                    static_cast<double>(effectiveCapacity(edge_index));
                const double speed_ratio = std::min(
                    1.0, std::max(result.config.min_speed_ratio, 1.0 - density_ratio));
                const double speed = std::max(
                    0.01, freeSpeedMps(edge) * speed_ratio *
                              edge_speed_factor[edge_index] * store.speed_factors_[i]);

                const double distance = edge_end - store.offsets_[i];
                if (distance <= 1e-9) {
                    if (at_last_edge) {
                        const double event_time = now + (step - remaining);
                        store.states_[i] = VehicleState::kArrived;
                        delta_out[edge_index] -= 1;
                        store.arrives_[i] = event_time;
                        result.vehicles[i].arrive_s = event_time;
                        recordSample(i, event_time, edge_index, edge_end);
                        moved = true;
                        break;
                    }
                    const zeus::map::EdgeIndex next = route.edges[route_index + 1];
                    if (edge_closed[next] || junction_closed[edge.to] ||
                        occupancy[next] + delta_in[next] >= effectiveCapacity(next)) {
                        remaining = 0.0;  // queue at the boundary
                        break;
                    }
                    delta_out[edge_index] -= 1;
                    delta_in[next] += 1;
                    store.route_indices_[i] = route_index + 1;
                    store.offsets_[i] = 0.0;
                    recordSample(i, now + (step - remaining), next, 0.0);
                    moved = true;
                    continue;
                }

                const double time_to_end = distance / speed;
                if (time_to_end > remaining) {
                    const double advance = speed * remaining;
                    store.offsets_[i] += advance;
                    store.traveled_[i] += advance;
                    moved = moved || advance > 1e-9;
                    remaining = 0.0;
                } else {
                    store.offsets_[i] = edge_end;
                    store.traveled_[i] += distance;
                    remaining -= time_to_end;
                    const double event_time = now + (step - remaining);
                    moved = true;
                    if (at_last_edge) {
                        store.states_[i] = VehicleState::kArrived;
                        delta_out[edge_index] -= 1;
                        store.arrives_[i] = event_time;
                        result.vehicles[i].arrive_s = event_time;
                        recordSample(i, event_time, edge_index, edge_end);
                        break;
                    }
                    const zeus::map::EdgeIndex next = route.edges[route_index + 1];
                    if (edge_closed[next] || junction_closed[edge.to] ||
                        occupancy[next] + delta_in[next] >= effectiveCapacity(next)) {
                        remaining = 0.0;  // queue at the boundary
                        break;
                    }
                    delta_out[edge_index] -= 1;
                    delta_in[next] += 1;
                    store.route_indices_[i] = route_index + 1;
                    store.offsets_[i] = 0.0;
                    recordSample(i, event_time, next, 0.0);
                }
            }
        }

        const double tick_end_s = (tick + 1) * step;
        if (tick_end_s + 1e-9 >= next_periodic_sample_s) {
            for (std::size_t i = 0; i < store.size(); ++i) {
                if (store.states_[i] != VehicleState::kDriving) {
                    continue;
                }
                recordSample(i, tick_end_s,
                             result.routes[store.route_ids_[i]].edges[store.route_indices_[i]],
                             store.offsets_[i]);
            }
            do {
                next_periodic_sample_s += result.config.sample_interval_seconds;
            } while (next_periodic_sample_s <= tick_end_s + 1e-9);
        }

        for (std::size_t e = 0; e < edge_count; ++e) {
            occupancy[e] = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(occupancy[e]) + delta_in[e] + delta_out[e]);
        }

        bool has_future_departure = false;
        for (std::size_t i = 0; i < store.size(); ++i) {
            if (store.states_[i] == VehicleState::kWaiting &&
                store.requested_departs_[i] > now + step + 1e-9) {
                has_future_departure = true;
                break;
            }
        }
        const bool has_future_control = control_cursor < sorted_controls.size();
        if (moved || control_activity || has_future_departure || has_future_control) {
            stalled_ticks = 0;
        } else {
            ++stalled_ticks;
            if (stalled_ticks >= config.deadlock_probe_ticks) {
                result.stats.deadlock = true;
                ++tick;
                break;
            }
        }
    }

    double travel_sum = 0.0;
    double min_travel = std::numeric_limits<double>::infinity();
    double max_travel = 0.0;
    std::uint64_t arrived = 0;
    for (std::size_t i = 0; i < store.size(); ++i) {
        switch (store.states_[i]) {
            case VehicleState::kArrived: {
                ++arrived;
                const double travel =
                    store.arrives_[i] - store.actual_departs_[i];
                travel_sum += travel;
                min_travel = std::min(min_travel, travel);
                max_travel = std::max(max_travel, travel);
                break;
            }
            case VehicleState::kUnroutable:
                ++result.stats.unroutable;
                break;
            case VehicleState::kWaiting:
                ++result.stats.waiting_at_end;
                break;
            case VehicleState::kDriving:
                ++result.stats.driving_at_end;
                break;
        }
        result.vehicles[i].traveled_m = store.traveled_[i];
        result.stats.total_distance_m += store.traveled_[i];
    }
    result.stats.arrived = arrived;
    result.stats.ticks_executed = tick;
    if (arrived > 0) {
        result.stats.average_travel_s = travel_sum / static_cast<double>(arrived);
        result.stats.min_travel_s = min_travel;
        result.stats.max_travel_s = max_travel;
    }
    result.ok = true;

    const auto end_time = std::chrono::steady_clock::now();
    result.stats.compute_ms =
        std::chrono::duration<double, std::milli>(end_time - start_time).count();
    return result;
}

}  // namespace zeus::simulation
