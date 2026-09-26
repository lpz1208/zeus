#include <cmath>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <random>
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
#include "zeus/routing/algorithm_lab.h"
#include "zeus/routing/incremental_search.h"
#include "zeus/routing/time_dependent.h"
#include "zeus/routing/landmarks.h"
#include "zeus/routing/contraction_hierarchy.h"

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

        const std::vector<zeus::routing::Algorithm> algorithms = {
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
        require(result.effective_algorithm == algorithm,
                "turn-restricted bidirectional requests execute without downgrade");
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

void runTurnBidirectionalDifferentialTest() {
    using namespace zeus::routing;
    std::mt19937 random(20260925);
    for (int scenario = 0; scenario < 12; ++scenario) {
        Fixture fixture;
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 5; ++x) fixture.addNode(x * 100, y * 100);
        for (std::uint32_t node = 0; node < 25; ++node) {
            for (const int delta : {1, 5}) {
                if ((delta == 1 && node % 5 == 4) || node + delta >= 25) continue;
                fixture.addEdge(node, node + delta, 5 + random() % 20);
                fixture.addEdge(node + delta, node, 5 + random() % 20);
            }
        }
        const auto count = fixture.data.edges.size();
        for (std::size_t from = 0; from < count; ++from) {
            for (std::size_t to = 0; to < count; ++to) {
                if (fixture.data.edges[from].to != fixture.data.edges[to].from) continue;
                const auto value = random() % 5;
                if (value <= 1) fixture.addTurn(from, to, value == 0, value == 1 ? random() % 25 : 0);
            }
        }
        PlanSetup setup(fixture.data);
        for (int sample = 0; sample < 50; ++sample) {
            std::vector<std::uint8_t> enabled(count, 1);
            std::vector<double> factors(count, 1);
            for (std::size_t edge = 0; edge < count; ++edge) {
                enabled[edge] = random() % 10 != 0;
                factors[edge] = 1.0 + (random() % 20) / 10.0;
            }
            const auto start = static_cast<zeus::map::EdgeIndex>(random() % count);
            const auto goal = static_cast<zeus::map::EdgeIndex>(random() % count);
            enabled[goal] = 1;
            RoutingOverlay overlay{enabled, factors};
            auto request = makeRequest({0, 0}, {400, 400});
            request.overlay = &overlay;
            if (sample % 3 != 0) {
                request.origin_position = RoutePosition{start, static_cast<double>(random() % 101)};
                request.destination_position = RoutePosition{goal, static_cast<double>(random() % 101)};
            } else {
                // Exercise both matched directed twins and multi-source/goal labels.
                const auto& a = fixture.data.nodes[fixture.data.edges[start].from].point;
                const auto& b = fixture.data.nodes[fixture.data.edges[start].to].point;
                const auto& c = fixture.data.nodes[fixture.data.edges[goal].from].point;
                const auto& d = fixture.data.nodes[fixture.data.edges[goal].to].point;
                request.origin = {(a.x + b.x) / 2, (a.y + b.y) / 2};
                request.destination = {(c.x + d.x) / 2, (c.y + d.y) / 2};
            }
            const auto reference = setup.planner->plan(request);
            for (const auto algorithm : {Algorithm::kBidirectionalDijkstra, Algorithm::kBidirectionalAStar, Algorithm::kLpaStar, Algorithm::kDStarLite, Algorithm::kTimeDependent, Algorithm::kAlt, Algorithm::kCH}) {
                request.algorithm = algorithm;
                request.record_trace = true;
                const auto actual = setup.planner->plan(request);
                const auto label = std::to_string(scenario) + "/" + std::to_string(sample) + "/" + algorithmName(algorithm);
                require(actual.ok == reference.ok, "bidirectional turn reachability " + label);
                if (!actual.ok) continue;
                require((actual.effective_algorithm == (algorithm == Algorithm::kCH && !ContractionHierarchy::baseWeights(request.overlay) ? Algorithm::kBidirectionalDijkstra : algorithm)), "bidirectional execution metadata " + label);
                require(near(actual.stats.time_s, reference.stats.time_s, 1e-6),
                        "bidirectional optimal cost " + label + " expected=" + std::to_string(reference.stats.time_s) +
                        " actual=" + std::to_string(actual.stats.time_s));
                require(actual.stats.expanded_nodes == 0 || !actual.search_trace.empty(),
                        "turn-aware bidirectional search preserves settle trace");
                double time = 0;
                for (std::size_t i = 0; i < actual.path.edges.size(); ++i) {
                    const auto edge = actual.path.edges[i];
                    require(i == 0 || enabled[edge], "route never re-enters a closed edge");
                    if (i > 0) time += setup.runtime->turnPenaltySeconds(actual.path.edges[i - 1], edge);
                    const double begin = i == 0 ? actual.path.start_offset_m : 0;
                    const double end = i + 1 == actual.path.edges.size() ? actual.path.end_offset_m : fixture.data.edges[edge].length_m;
                    time += (end - begin) / fixture.data.edges[edge].speed_limit_mps * factors[edge];
                }
                require(near(time, actual.stats.time_s, 1e-6), "returned path has legal turns and exact reported cost " + label);
            }
        }
    }
}

