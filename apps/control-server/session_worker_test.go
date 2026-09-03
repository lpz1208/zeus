package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// createFakeSessionWorker returns a shell script speaking the session-worker
// protocol. Commands are echoed to a log file so tests can assert which
// worker commands the server issued.
func createFakeSessionWorker(t *testing.T) (string, string) {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "fake-session-worker")
	logPath := filepath.Join(dir, "commands.log")
	script := `#!/bin/sh
if [ "$1" != "session-worker" ]; then
  exit 2
fi
log="` + logPath + `"
printf 'ZEUS_SESSION_WORKER\t1\n'
tab=$(printf '\t')
state_version=1
tick=0
while IFS="$tab" read -r command session rest; do
  printf '%s\t%s\t%s\n' "$command" "$session" "$rest" >> "$log"
  case "$command" in
    shutdown) exit 0 ;;
    tools)
      payload='{"registryVersion":"routing-tools-v1","algorithms":[{"algorithmId":"dijkstra","algorithmVersion":"1","supportedObjectives":["travel_time"],"supportsDynamicWeights":true,"supportsIncrementalRepair":false,"supportsKCandidates":false,"supportsTimeDependency":false,"deterministic":true,"exact":true,"usesHeuristic":false},{"algorithmId":"astar","algorithmVersion":"1","supportedObjectives":["travel_time"],"supportsDynamicWeights":true,"supportsIncrementalRepair":false,"supportsKCandidates":false,"supportsTimeDependency":false,"deterministic":true,"exact":true,"usesHeuristic":true}]}'
      exit_code=0 ;;
    reset)
      state_version=1
      payload=$(printf '{"sessionId":"%s","tick":0,"simulationTimeS":0.0,"stateVersion":1,"ready":true,"paused":true,"finished":false,"vehicles":2,"agents":[0]}' "$session")
      exit_code=0 ;;
    step|step_event|run-to-end|resume|pause)
      tick=$((tick + 1))
      state_version=$((state_version + 1))
      due=false
      reason=""
      if [ "$command" = "step_event" ]; then
        due=true
        reason="route_invalidated"
      fi
      payload=$(printf '{"tick":1,"simulationTimeS":1.0,"stateVersion":%s,"finished":false,"cancelled":false,"decisionDue":%s,"decisionReason":"%s","agentVehicleIds":[0]}' "$state_version" "$due" "$reason")
      exit_code=0 ;;
    observe)
      payload=$(printf '{"tick":1,"simulationTimeS":1.0,"stateVersion":%s,"finished":false,"cancelled":false,"decisionDue":true,"decisionReason":"route_invalidated","counts":{"arrived":0,"driving":2,"waiting":0,"unroutable":0},"edges":[],"agents":[]}' "$state_version")
      exit_code=0 ;;
    agent-observe)
      payload=$(printf '{"tick":1,"simulationTimeS":1.0,"stateVersion":%s,"vehicleId":0,"state":"driving","position":{"edgeId":4,"offsetM":12.5},"destinationEdgeId":9,"remainingEtaS":42.0,"routeInvalidated":true,"remainingEdgeIds":[4,5],"nearbyRoads":[],"activeEvents":[]}' "$state_version")
      exit_code=0 ;;
    plan)
      payload=$(printf '{"candidateId":"cand-1","vehicleId":0,"algorithm":"astar","effectiveAlgorithm":"astar","basedOnStateVersion":%s,"ok":true,"timeS":42.0,"lengthM":500.0,"expandedNodes":3,"edges":[4,5,9]}' "$state_version")
      exit_code=0 ;;
    commit|keep)
      case "$rest" in
        *reject*) payload='{"accepted":false,"reason":"unknown candidate","appliesAtNextTick":false}' ;;
        *) payload='{"accepted":true,"reason":"","appliesAtNextTick":true}' ;;
      esac
      exit_code=0 ;;
    snapshot)
      payload=$(printf '{"snapshotId":"%s","sourceSessionId":"%s","tick":%s,"simulationTimeS":1.0,"stateVersion":%s,"actionCount":1,"storage":"process_local_replay"}' "$rest" "$session" "$tick" "$state_version")
      exit_code=0 ;;
    restore)
      payload=$(printf '{"sessionId":"%s","snapshotId":"%s","tick":%s,"simulationTimeS":1.0,"stateVersion":%s,"finished":false,"cancelled":false,"decisionDue":false,"decisionReason":"","agentVehicleIds":[0],"restored":true}' "$rest" "$session" "$tick" "$state_version")
      exit_code=0 ;;
    drop-snapshot)
      payload=$(printf '{"deleted":true,"snapshotId":"%s"}' "$session")
      exit_code=0 ;;
    result)
      traj=$(printf '%s' "$rest" | cut -f1)
      play=$(printf '%s' "$rest" | cut -f2)
      printf '{"type":"FeatureCollection","features":[]}' > "$traj"
      printf '{"duration_s":1}' > "$play"
      payload=$(printf '{"ok":true,"arrived":2,"vehicles":2,"avgTravelS":50.0,"ticks":100,"trajectory":"%s","playback":"%s"}' "$traj" "$play")
      exit_code=0 ;;
    close)
      payload='{}'
      exit_code=0 ;;
    fail)
      payload='{"error":"unknown session: missing"}'
      exit_code=1 ;;
    *)
      payload='{"error":"unsupported command"}'
      exit_code=1 ;;
  esac
  size=${#payload}
  printf 'ZEUS_SESSION_RESPONSE\t%s\t%s\n%s\n' "$exit_code" "$size" "$payload"
done
`
	if err := os.WriteFile(path, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	return path, logPath
}

