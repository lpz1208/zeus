"""Client wire-format tests against the mock transport."""

from __future__ import annotations

import json

import httpx
import pytest

from zeus_agent.client import (
    ActionRequest,
    CreateSessionRequest,
    AgentVehicleSpec,
    EnvironmentError,
    RoadControl,
)

from conftest import FakeEnvironment


def make_client(environment: FakeEnvironment):
    import zeus_agent.client as client_module
    return client_module.HttpEnvironmentClient(
        map_id="m1", base_url="http://testserver",
        transport=httpx.MockTransport(environment.handle))


def test_tools_registry_parses(fake_client):
    registry = fake_client.tools()
    assert registry.registry_version == "routing-tools-v1"
    assert [a.algorithm_id for a in registry.algorithms] == ["dijkstra", "astar"]


def test_create_session_sends_camel_case(fake_environment):
    client = make_client(fake_environment)
    request = CreateSessionRequest(
        vehicles=[AgentVehicleSpec(
            from_lon=1.5, from_lat=2.5, to_lon=3.5, to_lat=4.5, agent=True)],
        duration_seconds=600,
        road_controls=[RoadControl(time_seconds=5.0, edge_ids=[7], action="close")],
    )
    created = client.create_session(request)
    assert created.session_id == "ses_test"
    assert created.agents == [0]
    sent = fake_environment.requests
    assert ("POST", "/api/maps/m1/agent/sessions") in sent


def test_observe_parses_agent_objects_and_vehicle_position(fake_client):
    fake_client.create_session(CreateSessionRequest(
        vehicles=[AgentVehicleSpec(from_lon=1, from_lat=2, to_lon=3, to_lat=4, agent=True)]))
    observation = fake_client.observe("ses_test")
    assert observation.counts.driving == 1
    assert observation.agents and observation.agents[0].vehicle_id == 0
    vehicle = fake_client.observe_vehicle("ses_test", 0)
    assert vehicle.position.edge_id == 4294967295  # waiting: invalid edge
    assert vehicle.remaining_eta_s == 900.0


def test_step_until_event_returns_nested_state_and_decision(fake_client):
    fake_client.create_session(CreateSessionRequest(
        vehicles=[AgentVehicleSpec(from_lon=1, from_lat=2, to_lon=3, to_lat=4, agent=True)]))
    response = fake_client.step_until_event("ses_test")
    assert response.decision_id == "dec_2"
    assert response.state.decision_due
    assert response.state.decision_reason == "route_invalidated"


def test_plan_candidate_round_trip(fake_client):
    fake_client.create_session(CreateSessionRequest(
        vehicles=[AgentVehicleSpec(from_lon=1, from_lat=2, to_lon=3, to_lat=4, agent=True)]))
    fake_client.step_until_event("ses_test")
    candidate = fake_client.plan("ses_test", 0, "astar")
    assert candidate.ok and candidate.candidate_id == "cand-astar"
    assert candidate.based_on_state_version == 2
    assert candidate.time_s == 300.0


def test_action_request_uses_camel_aliases(fake_environment):
    client = make_client(fake_environment)
    client.create_session(CreateSessionRequest(
        vehicles=[AgentVehicleSpec(from_lon=1, from_lat=2, to_lon=3, to_lat=4, agent=True)]))
    client.step_until_event("ses_test")
    # Intercept the raw JSON to assert wire keys.
    raw: dict = {}

    def capture(request: httpx.Request) -> httpx.Response:
        raw.update(json.loads(request.content))
        return httpx.Response(200, json={"accepted": True, "reason": "",
                                         "appliesAtNextTick": True})

    client._client._transport = httpx.MockTransport(capture)  # noqa: SLF001
    ack = client.submit_action("ses_test", ActionRequest(
        decision_id="dec_2", vehicle_id=0, kind="keep_route",
        based_on_state_version=2, reason_code="test"))
    assert ack.accepted
    assert raw["decisionId"] == "dec_2"
    assert raw["basedOnStateVersion"] == 2
    assert raw["kind"] == "keep_route"


def test_error_mapping(fake_environment):
    client = make_client(fake_environment)
    with pytest.raises(EnvironmentError) as excinfo:
        client.submit_action("ses_test", ActionRequest(
            decision_id="dec_x", vehicle_id=0, kind="keep_route",
            based_on_state_version=99))
    assert excinfo.value.status_code == 409
    assert "stale" in excinfo.value.message


def test_close(fake_environment):
    client = make_client(fake_environment)
    client.create_session(CreateSessionRequest(
        vehicles=[AgentVehicleSpec(from_lon=1, from_lat=2, to_lon=3, to_lat=4, agent=True)]))
    client.close("ses_test")
    assert fake_environment.session_closed
