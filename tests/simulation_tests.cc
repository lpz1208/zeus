#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include <cpl_conv.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include "zeus/map/map_runtime.h"
#include "zeus/map/types.h"
#include "zeus/routing/route_planner.h"
#include "zeus/simulation/playback_exporter.h"
#include "zeus/simulation/simulation_engine.h"
#include "zeus/simulation/simulation_session.h"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test failure: " + message);
    }
}

bool near(double value, double expected, double epsilon = 1e-6) {
    return std::abs(value - expected) <= epsilon;
}

// Same hand-built MapData fixture style as the routing tests: every edge gets
// a two-point polyline so the runtime spatial index accepts it.
struct Fixture {
    zeus::map::MapData data;

    zeus::map::NodeIndex addNode(double x, double y) {
        data.nodes.push_back({1000 + data.nodes.size(), {x, y}});
        return static_cast<zeus::map::NodeIndex>(data.nodes.size() - 1);
    }

    zeus::map::EdgeIndex addEdge(zeus::map::NodeIndex from, zeus::map::NodeIndex to,
                                 double speed_mps, std::uint16_t lane_count = 1) {
        const zeus::map::Point2d a = data.nodes[from].point;
        const zeus::map::Point2d b = data.nodes[to].point;
        const std::uint32_t geometry_offset =
            static_cast<std::uint32_t>(data.geometry_points.size());
        data.geometry_points.push_back(a);
        data.geometry_points.push_back(b);
        zeus::map::DirectedEdge edge;
        edge.id = 5000 + data.edges.size();
        edge.road_id = 1;
        edge.from = from;
        edge.to = to;
        edge.geometry_offset = geometry_offset;
        edge.geometry_count = 2;
        edge.length_m = zeus::map::distance(a, b);
        edge.speed_limit_mps = static_cast<float>(speed_mps);
        edge.lane_count = lane_count;
        edge.source_id = "e" + std::to_string(data.edges.size());
        edge.road_class = "primary";
        data.edges.push_back(edge);
        return static_cast<zeus::map::EdgeIndex>(data.edges.size() - 1);
    }
};

struct SimSetup {
    std::unique_ptr<zeus::map::MapRuntime> runtime;
    std::unique_ptr<zeus::routing::RoutePlanner> planner;
    std::unique_ptr<zeus::simulation::SimulationEngine> engine;

    explicit SimSetup(const zeus::map::MapData& data)
        : runtime(std::make_unique<zeus::map::MapRuntime>(data)),
          planner(std::make_unique<zeus::routing::RoutePlanner>(*runtime)),
          engine(std::make_unique<zeus::simulation::SimulationEngine>(*runtime, *planner)) {}
};

zeus::simulation::VehicleDemand demand(double ox, double oy, double dx, double dy,
                                       double depart_s = 0.0) {
    zeus::simulation::VehicleDemand result;
    result.origin = {ox, oy};
    result.destination = {dx, dy};
    result.depart_time_s = depart_s;
    return result;
}

zeus::simulation::SimulationConfig quickConfig(double duration, double step = 1.0,
                                               double sample_interval = 30.0) {
    zeus::simulation::SimulationConfig config;
    config.duration_seconds = duration;
    config.step_seconds = step;
    config.sample_interval_seconds = sample_interval;
    return config;
}

void runStatefulSessionStepTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(1000.0, 0.0);
    fixture.addEdge(n0, n1, 10.0);

    SimSetup setup(fixture.data);
    const zeus::simulation::SimulationConfig config = quickConfig(200.0, 1.0, 10.0);
    const std::vector<zeus::simulation::VehicleDemand> demands = {
        demand(0.0, 0.5, 1000.0, 0.5),
    };
    const zeus::simulation::SimulationResult direct =
        setup.engine->run(config, demands);

    zeus::simulation::SimulationSession session(*setup.engine, config, demands);
    const zeus::simulation::SimulationSessionState initial = session.reset();
    require(initial.ready && initial.paused && !initial.finished,
            "stateful session resets at a paused tick-zero barrier");
    require(initial.tick == 0 && near(initial.simulation_time_s, 0.0) &&
                initial.state_version > 0,
            "initial session observation exposes tick, time and version");
    require(!session.hasResult(), "paused session has no final result");

    const zeus::simulation::SimulationSessionState after_two = session.step(2);
    require(after_two.ready && after_two.paused && !after_two.finished,
            "step pauses again at the requested committed boundary");
    require(after_two.tick == 2 && near(after_two.simulation_time_s, 2.0),
            "two steps advance exactly two seconds of simulation time");
    require(after_two.state_version == initial.state_version + 2,
            "every committed tick advances the state version");
    const zeus::simulation::SimulationSessionState observed = session.observe();
    require(observed.tick == after_two.tick &&
                observed.state_version == after_two.state_version,
            "observe is stable while the session is paused");

    const zeus::simulation::SimulationSessionState finished = session.runToEnd();
    require(finished.finished && finished.paused && !finished.cancelled,
            "runToEnd reaches a normal terminal state");
    require(session.hasResult(), "finished session exposes its result");
    const zeus::simulation::SimulationResult stepped = session.result();
    require(stepped.ok && !stepped.stats.cancelled && stepped.stats.arrived == 1,
            "stateful execution preserves successful arrival");
    require(stepped.stats.ticks_executed == direct.stats.ticks_executed &&
                near(stepped.vehicles.front().arrive_s,
                     direct.vehicles.front().arrive_s) &&
                near(stepped.vehicles.front().traveled_m,
                     direct.vehicles.front().traveled_m),
            "stepped and one-shot executions are equivalent");

    const std::uint64_t completed_version = finished.state_version;
    const zeus::simulation::SimulationSessionState reset_again = session.reset();
    require(reset_again.tick == 0 && reset_again.state_version > completed_version,
            "reset invalidates every state version from the previous run");
    session.close();
}

void runStatefulSessionCancellationTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(1000.0, 0.0);
    fixture.addEdge(n0, n1, 1.0);

    SimSetup setup(fixture.data);
    const std::vector<zeus::simulation::VehicleDemand> demands = {
        demand(0.0, 0.5, 1000.0, 0.5),
    };
    zeus::simulation::SimulationSession session(
        *setup.engine, quickConfig(2000.0), demands);
    const zeus::simulation::SimulationSessionState initial = session.reset();
    const zeus::simulation::SimulationSessionState after_one = session.step();
    require(after_one.tick == 1 &&
                after_one.state_version == initial.state_version + 1,
            "single-step session reaches its first committed boundary");
    const zeus::simulation::SimulationSessionState restarted = session.reset();
    require(restarted.tick == 0 && restarted.paused && !restarted.finished &&
                restarted.state_version > after_one.state_version,
            "reset safely replaces a paused worker and invalidates its version");
    static_cast<void>(session.step());
    session.close();
    const zeus::simulation::SimulationSessionState closed = session.observe();
    require(closed.finished && closed.cancelled && closed.paused,
            "closing a paused session cancels and joins its worker");
    require(session.hasResult() && session.result().stats.cancelled,
            "cancelled session preserves a partial auditable result");

    bool rejected = false;
    try {
        static_cast<void>(session.step());
    } catch (const std::logic_error&) {
        rejected = true;
    }
    require(rejected, "closed session rejects further stepping");
}

void runSingleVehicleArrivalTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 20.0);

    SimSetup setup(fixture.data);
    const zeus::simulation::SimulationResult result = setup.engine->run(
        quickConfig(100.0), {demand(10.0, 0.5, 90.0, 0.5)});

    require(result.ok, "single vehicle simulation succeeds");
    require(result.stats.vehicles_total == 1 && result.stats.arrived == 1,
            "single vehicle arrives");
    const auto& record = result.vehicles.front();
    require(near(record.arrive_s, 4.0), "arrival happens after 80 m at 20 m/s");
    require(near(record.traveled_m, 80.0), "traveled distance is the partial edge");
    require(record.samples.size() == 2, "entry and arrival samples are recorded");
    require(near(record.samples.front().t, 0.0) &&
                near(record.samples.front().offset_s, 10.0),
            "entry sample sits at the route start offset");
    require(near(record.samples.back().t, 4.0) &&
                near(record.samples.back().offset_s, 90.0),
            "arrival sample sits at the route end offset");
}

void runMultiEdgeTickTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(50.0, 0.0);
    const zeus::map::NodeIndex n2 = fixture.addNode(100.0, 0.0);
    const zeus::map::NodeIndex n3 = fixture.addNode(150.0, 0.0);
    fixture.addEdge(n0, n1, 50.0);
    fixture.addEdge(n1, n2, 50.0);
    fixture.addEdge(n2, n3, 50.0);

    SimSetup setup(fixture.data);
    const zeus::simulation::SimulationResult result =
        setup.engine->run(quickConfig(10.0), {demand(0.0, 0.5, 150.0, 0.5)});

    require(result.ok && result.stats.arrived == 1, "multi-edge vehicle arrives");
    const auto& record = result.vehicles.front();
    require(near(record.arrive_s, 3.0), "150 m at 50 m/s takes 3 s in one-tick crossings");
    bool saw_second_edge = false;
    bool saw_third_edge = false;
    for (const auto& sample : record.samples) {
        if (near(sample.t, 1.0) && sample.edge == 1) saw_second_edge = true;
        if (near(sample.t, 2.0) && sample.edge == 2) saw_third_edge = true;
    }
    require(saw_second_edge && saw_third_edge,
            "boundary samples record each crossed edge");
}

std::vector<zeus::simulation::VehicleDemand> identicalFleet(
    std::size_t count, double ox, double oy, double dx, double dy) {
    std::vector<zeus::simulation::VehicleDemand> demands;
    demands.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        demands.push_back(demand(ox, oy, dx, dy));
    }
    return demands;
}

void runCongestionSlowdownTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 20.0);

    SimSetup setup(fixture.data);
    // Edge capacity is floor(100 / 7) = 14. Every vehicle sees 13 others, so
    // the speed clamps to 15 percent from the first movement tick.
    const zeus::simulation::SimulationResult result = setup.engine->run(
        quickConfig(600.0), identicalFleet(14, 10.0, 0.5, 90.0, 0.5));

    require(result.ok && result.stats.arrived == 14, "congested fleet all arrives");
    require(result.stats.route_plans == 1, "identical demands share one pooled route");
    require(std::abs(result.stats.max_travel_s - (80.0 / 3.0)) < 0.5,
            "jamming clamps the fleet speed to 15 percent");
    require(result.stats.max_travel_s > 4.0, "congestion is slower than free flow");
}

void runQueueAtEntryTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 20.0);

    SimSetup setup(fixture.data);
    const zeus::simulation::SimulationResult result = setup.engine->run(
        quickConfig(600.0), identicalFleet(15, 10.0, 0.5, 90.0, 0.5));

    require(result.ok && result.stats.arrived == 15, "queued fleet all arrives");
    const auto& last_vehicle = result.vehicles.back();
    // The fleet leaves at t≈26.7; released slots commit at the next boundary,
    // so the 15th vehicle enters at tick 27.
    require(near(last_vehicle.actual_depart_s, 27.0, 1e-9),
            "the 15th vehicle enters once the jam releases a slot");
    require(result.stats.waiting_at_end == 0, "nobody is left waiting");
}

void runLaneScaledCapacityTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 20.0, 2);

    SimSetup setup(fixture.data);
    const zeus::simulation::SimulationResult result = setup.engine->run(
        quickConfig(100.0), identicalFleet(15, 10.0, 0.5, 90.0, 0.5));
    require(result.ok && result.stats.arrived == 15, "two-lane fleet all arrives");
    require(near(result.vehicles.back().actual_depart_s, 0.0),
            "lane count scales capacity so the 15th vehicle enters immediately");
}

void runSpillbackTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(70.0, 0.0);
    const zeus::map::NodeIndex n2 = fixture.addNode(77.0, 0.0);
    const zeus::map::NodeIndex n3 = fixture.addNode(147.0, 0.0);
    fixture.addEdge(n0, n1, 10.0);   // capacity 10
    fixture.addEdge(n1, n2, 10.0);   // capacity 1: the bottleneck
    fixture.addEdge(n2, n3, 10.0);   // capacity 10
    const double free_flow_s = 147.0 / 10.0;

    SimSetup setup(fixture.data);
    const zeus::simulation::SimulationResult result = setup.engine->run(
        quickConfig(600.0), identicalFleet(5, 0.0, 0.5, 147.0, 0.5));

    require(result.ok && result.stats.arrived == 5, "spillback fleet all arrives");
    require(result.stats.max_travel_s > free_flow_s * 1.2,
            "the bottleneck delays the last vehicles beyond free flow");
    require(result.stats.max_travel_s > result.stats.min_travel_s,
            "earlier vehicles travel faster than the queued ones");
}

void runExitHeadwayTest() {
    // Two vehicles drive in lockstep on one wide edge and would arrive at the
    // same moment; the exit headway gate must stagger their arrivals.
    const auto runPair = [](double headway_ff) {
        Fixture fixture;
        const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
        const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
        fixture.addEdge(n0, n1, 10.0);
        SimSetup setup(fixture.data);
        zeus::simulation::SimulationConfig config = quickConfig(600.0);
        config.exit_headway_ff_s = headway_ff;
        config.exit_headway_jam_s = headway_ff;
        return setup.engine->run(config, identicalFleet(2, 10.0, 0.5, 90.0, 0.5));
    };

    const zeus::simulation::SimulationResult unthrottled = runPair(0.0);
    require(unthrottled.ok && unthrottled.stats.arrived == 2,
            "unthrottled pair arrives");
    require(unthrottled.vehicles[0].arrive_s > 8.0 - 1e-9 &&
                unthrottled.vehicles[0].arrive_s < 10.0,
            "unthrottled travel stays near the free-flow time");
    require(near(unthrottled.vehicles[0].arrive_s, unthrottled.vehicles[1].arrive_s),
            "unthrottled lockstep pair arrives together");

    const zeus::simulation::SimulationResult throttled = runPair(2.0);
    require(throttled.ok && throttled.stats.arrived == 2,
            "throttled pair arrives");
    require(throttled.vehicles[1].arrive_s >=
                throttled.vehicles[0].arrive_s + 2.0 - 1e-9,
            "the second discharge waits for the headway");
    require(throttled.stats.max_travel_s > throttled.stats.min_travel_s,
            "the headway gate staggers the fleet");
}