void runCHStaticDifferentialTest() {
    using namespace zeus::routing;
    std::mt19937 random(20260925);
    for (int scenario = 0; scenario < 12; ++scenario) {
        Fixture fixture;
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 5; ++x) fixture.addNode(x * 100, y * 100);
        for (std::uint32_t node = 0; node < 25; ++node) {
            for (const int delta : {1, 5}) {
                if ((delta == 1 && node % 5 == 4) || node + delta >= 25) continue;
                fixture.addEdge(node, node + delta, 5 + random() % 20);
                fixture.addEdge(node + delta, node, 5 + random() % 20);
            }
        }
        const auto count = fixture.data.edges.size();
        for (std::size_t from = 0; from < count; ++from) {
            for (std::size_t to = 0; to < count; ++to) {
                if (fixture.data.edges[from].to != fixture.data.edges[to].from) continue;
                const auto value = random() % 5;
                if (scenario % 2 == 0 && value <= 1) fixture.addTurn(from, to, value == 0, value == 1 ? random() % 25 : 0);
            }
        }
        PlanSetup setup(fixture.data);
        for (int sample = 0; sample < 50; ++sample) {
            std::vector<std::uint8_t> enabled(count, 1);
            std::vector<double> factors(count, 1);
            for (std::size_t edge = 0; edge < count; ++edge) {
                enabled[edge] = 1;
                factors[edge] = 1;
            }
            const auto start = static_cast<zeus::map::EdgeIndex>(random() % count);
            const auto goal = static_cast<zeus::map::EdgeIndex>(random() % count);
            enabled[goal] = 1;
            RoutingOverlay overlay{enabled, factors};
            auto request = makeRequest({0, 0}, {400, 400});
            request.overlay = &overlay;
            if (sample % 3 != 0) {
                request.origin_position = RoutePosition{start, static_cast<double>(random() % 101)};
                request.destination_position = RoutePosition{goal, static_cast<double>(random() % 101)};
            } else {
                // Exercise both matched directed twins and multi-source/goal labels.
                const auto& a = fixture.data.nodes[fixture.data.edges[start].from].point;
                const auto& b = fixture.data.nodes[fixture.data.edges[start].to].point;
                const auto& c = fixture.data.nodes[fixture.data.edges[goal].from].point;
                const auto& d = fixture.data.nodes[fixture.data.edges[goal].to].point;
                request.origin = {(a.x + b.x) / 2, (a.y + b.y) / 2};
                request.destination = {(c.x + d.x) / 2, (c.y + d.y) / 2};
            }
            const auto reference = setup.planner->plan(request);
            for (const auto algorithm : {Algorithm::kCH}) {
                request.algorithm = algorithm;
                request.record_trace = true;
                const auto actual = setup.planner->plan(request);
                const auto label = std::to_string(scenario) + "/" + std::to_string(sample) + "/" + algorithmName(algorithm);
                require(actual.ok == reference.ok, "bidirectional turn reachability " + label);
                if (!actual.ok) continue;
                require((actual.effective_algorithm == (algorithm == Algorithm::kCH && !ContractionHierarchy::baseWeights(request.overlay) ? Algorithm::kBidirectionalDijkstra : algorithm)), "bidirectional execution metadata " + label);
                require(near(actual.stats.time_s, reference.stats.time_s, 1e-6),
                        "bidirectional optimal cost " + label + " expected=" + std::to_string(reference.stats.time_s) +
                        " actual=" + std::to_string(actual.stats.time_s));
                require(actual.stats.expanded_nodes == 0 || !actual.search_trace.empty(),
                        "turn-aware bidirectional search preserves settle trace");
                double time = 0;
                for (std::size_t i = 0; i < actual.path.edges.size(); ++i) {
                    const auto edge = actual.path.edges[i];
                    require(i == 0 || enabled[edge], "route never re-enters a closed edge");
                    if (i > 0) time += setup.runtime->turnPenaltySeconds(actual.path.edges[i - 1], edge);
                    const double begin = i == 0 ? actual.path.start_offset_m : 0;
                    const double end = i + 1 == actual.path.edges.size() ? actual.path.end_offset_m : fixture.data.edges[edge].length_m;
                    time += (end - begin) / fixture.data.edges[edge].speed_limit_mps * factors[edge];
                }
                require(near(time, actual.stats.time_s, 1e-6), "returned path has legal turns and exact reported cost " + label);
            }
        }
    }
}

void runSelfLoopPartialRouteTest() {
    using namespace zeus::routing;
    Fixture fixture;
    const auto node = fixture.addNode(0, 0);
    const auto edge = fixture.addEdge(node, node, 10);
    fixture.data.edges[edge].length_m = 100;
    fixture.data.geometry_points[1] = {50, 0};
    fixture.data.geometry_points.push_back({0, 0});
    fixture.data.edges[edge].geometry_count = 3;
    PlanSetup setup(fixture.data);
    for (const auto algorithm : {Algorithm::kDijkstra, Algorithm::kLpaStar, Algorithm::kDStarLite, Algorithm::kTimeDependent, Algorithm::kAlt, Algorithm::kCH}) {
        auto request = makeRequest({0, 0}, {0, 0}, algorithm);
        request.origin_position = RoutePosition{edge, 75};
        request.destination_position = RoutePosition{edge, 25};
        const auto result = setup.planner->plan(request);
        require(result.ok && result.path.edges == std::vector<zeus::map::EdgeIndex>{edge, edge} &&
                    near(result.stats.time_s, 5), "self-loop behind origin retains both partial traversals");
        require(AlgorithmLab(*setup.runtime, request).validatePath(result.path).ok,
                "self-loop path passes exact validation");
    }
}

void runIncrementalTest(zeus::routing::Algorithm algorithm) {
    using namespace zeus::routing;
    std::mt19937 random(20260926);
    Fixture fixture;
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 6; ++x) fixture.addNode(100 * x, 100 * y);
    for (std::uint32_t node = 0; node < 36; ++node) {
        for (int delta : {1, 6}) {
            if (node + delta >= 36 || (delta == 1 && node % 6 == 5)) continue;
            fixture.addEdge(node, node + delta, 10 + random() % 10);
            fixture.addEdge(node + delta, node, 10 + random() % 10);
        }
    }
    for (std::size_t a = 0; a < fixture.data.edges.size(); ++a)
        for (std::size_t b = 0; b < fixture.data.edges.size(); ++b)
            if (fixture.data.edges[a].to == fixture.data.edges[b].from && random() % 4 == 0)
                fixture.addTurn(a, b, random() % 3 == 0, random() % 15);
    PlanSetup setup(fixture.data);
    IncrementalSearch repair(*setup.runtime);
    const auto count = fixture.data.edges.size();
    std::vector<std::uint8_t> enabled(count, 1);
    std::vector<double> costs(count, 1);
    RoutingOverlay overlay{enabled, costs};
    auto request = makeRequest({0, 0}, {500, 500});
    request.overlay = &overlay;
    std::uint32_t goal = count - 1;
    for (int step = 0; step < 240; ++step) {
        for (int edit = 0; edit < 4; ++edit) {
            const auto edge = random() % count;
            enabled[edge] = random() % 5 != 0;
            costs[edge] = 1 + random() % 5;
        }
        const bool reset = step % 60 == 0;
        if (reset) goal = (goal + 17) % count;
        enabled[goal] = 1;
        if (step % 30 == 15) std::fill(enabled.begin(), enabled.end(), 0);
        enabled[goal] = 1;
        request.origin_position = RoutePosition{static_cast<zeus::map::EdgeIndex>(random() % count), double(random() % 100)};
        request.destination_position = RoutePosition{goal, 75};
        request.algorithm = Algorithm::kDijkstra;
        const auto expected = setup.planner->plan(request);
        request.algorithm = algorithm;
        request.record_trace = true;
        const auto actual = setup.planner->plan(request, &repair);
        const auto label = std::string(algorithmName(algorithm)) + " repair step " + std::to_string(step);
        require(actual.stats.incremental_reused == !reset, label + " context reuse/reset");
        require(actual.ok == expected.ok, label + " reachability");
        if (actual.ok) {
            require(near(actual.stats.time_s, expected.stats.time_s, 1e-6), label + " optimal cost");
            const auto validated = AlgorithmLab(*setup.runtime, request).validatePath(actual.path);
            require(validated.ok && near(validated.stats.time_s, actual.stats.time_s, 1e-6), label + " legal exact path");
        }
        const auto repeat = setup.planner->plan(request, &repair);
        require(repeat.ok == actual.ok && repeat.path.edges == actual.path.edges &&
                    repeat.stats.incremental_reused && repeat.stats.updated_edges == 0 &&
                    repeat.stats.expanded_nodes == 0,
                label + " unchanged request performs no search expansions");
        for (const auto& point : actual.search_trace)
            require(std::isfinite(point.g) && std::isfinite(point.f), "repair trace is finite during invalidation");
    }
    // Separate contexts stay independent; a one-shot request always starts cold.
    request.algorithm = algorithm;
    const auto cold = setup.planner->plan(request);
    require(!cold.stats.incremental_reused, "stateless incremental calls report cold search");
    bool rejected = false;
    PlanSetup other(fixture.data);
    try { (void)other.planner->plan(request, &repair); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "incremental context cannot be reused on a different map");
}

