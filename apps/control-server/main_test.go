package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"math"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func newSimulationTestServer(t *testing.T, commandBody string, workers int) *Server {
	t.Helper()
	dataDir := t.TempDir()
	mapID := "map_sim_test"
	mapDir := filepath.Join(dataDir, "maps", mapID)
	if err := os.MkdirAll(mapDir, 0o755); err != nil {
		t.Fatal(err)
	}
	record := MapRecord{ID: mapID, Name: "Simulation fixture"}
	if err := saveJSONFile(filepath.Join(mapDir, "record.json"), record); err != nil {
		t.Fatal(err)
	}
	commandPath := filepath.Join(t.TempDir(), "fake-zeus-map")
	if err := os.WriteFile(commandPath, []byte(commandBody), 0o755); err != nil {
		t.Fatal(err)
	}
	server := NewServer(Config{
		DataDir: dataDir, WebDir: t.TempDir(), ZeusMap: commandPath,
		SimulateWorkers: workers,
	}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err := server.ensureDirectories(); err != nil {
		t.Fatal(err)
	}
	return server
}

func TestParseInspect(t *testing.T) {
	output := strings.Join([]string{
		"dataset=/tmp/roads.shp",
		"driver=ESRI Shapefile",
		"layers=1",
		"layer[0].name=roads",
		"layer[0].features=123",
		"layer[0].geometry=Line String",
		"layer[0].crs=WGS 84",
		"layer[0].field[0]=ROAD_ID:String",
		"layer[0].field[1]=SPEED:Real",
	}, "\n")

	result := parseInspect(output)
	if result.Driver != "ESRI Shapefile" || result.FeatureCount != 123 {
		t.Fatalf("unexpected inspect result: %#v", result)
	}
	if len(result.Fields) != 2 || result.Fields[0] != "ROAD_ID" {
		t.Fatalf("unexpected fields: %#v", result.Fields)
	}
	if !result.NavigationCompatible || result.SuggestedUsage != "road-network" {
		t.Fatalf("unexpected geometry classification: %#v", result)
	}
}

func TestParseInspectDetectsOSMRoadData(t *testing.T) {
	output := strings.Join([]string{
		"driver=GeoJSON",
		"layer[0].name=osm_roads",
		"layer[0].features=100",
		"layer[0].geometry=Line String",
		"layer[0].field[0]=osm_id:String",
		"layer[0].field[1]=HIGHWAY:String",
	}, "\n")
	result := parseInspect(output)
	if !result.OSMRoadData {
		t.Fatalf("expected highway field to identify OSM road data: %#v", result)
	}
	if hasField([]string{"highway_type"}, "highway") {
		t.Fatal("partial field names must not activate OSM preprocessing")
	}
}

func TestOSMPreprocessOptionsAndCanonicalMapping(t *testing.T) {
	options := normalizeOSMPreprocessOptions(OSMPreprocessOptions{Enabled: true})
	if options.MinLengthMeters != 2 {
		t.Fatalf("unexpected default minimum length: %#v", options)
	}
	if err := validateOSMPreprocessOptions(options); err != nil {
		t.Fatal(err)
	}
	if err := validateOSMPreprocessOptions(OSMPreprocessOptions{
		Enabled: true, MinLengthMeters: 1001,
	}); err == nil {
		t.Fatal("expected excessive minimum length to be rejected")
	}
	mapping := osmCanonicalMapping(Mapping{TargetCRS: "EPSG:32650", SnapToleranceMeters: 0.8})
	if mapping.IDField != "road_id" || mapping.SpeedField != "speed_kph" ||
		mapping.LanesField != "lanes" || mapping.RoadClassField != "road_class" ||
		mapping.TargetCRS != "EPSG:32650" ||
		mapping.SnapToleranceMeters != 0.8 || !mapping.DefaultBidirectional {
		t.Fatalf("unexpected canonical mapping: %#v", mapping)
	}
}

func TestLoadOSMCleaningSummary(t *testing.T) {
	path := filepath.Join(t.TempDir(), "report.json")
	content := `{
  "profile": "car",
  "options": {"include_service": false, "include_track": true, "include_private": false, "min_length_m": 2},
  "input_features": 100,
  "output_features": 61,
  "filtered_features": 39,
  "normalization": {
    "geometry_collections_converted": 1,
    "default_speed_applied": 40,
    "mph_speed_converted": 2,
    "implied_oneway_applied": 3,
    "reverse_oneway_normalized": 4,
    "duplicate_geometries_removed": 5
  },
  "excluded_by_reason": {"non_drivable_class": 39},
  "output_by_class": {"residential": 61}
}`
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	report, err := loadOSMCleaningSummary(path)
	if err != nil {
		t.Fatal(err)
	}
	if report.Profile != "car" || report.InputFeatures != 100 || report.OutputFeatures != 61 ||
		!report.Options.Enabled || !report.Options.IncludeTrack ||
		report.Normalization.ReverseOnewayNormalized != 4 ||
		report.ExcludedByReason["non_drivable_class"] != 39 {
		t.Fatalf("unexpected cleaning summary: %#v", report)
	}
}

func TestClassifyGeometry(t *testing.T) {
	tests := []struct {
		geometry   string
		compatible bool
		usage      string
	}{
		{geometry: "Line String", compatible: true, usage: "road-network"},
		{geometry: "Multi Line String", compatible: true, usage: "road-network"},
		{geometry: "Multi Polygon", compatible: false, usage: "reference-layer"},
		{geometry: "Point", compatible: false, usage: "reference-layer"},
		{geometry: "Unknown (any)", compatible: false, usage: "unsupported"},
	}
	for _, test := range tests {
		t.Run(test.geometry, func(t *testing.T) {
			compatible, usage := classifyGeometry(test.geometry)
			if compatible != test.compatible || usage != test.usage {
				t.Fatalf("classifyGeometry(%q) = %v, %q", test.geometry, compatible, usage)
			}
		})
	}
}

func TestClassifyInspectionCounts(t *testing.T) {
	tests := []struct {
		name       string
		geometry   string
		counts     map[string]int64
		compatible bool
		usage      string
	}{
		{
			name:       "mixed line geometries are road network",
			geometry:   "Unknown (any)",
			counts:     map[string]int64{"Line String": 68138, "Multi Line String": 421},
			compatible: true,
			usage:      "road-network",
		},
		{
			name:       "polygon only is reference layer",
			geometry:   "Unknown (any)",
			counts:     map[string]int64{"Multi Polygon": 1},
			compatible: false,
			usage:      "reference-layer",
		},
		{
			name:       "point only is reference layer",
			geometry:   "Unknown (any)",
			counts:     map[string]int64{"Point": 7, "Multi Point": 3},
			compatible: false,
			usage:      "reference-layer",
		},
		{
			name:       "lines mixed with points are unsupported",
			geometry:   "Unknown (any)",
			counts:     map[string]int64{"Line String": 10, "Point": 2},
			compatible: false,
			usage:      "unsupported",
		},
		{
			name:       "other geometry kinds are unsupported",
			geometry:   "Unknown (any)",
			counts:     map[string]int64{"Geometry Collection": 4},
			compatible: false,
			usage:      "unsupported",
		},
		{
			name:       "without counts falls back to label",
			geometry:   "Unknown (any)",
			compatible: false,
			usage:      "unsupported",
		},
		{
			name:       "without counts uses line label",
			geometry:   "Line String",
			compatible: true,
			usage:      "road-network",
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			compatible, usage := classifyInspection(test.geometry, test.counts)
			if compatible != test.compatible || usage != test.usage {
				t.Fatalf(
					"classifyInspection(%q, %#v) = %v, %q", test.geometry, test.counts, compatible, usage)
			}
		})
	}
}

