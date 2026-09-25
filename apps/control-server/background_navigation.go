package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

type navigationOwnerKey struct{}

type backgroundNavigation struct {
	mu        sync.Mutex
	ID        string                 `json:"id"`
	MapID     string                 `json:"mapId"`
	SessionID string                 `json:"sessionId"`
	VehicleID int                    `json:"vehicleId"`
	Source    string                 `json:"source"`
	Status    string                 `json:"status"`
	Error     string                 `json:"error,omitempty"`
	UpdatedAt time.Time              `json:"updatedAt"`
	Tick      uint64                 `json:"tick"`
	Records   []map[string]any       `json:"records"`
	Snapshot  *agentSnapshotArtifact `json:"snapshot,omitempty"`
	running   bool
}

// The same validated command handlers serve browser requests and detached jobs.
// Internal calls carry an unforgeable context owner instead of a public header.
type commandResponse struct {
	header http.Header
	status int
	bytes.Buffer
}

func (w *commandResponse) Header() http.Header    { return w.header }
func (w *commandResponse) WriteHeader(status int) { w.status = status }
func (s *Server) navigationCommand(job *backgroundNavigation, method string, handler http.HandlerFunc, body any, out any) error {
	payload, err := json.Marshal(body)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.WithValue(context.Background(), navigationOwnerKey{}, job.ID), 35*time.Second)
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, method, "/", bytes.NewReader(payload))
	if err != nil {
		return err
	}
	request.SetPathValue("id", job.MapID)
	request.SetPathValue("session", job.SessionID)
	response := &commandResponse{header: make(http.Header), status: 200}
	s.withAgentSession(handler)(response, request)
	if response.status >= 400 {
		return fmt.Errorf("command %d: %s", response.status, strings.TrimSpace(response.String()))
	}
	if out != nil {
		return json.Unmarshal(response.Bytes(), out)
	}
	return nil
}

func (s *Server) navigationJobPath(job *backgroundNavigation) string {
	return filepath.Join(s.config.DataDir, "maps", job.MapID, "navigation-jobs", job.ID+".json")
}
func (s *Server) saveNavigationJob(job *backgroundNavigation) error {
	job.UpdatedAt = time.Now().UTC()
	data, err := json.MarshalIndent(job, "", "  ")
	if err != nil {
		return err
	}
	if len(data) > 16<<20 {
		return errors.New("navigation job exceeds 16 MiB size limit")
	}
	path := s.navigationJobPath(job)
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	return saveJSONFile(path, json.RawMessage(data))
}

func (s *Server) ownNavigationSession(job *backgroundNavigation, release bool) error {
	s.agentSessions.mu.Lock()
	defer s.agentSessions.mu.Unlock()
	entry, ok := s.agentSessions.sessions[job.SessionID]
	if !ok || entry.mapID != job.MapID {
		return errors.New("navigation session is unavailable")
	}
	if entry.navigationOwner != "" && entry.navigationOwner != job.ID {
		return errors.New("session already belongs to another navigation job")
	}
	if release {
		entry.navigationOwner = ""
	} else {
		entry.navigationOwner = job.ID
	}
	s.agentSessions.sessions[job.SessionID] = entry
	return nil
}

// Called under navigationJobsMu. Interrupted jobs remain paused until explicit
// resume, which replays the atomically stored native checkpoint into a new session.
func (s *Server) loadNavigationJobs(mapID string) error {
	if s.navigationJobs == nil {
		s.navigationJobs = make(map[string]*backgroundNavigation)
	}
	dir := filepath.Join(s.config.DataDir, "maps", mapID, "navigation-jobs")
	entries, err := os.ReadDir(dir)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasPrefix(entry.Name(), "navjob_") || !strings.HasSuffix(entry.Name(), ".json") {
			continue
		}
		id := strings.TrimSuffix(entry.Name(), ".json")
		if _, ok := s.navigationJobs[id]; ok {
			continue
		}
		info, err := entry.Info()
		if err != nil {
			return err
		}
		if info.Size() > 16<<20 {
			return errors.New("navigation job exceeds size limit")
		}
		data, err := os.ReadFile(filepath.Join(dir, entry.Name()))
		if err != nil {
			return err
		}
		var job backgroundNavigation
		if json.Unmarshal(data, &job) != nil || job.ID != id || job.MapID != mapID {
			return errors.New("invalid navigation job file")
		}
		if job.Status == "running" || job.Status == "pausing" {
			job.Status = "paused"
			job.Error = "服务曾中断，继续时从检查点恢复"
		}
		s.navigationJobs[id] = &job
	}
	return nil
}

