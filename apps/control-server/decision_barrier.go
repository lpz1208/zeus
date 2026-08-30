package main

import (
	"context"
	"errors"
	"fmt"
	"math"
	"sync"
	"sync/atomic"
	"time"
)

var (
	ErrDecisionCoordinatorClosed = errors.New("decision coordinator is closed")
	ErrDecisionAlreadyPending    = errors.New("decision is already pending")
	ErrDecisionNotPending        = errors.New("decision is not pending")
	ErrDecisionAlreadyAwaited    = errors.New("decision is already being awaited")
	ErrDecisionAgentMismatch     = errors.New("decision agent does not match observation")
	ErrDecisionStateMismatch     = errors.New("decision state version is stale")
	ErrDecisionExpired           = errors.New("decision validity window has expired")
)

type DecisionMode string

const (
	DecisionModeBarrier           DecisionMode = "barrier"
	DecisionModeLatencySimulation DecisionMode = "latency_simulation"
	DecisionModeAsyncThroughput   DecisionMode = "async_throughput"
)

type NavigationActionKind string

const (
	NavigationActionKeepRoute       NavigationActionKind = "keep_route"
	NavigationActionSelectAlgorithm NavigationActionKind = "select_algorithm"
	NavigationActionCommitRoute     NavigationActionKind = "commit_route"
	NavigationActionDefer           NavigationActionKind = "defer"
	NavigationActionFallback        NavigationActionKind = "fallback"
)

type DecisionResolution string

const (
	DecisionApplied   DecisionResolution = "applied"
	DecisionTimedOut  DecisionResolution = "timed_out"
	DecisionCancelled DecisionResolution = "cancelled"
	DecisionClosed    DecisionResolution = "coordinator_closed"
)

// DecisionObservation identifies the immutable environment snapshot exposed
// to an agent. The validity window uses simulation time, never wall time.
type DecisionObservation struct {
	RunID                    string
	SessionID                string
	DecisionID               string
	AgentID                  string
	Tick                     uint64
	SimulationTimeSeconds    float64
	StateVersion             uint64
	ValidUntilSimulationTime float64
	Mode                     DecisionMode
}

// DecisionAction is the structured response submitted by an agent. Route
// candidates remain opaque IDs so an LLM cannot inject an arbitrary edge list.
type DecisionAction struct {
	DecisionID               string
	AgentID                  string
	BasedOnStateVersion      uint64
	ValidUntilSimulationTime float64
	Kind                     NavigationActionKind
	AlgorithmID              string
	CandidateID              string
	ReasonCode               string
	Constraints              map[string]string
}

// DecisionState is read from the authoritative simulation session immediately
// before accepting an action.
type DecisionState struct {
	SimulationTimeSeconds float64
	StateVersion          uint64
}

type DecisionOutcome struct {
	Observation  DecisionObservation
	Action       DecisionAction
	Resolution   DecisionResolution
	Reason       string
	UsedFallback bool
	OpenedAt     time.Time
	ResolvedAt   time.Time
}

func (o DecisionOutcome) WallLatency() time.Duration {
	return o.ResolvedAt.Sub(o.OpenedAt)
}

type pendingDecision struct {
	observation DecisionObservation
	fallback    DecisionAction
	openedAt    time.Time
	result      chan DecisionOutcome
	awaited     atomic.Bool
}

// PendingDecision is a handle returned after the barrier has been registered.
// Registering before publishing the observation removes the submit-before-wait
// race without creating a goroutine per logical agent.
type PendingDecision struct {
	coordinator *DecisionCoordinator
	pending     *pendingDecision
}

