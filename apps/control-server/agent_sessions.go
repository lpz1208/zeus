package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"math"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Agent session endpoints bridge the HTTP API, the resident C++ session
// worker, and the DecisionCoordinator. The decision loop is request driven:
// step(until_event) returns at a decision boundary, the coordinator barrier
// opens, an explicit actions call resolves it, and the client steps again.

type AgentVehicleSpec struct {
	FromLon       float64 `json:"fromLon"`
	FromLat       float64 `json:"fromLat"`
	ToLon         float64 `json:"toLon"`
	ToLat         float64 `json:"toLat"`
	DepartSeconds float64 `json:"departSeconds"`
	Algorithm     string  `json:"algorithm,omitempty"`
	Agent         bool    `json:"agent,omitempty"`
}

type AgentSessionRequest struct {
	Vehicles               []AgentVehicleSpec          `json:"vehicles"`
	DurationSeconds        float64                     `json:"durationSeconds"`
	StepSeconds            float64                     `json:"stepSeconds"`
	SampleIntervalSeconds  float64                     `json:"sampleIntervalSeconds"`
	ExitHeadwayFfSeconds   float64                     `json:"exitHeadwayFfSeconds"`
	ExitHeadwayJamSeconds  float64                     `json:"exitHeadwayJamSeconds"`
	RerouteIntervalSeconds float64                     `json:"rerouteIntervalSeconds"`
	RerouteCostRatio       float64                     `json:"rerouteCostRatio"`
	MinSpeedRatio          float64                     `json:"minSpeedRatio"`
	VehicleControls        []VehicleSimulationControl  `json:"vehicleControls,omitempty"`
	RoadControls           []RoadSimulationControl     `json:"roadControls,omitempty"`
	JunctionControls       []JunctionSimulationControl `json:"junctionControls,omitempty"`
	SignalPlans            []JunctionSignalPlan        `json:"signalPlans,omitempty"`
}

type agentSessionEntry struct {
	mapID          string
	runtime        string
	stepSecond     float64
	activeDecision string
}

type agentSnapshotEntry struct {
	mapID      string
	runtime    string
	stepSecond float64
}

type agentSessionRegistry struct {
	mu        sync.Mutex
	sessions  map[string]agentSessionEntry
	snapshots map[string]agentSnapshotEntry
}

func (r *agentSessionRegistry) add(id string, entry agentSessionEntry) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.sessions[id] = entry
}

func (r *agentSessionRegistry) get(id string) (agentSessionEntry, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	entry, ok := r.sessions[id]
	return entry, ok
}

func (r *agentSessionRegistry) remove(id string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.sessions, id)
}

func (r *agentSessionRegistry) beginDecision(id, decisionID string) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	entry, ok := r.sessions[id]
	if !ok {
		return errors.New("unknown agent session")
	}
	if entry.activeDecision != "" {
		return fmt.Errorf("decision %s is still pending", entry.activeDecision)
	}
	entry.activeDecision = decisionID
	r.sessions[id] = entry
	return nil
}

func (r *agentSessionRegistry) clearDecision(id, decisionID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	entry, ok := r.sessions[id]
	if !ok || entry.activeDecision != decisionID {
		return
	}
	entry.activeDecision = ""
	r.sessions[id] = entry
}

func (r *agentSessionRegistry) addSnapshot(id string, entry agentSnapshotEntry) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.snapshots[id] = entry
}

func (r *agentSessionRegistry) getSnapshot(id string) (agentSnapshotEntry, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	entry, ok := r.snapshots[id]
	return entry, ok
}

func (r *agentSessionRegistry) removeSnapshot(id string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.snapshots, id)
}

// sessionStateFields mirrors the worker's state header for step responses.
type sessionStateFields struct {
	Tick            uint64  `json:"tick"`
	SimulationTimeS float64 `json:"simulationTimeS"`
	StateVersion    uint64  `json:"stateVersion"`
	Finished        bool    `json:"finished"`
	Cancelled       bool    `json:"cancelled"`
	DecisionDue     bool    `json:"decisionDue"`
	DecisionReason  string  `json:"decisionReason"`
	SessionID       string  `json:"sessionId,omitempty"`
	Vehicles        int     `json:"vehicles,omitempty"`
	Agents          []int   `json:"agents,omitempty"`
	AgentVehicleIDs []int   `json:"agentVehicleIds,omitempty"`
	Accepted        *bool   `json:"accepted,omitempty"`
	Reason          string  `json:"reason,omitempty"`
	AppliesNextTick *bool   `json:"appliesAtNextTick,omitempty"`
	Error           string  `json:"error,omitempty"`
}