void runTimeDependentTest() {
    using namespace zeus::routing;
    Fixture fixture;
    const auto a = fixture.addNode(0, 0), b = fixture.addNode(100, 0);
    const auto c = fixture.addNode(300, 0), d = fixture.addNode(400, 0);
    const auto e = fixture.addNode(200, 100);
    const auto start = fixture.addEdge(a, b, 10);
    const auto direct = fixture.addEdge(b, c, 10);
    const auto goal = fixture.addEdge(c, d, 10);
    const auto detour = fixture.addEdge(b, e, 10);
    fixture.addEdge(e, c, 10);
    PlanSetup setup(fixture.data);
    const std::vector<SpeedChange> changes{{direct, 5, .1}, {direct, 60, 1}};
    const SpeedSchedule schedule(fixture.data.edges.size(), changes);
    auto request = makeRequest({10, 0}, {390, 0}, Algorithm::kTimeDependent);
    request.origin_position = RoutePosition{start, 10};
    request.destination_position = RoutePosition{goal, 90};
    request.speed_schedule = &schedule;
    request.record_trace = true;
    auto early = setup.planner->plan(request);
    require(early.ok && early.path.edges[1] == detour, "scheduled slowdown changes route before it occurs");
    request.departure_time_s = 100;
    auto late = setup.planner->plan(request);
    require(late.ok && late.path.edges[1] == direct && near(late.stats.time_s, 38), "later departure uses restored road");
    require(near(schedule.duration(*setup.runtime, direct, 200, 0), 69.5), "traversal integrates across two speed boundaries");
    double last_arrival = -1;
    for (int tick = 0; tick < 400; ++tick) {
        const double time = tick * .25;
        const double arrival = time + schedule.duration(*setup.runtime, direct, 75, time);
        require(arrival + 1e-9 >= last_arrival, "piecewise speeds preserve FIFO across recovery");
        last_arrival = arrival;
    }
    require(near(schedule.duration(*setup.runtime, direct, 20, 10, 10, 10), 20),
            "live speed penalty is replaced without double counting");
    request.origin_position = RoutePosition{direct, 20};
    request.destination_position = RoutePosition{direct, 70};
    request.departure_time_s = 4;
    require(near(setup.planner->plan(request).stats.time_s, 41), "same-edge partial traversal crosses speed boundary");
    const SpeedSchedule duplicate(fixture.data.edges.size(), std::vector<SpeedChange>{{direct, 0, .2}, {direct, 0, 2}});
    require(near(duplicate.duration(*setup.runtime, direct, 100, 0), 5), "last equal-time speed event wins");
    for (const auto change : std::vector<SpeedChange>{{direct, -1, 1}, {direct, 0, 0},
            {direct, 0, 4}, {9999, 0, 1}, {direct, std::numeric_limits<double>::quiet_NaN(), 1}}) {
        bool rejected = false;
        try { SpeedSchedule invalid(fixture.data.edges.size(), std::vector<SpeedChange>{change}); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid time-dependent profile rejected");
    }
    request.algorithm = Algorithm::kDijkstra;
    bool rejected = false;
    try { (void)setup.planner->plan(request); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "static algorithms cannot silently ignore speed profiles");
}

void runTimeDependentOracleTest() {
    using namespace zeus::routing;
    std::mt19937 random(20260927);
    for (int trial = 0; trial < 120; ++trial) {
        Fixture fixture;
        for (int i = 0; i < 7; ++i) fixture.addNode(i * 100, (i % 2) * 70);
        const auto start = fixture.addEdge(0, 1, 10);
        for (int from = 1; from < 5; ++from)
            for (int to = from + 1; to <= 5; ++to)
                if (to == from + 1 || random() % 2) fixture.addEdge(from, to, 5 + random() % 15);
        const auto goal = fixture.addEdge(5, 6, 10);
        for (std::size_t a = 0; a < fixture.data.edges.size(); ++a)
            for (std::size_t b = 0; b < fixture.data.edges.size(); ++b)
                if (fixture.data.edges[a].to == fixture.data.edges[b].from && random() % 3 == 0)
                    fixture.addTurn(a, b, random() % 4 == 0, random() % 8);
        PlanSetup setup(fixture.data);
        const auto count = fixture.data.edges.size();
        std::vector<SpeedChange> changes;
        std::vector<std::uint8_t> enabled(count, 1);
        std::vector<double> density(count, 1);
        for (std::size_t edge = 0; edge < count; ++edge) {
            enabled[edge] = edge == start || edge == goal || random() % 5 != 0;
            density[edge] = 1 + random() % 3;
            for (const double time : {5., 20., 40.})
                changes.push_back({static_cast<zeus::map::EdgeIndex>(edge), time, (1 + random() % 30) / 10.0});
        }
        RoutingOverlay overlay{enabled, density};
        const SpeedSchedule schedule(count, changes);
        auto request = makeRequest({0, 0}, {600, 0}, Algorithm::kTimeDependent);
        request.origin_position = RoutePosition{start, double(random() % 100)};
        request.destination_position = RoutePosition{goal, double(random() % 100)};
        request.departure_time_s = random() % 60;
        request.overlay = &overlay;
        request.speed_schedule = &schedule;
        // Independent oracle: integrate distance up to a proposed arrival, then
        // binary-search the arrival; enumerate every path in this acyclic graph.
        const auto arrival = [&](zeus::map::EdgeIndex edge, double length, double departure) {
            const double speed = fixture.data.edges[edge].speed_limit_mps / density[edge];
            const auto covered = [&](double end) {
                double distance = 0, begin = 0, factor = 1;
                for (const auto& change : changes) if (change.edge == edge) {
                    distance += std::max(0.0, std::min(end, change.time_s) - std::max(begin, departure)) * speed * factor;
                    begin = change.time_s; factor = change.factor;
                }
                return distance + std::max(0.0, end - std::max(begin, departure)) * speed * factor;
            };
            double low = departure, high = departure + length / (speed * .05) + 1;
            for (int i = 0; i < 60; ++i) {
                const double mid = (low + high) / 2;
                if (covered(mid) < length) low = mid; else high = mid;
            }
            return high;
        };
        double best = std::numeric_limits<double>::infinity();
        std::vector<zeus::map::EdgeIndex> path{start};
        const auto visit = [&](auto&& self, zeus::map::EdgeIndex current, double time) -> void {
            for (const auto next : setup.runtime->outgoingEdges(setup.runtime->edge(current).to)) {
                const double turn = setup.runtime->turnPenaltySeconds(current, next);
                if (!enabled[next] || !std::isfinite(turn)) continue;
                const double end = arrival(next, next == goal ? request.destination_position->offset_s : fixture.data.edges[next].length_m, time + turn);
                if (next == goal) best = std::min(best, end);
                else self(self, next, end);
            }
        };
        visit(visit, start, arrival(start, fixture.data.edges[start].length_m - request.origin_position->offset_s, request.departure_time_s));
        const auto actual = setup.planner->plan(request);
        require(actual.ok == std::isfinite(best), "TD oracle reachability trial " + std::to_string(trial));
        if (!actual.ok) continue;
        require(near(actual.stats.time_s, best - request.departure_time_s, 1e-6), "TD exhaustive oracle optimality");
        double time = request.departure_time_s;
        for (std::size_t i = 0; i < actual.path.edges.size(); ++i) {
            const auto edge = actual.path.edges[i];
            require(enabled[edge], "TD route never enters a closed edge");
            if (i) time += setup.runtime->turnPenaltySeconds(actual.path.edges[i-1], edge);
            const double length = (i + 1 == actual.path.edges.size() ? actual.path.end_offset_m : fixture.data.edges[edge].length_m)
                - (i == 0 ? actual.path.start_offset_m : 0);
            time = arrival(edge, length, time);
        }
        require(near(time, best, 1e-6), "TD emitted path agrees with independent traversal cost");
    }
}

void runCHIndexTest() {
    using namespace zeus::routing;
    const double infinity = std::numeric_limits<double>::infinity();
    Fixture fixture = makeTurnFixture(true);
    fixture.addNode(900, 900);
    fixture.addNode(1000, 900);
    fixture.addEdge(5, 6, 10); // Separate directed component.
    PlanSetup setup(fixture.data);
    const auto incoming = buildIncomingAdjacency(fixture.data);
    // Full contraction, no contraction, bounded witnesses, and a partial core.
    for (const auto limits : {ContractionHierarchy::Limits{},
            ContractionHierarchy::Limits{10000, 0, 128},
            ContractionHierarchy::Limits{10000, 100, 0},
            ContractionHierarchy::Limits{10000, 100000, 0}}) {
        ContractionHierarchy hierarchy(*setup.runtime, limits);
        require(hierarchy.available(), "CH publishes only complete searchable graphs");
        for (zeus::map::EdgeIndex a = 0; a < fixture.data.edges.size(); ++a) {
            for (zeus::map::EdgeIndex b = 0; b < fixture.data.edges.size(); ++b) {
                SearchQuery query;
                query.starts = {{setup.runtime->edge(a).to, a, 0, 2, 0}};
                query.goals = {{setup.runtime->edge(b).from, b, 0, 3, 0}};
                const auto expected = runTurnAwareSearch(*setup.runtime, query, setup.planner->maxSpeedMps(), infinity);
                const auto actual = hierarchy.search(query, incoming, infinity);
                require(actual.found == expected.found && (!actual.found || near(actual.total_time_s, expected.total_time_s)),
                        "CH complete/core/witness-limited search matches turn-aware Dijkstra");
                if (!actual.found) continue;
                double cost = 5;
                auto previous = a;
                for (auto edge : actual.node_edges) {
                    require(setup.runtime->edge(previous).to == setup.runtime->edge(edge).from, "unpacked CH path connects");
                    cost += setup.runtime->turnPenaltySeconds(previous, edge) + edgeCostSeconds(setup.runtime->edge(edge));
                    previous = edge;
                }
                require(setup.runtime->edge(previous).to == setup.runtime->edge(b).from, "CH reaches terminal entry");
                cost += setup.runtime->turnPenaltySeconds(previous, b);
                require(near(cost, actual.total_time_s), "CH unpacked path retains exact costs and terminal turn");
            }
        }
    }
    ContractionHierarchy rejected(*setup.runtime, {0, 100, 10});
    require(!rejected.available() && rejected.bytes() == 0, "oversized base graph is rejected before allocating index");

    Fixture parallel;
    for (int i = 0; i < 4; ++i) parallel.addNode(i * 100, 0);
    const auto source = parallel.addEdge(0, 1, 10);
    parallel.addEdge(1, 2, 5);
    const auto faster = parallel.addEdge(1, 2, 20);
    const auto target = parallel.addEdge(2, 3, 10);
    PlanSetup node_graph(parallel.data);
    auto request = makeRequest({50, 0}, {250, 0}, Algorithm::kCH);
    request.origin_position = RoutePosition{source, 50};
    request.destination_position = RoutePosition{target, 50};
    const auto result = node_graph.planner->plan(request);
    require(result.ok && near(result.stats.time_s, 15) &&
                result.path.edges == std::vector<zeus::map::EdgeIndex>{source, faster, target},
            "node-state CH parallel-arc replacement retains the correct original road ID");
}

void runCHReuseTest() {
    using namespace zeus::routing;
    Fixture fixture;
    for (int i = 0; i <= 300; ++i) fixture.addNode(i * 100, 0);
    std::vector<zeus::map::EdgeIndex> forward;
    for (int i = 0; i < 300; ++i) {
        forward.push_back(fixture.addEdge(i, i + 1, 10));
        fixture.addEdge(i + 1, i, 10);
    }
    PlanSetup setup(fixture.data);
    auto request = makeRequest({25, 0}, {29975, 0}, Algorithm::kCH);
    request.origin_position = RoutePosition{forward.front(), 25};
    request.destination_position = RoutePosition{forward.back(), 75};
    const auto first = setup.planner->plan(request);
    const auto warm = setup.planner->plan(request);
    request.algorithm = Algorithm::kDijkstra;
    const auto baseline = setup.planner->plan(request);
    require(first.ok && first.effective_algorithm == Algorithm::kCH && near(first.stats.time_s, baseline.stats.time_s),
            "CH preserves line route optimum");
    require(first.stats.ch_shortcuts > 0 && first.stats.ch_core_states == 0 && first.stats.ch_bytes > 0 &&
                !first.stats.ch_reused && warm.stats.ch_reused && warm.stats.ch_preprocess_ms == 0 &&
                first.path.edges == warm.path.edges && first.stats.expanded_nodes < baseline.stats.expanded_nodes,
            "CH nested shortcuts accelerate query and immutable index is reused");
    std::vector<std::uint8_t> enabled(fixture.data.edges.size(), 1);
    std::vector<double> factors(enabled.size(), 1);
    RoutingOverlay overlay{enabled, factors};
    request.overlay = &overlay;
    factors[forward[150]] = 2;
    request.algorithm = Algorithm::kDijkstra;
    const auto slowed = setup.planner->plan(request);
    request.algorithm = Algorithm::kCH;
    const auto fallback = setup.planner->plan(request);
    require(fallback.ok && near(fallback.stats.time_s, slowed.stats.time_s) &&
                fallback.effective_algorithm == Algorithm::kBidirectionalDijkstra &&
                fallback.stats.fallback_reason == "ch_dynamic_weights", "CH explicitly falls back for changed weights");
    factors[forward[150]] = 1;
    const auto recovered = setup.planner->plan(request);
    require(recovered.ok && recovered.stats.ch_reused && recovered.effective_algorithm == Algorithm::kCH &&
                recovered.path.edges == first.path.edges, "restoring base weights reuses CH without rebuilding");
    enabled[forward[150]] = 0;
    require(!setup.planner->plan(request).ok, "CH fallback cannot bypass closed interior edge via stale shortcut");
    enabled[forward[150]] = 1;
    PlanSetup concurrent(fixture.data);
    std::vector<std::future<RouteResult>> futures;
    for (int i = 0; i < 8; ++i) futures.push_back(std::async(std::launch::async, [&] { return concurrent.planner->plan(request); }));
    int builds = 0;
    for (auto& future : futures) {
        const auto result = future.get();
        require(result.ok && result.path.edges == first.path.edges, "CH concurrent queries have independent labels");
        builds += !result.stats.ch_reused;
    }
    require(builds == 1, "CH is published once under concurrent first queries");
    ContractionHierarchy partial(*setup.runtime, {100000, 2000, 10});
    require(partial.available() && partial.coreStates() > 0 && partial.coreStates() < fixture.data.edges.size(),
            "work budget leaves a partially contracted searchable core");
    const auto incoming = buildIncomingAdjacency(fixture.data);
    SearchQuery query;
    query.starts = {{setup.runtime->edge(forward.front()).to, forward.front(), 25, 7.5, 75}};
    query.goals = {{setup.runtime->edge(forward.back()).from, forward.back(), 75, 7.5, 75}};
    const auto core = partial.search(query, incoming, std::numeric_limits<double>::infinity());
    require(core.found && near(core.total_time_s, baseline.stats.time_s) &&
                core.node_edges.size() + 2 == baseline.path.edges.size(),
            "partially contracted core retains exact end-to-end route and unpacks shortcuts");
    ContractionHierarchy states_limited(*setup.runtime, {100000, 2000, 10, 1});
    require(!states_limited.available() && states_limited.bytes() == 0, "state limit bounds label/index allocation");
    Algorithm parsed;
    require(parseAlgorithm("ch", parsed) && parsed == Algorithm::kCH && isBidirectional(parsed) &&
                !algorithmCapability(parsed)->supports_dynamic_weights,
            "CH registry truthfully declares static weights");
}

void runAltLandmarkTest() {
    using namespace zeus::routing;
    Fixture fixture;
    for (int i = 0; i < 7; ++i) fixture.addNode(i * 100, 0);
    const auto first = fixture.addEdge(0, 1, 10);
    fixture.addEdge(1, 2, 10);
    fixture.addEdge(0, 2, 4);
    const auto goal = fixture.addEdge(2, 3, 10);
    fixture.addEdge(4, 5, 10); // disconnected one-way component; node 6 is isolated
    PlanSetup setup(fixture.data);
    const auto incoming = buildIncomingAdjacency(fixture.data);
    LandmarkIndex landmarks(*setup.runtime, incoming);
    constexpr double inf = std::numeric_limits<double>::infinity();
    std::vector<std::vector<double>> oracle(7, std::vector<double>(7, inf));
    for (int i = 0; i < 7; ++i) oracle[i][i] = 0;
    for (const auto& edge : fixture.data.edges) oracle[edge.from][edge.to] = edgeCostSeconds(edge);
    for (int k = 0; k < 7; ++k)
        for (int i = 0; i < 7; ++i)
            for (int j = 0; j < 7; ++j) oracle[i][j] = std::min(oracle[i][j], oracle[i][k] + oracle[k][j]);
    for (int from = 0; from < 7; ++from) for (int to = 0; to < 7; ++to) {
        const auto bound = landmarks.lowerBound(from, to);
        require(!std::isnan(bound) && bound >= 0 && bound <= oracle[from][to] + 1e-7,
                "directed ALT lower bound is admissible across disconnected components");
        if (std::isfinite(oracle[from][to])) require(near(bound, oracle[from][to], 1e-6),
                "landmarks on all active nodes recover exact relaxed distances");
    }
    const auto budget = 7 * 2 * sizeof(double) * 2;
    LandmarkIndex bounded(*setup.runtime, incoming, 8, budget);
    require(bounded.count() == 2 && bounded.tableBytes() <= budget, "landmark distance tables respect the byte budget");
    LandmarkIndex empty(*setup.runtime, incoming, 8, 0);
    require(empty.count() == 0 && empty.lowerBound(0, 3) == 0, "zero table budget falls back to zero heuristic");
    SearchQuery query;
    query.algorithm = Algorithm::kAlt;
    query.starts = {{1, first, 0, 10, 100}};
    query.goals = {{2, goal, 50, 5, 50}};
    const auto fallback = runAltSearch(*setup.runtime, query, empty, inf);
    require(fallback.found && near(fallback.total_time_s, 25), "ALT without landmarks still finds the optimal path");
    PlanSetup other(fixture.data);
    bool rejected = false;
    try { (void)runAltSearch(*other.runtime, query, landmarks, inf); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "ALT index cannot cross map runtime identities");
}

void runAltReuseTest() {
    using namespace zeus::routing;
    Fixture fixture;
    constexpr std::uint32_t side = 20;
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) fixture.addNode(x * 100, y * 100);
    std::vector<zeus::map::EdgeIndex> bottom;
    for (std::uint32_t node = 0; node < side * side; ++node) for (const auto delta : {1u, side}) {
        if (node + delta >= side * side || (delta == 1 && node % side == side - 1)) continue;
        const auto edge = fixture.addEdge(node, node + delta, 10);
        fixture.addEdge(node + delta, node, 10);
        if (node < side && delta == 1) bottom.push_back(edge);
    }
    PlanSetup setup(fixture.data);
    auto request = makeRequest({50, 0}, {1850, 0});
    request.origin_position = RoutePosition{bottom.front(), 50};
    request.destination_position = RoutePosition{bottom.back(), 50};
    const auto baseline = setup.planner->plan(request);
    request.algorithm = Algorithm::kAlt;
    const auto first = setup.planner->plan(request);
    const auto second = setup.planner->plan(request);
    require(first.ok && near(first.stats.time_s, baseline.stats.time_s) &&
                first.stats.expanded_nodes < baseline.stats.expanded_nodes,
            "ALT reduces grid expansions and preserves optimal cost");
    require(!first.stats.landmark_reused && first.stats.landmark_count == 8 &&
                second.stats.landmark_reused && second.stats.landmark_preprocess_ms == 0 &&
                first.path.edges == second.path.edges && first.stats.landmark_bytes == second.stats.landmark_bytes,
            "cold preprocessing is reused by later queries");
    std::vector<std::uint8_t> enabled(fixture.data.edges.size(), 1);
    std::vector<double> factors(enabled.size(), 1);
    RoutingOverlay overlay{enabled, factors};
    request.overlay = &overlay;
    for (int step = 0; step < 32; ++step) {
        enabled[bottom[8]] = step % 2;
        factors[bottom[10]] = 1 + step % 4;
        request.origin_position = RoutePosition{bottom[step % 7], 25};
        request.destination_position = RoutePosition{bottom[12 + step % 7], 75};
        request.algorithm = Algorithm::kDijkstra;
        const auto expected = setup.planner->plan(request);
        request.algorithm = Algorithm::kAlt;
        const auto actual = setup.planner->plan(request);
        require(actual.ok == expected.ok && near(actual.stats.time_s, expected.stats.time_s) && actual.stats.landmark_reused,
                "ALT index survives movement, goal changes, closures and cost recovery");
        require(AlgorithmLab(*setup.runtime, request).validatePath(actual.path).ok, "ALT emits legal exact paths");
    }
    // Multiple first callers share one lazy initialization and immutable tables.
    PlanSetup concurrent(fixture.data);
    std::vector<std::future<RouteResult>> queries;
    for (int i = 0; i < 8; ++i) queries.push_back(std::async(std::launch::async, [&] {
        return concurrent.planner->plan(request);
    }));
    std::size_t builds = 0;
    for (auto& future : queries) {
        const auto result = future.get();
        require(result.ok && result.stats.landmark_count == 8 && result.path.edges == concurrent.planner->plan(request).path.edges,
                "concurrent ALT queries return deterministic paths");
        builds += !result.stats.landmark_reused;
    }
    require(builds == 1, "concurrent first queries build landmarks exactly once");
    Algorithm parsed = Algorithm::kDijkstra;
    require(parseAlgorithm("alt", parsed) && parsed == Algorithm::kAlt &&
                std::string(algorithmName(parsed)) == "alt" && algorithmCapability(parsed)->uses_heuristic,
            "ALT registry declares heuristic search and canonical name");
}