void runClosureRerouteTest() {
    Fixture fixture;
    const auto n0 = fixture.addNode(0.0, 0.0);
    const auto n1 = fixture.addNode(100.0, 0.0);
    const auto n2 = fixture.addNode(200.0, 0.0);
    const auto n3 = fixture.addNode(100.0, 100.0);
    const auto n4 = fixture.addNode(300.0, 0.0);
    const auto first = fixture.addEdge(n0, n1, 10.0);
    const auto blocked = fixture.addEdge(n1, n2, 10.0);
    const auto detour_a = fixture.addEdge(n1, n3, 10.0);
    const auto detour_b = fixture.addEdge(n3, n2, 10.0);
    const auto goal = fixture.addEdge(n2, n4, 10.0);

    SimSetup setup(fixture.data);
    const std::vector<zeus::simulation::SimulationControlEvent> controls = {
        {1.0, zeus::simulation::ControlScope::kEdge, blocked,
         zeus::simulation::ControlAction::kClose, 1.0},
    };
    const auto result = setup.engine->run(
        quickConfig(120.0, 1.0, 5.0),
        {demand(10.0, 0.5, 290.0, 0.5)}, controls);

    require(result.ok && result.stats.arrived == 1,
            "vehicle arrives after a future edge closes");
    require(result.stats.reroute_attempts == 1 &&
                result.stats.reroute_succeeded == 1 &&
                result.stats.reroute_failed == 0,
            "closure reroute statistics record one success");
    require(result.reroutes.size() == 1 && result.reroutes[0].success &&
                near(result.reroutes[0].time_s, 1.0),
            "closure reroute record preserves event time and outcome");
    require(result.vehicles[0].route_id == result.reroutes[0].new_route_id &&
                result.vehicles[0].route_id != result.reroutes[0].old_route_id,
            "vehicle switches to the newly planned route");
    const auto& route = result.routes[result.vehicles[0].route_id];
    require(route.edges == std::vector<zeus::map::EdgeIndex>{
                               first, detour_a, detour_b, goal},
            "new route avoids the closed edge without leaving the current edge");
}

void runClosureRerouteFailureTest() {
    Fixture fixture;
    const auto n0 = fixture.addNode(0.0, 0.0);
    const auto n1 = fixture.addNode(100.0, 0.0);
    const auto n2 = fixture.addNode(200.0, 0.0);
    const auto first = fixture.addEdge(n0, n1, 10.0);
    const auto blocked = fixture.addEdge(n1, n2, 10.0);

    SimSetup setup(fixture.data);
    const std::vector<zeus::simulation::SimulationControlEvent> controls = {
        {1.0, zeus::simulation::ControlScope::kEdge, blocked,
         zeus::simulation::ControlAction::kClose, 1.0},
    };
    auto config = quickConfig(8.0, 1.0, 2.0);
    config.deadlock_probe_ticks = 20;
    const auto result = setup.engine->run(
        config, {demand(10.0, 0.5, 190.0, 0.5)}, controls);

    require(result.ok && result.stats.arrived == 0 &&
                result.stats.driving_at_end == 1,
            "vehicle remains on its current route when no detour exists");
    require(result.stats.reroute_attempts == 1 &&
                result.stats.reroute_succeeded == 0 &&
                result.stats.reroute_failed == 1,
            "closure reroute statistics record one failure");
    require(result.reroutes.size() == 1 && !result.reroutes[0].success &&
                result.reroutes[0].old_route_id == result.reroutes[0].new_route_id,
            "failed reroute record retains the original route");
    require(result.routes[result.vehicles[0].route_id].edges ==
                std::vector<zeus::map::EdgeIndex>{first, blocked},
            "failed reroute does not mutate the original route");
}

void runDynamicWeightControlRerouteTest() {
    const auto run = [](zeus::simulation::ControlAction action) {
        Fixture fixture;
        const auto n0 = fixture.addNode(0.0, 0.0);
        const auto n1 = fixture.addNode(100.0, 0.0);
        const auto n2 = fixture.addNode(200.0, 0.0);
        const auto n3 = fixture.addNode(100.0, 100.0);
        const auto n4 = fixture.addNode(300.0, 0.0);
        const auto first = fixture.addEdge(n0, n1, 10.0);
        const auto penalized = fixture.addEdge(n1, n2, 10.0);
        const auto detour_a = fixture.addEdge(n1, n3, 10.0);
        const auto detour_b = fixture.addEdge(n3, n2, 10.0);
        const auto goal = fixture.addEdge(n2, n4, 10.0);

        SimSetup setup(fixture.data);
        const std::vector<zeus::simulation::SimulationControlEvent> controls = {
            {1.0, zeus::simulation::ControlScope::kEdge, penalized, action, 0.2},
        };
        const auto result = setup.engine->run(
            quickConfig(120.0, 1.0, 5.0),
            {demand(10.0, 0.5, 290.0, 0.5)}, controls);

        require(result.ok && result.stats.arrived == 1,
                "vehicle arrives after a dynamic edge cost event");
        require(result.stats.reroute_attempts == 1 &&
                    result.stats.reroute_succeeded == 1,
                "dynamic edge cost event triggers one successful reroute");
        const auto& route = result.routes[result.vehicles[0].route_id];
        require(route.edges == std::vector<zeus::map::EdgeIndex>{
                                   first, detour_a, detour_b, goal},
                "dynamic edge cost route uses the cheaper detour");
    };

    run(zeus::simulation::ControlAction::kSetSpeedFactor);
    run(zeus::simulation::ControlAction::kSetCapacityFactor);
}