func (s *Server) agentSessionCommand(
	w http.ResponseWriter,
	r *http.Request,
	fields ...string,
) (SessionCommandResult, json.RawMessage) {
	entry, ok := s.agentSessions.get(r.PathValue("session"))
	if !ok {
		writeError(w, http.StatusNotFound, "unknown agent session")
		return SessionCommandResult{}, nil
	}
	if mapID := r.PathValue("id"); mapID != "" && mapID != entry.mapID {
		writeError(w, http.StatusNotFound, "agent session belongs to another map")
		return SessionCommandResult{}, nil
	}
	result, err := s.sessionWorkers.Command(r.Context(), entry.runtime, fields...)
	if err != nil {
		writeError(w, http.StatusBadGateway, err.Error())
		return SessionCommandResult{}, nil
	}
	if result.ExitCode == 1 {
		var state sessionStateFields
		_ = json.Unmarshal(result.Payload, &state)
		message := state.Error
		if message == "" {
			message = string(result.Payload)
		}
		if strings.Contains(message, "unknown session") {
			s.agentSessions.remove(r.PathValue("session"))
			writeError(w, http.StatusNotFound, message)
			return result, nil
		}
		writeError(w, http.StatusBadRequest, message)
		return result, nil
	}
	return result, result.Payload
}

type agentWorkerActionError struct {
	status  int
	message string
}

func (e *agentWorkerActionError) Error() string { return e.message }

// applyAgentWorkerAction is the environment side of a two-phase decision
// submission. Only an explicit accepted acknowledgement allows the
// DecisionCoordinator to close the barrier.
func (s *Server) applyAgentWorkerAction(
	ctx context.Context,
	entry agentSessionEntry,
	fields ...string,
) (SessionCommandResult, error) {
	result, err := s.sessionWorkers.Command(ctx, entry.runtime, fields...)
	if err != nil {
		return SessionCommandResult{}, &agentWorkerActionError{
			status: http.StatusBadGateway, message: err.Error()}
	}
	if result.ExitCode != 0 {
		var payload struct {
			Error string `json:"error"`
		}
		_ = json.Unmarshal(result.Payload, &payload)
		if payload.Error == "" {
			payload.Error = string(result.Payload)
		}
		return SessionCommandResult{}, &agentWorkerActionError{
			status: http.StatusConflict, message: payload.Error}
	}
	var acknowledgement struct {
		Accepted bool   `json:"accepted"`
		Reason   string `json:"reason"`
	}
	if err := json.Unmarshal(result.Payload, &acknowledgement); err != nil {
		return SessionCommandResult{}, &agentWorkerActionError{
			status:  http.StatusBadGateway,
			message: "unreadable worker action acknowledgement"}
	}
	if !acknowledgement.Accepted {
		message := acknowledgement.Reason
		if message == "" {
			message = "worker rejected agent action"
		}
		return SessionCommandResult{}, &agentWorkerActionError{
			status: http.StatusConflict, message: message}
	}
	return result, nil
}

func (s *Server) handleAgentTools(w http.ResponseWriter, r *http.Request) {
	record, err := s.mapRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}
	result, err := s.sessionWorkers.Command(r.Context(), record.Runtime, "tools")
	if err != nil {
		writeError(w, http.StatusBadGateway, err.Error())
		return
	}
	if result.ExitCode != 0 {
		writeError(w, http.StatusBadRequest, sessionWorkerErrorMessage(result.Payload))
		return
	}
	writeJSON(w, http.StatusOK, json.RawMessage(result.Payload))
}