void runAlgorithmCapabilityRegistryTest() {
    const auto capabilities = zeus::routing::algorithmCapabilities();
    require(capabilities.size() == 10, "tool registry exposes all ten algorithms");
    for (const zeus::routing::AlgorithmCapability& capability : capabilities) {
        const bool expects_k =
            capability.algorithm == zeus::routing::Algorithm::kKShortest;
        require(capability.version != nullptr && capability.deterministic &&
                    capability.exact && (capability.supports_dynamic_weights == (capability.algorithm != zeus::routing::Algorithm::kCH)) &&
                    capability.supports_incremental_repair == zeus::routing::isIncremental(capability.algorithm) &&
                    capability.supports_k_candidates == expects_k &&
                    capability.supports_time_dependency == (capability.algorithm == zeus::routing::Algorithm::kTimeDependent),
                "baseline capability flags match implemented search semantics");
        require(zeus::routing::algorithmCapability(capability.algorithm) == &capability,
                "algorithm lookup returns its stable registry entry");
    }
    const auto* astar =
        zeus::routing::algorithmCapability(zeus::routing::Algorithm::kAStar);
    const auto* bidijkstra = zeus::routing::algorithmCapability(
        zeus::routing::Algorithm::kBidirectionalDijkstra);
    require(astar != nullptr && astar->uses_heuristic &&
                std::string(astar->search_direction) == "forward",
            "A* advertises forward heuristic search");
    require(bidijkstra != nullptr && !bidijkstra->uses_heuristic &&
                std::string(bidijkstra->search_direction) == "bidirectional",
            "bidirectional Dijkstra advertises its search direction");
    const auto* kshortest = zeus::routing::algorithmCapability(
        zeus::routing::Algorithm::kKShortest);
    require(kshortest != nullptr && kshortest->supports_k_candidates &&
                !kshortest->uses_heuristic &&
                std::string(kshortest->search_direction) == "forward",
            "k-shortest advertises multi-candidate forward search");
    zeus::routing::Algorithm parsed = zeus::routing::Algorithm::kDijkstra;
    require(zeus::routing::parseAlgorithm("kshortest", parsed) &&
                parsed == zeus::routing::Algorithm::kKShortest &&
                std::string(zeus::routing::algorithmName(parsed)) == "kshortest",
            "k-shortest parses and names round-trip");
    require(zeus::routing::parseAlgorithm("yen", parsed) &&
                parsed == zeus::routing::Algorithm::kKShortest,
            "k-shortest accepts its alias");
    const auto* dstar = zeus::routing::algorithmCapability(zeus::routing::Algorithm::kDStarLite);
    require(dstar && dstar->uses_heuristic && dstar->supports_incremental_repair &&
                std::string(dstar->search_direction) == "backward",
            "D* Lite advertises incremental backward heuristic search");
    for (const auto* name : {"dstar", "dstar-lite", "D*Lite"})
        require(zeus::routing::parseAlgorithm(name, parsed) && parsed == zeus::routing::Algorithm::kDStarLite &&
                    std::string(zeus::routing::algorithmName(parsed)) == "dstar",
                "D* Lite canonical name and aliases round-trip");
}

