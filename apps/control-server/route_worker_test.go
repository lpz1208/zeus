package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

func createFakeRouteWorker(t *testing.T) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "fake-route-worker")
	script := `#!/bin/sh
if [ "$1" != "route-worker" ]; then
  exit 2
fi
printf 'ZEUS_ROUTE_WORKER\t1\n'
tab=$(printf '\t')
while IFS="$tab" read -r from_lon from_lat to_lon to_lat algorithm max_distance output_path; do
  if [ "$algorithm" = "hang" ]; then
    continue
  fi
  if [ "$algorithm" = "dijkstra" ]; then
    payload=$(printf 'route=failed\nalgorithm=dijkstra\nreason=unreachable\nmessage=no path\nworker_pid=%s' "$$")
    exit_code=3
  else
    printf '{"type":"FeatureCollection","features":[]}' > "$output_path"
    payload=$(printf 'route=ok\nalgorithm=%s\norigin.edge=1 road_id=10 source=a offset_s=1 distance=0 confidence=1\ndest.edge=2 road_id=20 source=b offset_s=2 distance=0 confidence=1\nedges=2\nlength_m=100\ntime_s=10\nexpanded_nodes=3\ncompute_ms=0.5\nworker_pid=%s' "$algorithm" "$$")
    exit_code=0
  fi
  size=$(printf '%s' "$payload" | wc -c | tr -d ' ')
  printf 'ZEUS_ROUTE_RESPONSE\t%s\t%s\n%s' "$exit_code" "$size" "$payload"
done
`
	if err := os.WriteFile(path, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	return path
}

func outputValue(output, key string) string {
	for _, line := range strings.Split(output, "\n") {
		if value, ok := strings.CutPrefix(line, key+"="); ok {
			return value
		}
	}
	return ""
}

func TestRouteWorkerManagerReusesProcessAndRestartsAfterTimeout(t *testing.T) {
	manager := NewRouteWorkerManager(createFakeRouteWorker(t), time.Second, 2)
	defer manager.Close()
	request := RouteWorkerRequest{
		FromLon: 1, FromLat: 2, ToLon: 3, ToLat: 4,
		Algorithm: "astar", MaxDistance: 100,
		OutputPath: filepath.Join(t.TempDir(), "first.geojson"),
	}
	first, err := manager.Route(context.Background(), "map-a.zmap", request)
	if err != nil || first.ExitCode != 0 {
		t.Fatalf("first worker request failed: %#v, %v", first, err)
	}
	request.OutputPath = filepath.Join(t.TempDir(), "second.geojson")
	second, err := manager.Route(context.Background(), "map-a.zmap", request)
	if err != nil || outputValue(first.Output, "worker_pid") == "" ||
		outputValue(first.Output, "worker_pid") != outputValue(second.Output, "worker_pid") {
		t.Fatalf("map worker was not reused: first=%q second=%q err=%v", first.Output, second.Output, err)
	}

	hanging := request
	hanging.Algorithm = "hang"
	ctx, cancel := context.WithTimeout(context.Background(), 40*time.Millisecond)
	defer cancel()
	if _, err := manager.Route(ctx, "map-a.zmap", hanging); err == nil {
		t.Fatal("hanging worker request should honor context cancellation")
	}
	request.OutputPath = filepath.Join(t.TempDir(), "after-timeout.geojson")
	if result, err := manager.Route(context.Background(), "map-a.zmap", request); err != nil || result.ExitCode != 0 {
		t.Fatalf("worker did not restart after timeout: %#v, %v", result, err)
	}
}

