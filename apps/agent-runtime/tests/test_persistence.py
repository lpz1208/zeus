"""Persistent LangGraph interruption, resume, and audit trace tests."""

from __future__ import annotations

import httpx
import pytest

from zeus_agent.client import HttpEnvironmentClient
from zeus_agent.persistence import DecisionTraceStore
from zeus_agent.runner import Scenario, run_episode

from conftest import FakeEnvironment


def _client(environment: FakeEnvironment) -> HttpEnvironmentClient:
    return HttpEnvironmentClient(
        map_id="m1",
        base_url="http://testserver",
        transport=httpx.MockTransport(environment.handle),
    )


def _scenario() -> Scenario:
    return Scenario(
        map_id="m1",
        origin=(1.0, 2.0),
        destination=(3.0, 4.0),
        duration_seconds=600,
        max_decisions=10,
    )


def test_sqlite_checkpoint_interrupts_and_resumes_without_recreating_session(
    tmp_path,
):
    environment = FakeEnvironment()
    checkpoint = tmp_path / "checkpoints.sqlite"
    audit = tmp_path / "traces.sqlite"

    first = run_episode(
        _client(environment),
        _scenario(),
        checkpoint_path=checkpoint,
        trace_path=audit,
        thread_id="run-resume",
        interrupt_after="observe",
    )
    assert first.interrupted
    assert first.environment_snapshot_id == "snp_test"
    assert environment.snapshot_created
    assert not first.finished
    assert not environment.session_closed
    assert environment.actions == []
    assert sum(
        1 for method, path in environment.requests
        if method == "POST" and path.endswith("/agent/sessions")
    ) == 1

    with DecisionTraceStore(audit) as store:
        assert store.get_run("run-resume")["status"] == "interrupted"
        assert [event["node"] for event in store.list_events("run-resume")] == [
            "advance", "observe",
        ]

    environment.session_lost = True
    resumed = run_episode(
        _client(environment),
        _scenario(),
        checkpoint_path=checkpoint,
        trace_path=audit,
        thread_id="run-resume",
        resume=True,
    )
    assert resumed.resumed
    assert resumed.environment_restored
    assert resumed.environment_snapshot_id == "snp_test"
    assert environment.restores == 1
    assert not resumed.interrupted
    assert resumed.finished and resumed.arrived
    assert environment.session_closed
    assert sum(
        1 for method, path in environment.requests
        if method == "POST" and path.endswith("/agent/sessions")
    ) == 1
    assert environment.actions[0]["decisionId"] == "dec_restored"

    with DecisionTraceStore(audit) as store:
        document = store.export("run-resume")
    assert document["run"]["status"] == "completed"
    assert document["run"]["environment_snapshot_id"] == "snp_test"
    nodes = [event["node"] for event in document["events"]]
    assert nodes[:2] == ["advance", "observe"]
    assert {"decide", "guard", "act"}.issubset(nodes)
    decide = next(event for event in document["events"] if event["node"] == "decide")
    guard = next(event for event in document["events"] if event["node"] == "guard")
    act = next(event for event in document["events"] if event["node"] == "act")
    assert decide["payload"]["decision"]["kind"] == "commit_route"
    assert guard["payload"]["guard_allowed"] is True
    assert act["payload"]["last_action"]["kind"] == "commit_route"
    assert act["payload"]["action_ack"]["accepted"] is True
    assert act["decision_id"] is not None
    keys = [
        (event["checkpoint_step"], event["task_id"])
        for event in document["events"]
    ]
    assert len(keys) == len(set(keys))


def test_persistence_options_reject_plain_loop(tmp_path):
    with pytest.raises(ValueError, match="require LangGraph"):
        run_episode(
            _client(FakeEnvironment()),
            _scenario(),
            use_langgraph=False,
            checkpoint_path=tmp_path / "checkpoint.sqlite",
        )


def test_resume_requires_stable_thread_and_checkpoint():
    with pytest.raises(ValueError, match="resume requires"):
        run_episode(_client(FakeEnvironment()), _scenario(), resume=True)


def test_interrupt_requires_checkpoint():
    with pytest.raises(ValueError, match="interrupt_after requires"):
        run_episode(
            _client(FakeEnvironment()), _scenario(), interrupt_after="observe")


def test_existing_thread_id_must_be_resumed(tmp_path):
    environment = FakeEnvironment()
    checkpoint = tmp_path / "checkpoints.sqlite"
    run_episode(
        _client(environment), _scenario(), checkpoint_path=checkpoint,
        thread_id="stable", interrupt_after="observe")
    with pytest.raises(ValueError, match="already exists"):
        run_episode(
            _client(environment), _scenario(), checkpoint_path=checkpoint,
            thread_id="stable")
    assert sum(
        1 for method, path in environment.requests
        if method == "POST" and path.endswith("/agent/sessions")
    ) == 1