void runDStarMovingOriginTest() {
    using namespace zeus::routing;
    Fixture fixture;
    constexpr std::uint32_t side = 13;
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) fixture.addNode(x * 100, y * 100);
    std::vector<zeus::map::EdgeIndex> bottom;
    for (std::uint32_t node = 0; node < side * side; ++node)
        for (const auto delta : {1u, side}) {
            if (node + delta >= side * side || (delta == 1 && node % side == side - 1)) continue;
            const auto edge = fixture.addEdge(node, node + delta, 10);
            fixture.addEdge(node + delta, node, 10);
            if (node < side && delta == 1) bottom.push_back(edge);
        }
    PlanSetup setup(fixture.data);
    IncrementalSearch repair(*setup.runtime);
    auto request = makeRequest({50, 0}, {1150, 0}, Algorithm::kDStarLite);
    request.origin_position = RoutePosition{bottom.front(), 50};
    request.destination_position = RoutePosition{bottom.back(), 50};
    const auto focused = setup.planner->plan(request, &repair);
    request.algorithm = Algorithm::kLpaStar;
    const auto unfocused = setup.planner->plan(request);
    require(focused.ok && near(focused.stats.time_s, unfocused.stats.time_s) &&
                focused.stats.expanded_nodes < unfocused.stats.expanded_nodes,
            "D* Lite heuristic focuses the search on a grid while preserving optimal cost");
    std::vector<std::uint8_t> enabled(fixture.data.edges.size(), 1);
    std::vector<double> factors(enabled.size(), 1);
    RoutingOverlay overlay{enabled, factors};
    request.overlay = &overlay;
    // Alternate exact and twin origins, including backward jumps, so the
    // heuristic anchor and radius both change while old queue entries survive.
    for (int step = 0; step < 80; ++step) {
        const auto index = (step * 7) % bottom.size();
        request.origin = {100.0 * index + 25, 0};
        if (step % 2) request.origin_position = RoutePosition{bottom[index], 25};
        else request.origin_position.reset();
        enabled[bottom[6]] = step % 4 != 0;
        factors[bottom[8]] = step % 3 == 0 ? 4 : 1;
        request.algorithm = Algorithm::kDijkstra;
        const auto baseline = setup.planner->plan(request);
        request.algorithm = Algorithm::kDStarLite;
        const auto actual = setup.planner->plan(request, &repair);
        require(actual.ok == baseline.ok && actual.stats.incremental_reused &&
                    near(actual.stats.time_s, baseline.stats.time_s),
                "moving single/twin origins and changing costs preserve Dijkstra result");
        require(AlgorithmLab(*setup.runtime, request).validatePath(actual.path).ok,
                "moving D* Lite path respects partial endpoints and closures");
        require(setup.planner->plan(request, &repair).stats.expanded_nodes == 0,
                "moving-origin repeat finishes without expansions");
    }
    request.algorithm = Algorithm::kLpaStar;
    require(!setup.planner->plan(request, &repair).stats.incremental_reused,
            "switching to LPA resets retained heuristic priorities");
    request.algorithm = Algorithm::kDStarLite;
    require(!setup.planner->plan(request, &repair).stats.incremental_reused,
            "switching back to D* Lite starts a fresh context");
    require(setup.planner->plan(request, &repair).stats.incremental_reused,
            "D* Lite resumes reuse after an algorithm switch");
    // Stored length can differ from projected distance. The heuristic scale
    // must honor even an unusually fast edge rather than assume max-speed alone.
    fixture.data.edges[bottom[5]].length_m = 1;
    PlanSetup compressed(fixture.data);
    request.overlay = nullptr;
    const auto bounded = compressed.planner->plan(request);
    request.algorithm = Algorithm::kDijkstra;
    const auto reference = compressed.planner->plan(request);
    require(bounded.ok == reference.ok && near(bounded.stats.time_s, reference.stats.time_s),
            "heuristic remains admissible with non-geometric stored edge lengths");
}

