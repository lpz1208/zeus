package main

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"
)

func TestAlgorithmVehiclePlan(t *testing.T) {
	s, logPath := newAgentSessionTestServer(t)
	defer s.Close()
	status, created := agentSessionRequest(t, s, http.MethodPost, "/api/maps/m1/agent/sessions",
		`{"vehicles":[{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4,"agent":true}]}`)
	if status != 200 {
		t.Fatal(status, created)
	}
	session := created["sessionId"].(string)
	url := "/api/maps/m1/agent/sessions/" + session + "/algorithms/plan"
	body, _ := json.Marshal(algorithmVehicleRequest{VehicleID: 0, BasedOnStateVersion: 1, Source: algorithmTemplate})
	status, value := agentSessionRequest(t, s, http.MethodPost, url, string(body))
	if status != 200 {
		t.Fatal(status, value)
	}
	result, ok := value["result"].(map[string]any)
	if !ok || result["phase"] != "verified" || result["executionMode"] != "vehicle" || result["timeS"] != float64(5) {
		t.Fatal("missing verified vehicle result", value)
	}
	candidate, ok := value["candidate"].(map[string]any)
	if !ok || candidate["candidateId"] != "cand-custom" {
		t.Fatal(value)
	}
	log := fakeSessionLog(t, logPath)
	if !strings.Contains(log, "algorithm-candidate\t"+session+"\t0\t1\t-1,0,-2") || strings.Contains(log, "commit\t") {
		t.Fatal("planning must register exact states without committing", log)
	}
	for _, bad := range []string{
		`{"vehicleId":-1,"basedOnStateVersion":1,"source":"x"}`,
		`{"vehicleId":0,"source":"x"}`,
		`{"vehicleId":0,"basedOnStateVersion":1,"source":"x","fromLon":1}`,
		string(body) + ` {}`,
	} {
		if code, _ := agentSessionRequest(t, s, http.MethodPost, url, bad); code != 400 {
			t.Fatal("invalid request accepted", code, bad)
		}
	}
	status, _ = agentSessionRequest(t, s, http.MethodPost, strings.Replace(url, "/m1/", "/other/", 1), string(body))
	if status != 404 {
		t.Fatal("cross-map access accepted")
	}
}

func TestAlgorithmVehicleExactPathReplay(t *testing.T) {
	s, logPath := newAgentSessionTestServer(t)
	defer s.Close()
	artifact := agentSnapshotArtifact{
		FormatVersion: 2,
		MapID:         "m1", Tick: 0, StateVersion: 1,
		Request: AgentSessionRequest{Vehicles: []AgentVehicleSpec{{FromLon: 1, FromLat: 2, ToLon: 3, ToLat: 4, Agent: true}}},
		AppliedActions: []agentReplayAction{{Tick: 0, VehicleID: 0, Kind: "commit_route", Algorithm: "dijkstra",
			Path: &agentExactPath{Edges: []int{0, 3, 4, 1}, StartOffsetM: 12.123456789, EndOffsetM: 23.987654321}}},
	}
	_, _, err := s.replayAgentSnapshot(context.Background(), artifact, "exact-replay")
	if err != nil {
		t.Fatal(err)
	}
	log := fakeSessionLog(t, logPath)
	if !strings.Contains(log, "path-candidate\texact-replay\t0\t1\t12.123456789\t23.987654321\t0,3,4,1") ||
		!strings.Contains(log, "commit\texact-replay\t0\tcand-custom\t1") || strings.Contains(log, "\nplan\t") {
		t.Fatal("replay lost exact path or precision", log)
	}
}