func (s *Server) handleStartBackgroundNavigation(w http.ResponseWriter, r *http.Request) {
	var request struct {
		Source    string `json:"source"`
		VehicleID int    `json:"vehicleId"`
	}
	r.Body = http.MaxBytesReader(w, r.Body, 64<<10)
	if decodeJSON(r, &request) != nil || len(request.Source) > 32768 || strings.TrimSpace(request.Source) == "" || request.VehicleID < 0 {
		writeError(w, 400, "有效源码和车辆编号是必填项")
		return
	}
	s.navigationJobsMu.Lock()
	defer s.navigationJobsMu.Unlock()
	if s.navigationJobsClosed {
		writeError(w, 503, "server shutting down")
		return
	}
	if err := s.loadNavigationJobs(r.PathValue("id")); err != nil {
		writeError(w, 500, err.Error())
		return
	}
	count := 0
	for _, job := range s.navigationJobs {
		if job.MapID == r.PathValue("id") {
			count++
		}
	}
	if count >= 100 {
		writeError(w, 409, "后台导航历史已达 100 条，请先删除已停止任务")
		return
	}
	job := &backgroundNavigation{ID: newID("navjob"), MapID: r.PathValue("id"), SessionID: r.PathValue("session"), VehicleID: request.VehicleID, Source: request.Source, Status: "running", Records: []map[string]any{}}
	if err := s.ownNavigationSession(job, false); err != nil {
		writeError(w, 409, err.Error())
		return
	}
	if err := s.saveNavigationJob(job); err != nil {
		_ = s.ownNavigationSession(job, true)
		writeError(w, 500, err.Error())
		return
	}
	s.navigationJobs[job.ID] = job
	job.running = true
	s.navigationJobsWG.Add(1)
	// The request still holds commandMu; the worker waits for its release.
	entry, _ := s.agentSessions.get(job.SessionID)
	go func() { entry.commandMu.Lock(); entry.commandMu.Unlock(); s.runBackgroundNavigation(job) }()
	writeJSON(w, 202, job)
}

