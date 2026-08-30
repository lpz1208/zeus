#include <cmath>
#include <filesystem>
#include <iostream>
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
#include "zeus/routing/route_exporter.h"
#include "zeus/routing/route_planner.h"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test failure: " + message);
    }
}

bool near(double value, double expected, double epsilon = 1e-6) {
    return std::abs(value - expected) <= epsilon;
}

// Hand-built MapData fixture: every edge gets a two-point polyline so the
// runtime spatial index accepts it.
struct Fixture {
    zeus::map::MapData data;

    zeus::map::NodeIndex addNode(double x, double y) {
        data.nodes.push_back({1000 + data.nodes.size(), {x, y}});
        return static_cast<zeus::map::NodeIndex>(data.nodes.size() - 1);
    }

    zeus::map::EdgeIndex addEdge(
        zeus::map::NodeIndex from,
        zeus::map::NodeIndex to,
        double speed_mps,
        zeus::map::PersistentId road_id = 1,
        bool share_reversed_geometry = false) {
        const zeus::map::Point2d a = data.nodes[from].point;
        const zeus::map::Point2d b = data.nodes[to].point;
        std::uint32_t geometry_offset;
        if (share_reversed_geometry) {
            geometry_offset = data.edges.back().geometry_offset;
        } else {
            geometry_offset = static_cast<std::uint32_t>(data.geometry_points.size());
            data.geometry_points.push_back(a);
            data.geometry_points.push_back(b);
        }
        zeus::map::DirectedEdge edge;
        edge.id = 5000 + data.edges.size();
        edge.road_id = road_id;
        edge.from = from;
        edge.to = to;
        edge.geometry_offset = geometry_offset;
        edge.geometry_count = 2;
        edge.geometry_reversed = share_reversed_geometry;
        edge.length_m = zeus::map::distance(a, b);
        edge.speed_limit_mps = static_cast<float>(speed_mps);
        edge.source_id = "e" + std::to_string(data.edges.size());
        edge.road_class = "primary";
        data.edges.push_back(edge);
        return static_cast<zeus::map::EdgeIndex>(data.edges.size() - 1);
    }

    void addTurn(
        zeus::map::EdgeIndex from,
        zeus::map::EdgeIndex to,
        bool prohibited,
        float penalty_s = 0.0F) {
        data.turn_transitions.push_back({from, to, penalty_s, prohibited});
    }
};

// RoutePlanner keeps a reference to the runtime, so tests always pair the two
// in named locals to avoid dangling temporaries.
struct PlanSetup {
    std::unique_ptr<zeus::map::MapRuntime> runtime;
    std::unique_ptr<zeus::routing::RoutePlanner> planner;

    explicit PlanSetup(const zeus::map::MapData& data)
        : runtime(std::make_unique<zeus::map::MapRuntime>(data)),
          planner(std::make_unique<zeus::routing::RoutePlanner>(*runtime)) {}
};

zeus::routing::RouteRequest makeRequest(
    zeus::map::Point2d origin,
    zeus::map::Point2d destination,
    zeus::routing::Algorithm algorithm = zeus::routing::Algorithm::kDijkstra) {
    zeus::routing::RouteRequest request;
    request.origin = origin;
    request.destination = destination;
    request.algorithm = algorithm;
    request.max_snap_distance_m = 100.0;
    return request;
}

void runShortStraightRouteTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    const zeus::map::EdgeIndex e0 = fixture.addEdge(n0, n1, 20.0);

    PlanSetup setup(fixture.data);
    const zeus::routing::RouteResult result =
        setup.planner->plan(makeRequest({10.0, 0.5}, {90.0, 0.5}));

    require(result.ok, "short straight route succeeds");
    require(result.path.edges.size() == 1 && result.path.edges[0] == e0,
            "short straight route uses the only edge");
    require(near(result.path.start_offset_m, 10.0), "start offset matches the origin");
    require(near(result.path.end_offset_m, 90.0), "end offset matches the destination");
    require(near(result.stats.length_m, 80.0), "length covers the partial edge");
    require(near(result.stats.time_s, 4.0), "time covers the partial edge at 20 m/s");
    require(result.stats.expanded_nodes == 0, "direct combination skips the node search");
}

