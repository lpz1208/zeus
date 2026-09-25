package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func waitNavigationJobs(t *testing.T, s *Server) {
	t.Helper()
	done := make(chan struct{})
	go func() { s.navigationJobsWG.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(10 * time.Second):
		t.Fatal("background worker did not stop")
	}
}

func TestBackgroundNavigationFailurePersistsAndReleasesSession(t *testing.T) {
	s, _, log, _ := navigationFixture(t)
	_, created := agentSessionRequest(t, s, "POST", "/api/maps/m1/agent/sessions", `{"vehicles":[{"fromLon":1,"fromLat":2,"toLon":3,"toLat":4,"agent":true}]}`)
	session := created["sessionId"].(string)
	base := "/api/maps/m1/agent/sessions/" + session
	code, started := navigationSave(t, s, base+"/navigation-jobs", map[string]any{"source": "def route(ctx):\n    return 1 / 0", "vehicleId": 0})
	if code != 202 {
		t.Fatal(code, started)
	}
	waitNavigationJobs(t, s)
	jobPath := "/api/maps/m1/agent/navigation-jobs/" + started["id"].(string)
	code, detail := agentSessionRequest(t, s, "GET", jobPath, "")
	if code != 200 || detail["status"] != "error" || detail["snapshot"] == nil {
		t.Fatal(code, detail)
	}
	records := detail["records"].([]any)
	record := records[0].(map[string]any)
	if record["status"] != "error" || record["line"] != float64(2) || record["error"] == nil {
		t.Fatal(record)
	}
	entry, _ := s.agentSessions.get(session)
	if entry.navigationOwner != "" {
		t.Fatal("failed job still owns session")
	}
	if strings.Contains(fakeSessionLog(t, log), "\ncommit\t") {
		t.Fatal("failed code submitted a route")
	}
	s.Close()
	restarted := newAgentSessionServer(t, s.config.DataDir, s.config.ZeusMap)
	defer restarted.Close()
	code, detail = agentSessionRequest(t, restarted, "GET", jobPath, "")
	if code != 200 || detail["status"] != "error" || len(detail["records"].([]any)) != 1 {
		t.Fatal(code, detail)
	}
	if code, _ = agentSessionRequest(t, restarted, "DELETE", jobPath, ""); code != 200 {
		t.Fatal(code)
	}
}

func TestBackgroundNavigationOwnershipAndPausedLifecycle(t *testing.T) {
	s, path, _, _ := navigationFixture(t)
	session := strings.Split(path, "/")[6]
	job := &backgroundNavigation{ID: newID("navjob"), MapID: "m1", SessionID: session, Source: algorithmTemplate, Status: "paused", Records: []map[string]any{}}
	s.navigationJobs = map[string]*backgroundNavigation{job.ID: job}
	if err := s.ownNavigationSession(job, false); err != nil {
		t.Fatal(err)
	}
	if err := s.saveNavigationJob(job); err != nil {
		t.Fatal(err)
	}
	base := "/api/maps/m1/agent/sessions/" + session
	for method, suffix := range map[string]string{"POST": "/step", "DELETE": ""} {
		if code, _ := agentSessionRequest(t, s, method, base+suffix, `{"ticks":1}`); code != 409 {
			t.Fatal("manual mutation accepted", code)
		}
	}
	code, observed := agentSessionRequest(t, s, "GET", base, "")
	if code != 200 || observed["navigationJobId"] != job.ID {
		t.Fatal(code, observed)
	}
	jobPath := "/api/maps/m1/agent/navigation-jobs/" + job.ID
	if code, _ = agentSessionRequest(t, s, "DELETE", jobPath, ""); code != 409 {
		t.Fatal("deleted owned checkpoint", code)
	}
	if code, _ = agentSessionRequest(t, s, "GET", strings.Replace(jobPath, "/m1/", "/other/", 1), ""); code != 404 {
		t.Fatal(code)
	}
	if code, _ = agentSessionRequest(t, s, "POST", jobPath, `{"action":"invalid"}`); code != 400 {
		t.Fatal(code)
	}
	if code, _ = agentSessionRequest(t, s, "POST", jobPath, `{"action":"stop"}`); code != 200 {
		t.Fatal(code)
	}
	entry, _ := s.agentSessions.get(session)
	if entry.navigationOwner != "" {
		t.Fatal("stop did not release session")
	}
	if code, _ = agentSessionRequest(t, s, "DELETE", jobPath, ""); code != 200 {
		t.Fatal(code)
	}
}

func TestBackgroundNavigationInterruptedLoadAndStorageLimit(t *testing.T) {
	s, _, _, _ := navigationFixture(t)
	job := &backgroundNavigation{ID: newID("navjob"), MapID: "m1", SessionID: "lost", Source: algorithmTemplate, Status: "running", Records: []map[string]any{}}
	if err := s.saveNavigationJob(job); err != nil {
		t.Fatal(err)
	}
	saved, err := os.ReadFile(s.navigationJobPath(job))
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(filepath.Dir(s.navigationJobPath(job)), ".record-in-progress.json"), []byte("{"), 0600); err != nil {
		t.Fatal(err)
	}
	job.Records = []map[string]any{{"logs": strings.Repeat("x", 16<<20)}}
	if err := s.saveNavigationJob(job); err == nil {
		t.Fatal("oversized checkpoint accepted")
	}
	after, _ := os.ReadFile(s.navigationJobPath(job))
	if string(after) != string(saved) {
		t.Fatal("failed save replaced checkpoint")
	}
	if err := s.loadNavigationJobs("m1"); err != nil {
		t.Fatal(err)
	}
	loaded := s.navigationJobs[job.ID]
	if loaded.Status != "paused" || loaded.running || loaded.Error == "" {
		t.Fatal(loaded.Status)
	}
	body, _ := json.Marshal(map[string]string{"action": "resume"})
	if code, _ := agentSessionRequest(t, s, "POST", "/api/maps/m1/agent/navigation-jobs/"+job.ID, string(body)); code != 409 {
		t.Fatal("resumed missing snapshot", code)
	}
}