func (s *Server) handleBackgroundNavigation(w http.ResponseWriter, r *http.Request) {
	if _, err := s.mapRecord(r.PathValue("id")); err != nil {
		writeError(w, 404, "地图不存在")
		return
	}
	s.navigationJobsMu.Lock()
	defer s.navigationJobsMu.Unlock()
	if s.navigationJobsClosed {
		writeError(w, 503, "server shutting down")
		return
	}
	if err := s.loadNavigationJobs(r.PathValue("id")); err != nil {
		writeError(w, 500, err.Error())
		return
	}
	id := r.PathValue("job")
	if id == "" {
		summaries := []map[string]any{}
		for _, job := range s.navigationJobs {
			if job.MapID != r.PathValue("id") {
				continue
			}
			job.mu.Lock()
			summaries = append(summaries, map[string]any{"id": job.ID, "sessionId": job.SessionID, "status": job.Status, "tick": job.Tick, "records": len(job.Records), "error": job.Error, "updatedAt": job.UpdatedAt})
			job.mu.Unlock()
		}
		sort.Slice(summaries, func(i, j int) bool {
			return summaries[i]["updatedAt"].(time.Time).After(summaries[j]["updatedAt"].(time.Time))
		})
		writeJSON(w, 200, summaries)
		return
	}
	job, ok := s.navigationJobs[id]
	if !ok || job.MapID != r.PathValue("id") {
		writeError(w, 404, "navigation job not found")
		return
	}
	job.mu.Lock()
	defer job.mu.Unlock()
	if r.Method == http.MethodDelete {
		if job.running || job.Status == "paused" {
			writeError(w, 409, "请先停止任务")
			return
		}
		if err := os.Remove(s.navigationJobPath(job)); err != nil {
			writeError(w, 500, err.Error())
			return
		}
		delete(s.navigationJobs, id)
		writeJSON(w, 200, map[string]bool{"deleted": true})
		return
	}
	if r.Method == http.MethodPost {
		var request struct {
			Action string `json:"action"`
		}
		r.Body = http.MaxBytesReader(w, r.Body, 1024)
		if decodeJSON(r, &request) != nil {
			writeError(w, 400, "invalid action")
			return
		}
		switch request.Action {
		case "pause":
			if job.Status == "running" {
				job.Status = "pausing"
			}
		case "stop":
			if job.Status != "completed" && job.Status != "error" {
				job.Status = "stopped"
			}
			if !job.running {
				_ = s.ownNavigationSession(job, true)
			}
		case "resume":
			if job.running || job.Status != "paused" {
				writeError(w, 409, "只有暂停任务可继续")
				return
			}
			if len(job.Records) >= 200 {
				writeError(w, 409, "已达 200 次决策上限，请停止后创建新任务")
				return
			}
			if _, exists := s.agentSessions.get(job.SessionID); !exists {
				if job.Snapshot == nil {
					writeError(w, 409, "任务尚无可恢复检查点")
					return
				}
				sessionID := newID("ses")
				ctx, cancel := context.WithTimeout(r.Context(), 2*time.Minute)
				_, record, err := s.replayAgentSnapshot(ctx, *job.Snapshot, sessionID)
				cancel()
				if err != nil {
					writeError(w, 409, err.Error())
					return
				}
				s.agentSessions.add(sessionID, agentSessionEntry{mapID: job.MapID, runtime: record.Runtime, mapRevision: record.RuntimeRevision, stepSecond: job.Snapshot.StepSecond, request: job.Snapshot.Request})
				job.SessionID = sessionID
			}
			if err := s.ownNavigationSession(job, false); err != nil {
				writeError(w, 409, err.Error())
				return
			}
			job.Status, job.Error = "running", ""
			if err := s.saveNavigationJob(job); err != nil {
				job.Status = "paused"
				writeError(w, 500, err.Error())
				return
			}
			job.running = true
			s.navigationJobsWG.Add(1)
			go s.runBackgroundNavigation(job)
		default:
			writeError(w, 400, "action must be pause, resume or stop")
			return
		}
		if err := s.saveNavigationJob(job); err != nil {
			writeError(w, 500, err.Error())
			return
		}
	}
	writeJSON(w, 200, job)
}

type navigationObservation struct {
	sessionStateFields
	DecisionID string `json:"decisionId"`
	Vehicles   []struct {
		VehicleID int    `json:"vehicleId"`
		RouteID   uint64 `json:"routeId"`
		State     string `json:"state"`
	} `json:"agents"`
}

func (o navigationObservation) vehicle(id int) (uint64, string) {
	for _, v := range o.Vehicles {
		if v.VehicleID == id {
			return v.RouteID, v.State
		}
	}
	return 0, "missing"
}

