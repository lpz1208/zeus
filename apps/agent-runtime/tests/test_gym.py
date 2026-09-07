"""Tests for the Gymnasium-style adapter (no gymnasium package required)."""

from __future__ import annotations

import pytest

from zeus_agent.gym import ZeusEnv
from zeus_agent.runner import Scenario


def scenario() -> Scenario:
    return Scenario(
        map_id="m1",
        origin=(1.0, 2.0),
        destination=(3.0, 4.0),
        duration_seconds=900.0,
        max_decisions=5,
    )


def make_env(client) -> ZeusEnv:
    return ZeusEnv(client, scenario())


def test_reset_opens_first_decision_and_baselines_reward(fake_client):
    env = make_env(fake_client)

    env.reset()

    # reset advanced to a decision boundary: step() must be callable
    # immediately instead of raising for a missing open decision.
    observation, reward, terminated, truncated, info = env.step(
        "commit_route", candidate_id="cand-astar")

    # reset's advance step consumes the invalidation event (ETA 1200 s),
    # committing the route brings the ETA back to 300 s, and the periodic
    # step boundary does not change it. The reward must be that delta
    # measured against the reset baseline — not the negated total ETA the
    # pre-fix missing baseline produced.
    assert observation.remaining_eta_s == pytest.approx(300.0)
    assert reward == pytest.approx(1200.0 - 300.0)
    assert not terminated
    assert not truncated
    assert info["decision_id"]


def test_re_reset_uses_new_session_decision(fake_client):
    env = make_env(fake_client)
    env.reset()
    env.step("commit_route", candidate_id="cand-astar")

    env.reset()

    # The second episode must resolve the new session's decision, not the
    # stale StepResponse of the previous one; the fake enforces decision
    # state-version consistency, so a stale boundary would raise a 409. The
    # fake's event script holds one final periodic boundary, after which the
    # vehicle arrives.
    observation, _, terminated, _, _ = env.step("keep_route")
    assert observation.state == "arrived"
    assert terminated


def test_close_closes_session_and_is_idempotent(fake_client, fake_environment):
    env = make_env(fake_client)
    env.reset()
    env.close()
    env.close()  # second close must not raise
    assert fake_environment.session_closed
