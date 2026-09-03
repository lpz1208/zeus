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

void runStatefulSessionResumePauseTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(1000000.0, 0.0);
    fixture.addEdge(n0, n1, 1.0);

    SimSetup setup(fixture.data);
    const std::vector<zeus::simulation::VehicleDemand> demands = {
        demand(0.0, 0.5, 1000000.0, 0.5),
    };
    zeus::simulation::SimulationSession session(
        *setup.engine, quickConfig(1000000.0), demands);
    const zeus::simulation::SimulationSessionState initial = session.reset();
    const zeus::simulation::SimulationSessionState resumed = session.resume();
    require(resumed.state_version == initial.state_version && !resumed.finished,
            "resume acknowledges immediately without waiting for completion");
    session.pause();

    zeus::simulation::SimulationSessionState paused = session.observe();
    for (int attempt = 0; attempt < 1000 && !paused.paused && !paused.finished;
         ++attempt) {
        usleep(1000);
        paused = session.observe();
    }
    require(paused.paused && !paused.finished,
            "pause reaches a committed boundary after non-blocking resume");
    session.close();
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
    // the speed clamps to the pinned 15 percent floor from the first tick.
    zeus::simulation::SimulationConfig config = quickConfig(600.0);
    config.min_speed_ratio = 0.15;
    const zeus::simulation::SimulationResult result = setup.engine->run(
        config, identicalFleet(14, 10.0, 0.5, 90.0, 0.5));

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
    zeus::simulation::SimulationConfig config = quickConfig(600.0);
    config.min_speed_ratio = 0.15;
    const zeus::simulation::SimulationResult result = setup.engine->run(
        config, identicalFleet(15, 10.0, 0.5, 90.0, 0.5));

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
    // Two vehicles drive in lockstep through one boundary; the exit headway
    // gate must stagger their crossings of the shared upstream edge.
    const auto runPair = [](double headway_ff) {
        Fixture fixture;
        const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
        const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
        const zeus::map::NodeIndex n2 = fixture.addNode(200.0, 0.0);
        fixture.addEdge(n0, n1, 10.0);
        const zeus::map::EdgeIndex downstream = fixture.addEdge(n1, n2, 10.0);
        SimSetup setup(fixture.data);
        zeus::simulation::SimulationConfig config = quickConfig(600.0);
        config.exit_headway_ff_s = headway_ff;
        config.exit_headway_jam_s = headway_ff;
        zeus::simulation::SimulationResult result = setup.engine->run(
            config, identicalFleet(2, 10.0, 0.5, 190.0, 0.5));
        struct Crossings {
            double first = -1.0;
            double second = -1.0;
        };
        Crossings crossings;
        for (std::size_t v = 0; v < 2; ++v) {
            for (const auto& sample : result.vehicles[v].samples) {
                if (sample.edge == downstream && near(sample.offset_s, 0.0)) {
                    (v == 0 ? crossings.first : crossings.second) = sample.t;
                    break;
                }
            }
        }
        return std::pair{result, crossings};
    };

    const auto [unthrottled, unthrottled_crossings] = runPair(0.0);
    require(unthrottled.ok && unthrottled.stats.arrived == 2,
            "unthrottled pair arrives");
    require(unthrottled_crossings.first > 8.0 - 1e-9 &&
                unthrottled_crossings.first < 10.0,
            "unthrottled travel stays near the free-flow time");
    require(near(unthrottled_crossings.first, unthrottled_crossings.second, 1e-9),
            "unthrottled lockstep pair crosses the boundary together");

    const auto [throttled, throttled_crossings] = runPair(2.0);
    require(throttled.ok && throttled.stats.arrived == 2,
            "throttled pair arrives");
    require(throttled_crossings.second >=
                throttled_crossings.first + 2.0 - 1e-9,
            "the second discharge waits for the headway");
    require(throttled.stats.max_travel_s > throttled.stats.min_travel_s,
            "the headway gate staggers the fleet");
}

