"""Full-episode loop tests on the fake transport."""

from __future__ import annotations

import httpx
import pytest

from zeus_agent.client import (
    CreateSessionRequest,
    HttpEnvironmentClient,
    AgentVehicleSpec,
    RoadControl,
)
from zeus_agent.policy import RulePolicy
from zeus_agent.model import MockModelProvider, ModelProviderError
from zeus_agent.policy import Decision
from zeus_agent.runner import Scenario, run_episode

from conftest import FakeEnvironment


def scenario() -> Scenario:
    return Scenario(
        map_id="m1",
        origin=(1.0, 2.0),
        destination=(3.0, 4.0),
        duration_seconds=600,
        road_controls=(RoadControl(time_seconds=5.0, edge_ids=[7], action="close"),),
        max_decisions=10,
    )


def client_for(environment: FakeEnvironment) -> HttpEnvironmentClient:
    return HttpEnvironmentClient(
        map_id="m1", base_url="http://testserver",
        transport=httpx.MockTransport(environment.handle))


def test_episode_commits_after_invalidation_and_arrives(fake_client):
    trace = run_episode(fake_client, scenario(), RulePolicy())
    assert trace.finished
    assert trace.arrived
    assert trace.route_invalidated_events >= 1
    assert trace.commits >= 1
    assert trace.error is None
    assert trace.final_observation is not None
    assert trace.final_observation.route_invalidated is False
    assert trace.execution_mode == "langgraph"


def test_episode_route_changed_across_commit(fake_environment, fake_client):
    run_episode(fake_client, scenario(), RulePolicy())
    commit_actions = [a for a in fake_environment.actions
                      if a.get("kind") == "commit_route"]
    assert commit_actions, "no commit action reached the environment"
    assert commit_actions[0]["candidateId"].startswith("cand-")


def test_episode_falls_back_to_keep_on_conflict():
    environment = FakeEnvironment(fail_first_commit=True)
    trace = run_episode(client_for(environment), scenario(), RulePolicy())
    assert trace.fallbacks == 1
    # The fallback keep unblocks the barrier and the episode still finishes.
    assert trace.finished
    assert trace.arrived
    keeps = [a for a in environment.actions if a.get("kind") == "keep_route"]
    assert any(a.get("reasonCode") == "agent_fallback_keep" for a in keeps)


def test_episode_always_closes_the_session(fake_environment):
    trace = run_episode(client_for(fake_environment), scenario(), RulePolicy())
    assert fake_environment.session_closed
    assert trace.error is None


def test_langgraph_optional_execution(fake_client):
    pytest.importorskip("langgraph")
    from zeus_agent.graph import build_graph, make_nodes
    nodes = make_nodes(fake_client, RulePolicy())
    graph = build_graph(nodes)
    fake_client.create_session(CreateSessionRequest(
        vehicles=[AgentVehicleSpec(from_lon=1, from_lat=2, to_lon=3, to_lat=4,
                                   agent=True)]))
    state = {"session_id": "ses_test", "vehicle_id": 0}
    result = graph.invoke(state, {"recursion_limit": 50})
    assert result["done"] or result.get("iterations", 0) > 0


def test_episode_model_provider_owns_decision_node(fake_environment, fake_client):
    model = MockModelProvider(scripted={
        "route_invalidated": Decision(
            "commit_route", candidate_id="cand-dijkstra", reason="model_detour"),
    })
    trace = run_episode(fake_client, scenario(), model=model)
    commits = [action for action in fake_environment.actions
               if action.get("kind") == "commit_route"]
    assert commits[0]["candidateId"] == "cand-dijkstra"
    assert trace.model_calls >= 1
    assert trace.model_name == "mock"
    assert any("decide:model:mock" in event for event in trace.graph_events)


def test_model_failure_uses_rule_policy_without_stranding_barrier(fake_client):
    class FailingModel:
        def decide(self, request):
            del request
            raise ModelProviderError("provider unavailable")

    trace = run_episode(fake_client, scenario(), model=FailingModel())
    assert trace.finished and trace.arrived
    assert trace.commits >= 1
    assert trace.model_calls == trace.model_failures
    assert trace.model_failures >= 1
    assert trace.model_error == "provider unavailable"
    assert trace.error is None


def test_episode_can_explicitly_use_plain_loop(fake_client):
    trace = run_episode(fake_client, scenario(), use_langgraph=False)
    assert trace.execution_mode == "plain_loop"
    assert trace.finished and trace.arrived