void runPeriodicCongestionRerouteTest() {
    Fixture fixture;
    const auto n0 = fixture.addNode(0.0, 0.0);
    const auto n1 = fixture.addNode(70.0, 0.0);
    const auto n2 = fixture.addNode(140.0, 0.0);
    const auto n3 = fixture.addNode(70.0, 140.0);
    const auto n4 = fixture.addNode(210.0, 0.0);
    fixture.addEdge(n0, n1, 10.0);  // capacity 10
    const auto congested = fixture.addEdge(n1, n2, 10.0);  // capacity 10
    const auto detour_a = fixture.addEdge(n1, n3, 10.0);
    const auto detour_b = fixture.addEdge(n3, n2, 10.0);
    const auto goal = fixture.addEdge(n2, n4, 10.0);

    SimSetup setup(fixture.data);
    auto config = quickConfig(180.0, 1.0, 5.0);
    config.reroute_interval_seconds = 1.0;
    config.reroute_cost_ratio = 1.25;
    const auto result = setup.engine->run(
        config, identicalFleet(11, 0.0, 0.5, 210.0, 0.5));

    require(result.ok && result.stats.arrived == 11,
            "periodic congestion scenario completes");
    require(result.stats.reroute_succeeded >= 1,
            "a material live occupancy change triggers a reroute");
    const auto& last_route = result.routes[result.vehicles.back().route_id];
    require(std::find(last_route.edges.begin(), last_route.edges.end(), congested) ==
                last_route.edges.end() &&
                std::find(last_route.edges.begin(), last_route.edges.end(), detour_a) !=
                    last_route.edges.end() &&
                std::find(last_route.edges.begin(), last_route.edges.end(), detour_b) !=
                    last_route.edges.end() &&
                last_route.edges.back() == goal,
            "the queued vehicle avoids the live congested branch");
}

void runDeterminismTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 20.0);

    SimSetup setup(fixture.data);
    const auto fleet = identicalFleet(15, 10.0, 0.5, 90.0, 0.5);
    const zeus::simulation::SimulationResult first = setup.engine->run(
        quickConfig(600.0), fleet);
    const zeus::simulation::SimulationResult second = setup.engine->run(
        quickConfig(600.0), fleet);

    require(first.stats.arrived == second.stats.arrived &&
                first.stats.sample_count == second.stats.sample_count &&
                near(first.stats.max_travel_s, second.stats.max_travel_s, 0.0),
            "repeated runs agree on aggregate statistics");
    for (std::size_t i = 0; i < first.vehicles.size(); ++i) {
        const auto& a = first.vehicles[i];
        const auto& b = second.vehicles[i];
        require(near(a.arrive_s, b.arrive_s, 0.0) &&
                    near(a.actual_depart_s, b.actual_depart_s, 0.0) &&
                    a.samples.size() == b.samples.size(),
                "repeated runs are identical per vehicle");
        for (std::size_t s = 0; s < a.samples.size(); ++s) {
            require(a.samples[s].t == b.samples[s].t &&
                        a.samples[s].edge == b.samples[s].edge &&
                        a.samples[s].offset_s == b.samples[s].offset_s,
                    "repeated runs produce identical samples");
        }
    }
}

void runUnfinishedTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 20.0);

    SimSetup setup(fixture.data);
    const zeus::simulation::SimulationResult result =
        setup.engine->run(quickConfig(2.0), {demand(10.0, 0.5, 90.0, 0.5)});

    require(result.ok, "unfinished simulation still succeeds");
    require(result.stats.arrived == 0 && result.stats.driving_at_end == 1,
            "the vehicle is still driving when the horizon ends");
    require(std::isnan(result.vehicles.front().arrive_s), "arrival stays unset");
    require(!result.vehicles.front().samples.empty(), "partial samples exist");
}

void runFutureDepartureDoesNotDeadlockTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 20.0);

    SimSetup setup(fixture.data);
    auto config = quickConfig(30.0);
    config.deadlock_probe_ticks = 3;
    const zeus::simulation::SimulationResult result = setup.engine->run(
        config, {demand(10.0, 0.5, 90.0, 0.5, 10.0)});
    require(result.ok && !result.stats.deadlock,
            "a scheduled future departure does not trigger deadlock detection");
    require(result.stats.arrived == 1 && near(result.vehicles.front().actual_depart_s, 10.0),
            "the delayed vehicle enters at its requested tick");
}

void runVehicleControlTest() {
    Fixture fixture;
    const auto n0 = fixture.addNode(0.0, 0.0);
    const auto n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 10.0);
    SimSetup setup(fixture.data);
    using zeus::simulation::ControlAction;
    using zeus::simulation::ControlScope;
    const std::vector<zeus::simulation::SimulationControlEvent> controls = {
        {0.0, ControlScope::kVehicle, 0, ControlAction::kHold, 1.0},
        {0.0, ControlScope::kVehicle, 0, ControlAction::kSetSpeedFactor, 0.5},
        {3.0, ControlScope::kVehicle, 0, ControlAction::kRelease, 1.0},
    };
    const auto result = setup.engine->run(
        quickConfig(60.0), {demand(0.0, 0.5, 100.0, 0.5)}, controls);
    require(result.stats.arrived == 1, "released controlled vehicle arrives");
    require(near(result.vehicles[0].actual_depart_s, 3.0),
            "held vehicle enters at release tick");
    require(near(result.vehicles[0].arrive_s, 23.0),
            "vehicle speed factor persists after release");
    require(result.stats.control_events_applied == 3 &&
                result.stats.vehicle_control_events == 3,
            "vehicle controls are counted");
}