func TestParseInspectGeometryCounts(t *testing.T) {
	output := strings.Join([]string{
		"dataset=/tmp/roads.geojson",
		"driver=GeoJSON",
		"layers=1",
		"layer[0].name=lines",
		"layer[0].features=68559",
		"layer[0].geometry=Unknown (any)",
		"layer[0].geometry_counts=Line String:68138,Multi Line String:421",
		"layer[0].crs=WGS 84",
	}, "\n")

	result := parseInspect(output)
	if len(result.GeometryCounts) != 2 || result.GeometryCounts["Line String"] != 68138 ||
		result.GeometryCounts["Multi Line String"] != 421 {
		t.Fatalf("unexpected geometry counts: %#v", result.GeometryCounts)
	}
	if !result.NavigationCompatible || result.SuggestedUsage != "road-network" {
		t.Fatalf("unexpected geometry classification: %#v", result)
	}
}

func TestNormalizeReferenceLayerStyle(t *testing.T) {
	polygon, err := normalizeReferenceLayerStyle(ReferenceLayerStyle{}, "Multi Polygon")
	if err != nil || polygon.Color != "#55c7b2" || polygon.Opacity != 0.24 {
		t.Fatalf("unexpected polygon style: %#v, %v", polygon, err)
	}
	point, err := normalizeReferenceLayerStyle(ReferenceLayerStyle{}, "Point")
	if err != nil || point.Color != "#ffb24a" || point.Opacity != 0.9 {
		t.Fatalf("unexpected point style: %#v, %v", point, err)
	}
	if _, err := normalizeReferenceLayerStyle(
		ReferenceLayerStyle{Color: "orange", Opacity: 0.5}, "Polygon"); err == nil {
		t.Fatal("expected invalid color to be rejected")
	}
	if _, err := normalizeReferenceLayerStyle(
		ReferenceLayerStyle{Color: "#112233", Opacity: 1.5}, "Polygon"); err == nil {
		t.Fatal("expected invalid opacity to be rejected")
	}
}

func TestSelectVectorSource(t *testing.T) {
	tests := []struct {
		name    string
		files   []string
		want    string
		wantErr string
	}{
		{name: "geojson", files: []string{"roads.geojson"}, want: "roads.geojson"},
		{name: "json extension", files: []string{"network.json"}, want: "network.json"},
		{
			name:  "complete shapefile",
			files: []string{"roads.SHP", "roads.shx", "roads.dbf", "roads.PRJ", "roads.cpg"},
			want:  "roads.SHP",
		},
		{
			name:    "missing shapefile sidecar",
			files:   []string{"roads.shp", "roads.shx", "roads.dbf"},
			wantErr: "missing .prj",
		},
		{
			name:    "mixed sources",
			files:   []string{"roads.geojson", "other.json"},
			wantErr: "exactly one source",
		},
		{name: "no source", files: []string{"roads.dbf"}, wantErr: "exactly one source"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			got, err := selectVectorSource(test.files)
			if test.wantErr != "" {
				if err == nil || !strings.Contains(err.Error(), test.wantErr) {
					t.Fatalf("expected error containing %q, got %v", test.wantErr, err)
				}
				return
			}
			if err != nil || got != test.want {
				t.Fatalf("selectVectorSource() = %q, %v; want %q", got, err, test.want)
			}
		})
	}
}

