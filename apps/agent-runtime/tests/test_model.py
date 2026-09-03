"""Structured model provider protocol and safety tests."""

from __future__ import annotations

import json

import httpx
import pytest

from zeus_agent.client import RouteCandidate, VehicleObservation
from zeus_agent.model import (
    DecisionRequest,
    ModelProviderError,
    OpenAICompatibleModelProvider,
)


def request() -> DecisionRequest:
    return DecisionRequest(
        observation=VehicleObservation(
            vehicle_id=0,
            state="driving",
            state_version=7,
            tick=10,
            simulation_time_s=10.0,
            decision_reason="route_invalidated",
            remaining_eta_s=900.0,
            route_invalidated=True,
            remaining_edge_ids=list(range(100)),
        ),
        candidates=[
            RouteCandidate(
                candidate_id="cand-a",
                algorithm="astar",
                based_on_state_version=7,
                ok=True,
                time_s=400.0,
                length_m=5000.0,
            )
        ],
    )


def provider(handler) -> OpenAICompatibleModelProvider:
    return OpenAICompatibleModelProvider(
        api_key="secret-test-key",
        model="test-model",
        base_url="https://model.test/v1",
        transport=httpx.MockTransport(handler),
    )


def test_openai_compatible_provider_returns_validated_decision():
    captured: dict = {}

    def handler(http_request: httpx.Request) -> httpx.Response:
        assert http_request.url.path == "/v1/chat/completions"
        assert http_request.headers["authorization"] == "Bearer secret-test-key"
        captured.update(json.loads(http_request.content))
        return httpx.Response(200, json={
            "model": "test-model-2026",
            "choices": [{"message": {"content": json.dumps({
                "action": "commit_route",
                "candidate_id": "cand-a",
                "reason_code": "incident_detour",
                "rationale": "The current route is invalid and this is the only safe candidate.",
            })}}],
            "usage": {"prompt_tokens": 123, "completion_tokens": 17},
        })

    result = provider(handler).decide(request())
    assert result.decision.kind == "commit_route"
    assert result.decision.candidate_id == "cand-a"
    assert result.model == "test-model-2026"
    assert result.input_tokens == 123 and result.output_tokens == 17
    assert result.latency_ms >= 0
    assert captured["response_format"] == {"type": "json_object"}
    prompt = json.loads(captured["messages"][1]["content"])
    assert prompt["observation"]["state_version"] == 7
    assert prompt["observation"]["remaining_route_edge_count"] == 100
    assert prompt["candidates"][0]["candidate_id"] == "cand-a"
    assert "secret-test-key" not in captured["messages"][1]["content"]


def test_provider_rejects_candidate_not_issued_by_environment():
    def handler(_: httpx.Request) -> httpx.Response:
        return httpx.Response(200, json={
            "choices": [{"message": {"content": json.dumps({
                "action": "commit_route",
                "candidate_id": "invented-by-model",
                "reason_code": "unsafe",
            })}}],
        })

    with pytest.raises(ModelProviderError, match="unknown candidate"):
        provider(handler).decide(request())


@pytest.mark.parametrize("content", [
    "not json",
    '{"action":"commit_route","reason_code":"missing_candidate"}',
    '{"action":"keep_route","candidate_id":"cand-a","reason_code":"bad"}',
])
def test_provider_rejects_invalid_structured_output(content: str):
    def handler(_: httpx.Request) -> httpx.Response:
        return httpx.Response(200, json={
            "choices": [{"message": {"content": content}}],
        })

    with pytest.raises(ModelProviderError, match="invalid structured"):
        provider(handler).decide(request())


def test_provider_maps_http_failure_without_exposing_key():
    def handler(_: httpx.Request) -> httpx.Response:
        return httpx.Response(429, text="rate limited")

    with pytest.raises(ModelProviderError) as caught:
        provider(handler).decide(request())
    assert "429" in str(caught.value)
    assert "secret-test-key" not in str(caught.value)