func fakeSessionLog(t *testing.T, path string) string {
	t.Helper()
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(content)
}

func TestSessionWorkerManagerReusesProcess(t *testing.T) {
	executable, logPath := createFakeSessionWorker(t)
	manager := NewSessionWorkerManager(executable, time.Second, 2)
	defer manager.Close()
	first, err := manager.Command(context.Background(), "map-a.zmap", "observe", "s1", "hot")
	if err != nil || first.ExitCode != 0 {
		t.Fatalf("first command failed: exit=%d err=%v payload=%s",
			first.ExitCode, err, first.Payload)
	}
	second, err := manager.Command(context.Background(), "map-a.zmap", "plan", "s1", "0", "astar")
	if err != nil || second.ExitCode != 0 {
		t.Fatalf("second command failed: exit=%d err=%v", second.ExitCode, err)
	}
	var plan map[string]any
	if err := json.Unmarshal(second.Payload, &plan); err != nil {
		t.Fatalf("plan payload is not JSON: %v", err)
	}
	if plan["candidateId"] != "cand-1" {
		t.Fatalf("unexpected plan payload: %s", second.Payload)
	}
	log := fakeSessionLog(t, logPath)
	if strings.Count(log, "observe\ts1") != 1 || strings.Count(log, "plan\ts1") != 1 {
		t.Fatalf("commands missing from worker log: %q", log)
	}
}

func TestSessionWorkerManagerEvictsIdleMap(t *testing.T) {
	executable, _ := createFakeSessionWorker(t)
	manager := NewSessionWorkerManager(executable, time.Second, 1)
	defer manager.Close()
	for _, mapPath := range []string{"map-a.zmap", "map-b.zmap"} {
		if _, err := manager.Command(context.Background(), mapPath, "observe", "s", "hot"); err != nil {
			t.Fatalf("command on %s failed: %v", mapPath, err)
		}
	}
}