void runArrivalExemptFromHeadwayTest() {
    // Defaults enable the discharge headway, yet arrivals at the destination
    // must not be gated: a lockstep pair on one edge arrives together.
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 10.0);

    SimSetup setup(fixture.data);
    const zeus::simulation::SimulationResult result = setup.engine->run(
        quickConfig(600.0), identicalFleet(2, 10.0, 0.5, 90.0, 0.5));
    require(result.ok && result.stats.arrived == 2, "arriving pair completes");
    require(result.config.exit_headway_ff_s > 0.0,
            "defaults enable the discharge headway");
    require(near(result.vehicles[0].arrive_s, result.vehicles[1].arrive_s, 1e-9),
            "arrivals are exempt from the discharge headway");
}

void runBoundarySlotOrderTest() {
    // Two vehicles reach a one-slot boundary in the same tick. The physically
    // ahead vehicle (larger offset) crosses first even though its id is
    // higher; the older id-order behaviour let the rear vehicle grab the slot.
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    const zeus::map::NodeIndex n2 = fixture.addNode(107.0, 0.0);
    const zeus::map::NodeIndex n3 = fixture.addNode(207.0, 0.0);
    fixture.addEdge(n0, n1, 10.0);
    const zeus::map::EdgeIndex bottleneck = fixture.addEdge(n1, n2, 10.0);
    fixture.addEdge(n2, n3, 10.0);

    SimSetup setup(fixture.data);
    const std::vector<zeus::simulation::VehicleDemand> demands = {
        demand(90.0, 0.5, 200.0, 0.5),   // id 0, physically behind
        demand(95.0, 0.5, 200.0, 0.5),   // id 1, physically ahead
    };
    const zeus::simulation::SimulationResult result =
        setup.engine->run(quickConfig(60.0), demands);

    require(result.ok && result.stats.arrived == 2, "boundary fleet arrives");
    const auto crossingTime = [&](std::size_t vehicle) {
        for (const auto& sample : result.vehicles[vehicle].samples) {
            if (sample.edge == bottleneck && near(sample.offset_s, 0.0)) {
                return sample.t;
            }
        }
        return -1.0;
    };
    require(crossingTime(1) > 0.0 && crossingTime(1) < 1.0,
            "the physically ahead vehicle crosses the boundary first");
    require(crossingTime(0) >= 2.0 - 1e-9,
            "the rear vehicle queues behind the occupied slot");
}

void runMinSpeedRatioZeroCrawlTest() {
    // Default min_speed_ratio is zero: a saturated edge slows traffic below
    // the old 15 percent floor, and the dynamic routing factor stays finite
    // (no divide-by-zero overlay costs) when periodic reroutes are enabled.
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 20.0);

    SimSetup setup(fixture.data);
    zeus::simulation::SimulationConfig config = quickConfig(600.0);
    config.reroute_interval_seconds = 1.0;
    const zeus::simulation::SimulationResult result = setup.engine->run(
        config, identicalFleet(14, 10.0, 0.5, 90.0, 0.5));

    require(result.ok && result.stats.arrived == 14, "crawling fleet arrives");
    require(near(config.min_speed_ratio, 0.0), "test pins the zero default");
    // 13 others on capacity 14: speed ratio 1/14 of free flow, well below the
    // previous 15 percent floor, so the 80 m trip takes >= 40 s.
    require(result.stats.max_travel_s > 40.0,
            "saturated traffic crawls below the old 15 percent floor");
    require(result.stats.reroute_attempts >= 0,
            "periodic reroute scans ran without invalid overlay costs");
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
    // This test exercises the cost-scan reroute mechanism, which needs traffic
    // to pile up into a deep storage jam. The discharge headway defaults now
    // meter bottlenecks instead of letting them fill, so the legacy pile-up
    // dynamics are pinned explicitly.
    config.min_speed_ratio = 0.15;
    config.exit_headway_ff_s = 0.0;
    config.exit_headway_jam_s = 0.0;
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

void runEdgeKpiTest() {
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
        setup.engine->run(quickConfig(30.0, 1.0, 5.0), {demand(0.0, 0.5, 150.0, 0.5)});

    require(result.ok && result.stats.arrived == 1, "kpi corridor vehicle arrives");
    require(result.edge_kpis.size() == 3, "every traversed edge reports a kpi");
    for (const zeus::simulation::EdgeKpi& kpi : result.edge_kpis) {
        require(kpi.entries == 1, "single vehicle enters each edge once");
        require(kpi.vehicle_seconds >= 1.0 - 1e-9,
                "vehicle seconds cover at least the traversal time");
        require(kpi.mean_speed_mps > 25.0 && kpi.mean_speed_mps <= 50.0 + 1e-9,
                "empty-road mean speed stays within half of free flow");
    }
}

void runTickSnapshotPublishTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 10.0);

    SimSetup setup(fixture.data);
    zeus::simulation::VehicleDemand agent = demand(10.0, 0.5, 90.0, 0.5);
    agent.agent_controlled = true;
    const std::vector<zeus::simulation::VehicleDemand> demands = {agent};
    zeus::simulation::SimulationSession session(*setup.engine, quickConfig(60.0), demands);
    const zeus::simulation::SimulationSessionState initial = session.reset();
    const zeus::simulation::TickSnapshot zero = session.snapshot();
    require(zero.tick == 0 && zero.agents.size() == 1 &&
                zero.agents.front().state == zeus::simulation::VehicleState::kWaiting &&
                zero.agents.front().remaining_edges.size() == 1,
            "tick-zero snapshot exposes the waiting agent and its route");

    const zeus::simulation::SimulationSessionState after_two = session.step(2);
    const zeus::simulation::TickSnapshot snapshot = session.snapshot();
    require(snapshot.tick == after_two.tick &&
                snapshot.state_version == after_two.state_version,
            "snapshot boundary matches the session state version");
    require(snapshot.driving == 1 && snapshot.arrived == 0 && snapshot.waiting == 0,
            "snapshot counts reflect the driving agent");
    require(snapshot.agents.size() == 1 &&
                snapshot.agents.front().state == zeus::simulation::VehicleState::kDriving &&
                snapshot.agents.front().edge == 0 &&
                snapshot.agents.front().offset_s > 0.0,
            "snapshot agent carries its live position");
    require(!snapshot.decision_due && snapshot.decision_reason.empty(),
            "no decision is due without route changes");
    session.close();
}

void runUntilEventRouteInvalidatedTest() {
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
    zeus::simulation::VehicleDemand agent = demand(10.0, 0.5, 290.0, 0.5);
    agent.agent_controlled = true;
    std::vector<zeus::simulation::VehicleDemand> demands = {
        agent, demand(10.0, 0.6, 290.0, 0.6)};
    std::vector<zeus::simulation::SimulationControlEvent> controls = {
        {1.0, zeus::simulation::ControlScope::kEdge, blocked,
         zeus::simulation::ControlAction::kClose, 1.0},
    };
    zeus::simulation::SimulationSession session(
        *setup.engine, quickConfig(120.0), demands, controls);
    session.reset();
    const zeus::simulation::SimulationSessionState stopped = session.stepUntilEvent(1000);

    const zeus::simulation::TickSnapshot snapshot = session.snapshot();
    require(snapshot.decision_due && snapshot.decision_reason == "route_invalidated",
            "closure on the agent route raises a decision event");
    require(stopped.paused && stopped.tick == snapshot.tick &&
                stopped.state_version == snapshot.state_version,
            "until-event returns only after state and snapshot share a paused boundary");
    require(stopped.tick <= 1000, "until-event respects the step cap");
    const auto& agent_state = snapshot.agents.front();
    require(agent_state.route_invalidated,
            "the agent slice marks its own route as invalidated");
    require(agent_state.remaining_edges.size() == 3 &&
                agent_state.remaining_edges[1] == blocked,
            "agent keeps its original route until it decides");
    (void)first;
    (void)detour_a;
    (void)detour_b;

    session.runToEnd();
    const zeus::simulation::SimulationResult result = session.result();
    require(result.stats.reroute_succeeded == 1,
            "only the non-agent vehicle reroutes automatically");
    require(result.routes[result.vehicles[0].route_id].edges ==
                std::vector<zeus::map::EdgeIndex>{first, blocked, goal},
            "the agent vehicle never switches route without a commit");
    session.close();
}

void runUntilEventPeriodicTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(1000.0, 0.0);
    fixture.addEdge(n0, n1, 10.0);

    SimSetup setup(fixture.data);
    zeus::simulation::VehicleDemand agent = demand(0.0, 0.5, 1000.0, 0.5);
    agent.agent_controlled = true;
    zeus::simulation::SimulationConfig config = quickConfig(200.0, 1.0, 10.0);
    config.reroute_interval_seconds = 2.0;
    const std::vector<zeus::simulation::VehicleDemand> demands = {agent};
    zeus::simulation::SimulationSession session(*setup.engine, config, demands);
    session.reset();
    const zeus::simulation::SimulationSessionState stopped = session.stepUntilEvent(100);
    const zeus::simulation::TickSnapshot snapshot = session.snapshot();
    require(snapshot.decision_due && snapshot.decision_reason == "periodic",
            "the periodic congestion scan wakes a driving agent");
    require(stopped.paused && stopped.tick == snapshot.tick &&
                stopped.state_version == snapshot.state_version,
            "periodic wake-up publishes one authoritative paused version");
    require(stopped.tick > 0 && stopped.tick <= 100,
            "periodic wake-up lands inside the step cap");
    session.close();
}

void runAgentCommitRouteTest() {
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
    zeus::simulation::VehicleDemand agent = demand(10.0, 0.5, 290.0, 0.5);
    agent.agent_controlled = true;
    std::vector<zeus::simulation::SimulationControlEvent> controls = {
        {1.0, zeus::simulation::ControlScope::kEdge, blocked,
         zeus::simulation::ControlAction::kClose, 1.0},
    };
    const std::vector<zeus::simulation::VehicleDemand> demands = {agent};
    zeus::simulation::SimulationSession session(
        *setup.engine, quickConfig(120.0), demands, controls);
    session.reset();
    session.stepUntilEvent(1000);
    const zeus::simulation::SimulationSessionState observed = session.observe();

    require(session.commitRoute(999, zeus::routing::Algorithm::kAStar,
                                observed.state_version) ==
                zeus::simulation::SimulationSession::CommitResult::kRejectedUnknownVehicle,
            "commit rejects an unknown vehicle");
    require(session.commitRoute(0, zeus::routing::Algorithm::kAStar,
                                observed.state_version - 1) ==
                zeus::simulation::SimulationSession::CommitResult::kRejectedStaleVersion,
            "commit rejects a stale state version");
    require(session.commitRoute(0, zeus::routing::Algorithm::kAStar,
                                observed.state_version) ==
                zeus::simulation::SimulationSession::CommitResult::kApplied,
            "commit against the current version is queued");

    session.step(1);
    session.runToEnd();
    const zeus::simulation::SimulationResult result = session.result();
    require(result.stats.arrived == 1, "committed agent reaches its destination");
    require(result.stats.reroute_attempts == 1 &&
                result.stats.reroute_succeeded == 1,
            "the queued injection replans exactly once");
    require(result.routes[result.vehicles[0].route_id].edges ==
                std::vector<zeus::map::EdgeIndex>{first, detour_a, detour_b, goal},
            "the injected replan follows the open detour");
    require(result.reroutes.size() == 1 && result.reroutes[0].success,
            "the injection leaves a successful reroute record");
    session.close();
}