func TestSelectTurnRestrictionsFile(t *testing.T) {
	selected, err := selectTurnRestrictionsFile(
		[]string{"roads.geojson", "wuhan-turn-restrictions.CSV"})
	if err != nil || selected != "wuhan-turn-restrictions.CSV" {
		t.Fatalf("unexpected sidecar selection: %q, %v", selected, err)
	}
	if selected, err := selectTurnRestrictionsFile([]string{"roads.geojson"}); err != nil || selected != "" {
		t.Fatalf("sidecar should be optional: %q, %v", selected, err)
	}
	if _, err := selectTurnRestrictionsFile([]string{"a.csv", "b.csv"}); err == nil {
		t.Fatal("multiple turn sidecars must be rejected")
	}
}

func TestAllowedUploadExtensions(t *testing.T) {
	for _, extension := range []string{".shp", ".prj", ".geojson", ".GEOJSON", ".json", ".csv"} {
		if !allowedUploadExtension(extension) {
			t.Errorf("expected %s to be accepted", extension)
		}
	}
	if allowedUploadExtension(".zip") {
		t.Error("zip must not be accepted")
	}
}

func TestResolveTurnRestrictionsFile(t *testing.T) {
	directory := t.TempDir()
	const filename = "wuhan-turn-restrictions.csv"
	if err := os.WriteFile(filepath.Join(directory, filename), []byte("# turns\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	path, err := resolveTurnRestrictionsFile(
		directory, filename, []string{"roads.geojson", filename})
	if err != nil || path != filepath.Join(directory, filename) {
		t.Fatalf("unexpected turn sidecar resolution: %q, %v", path, err)
	}
	for _, invalid := range []string{"../turns.csv", "turns.txt", "missing.csv"} {
		if _, err := resolveTurnRestrictionsFile(
			directory, invalid, []string{"roads.geojson", filename}); err == nil {
			t.Fatalf("expected %q to be rejected", invalid)
		}
	}
	if path, err := resolveTurnRestrictionsFile(directory, "", nil); err != nil || path != "" {
		t.Fatalf("empty optional sidecar should resolve cleanly: %q, %v", path, err)
	}
}

func waitForTerminalJob(t *testing.T, manager *JobManager, id string) ImportJob {
	t.Helper()
	initial, updates, unsubscribe, ok := manager.Subscribe(id)
	if !ok {
		t.Fatal("job was not found")
	}
	defer unsubscribe()
	if terminalJob(initial.Status) {
		return initial
	}
	timer := time.NewTimer(2 * time.Second)
	defer timer.Stop()
	for {
		select {
		case job := <-updates:
			if terminalJob(job.Status) {
				return job
			}
		case <-timer.C:
			t.Fatal("timed out waiting for terminal job")
		}
	}
}

func TestJobManagerPublishesSuccessfulResult(t *testing.T) {
	manager := NewJobManager(1)
	job := manager.Submit(func(_ context.Context, publish func(JobProgress)) (MapRecord, error) {
		publish(JobProgress{Phase: "topology", Progress: 55, Message: "building"})
		return MapRecord{ID: "map_test", Issues: []ValidationIssue{{Code: "TEST"}}}, nil
	})
	completed := waitForTerminalJob(t, manager, job.ID)
	if completed.Status != JobSucceeded || completed.Progress != 100 {
		t.Fatalf("unexpected completed job: %#v", completed)
	}
	if completed.Map == nil || completed.Map.ID != "map_test" {
		t.Fatalf("missing map result: %#v", completed.Map)
	}
}

func TestJobManagerCancelsWork(t *testing.T) {
	manager := NewJobManager(1)
	started := make(chan struct{})
	job := manager.Submit(func(ctx context.Context, _ func(JobProgress)) (MapRecord, error) {
		close(started)
		<-ctx.Done()
		return MapRecord{}, ctx.Err()
	})
	<-started
	if _, ok := manager.Cancel(job.ID); !ok {
		t.Fatal("cancel did not find job")
	}
	completed := waitForTerminalJob(t, manager, job.ID)
	if completed.Status != JobCancelled || !strings.Contains(completed.Error, "canceled") {
		t.Fatalf("unexpected cancelled job: %#v", completed)
	}
}

func TestJobManagerRespectsWorkerLimit(t *testing.T) {
	manager := NewJobManager(1)
	firstStarted := make(chan struct{})
	releaseFirst := make(chan struct{})
	first := manager.Submit(func(_ context.Context, _ func(JobProgress)) (MapRecord, error) {
		close(firstStarted)
		<-releaseFirst
		return MapRecord{ID: "first"}, nil
	})
	<-firstStarted
	secondStarted := make(chan struct{})
	second := manager.Submit(func(_ context.Context, _ func(JobProgress)) (MapRecord, error) {
		close(secondStarted)
		return MapRecord{ID: "second"}, nil
	})
	select {
	case <-secondStarted:
		t.Fatal("second job started before a worker was available")
	default:
	}
	queued, ok := manager.Get(second.ID)
	if !ok || queued.Status != JobQueued {
		t.Fatalf("second job was not queued: %#v", queued)
	}
	close(releaseFirst)
	if completed := waitForTerminalJob(t, manager, first.ID); completed.Status != JobSucceeded {
		t.Fatalf("first job did not succeed: %#v", completed)
	}
	if completed := waitForTerminalJob(t, manager, second.ID); completed.Status != JobSucceeded {
		t.Fatalf("second job did not succeed: %#v", completed)
	}
}

func TestJobManagerReportsFailure(t *testing.T) {
	manager := NewJobManager(1)
	job := manager.Submit(func(_ context.Context, _ func(JobProgress)) (MapRecord, error) {
		return MapRecord{}, errors.New("broken fixture")
	})
	completed := waitForTerminalJob(t, manager, job.ID)
	if completed.Status != JobFailed || completed.Error != "broken fixture" {
		t.Fatalf("unexpected failed job: %#v", completed)
	}
}

func TestParseValidation(t *testing.T) {
	output := strings.Join([]string{
		"nodes=12",
		"directed_edges=24",
		"components=2",
		"largest_component_nodes=10",
		"turn_transitions=7",
		"fatal=0",
		"errors=1",
		"warnings=3",
		"info=2",
		"issue=warning:DANGLING_ENDPOINT source=- location=12.5,30 message=\"Topology node is dangling\"",
	}, "\n")

	summary, issues := parseValidation(output)
	if summary.Nodes != 12 || summary.DirectedEdges != 24 ||
		summary.TurnTransitions != 7 || summary.Warnings != 3 {
		t.Fatalf("unexpected summary: %#v", summary)
	}
	if len(issues) != 1 || issues[0].Code != "DANGLING_ENDPOINT" {
		t.Fatalf("unexpected issues: %#v", issues)
	}
}

func TestParseQuery(t *testing.T) {
	output := strings.Join([]string{
		"query_runtime_xy=500000.000,3450000.000",
		"matches=1",
		"match[0].edge=42 road_id=123456 source=road-a offset_s=18.250 distance=1.200 heading_delta=0.050 confidence=0.912 projected=500001.000,3450001.000",
	}, "\n")

	result := parseQuery(output)
	if len(result.Matches) != 1 || result.Matches[0].Edge != 42 {
		t.Fatalf("unexpected query result: %#v", result)
	}
	if result.Matches[0].RoadID != "123456" || result.Matches[0].Confidence != 0.912 {
		t.Fatalf("unexpected candidate: %#v", result.Matches[0])
	}
}

func TestParseSimulate(t *testing.T) {
	t.Run("successful simulation", func(t *testing.T) {
		output := strings.Join([]string{
			"simulate=ok",
			"vehicles=100",
			"arrived=15",
			"unroutable=0",
			"waiting_at_end=0",
			"driving_at_end=85",
			"ticks=900",
			"route_plans=1",
			"avg_travel_s=799.142",
			"min_travel_s=773.521",
			"max_travel_s=811.016",
			"total_distance_m=617881.984",
			"samples=10192",
			"deadlock=0",
			"compute_ms=36.229",
			"control_events=5",
			"vehicle_controls=2",
			"edge_controls=2",
			"junction_controls=1",
		}, "\n")

		result := parseSimulate(output)
		if !result.OK || result.Vehicles != 100 || result.Arrived != 15 ||
			result.DrivingAtEnd != 85 || result.Ticks != 900 {
			t.Fatalf("unexpected simulation result: %#v", result)
		}
		if result.RoutePlans != 1 || result.Deadlock {
			t.Fatalf("unexpected plan pooling: %#v", result)
		}
		if result.Samples != 10192 {
			t.Fatalf("unexpected sample count: %#v", result)
		}
		if result.ControlEvents != 5 || result.VehicleControls != 2 ||
			result.RoadControls != 2 || result.JunctionControls != 1 {
			t.Fatalf("unexpected control statistics: %#v", result)
		}
		if result.AvgTravelS != 799.142 || result.MaxTravelS != 811.016 ||
			result.TotalDistanceM != 617881.984 || result.ComputeMs != 36.229 {
			t.Fatalf("unexpected simulation statistics: %#v", result)
		}
	})

	t.Run("failed simulation", func(t *testing.T) {
		result := parseSimulate("simulate=failed\nreason=unroutable\nmessage=no demand routed")
		if result.OK || result.Reason != "unroutable" || result.Message != "no demand routed" {
			t.Fatalf("unexpected failure result: %#v", result)
		}
	})
}

func TestBuildSimulateArgs(t *testing.T) {
	lon := 114.26
	lat := 30.47
	valid := SimulateRequest{
		FromLon: &lon, FromLat: &lat, ToLon: &lon, ToLat: &lat,
		Count: 100, SpreadSeconds: 600, DurationSeconds: 900,
		StepSeconds: 1, SampleIntervalSeconds: 15, Algorithm: "biastar",
	}

	args, err := buildSimulateArgs(valid)
	if err != nil {
		t.Fatalf("valid request rejected: %v", err)
	}
	joined := strings.Join(args, " ")
	for _, wanted := range []string{"--algorithm biastar", "--count 100", "--spread", "--duration"} {
		if !strings.Contains(joined, wanted) {
			t.Fatalf("args missing %q: %v", wanted, args)
		}
	}

	if _, err := buildSimulateArgs(SimulateRequest{
		FromLon: &lon, FromLat: &lat, ToLon: &lon,
	}); err == nil {
		t.Fatal("missing destination should fail")
	}
	if _, err := buildSimulateArgs(SimulateRequest{
		FromLon: &lon, FromLat: &lat, ToLon: &lon, ToLat: &lat, Algorithm: "bfs",
	}); err == nil {
		t.Fatal("unknown algorithm should fail")
	}
	if _, err := buildSimulateArgs(SimulateRequest{
		FromLon: &lon, FromLat: &lat, ToLon: &lon, ToLat: &lat, Count: 20000,
	}); err == nil {
		t.Fatal("oversized count should fail")
	}
	if _, err := buildSimulateArgs(SimulateRequest{
		FromLon: &lon, FromLat: &lat, ToLon: &lon, ToLat: &lat,
		Count: 10000, DurationSeconds: 28800, SampleIntervalSeconds: 1,
	}); err == nil {
		t.Fatal("sample budget overflow should fail")
	}
	if _, err := buildSimulateArgs(SimulateRequest{
		FromLon: &lon, FromLat: &lat, ToLon: &lon, ToLat: &lat,
		DurationSeconds: 900, SpreadSeconds: -1,
	}); err == nil {
		t.Fatal("negative spread should fail")
	}
	if _, err := buildSimulateArgs(SimulateRequest{
		FromLon: &lon, FromLat: &lat, ToLon: &lon, ToLat: &lat,
		DurationSeconds: 900, StepSeconds: 2, SampleIntervalSeconds: 1,
	}); err == nil {
		t.Fatal("sampling faster than the tick should fail")
	}
	nonFinite := math.Inf(1)
	if _, err := buildSimulateArgs(SimulateRequest{
		FromLon: &nonFinite, FromLat: &lat, ToLon: &lon, ToLat: &lat,
	}); err == nil {
		t.Fatal("non-finite coordinates should fail")
	}
}

func TestBuildSimulationControls(t *testing.T) {
	request := SimulateRequest{
		Count: 3, DurationSeconds: 60,
		VehicleControls: []VehicleSimulationControl{
			{TimeSeconds: 0, VehicleID: 2, Action: "hold"},
			{TimeSeconds: 12.5, VehicleID: 2, Action: "speedFactor", Value: 0.4},
		},
		RoadControls: []RoadSimulationControl{
			{TimeSeconds: 10, EdgeIDs: []uint32{7, 8, 8}, Action: "close"},
			{TimeSeconds: 20, EdgeIDs: []uint32{7}, Action: "capacityFactor", Value: 0.5},
		},
		JunctionControls: []JunctionSimulationControl{
			{TimeSeconds: 15, NodeID: 4, Action: "close"},
		},
	}
	content, err := buildSimulationControls(request, ValidationSummary{Nodes: 10, DirectedEdges: 20})
	if err != nil {
		t.Fatalf("valid controls rejected: %v", err)
	}
	for _, wanted := range []string{
		"0,vehicle,2,hold,1",
		"12.5,vehicle,2,speed_factor,0.4",
		"10,edge,7,close,1",
		"10,edge,8,close,1",
		"20,edge,7,capacity_factor,0.5",
		"15,junction,4,close,1",
	} {
		if !strings.Contains(content, wanted) {
			t.Errorf("control file missing %q:\n%s", wanted, content)
		}
	}
	if strings.Count(content, "10,edge,8,close,1") != 1 {
		t.Fatalf("duplicate edge target was not removed:\n%s", content)
	}

	tests := []struct {
		name    string
		request SimulateRequest
	}{
		{"vehicle range", SimulateRequest{Count: 2, DurationSeconds: 60, VehicleControls: []VehicleSimulationControl{{VehicleID: 2, Action: "hold"}}}},
		{"event time", SimulateRequest{Count: 2, DurationSeconds: 60, VehicleControls: []VehicleSimulationControl{{TimeSeconds: 60, VehicleID: 0, Action: "hold"}}}},
		{"vehicle action", SimulateRequest{Count: 2, DurationSeconds: 60, VehicleControls: []VehicleSimulationControl{{VehicleID: 0, Action: "close"}}}},
		{"speed factor", SimulateRequest{Count: 2, DurationSeconds: 60, VehicleControls: []VehicleSimulationControl{{VehicleID: 0, Action: "speedFactor", Value: 0}}}},
		{"empty road", SimulateRequest{DurationSeconds: 60, RoadControls: []RoadSimulationControl{{Action: "close"}}}},
		{"edge range", SimulateRequest{DurationSeconds: 60, RoadControls: []RoadSimulationControl{{EdgeIDs: []uint32{20}, Action: "close"}}}},
		{"road action", SimulateRequest{DurationSeconds: 60, RoadControls: []RoadSimulationControl{{EdgeIDs: []uint32{1}, Action: "hold"}}}},
		{"junction range", SimulateRequest{DurationSeconds: 60, JunctionControls: []JunctionSimulationControl{{NodeID: 10, Action: "close"}}}},
		{"junction action", SimulateRequest{DurationSeconds: 60, JunctionControls: []JunctionSimulationControl{{NodeID: 1, Action: "speedFactor"}}}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if _, err := buildSimulationControls(test.request, ValidationSummary{Nodes: 10, DirectedEdges: 20}); err == nil {
				t.Fatal("invalid controls should fail")
			}
		})
	}
}