void runRoutingOverlayTest() {
    const auto run = [](bool turn_aware) {
        Fixture fixture;
        const auto n0 = fixture.addNode(0.0, 0.0);
        const auto n1 = fixture.addNode(100.0, 0.0);
        const auto n2 = fixture.addNode(200.0, 0.0);
        const auto n3 = fixture.addNode(100.0, 100.0);
        const auto n4 = fixture.addNode(300.0, 0.0);
        const auto e0 = fixture.addEdge(n0, n1, 20.0);
        const auto blocked = fixture.addEdge(n1, n2, 20.0);
        const auto detour_a = fixture.addEdge(n1, n3, 20.0);
        const auto detour_b = fixture.addEdge(n3, n2, 20.0);
        const auto goal = fixture.addEdge(n2, n4, 20.0);
        if (turn_aware) {
            fixture.addTurn(e0, blocked, false, 0.1F);
        }

        PlanSetup setup(fixture.data);
        std::vector<std::uint8_t> enabled(fixture.data.edges.size(), 1);
        enabled[e0] = 0;       // An exact origin may finish its current closed edge.
        enabled[blocked] = 0;  // It may not enter another closed edge.
        std::vector<double> factors(fixture.data.edges.size(), 1.0);
        const zeus::routing::RoutingOverlay overlay{enabled, factors};

        const std::vector<zeus::routing::Algorithm> algorithms = turn_aware
            ? std::vector<zeus::routing::Algorithm>{
                  zeus::routing::Algorithm::kDijkstra,
                  zeus::routing::Algorithm::kAStar}
            : std::vector<zeus::routing::Algorithm>{
                  zeus::routing::Algorithm::kDijkstra,
                  zeus::routing::Algorithm::kAStar,
                  zeus::routing::Algorithm::kBidirectionalDijkstra,
                  zeus::routing::Algorithm::kBidirectionalAStar};
        for (const auto algorithm : algorithms) {
            auto request = makeRequest({0.0, 0.0}, {0.0, 0.0}, algorithm);
            request.origin_position = zeus::routing::RoutePosition{e0, 50.0};
            request.destination_position = zeus::routing::RoutePosition{goal, 90.0};
            request.overlay = &overlay;
            const auto result = setup.planner->plan(request);
            require(result.ok, "dynamic overlay finds a legal detour");
            require(result.path.edges ==
                        std::vector<zeus::map::EdgeIndex>{e0, detour_a, detour_b, goal},
                    "dynamic overlay excludes closed edges without jumping off current edge");
            require(near(result.path.start_offset_m, 50.0) &&
                        near(result.path.end_offset_m, 90.0),
                    "exact route positions preserve offsets during dynamic routing");
        }
    };
    run(false);
    run(true);
}

// Fast detour versus slow shortcut: e1 is short but slow; e2+e3 is longer and
// fast. Time-optimal routing must prefer the detour.
Fixture makeDetourFixture(double slow_speed) {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    const zeus::map::NodeIndex n2 = fixture.addNode(0.0, 100.0);
    const zeus::map::NodeIndex n3 = fixture.addNode(-50.0, 0.0);
    const zeus::map::NodeIndex n4 = fixture.addNode(150.0, 0.0);
    fixture.addEdge(n3, n0, 30.0);        // e0
    fixture.addEdge(n0, n1, slow_speed);  // e1: the slow shortcut
    fixture.addEdge(n0, n2, 30.0);        // e2
    fixture.addEdge(n2, n1, 30.0);        // e3
    fixture.addEdge(n1, n4, 30.0);        // e4
    return fixture;
}

