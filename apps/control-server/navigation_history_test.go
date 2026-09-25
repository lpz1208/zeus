package main

import (
	"encoding/json"
	"fmt"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func navigationFixture(t *testing.T) (*Server, string, string, map[string]any) {
	t.Helper()
	s, log := newAgentSessionTestServer(t)
	t.Cleanup(s.Close)
	status, created := agentSessionRequest(t, s, "POST", "/api/maps/m1/agent/sessions",
		`{"vehicles":[{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4,"agent":true}]}`)
	if status != 200 {
		t.Fatal(status, created)
	}
	id := created["sessionId"].(string)
	request := map[string]any{"revision": 0, "basedOnStateVersion": 1, "source": algorithmTemplate,
		"records": []any{map[string]any{"id": "decision-1", "status": "kept", "mapId": "m1", "sessionId": id,
			"stateVersion": 1, "simulationTimeS": 0, "source": algorithmTemplate}}}
	return s, "/api/maps/m1/agent/sessions/" + id + "/navigation-history", log, request
}

func navigationSave(t *testing.T, s *Server, path string, request map[string]any) (int, map[string]any) {
	t.Helper()
	data, err := json.Marshal(request)
	if err != nil {
		t.Fatal(err)
	}
	return agentSessionRequest(t, s, "POST", path, string(data))
}

func TestNavigationHistoryRestartAndRestore(t *testing.T) {
	s, path, log, request := navigationFixture(t)
	status, saved := navigationSave(t, s, path, request)
	if status != 200 || saved["revision"] != float64(1) {
		t.Fatal(status, saved)
	}
	id := saved["id"].(string)
	if !strings.Contains(fakeSessionLog(t, log), "drop-snapshot\t") {
		t.Fatal("temporary native snapshot leaked")
	}
	s.Close()
	restarted := newAgentSessionServer(t, s.config.DataDir, s.config.ZeusMap)
	defer restarted.Close()
	base := "/api/maps/m1/agent/navigation-history/" + id
	status, history := agentSessionRequest(t, restarted, "GET", base, "")
	if status != 200 || history["source"] != algorithmTemplate || len(history["records"].([]any)) != 1 {
		t.Fatal(status, history)
	}
	w := httptest.NewRecorder()
	restarted.routes().ServeHTTP(w, httptest.NewRequest("GET", "/api/maps/m1/agent/navigation-history", nil))
	if w.Code != 200 || !strings.Contains(w.Body.String(), id) {
		t.Fatal(w.Body.String())
	}
	status, restored := agentSessionRequest(t, restarted, "POST", base+"/restore", `{"revision":1}`)
	if status != 200 || restored["state"].(map[string]any)["sessionId"] == saved["sessionId"] {
		t.Fatal(status, restored)
	}
	// Missing revision or a stale view must not restore a different boundary.
	for body, expected := range map[string]int{`{}`: 400, `{"revision":2}`: 409} {
		if status, _ := agentSessionRequest(t, restarted, "POST", base+"/restore", body); status != expected {
			t.Fatal(status, expected)
		}
	}
	if status, _ := agentSessionRequest(t, restarted, "GET", strings.Replace(base, "/m1/", "/other/", 1), ""); status != 404 {
		t.Fatal(status)
	}
	if status, _ := agentSessionRequest(t, restarted, "DELETE", base, ""); status != 200 {
		t.Fatal(status)
	}
	if status, _ := agentSessionRequest(t, restarted, "POST", base+"/restore", `{"revision":1}`); status != 404 {
		t.Fatal(status)
	}
}

func TestNavigationHistoryRejectsStaleWritesAndBounds(t *testing.T) {
	s, path, _, request := navigationFixture(t)
	status, saved := navigationSave(t, s, path, request)
	if status != 200 {
		t.Fatal(status, saved)
	}
	request["id"] = saved["id"]
	if status, _ := navigationSave(t, s, path, request); status != 409 {
		t.Fatal("stale revision accepted", status)
	}
	request["revision"] = 1
	request["basedOnStateVersion"] = 999
	if status, _ := navigationSave(t, s, path, request); status != 409 {
		t.Fatal("stale native version accepted", status)
	}
	request["basedOnStateVersion"] = 1
	status, saved = navigationSave(t, s, path, request)
	if status != 200 || saved["revision"] != float64(2) {
		t.Fatal(status, saved)
	}
	request["revision"] = 2
	for _, mutate := range []func(map[string]any){
		func(r map[string]any) { r["id"] = "nav_../../secret" },
		func(r map[string]any) { r["source"] = strings.Repeat("x", 32769) },
		func(r map[string]any) { r["records"] = make([]any, 201) },
		func(r map[string]any) {
			r["records"] = []any{map[string]any{"id": "foreign", "status": "applied", "sessionId": "other", "mapId": "m1"}}
		},
		func(r map[string]any) {
			r["records"] = []any{map[string]any{"id": "bad-types", "status": "applied", "sessionId": saved["sessionId"], "mapId": "m1", "stateVersion": 1, "computeMs": "bad"}}
		},
	} {
		copy := make(map[string]any)
		for key, value := range request {
			copy[key] = value
		}
		mutate(copy)
		if status, _ := navigationSave(t, s, path, copy); status != 400 {
			t.Fatal("invalid history accepted", status)
		}
	}
	stored, err := s.readNavigationHistory("m1", saved["id"].(string))
	if err != nil || stored.Revision != 2 {
		t.Fatal("rejected writes changed checkpoint", err, stored.Revision)
	}
}

func TestNavigationHistoryConcurrentRevision(t *testing.T) {
	s, path, _, request := navigationFixture(t)
	status, saved := navigationSave(t, s, path, request)
	if status != 200 {
		t.Fatal(status, saved)
	}
	request["id"], request["revision"] = saved["id"], 1
	data, _ := json.Marshal(request)
	statuses := make(chan int, 2)
	var group sync.WaitGroup
	for i := 0; i < 2; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			w := httptest.NewRecorder()
			s.routes().ServeHTTP(w, httptest.NewRequest("POST", path, strings.NewReader(string(data))))
			statuses <- w.Code
		}()
	}
	group.Wait()
	close(statuses)
	counts := map[int]int{}
	for code := range statuses {
		counts[code]++
	}
	if counts[200] != 1 || counts[409] != 1 {
		t.Fatal(counts)
	}
}

func TestNavigationHistoryCapacityPreservesExistingBoundary(t *testing.T) {
	s, path, log, request := navigationFixture(t)
	status, saved := navigationSave(t, s, path, request)
	if status != 200 {
		t.Fatal(status, saved)
	}
	for i := 0; i < navigationHistoryLimit-1; i++ {
		if err := os.WriteFile(filepath.Join(s.navigationHistoryDir("m1"), fmt.Sprintf("nav_%016x.json", i)), []byte("{}"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	before := fakeSessionLog(t, log)
	if status, _ := navigationSave(t, s, path, request); status != 409 {
		t.Fatal("capacity ignored", status)
	}
	if before != fakeSessionLog(t, log) {
		t.Fatal("full history still contacted native worker")
	}
	request["id"], request["revision"] = saved["id"], 1
	if status, _ := navigationSave(t, s, path, request); status != 200 {
		t.Fatal("existing history cannot advance at capacity", status)
	}
}