func (s *Server) handleCreateAgentSession(w http.ResponseWriter, r *http.Request) {
	record, err := s.mapRecord(r.PathValue("id"))
	if err != nil {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}
	var request AgentSessionRequest
	if err := decodeJSON(r, &request); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	odPath, err := writeAgentOdFile(&request)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	defer os.Remove(odPath)

	config := SimulateRequest{
		Count:            len(request.Vehicles),
		DurationSeconds:  request.DurationSeconds,
		StepSeconds:      request.StepSeconds,
		VehicleControls:  request.VehicleControls,
		RoadControls:     request.RoadControls,
		JunctionControls: request.JunctionControls,
		SignalPlans:      request.SignalPlans,
	}
	controlsPath, err := writeAgentCsvFile(
		func() (string, error) { return buildSimulationControls(config, record.Summary) },
		"zeus-agent-controls-*.csv")
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if controlsPath != "" {
		defer os.Remove(controlsPath)
	}
	signalsPath, err := writeAgentCsvFile(
		func() (string, error) { return buildSignalPlans(config, record.Summary) },
		"zeus-agent-signals-*.csv")
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if signalsPath != "" {
		defer os.Remove(signalsPath)
	}

	sessionID := newID("ses")
	result, err := s.sessionWorkers.Command(
		r.Context(), record.Runtime,
		"reset", sessionID,
		formatFloat(request.DurationSeconds),
		formatFloat(request.StepSeconds),
		formatFloat(request.SampleIntervalSeconds),
		formatFloat(request.ExitHeadwayFfSeconds),
		formatFloat(request.ExitHeadwayJamSeconds),
		formatFloat(request.RerouteIntervalSeconds),
		formatFloat(request.RerouteCostRatio),
		formatFloat(request.MinSpeedRatio),
		odPath, controlsPath, signalsPath)
	if err != nil {
		writeError(w, http.StatusBadGateway, err.Error())
		return
	}
	if result.ExitCode != 0 {
		writeError(w, http.StatusBadRequest, string(result.Payload))
		return
	}
	step := request.StepSeconds
	if step <= 0 {
		step = 1
	}
	s.agentSessions.add(sessionID, agentSessionEntry{
		mapID: r.PathValue("id"), runtime: record.Runtime, stepSecond: step})
	s.logger.Info("agent session created",
		slog.String("session", sessionID), slog.String("map", r.PathValue("id")))
	writeJSON(w, http.StatusOK, json.RawMessage(result.Payload))
}

func writeAgentOdFile(request *AgentSessionRequest) (string, error) {
	if len(request.Vehicles) == 0 {
		return "", errors.New("vehicles must contain at least one demand row")
	}
	if len(request.Vehicles) > 10000 {
		return "", errors.New("vehicles must contain at most 10000 demand rows")
	}
	var output strings.Builder
	output.WriteString("# lon,lat,dest_lon,dest_lat,depart_s,algorithm,agent\n")
	for index, vehicle := range request.Vehicles {
		for _, value := range []float64{
			vehicle.FromLon, vehicle.FromLat, vehicle.ToLon, vehicle.ToLat,
			vehicle.DepartSeconds,
		} {
			if math.IsNaN(value) || math.IsInf(value, 0) {
				return "", fmt.Errorf("vehicles[%d] contains a non-finite value", index)
			}
		}
		algorithm := vehicle.Algorithm
		if algorithm == "" {
			algorithm = "dijkstra"
		}
		switch algorithm {
		case "dijkstra", "astar", "bidijkstra", "biastar":
		default:
			return "", fmt.Errorf(
				"vehicles[%d].algorithm must be dijkstra, astar, bidijkstra or biastar", index)
		}
		agent := ""
		if vehicle.Agent {
			agent = "agent"
		}
		fmt.Fprintf(&output, "%s,%s,%s,%s,%s,%s,%s\n",
			formatFloat(vehicle.FromLon), formatFloat(vehicle.FromLat),
			formatFloat(vehicle.ToLon), formatFloat(vehicle.ToLat),
			formatFloat(vehicle.DepartSeconds), algorithm, agent)
	}
	file, err := os.CreateTemp("", "zeus-agent-od-*.csv")
	if err != nil {
		return "", fmt.Errorf("create od file: %w", err)
	}
	if _, err := file.WriteString(output.String()); err != nil {
		file.Close()
		os.Remove(file.Name())
		return "", fmt.Errorf("write od file: %w", err)
	}
	if err := file.Close(); err != nil {
		os.Remove(file.Name())
		return "", fmt.Errorf("close od file: %w", err)
	}
	return file.Name(), nil
}