void runTimeVersusDistanceTest() {
    {
        Fixture fixture = makeDetourFixture(5.0);
        PlanSetup setup(fixture.data);
        const zeus::routing::RouteResult result =
            setup.planner->plan(makeRequest({-50.0, 1.0}, {150.0, 1.0}));

        require(result.ok, "detour route succeeds");
        require(result.path.edges.size() == 4, "fast route takes the e0,e2,e3,e4 detour");
        require(result.path.edges[0] == 0 && result.path.edges[1] == 2 &&
                    result.path.edges[2] == 3 && result.path.edges[3] == 4,
                "fast route avoids the slow shortcut e1");
        require(near(result.stats.time_s, 341.4213562 / 30.0, 1e-4),
                "detour time is 341.42 m at 30 m/s");
        require(near(result.stats.length_m, 341.4213562, 1e-3), "detour length");
    }
    {
        Fixture fixture = makeDetourFixture(30.0);
        PlanSetup setup(fixture.data);
        const zeus::routing::RouteResult result =
            setup.planner->plan(makeRequest({-50.0, 1.0}, {150.0, 1.0}));

        require(result.ok, "uniform-speed route succeeds");
        require(result.path.edges.size() == 3 && result.path.edges[1] == 1,
                "uniform speeds prefer the distance shortcut e1");
        require(near(result.stats.length_m, 200.0), "shortcut length is 200 m");
        require(near(result.stats.time_s, 200.0 / 30.0), "shortcut time at 30 m/s");
    }
}

void runUnreachableTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 20.0);
    const zeus::map::NodeIndex n2 = fixture.addNode(5000.0, 5000.0);
    const zeus::map::NodeIndex n3 = fixture.addNode(5100.0, 5000.0);
    fixture.addEdge(n2, n3, 20.0);

    PlanSetup setup(fixture.data);
    const zeus::routing::RouteResult result =
        setup.planner->plan(makeRequest({50.0, 1.0}, {5050.0, 5001.0}));

    require(!result.ok, "disconnected components cannot be routed");
    require(
        result.failure == zeus::routing::RouteFailure::kUnreachable,
        "disconnected route reports unreachable");
    require(result.stats.expanded_nodes == 1,
            "unreachable search settles only the reachable start side");
    require(!result.message.empty(), "unreachable route explains itself");
}

void runLoopSameEdgeBehindTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    const zeus::map::NodeIndex n2 = fixture.addNode(100.0, 100.0);
    const zeus::map::NodeIndex n3 = fixture.addNode(0.0, 100.0);
    fixture.addEdge(n0, n1, 10.0);
    fixture.addEdge(n1, n2, 10.0);
    fixture.addEdge(n2, n3, 10.0);
    fixture.addEdge(n3, n0, 10.0);

    PlanSetup setup(fixture.data);
    const zeus::routing::RouteResult result =
        setup.planner->plan(makeRequest({90.0, 1.0}, {10.0, 1.0}));

    require(result.ok, "same-edge-behind route succeeds");
    require(result.path.edges.size() == 5, "loop route repeats the first edge at the end");
    require(result.path.edges.front() == 0 && result.path.edges.back() == 0,
            "loop route starts and ends on the matched edge");
    require(near(result.stats.length_m, 320.0), "loop route length is 10+300+10");
    require(near(result.stats.time_s, 32.0), "loop route time at 10 m/s");
}

void runAlgorithmConsistencyTest() {
    const auto run = [](zeus::routing::Algorithm algorithm) {
        Fixture fixture = makeDetourFixture(5.0);
        PlanSetup setup(fixture.data);
        return setup.planner->plan(makeRequest({-50.0, 1.0}, {150.0, 1.0}, algorithm));
    };
    const zeus::routing::RouteResult dijkstra = run(zeus::routing::Algorithm::kDijkstra);
    const zeus::routing::RouteResult astar = run(zeus::routing::Algorithm::kAStar);

    require(dijkstra.ok && astar.ok, "both algorithms find the route");
    require(near(dijkstra.stats.time_s, astar.stats.time_s, 1e-9),
            "both algorithms agree on time");
    require(near(dijkstra.stats.length_m, astar.stats.length_m, 1e-9),
            "both algorithms agree on length");
    require(dijkstra.path.edges == astar.path.edges,
            "both algorithms pick the same edges");
    require(astar.stats.expanded_nodes <= dijkstra.stats.expanded_nodes,
            "A* expands no more nodes than Dijkstra");
}

void runUnmatchedEndpointTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 20.0);
    PlanSetup setup(fixture.data);

    const zeus::routing::RouteResult origin_result =
        setup.planner->plan(makeRequest({5000.0, 5000.0}, {50.0, 1.0}));
    require(
        !origin_result.ok &&
            origin_result.failure == zeus::routing::RouteFailure::kOriginUnmatched,
        "far-away origin reports unmatched");
    require(origin_result.stats.expanded_nodes == 0, "unmatched origin never searches");

    const zeus::routing::RouteResult destination_result =
        setup.planner->plan(makeRequest({50.0, 1.0}, {5000.0, 5000.0}));
    require(
        !destination_result.ok &&
            destination_result.failure == zeus::routing::RouteFailure::kDestinationUnmatched,
        "far-away destination reports unmatched");
}

void runTwinEdgeTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(200.0, 0.0);
    const zeus::map::EdgeIndex forward = fixture.addEdge(n0, n1, 20.0, 42);
    const zeus::map::EdgeIndex reverse = fixture.addEdge(n1, n0, 20.0, 42, true);
    require(reverse == forward + 1, "fixture sanity: reverse twin follows forward edge");

    PlanSetup setup(fixture.data);

    // Both endpoints tie between the twins; the smaller edge index must win.
    const zeus::routing::RouteResult forward_result =
        setup.planner->plan(makeRequest({50.0, 1.0}, {150.0, 1.0}));
    require(forward_result.ok, "twin direct route succeeds");
    require(forward_result.origin.edge == forward && forward_result.destination.edge == forward,
            "twin tie-break picks the forward edge");
    require(forward_result.path.edges.size() == 1 && forward_result.path.edges[0] == forward,
            "twin direct route stays on one edge");
    require(near(forward_result.stats.length_m, 100.0), "twin direct length");
    require(near(forward_result.stats.time_s, 5.0), "twin direct time at 20 m/s");

    // Destination behind the origin on the same bidirectional road: the twin
    // expansion turns this into a direct traversal of the reverse twin, no
    // detour to the road ends needed.
    const zeus::routing::RouteResult cross_result =
        setup.planner->plan(makeRequest({150.0, 1.0}, {50.0, 1.0}));
    require(cross_result.ok, "twin cross route succeeds");
    require(cross_result.path.edges.size() == 1 && cross_result.path.edges[0] == reverse,
            "twin cross route traverses the reverse twin directly");
    require(near(cross_result.path.start_offset_m, 50.0) &&
                near(cross_result.path.end_offset_m, 150.0),
            "twin cross offsets map onto the reverse twin");
    require(near(cross_result.stats.time_s, 5.0), "twin cross time is 100 m at 20 m/s");
    require(near(cross_result.stats.length_m, 100.0), "twin cross length");
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

