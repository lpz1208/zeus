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
        zeus::simulation::PlaybackExporter::save(
            *setup.runtime, result, playback.string());
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
        runNonIntegralSamplingTest();
        runUnroutableDemandTest();
        runExportTest();
        std::cout << "all simulation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