void runEdgeControlTest() {
    Fixture fixture;
    const auto n0 = fixture.addNode(0.0, 0.0);
    const auto n1 = fixture.addNode(100.0, 0.0);
    const auto n2 = fixture.addNode(200.0, 0.0);
    fixture.addEdge(n0, n1, 10.0);
    fixture.addEdge(n1, n2, 10.0);
    SimSetup setup(fixture.data);
    using zeus::simulation::ControlAction;
    using zeus::simulation::ControlScope;
    const std::vector<zeus::simulation::SimulationControlEvent> controls = {
        {0.0, ControlScope::kEdge, 1, ControlAction::kClose, 1.0},
        {15.0, ControlScope::kEdge, 1, ControlAction::kOpen, 1.0},
    };
    const auto result = setup.engine->run(
        quickConfig(60.0), {demand(0.0, 0.5, 200.0, 0.5)}, controls);
    require(result.stats.arrived == 1 && near(result.vehicles[0].arrive_s, 25.0),
            "closed downstream edge queues the vehicle until reopening");
    require(result.stats.edge_control_events == 2,
            "edge closure and reopening are counted");

    const std::vector<zeus::simulation::SimulationControlEvent> slow = {
        {0.0, ControlScope::kEdge, 0, ControlAction::kSetSpeedFactor, 0.5},
        {0.0, ControlScope::kEdge, 0, ControlAction::kSetCapacityFactor, 0.5},
    };
    const auto slowed = setup.engine->run(
        quickConfig(60.0), {demand(0.0, 0.5, 100.0, 0.5)}, slow);
    require(slowed.stats.arrived == 1 && near(slowed.vehicles[0].arrive_s, 20.0),
            "edge speed factor changes traversal time");
}

void runJunctionControlTest() {
    Fixture fixture;
    const auto n0 = fixture.addNode(0.0, 0.0);
    const auto n1 = fixture.addNode(100.0, 0.0);
    const auto n2 = fixture.addNode(200.0, 0.0);
    fixture.addEdge(n0, n1, 10.0);
    fixture.addEdge(n1, n2, 10.0);
    SimSetup setup(fixture.data);
    using zeus::simulation::ControlAction;
    using zeus::simulation::ControlScope;
    const std::vector<zeus::simulation::SimulationControlEvent> controls = {
        {0.0, ControlScope::kJunction, n1, ControlAction::kClose, 1.0},
        {12.0, ControlScope::kJunction, n1, ControlAction::kOpen, 1.0},
    };
    const auto result = setup.engine->run(
        quickConfig(60.0), {demand(0.0, 0.5, 200.0, 0.5)}, controls);
    require(result.stats.arrived == 1 && near(result.vehicles[0].arrive_s, 22.0),
            "closed junction gates the outgoing transition until reopening");
    require(result.applied_controls.size() == 2 &&
                near(result.applied_controls[1].effective_time_s, 12.0),
            "junction control application is recorded");
}

void runTurnSignalPlanTest() {
    Fixture fixture;
    const auto west = fixture.addNode(-100.0, 0.0);
    const auto junction = fixture.addNode(0.0, 0.0);
    const auto east = fixture.addNode(100.0, 0.0);
    const auto south = fixture.addNode(0.0, -100.0);
    const auto north = fixture.addNode(0.0, 100.0);
    const auto west_in = fixture.addEdge(west, junction, 10.0);
    const auto east_out = fixture.addEdge(junction, east, 10.0);
    const auto south_in = fixture.addEdge(south, junction, 10.0);
    const auto north_out = fixture.addEdge(junction, north, 10.0);

    zeus::simulation::JunctionSignalPlan signal;
    signal.node = junction;
    signal.yellow_seconds = 2.0;
    signal.all_red_seconds = 1.0;
    signal.phases = {
        {5.0, {{west_in, east_out}}},
        {5.0, {{south_in, north_out}}},
    };

    SimSetup setup(fixture.data);
    const std::vector<zeus::simulation::VehicleDemand> demands = {
        demand(-100.0, 0.5, 100.0, 0.5),
        demand(0.5, -100.0, 0.5, 100.0),
    };
    const std::vector<zeus::simulation::SimulationControlEvent> controls;
    const std::vector<zeus::simulation::JunctionSignalPlan> signals = {signal};
    const auto result = setup.engine->run(
        quickConfig(60.0, 1.0, 2.0), demands, controls, signals);

    require(result.ok && result.stats.arrived == 2,
            "both signal-controlled movements eventually arrive");
    require(near(result.vehicles[1].arrive_s, 20.0),
            "the northbound movement crosses during its green phase");
    require(near(result.vehicles[0].arrive_s, 26.0),
            "the eastbound movement waits through the other phase and clearance");
    require(result.stats.signal_plans == 1 && result.stats.signal_phases == 2 &&
                result.stats.signal_wait_events > 0,
            "signal plan structure and red-phase waits are counted");
    require(result.stats.signal_red_wait_events == result.stats.signal_wait_events &&
                result.stats.signal_saturation_wait_events == 0 &&
                result.stats.signal_movements_passed == 2,
            "phase test classifies waits as red and counts successful crossings");
    require(result.signal_plans.size() == 1 &&
                result.signal_plans[0].phases.size() == 2,
            "effective signal plan is retained for playback export");

    auto invalid_signal = signal;
    invalid_signal.node = west;
    bool rejected = false;
    try {
        static_cast<void>(setup.engine->run(
            quickConfig(60.0), demands, controls,
            std::vector<zeus::simulation::JunctionSignalPlan>{invalid_signal}));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected,
            "signal movements that do not traverse the configured node are rejected");
}

