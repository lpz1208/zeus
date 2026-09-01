package main

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"testing"
	"time"
)

func decisionFixture(id string) (DecisionObservation, DecisionAction) {
	observation := DecisionObservation{
		RunID:                    "run-1",
		SessionID:                "session-1",
		DecisionID:               id,
		AgentID:                  "regional-agent-1",
		Tick:                     120,
		SimulationTimeSeconds:    120,
		StateVersion:             1842,
		ValidUntilSimulationTime: 135,
		Mode:                     DecisionModeBarrier,
	}
	fallback := DecisionAction{
		Kind:       NavigationActionFallback,
		ReasonCode: "dynamic_astar",
	}
	return observation, fallback
}

func TestDecisionCoordinatorAcceptsCurrentAction(t *testing.T) {
	coordinator := NewDecisionCoordinator()
	defer coordinator.Close()
	openedAt := time.Unix(100, 0)
	resolvedAt := openedAt.Add(1750 * time.Millisecond)
	now := openedAt
	coordinator.clock = func() time.Time { return now }

	observation, fallback := decisionFixture("decision-current")
	pending, err := coordinator.Begin(observation, fallback)
	if err != nil {
		t.Fatal(err)
	}
	action := DecisionAction{
		DecisionID:               observation.DecisionID,
		AgentID:                  observation.AgentID,
		BasedOnStateVersion:      observation.StateVersion,
		ValidUntilSimulationTime: 130,
		Kind:                     NavigationActionSelectAlgorithm,
		AlgorithmID:              "time_dependent_astar",
		Constraints:              map[string]string{"max_detour_ratio": "1.15"},
	}
	now = resolvedAt
	if err := coordinator.Submit(action, DecisionState{
		SimulationTimeSeconds: 120,
		StateVersion:          observation.StateVersion,
	}); err != nil {
		t.Fatal(err)
	}
	// Mutating the caller's map must not mutate the accepted decision trace.
	action.Constraints["max_detour_ratio"] = "9"

	outcome, err := pending.Wait(context.Background(), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if outcome.Resolution != DecisionApplied || outcome.UsedFallback ||
		outcome.Action.AlgorithmID != "time_dependent_astar" ||
		outcome.Action.Constraints["max_detour_ratio"] != "1.15" {
		t.Fatalf("unexpected decision outcome: %#v", outcome)
	}
	if outcome.WallLatency() != 1750*time.Millisecond {
		t.Fatalf("unexpected wall latency: %s", outcome.WallLatency())
	}
	if coordinator.PendingCount() != 0 {
		t.Fatal("accepted decision must leave no pending barrier")
	}
}

func TestDecisionCoordinatorEffectFailureLeavesBarrierOpen(t *testing.T) {
	coordinator := NewDecisionCoordinator()
	defer coordinator.Close()
	observation, fallback := decisionFixture("decision-effect-retry")
	pending, err := coordinator.Begin(observation, fallback)
	if err != nil {
		t.Fatal(err)
	}
	action := DecisionAction{
		DecisionID:               observation.DecisionID,
		AgentID:                  observation.AgentID,
		BasedOnStateVersion:      observation.StateVersion,
		ValidUntilSimulationTime: observation.ValidUntilSimulationTime,
		Kind:                     NavigationActionCommitRoute,
		CandidateID:              "candidate-1",
	}
	applyErr := errors.New("worker rejected candidate")
	if err := coordinator.SubmitWithEffect(action, DecisionState{
		SimulationTimeSeconds: observation.SimulationTimeSeconds,
		StateVersion:          observation.StateVersion,
	}, func() error { return applyErr }); !errors.Is(err, applyErr) {
		t.Fatalf("expected worker failure, got %v", err)
	}
	if coordinator.PendingCount() != 1 {
		t.Fatal("failed environment effect must leave the decision pending")
	}
	action.Kind = NavigationActionKeepRoute
	action.CandidateID = ""
	if err := coordinator.SubmitWithEffect(action, DecisionState{
		SimulationTimeSeconds: observation.SimulationTimeSeconds,
		StateVersion:          observation.StateVersion,
	}, func() error { return nil }); err != nil {
		t.Fatal(err)
	}
	outcome, err := pending.Wait(context.Background(), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if outcome.Resolution != DecisionApplied || outcome.Action.Kind != NavigationActionKeepRoute {
		t.Fatalf("unexpected retry outcome: %#v", outcome)
	}
}

func TestDecisionCoordinatorTimeoutDoesNotRaceInFlightEffect(t *testing.T) {
	coordinator := NewDecisionCoordinator()
	defer coordinator.Close()
	observation, fallback := decisionFixture("decision-effect-timeout")
	pending, err := coordinator.Begin(observation, fallback)
	if err != nil {
		t.Fatal(err)
	}
	action := DecisionAction{
		DecisionID:               observation.DecisionID,
		AgentID:                  observation.AgentID,
		BasedOnStateVersion:      observation.StateVersion,
		ValidUntilSimulationTime: observation.ValidUntilSimulationTime,
		Kind:                     NavigationActionKeepRoute,
	}
	effectStarted := make(chan struct{})
	releaseEffect := make(chan struct{})
	submitDone := make(chan error, 1)
	go func() {
		submitDone <- coordinator.SubmitWithEffect(action, DecisionState{
			SimulationTimeSeconds: observation.SimulationTimeSeconds,
			StateVersion:          observation.StateVersion,
		}, func() error {
			close(effectStarted)
			<-releaseEffect
			return nil
		})
	}()
	<-effectStarted
	waitDone := make(chan DecisionOutcome, 1)
	go func() {
		outcome, waitErr := pending.Wait(context.Background(), 10*time.Millisecond)
		if waitErr != nil {
			waitDone <- DecisionOutcome{Reason: waitErr.Error()}
			return
		}
		waitDone <- outcome
	}()
	time.Sleep(20 * time.Millisecond)
	close(releaseEffect)
	if err := <-submitDone; err != nil {
		t.Fatal(err)
	}
	outcome := <-waitDone
	if outcome.Resolution != DecisionApplied || outcome.UsedFallback {
		t.Fatalf("an accepted in-flight effect must win timeout deferral: %#v", outcome)
	}
}

func TestDecisionCoordinatorRejectsStaleThenAcceptsCorrection(t *testing.T) {
	coordinator := NewDecisionCoordinator()
	defer coordinator.Close()
	observation, fallback := decisionFixture("decision-stale")
	pending, err := coordinator.Begin(observation, fallback)
	if err != nil {
		t.Fatal(err)
	}
	action := DecisionAction{
		DecisionID:               observation.DecisionID,
		AgentID:                  observation.AgentID,
		BasedOnStateVersion:      observation.StateVersion,
		ValidUntilSimulationTime: observation.ValidUntilSimulationTime,
		Kind:                     NavigationActionKeepRoute,
	}

	if err := coordinator.Submit(action, DecisionState{
		SimulationTimeSeconds: 121,
		StateVersion:          observation.StateVersion + 1,
	}); !errors.Is(err, ErrDecisionStateMismatch) {
		t.Fatalf("expected stale current state rejection, got %v", err)
	}
	if coordinator.PendingCount() != 1 {
		t.Fatal("a rejected response must leave the barrier open for correction")
	}
	action.BasedOnStateVersion--
	if err := coordinator.Submit(action, DecisionState{
		SimulationTimeSeconds: 121,
		StateVersion:          observation.StateVersion,
	}); !errors.Is(err, ErrDecisionStateMismatch) {
		t.Fatalf("expected stale response rejection, got %v", err)
	}
	action.BasedOnStateVersion = observation.StateVersion
	if err := coordinator.Submit(action, DecisionState{
		SimulationTimeSeconds: 121,
		StateVersion:          observation.StateVersion,
	}); err != nil {
		t.Fatal(err)
	}
	if _, err := pending.Wait(context.Background(), time.Second); err != nil {
		t.Fatal(err)
	}
}

func TestDecisionCoordinatorRejectsExpiredAndWrongAgent(t *testing.T) {
	coordinator := NewDecisionCoordinator()
	defer coordinator.Close()
	observation, fallback := decisionFixture("decision-invalid")
	pending, err := coordinator.Begin(observation, fallback)
	if err != nil {
		t.Fatal(err)
	}
	action := DecisionAction{
		DecisionID:               observation.DecisionID,
		AgentID:                  "another-agent",
		BasedOnStateVersion:      observation.StateVersion,
		ValidUntilSimulationTime: observation.ValidUntilSimulationTime,
		Kind:                     NavigationActionKeepRoute,
	}
	current := DecisionState{
		SimulationTimeSeconds: 136,
		StateVersion:          observation.StateVersion,
	}
	if err := coordinator.Submit(action, current); !errors.Is(err, ErrDecisionAgentMismatch) {
		t.Fatalf("expected agent mismatch, got %v", err)
	}
	action.AgentID = observation.AgentID
	if err := coordinator.Submit(action, current); !errors.Is(err, ErrDecisionExpired) {
		t.Fatalf("expected expired response, got %v", err)
	}

	coordinator.Close()
	outcome, err := pending.Wait(context.Background(), time.Second)
	if !errors.Is(err, ErrDecisionCoordinatorClosed) ||
		outcome.Resolution != DecisionClosed || !outcome.UsedFallback {
		t.Fatalf("unexpected close outcome: %#v, %v", outcome, err)
	}
}

func TestDecisionCoordinatorTimeoutUsesFallback(t *testing.T) {
	coordinator := NewDecisionCoordinator()
	defer coordinator.Close()
	observation, fallback := decisionFixture("decision-timeout")
	pending, err := coordinator.Begin(observation, fallback)
	if err != nil {
		t.Fatal(err)
	}
	outcome, err := pending.Wait(context.Background(), 10*time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	if outcome.Resolution != DecisionTimedOut || !outcome.UsedFallback ||
		outcome.Action.DecisionID != observation.DecisionID ||
		outcome.Action.AgentID != observation.AgentID ||
		outcome.Action.BasedOnStateVersion != observation.StateVersion ||
		outcome.Action.ReasonCode != "dynamic_astar" {
		t.Fatalf("unexpected timeout outcome: %#v", outcome)
	}
	if coordinator.PendingCount() != 0 {
		t.Fatal("timed-out decision must be removed")
	}
}

func TestDecisionCoordinatorCancellationAndDuplicateWait(t *testing.T) {
	coordinator := NewDecisionCoordinator()
	defer coordinator.Close()
	observation, fallback := decisionFixture("decision-cancel")
	pending, err := coordinator.Begin(observation, fallback)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	outcome, err := pending.Wait(ctx, time.Second)
	if !errors.Is(err, context.Canceled) || outcome.Resolution != DecisionCancelled ||
		!outcome.UsedFallback {
		t.Fatalf("unexpected cancellation outcome: %#v, %v", outcome, err)
	}
	if _, err := pending.Wait(context.Background(), time.Second); !errors.Is(err, ErrDecisionAlreadyAwaited) {
		t.Fatalf("expected duplicate wait rejection, got %v", err)
	}
}

func TestDecisionCoordinatorConcurrentBarriers(t *testing.T) {
	coordinator := NewDecisionCoordinator()
	defer coordinator.Close()
	const decisions = 128
	waiters := make([]*PendingDecision, decisions)
	observations := make([]DecisionObservation, decisions)
	for i := 0; i < decisions; i++ {
		observation, fallback := decisionFixture(fmt.Sprintf("decision-%03d", i))
		observation.AgentID = fmt.Sprintf("agent-%03d", i)
		observations[i] = observation
		pending, err := coordinator.Begin(observation, fallback)
		if err != nil {
			t.Fatal(err)
		}
		waiters[i] = pending
	}

	var submitWG sync.WaitGroup
	for i := range observations {
		submitWG.Add(1)
		go func(observation DecisionObservation) {
			defer submitWG.Done()
			action := DecisionAction{
				DecisionID:               observation.DecisionID,
				AgentID:                  observation.AgentID,
				BasedOnStateVersion:      observation.StateVersion,
				ValidUntilSimulationTime: observation.ValidUntilSimulationTime,
				Kind:                     NavigationActionKeepRoute,
			}
			if err := coordinator.Submit(action, DecisionState{
				SimulationTimeSeconds: observation.SimulationTimeSeconds,
				StateVersion:          observation.StateVersion,
			}); err != nil {
				t.Errorf("submit %s: %v", observation.DecisionID, err)
			}
		}(observations[i])
	}
	submitWG.Wait()

	for _, pending := range waiters {
		outcome, err := pending.Wait(context.Background(), time.Second)
		if err != nil || outcome.Resolution != DecisionApplied {
			t.Fatalf("unexpected concurrent result: %#v, %v", outcome, err)
		}
	}
	if coordinator.PendingCount() != 0 {
		t.Fatalf("pending decisions leaked: %d", coordinator.PendingCount())
	}
}

func TestDecisionCoordinatorValidatesInputsAndDuplicates(t *testing.T) {
	coordinator := NewDecisionCoordinator()
	defer coordinator.Close()
	observation, fallback := decisionFixture("decision-duplicate")
	if _, err := coordinator.Begin(observation, fallback); err != nil {
		t.Fatal(err)
	}
	if _, err := coordinator.Begin(observation, fallback); !errors.Is(err, ErrDecisionAlreadyPending) {
		t.Fatalf("expected duplicate rejection, got %v", err)
	}
	invalid := observation
	invalid.ValidUntilSimulationTime = invalid.SimulationTimeSeconds - 1
	if _, err := coordinator.Begin(invalid, fallback); err == nil {
		t.Fatal("expected invalid simulation-time window to fail")
	}
	if err := coordinator.Submit(DecisionAction{
		DecisionID:               observation.DecisionID,
		AgentID:                  observation.AgentID,
		BasedOnStateVersion:      observation.StateVersion,
		ValidUntilSimulationTime: observation.ValidUntilSimulationTime,
		Kind:                     NavigationActionCommitRoute,
	}, DecisionState{
		SimulationTimeSeconds: observation.SimulationTimeSeconds,
		StateVersion:          observation.StateVersion,
	}); err == nil {
		t.Fatal("commit_route without an issued candidate must fail validation")
	}
}