func TestSimulateEndpointSuccess(t *testing.T) {
	server := newSimulationTestServer(t, `#!/bin/sh
trajectory=""
playback=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --output) trajectory="$2"; shift 2 ;;
    --playback) playback="$2"; shift 2 ;;
    *) shift ;;
  esac
done
printf '%s' '{"type":"FeatureCollection","features":[]}' > "$trajectory"
printf '%s' '{"duration_s":900,"step_s":1,"sample_interval_s":15,"vehicles":[]}' > "$playback"
printf '%s\n' 'simulate=ok' 'vehicles=2' 'arrived=1' 'driving_at_end=1' 'ticks=900' 'route_plans=1' 'samples=12' 'compute_ms=3.5'
`, 2)

	payload := []byte(`{"fromLon":114.26,"fromLat":30.47,"toLon":114.31,"toLat":30.52,"count":2,"durationSeconds":900,"stepSeconds":1,"sampleIntervalSeconds":15,"algorithm":"biastar"}`)
	request := httptest.NewRequest(
		http.MethodPost, "/api/maps/map_sim_test/simulate", bytes.NewReader(payload))
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("unexpected status %d: %s", response.Code, response.Body.String())
	}
	var result SimulateResponse
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if !result.OK || result.Vehicles != 2 || result.Arrived != 1 ||
		result.Samples != 12 || result.ComputeMs != 3.5 {
		t.Fatalf("unexpected simulation response: %#v", result)
	}
	if len(result.GeoJSON) == 0 || len(result.Playback) == 0 ||
		!json.Valid(result.GeoJSON) || !json.Valid(result.Playback) {
		t.Fatalf("simulation artifacts were not inlined: %#v", result)
	}
}

