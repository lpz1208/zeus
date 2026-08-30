#include "zeus/simulation/simulation_engine.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>
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

struct DynamicRouteKey {
    std::uint32_t old_route_id = 0;
    bool exact_origin = false;
    zeus::map::EdgeIndex origin_edge = zeus::map::kInvalidEdge;
    std::uint64_t origin_offset_bits = 0;
    std::uint64_t origin_x_bits = 0;
    std::uint64_t origin_y_bits = 0;
    zeus::map::EdgeIndex destination_edge = zeus::map::kInvalidEdge;
    std::uint64_t destination_offset_bits = 0;
    zeus::routing::Algorithm algorithm = zeus::routing::Algorithm::kDijkstra;

    bool operator<(const DynamicRouteKey& other) const {
        return std::tie(old_route_id, exact_origin, origin_edge, origin_offset_bits,
                        origin_x_bits, origin_y_bits, destination_edge,
                        destination_offset_bits, algorithm) <
               std::tie(other.old_route_id, other.exact_origin, other.origin_edge,
                        other.origin_offset_bits, other.origin_x_bits,
                        other.origin_y_bits, other.destination_edge,
                        other.destination_offset_bits, other.algorithm);
    }
};

struct CompiledSignalPlan {
    const JunctionSignalPlan* plan = nullptr;
    double cycle_seconds = 0.0;
};

enum class SignalGate : std::uint8_t {
    kOpen = 0,
    kRed,
    kSaturated,
};