func writeAgentCsvFile(build func() (string, error), pattern string) (string, error) {
	content, err := build()
	if err != nil {
		return "", err
	}
	if content == "" {
		return "", nil
	}
	file, err := os.CreateTemp("", pattern)
	if err != nil {
		return "", fmt.Errorf("create csv file: %w", err)
	}
	if _, err := file.WriteString(content); err != nil {
		file.Close()
		os.Remove(file.Name())
		return "", fmt.Errorf("write csv file: %w", err)
	}
	if err := file.Close(); err != nil {
		os.Remove(file.Name())
		return "", fmt.Errorf("close csv file: %w", err)
	}
	return file.Name(), nil
}

// agentSessionPassthrough forwards a worker response to the HTTP client
// verbatim; agentSessionCommand only handles errors.
func (s *Server) agentSessionPassthrough(
	w http.ResponseWriter,
	r *http.Request,
	fields ...string,
) {
	_, payload := s.agentSessionCommand(w, r, fields...)
	if payload == nil {
		return
	}
	writeJSON(w, http.StatusOK, payload)
}

func (s *Server) handleObserveAgentSession(w http.ResponseWriter, r *http.Request) {
	s.agentSessionPassthrough(w, r, "observe", r.PathValue("session"), "hot")
}

func (s *Server) handleAgentObserveVehicle(w http.ResponseWriter, r *http.Request) {
	s.agentSessionPassthrough(
		w, r, "agent-observe", r.PathValue("session"), r.PathValue("vehicle"))
}

type AgentPlanRequest struct {
	VehicleID int    `json:"vehicleId"`
	Algorithm string `json:"algorithm"`
}

func (s *Server) handleAgentPlan(w http.ResponseWriter, r *http.Request) {
	var request AgentPlanRequest
	if err := decodeJSON(r, &request); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if request.Algorithm == "" {
		request.Algorithm = "astar"
	}
	switch request.Algorithm {
	case "dijkstra", "astar", "bidijkstra", "biastar":
	default:
		writeError(w, http.StatusBadRequest,
			"algorithm must be dijkstra, astar, bidijkstra or biastar")
		return
	}
	if request.VehicleID < 0 {
		writeError(w, http.StatusBadRequest, "vehicleId must be non-negative")
		return
	}
	s.agentSessionPassthrough(
		w, r, "plan", r.PathValue("session"),
		strconv.Itoa(request.VehicleID), request.Algorithm)
}

type AgentStepRequest struct {
	Ticks      uint64 `json:"ticks"`
	UntilEvent bool   `json:"untilEvent"`
	MaxTicks   uint64 `json:"maxTicks"`
}

func (s *Server) handleAgentStep(w http.ResponseWriter, r *http.Request) {
	var request AgentStepRequest
	if err := decodeJSON(r, &request); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	sessionID := r.PathValue("session")
	if entry, ok := s.agentSessions.get(sessionID); ok && entry.activeDecision != "" {
		writeError(w, http.StatusConflict,
			"resolve pending decision "+entry.activeDecision+" before stepping")
		return
	}
	var result SessionCommandResult
	var payload json.RawMessage
	if request.UntilEvent {
		maxTicks := request.MaxTicks
		if maxTicks == 0 {
			maxTicks = 100000
		}
		result, payload = s.agentSessionCommand(
			w, r, "step_event", sessionID, strconv.FormatUint(maxTicks, 10))
	} else {
		if request.Ticks == 0 {
			request.Ticks = 1
		}
		result, payload = s.agentSessionCommand(
			w, r, "step", sessionID, strconv.FormatUint(request.Ticks, 10))
	}
	if payload == nil {
		return
	}
	var state sessionStateFields
	if err := json.Unmarshal(result.Payload, &state); err != nil {
		writeError(w, http.StatusBadGateway, "unreadable step response")
		return
	}
	response := map[string]any{"state": state}
	if state.DecisionDue {
		decision, err := s.openDecisionBarrier(sessionID, state)
		if err != nil {
			writeError(w, http.StatusInternalServerError, err.Error())
			return
		}
		response["decisionId"] = decision
	}
	writeJSON(w, http.StatusOK, response)
}