func TestAlgorithmVehicleObservationAndKeep(t *testing.T) {
	s, logPath := newAgentSessionTestServer(t)
	defer s.Close()
	_, created := agentSessionRequest(t, s, http.MethodPost, "/api/maps/m1/agent/sessions",
		`{"vehicles":[{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4,"agent":true}]}`)
	base := "/api/maps/m1/agent/sessions/" + created["sessionId"].(string)
	body, _ := json.Marshal(algorithmVehicleRequest{BasedOnStateVersion: 1, Source: "def route(ctx):\n    ctx.log(ctx.observation()[\"stateVersion\"])\n    return ctx.keep()"})
	status, value := agentSessionRequest(t, s, http.MethodPost, base+"/algorithms/plan", string(body))
	if status != 200 {
		t.Fatal(status, value)
	}
	result := value["result"].(map[string]any)
	if result["phase"] != "kept" || result["keepCurrentRoute"] != true || value["candidate"] != nil || result["observation"] == nil {
		t.Fatal("keep must be distinct from unreachable", value)
	}
	log := fakeSessionLog(t, logPath)
	if strings.Contains(log, "algorithm-candidate\t") || strings.Contains(log, "commit\t") {
		t.Fatal("keep planned a replacement", log)
	}
}

func TestAlgorithmNavigationDecisionAndStepGuards(t *testing.T) {
	s, _ := newAgentSessionTestServer(t)
	defer s.Close()
	_, created := agentSessionRequest(t, s, http.MethodPost, "/api/maps/m1/agent/sessions",
		`{"vehicles":[{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4,"agent":true}]}`)
	id := created["sessionId"].(string)
	base := "/api/maps/m1/agent/sessions/" + id
	entry, _ := s.agentSessions.get(id)
	entry.commandMu.Lock()
	status, _ := agentSessionRequest(t, s, http.MethodPost, base+"/step", `{"ticks":1}`)
	entry.commandMu.Unlock()
	if status != 409 {
		t.Fatal("concurrent mutation accepted")
	}
	status, opened := agentSessionRequest(t, s, http.MethodPost, base+"/decisions", `{"vehicleId":0,"basedOnStateVersion":1}`)
	if status != 200 || opened["decisionId"] == "" {
		t.Fatal(status, opened)
	}
	_, observed := agentSessionRequest(t, s, http.MethodGet, base, "")
	if observed["decisionId"] != opened["decisionId"] {
		t.Fatal("observation omitted current barrier", observed)
	}
	status, _ = agentSessionRequest(t, s, http.MethodPost, base+"/step", `{"ticks":1,"basedOnStateVersion":1}`)
	if status != 409 {
		t.Fatal("stepped through pending decision")
	}
	body, _ := json.Marshal(map[string]any{"decisionId": opened["decisionId"], "vehicleId": 0, "kind": "keep_route", "basedOnStateVersion": 1})
	status, value := agentSessionRequest(t, s, http.MethodPost, base+"/actions", string(body))
	if status != 200 {
		t.Fatal(status, value)
	}
	status, _ = agentSessionRequest(t, s, http.MethodPost, base+"/step", `{"ticks":1,"basedOnStateVersion":1}`)
	if status != 200 {
		t.Fatal("current version step rejected", status)
	}
	status, _ = agentSessionRequest(t, s, http.MethodPost, base+"/step", `{"ticks":1,"basedOnStateVersion":1}`)
	if status != 409 {
		t.Fatal("duplicate old step advanced again")
	}
}

func TestAlgorithmNavigationResidentCommandCancellation(t *testing.T) {
	deadline := time.Now().Add(time.Minute)
	parent, disconnect := context.WithDeadline(context.Background(), deadline)
	command, finish := residentAgentCommandContext(parent)
	defer finish()
	disconnect()
	if command.Err() != nil {
		t.Fatal("browser disconnect cancelled the resident command")
	}
	if actual, ok := command.Deadline(); !ok || !actual.Equal(deadline) {
		t.Fatal("server deadline was dropped")
	}
	alreadyCancelled, stop := residentAgentCommandContext(parent)
	defer stop()
	if alreadyCancelled.Err() == nil {
		t.Fatal("a new command started after disconnection")
	}
}