void runKShortestTest() {
    // The detour fixture offers exactly two s-t paths: the selection must
    // return what the graph has, not what was asked for.
    {
        Fixture fixture = makeDetourFixture(5.0);
        PlanSetup setup(fixture.data);
        zeus::routing::RouteRequest request =
            makeRequest({-50.0, 1.0}, {150.0, 1.0}, zeus::routing::Algorithm::kKShortest);
        request.k_paths = 4;
        const zeus::routing::RouteResult result = setup.planner->plan(request);
        require(result.ok, "k-shortest route succeeds");
        require(result.alternatives.size() == 2,
                "k-shortest returns the two distinct paths the graph offers");
        require(result.alternatives.front().path.edges ==
                    std::vector<zeus::map::EdgeIndex>({0, 2, 3, 4}),
                "k-shortest first candidate is the plain shortest path");
        require(result.alternatives.back().path.edges ==
                    std::vector<zeus::map::EdgeIndex>({0, 1, 4}),
                "k-shortest second candidate is the slow shortcut");
        require(result.path.edges == result.alternatives.front().path.edges,
                "best path mirrors the first alternative");
        require(result.alternatives.front().time_s <= result.alternatives.back().time_s,
                "k-shortest candidates are ordered by travel time");

        const zeus::routing::RouteResult plain = setup.planner->plan(
            makeRequest({-50.0, 1.0}, {150.0, 1.0}));
        require(near(result.stats.time_s, plain.stats.time_s) &&
                    near(result.alternatives.front().time_s, plain.stats.time_s),
                "k-shortest first candidate matches the plain shortest time");

        const zeus::routing::RouteResult again = setup.planner->plan(request);
        require(again.alternatives.size() == result.alternatives.size(),
                "k-shortest candidate count is deterministic");
        for (std::size_t i = 0; i < result.alternatives.size(); ++i) {
            require(again.alternatives[i].path.edges ==
                        result.alternatives[i].path.edges &&
                        near(again.alternatives[i].time_s,
                             result.alternatives[i].time_s),
                    "k-shortest candidates are deterministic");
        }

        zeus::routing::RouteRequest traced = request;
        traced.record_trace = true;
        const zeus::routing::RouteResult traced_result = setup.planner->plan(traced);
        require(traced_result.ok && !traced_result.search_trace.empty(),
                "recorded search trace is returned");
        bool ordered = true;
        for (std::size_t i = 1; i < traced_result.search_trace.size(); ++i) {
            ordered = ordered && traced_result.search_trace[i].order >
                                     traced_result.search_trace[i - 1].order;
        }
        require(ordered, "trace steps are in strictly increasing settle order");
    }

    // k = 1 degrades to the plain single-path search.
    {
        Fixture fixture = makeDetourFixture(5.0);
        PlanSetup setup(fixture.data);
        zeus::routing::RouteRequest request =
            makeRequest({-50.0, 1.0}, {150.0, 1.0}, zeus::routing::Algorithm::kKShortest);
        request.k_paths = 1;
        const zeus::routing::RouteResult result = setup.planner->plan(request);
        require(result.ok && result.alternatives.empty(),
                "k=1 k-shortest behaves like a plain search without alternatives");
        require(result.path.edges == std::vector<zeus::map::EdgeIndex>({0, 2, 3, 4}),
                "k=1 k-shortest finds the plain shortest path");
    }

    // Turn-restricted maps route k-shortest through the edge-state search.
    {
        Fixture fixture = makeTurnFixture(true);
        PlanSetup setup(fixture.data);
        zeus::routing::RouteRequest request =
            makeRequest({-90.0, 1.0}, {190.0, 1.0}, zeus::routing::Algorithm::kKShortest);
        request.k_paths = 3;
        const zeus::routing::RouteResult result = setup.planner->plan(request);
        require(result.ok && !result.alternatives.empty(),
                "turn-restricted k-shortest succeeds");
        require(result.alternatives.front().path.edges ==
                    std::vector<zeus::map::EdgeIndex>({0, 2, 3, 4}),
                "turn-restricted k-shortest respects the prohibited transition");
    }

    // Unreachable destinations return a first-class failure.
    {
        Fixture fixture;
        const zeus::map::NodeIndex a = fixture.addNode(0.0, 0.0);
        const zeus::map::NodeIndex b = fixture.addNode(100.0, 0.0);
        const zeus::map::NodeIndex c = fixture.addNode(0.0, 1000.0);
        const zeus::map::NodeIndex d = fixture.addNode(100.0, 1000.0);
        fixture.addEdge(a, b, 30.0);
        fixture.addEdge(c, d, 30.0);
        PlanSetup setup(fixture.data);
        zeus::routing::RouteRequest request =
            makeRequest({0.0, 0.0}, {100.0, 1000.0}, zeus::routing::Algorithm::kKShortest);
        request.k_paths = 3;
        const zeus::routing::RouteResult result = setup.planner->plan(request);
        require(!result.ok && result.alternatives.empty(),
                "unreachable k-shortest request fails without alternatives");
        require(result.failure == zeus::routing::RouteFailure::kUnreachable,
                "unreachable k-shortest reports the unreachable failure");
    }
}