void runSignalSaturationFlowTest() {
    Fixture fixture;
    const auto west = fixture.addNode(-100.0, 0.0);
    const auto junction = fixture.addNode(0.0, 0.0);
    const auto east = fixture.addNode(100.0, 0.0);
    const auto incoming = fixture.addEdge(west, junction, 10.0, 4);
    const auto outgoing = fixture.addEdge(junction, east, 10.0, 4);

    zeus::simulation::SignalPhase phase;
    phase.green_seconds = 100.0;
    phase.movements = {{incoming, outgoing}};
    phase.saturation_flow_vph = 1800.0;  // one vehicle every two seconds
    zeus::simulation::JunctionSignalPlan signal;
    signal.node = junction;
    signal.yellow_seconds = 0.0;
    signal.all_red_seconds = 0.0;
    signal.phases = {phase};

    SimSetup setup(fixture.data);
    const auto demands = identicalFleet(2, -100.0, 0.5, 100.0, 0.5);
    const std::vector<zeus::simulation::SimulationControlEvent> controls;
    const auto result = setup.engine->run(
        quickConfig(60.0, 1.0, 2.0), demands, controls,
        std::vector<zeus::simulation::JunctionSignalPlan>{signal});

    require(result.ok && result.stats.arrived == 2,
            "movement saturation scenario completes");
    double first_crossing = -1.0;
    double second_crossing = -1.0;
    for (const auto& sample : result.vehicles[0].samples) {
        if (sample.edge == outgoing && near(sample.offset_s, 0.0)) {
            first_crossing = sample.t;
            break;
        }
    }
    for (const auto& sample : result.vehicles[1].samples) {
        if (sample.edge == outgoing && near(sample.offset_s, 0.0)) {
            second_crossing = sample.t;
            break;
        }
    }
    require(first_crossing >= 0.0 &&
                second_crossing >= first_crossing + 2.0 - 1e-9,
            "1800 veh/h saturation flow enforces a two-second movement headway");
    require(result.stats.signal_red_wait_events == 0 &&
                result.stats.signal_saturation_wait_events > 0 &&
                result.stats.signal_wait_events ==
                    result.stats.signal_saturation_wait_events &&
                result.stats.signal_movements_passed == 2,
            "saturation waits are separated from red-phase waits");

    signal.phases[0].saturation_flow_vph = 10.0;
    bool rejected = false;
    try {
        static_cast<void>(setup.engine->run(
            quickConfig(60.0), demands, controls,
            std::vector<zeus::simulation::JunctionSignalPlan>{signal}));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "out-of-range movement saturation flow is rejected");
}

void runNonIntegralSamplingTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(1000.0, 0.0);
    fixture.addEdge(n0, n1, 1.0);

    SimSetup setup(fixture.data);
    const zeus::simulation::SimulationResult result = setup.engine->run(
        quickConfig(10.0, 2.0, 3.0), {demand(0.0, 0.5, 1000.0, 0.5)});
    require(result.ok && result.stats.driving_at_end == 1,
            "non-integral sampling simulation stays active");
    const auto& samples = result.vehicles.front().samples;
    require(samples.size() == 4, "entry plus three periodic samples are emitted");
    require(near(samples[1].t, 4.0) && near(samples[2].t, 6.0) &&
                near(samples[3].t, 10.0),
            "sampling uses the first tick boundary at or after each interval");
}

void runUnroutableDemandTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 20.0);
    const zeus::map::NodeIndex n2 = fixture.addNode(5000.0, 5000.0);
    const zeus::map::NodeIndex n3 = fixture.addNode(5100.0, 5000.0);
    fixture.addEdge(n2, n3, 20.0);

    SimSetup setup(fixture.data);
    const zeus::simulation::SimulationResult result = setup.engine->run(
        quickConfig(100.0),
        {demand(10.0, 0.5, 90.0, 0.5), demand(50.0, 1.0, 5050.0, 5001.0)});

    require(result.ok, "mixed routability is not a global failure");
    require(result.stats.unroutable == 1 && result.stats.arrived == 1,
            "one vehicle routes, one is reported unroutable");
    require(result.vehicles[1].samples.empty(), "unroutable vehicles never sample");

    const zeus::simulation::SimulationResult all_failed = setup.engine->run(
        quickConfig(100.0), {demand(50.0, 1.0, 5050.0, 5001.0)});
    require(!all_failed.ok && !all_failed.message.empty(),
            "a fully unroutable demand set fails with a message");
    require(all_failed.stats.vehicles_total == 1 && all_failed.stats.unroutable == 1,
            "a fully unroutable result still reports demand counts");
}

std::string webMercatorWkt() {
    OGRSpatialReference reference;
    reference.SetFromUserInput("EPSG:3857");
    char* wkt = nullptr;
    reference.exportToWkt(&wkt);
    std::string result(wkt);
    CPLFree(wkt);
    return result;
}