func TestSimulateEndpointPassesValidatedControls(t *testing.T) {
	server := newSimulationTestServer(t, `#!/bin/sh
trajectory=""
playback=""
controls=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --output) trajectory="$2"; shift 2 ;;
    --playback) playback="$2"; shift 2 ;;
    --controls) controls="$2"; shift 2 ;;
    *) shift ;;
  esac
done
test -f "$controls" || exit 10
grep -q '10,vehicle,1,hold,1' "$controls" || exit 11
grep -q '20,edge,7,capacity_factor,0.5' "$controls" || exit 12
grep -q '30,junction,4,close,1' "$controls" || exit 13
printf '%s' '{"type":"FeatureCollection","features":[]}' > "$trajectory"
printf '%s' '{"duration_s":60,"step_s":1,"sample_interval_s":5,"controls":[],"vehicles":[]}' > "$playback"
printf '%s\n' 'simulate=ok' 'vehicles=2' 'control_events=3' 'vehicle_controls=1' 'edge_controls=1' 'junction_controls=1'
`, 2)

	payload := []byte(`{
		"fromLon":114.26,"fromLat":30.47,"toLon":114.31,"toLat":30.52,
		"count":2,"durationSeconds":60,"stepSeconds":1,"sampleIntervalSeconds":5,
		"vehicleControls":[{"timeSeconds":10,"vehicleId":1,"action":"hold"}],
		"roadControls":[{"timeSeconds":20,"edgeIds":[7],"action":"capacityFactor","value":0.5}],
		"junctionControls":[{"timeSeconds":30,"nodeId":4,"action":"close"}]
	}`)
	request := httptest.NewRequest(
		http.MethodPost, "/api/maps/map_sim_test/simulate", bytes.NewReader(payload))
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("unexpected status %d: %s", response.Code, response.Body.String())
	}
	var result SimulateResponse
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if !result.OK || result.ControlEvents != 3 || result.VehicleControls != 1 ||
		result.RoadControls != 1 || result.JunctionControls != 1 {
		t.Fatalf("unexpected control response: %#v", result)
	}
}