func (s *Server) checkpointNavigationJob(job *backgroundNavigation, observed navigationObservation) error {
	entry, ok := s.agentSessions.get(job.SessionID)
	if !ok {
		return errors.New("session disappeared")
	}
	var snapshot struct {
		Tick         uint64              `json:"tick"`
		StateVersion uint64              `json:"stateVersion"`
		Actions      []agentReplayAction `json:"actions"`
	}
	id := newID("snp")
	err := s.navigationCommand(job, "POST", func(w http.ResponseWriter, r *http.Request) {
		s.agentSessionPassthrough(w, r, "snapshot", job.SessionID, id)
	}, nil, &snapshot)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	_, dropErr := s.sessionWorkers.Command(ctx, entry.runtime, "drop-snapshot", id)
	cancel()
	if dropErr != nil {
		return dropErr
	}
	previousTick, previousSnapshot := job.Tick, job.Snapshot
	job.Tick = snapshot.Tick
	job.Snapshot = &agentSnapshotArtifact{FormatVersion: agentSnapshotFormatVersion, SnapshotID: id, MapID: job.MapID, SourceSessionID: job.SessionID, CreatedAt: time.Now().UTC(), Tick: snapshot.Tick, StateVersion: snapshot.StateVersion, StepSecond: entry.stepSecond, DecisionPending: observed.DecisionID != "", Request: entry.request, AppliedActions: snapshot.Actions}
	if err := sealAgentSnapshot(job.Snapshot, entry.mapRevision); err != nil {
		job.Tick, job.Snapshot = previousTick, previousSnapshot
		return err
	}
	if err := s.saveNavigationJob(job); err != nil {
		job.Tick, job.Snapshot = previousTick, previousSnapshot
		return err
	}
	return nil
}

func (s *Server) runBackgroundNavigation(job *backgroundNavigation) {
	defer s.navigationJobsWG.Done()
	finish := func() {
		if job.Status == "pausing" {
			job.Status = "paused"
		}
		// A pause can arrive before the first iteration starts.
		if job.Snapshot == nil && job.Status == "paused" {
			var observed navigationObservation
			err := s.navigationCommand(job, "GET", s.handleObserveAgentSession, nil, &observed)
			if err == nil {
				err = s.checkpointNavigationJob(job, observed)
			}
			if err != nil {
				job.Status, job.Error = "error", err.Error()
			}
		}
		job.running = false
		if job.Status != "paused" {
			_ = s.ownNavigationSession(job, true)
		}
		if err := s.saveNavigationJob(job); err != nil {
			job.Error = err.Error()
		}
	}
	initial := true
	for {
		job.mu.Lock()
		if job.Status != "running" {
			finish()
			job.mu.Unlock()
			return
		}
		err := s.navigationIteration(job, initial)
		initial = false
		if err != nil {
			job.Status, job.Error = "error", err.Error()
			if len(job.Records) > 0 {
				record := job.Records[len(job.Records)-1]
				if record["status"] == "planning" || record["status"] == "queued" {
					record["error"] = err.Error()
					// A queued action may still apply on a later manual step.
					if record["status"] == "planning" {
						record["status"] = "error"
					}
				}
			}
		}
		if job.Status != "running" {
			finish()
			job.mu.Unlock()
			return
		}
		job.mu.Unlock()
		// Give control requests a chance at every acknowledged checkpoint.
		time.Sleep(time.Millisecond)
	}
}