void runRouteExportTest() {
    struct DatasetCloser {
        void operator()(GDALDataset* dataset) const {
            if (dataset != nullptr) {
                GDALClose(dataset);
            }
        }
    };

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("zeus-route-test-" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::create_directories(directory);
    try {
        GDALAllRegister();

        Fixture fixture = makeDetourFixture(5.0);
        fixture.data.metadata.runtime_crs_wkt = webMercatorWkt();
        PlanSetup setup(fixture.data);
        const zeus::routing::RouteResult result =
            setup.planner->plan(makeRequest({-50.0, 1.0}, {150.0, 1.0}));
        require(result.ok, "exported route succeeded");

        const std::filesystem::path output = directory / "route.geojson";
        const std::size_t features = zeus::routing::RouteGeoJsonExporter::save(
            setup.runtime->data(), result, output.string());
        require(features == 4, "route export writes one feature per traversed edge");

        std::unique_ptr<GDALDataset, DatasetCloser> dataset(static_cast<GDALDataset*>(
            GDALOpenEx(output.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                       nullptr, nullptr, nullptr)));
        require(dataset != nullptr, "exported route GeoJSON opens");
        OGRLayer* layer = dataset->GetLayer(0);
        require(layer->GetFeatureCount(TRUE) == 4, "exported route feature count");
        OGRFeatureDefn* definition = layer->GetLayerDefn();
        for (const char* field :
             {"ROAD_ID", "SOURCE_ID", "CLASS", "LENGTH_M", "EDGE_INDEX"}) {
            require(definition->GetFieldIndex(field) >= 0,
                    std::string("route export has field ") + field);
        }
        layer->ResetReading();
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> first(
            layer->GetNextFeature(), &OGRFeature::DestroyFeature);
        require(first != nullptr, "route export reads the first feature");
        require(near(first->GetFieldAsDouble("LENGTH_M"), 50.0),
                "first edge keeps its full length when the origin sits at its start");

        std::filesystem::remove_all(directory);
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
}

void runBidirectionalConsistencyTest() {
    const auto planOn = [](zeus::routing::Algorithm algorithm) {
        Fixture fixture = makeDetourFixture(5.0);
        PlanSetup setup(fixture.data);
        return setup.planner->plan(makeRequest({-50.0, 1.0}, {150.0, 1.0}, algorithm));
    };
    const zeus::routing::RouteResult dijkstra =
        planOn(zeus::routing::Algorithm::kDijkstra);
    const zeus::routing::RouteResult astar = planOn(zeus::routing::Algorithm::kAStar);
    const zeus::routing::RouteResult bidijkstra =
        planOn(zeus::routing::Algorithm::kBidirectionalDijkstra);
    const zeus::routing::RouteResult biastar =
        planOn(zeus::routing::Algorithm::kBidirectionalAStar);

    require(dijkstra.ok && astar.ok && bidijkstra.ok && biastar.ok,
            "all four algorithms find the route");
    for (const zeus::routing::RouteResult* result : {&astar, &bidijkstra, &biastar}) {
        require(near(result->stats.time_s, dijkstra.stats.time_s, 1e-9),
                "all algorithms agree on time");
        require(near(result->stats.length_m, dijkstra.stats.length_m, 1e-9),
                "all algorithms agree on length");
        require(result->path.edges == dijkstra.path.edges,
                "all algorithms pick the same edges");
    }
    require(bidijkstra.stats.expanded_nodes <= dijkstra.stats.expanded_nodes,
            "bidirectional Dijkstra expands no more nodes than Dijkstra");
}

void runBidirectionalLoopTest() {
    for (const zeus::routing::Algorithm algorithm :
         {zeus::routing::Algorithm::kBidirectionalDijkstra,
          zeus::routing::Algorithm::kBidirectionalAStar}) {
        Fixture fixture;
        const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
        const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
        const zeus::map::NodeIndex n2 = fixture.addNode(100.0, 100.0);
        const zeus::map::NodeIndex n3 = fixture.addNode(0.0, 100.0);
        fixture.addEdge(n0, n1, 10.0);
        fixture.addEdge(n1, n2, 10.0);
        fixture.addEdge(n2, n3, 10.0);
        fixture.addEdge(n3, n0, 10.0);

        PlanSetup setup(fixture.data);
        const zeus::routing::RouteResult result =
            setup.planner->plan(makeRequest({90.0, 1.0}, {10.0, 1.0}, algorithm));
        require(result.ok, "bidirectional loop route succeeds");
        require(result.path.edges.size() == 5, "bidirectional loop route repeats the edge");
        require(near(result.stats.length_m, 320.0), "bidirectional loop length");
        require(near(result.stats.time_s, 32.0), "bidirectional loop time");
    }
}

void runBidirectionalUnreachableTest() {
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    fixture.addEdge(n0, n1, 20.0);
    const zeus::map::NodeIndex n2 = fixture.addNode(5000.0, 5000.0);
    const zeus::map::NodeIndex n3 = fixture.addNode(5100.0, 5000.0);
    fixture.addEdge(n2, n3, 20.0);

    PlanSetup setup(fixture.data);
    const zeus::routing::RouteResult result = setup.planner->plan(
        makeRequest({50.0, 1.0}, {5050.0, 5001.0}, zeus::routing::Algorithm::kBidirectionalAStar));
    require(!result.ok, "bidirectional search cannot bridge components");
    require(
        result.failure == zeus::routing::RouteFailure::kUnreachable,
        "bidirectional search reports unreachable");
    require(result.stats.expanded_nodes == 1,
            "forward side exhausts and terminates before the backward side settles");
}

void runBidirectionalMeetingAtSharedNodeTest() {
    // Origin and destination snap to two different edges that share a node:
    // the route is prefix + suffix with no intermediate edges.
    Fixture fixture;
    const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
    const zeus::map::NodeIndex n1 = fixture.addNode(100.0, 0.0);
    const zeus::map::NodeIndex n2 = fixture.addNode(200.0, 0.0);
    fixture.addEdge(n0, n1, 20.0);  // e0
    fixture.addEdge(n1, n2, 20.0);  // e1

    for (const zeus::routing::Algorithm algorithm :
         {zeus::routing::Algorithm::kBidirectionalDijkstra,
          zeus::routing::Algorithm::kBidirectionalAStar}) {
        PlanSetup setup(fixture.data);
        const zeus::routing::RouteResult result =
            setup.planner->plan(makeRequest({80.0, 0.5}, {120.0, 0.5}, algorithm));
        require(result.ok, "shared-node route succeeds");
        require(result.origin.edge == 0 && result.destination.edge == 1,
                "endpoints snap to the two edges around the shared node");
        // prefix (80→100 = 20 m) + suffix (100→120 = 20 m) at 20 m/s = 2 s.
        require(near(result.stats.time_s, 2.0, 1e-9), "shared-node route is prefix + suffix");
        require(near(result.stats.length_m, 40.0, 1e-9), "shared-node route length");
        require(result.path.edges.size() == 2 && result.path.edges[0] == 0 &&
                    result.path.edges[1] == 1,
                "shared-node route keeps both endpoint edges");
    }
}

void runBidirectionalTwinTest() {
    for (const zeus::routing::Algorithm algorithm :
         {zeus::routing::Algorithm::kBidirectionalDijkstra,
          zeus::routing::Algorithm::kBidirectionalAStar}) {
        Fixture fixture;
        const zeus::map::NodeIndex n0 = fixture.addNode(0.0, 0.0);
        const zeus::map::NodeIndex n1 = fixture.addNode(200.0, 0.0);
        const zeus::map::EdgeIndex forward = fixture.addEdge(n0, n1, 20.0, 42);
        const zeus::map::EdgeIndex reverse = fixture.addEdge(n1, n0, 20.0, 42, true);

        PlanSetup setup(fixture.data);
        const zeus::routing::RouteResult forward_result =
            setup.planner->plan(makeRequest({50.0, 1.0}, {150.0, 1.0}, algorithm));
        require(forward_result.ok, "bidirectional twin direct route succeeds");
        require(forward_result.origin.edge == forward, "twin tie-break stays deterministic");
        require(forward_result.path.edges.size() == 1 &&
                    forward_result.path.edges[0] == forward,
                "bidirectional twin direct route keeps one edge");
        require(near(forward_result.stats.time_s, 5.0), "bidirectional twin direct time");

        const zeus::routing::RouteResult cross_result =
            setup.planner->plan(makeRequest({150.0, 1.0}, {50.0, 1.0}, algorithm));
        require(cross_result.ok, "bidirectional twin cross route succeeds");
        require(cross_result.path.edges.size() == 1 &&
                    cross_result.path.edges[0] == reverse,
                "bidirectional twin cross traverses the reverse twin");
        require(near(cross_result.stats.time_s, 5.0), "bidirectional twin cross time");
        require(near(cross_result.stats.length_m, 100.0), "bidirectional twin cross length");
    }
}

void runBidirectionalDeterminismTest() {
    Fixture fixture = makeDetourFixture(5.0);
    PlanSetup setup(fixture.data);
    const zeus::routing::RouteRequest request =
        makeRequest({-50.0, 1.0}, {150.0, 1.0}, zeus::routing::Algorithm::kBidirectionalAStar);
    const zeus::routing::RouteResult first = setup.planner->plan(request);
    const zeus::routing::RouteResult second = setup.planner->plan(request);
    require(first.ok && second.ok, "determinism fixture routes succeed");
    require(first.path.edges == second.path.edges &&
                near(first.stats.time_s, second.stats.time_s, 0.0) &&
                first.stats.expanded_nodes == second.stats.expanded_nodes,
            "repeated bidirectional requests are identical");
}

Fixture makeTurnFixture(bool prohibited, float penalty_s = 0.0F) {
    Fixture fixture;
    const auto before = fixture.addNode(-100.0, 0.0);
    const auto junction = fixture.addNode(0.0, 0.0);
    const auto destination_junction = fixture.addNode(100.0, 0.0);
    const auto detour = fixture.addNode(0.0, 100.0);
    const auto after = fixture.addNode(200.0, 0.0);
    const auto incoming = fixture.addEdge(before, junction, 20.0);          // e0
    const auto straight = fixture.addEdge(junction, destination_junction, 20.0); // e1
    fixture.addEdge(junction, detour, 20.0);                               // e2
    fixture.addEdge(detour, destination_junction, 20.0);                   // e3
    fixture.addEdge(destination_junction, after, 20.0);                    // e4
    fixture.addTurn(incoming, straight, prohibited, penalty_s);
    return fixture;
}

void runTurnRestrictionTest() {
    for (const zeus::routing::Algorithm algorithm : {
             zeus::routing::Algorithm::kDijkstra,
             zeus::routing::Algorithm::kAStar,
             zeus::routing::Algorithm::kBidirectionalDijkstra,
             zeus::routing::Algorithm::kBidirectionalAStar}) {
        Fixture fixture = makeTurnFixture(true);
        PlanSetup setup(fixture.data);
        const auto result = setup.planner->plan(
            makeRequest({-90.0, 1.0}, {190.0, 1.0}, algorithm));
        require(result.ok, "turn-restricted route succeeds for every algorithm selection");
        require(result.path.edges == std::vector<zeus::map::EdgeIndex>({0, 2, 3, 4}),
                "prohibited straight transition forces the legal detour");
    }
}

void runTurnPenaltyTest() {
    Fixture fixture = makeTurnFixture(false, 20.0F);
    PlanSetup setup(fixture.data);
    const auto result = setup.planner->plan(
        makeRequest({-90.0, 1.0}, {190.0, 1.0}));
    require(result.ok, "turn-penalty route succeeds");
    require(result.path.edges == std::vector<zeus::map::EdgeIndex>({0, 2, 3, 4}),
            "a sufficiently expensive straight turn changes the chosen route");
    require(near(result.stats.time_s, (90.0 + 100.0 + std::sqrt(20000.0) + 90.0) / 20.0),
            "turn-aware result reports the legal detour travel time");
}

}  // namespace

int main() {
    try {
        runShortStraightRouteTest();
        runRoutingOverlayTest();
        runTimeVersusDistanceTest();
        runUnreachableTest();
        runLoopSameEdgeBehindTest();
        runAlgorithmConsistencyTest();
        runUnmatchedEndpointTest();
        runTwinEdgeTest();
        runRouteExportTest();
        runBidirectionalConsistencyTest();
        runBidirectionalLoopTest();
        runBidirectionalUnreachableTest();
        runBidirectionalMeetingAtSharedNodeTest();
        runBidirectionalTwinTest();
        runBidirectionalDeterminismTest();
        runTurnRestrictionTest();
        runTurnPenaltyTest();
        std::cout << "all routing tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