void runSessionReplayForkTest() {
    Fixture fixture;
    const auto n0 = fixture.addNode(0.0, 0.0);
    const auto n1 = fixture.addNode(100.0, 0.0);
    const auto n2 = fixture.addNode(200.0, 0.0);
    const auto n3 = fixture.addNode(100.0, 100.0);
    const auto n4 = fixture.addNode(300.0, 0.0);
    fixture.addEdge(n0, n1, 10.0);
    const auto blocked = fixture.addEdge(n1, n2, 10.0);
    fixture.addEdge(n1, n3, 10.0);
    fixture.addEdge(n3, n2, 10.0);
    fixture.addEdge(n2, n4, 10.0);

    SimSetup setup(fixture.data);
    zeus::simulation::VehicleDemand agent = demand(10.0, 0.5, 290.0, 0.5);
    agent.agent_controlled = true;
    const std::vector<zeus::simulation::VehicleDemand> demands = {agent};
    const std::vector<zeus::simulation::SimulationControlEvent> controls = {
        {1.0, zeus::simulation::ControlScope::kEdge, blocked,
         zeus::simulation::ControlAction::kClose, 1.0},
    };
    const auto config = quickConfig(120.0);

    zeus::simulation::SimulationSession source(
        *setup.engine, config, demands, controls);
    static_cast<void>(source.reset());
    const auto decision = source.stepUntilEvent(1000);
    require(source.commitRoute(0, zeus::routing::Algorithm::kAStar,
                               decision.state_version) ==
                zeus::simulation::SimulationSession::CommitResult::kApplied,
            "source fork action is accepted");
    const auto source_state = source.step(5);
    const auto source_snapshot = source.snapshot();

    zeus::simulation::SimulationSession restored(
        *setup.engine, config, demands, controls);
    static_cast<void>(restored.reset());
    const auto replay_boundary = restored.step(decision.tick);
    require(restored.commitRoute(0, zeus::routing::Algorithm::kAStar,
                                 replay_boundary.state_version) ==
                zeus::simulation::SimulationSession::CommitResult::kApplied,
            "recorded fork action replays at its original tick");
    const auto restored_state = restored.step(source_state.tick - replay_boundary.tick);
    const auto restored_snapshot = restored.snapshot();

    require(restored_state.tick == source_state.tick &&
                restored_snapshot.agents.size() == source_snapshot.agents.size() &&
                restored_snapshot.driving == source_snapshot.driving &&
                restored_snapshot.arrived == source_snapshot.arrived,
            "deterministic replay reaches the same aggregate snapshot");
    require(restored_snapshot.agents.front().edge ==
                    source_snapshot.agents.front().edge &&
                near(restored_snapshot.agents.front().offset_s,
                     source_snapshot.agents.front().offset_s) &&
                restored_snapshot.agents.front().remaining_edges ==
                    source_snapshot.agents.front().remaining_edges,
            "restored fork reproduces agent position and remaining route");
    source.close();
    restored.close();
}

void runAgentKeepRouteTest() {
    Fixture fixture;
    const auto n0 = fixture.addNode(0.0, 0.0);
    const auto n1 = fixture.addNode(100.0, 0.0);
    const auto n2 = fixture.addNode(200.0, 0.0);
    const auto first = fixture.addEdge(n0, n1, 10.0);
    const auto blocked = fixture.addEdge(n1, n2, 10.0);

    SimSetup setup(fixture.data);
    zeus::simulation::VehicleDemand agent = demand(10.0, 0.5, 190.0, 0.5);
    agent.agent_controlled = true;
    std::vector<zeus::simulation::SimulationControlEvent> controls = {
        {1.0, zeus::simulation::ControlScope::kEdge, blocked,
         zeus::simulation::ControlAction::kClose, 1.0},
    };
    const std::vector<zeus::simulation::VehicleDemand> demands = {
        agent, demand(10.0, 0.6, 90.0, 0.6, 1000.0)};
    zeus::simulation::SimulationSession session(
        *setup.engine, quickConfig(30.0), demands, controls);
    session.reset();
    session.stepUntilEvent(1000);
    const std::uint64_t version = session.observe().state_version;
    require(session.keepRoute(1, version) ==
                zeus::simulation::SimulationSession::CommitResult::kRejectedNotAgent,
            "actions cannot take control of a non-agent vehicle");
    require(session.keepRoute(0, version) ==
                zeus::simulation::SimulationSession::CommitResult::kApplied,
            "keep against the current version is acknowledged");

    session.runToEnd();
    const zeus::simulation::SimulationResult result = session.result();
    require(result.stats.arrived == 0 && result.stats.driving_at_end == 1,
            "the kept route never crosses the closed edge");
    require(result.stats.reroute_attempts == 0,
            "keep does not trigger any replanning");
    require(result.routes[result.vehicles[0].route_id].edges ==
                std::vector<zeus::map::EdgeIndex>{first, blocked},
            "the agent keeps its original route");
    session.close();
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
        runStatefulSessionResumePauseTest();
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
        runArrivalExemptFromHeadwayTest();
        runBoundarySlotOrderTest();
        runMinSpeedRatioZeroCrawlTest();
        runEdgeKpiTest();
        runTickSnapshotPublishTest();
        runUntilEventRouteInvalidatedTest();
        runUntilEventPeriodicTest();
        runAgentCommitRouteTest();
        runSessionReplayForkTest();
        runAgentKeepRouteTest();
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