func TestRouteWorkerManagerEvictsIdleMap(t *testing.T) {
	manager := NewRouteWorkerManager(createFakeRouteWorker(t), time.Second, 1)
	defer manager.Close()
	request := RouteWorkerRequest{
		FromLon: 1, FromLat: 2, ToLon: 3, ToLat: 4,
		Algorithm: "astar", MaxDistance: 100,
	}
	request.OutputPath = filepath.Join(t.TempDir(), "a.geojson")
	if _, err := manager.Route(context.Background(), "map-a.zmap", request); err != nil {
		t.Fatal(err)
	}
	request.OutputPath = filepath.Join(t.TempDir(), "b.geojson")
	if _, err := manager.Route(context.Background(), "map-b.zmap", request); err != nil {
		t.Fatal(err)
	}
	manager.mu.Lock()
	_, hasA := manager.workers["map-a.zmap"]
	_, hasB := manager.workers["map-b.zmap"]
	manager.mu.Unlock()
	if hasA || !hasB {
		t.Fatalf("unexpected resident worker set: map-a=%v map-b=%v", hasA, hasB)
	}
}

func TestRouteWorkerManagerSerializesConcurrentFrames(t *testing.T) {
	manager := NewRouteWorkerManager(createFakeRouteWorker(t), 2*time.Second, 1)
	defer manager.Close()
	const count = 24
	outputDir := t.TempDir()
	errorsSeen := make(chan error, count)
	var wait sync.WaitGroup
	for i := 0; i < count; i++ {
		wait.Add(1)
		go func(index int) {
			defer wait.Done()
			result, err := manager.Route(context.Background(), "shared.zmap", RouteWorkerRequest{
				FromLon: float64(index), FromLat: 2, ToLon: 3, ToLat: 4,
				Algorithm: "astar", MaxDistance: 100,
				OutputPath: filepath.Join(outputDir, "concurrent-"+strconv.Itoa(index)+".geojson"),
			})
			if err != nil {
				errorsSeen <- err
				return
			}
			if result.ExitCode != 0 || !strings.Contains(result.Output, "route=ok") {
				errorsSeen <- errors.New("corrupt concurrent route response")
			}
		}(i)
	}
	wait.Wait()
	close(errorsSeen)
	for err := range errorsSeen {
		t.Fatal(err)
	}
}

func TestRouteHandlerUsesPersistentWorker(t *testing.T) {
	dataDir := t.TempDir()
	mapID := "map_worker_test"
	mapDir := filepath.Join(dataDir, "maps", mapID)
	if err := os.MkdirAll(mapDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := saveJSONFile(filepath.Join(mapDir, "record.json"), MapRecord{ID: mapID}); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(mapDir, "map.zmap"), []byte("fixture"), 0o644); err != nil {
		t.Fatal(err)
	}
	server := NewServer(Config{
		DataDir: dataDir, WebDir: t.TempDir(), ZeusMap: createFakeRouteWorker(t),
		CmdLimit: time.Second, RouteWorkerMaps: 2,
	}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	defer server.Close()
	if err := server.ensureDirectories(); err != nil {
		t.Fatal(err)
	}

	call := func(algorithm string) *httptest.ResponseRecorder {
		payload, err := json.Marshal(RouteRequest{
			FromLon: floatPointer(1), FromLat: floatPointer(2),
			ToLon: floatPointer(3), ToLat: floatPointer(4), Algorithm: algorithm,
		})
		if err != nil {
			t.Fatal(err)
		}
		request := httptest.NewRequest(
			http.MethodPost, "/api/maps/"+mapID+"/route", bytes.NewReader(payload))
		request.Header.Set("Content-Type", "application/json")
		response := httptest.NewRecorder()
		server.routes().ServeHTTP(response, request)
		return response
	}

	success := call("astar")
	if success.Code != http.StatusOK || !strings.Contains(success.Body.String(), `"ok":true`) ||
		!strings.Contains(success.Body.String(), `"geojson":{"type":"FeatureCollection"`) {
		t.Fatalf("unexpected successful route response %d: %s", success.Code, success.Body.String())
	}
	failure := call("dijkstra")
	if failure.Code != http.StatusOK || !strings.Contains(failure.Body.String(), `"ok":false`) ||
		!strings.Contains(failure.Body.String(), `"reason":"unreachable"`) {
		t.Fatalf("unexpected route computation failure %d: %s", failure.Code, failure.Body.String())
	}
}

func floatPointer(value float64) *float64 { return &value }