func TestSessionWorkerManagerRestartsAfterHang(t *testing.T) {
	executable, _ := createFakeSessionWorker(t)
	manager := NewSessionWorkerManager(executable, time.Second, 2)
	defer manager.Close()
	if _, err := manager.Command(context.Background(), "map-a.zmap", "observe", "s1", "hot"); err != nil {
		t.Fatalf("initial command failed: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 40*time.Millisecond)
	defer cancel()
	if _, err := manager.Command(ctx, "map-a.zmap", "shutdown"); err == nil {
		t.Fatal("shutdown stops responding; the context should cancel the call")
	}
	if _, err := manager.Command(context.Background(), "map-a.zmap", "observe", "s1", "hot"); err != nil {
		t.Fatalf("worker did not restart after the hang: %v", err)
	}
}

func newAgentSessionTestServer(t *testing.T) (*Server, string) {
	t.Helper()
	executable, logPath := createFakeSessionWorker(t)
	dataDir := t.TempDir()
	return newAgentSessionServer(t, dataDir, executable), logPath
}

func newAgentSessionServer(t *testing.T, dataDir, executable string) *Server {
	t.Helper()
	if err := os.MkdirAll(filepath.Join(dataDir, "maps", "m1"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dataDir, "maps", "m1", "record.json"), []byte(`{
		"id": "m1", "name": "test", "source": "test", "status": "ready",
		"summary": {"nodes": 10, "directedEdges": 20}
	}`), 0o644); err != nil {
		t.Fatal(err)
	}
	server := NewServer(Config{
		DataDir: dataDir, WebDir: t.TempDir(), ZeusMap: executable,
		CmdLimit: 2 * time.Second, SessionWorkerMaps: 2,
	}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err := server.ensureDirectories(); err != nil {
		server.Close()
		t.Fatal(err)
	}
	return server
}

func agentSessionRequest(
	t *testing.T,
	server *Server,
	method string,
	path string,
	body string,
) (int, map[string]any) {
	t.Helper()
	var reader io.Reader
	if body != "" {
		reader = strings.NewReader(body)
	}
	request := httptest.NewRequest(method, path, reader)
	if body != "" {
		request.Header.Set("Content-Type", "application/json")
	}
	recorder := httptest.NewRecorder()
	server.routes().ServeHTTP(recorder, request)
	var payload map[string]any
	if recorder.Body.Len() > 0 {
		if err := json.Unmarshal(recorder.Body.Bytes(), &payload); err != nil {
			t.Fatalf("response is not JSON: %q", recorder.Body.String())
		}
	}
	return recorder.Code, payload
}

func TestAgentSessionEndpointsDriveDecisionLoop(t *testing.T) {
	server, logPath := newAgentSessionTestServer(t)
	defer server.Close()

	status, created := agentSessionRequest(
		t, server, http.MethodPost, "/api/maps/m1/agent/sessions",
		`{"vehicles":[
			{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4,"agent":true},
			{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4}],
		 "durationSeconds":900,"stepSeconds":1,"sampleIntervalSeconds":15}`)
	if status != http.StatusOK {
		t.Fatalf("create session failed: %d %v", status, created)
	}
	sessionID, _ := created["sessionId"].(string)
	if sessionID == "" {
		t.Fatalf("create response missing sessionId: %v", created)
	}
	base := "/api/maps/m1/agent/sessions/" + sessionID

	status, stepped := agentSessionRequest(
		t, server, http.MethodPost, base+"/step",
		`{"untilEvent":true,"maxTicks":50}`)
	if status != http.StatusOK {
		t.Fatalf("step failed: %d %v", status, stepped)
	}
	decisionID, _ := stepped["decisionId"].(string)
	if decisionID == "" {
		t.Fatalf("decision boundary did not open a barrier: %v", stepped)
	}
	status, _ = agentSessionRequest(
		t, server, http.MethodPost, base+"/step", `{"ticks":1}`)
	if status != http.StatusConflict {
		t.Fatalf("step with an unresolved decision should conflict, got %d", status)
	}

	missingCandidate := fmt.Sprintf(
		`{"decisionId":%q,"agentId":"default","vehicleId":0,"kind":"commit_route",`+
			`"basedOnStateVersion":2}`, decisionID)
	status, _ = agentSessionRequest(
		t, server, http.MethodPost, base+"/actions", missingCandidate)
	if status != http.StatusBadRequest {
		t.Fatalf("missing candidate should fail before consuming the decision, got %d", status)
	}

	status, plan := agentSessionRequest(
		t, server, http.MethodPost, base+"/plan",
		`{"vehicleId":0,"algorithm":"astar"}`)
	if status != http.StatusOK || plan["candidateId"] != "cand-1" {
		t.Fatalf("plan failed: %d %v", status, plan)
	}
	version := uint64(2) // fake observe advances the version once
	rejected := fmt.Sprintf(
		`{"decisionId":%q,"agentId":"default","vehicleId":0,"kind":"commit_route",`+
			`"candidateId":"reject","basedOnStateVersion":%d}`,
		decisionID, version)
	status, _ = agentSessionRequest(
		t, server, http.MethodPost, base+"/actions", rejected)
	if status != http.StatusConflict {
		t.Fatalf("worker rejection should leave the decision retryable, got %d", status)
	}
	body := fmt.Sprintf(
		`{"decisionId":%q,"agentId":"default","vehicleId":0,"kind":"commit_route",`+
			`"candidateId":"cand-1","basedOnStateVersion":%d}`,
		decisionID, version)
	status, _ = agentSessionRequest(
		t, server, http.MethodPost, base+"/actions", body)
	if status != http.StatusOK {
		t.Fatalf("commit action failed: %d", status)
	}

	// A repeated submit for the same decision must conflict (already applied).
	status, _ = agentSessionRequest(
		t, server, http.MethodPost, base+"/actions", body)
	if status != http.StatusConflict {
		t.Fatalf("duplicate decision should conflict, got %d", status)
	}

	log := fakeSessionLog(t, logPath)
	for _, expected := range []string{
		"reset\t" + sessionID, "step_event\t" + sessionID,
		"plan\t" + sessionID + "\t0\tastar",
		"observe\t" + sessionID, "commit\t" + sessionID + "\t0\tcand-1",
	} {
		if !strings.Contains(log, expected) {
			t.Fatalf("worker log missing %q: %q", expected, log)
		}
	}

	status, _ = agentSessionRequest(
		t, server, http.MethodDelete, base, "")
	if status != http.StatusOK {
		t.Fatalf("close failed: %d", status)
	}
	if !strings.Contains(fakeSessionLog(t, logPath), "close\t"+sessionID) {
		t.Fatal("close never reached the worker")
	}
	status, _ = agentSessionRequest(
		t, server, http.MethodGet, base, "")
	if status != http.StatusNotFound {
		t.Fatalf("closed session should 404, got %d", status)
	}
}

func TestAgentToolRegistryEndpoint(t *testing.T) {
	server, _ := newAgentSessionTestServer(t)
	defer server.Close()
	status, registry := agentSessionRequest(
		t, server, http.MethodGet, "/api/maps/m1/agent/tools", "")
	if status != http.StatusOK || registry["registryVersion"] != "routing-tools-v1" {
		t.Fatalf("tool registry failed: %d %v", status, registry)
	}
	algorithms, _ := registry["algorithms"].([]any)
	if len(algorithms) != 2 {
		t.Fatalf("tool registry algorithms missing: %v", registry)
	}
	astar, _ := algorithms[1].(map[string]any)
	if astar["algorithmId"] != "astar" || astar["usesHeuristic"] != true {
		t.Fatalf("unexpected astar capability: %v", astar)
	}
}

func TestAgentDecisionTimeoutAppliesKeepFallback(t *testing.T) {
	server, logPath := newAgentSessionTestServer(t)
	server.decisionWallTTL = 20 * time.Millisecond
	defer server.Close()
	status, created := agentSessionRequest(
		t, server, http.MethodPost, "/api/maps/m1/agent/sessions",
		`{"vehicles":[{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4,"agent":true}],`+
			`"durationSeconds":900,"stepSeconds":1,"sampleIntervalSeconds":15}`)
	if status != http.StatusOK {
		t.Fatalf("create failed: %d %v", status, created)
	}
	sessionID, _ := created["sessionId"].(string)
	base := "/api/maps/m1/agent/sessions/" + sessionID
	status, stepped := agentSessionRequest(
		t, server, http.MethodPost, base+"/step",
		`{"untilEvent":true,"maxTicks":50}`)
	if status != http.StatusOK || stepped["decisionId"] == nil {
		t.Fatalf("step failed: %d %v", status, stepped)
	}

	deadline := time.Now().Add(2 * time.Second)
	expected := "keep\t" + sessionID + "\t0\t2"
	for !strings.Contains(fakeSessionLog(t, logPath), expected) {
		if time.Now().After(deadline) {
			t.Fatalf("timeout fallback never reached worker: %q", fakeSessionLog(t, logPath))
		}
		time.Sleep(5 * time.Millisecond)
	}
	status, _ = agentSessionRequest(
		t, server, http.MethodPost, base+"/step", `{"ticks":1}`)
	if status != http.StatusOK {
		t.Fatalf("fallback must release the session decision gate, got %d", status)
	}
}

func TestAgentRunUsesNonBlockingResumeCommand(t *testing.T) {
	server, logPath := newAgentSessionTestServer(t)
	defer server.Close()
	status, created := agentSessionRequest(
		t, server, http.MethodPost, "/api/maps/m1/agent/sessions",
		`{"vehicles":[{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4}],`+
			`"durationSeconds":900,"stepSeconds":1,"sampleIntervalSeconds":15}`)
	if status != http.StatusOK {
		t.Fatalf("create failed: %d %v", status, created)
	}
	sessionID, _ := created["sessionId"].(string)
	base := "/api/maps/m1/agent/sessions/" + sessionID
	status, _ = agentSessionRequest(t, server, http.MethodPost, base+"/run", "")
	if status != http.StatusOK {
		t.Fatalf("run failed: %d", status)
	}
	status, _ = agentSessionRequest(t, server, http.MethodPost, base+"/pause", "")
	if status != http.StatusOK {
		t.Fatalf("pause failed: %d", status)
	}
	log := fakeSessionLog(t, logPath)
	if !strings.Contains(log, "resume\t"+sessionID) ||
		!strings.Contains(log, "pause\t"+sessionID) {
		t.Fatalf("run/pause commands missing: %q", log)
	}
}

func TestAgentSessionSnapshotRestoreFork(t *testing.T) {
	server, logPath := newAgentSessionTestServer(t)
	defer server.Close()
	status, created := agentSessionRequest(
		t, server, http.MethodPost, "/api/maps/m1/agent/sessions",
		`{"vehicles":[{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4,"agent":true}],`+
			`"durationSeconds":900,"stepSeconds":1,"sampleIntervalSeconds":15}`)
	if status != http.StatusOK {
		t.Fatalf("create failed: %d %v", status, created)
	}
	sourceID, _ := created["sessionId"].(string)
	sourceBase := "/api/maps/m1/agent/sessions/" + sourceID
	status, snapshot := agentSessionRequest(
		t, server, http.MethodPost, sourceBase+"/snapshots", "")
	if status != http.StatusOK {
		t.Fatalf("snapshot failed: %d %v", status, snapshot)
	}
	snapshotID, _ := snapshot["snapshotId"].(string)
	if snapshotID == "" {
		t.Fatalf("snapshot response missing id: %v", snapshot)
	}
	if snapshot["storage"] != "durable_replay_v1" {
		t.Fatalf("snapshot was not persisted: %v", snapshot)
	}
	snapshotPath := filepath.Join(
		server.config.DataDir, "agent-snapshots", snapshotID+".json")
	if _, err := os.Stat(snapshotPath); err != nil {
		t.Fatalf("durable snapshot file missing: %v", err)
	}
	status, restored := agentSessionRequest(
		t, server, http.MethodPost,
		"/api/maps/m1/agent/snapshots/"+snapshotID+"/restore", "")
	if status != http.StatusOK {
		t.Fatalf("restore failed: %d %v", status, restored)
	}
	state, _ := restored["state"].(map[string]any)
	restoredID, _ := state["sessionId"].(string)
	if restoredID == "" || restoredID == sourceID {
		t.Fatalf("restore must create a new session: %v", restored)
	}
	status, _ = agentSessionRequest(
		t, server, http.MethodGet,
		"/api/maps/m1/agent/sessions/"+restoredID, "")
	if status != http.StatusOK {
		t.Fatalf("restored session was not registered: %d", status)
	}
	status, _ = agentSessionRequest(
		t, server, http.MethodDelete,
		"/api/maps/m1/agent/snapshots/"+snapshotID, "")
	if status != http.StatusOK {
		t.Fatalf("delete snapshot failed: %d", status)
	}
	if _, err := os.Stat(snapshotPath); !os.IsNotExist(err) {
		t.Fatalf("snapshot artifact still exists after delete: %v", err)
	}
	log := fakeSessionLog(t, logPath)
	for _, expected := range []string{
		"snapshot\t" + sourceID + "\t" + snapshotID,
		"reset\t" + restoredID,
		"drop-snapshot\t" + snapshotID,
	} {
		if !strings.Contains(log, expected) {
			t.Fatalf("snapshot command log missing %q: %q", expected, log)
		}
	}
}

func TestAgentSnapshotRestoresAfterServerAndWorkerRestart(t *testing.T) {
	executable, _ := createFakeSessionWorker(t)
	dataDir := t.TempDir()
	server := newAgentSessionServer(t, dataDir, executable)
	status, created := agentSessionRequest(
		t, server, http.MethodPost, "/api/maps/m1/agent/sessions",
		`{"vehicles":[{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4,"agent":true}],`+
			`"durationSeconds":900,"stepSeconds":1,"sampleIntervalSeconds":15}`)
	if status != http.StatusOK {
		t.Fatalf("create failed: %d %v", status, created)
	}
	sourceID, _ := created["sessionId"].(string)
	status, snapshot := agentSessionRequest(
		t, server, http.MethodPost,
		"/api/maps/m1/agent/sessions/"+sourceID+"/snapshots", "")
	if status != http.StatusOK {
		t.Fatalf("snapshot failed: %d %v", status, snapshot)
	}
	snapshotID, _ := snapshot["snapshotId"].(string)
	server.Close() // drops both the registry and the resident worker process

	restarted := newAgentSessionServer(t, dataDir, executable)
	defer restarted.Close()
	if _, ok := restarted.agentSessions.getSnapshot(snapshotID); ok {
		t.Fatal("restart test requires an initially empty in-memory registry")
	}
	status, restored := agentSessionRequest(
		t, restarted, http.MethodPost,
		"/api/maps/m1/agent/snapshots/"+snapshotID+"/restore", "")
	if status != http.StatusOK {
		t.Fatalf("durable restore failed after restart: %d %v", status, restored)
	}
	if restored["storage"] != "durable_replay_v1" {
		t.Fatalf("restore did not use durable replay: %v", restored)
	}
	state, _ := restored["state"].(map[string]any)
	if state["sessionId"] == "" || state["sessionId"] == sourceID {
		t.Fatalf("restart restore did not create a fresh session: %v", restored)
	}
}

func TestAgentSessionResultInlinesExports(t *testing.T) {
	server, _ := newAgentSessionTestServer(t)
	defer server.Close()
	status, created := agentSessionRequest(
		t, server, http.MethodPost, "/api/maps/m1/agent/sessions",
		`{"vehicles":[{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4}],
		 "durationSeconds":900,"stepSeconds":1,"sampleIntervalSeconds":15}`)
	if status != http.StatusOK {
		t.Fatalf("create failed: %d %v", status, created)
	}
	sessionID, _ := created["sessionId"].(string)
	status, result := agentSessionRequest(
		t, server, http.MethodGet,
		"/api/maps/m1/agent/sessions/"+sessionID+"/result", "")
	if status != http.StatusOK {
		t.Fatalf("result failed: %d %v", status, result)
	}
	if result["geojson"] == nil || result["playback"] == nil {
		t.Fatalf("result missing inline exports: %v", result)
	}
}