// Wait blocks only the caller servicing the active decision. Sleeping logical
// agents occupy no goroutine. A timeout atomically resolves the decision with
// the configured deterministic fallback.
func (p *PendingDecision) Wait(
	ctx context.Context,
	maxWallWait time.Duration,
) (DecisionOutcome, error) {
	if p == nil || p.coordinator == nil || p.pending == nil {
		return DecisionOutcome{}, ErrDecisionNotPending
	}
	if maxWallWait <= 0 {
		return DecisionOutcome{}, errors.New("decision wall timeout must be positive")
	}
	if !p.pending.awaited.CompareAndSwap(false, true) {
		return DecisionOutcome{}, ErrDecisionAlreadyAwaited
	}

	timer := time.NewTimer(maxWallWait)
	defer timer.Stop()
	select {
	case outcome := <-p.pending.result:
		return outcome, outcomeError(outcome)
	case <-timer.C:
		p.coordinator.resolveFallback(p.pending, DecisionTimedOut, "agent wall timeout")
		outcome := <-p.pending.result
		return outcome, outcomeError(outcome)
	case <-ctx.Done():
		p.coordinator.resolveFallback(p.pending, DecisionCancelled, ctx.Err().Error())
		outcome := <-p.pending.result
		if outcome.Resolution == DecisionCancelled {
			return outcome, ctx.Err()
		}
		return outcome, outcomeError(outcome)
	}
}

func outcomeError(outcome DecisionOutcome) error {
	switch outcome.Resolution {
	case DecisionApplied, DecisionTimedOut:
		return nil
	case DecisionCancelled:
		return context.Canceled
	case DecisionClosed:
		return ErrDecisionCoordinatorClosed
	default:
		return fmt.Errorf("unknown decision resolution %q", outcome.Resolution)
	}
}

// DecisionCoordinator owns active barriers only. Long-lived agent state and
// simulation state belong to LangGraph and the C++ SimulationSession.
type DecisionCoordinator struct {
	mu      sync.Mutex
	pending map[string]*pendingDecision
	closed  bool
	clock   func() time.Time
}

func NewDecisionCoordinator() *DecisionCoordinator {
	return &DecisionCoordinator{
		pending: make(map[string]*pendingDecision),
		clock:   time.Now,
	}
}

func (c *DecisionCoordinator) Begin(
	observation DecisionObservation,
	fallback DecisionAction,
) (*PendingDecision, error) {
	if err := validateDecisionObservation(observation); err != nil {
		return nil, err
	}
	if fallback.DecisionID == "" {
		fallback.DecisionID = observation.DecisionID
	}
	if fallback.AgentID == "" {
		fallback.AgentID = observation.AgentID
	}
	if fallback.BasedOnStateVersion == 0 {
		fallback.BasedOnStateVersion = observation.StateVersion
	}
	if fallback.ValidUntilSimulationTime == 0 {
		fallback.ValidUntilSimulationTime = observation.ValidUntilSimulationTime
	}
	if fallback.Kind == "" {
		fallback.Kind = NavigationActionFallback
	}
	if err := validateDecisionAction(fallback); err != nil {
		return nil, fmt.Errorf("invalid fallback: %w", err)
	}
	if fallback.DecisionID != observation.DecisionID ||
		fallback.AgentID != observation.AgentID ||
		fallback.BasedOnStateVersion != observation.StateVersion {
		return nil, errors.New("fallback must target the observation decision, agent and state")
	}

	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return nil, ErrDecisionCoordinatorClosed
	}
	if _, exists := c.pending[observation.DecisionID]; exists {
		return nil, ErrDecisionAlreadyPending
	}
	pending := &pendingDecision{
		observation: observation,
		fallback:    cloneDecisionAction(fallback),
		openedAt:    c.clock(),
		result:      make(chan DecisionOutcome, 1),
	}
	c.pending[observation.DecisionID] = pending
	return &PendingDecision{coordinator: c, pending: pending}, nil
}

// Submit validates the response against both the observed snapshot and the
// authoritative current session state. Invalid responses leave the barrier
// open so the agent may correct them before its wall timeout.
func (c *DecisionCoordinator) Submit(
	action DecisionAction,
	current DecisionState,
) error {
	if err := validateDecisionAction(action); err != nil {
		return err
	}
	if !finiteNonNegative(current.SimulationTimeSeconds) || current.StateVersion == 0 {
		return errors.New("invalid current decision state")
	}

	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return ErrDecisionCoordinatorClosed
	}
	pending, exists := c.pending[action.DecisionID]
	if !exists {
		return ErrDecisionNotPending
	}
	observation := pending.observation
	if action.AgentID != observation.AgentID {
		return ErrDecisionAgentMismatch
	}
	if action.BasedOnStateVersion != observation.StateVersion ||
		current.StateVersion != observation.StateVersion {
		return ErrDecisionStateMismatch
	}
	validUntil := math.Min(
		observation.ValidUntilSimulationTime,
		action.ValidUntilSimulationTime,
	)
	if current.SimulationTimeSeconds > validUntil+1e-9 {
		return ErrDecisionExpired
	}

	delete(c.pending, action.DecisionID)
	pending.result <- DecisionOutcome{
		Observation: observation,
		Action:      cloneDecisionAction(action),
		Resolution:  DecisionApplied,
		Reason:      "action accepted by version and validity guard",
		OpenedAt:    pending.openedAt,
		ResolvedAt:  c.clock(),
	}
	return nil
}