// openDecisionBarrier registers the coordinator barrier for a decision
// boundary and spawns the wall-clock waiter that resolves it through the
// deterministic fallback when no action arrives in time.
func (s *Server) openDecisionBarrier(
	sessionID string,
	state sessionStateFields,
) (string, error) {
	entry, ok := s.agentSessions.get(sessionID)
	if !ok {
		return "", errors.New("unknown agent session")
	}
	decisionID := newID("dec")
	if err := s.agentSessions.beginDecision(sessionID, decisionID); err != nil {
		return "", err
	}
	validity := math.Min(30*entry.stepSecond, 60)
	observation := DecisionObservation{
		SessionID:                sessionID,
		DecisionID:               decisionID,
		AgentID:                  "default",
		Tick:                     state.Tick,
		SimulationTimeSeconds:    state.SimulationTimeS,
		StateVersion:             state.StateVersion,
		ValidUntilSimulationTime: state.SimulationTimeS + validity,
		Mode:                     DecisionModeAsyncThroughput,
	}
	fallback := DecisionAction{
		DecisionID:               decisionID,
		AgentID:                  "default",
		BasedOnStateVersion:      state.StateVersion,
		ValidUntilSimulationTime: observation.ValidUntilSimulationTime,
		Kind:                     NavigationActionKeepRoute,
		ReasonCode:               "decision_wall_timeout",
	}
	pending, err := s.decisions.Begin(observation, fallback)
	if err != nil {
		s.agentSessions.clearDecision(sessionID, decisionID)
		return "", err
	}
	agents := append([]int(nil), state.AgentVehicleIDs...)
	if len(agents) == 0 {
		agents = append(agents, state.Agents...)
	}
	go func() {
		defer s.agentSessions.clearDecision(sessionID, decisionID)
		outcome, waitErr := pending.Wait(
			context.Background(), s.decisionWallTTL)
		if waitErr != nil {
			s.logger.Warn("decision wait failed",
				slog.String("decision", decisionID), slog.String("error", waitErr.Error()))
			return
		}
		if outcome.UsedFallback {
			for _, vehicleID := range agents {
				timeout := s.config.CmdLimit
				if timeout <= 0 || timeout > 30*time.Second {
					timeout = 30 * time.Second
				}
				ctx, cancel := context.WithTimeout(context.Background(), timeout)
				_, applyErr := s.applyAgentWorkerAction(
					ctx, entry, "keep", sessionID, strconv.Itoa(vehicleID),
					strconv.FormatUint(outcome.Action.BasedOnStateVersion, 10))
				cancel()
				if applyErr != nil {
					s.logger.Warn("decision fallback apply failed",
						slog.String("decision", decisionID),
						slog.Int("vehicle", vehicleID),
						slog.String("error", applyErr.Error()))
				}
			}
		}
		s.logger.Info("decision resolved",
			slog.String("decision", decisionID),
			slog.String("resolution", string(outcome.Resolution)),
			slog.Bool("fallback", outcome.UsedFallback),
			slog.Int64("wall_latency_ms", outcome.WallLatency().Milliseconds()))
	}()
	return decisionID, nil
}

type AgentActionRequest struct {
	DecisionID               string  `json:"decisionId"`
	AgentID                  string  `json:"agentId"`
	VehicleID                int     `json:"vehicleId"`
	Kind                     string  `json:"kind"`
	CandidateID              string  `json:"candidateId"`
	BasedOnStateVersion      uint64  `json:"basedOnStateVersion"`
	ValidUntilSimulationTime float64 `json:"validUntilSimulationTime"`
	ReasonCode               string  `json:"reasonCode"`
}

