package main

import (
	"encoding/json"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

const debugFixtureCommand = `#!/bin/sh
while [ "$#" -gt 0 ]; do
  if [ "$1" = "--output" ]; then target=$2; shift; fi
  shift
done
cat > "$target" <<'JSON'
{"adjacency":{"-1":[]},"nodes":[],"estimates":[],"baseline":{"ok":false,"timeS":0,"lengthM":0}}
JSON
`

func startDebugFixture(t *testing.T, s *Server) (algorithmDebugEvent, *algorithmDebugSession) {
	t.Helper()
	payload, _ := json.Marshal(map[string]any{"fromLon": 114, "fromLat": 30, "toLon": 114, "toLat": 30,
		"source": "def route(ctx):\n    n = 1\n    n = n + 2\n    return None"})
	w := httptest.NewRecorder()
	s.routes().ServeHTTP(w, httptest.NewRequest("POST", "/api/maps/map_sim_test/algorithms/debug", strings.NewReader(string(payload))))
	var event algorithmDebugEvent
	if w.Code != 200 || json.Unmarshal(w.Body.Bytes(), &event) != nil || event.Kind != "paused" || event.Line != 2 {
		t.Fatalf("%d %s", w.Code, w.Body.String())
	}
	s.algorithmDebugMu.Lock()
	d := s.algorithmDebugSessions[event.SessionID]
	s.algorithmDebugMu.Unlock()
	if d == nil {
		t.Fatal("debugger not registered")
	}
	return event, d
}

func debugCommand(t *testing.T, s *Server, id, command string) algorithmDebugEvent {
	t.Helper()
	var payload map[string]any
	if json.Unmarshal([]byte(command), &payload) != nil {
		t.Fatal("invalid test command")
	}
	s.algorithmDebugMu.Lock()
	d := s.algorithmDebugSessions[id]
	s.algorithmDebugMu.Unlock()
	if d == nil {
		t.Fatal("missing debugger")
	}
	d.mu.Lock()
	payload["sequence"] = d.sequence
	d.mu.Unlock()
	encoded, _ := json.Marshal(payload)
	w := httptest.NewRecorder()
	s.routes().ServeHTTP(w, httptest.NewRequest("POST", "/api/maps/map_sim_test/algorithms/debug/"+id, strings.NewReader(string(encoded))))
	var event algorithmDebugEvent
	if w.Code != 200 || json.Unmarshal(w.Body.Bytes(), &event) != nil {
		t.Fatalf("%d %s", w.Code, w.Body.String())
	}
	return event
}

func waitDebugCleanup(t *testing.T, d *algorithmDebugSession) {
	t.Helper()
	select {
	case <-d.cleaned:
	case <-time.After(2 * time.Second):
		t.Fatal("debug process/slot was not released")
	}
}

func TestAlgorithmDebugStepBreakpointAndVerifiedCompletion(t *testing.T) {
	s := newSimulationTestServer(t, debugFixtureCommand, 1)
	defer s.Close()
	first, d := startDebugFixture(t, s)
	next := debugCommand(t, s, first.SessionID, `{"action":"step"}`)
	if next.Line != 3 || next.Variables["n"] != "1" {
		t.Fatalf("wrong step: %+v", next)
	}
	repeated := httptest.NewRecorder()
	s.routes().ServeHTTP(repeated, httptest.NewRequest("POST", "/api/maps/map_sim_test/algorithms/debug/"+first.SessionID, strings.NewReader(`{"action":"step","sequence":1}`)))
	if repeated.Code != 409 {
		t.Fatal("a repeated step changed the current execution")
	}
	next = debugCommand(t, s, first.SessionID, `{"action":"continue","breakpoints":[4]}`)
	if next.Line != 4 || next.Variables["n"] != "3" {
		t.Fatalf("wrong breakpoint: %+v", next)
	}
	next = debugCommand(t, s, first.SessionID, `{"action":"continue"}`)
	var result AlgorithmLabResult
	if next.Kind != "finished" || json.Unmarshal(next.Result, &result) != nil || result.Phase != "unreachable" || result.RunID == "" {
		t.Fatalf("%+v %s", next, next.Result)
	}
	waitDebugCleanup(t, d)
	if len(s.algorithmSlots) != 0 {
		t.Fatal("slot leaked")
	}
}

func TestAlgorithmDebugStopExpiryAndServerShutdown(t *testing.T) {
	for _, mode := range []string{"stop", "expire", "shutdown"} {
		t.Run(mode, func(t *testing.T) {
			s := newSimulationTestServer(t, debugFixtureCommand, 1)
			defer s.Close()
			first, d := startDebugFixture(t, s)
			switch mode {
			case "stop":
				w := httptest.NewRecorder()
				s.routes().ServeHTTP(w, httptest.NewRequest("DELETE", "/api/maps/map_sim_test/algorithms/debug/"+first.SessionID, nil))
				if w.Code != 200 {
					t.Fatal(w.Body.String())
				}
			case "expire":
				d.idle.Reset(time.Millisecond)
			case "shutdown":
				s.Close()
			}
			waitDebugCleanup(t, d)
			if len(s.algorithmSlots) != 0 {
				t.Fatal("debug slot remains reserved")
			}
		})
	}
}

func TestAlgorithmDebugMapBindingAndConcurrencyLimit(t *testing.T) {
	s := newSimulationTestServer(t, debugFixtureCommand, 1)
	defer s.Close()
	first, _ := startDebugFixture(t, s)
	startDebugFixture(t, s)
	w := httptest.NewRecorder()
	s.routes().ServeHTTP(w, httptest.NewRequest("DELETE", "/api/maps/other/algorithms/debug/"+first.SessionID, nil))
	if w.Code != 404 {
		t.Fatal("cross-map debugger access accepted")
	}
	w = httptest.NewRecorder()
	s.routes().ServeHTTP(w, httptest.NewRequest("POST", "/api/algorithms/check", strings.NewReader(`{"source":"def route(ctx):\n    return None"}`)))
	if w.Code != 429 {
		t.Fatal("paused debuggers did not reserve execution slots")
	}
	w = httptest.NewRecorder()
	s.routes().ServeHTTP(w, httptest.NewRequest("POST", "/api/maps/map_sim_test/algorithms/debug/"+first.SessionID, strings.NewReader(`{"action":"continue","breakpoints":[999]}`)))
	if w.Code != 400 {
		t.Fatal("invalid breakpoint accepted")
	}
	next := debugCommand(t, s, first.SessionID, `{"action":"step"}`)
	if next.Line != 3 {
		t.Fatal("invalid command changed paused execution")
	}
}