void runExportTest() {
    struct DatasetCloser {
        void operator()(GDALDataset* dataset) const {
            if (dataset != nullptr) {
                GDALClose(dataset);
            }
        }
    };

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("zeus-sim-test-" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::create_directories(directory);
    try {
        GDALAllRegister();

        Fixture fixture;
        const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
        const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
        fixture.addEdge(n0, n1, 20.0);
        fixture.data.metadata.runtime_crs_wkt = webMercatorWkt();

        SimSetup setup(fixture.data);
        const std::vector<zeus::simulation::SimulationControlEvent> controls = {
            {0.0, zeus::simulation::ControlScope::kVehicle, 0,
             zeus::simulation::ControlAction::kSetSpeedFactor, 1.0},
        };
        const zeus::simulation::SimulationResult result = setup.engine->run(
            quickConfig(600.0), identicalFleet(15, 10.0, 0.5, 90.0, 0.5), controls);
        require(result.ok && result.stats.arrived == 15, "exported simulation arrived");

        const std::filesystem::path trajectories = directory / "trajectories.geojson";
        const std::size_t features = zeus::simulation::TrajectoryExporter::save(
            *setup.runtime, result, trajectories.string());
        require(features == 15, "one trajectory feature per vehicle");

        std::unique_ptr<GDALDataset, DatasetCloser> dataset(static_cast<GDALDataset*>(
            GDALOpenEx(trajectories.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                       nullptr, nullptr, nullptr)));
        require(dataset != nullptr, "trajectory GeoJSON opens");
        OGRLayer* layer = dataset->GetLayer(0);
        require(layer->GetFeatureCount(TRUE) == 15, "trajectory feature count");
        OGRFeatureDefn* definition = layer->GetLayerDefn();
        for (const char* field :
             {"VEHICLE_ID", "DEPART_S", "ARRIVE_S", "TRAVEL_S", "DISTANCE_M"}) {
            require(definition->GetFieldIndex(field) >= 0,
                    std::string("trajectory export has field ") + field);
        }

        const std::filesystem::path playback = directory / "playback.json";
        zeus::simulation::SimulationResult playback_result = result;
        playback_result.reroutes.push_back({12.0, 3, 0, 1, true});
        playback_result.signal_plans.push_back({
            0, 2.0, 3.0, 1.0, {{20.0, {{0, 0}}}}});
        zeus::simulation::PlaybackExporter::save(
            *setup.runtime, playback_result, playback.string());
        std::ifstream input(playback);
        std::string content((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
        require(content.find("\"vehicles\"") != std::string::npos,
                "playback document lists vehicles");
        require(content.find("\"samples\"") != std::string::npos,
                "playback document lists samples");
        require(content.find("\"controls\"") != std::string::npos &&
                    content.find("\"action\": \"speed_factor\"") != std::string::npos,
                "playback document lists applied controls");
        require(content.find("\"reroutes\"") != std::string::npos &&
                    content.find("\"vehicle_id\": 3") != std::string::npos &&
                    content.find("\"success\": true") != std::string::npos,
                "playback document lists vehicle reroutes");
        require(content.find("\"reroute_interval_s\": 0.000") != std::string::npos &&
                    content.find("\"reroute_cost_ratio\": 1.250") != std::string::npos,
                "playback document preserves dynamic routing configuration");
        require(content.find("\"cancelled\": false") != std::string::npos &&
                    content.find("\"barrier_wait_ms\": 0.000") !=
                        std::string::npos &&
                    content.find("\"compute_ms\":") != std::string::npos,
                "playback separates barrier wait from simulation compute time");
        require(content.find("\"signal_plans\"") != std::string::npos &&
                    content.find("\"node_id\": 0") != std::string::npos &&
                    content.find("\"saturation_flow_vph\": 1800.000") !=
                        std::string::npos &&
                    content.find("\"movements\": [[0, 0]]") != std::string::npos,
                "playback document preserves turn-level signal plans");
        require(content.find("nan") == std::string::npos,
                "playback document contains no NaN values");
        int depth = 0;
        bool balanced = true;
        for (const char character : content) {
            if (character == '[' || character == '{') ++depth;
            if (character == ']' || character == '}') --depth;
            if (depth < 0) balanced = false;
        }
        require(balanced && depth == 0, "playback JSON brackets balance");

        std::filesystem::remove_all(directory);
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
}

}  // namespace

int main() {
    try {
        runStatefulSessionStepTest();
        runStatefulSessionCancellationTest();
        runSingleVehicleArrivalTest();
        runMultiEdgeTickTest();
        runCongestionSlowdownTest();
        runQueueAtEntryTest();
        runLaneScaledCapacityTest();
        runSpillbackTest();
        runDeterminismTest();
        runUnfinishedTest();
        runFutureDepartureDoesNotDeadlockTest();
        runVehicleControlTest();
        runEdgeControlTest();
        runJunctionControlTest();
        runTurnSignalPlanTest();
        runSignalSaturationFlowTest();
        runNonIntegralSamplingTest();
        runUnroutableDemandTest();
        runExportTest();
        runExitHeadwayTest();
        runClosureRerouteTest();
        runClosureRerouteFailureTest();
        runDynamicWeightControlRerouteTest();
        runPeriodicCongestionRerouteTest();
        std::cout << "all simulation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
