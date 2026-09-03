"""Guard rule tests."""

from __future__ import annotations

from zeus_agent.client import RouteCandidate, VehicleObservation
from zeus_agent.graph import GuardConfig, evaluate_guard
from zeus_agent.policy import Decision


def observation(*, invalidated: bool = False, eta: float = 1000.0,
                version: int = 7) -> VehicleObservation:
    return VehicleObservation(
        vehicle_id=0, state="driving", remaining_eta_s=eta,
        route_invalidated=invalidated, state_version=version, tick=10)


def candidate(cid: str, time_s: float, version: int = 7) -> RouteCandidate:
    return RouteCandidate(
        candidate_id=cid, ok=True, time_s=time_s, length_m=100.0,
        based_on_state_version=version)


CONFIG = GuardConfig(min_improvement_ratio=0.10, cooldown_decisions=1)


def test_keep_route_always_allowed():
    verdict = evaluate_guard(
        Decision("keep_route", reason="r"), observation(), [], 0, 0, CONFIG)
    assert verdict.allowed


def test_stale_state_version_rejected():
    verdict = evaluate_guard(
        Decision("commit_route", candidate_id="c1", reason="r"),
        observation(version=8), [candidate("c1", 100.0, version=7)],
        0, 0, CONFIG)
    assert not verdict.allowed
    assert verdict.reason == "stale_state_version"


def test_below_min_improvement_rejected():
    verdict = evaluate_guard(
        Decision("commit_route", candidate_id="c1", reason="r"),
        observation(eta=1000.0), [candidate("c1", 950.0)],
        0, 0, CONFIG)
    assert not verdict.allowed
    assert verdict.reason == "below_min_improvement"


def test_invalidation_bypasses_improvement_but_not_version():
    verdict = evaluate_guard(
        Decision("commit_route", candidate_id="c1", reason="r"),
        observation(invalidated=True), [candidate("c1", 2000.0)],
        0, 0, CONFIG)
    assert verdict.allowed
    stale = evaluate_guard(
        Decision("commit_route", candidate_id="c1", reason="r"),
        observation(invalidated=True, version=9), [candidate("c1", 100.0, version=7)],
        0, 0, CONFIG)
    assert not stale.allowed
    assert stale.reason == "stale_state_version"


def test_cooldown_only_between_voluntary_switches():
    # First commit ever: allowed even at decisions_since_commit = 0.
    first = evaluate_guard(
        Decision("commit_route", candidate_id="c1", reason="r"),
        observation(), [candidate("c1", 500.0)], 0, 0, CONFIG)
    assert first.allowed
    # A voluntary switch immediately after a previous commit: rejected.
    second = evaluate_guard(
        Decision("commit_route", candidate_id="c1", reason="r"),
        observation(), [candidate("c1", 500.0)], 1, 0, CONFIG)
    assert not second.allowed
    assert second.reason == "commit_cooldown"
    # ...unless the route was invalidated.
    forced = evaluate_guard(
        Decision("commit_route", candidate_id="c1", reason="r"),
        observation(invalidated=True), [candidate("c1", 2000.0)],
        1, 0, CONFIG)
    assert forced.allowed


def test_unknown_candidate_rejected():
    verdict = evaluate_guard(
        Decision("commit_route", candidate_id="ghost", reason="r"),
        observation(), [candidate("c1", 500.0)], 0, 0, CONFIG)
    assert not verdict.allowed
    assert verdict.reason == "candidate_not_in_set"
