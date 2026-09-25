package main

import (
	"encoding/json"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestAlgorithmHistorySurvivesServerRestart(t *testing.T) {
	s := newSimulationTestServer(t, "#!/bin/sh\nexit 0\n", 1)
	defer s.Close()
	lon, lat := 114.0, 30.0
	req := AlgorithmLabRequest{FromLon: &lon, ToLon: &lon, FromLat: &lat, ToLat: &lat, Source: algorithmTemplate, Steps: 1000}
	result := AlgorithmLabResult{OK: true, Phase: "verified", MapID: "map_sim_test", CodeRevision: "rev", SnapshotID: "snapshot", TimeS: 5}
	w := httptest.NewRecorder()
	s.finishAlgorithmRun(w, httptest.NewRequest("POST", "/", nil), req, result)
	if json.Unmarshal(w.Body.Bytes(), &result) != nil || result.RunID == "" || result.StorageError != "" {
		t.Fatal(w.Body.String())
	}
	restarted := NewServer(s.config, s.logger)
	defer restarted.Close()
	base := "/api/maps/map_sim_test/algorithms/experiments"
	for _, path := range []string{base, base + "/" + result.RunID} {
		response := httptest.NewRecorder()
		restarted.routes().ServeHTTP(response, httptest.NewRequest("GET", path, nil))
		if response.Code != 200 || !strings.Contains(response.Body.String(), result.RunID) {
			t.Fatal(response.Code, response.Body.String())
		}
		if path != base && !strings.Contains(response.Body.String(), "def route(ctx)") {
			t.Fatal("source not restored")
		}
	}
	response := httptest.NewRecorder()
	restarted.routes().ServeHTTP(response, httptest.NewRequest("DELETE", base+"/"+result.RunID, nil))
	if response.Code != 200 {
		t.Fatal(response.Body.String())
	}
	response = httptest.NewRecorder()
	restarted.routes().ServeHTTP(response, httptest.NewRequest("GET", base+"/"+result.RunID, nil))
	if response.Code != 404 {
		t.Fatal("deleted experiment remains")
	}
}

func TestLivenessDoesNotCheckBenchmark(t *testing.T) {
	s := newSimulationTestServer(t, "#!/bin/sh\nexit 0\n", 1)
	defer s.Close()
	s.benchmarkHealth = nil
	w := httptest.NewRecorder()
	s.routes().ServeHTTP(w, httptest.NewRequest("GET", "/api/live", nil))
	if w.Code != 200 || !strings.Contains(w.Body.String(), `"service":"zeus-control-server"`) {
		t.Fatal(w.Body.String())
	}
}

func TestAlgorithmExperimentIDs(t *testing.T) {
	for _, id := range []string{"../record", "run_../../record.json", "run_000000000000000g", "run_0", ""} {
		if validAlgorithmRunID(id) {
			t.Fatalf("accepted unsafe id %q", id)
		}
	}
	if !validAlgorithmRunID("run_1234567890abcdef") {
		t.Fatal("valid id rejected")
	}
}