func (s *Server) handleAgentAction(w http.ResponseWriter, r *http.Request) {
	var request AgentActionRequest
	if err := decodeJSON(r, &request); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	sessionID := r.PathValue("session")
	kind := NavigationActionKind(request.Kind)
	if kind != NavigationActionKeepRoute && kind != NavigationActionCommitRoute {
		writeError(w, http.StatusBadRequest, "kind must be keep_route or commit_route")
		return
	}
	if request.VehicleID < 0 {
		writeError(w, http.StatusBadRequest, "vehicleId must be non-negative")
		return
	}
	if kind == NavigationActionCommitRoute && request.CandidateID == "" {
		writeError(w, http.StatusBadRequest, "commit_route requires candidateId")
		return
	}

	// The guard reads the authoritative session state immediately before
	// accepting the action.
	result, payload := s.agentSessionCommand(w, r, "observe", sessionID, "hot")
	if payload == nil {
		return
	}
	var state sessionStateFields
	if err := json.Unmarshal(result.Payload, &state); err != nil {
		writeError(w, http.StatusBadGateway, "unreadable observe response")
		return
	}
	action := DecisionAction{
		DecisionID:               request.DecisionID,
		AgentID:                  request.AgentID,
		BasedOnStateVersion:      request.BasedOnStateVersion,
		ValidUntilSimulationTime: request.ValidUntilSimulationTime,
		Kind:                     kind,
		CandidateID:              request.CandidateID,
		ReasonCode:               request.ReasonCode,
	}
	if action.AgentID == "" {
		action.AgentID = "default"
	}
	// The guard takes the minimum of the observation and action windows, so
	// an omitted action window must not silently expire the decision.
	if action.ValidUntilSimulationTime == 0 {
		entry, ok := s.agentSessions.get(sessionID)
		step := 1.0
		if ok && entry.stepSecond > 0 {
			step = entry.stepSecond
		}
		action.ValidUntilSimulationTime =
			state.SimulationTimeS + math.Min(30*step, 60)
	}
	entry, ok := s.agentSessions.get(sessionID)
	if !ok {
		writeError(w, http.StatusNotFound, "unknown agent session")
		return
	}
	version := strconv.FormatUint(state.StateVersion, 10)
	vehicle := strconv.Itoa(request.VehicleID)
	fields := []string{"keep", sessionID, vehicle, version}
	if kind == NavigationActionCommitRoute {
		fields = []string{"commit", sessionID, vehicle, request.CandidateID, version}
	}
	var applied SessionCommandResult
	if err := s.decisions.SubmitWithEffect(action, DecisionState{
		SimulationTimeSeconds: state.SimulationTimeS,
		StateVersion:          state.StateVersion,
	}, func() error {
		var applyErr error
		applied, applyErr = s.applyAgentWorkerAction(r.Context(), entry, fields...)
		return applyErr
	}); err != nil {
		if errors.Is(err, ErrDecisionNotPending) ||
			errors.Is(err, ErrDecisionStateMismatch) ||
			errors.Is(err, ErrDecisionExpired) ||
			errors.Is(err, ErrDecisionAgentMismatch) ||
			errors.Is(err, ErrDecisionApplying) {
			writeError(w, http.StatusConflict, err.Error())
			return
		}
		var applyErr *agentWorkerActionError
		if errors.As(err, &applyErr) {
			writeError(w, applyErr.status, applyErr.Error())
			return
		}
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	s.agentSessions.clearDecision(sessionID, request.DecisionID)
	writeJSON(w, http.StatusOK, json.RawMessage(applied.Payload))
}

func (s *Server) handleAgentRunToEnd(w http.ResponseWriter, r *http.Request) {
	// Start the engine thread without occupying the framed command loop, so a
	// later pause/observe request can still reach this map worker.
	s.agentSessionPassthrough(w, r, "resume", r.PathValue("session"))
}

func (s *Server) handleAgentPause(w http.ResponseWriter, r *http.Request) {
	s.agentSessionPassthrough(w, r, "pause", r.PathValue("session"))
}

func (s *Server) handleCreateAgentSnapshot(w http.ResponseWriter, r *http.Request) {
	sessionID := r.PathValue("session")
	entry, ok := s.agentSessions.get(sessionID)
	if !ok || entry.mapID != r.PathValue("id") {
		writeError(w, http.StatusNotFound, "unknown agent session")
		return
	}
	snapshotID := newID("snp")
	_, payload := s.agentSessionCommand(
		w, r, "snapshot", sessionID, snapshotID)
	if payload == nil {
		return
	}
	s.agentSessions.addSnapshot(snapshotID, agentSnapshotEntry{
		mapID: entry.mapID, runtime: entry.runtime, stepSecond: entry.stepSecond})
	writeJSON(w, http.StatusOK, payload)
}

func (s *Server) handleRestoreAgentSnapshot(w http.ResponseWriter, r *http.Request) {
	snapshotID := r.PathValue("snapshot")
	snapshot, ok := s.agentSessions.getSnapshot(snapshotID)
	if !ok || snapshot.mapID != r.PathValue("id") {
		writeError(w, http.StatusNotFound, "unknown agent snapshot")
		return
	}
	sessionID := newID("ses")
	result, err := s.sessionWorkers.Command(
		r.Context(), snapshot.runtime, "restore", snapshotID, sessionID)
	if err != nil {
		writeError(w, http.StatusBadGateway, err.Error())
		return
	}
	if result.ExitCode != 0 {
		message := sessionWorkerErrorMessage(result.Payload)
		if strings.Contains(message, "unknown snapshot") {
			s.agentSessions.removeSnapshot(snapshotID)
			writeError(w, http.StatusNotFound, message)
			return
		}
		writeError(w, http.StatusBadRequest, message)
		return
	}
	var state sessionStateFields
	if err := json.Unmarshal(result.Payload, &state); err != nil {
		writeError(w, http.StatusBadGateway, "unreadable restore response")
		return
	}
	s.agentSessions.add(sessionID, agentSessionEntry{
		mapID: snapshot.mapID, runtime: snapshot.runtime,
		stepSecond: snapshot.stepSecond})
	response := map[string]any{
		"snapshotId": snapshotID,
		"state":      state,
	}
	if state.DecisionDue {
		decisionID, barrierErr := s.openDecisionBarrier(sessionID, state)
		if barrierErr != nil {
			_, _ = s.sessionWorkers.Command(
				context.Background(), snapshot.runtime, "close", sessionID)
			s.agentSessions.remove(sessionID)
			writeError(w, http.StatusInternalServerError, barrierErr.Error())
			return
		}
		response["decisionId"] = decisionID
	}
	writeJSON(w, http.StatusOK, response)
}

func (s *Server) handleDeleteAgentSnapshot(w http.ResponseWriter, r *http.Request) {
	snapshotID := r.PathValue("snapshot")
	snapshot, ok := s.agentSessions.getSnapshot(snapshotID)
	if !ok || snapshot.mapID != r.PathValue("id") {
		writeError(w, http.StatusNotFound, "unknown agent snapshot")
		return
	}
	result, err := s.sessionWorkers.Command(
		r.Context(), snapshot.runtime, "drop-snapshot", snapshotID)
	if err != nil {
		writeError(w, http.StatusBadGateway, err.Error())
		return
	}
	if result.ExitCode != 0 {
		message := sessionWorkerErrorMessage(result.Payload)
		if strings.Contains(message, "unknown snapshot") {
			s.agentSessions.removeSnapshot(snapshotID)
			writeError(w, http.StatusNotFound, message)
			return
		}
		writeError(w, http.StatusBadRequest, message)
		return
	}
	s.agentSessions.removeSnapshot(snapshotID)
	writeJSON(w, http.StatusOK, json.RawMessage(result.Payload))
}

func sessionWorkerErrorMessage(payload json.RawMessage) string {
	var response struct {
		Error string `json:"error"`
	}
	_ = json.Unmarshal(payload, &response)
	if response.Error != "" {
		return response.Error
	}
	return string(payload)
}

func (s *Server) handleAgentSessionResult(w http.ResponseWriter, r *http.Request) {
	trajectory, err := os.CreateTemp("", "zeus-agent-*.geojson")
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	trajectoryName := trajectory.Name()
	trajectory.Close()
	defer os.Remove(trajectoryName)
	playback, err := os.CreateTemp("", "zeus-agent-*.json")
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	playbackName := playback.Name()
	playback.Close()
	defer os.Remove(playbackName)

	_, payload := s.agentSessionCommand(
		w, r, "result", r.PathValue("session"), trajectoryName, playbackName)
	if payload == nil {
		return
	}
	trajectoryContent, err := os.ReadFile(trajectoryName)
	if err != nil {
		writeError(w, http.StatusBadGateway, "trajectory export missing")
		return
	}
	playbackContent, err := os.ReadFile(playbackName)
	if err != nil {
		writeError(w, http.StatusBadGateway, "playback export missing")
		return
	}
	var summary json.RawMessage = payload
	response := map[string]any{
		"summary":  summary,
		"geojson":  json.RawMessage(trajectoryContent),
		"playback": json.RawMessage(playbackContent),
	}
	writeJSON(w, http.StatusOK, response)
}

func (s *Server) handleCloseAgentSession(w http.ResponseWriter, r *http.Request) {
	result, payload := s.agentSessionCommand(w, r, "close", r.PathValue("session"))
	if payload == nil {
		return
	}
	s.agentSessions.remove(r.PathValue("session"))
	s.logger.Info("agent session closed",
		slog.String("session", r.PathValue("session")))
	_ = result
	writeJSON(w, http.StatusOK, json.RawMessage(`{"closed": true}`))
}