std::uint64_t movementKey(
    zeus::map::EdgeIndex from_edge, zeus::map::EdgeIndex to_edge) {
    return (static_cast<std::uint64_t>(from_edge) << 32U) |
           static_cast<std::uint64_t>(to_edge);
}

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
    std::span<const SimulationControlEvent> controls,
    std::span<const JunctionSignalPlan> signal_plans,
    SimulationRunControl* run_control) const {
    const auto start_time = std::chrono::steady_clock::now();

    if (!std::isfinite(config.step_seconds) || config.step_seconds <= 0.0 ||
        !std::isfinite(config.duration_seconds) ||
        config.duration_seconds < config.step_seconds ||
        !std::isfinite(config.sample_interval_seconds) ||
        config.sample_interval_seconds <= 0.0 ||
        !std::isfinite(config.jam_spacing_m) || config.jam_spacing_m <= 0.0 ||
        !std::isfinite(config.min_speed_ratio) || config.min_speed_ratio < 0.0 ||
        config.min_speed_ratio > 1.0 ||
        !std::isfinite(config.exit_headway_ff_s) ||
        config.exit_headway_ff_s < 0.0 ||
        !std::isfinite(config.exit_headway_jam_s) ||
        config.exit_headway_jam_s < 0.0 ||
        !std::isfinite(config.reroute_interval_seconds) ||
        config.reroute_interval_seconds < 0.0 ||
        !std::isfinite(config.reroute_cost_ratio) ||
        config.reroute_cost_ratio < 1.01 || config.deadlock_probe_ticks == 0) {
        throw std::invalid_argument("invalid simulation configuration");
    }

    SimulationResult result;
    result.config = config;
    result.config.sample_interval_seconds =
        std::max(config.sample_interval_seconds, config.step_seconds);
    result.config.exit_headway_jam_s =
        std::max(config.exit_headway_ff_s, config.exit_headway_jam_s);

    const zeus::map::MapData& data = runtime_.data();
    const std::size_t edge_count = data.edges.size();

    std::vector<CompiledSignalPlan> compiled_signal_plans;
    compiled_signal_plans.reserve(signal_plans.size());
    std::vector<std::int32_t> signal_plan_by_node(data.nodes.size(), -1);
    result.signal_plans.assign(signal_plans.begin(), signal_plans.end());
    for (std::size_t plan_index = 0; plan_index < result.signal_plans.size(); ++plan_index) {
        const JunctionSignalPlan& plan = result.signal_plans[plan_index];
        if (plan.node >= data.nodes.size() || plan.phases.empty() ||
            !std::isfinite(plan.offset_seconds) || plan.offset_seconds < 0.0 ||
            !std::isfinite(plan.yellow_seconds) || plan.yellow_seconds < 0.0 ||
            !std::isfinite(plan.all_red_seconds) || plan.all_red_seconds < 0.0 ||
            signal_plan_by_node[plan.node] >= 0) {
            throw std::invalid_argument("invalid or duplicate junction signal plan");
        }
        double cycle_seconds = 0.0;
        for (const SignalPhase& phase : plan.phases) {
            if (!std::isfinite(phase.green_seconds) || phase.green_seconds <= 0.0 ||
                phase.movements.empty() ||
                !std::isfinite(phase.saturation_flow_vph) ||
                phase.saturation_flow_vph < 60.0 ||
                phase.saturation_flow_vph > 7200.0) {
                throw std::invalid_argument(
                    "signal phases require valid green time, movements and saturation flow");
            }
            cycle_seconds += phase.green_seconds + plan.yellow_seconds +
                             plan.all_red_seconds;
            for (const SignalMovement& movement : phase.movements) {
                if (movement.from_edge >= edge_count || movement.to_edge >= edge_count ||
                    data.edges[movement.from_edge].to != plan.node ||
                    data.edges[movement.to_edge].from != plan.node ||
                    !std::isfinite(runtime_.turnPenaltySeconds(
                        movement.from_edge, movement.to_edge))) {
                    throw std::invalid_argument(
                        "signal movement must be a legal transition through its junction");
                }
            }
        }
        if (!std::isfinite(cycle_seconds) || cycle_seconds <= 0.0) {
            throw std::invalid_argument("invalid signal cycle duration");
        }
        signal_plan_by_node[plan.node] =
            static_cast<std::int32_t>(compiled_signal_plans.size());
        compiled_signal_plans.push_back({&plan, cycle_seconds});
        ++result.stats.signal_plans;
        result.stats.signal_phases += plan.phases.size();
    }

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
    std::vector<std::uint8_t> routing_edge_enabled(edge_count, 1);
    std::vector<double> edge_speed_factor(edge_count, 1.0);
    std::vector<double> edge_capacity_factor(edge_count, 1.0);
    std::vector<double> routing_edge_cost_factors(edge_count, 1.0);
    std::vector<double> published_edge_cost_factors(edge_count, 1.0);
    std::vector<std::uint8_t> changed_routing_edges(edge_count, 0);
    std::vector<std::uint8_t> explicit_cost_edges(edge_count, 0);
    std::vector<bool> junction_closed(data.nodes.size(), false);
    double next_reroute_scan_s = result.config.reroute_interval_seconds > 0.0
                                     ? result.config.reroute_interval_seconds
                                     : std::numeric_limits<double>::infinity();

    // Optional exit-headway gate: leaving an edge must respect the time since
    // that edge's previous exit, so a bottleneck does not only "fill up" but
    // also "flows" at a bounded discharge rate.
    const double headway_ff_s = result.config.exit_headway_ff_s;
    const double headway_jam_s = result.config.exit_headway_jam_s;
    const bool exit_headway_enabled = headway_jam_s > 0.0;
    std::vector<double> last_exit_s(
        edge_count, -std::numeric_limits<double>::infinity());

    // Per-tick activity counters: maintaining them incrementally keeps the
    // loop head O(1) instead of rescanning every vehicle each tick.
    std::uint64_t driving_count = 0;
    std::uint64_t pending_entries = 0;
    for (std::size_t i = 0; i < store.size(); ++i) {
        if (store.states_[i] == VehicleState::kWaiting &&
            store.requested_departs_[i] < duration - 1e-9) {
            ++pending_entries;
        }
    }

    const auto effectiveCapacity = [&](zeus::map::EdgeIndex edge) {
        const double scaled = std::floor(
            static_cast<double>(capacity[edge]) * edge_capacity_factor[edge]);
        return static_cast<std::uint32_t>(std::max(1.0, scaled));
    };

    const auto dynamicRoutingCostFactor = [&](zeus::map::EdgeIndex edge) {
        const double others = std::max(
            0.0, static_cast<double>(occupancy[edge]) - 1.0);
        const double density_ratio =
            others / static_cast<double>(effectiveCapacity(edge));
        const double congestion_speed_ratio = std::min(
            1.0, std::max(result.config.min_speed_ratio, 1.0 - density_ratio));
        const double speed_penalty =
            1.0 / std::max(0.01, edge_speed_factor[edge]);
        const double capacity_penalty =
            1.0 / std::min(1.0, edge_capacity_factor[edge]);
        return std::max(
            1.0, speed_penalty * capacity_penalty / congestion_speed_ratio);
    };

    const auto refreshRoutingCosts = [&]() {
        for (std::size_t edge = 0; edge < edge_count; ++edge) {
            routing_edge_cost_factors[edge] = dynamicRoutingCostFactor(
                static_cast<zeus::map::EdgeIndex>(edge));
        }
    };

    const auto exitHeadway = [&](zeus::map::EdgeIndex edge) {
        if (!exit_headway_enabled) {
            return 0.0;
        }
        const double ratio = std::min(
            1.0, static_cast<double>(occupancy[edge]) /
                     static_cast<double>(effectiveCapacity(edge)));
        return headway_ff_s + (headway_jam_s - headway_ff_s) * ratio;
    };

    const auto canExit = [&](zeus::map::EdgeIndex edge, double event_time) {
        return !exit_headway_enabled ||
               event_time - last_exit_s[edge] >= exitHeadway(edge) - 1e-9;
    };

    std::unordered_map<std::uint64_t, double> last_signal_pass_s;
    last_signal_pass_s.reserve(signal_plans.size() * 8);

    const auto signalGate = [&](zeus::map::NodeIndex node,
                                zeus::map::EdgeIndex from_edge,
                                zeus::map::EdgeIndex to_edge,
                                double event_time) {
        const std::int32_t signal_index = signal_plan_by_node[node];
        if (signal_index < 0) {
            return SignalGate::kOpen;
        }
        const CompiledSignalPlan& compiled =
            compiled_signal_plans[static_cast<std::size_t>(signal_index)];
        const JunctionSignalPlan& plan = *compiled.plan;
        double cycle_position = std::fmod(
            event_time + plan.offset_seconds, compiled.cycle_seconds);
        if (cycle_position < 0.0) {
            cycle_position += compiled.cycle_seconds;
        }
        for (const SignalPhase& phase : plan.phases) {
            if (cycle_position < phase.green_seconds - 1e-9) {
                const bool allowed = std::any_of(
                    phase.movements.begin(), phase.movements.end(),
                    [&](const SignalMovement& movement) {
                        return movement.from_edge == from_edge &&
                               movement.to_edge == to_edge;
                    });
                if (!allowed) {
                    return SignalGate::kRed;
                }
                const auto last = last_signal_pass_s.find(
                    movementKey(from_edge, to_edge));
                const double last_pass = last == last_signal_pass_s.end()
                                             ? -std::numeric_limits<double>::infinity()
                                             : last->second;
                const double headway_s = 3600.0 / phase.saturation_flow_vph;
                return event_time - last_pass >= headway_s - 1e-9
                           ? SignalGate::kOpen
                           : SignalGate::kSaturated;
            }
            cycle_position -= phase.green_seconds;
            const double clearance = plan.yellow_seconds + plan.all_red_seconds;
            if (cycle_position < clearance - 1e-9) {
                return SignalGate::kRed;
            }
            cycle_position -= clearance;
        }
        return SignalGate::kRed;
    };

    const auto recordSignalPass = [&](zeus::map::NodeIndex node,
                                      zeus::map::EdgeIndex from_edge,
                                      zeus::map::EdgeIndex to_edge,
                                      double event_time) {
        if (signal_plan_by_node[node] < 0) {
            return;
        }
        last_signal_pass_s[movementKey(from_edge, to_edge)] = event_time;
        ++result.stats.signal_movements_passed;
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

    const auto remainingRouteTime = [&](
        const zeus::routing::RoutePath& route, std::size_t first_index,
        double first_offset) {
        double total = 0.0;
        for (std::size_t route_index = first_index;
             route_index < route.edges.size(); ++route_index) {
            const zeus::map::EdgeIndex edge_index = route.edges[route_index];
            if (routing_edge_enabled[edge_index] == 0) {
                return std::numeric_limits<double>::infinity();
            }
            if (route_index > first_index) {
                const double turn = runtime_.turnPenaltySeconds(
                    route.edges[route_index - 1], edge_index);
                if (!std::isfinite(turn)) {
                    return std::numeric_limits<double>::infinity();
                }
                total += turn;
            }
            const zeus::map::DirectedEdge& edge = runtime_.edge(edge_index);
            const double begin = route_index == first_index ? first_offset : 0.0;
            const double end = route_index + 1 == route.edges.size()
                                   ? route.end_offset_m
                                   : edge.length_m;
            total += std::max(0.0, end - begin) /
                     freeSpeedMps(edge) * routing_edge_cost_factors[edge_index];
        }
        return total;
    };

    const auto rerouteAffectedVehicles = [&](double now) {
        const zeus::routing::RoutingOverlay overlay{
            routing_edge_enabled, routing_edge_cost_factors};
        struct CachedRoute {
            std::int32_t route_id = -2;
        };
        std::map<DynamicRouteKey, CachedRoute> cache;
        bool switched_any = false;
        for (std::size_t i = 0; i < store.size(); ++i) {
            if (store.states_[i] != VehicleState::kWaiting &&
                store.states_[i] != VehicleState::kDriving) {
                continue;
            }
            const zeus::routing::RoutePath& old_route = result.routes[store.route_ids_[i]];
            const std::size_t first_future = store.states_[i] == VehicleState::kDriving
                                                 ? store.route_indices_[i] + 1
                                                 : 0;
            bool topology_affected = false;
            bool cost_affected = false;
            for (std::size_t route_index = first_future;
                 route_index < old_route.edges.size(); ++route_index) {
                if (routing_edge_enabled[old_route.edges[route_index]] == 0) {
                    topology_affected = true;
                }
                if (changed_routing_edges[old_route.edges[route_index]] != 0) {
                    cost_affected = true;
                }
            }
            if (!topology_affected && !cost_affected) {
                continue;
            }

            ++result.stats.reroute_attempts;
            const VehicleDemand& demand = demands[i];
            DynamicRouteKey key;
            key.old_route_id = store.route_ids_[i];
            key.exact_origin = store.states_[i] == VehicleState::kDriving;
            if (key.exact_origin) {
                key.origin_edge = old_route.edges[store.route_indices_[i]];
                key.origin_offset_bits = std::bit_cast<std::uint64_t>(store.offsets_[i]);
            } else {
                key.origin_x_bits = std::bit_cast<std::uint64_t>(demand.origin.x);
                key.origin_y_bits = std::bit_cast<std::uint64_t>(demand.origin.y);
            }
            key.destination_edge = old_route.edges.back();
            key.destination_offset_bits =
                std::bit_cast<std::uint64_t>(old_route.end_offset_m);
            key.algorithm = demand.algorithm;

            auto [cached, inserted] = cache.try_emplace(key);
            if (inserted) {
                if (routing_edge_enabled[key.destination_edge] == 0) {
                    cached->second.route_id = -1;
                } else {
                    zeus::routing::RouteRequest request;
                    request.origin = demand.origin;
                    request.destination = demand.destination;
                    request.algorithm = demand.algorithm;
                    request.overlay = &overlay;
                    request.destination_position = zeus::routing::RoutePosition{
                        key.destination_edge, old_route.end_offset_m};
                    if (key.exact_origin) {
                        request.origin_position = zeus::routing::RoutePosition{
                            key.origin_edge, store.offsets_[i]};
                    }
                    const zeus::routing::RouteResult planned = planner_.plan(request);
                    ++result.stats.route_plans;
                    if (planned.ok) {
                        const std::size_t current_index = key.exact_origin
                                                              ? store.route_indices_[i]
                                                              : 0;
                        const double current_offset = key.exact_origin
                                                          ? store.offsets_[i]
                                                          : old_route.start_offset_m;
                        const double current_time = remainingRouteTime(
                            old_route, current_index, current_offset);
                        const bool same_path =
                            planned.path.edges.size() ==
                                old_route.edges.size() - current_index &&
                            std::equal(
                                planned.path.edges.begin(), planned.path.edges.end(),
                                old_route.edges.begin() +
                                    static_cast<std::ptrdiff_t>(current_index)) &&
                            std::abs(planned.path.start_offset_m - current_offset) <= 1e-9 &&
                            std::abs(planned.path.end_offset_m - old_route.end_offset_m) <= 1e-9;
                        const bool beneficial = topology_affected ||
                            (!same_path && planned.stats.time_s + 1e-9 < current_time);
                        if (beneficial) {
                            result.routes.push_back(planned.path);
                            cached->second.route_id = static_cast<std::int32_t>(
                                result.routes.size() - 1);
                        } else {
                            cached->second.route_id = -1;
                        }
                    } else {
                        cached->second.route_id = -1;
                    }
                }
            }

            const std::uint32_t old_route_id = store.route_ids_[i];
            if (cached->second.route_id < 0) {
                ++result.stats.reroute_failed;
                result.reroutes.push_back({
                    now, store.ids_[i], old_route_id, old_route_id, false});
                continue;
            }
            const std::uint32_t new_route_id =
                static_cast<std::uint32_t>(cached->second.route_id);
            store.route_ids_[i] = new_route_id;
            store.route_indices_[i] = 0;
            result.vehicles[i].route_id = new_route_id;
            ++result.stats.reroute_succeeded;
            switched_any = true;
            result.reroutes.push_back({
                now, store.ids_[i], old_route_id, new_route_id, true});
        }
        return switched_any;
    };

    std::uint64_t stalled_ticks = 0;
    std::uint64_t tick = 0;
    std::size_t control_cursor = 0;
    std::chrono::steady_clock::duration barrier_wait{};
    for (; tick < total_ticks; ++tick) {
        const double now = tick * step;
        if (run_control != nullptr) {
            const auto wait_start = std::chrono::steady_clock::now();
            const bool continue_run = run_control->waitForTick(tick, now);
            barrier_wait += std::chrono::steady_clock::now() - wait_start;
            if (!continue_run) {
                result.stats.cancelled = true;
                break;
            }
        }

        bool control_activity = false;
        bool topology_restricted = false;
        std::fill(changed_routing_edges.begin(), changed_routing_edges.end(), 0);
        std::fill(explicit_cost_edges.begin(), explicit_cost_edges.end(), 0);
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
                        topology_restricted = topology_restricted ||
                                              !edge_closed[control.target_id];
                        edge_closed[control.target_id] = true;
                        routing_edge_enabled[control.target_id] = 0;
                    } else if (control.action == ControlAction::kOpen) {
                        edge_closed[control.target_id] = false;
                        routing_edge_enabled[control.target_id] =
                            junction_closed[data.edges[control.target_id].from] ? 0 : 1;
                    } else if (control.action == ControlAction::kSetSpeedFactor) {
                        edge_speed_factor[control.target_id] = control.value;
                        explicit_cost_edges[control.target_id] = 1;
                    } else {
                        edge_capacity_factor[control.target_id] = control.value;
                        explicit_cost_edges[control.target_id] = 1;
                    }
                    ++result.stats.edge_control_events;
                    break;
                case ControlScope::kJunction:
                    topology_restricted = topology_restricted ||
                                          (control.action == ControlAction::kClose &&
                                           !junction_closed[control.target_id]);
                    junction_closed[control.target_id] =
                        control.action == ControlAction::kClose;
                    for (const zeus::map::EdgeIndex edge :
                         runtime_.outgoingEdges(control.target_id)) {
                        routing_edge_enabled[edge] =
                            junction_closed[control.target_id] || edge_closed[edge] ? 0 : 1;
                    }
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

        const bool has_explicit_cost_event = std::any_of(
            explicit_cost_edges.begin(), explicit_cost_edges.end(),
            [](std::uint8_t value) { return value != 0; });
        const bool periodic_cost_scan = now + 1e-9 >= next_reroute_scan_s;
        if (topology_restricted || has_explicit_cost_event || periodic_cost_scan) {
            refreshRoutingCosts();
            for (std::size_t edge = 0; edge < edge_count; ++edge) {
                const double old_factor = published_edge_cost_factors[edge];
                const double new_factor = routing_edge_cost_factors[edge];
                const double ratio = std::max(
                    new_factor / old_factor, old_factor / new_factor);
                if ((explicit_cost_edges[edge] != 0 &&
                     std::abs(new_factor - old_factor) > 1e-9) ||
                    (periodic_cost_scan &&
                     ratio + 1e-9 >= result.config.reroute_cost_ratio)) {
                    changed_routing_edges[edge] = 1;
                    published_edge_cost_factors[edge] = new_factor;
                }
            }
            if (periodic_cost_scan) {
                do {
                    next_reroute_scan_s += result.config.reroute_interval_seconds;
                } while (next_reroute_scan_s <= now + 1e-9);
            }
            const bool has_changed_route_cost = std::any_of(
                changed_routing_edges.begin(), changed_routing_edges.end(),
                [](std::uint8_t value) { return value != 0; });
            if (topology_restricted || has_changed_route_cost) {
                control_activity = rerouteAffectedVehicles(now) || control_activity;
            }
        }

        if (driving_count == 0 && pending_entries == 0) {
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
            --pending_entries;
            ++driving_count;
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
                        if (!canExit(edge_index, event_time)) {
                            remaining = 0.0;  // wait for the discharge headway
                            break;
                        }
                        store.states_[i] = VehicleState::kArrived;
                        --driving_count;
                        delta_out[edge_index] -= 1;
                        store.arrives_[i] = event_time;
                        result.vehicles[i].arrive_s = event_time;
                        last_exit_s[edge_index] = event_time;
                        recordSample(i, event_time, edge_index, edge_end);
                        moved = true;
                        break;
                    }
                    const double event_time = now + (step - remaining);
                    if (!canExit(edge_index, event_time)) {
                        remaining = 0.0;  // wait for the discharge headway
                        break;
                    }
                    const zeus::map::EdgeIndex next = route.edges[route_index + 1];
                    const SignalGate signal_gate =
                        signalGate(edge.to, edge_index, next, event_time);
                    if (signal_gate != SignalGate::kOpen) {
                        ++result.stats.signal_wait_events;
                        if (signal_gate == SignalGate::kRed) {
                            ++result.stats.signal_red_wait_events;
                        } else {
                            ++result.stats.signal_saturation_wait_events;
                        }
                        control_activity = true;
                        remaining = 0.0;  // wait for green or discharge headway
                        break;
                    }
                    if (edge_closed[next] || junction_closed[edge.to] ||
                        occupancy[next] + delta_in[next] >= effectiveCapacity(next)) {
                        remaining = 0.0;  // queue at the boundary
                        break;
                    }
                    delta_out[edge_index] -= 1;
                    delta_in[next] += 1;
                    last_exit_s[edge_index] = event_time;
                    recordSignalPass(edge.to, edge_index, next, event_time);
                    store.route_indices_[i] = route_index + 1;
                    store.offsets_[i] = 0.0;
                    recordSample(i, event_time, next, 0.0);
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
                        if (!canExit(edge_index, event_time)) {
                            remaining = 0.0;  // wait for the discharge headway
                            break;
                        }
                        store.states_[i] = VehicleState::kArrived;
                        --driving_count;
                        delta_out[edge_index] -= 1;
                        store.arrives_[i] = event_time;
                        result.vehicles[i].arrive_s = event_time;
                        last_exit_s[edge_index] = event_time;
                        recordSample(i, event_time, edge_index, edge_end);
                        break;
                    }
                    if (!canExit(edge_index, event_time)) {
                        remaining = 0.0;  // wait for the discharge headway
                        break;
                    }
                    const zeus::map::EdgeIndex next = route.edges[route_index + 1];
                    const SignalGate signal_gate =
                        signalGate(edge.to, edge_index, next, event_time);
                    if (signal_gate != SignalGate::kOpen) {
                        ++result.stats.signal_wait_events;
                        if (signal_gate == SignalGate::kRed) {
                            ++result.stats.signal_red_wait_events;
                        } else {
                            ++result.stats.signal_saturation_wait_events;
                        }
                        control_activity = true;
                        remaining = 0.0;  // wait for green or discharge headway
                        break;
                    }
                    if (edge_closed[next] || junction_closed[edge.to] ||
                        occupancy[next] + delta_in[next] >= effectiveCapacity(next)) {
                        remaining = 0.0;  // queue at the boundary
                        break;
                    }
                    delta_out[edge_index] -= 1;
                    delta_in[next] += 1;
                    last_exit_s[edge_index] = event_time;
                    recordSignalPass(edge.to, edge_index, next, event_time);
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
    result.stats.barrier_wait_ms =
        std::chrono::duration<double, std::milli>(barrier_wait).count();
    result.stats.compute_ms = std::max(
        0.0,
        std::chrono::duration<double, std::milli>(
            end_time - start_time - barrier_wait).count());
    return result;
}

}  // namespace zeus::simulation