func (s *Server) navigationIteration(job *backgroundNavigation, initial bool) error {
	var observed navigationObservation
	observe := func() error { return s.navigationCommand(job, "GET", s.handleObserveAgentSession, nil, &observed) }
	if err := observe(); err != nil {
		return err
	}
	oldRoute, state := observed.vehicle(job.VehicleID)
	if state == "arrived" {
		job.Status = "completed"
		return s.checkpointNavigationJob(job, observed)
	}
	if observed.Finished {
		return errors.New("仿真已结束，车辆未到达终点")
	}
	if !observed.Paused || state == "missing" || state == "unroutable" {
		return errors.New("车辆不处于可导航的暂停边界")
	}
	if initial {
		if err := s.checkpointNavigationJob(job, observed); err != nil {
			return err
		}
		if observed.DecisionID == "" {
			var decision struct {
				DecisionID string `json:"decisionId"`
			}
			if err := s.navigationCommand(job, "POST", s.handleAgentDecision, map[string]any{"vehicleId": job.VehicleID, "basedOnStateVersion": observed.StateVersion}, &decision); err != nil {
				return err
			}
			observed.DecisionID = decision.DecisionID
		}
	}
	body := map[string]any{"basedOnStateVersion": observed.StateVersion, "untilEvent": true, "maxTicks": 16}
	var record map[string]any
	if observed.DecisionID != "" {
		if len(job.Records) >= 200 {
			job.Status = "pausing"
			return s.checkpointNavigationJob(job, observed)
		}
		record = map[string]any{"stateVersion": observed.StateVersion, "tick": observed.Tick, "decisionId": observed.DecisionID, "status": "planning", "sessionId": job.SessionID}
		job.Records = append(job.Records, record)
		var planned struct {
			Result    AlgorithmLabResult `json:"result"`
			Candidate struct {
				OK      bool   `json:"ok"`
				ID      string `json:"candidateId"`
				Version uint64 `json:"basedOnStateVersion"`
				Edges   []int  `json:"edges"`
			} `json:"candidate"`
		}
		if err := s.navigationCommand(job, "POST", s.handleAlgorithmVehiclePlan, algorithmVehicleRequest{VehicleID: job.VehicleID, BasedOnStateVersion: observed.StateVersion, Source: job.Source, Steps: 5000000}, &planned); err != nil {
			return err
		}
		record["codeRevision"], record["snapshotId"], record["logs"], record["edges"] = planned.Result.CodeRevision, planned.Result.SnapshotID, planned.Result.Logs, planned.Candidate.Edges
		record["line"], record["computeMs"], record["observation"] = planned.Result.Line, planned.Result.ComputeMs, planned.Result.Observation
		if !planned.Result.OK || (planned.Result.Phase != "verified" && planned.Result.Phase != "kept") {
			return fmt.Errorf("代码未产生可执行路线: %s %s", planned.Result.Phase, planned.Result.Error)
		}
		kind := "keep_route"
		if planned.Result.Phase == "verified" {
			if !planned.Candidate.OK || planned.Candidate.Version != observed.StateVersion {
				return errors.New("候选版本已过期")
			}
			kind = "commit_route"
		}
		var ack struct {
			Accepted bool   `json:"accepted"`
			Reason   string `json:"reason"`
		}
		if err := s.navigationCommand(job, "POST", s.handleAgentAction, map[string]any{"decisionId": observed.DecisionID, "vehicleId": job.VehicleID, "kind": kind, "candidateId": planned.Candidate.ID, "basedOnStateVersion": observed.StateVersion, "reasonCode": "background_navigation"}, &ack); err != nil {
			return err
		}
		if !ack.Accepted {
			return fmt.Errorf("动作被拒绝: %s", ack.Reason)
		}
		record["status"], record["action"] = "queued", kind
		body = map[string]any{"ticks": 1, "basedOnStateVersion": observed.StateVersion}
	}
	if err := s.navigationCommand(job, "POST", s.handleAgentStep, body, nil); err != nil {
		return err
	}
	if err := observe(); err != nil {
		return err
	}
	if record != nil {
		route, _ := observed.vehicle(job.VehicleID)
		if record["action"] == "commit_route" {
			if route == oldRoute {
				return errors.New("候选未被车辆采用")
			}
			record["status"] = "applied"
		} else {
			record["status"] = "kept"
		}
	}
	return s.checkpointNavigationJob(job, observed)
}

func (s *Server) closeBackgroundNavigation() {
	s.navigationJobsMu.Lock()
	s.navigationJobsClosed = true
	jobs := make([]*backgroundNavigation, 0, len(s.navigationJobs))
	for _, job := range s.navigationJobs {
		jobs = append(jobs, job)
	}
	s.navigationJobsMu.Unlock()
	for _, job := range jobs {
		job.mu.Lock()
		if job.Status == "running" {
			job.Status = "pausing"
		}
		job.mu.Unlock()
	}
	s.navigationJobsWG.Wait()
}