void runAlgorithmLabTest() {
    Fixture fixture;
    auto a = fixture.addNode(0, 0), b = fixture.addNode(100, 0);
    auto c = fixture.addNode(200, 0), d = fixture.addNode(100, 100);
    fixture.addEdge(a, b, 10, 10);
    fixture.addEdge(b, c, 10, 11);
    fixture.addEdge(b, d, 10, 12);
    fixture.addEdge(d, b, 10, 13);
    fixture.addTurn(0, 1, false, 7);
    PlanSetup setup(fixture.data);
    auto request = makeRequest({25,0}, {150,0}, zeus::routing::Algorithm::kDijkstra);
    request.origin_position = zeus::routing::RoutePosition{0,25};
    request.destination_position = zeus::routing::RoutePosition{1,50};
    zeus::routing::AlgorithmLab lab(*setup.runtime, request);
    auto route = lab.validate({-1,0,-2});
    require(near(route.stats.time_s,19.5), "lab includes partial edges and turn penalty");
    require(near(route.stats.time_s, lab.baseline().stats.time_s), "lab matches authoritative baseline");
    require(near(route.path.start_offset_m,25) && near(route.path.end_offset_m,50), "lab preserves endpoint offsets");
    for (auto states : std::vector<std::vector<int>>{{-1,-2},{-1,2,-2},{-1,999,-2},{0,-2},{-1,0}}) {
        bool rejected = false;
        try { (void)lab.validate(states); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "lab rejects malformed, disconnected or unknown routes");
    }
    fixture.data.turn_transitions.clear();
    fixture.addTurn(0,1,true);
    PlanSetup forbidden(fixture.data);
    zeus::routing::AlgorithmLab restricted(*forbidden.runtime, request);
    bool rejected = false;
    try { (void)restricted.validate({-1,0,-2}); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "lab rejects forbidden turn into destination");
    route = restricted.validate({-1,0,2,3,-2});
    require(near(route.stats.time_s,32.5), "lab accepts legal detour around turn restriction");
    require(near(route.stats.time_s,restricted.baseline().stats.time_s), "detour agrees with Dijkstra");
    std::vector<std::uint8_t> enabled{1,1,0,1};
    zeus::routing::RoutingOverlay overlay{enabled,{}};
    request.overlay = &overlay;
    zeus::routing::AlgorithmLab closed(*forbidden.runtime,request);
    require(!closed.baseline().ok, "closed detour is unreachable");
    require(closed.neighbors(0).empty(), "closed and forbidden transitions omitted");
    request.overlay = nullptr;
    request.destination_position = zeus::routing::RoutePosition{0,75};
    zeus::routing::AlgorithmLab direct(*setup.runtime,request);
    route = direct.validate({-1,-2});
    require(near(route.stats.time_s,5) && route.path.edges.size()==1, "same edge direct path is sliced once");
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
        runSelfLoopPartialRouteTest();
        runIncrementalTest(zeus::routing::Algorithm::kLpaStar);
        runIncrementalTest(zeus::routing::Algorithm::kDStarLite);
        runTurnRestrictionTest();
        runTurnBidirectionalDifferentialTest();
        runTurnPenaltyTest();
        runTimeDependentTest();
        runTimeDependentOracleTest();
        runCHStaticDifferentialTest();
        runCHIndexTest();
        runCHReuseTest();
        runAltLandmarkTest();
        runAltReuseTest();
        runAlgorithmCapabilityRegistryTest();
        runDStarMovingOriginTest();
        runKShortestTest();
        runAlgorithmLabTest();
        std::cout << "all routing tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