func (c *DecisionCoordinator) resolveFallback(
	pending *pendingDecision,
	resolution DecisionResolution,
	reason string,
) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	current, exists := c.pending[pending.observation.DecisionID]
	if !exists || current != pending {
		return false
	}
	delete(c.pending, pending.observation.DecisionID)
	pending.result <- DecisionOutcome{
		Observation:  pending.observation,
		Action:       cloneDecisionAction(pending.fallback),
		Resolution:   resolution,
		Reason:       reason,
		UsedFallback: true,
		OpenedAt:     pending.openedAt,
		ResolvedAt:   c.clock(),
	}
	return true
}

func (c *DecisionCoordinator) PendingCount() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.pending)
}

func (c *DecisionCoordinator) Close() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return
	}
	c.closed = true
	now := c.clock()
	for id, pending := range c.pending {
		delete(c.pending, id)
		pending.result <- DecisionOutcome{
			Observation:  pending.observation,
			Action:       cloneDecisionAction(pending.fallback),
			Resolution:   DecisionClosed,
			Reason:       ErrDecisionCoordinatorClosed.Error(),
			UsedFallback: true,
			OpenedAt:     pending.openedAt,
			ResolvedAt:   now,
		}
	}
}

func validateDecisionObservation(observation DecisionObservation) error {
	if observation.DecisionID == "" || observation.AgentID == "" ||
		observation.SessionID == "" {
		return errors.New("decision observation requires session, decision and agent IDs")
	}
	if observation.StateVersion == 0 {
		return errors.New("decision observation requires a positive state version")
	}
	if !finiteNonNegative(observation.SimulationTimeSeconds) ||
		!finiteNonNegative(observation.ValidUntilSimulationTime) ||
		observation.ValidUntilSimulationTime < observation.SimulationTimeSeconds {
		return errors.New("invalid decision observation simulation-time window")
	}
	switch observation.Mode {
	case DecisionModeBarrier, DecisionModeLatencySimulation, DecisionModeAsyncThroughput:
		return nil
	default:
		return errors.New("invalid decision mode")
	}
}

func validateDecisionAction(action DecisionAction) error {
	if action.DecisionID == "" || action.AgentID == "" {
		return errors.New("decision action requires decision and agent IDs")
	}
	if action.BasedOnStateVersion == 0 {
		return errors.New("decision action requires a positive state version")
	}
	if !finiteNonNegative(action.ValidUntilSimulationTime) {
		return errors.New("invalid decision action validity time")
	}
	switch action.Kind {
	case NavigationActionKeepRoute, NavigationActionDefer, NavigationActionFallback:
		return nil
	case NavigationActionSelectAlgorithm:
		if action.AlgorithmID == "" {
			return errors.New("select_algorithm requires algorithm ID")
		}
		return nil
	case NavigationActionCommitRoute:
		if action.CandidateID == "" {
			return errors.New("commit_route requires candidate ID")
		}
		return nil
	default:
		return errors.New("invalid navigation action kind")
	}
}

func finiteNonNegative(value float64) bool {
	return !math.IsNaN(value) && !math.IsInf(value, 0) && value >= 0
}

func cloneDecisionAction(action DecisionAction) DecisionAction {
	cloned := action
	if action.Constraints != nil {
		cloned.Constraints = make(map[string]string, len(action.Constraints))
		for key, value := range action.Constraints {
			cloned.Constraints[key] = value
		}
	}
	return cloned
}