func TestSimulateEndpointRejectsInvalidRequest(t *testing.T) {
	server := newSimulationTestServer(t, "#!/bin/sh\nexit 99\n", 2)
	payload := []byte(`{"fromLon":114.26,"fromLat":30.47,"toLon":114.31,"toLat":30.52,"count":10001}`)
	request := httptest.NewRequest(
		http.MethodPost, "/api/maps/map_sim_test/simulate", bytes.NewReader(payload))
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)
	if response.Code != http.StatusBadRequest {
		t.Fatalf("unexpected status %d: %s", response.Code, response.Body.String())
	}
}

func TestSimulateEndpointReportsCommandFailure(t *testing.T) {
	server := newSimulationTestServer(t, "#!/bin/sh\nprintf 'native simulation failed\\n'\nexit 2\n", 2)
	payload := []byte(`{"fromLon":114.26,"fromLat":30.47,"toLon":114.31,"toLat":30.52}`)
	request := httptest.NewRequest(
		http.MethodPost, "/api/maps/map_sim_test/simulate", bytes.NewReader(payload))
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)
	if response.Code != http.StatusUnprocessableEntity {
		t.Fatalf("unexpected status %d: %s", response.Code, response.Body.String())
	}
}

func TestSimulateEndpointHonorsSemaphoreCancellation(t *testing.T) {
	server := newSimulationTestServer(t, "#!/bin/sh\nexit 99\n", 1)
	server.simSlots <- struct{}{}
	defer func() { <-server.simSlots }()

	payload := []byte(`{"fromLon":114.26,"fromLat":30.47,"toLon":114.31,"toLat":30.52}`)
	base := httptest.NewRequest(
		http.MethodPost, "/api/maps/map_sim_test/simulate", bytes.NewReader(payload))
	context, cancel := context.WithCancel(base.Context())
	cancel()
	request := base.WithContext(context)
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("unexpected status %d: %s", response.Code, response.Body.String())
	}
}

