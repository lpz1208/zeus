"""RulePolicy behaviour tests."""

from __future__ import annotations

from zeus_agent.client import RouteCandidate, VehicleObservation
from zeus_agent.policy import RulePolicy, RulePolicyConfig


def observation(*, invalidated: bool = False, eta: float = 1000.0) -> VehicleObservation:
    return VehicleObservation(
        vehicle_id=0, state="driving", remaining_eta_s=eta,
        route_invalidated=invalidated, state_version=7, tick=10,
        remaining_edge_ids=[1, 2, 3])


def candidate(cid: str, time_s: float, length_m: float = 100.0, *, ok: bool = True,
              version: int = 7) -> RouteCandidate:
    return RouteCandidate(
        candidate_id=cid, ok=ok, time_s=time_s if ok else None,
        length_m=length_m, based_on_state_version=version)


def test_keep_when_no_valid_candidate():
    decision = RulePolicy().decide(
        observation(), [candidate("bad", 0, ok=False)], None)
    assert decision.kind == "keep_route"
    assert decision.reason == "no_valid_candidate"


def test_commit_when_route_invalidated():
    decision = RulePolicy().decide(
        observation(invalidated=True), [candidate("c1", 500.0)], None)
    assert decision.kind == "commit_route"
    assert decision.candidate_id == "c1"
    assert decision.reason == "route_invalidated"


def test_keep_within_threshold():
    # best 950s vs eta 1000s: below the 10% improvement bar.
    decision = RulePolicy().decide(observation(), [candidate("c1", 950.0)], None)
    assert decision.kind == "keep_route"
    assert decision.reason == "within_threshold"


def test_commit_beyond_threshold():
    decision = RulePolicy().decide(observation(), [candidate("c1", 880.0)], None)
    assert decision.kind == "commit_route"
    assert decision.candidate_id == "c1"


def test_threshold_is_configurable():
    policy = RulePolicy(RulePolicyConfig(eta_improvement_threshold=0.02))
    decision = policy.decide(observation(), [candidate("c1", 950.0)], None)
    assert decision.kind == "commit_route"  # 5% > 2%


def test_best_candidate_deterministic_tiebreak():
    best = RulePolicy.best_candidate([
        candidate("b", 300.0, length_m=50.0),
        candidate("a", 300.0, length_m=40.0),
        candidate("c", 200.0),
    ])
    assert best.candidate_id == "c"
    same_time = [
        candidate("b", 300.0, length_m=50.0),
        candidate("a", 300.0, length_m=40.0),
    ]
    assert RulePolicy.best_candidate(same_time).candidate_id == "a"
    same_all = [candidate("y", 300.0, 40.0), candidate("x", 300.0, 40.0)]
    assert RulePolicy.best_candidate(same_all).candidate_id == "x"