func TestParseRoute(t *testing.T) {
	t.Run("successful route", func(t *testing.T) {
		output := strings.Join([]string{
			"route=ok",
			"algorithm=astar",
			"origin.edge=128 road_id=429000123 source=osm-1 offset_s=18.250 distance=1.200 confidence=0.912",
			"dest.edge=256 road_id=429999999 source=osm-2 offset_s=42.500 distance=0.800 confidence=0.940",
			"edges=87",
			"length_m=8466.264",
			"time_s=761.964",
			"expanded_nodes=5308",
			"compute_ms=0.743",
		}, "\n")

		result := parseRoute(output)
		if !result.OK || result.Algorithm != "astar" || result.Edges != 87 {
			t.Fatalf("unexpected route result: %#v", result)
		}
		if result.LengthM != 8466.264 || result.TimeS != 761.964 ||
			result.ExpandedNodes != 5308 || result.ComputeMs != 0.743 {
			t.Fatalf("unexpected route statistics: %#v", result)
		}
		if result.Origin.Edge != 128 || result.Origin.RoadID != "429000123" ||
			result.Origin.OffsetS != 18.25 || result.Origin.Confidence != 0.912 {
			t.Fatalf("unexpected origin match: %#v", result.Origin)
		}
		if result.Destination.Edge != 256 || result.Destination.OffsetS != 42.5 ||
			result.Destination.Distance != 0.8 {
			t.Fatalf("unexpected destination match: %#v", result.Destination)
		}
	})

	t.Run("failed route", func(t *testing.T) {
		output := strings.Join([]string{
			"route=failed",
			"algorithm=dijkstra",
			"reason=unreachable",
			"message=no path connects the matched origin and destination edges",
		}, "\n")

		result := parseRoute(output)
		if result.OK || result.Reason != "unreachable" || result.Algorithm != "dijkstra" {
			t.Fatalf("unexpected failure result: %#v", result)
		}
		if result.Message == "" {
			t.Fatalf("failure message is missing: %#v", result)
		}
	})

	t.Run("malformed lines are skipped", func(t *testing.T) {
		result := parseRoute(strings.Join([]string{
			"route=ok",
			"origin.edge=not-a-number road_id=1 source=x offset_s=1 distance=1 confidence=1",
			"garbage line without equals",
			"edges=not-a-number",
		}, "\n"))
		if !result.OK {
			t.Fatalf("route status should parse: %#v", result)
		}
		if result.Edges != 0 || result.Origin.Edge != 0 {
			t.Fatalf("malformed values should default to zero: %#v", result)
		}
	})
}

func TestHealthRoute(t *testing.T) {
	server := NewServer(Config{DataDir: t.TempDir(), WebDir: t.TempDir()}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err := server.ensureDirectories(); err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodGet, "/api/health", nil)
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("unexpected status: %d", response.Code)
	}
	if !strings.Contains(response.Body.String(), `"ok":true`) {
		t.Fatalf("unexpected body: %s", response.Body.String())
	}
}

func TestImportRejectsReferenceGeometryBeforeStartingJob(t *testing.T) {
	dataDir := t.TempDir()
	server := NewServer(
		Config{DataDir: dataDir, WebDir: t.TempDir(), ZeusMap: "should-not-run"},
		slog.New(slog.NewTextHandler(io.Discard, nil)),
	)
	if err := server.ensureDirectories(); err != nil {
		t.Fatal(err)
	}
	uploadID := "upl_polygon"
	uploadDir := filepath.Join(dataDir, "uploads", uploadID)
	if err := os.MkdirAll(uploadDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(uploadDir, "boundary.geojson"), []byte("{}"), 0o644); err != nil {
		t.Fatal(err)
	}
	inspection := InspectResult{
		UploadID:             uploadID,
		SourceFile:           "boundary.geojson",
		Geometry:             "Multi Polygon",
		NavigationCompatible: false,
		SuggestedUsage:       "reference-layer",
	}
	if err := saveJSONFile(filepath.Join(uploadDir, "inspect.json"), inspection); err != nil {
		t.Fatal(err)
	}
	payload, err := json.Marshal(ImportRequest{
		UploadID:   uploadID,
		SourceFile: "boundary.geojson",
		Name:       "Wuhan boundary",
		Mapping:    Mapping{DefaultSpeedKPH: 40, SnapToleranceMeters: 0.5},
	})
	if err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodPost, "/api/maps/import", bytes.NewReader(payload))
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)
	if response.Code != http.StatusUnprocessableEntity {
		t.Fatalf("unexpected status %d: %s", response.Code, response.Body.String())
	}
	if !strings.Contains(response.Body.String(), "LineString or MultiLineString") {
		t.Fatalf("unexpected body: %s", response.Body.String())
	}
}

func TestImportRejectsMixedGeometryBeforeStartingJob(t *testing.T) {
	dataDir := t.TempDir()
	server := NewServer(
		Config{DataDir: dataDir, WebDir: t.TempDir(), ZeusMap: "should-not-run"},
		slog.New(slog.NewTextHandler(io.Discard, nil)),
	)
	if err := server.ensureDirectories(); err != nil {
		t.Fatal(err)
	}
	uploadID := "upl_mixed"
	uploadDir := filepath.Join(dataDir, "uploads", uploadID)
	if err := os.MkdirAll(uploadDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(uploadDir, "mixed.geojson"), []byte("{}"), 0o644); err != nil {
		t.Fatal(err)
	}
	inspection := InspectResult{
		UploadID:       uploadID,
		SourceFile:     "mixed.geojson",
		Geometry:       "Unknown (any)",
		GeometryCounts: map[string]int64{"Line String": 10, "Point": 2},
	}
	if err := saveJSONFile(filepath.Join(uploadDir, "inspect.json"), inspection); err != nil {
		t.Fatal(err)
	}
	payload, err := json.Marshal(ImportRequest{
		UploadID:   uploadID,
		SourceFile: "mixed.geojson",
		Name:       "Mixed dataset",
		Mapping:    Mapping{DefaultSpeedKPH: 40, SnapToleranceMeters: 0.5},
	})
	if err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodPost, "/api/maps/import", bytes.NewReader(payload))
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)
	if response.Code != http.StatusUnprocessableEntity {
		t.Fatalf("unexpected status %d: %s", response.Code, response.Body.String())
	}
	if !strings.Contains(response.Body.String(), "LineString or MultiLineString") {
		t.Fatalf("unexpected body: %s", response.Body.String())
	}
}

func TestReferenceLayerRejectsRoadGeometry(t *testing.T) {
	dataDir := t.TempDir()
	server := NewServer(
		Config{DataDir: dataDir, WebDir: t.TempDir(), ZeusMap: "should-not-run"},
		slog.New(slog.NewTextHandler(io.Discard, nil)),
	)
	if err := server.ensureDirectories(); err != nil {
		t.Fatal(err)
	}
	uploadID := "upl_roads"
	uploadDir := filepath.Join(dataDir, "uploads", uploadID)
	if err := os.MkdirAll(uploadDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(uploadDir, "roads.geojson"), []byte("{}"), 0o644); err != nil {
		t.Fatal(err)
	}
	inspection := InspectResult{
		UploadID:             uploadID,
		SourceFile:           "roads.geojson",
		Geometry:             "Line String",
		NavigationCompatible: true,
		SuggestedUsage:       "road-network",
	}
	if err := saveJSONFile(filepath.Join(uploadDir, "inspect.json"), inspection); err != nil {
		t.Fatal(err)
	}
	payload, err := json.Marshal(ReferenceLayerRequest{
		UploadID: uploadID, SourceFile: "roads.geojson", Name: "Road overlay",
	})
	if err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(
		http.MethodPost, "/api/reference-layers", bytes.NewReader(payload))
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)
	if response.Code != http.StatusUnprocessableEntity {
		t.Fatalf("unexpected status %d: %s", response.Code, response.Body.String())
	}
	if !strings.Contains(response.Body.String(), "not a point or polygon") {
		t.Fatalf("unexpected body: %s", response.Body.String())
	}
}

func TestListReferenceLayers(t *testing.T) {
	dataDir := t.TempDir()
	server := NewServer(
		Config{DataDir: dataDir, WebDir: t.TempDir()},
		slog.New(slog.NewTextHandler(io.Discard, nil)),
	)
	if err := server.ensureDirectories(); err != nil {
		t.Fatal(err)
	}
	directory := filepath.Join(dataDir, "reference-layers", "ref_test")
	if err := os.MkdirAll(directory, 0o755); err != nil {
		t.Fatal(err)
	}
	record := ReferenceLayerRecord{
		ID: "ref_test", Name: "Boundary", CreatedAt: time.Now().UTC(),
		Geometry: "Multi Polygon", FeatureCount: 1,
		Style: ReferenceLayerStyle{Color: "#55c7b2", Opacity: 0.24},
	}
	if err := saveJSONFile(filepath.Join(directory, "record.json"), record); err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodGet, "/api/reference-layers", nil)
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)
	if response.Code != http.StatusOK || !strings.Contains(response.Body.String(), "Boundary") {
		t.Fatalf("unexpected response %d: %s", response.Code, response.Body.String())
	}
}

func TestUpdateAndDeleteReferenceLayer(t *testing.T) {
	dataDir := t.TempDir()
	server := NewServer(
		Config{DataDir: dataDir, WebDir: t.TempDir()},
		slog.New(slog.NewTextHandler(io.Discard, nil)),
	)
	if err := server.ensureDirectories(); err != nil {
		t.Fatal(err)
	}
	layerID := "ref_manage"
	directory := filepath.Join(dataDir, "reference-layers", layerID)
	if err := os.MkdirAll(directory, 0o755); err != nil {
		t.Fatal(err)
	}
	record := ReferenceLayerRecord{
		ID: layerID, Name: "Old name", Geometry: "Multi Polygon",
		Style: ReferenceLayerStyle{Color: "#55c7b2", Opacity: 0.24},
	}
	if err := saveJSONFile(filepath.Join(directory, "record.json"), record); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(directory, "layer.geojson"), []byte("{}"), 0o644); err != nil {
		t.Fatal(err)
	}
	payload := []byte(`{"name":"New name","style":{"color":"#112233","opacity":0.42}}`)
	request := httptest.NewRequest(
		http.MethodPatch, "/api/reference-layers/"+layerID, bytes.NewReader(payload))
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("unexpected update response %d: %s", response.Code, response.Body.String())
	}
	updated, err := loadReferenceLayerRecord(filepath.Join(directory, "record.json"))
	if err != nil {
		t.Fatal(err)
	}
	if updated.Name != "New name" || updated.Style.Color != "#112233" || updated.Style.Opacity != 0.42 {
		t.Fatalf("unexpected updated record: %#v", updated)
	}

	request = httptest.NewRequest(http.MethodDelete, "/api/reference-layers/"+layerID, nil)
	response = httptest.NewRecorder()
	server.routes().ServeHTTP(response, request)
	if response.Code != http.StatusNoContent {
		t.Fatalf("unexpected delete response %d: %s", response.Code, response.Body.String())
	}
	if _, err := os.Stat(directory); !os.IsNotExist(err) {
		t.Fatalf("reference layer directory still exists: %v", err)
	}
}
