"""Structured model provider protocol and safety tests."""

from __future__ import annotations

import asyncio
import json
import time

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


def provider(handler, **overrides) -> OpenAICompatibleModelProvider:
    # sleep is injected so retry paths run instantly in tests.
    async def no_sleep(seconds):
        pass
    overrides.setdefault("sleep", no_sleep)
    return OpenAICompatibleModelProvider(
        api_key="secret-test-key",
        model="test-model",
        base_url="https://model.test/v1",
        transport=httpx.MockTransport(handler),
        **overrides,
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


def test_provider_retries_transient_failures_then_succeeds():
    statuses: list[int] = []
    backoffs: list[float] = []

    def handler(_: httpx.Request) -> httpx.Response:
        statuses.append(1)  # count attempts
        if len(statuses) < 3:
            return httpx.Response(500, text="upstream hiccup")
        return httpx.Response(200, json={
            "choices": [{"message": {"content": json.dumps({
                "action": "keep_route",
                "reason_code": "stable",
            })}}],
        })

    async def record_backoff(seconds):
        backoffs.append(seconds)

    result = provider(handler, sleep=record_backoff).decide(request())
    assert result.decision.kind == "keep_route"
    assert len(statuses) == 3
    assert backoffs == [1.0, 2.0]  # exponential backoff, none after success


def test_provider_exhausts_retries_on_persistent_rate_limit():
    attempts: list[int] = []

    def handler(_: httpx.Request) -> httpx.Response:
        attempts.append(1)
        return httpx.Response(429, text="rate limited")

    with pytest.raises(ModelProviderError) as caught:
        provider(handler).decide(request())
    assert "429" in str(caught.value)
    assert len(attempts) == 3  # 1 + max_retries(2)


def test_provider_fails_fast_on_persistent_client_error():
    attempts: list[int] = []

    def handler(_: httpx.Request) -> httpx.Response:
        attempts.append(1)
        return httpx.Response(401, text="bad key")

    with pytest.raises(ModelProviderError, match="401"):
        provider(handler).decide(request())
    assert len(attempts) == 1


def test_provider_repr_hides_api_key():
    def handler(_: httpx.Request) -> httpx.Response:
        return httpx.Response(200, json={
            "choices": [{"message": {"content": json.dumps({
                "action": "keep_route", "reason_code": "stable",
            })}}],
        })

    rendered = repr(provider(handler))
    assert "secret-test-key" not in rendered
    assert "test-model" in rendered


def test_provider_closes_client_after_each_decision():
    closed = []

    class Transport(httpx.MockTransport):
        async def aclose(self):
            closed.append(True)

    configured = OpenAICompatibleModelProvider(
        api_key="test", model="test", transport=Transport(
            lambda _: model_response('{"action":"keep_route","reason_code":"stable"}')))
    for _ in range(2):
        assert configured.decide(request()).decision.kind == "keep_route"
    assert len(closed) == 2


def model_response(content, input_tokens=100, output_tokens=20):
    return httpx.Response(200, json={
        "choices": [{"message": {"content": content}}],
        "usage": {"prompt_tokens": input_tokens, "completion_tokens": output_tokens},
    })


def test_retry_usage_includes_invalid_output_attempt():
    replies = iter([model_response("bad JSON"), model_response(
        '{"action":"keep_route","reason_code":"stable"}')])
    result = provider(lambda _: next(replies)).decide(request())
    assert result.input_tokens == 200
    assert result.output_tokens == 40


def test_exhausted_retry_usage_and_latency_survive_rule_fallback():
    from zeus_agent.graph import make_nodes

    async def handler(_):
        await asyncio.sleep(0.005)
        return model_response("bad JSON")

    req = request()
    state = make_nodes(None, model=provider(handler))["decide"]({
        "observation": req.observation, "candidates": req.candidates,
        "model_input_tokens": 7, "model_output_tokens": 3,
        "model_latency_ms": 10,
    })
    assert state["model_input_tokens"] == 307
    assert state["model_output_tokens"] == 63
    assert state["model_latency_ms"] > 10
    assert state["model_failures"] == 1
    assert state["decision"].kind == "commit_route"  # rule fallback still works


def test_permanent_candidate_error_preserves_usage():
    configured = provider(lambda _: model_response(
        '{"action":"commit_route","candidate_id":"invented","reason_code":"bad"}'))
    with pytest.raises(ModelProviderError) as caught:
        configured.decide(request())
    assert caught.value.input_tokens == 100
    assert caught.value.output_tokens == 20
    assert caught.value.latency_ms > 0


def test_total_deadline_covers_retries_and_cancels_pending_io():
    calls = []
    aborted = []

    async def handler(_):
        calls.append(True)
        if len(calls) == 1:
            await asyncio.sleep(0.01)
            return model_response("bad JSON")
        try:
            await asyncio.sleep(10)
        finally:
            aborted.append(True)

    start = time.monotonic()
    with pytest.raises(ModelProviderError, match="deadline") as caught:
        provider(handler, timeout_seconds=0.08).decide(request())
    assert time.monotonic() - start < 1
    assert len(calls) == 2
    assert aborted == [True]
    assert caught.value.input_tokens == 100
    assert caught.value.latency_ms >= 70


def test_deadline_includes_backoff():
    calls = []
    def handler(_):
        calls.append(True)
        return httpx.Response(500)

    with pytest.raises(ModelProviderError, match="deadline"):
        provider(handler, timeout_seconds=0.03, sleep=asyncio.sleep).decide(request())
    assert len(calls) == 1


@pytest.mark.parametrize("during_backoff", [False, True])
def test_cancel_aborts_pending_request_or_backoff(during_backoff):
    cancel = False
    calls = []
    aborted = []

    async def handler(_):
        nonlocal cancel
        calls.append(True)
        cancel = True
        if during_backoff:
            return httpx.Response(500)
        try:
            await asyncio.sleep(10)
        finally:
            aborted.append(True)

    req = request()
    req.should_cancel = lambda: cancel
    started = time.monotonic()
    with pytest.raises(ModelProviderError, match="cancelled"):
        provider(handler, sleep=asyncio.sleep).decide(req)
    assert time.monotonic() - started < 1
    assert len(calls) == 1
    assert aborted == ([] if during_backoff else [True])


def test_already_cancelled_decision_does_not_call_model():
    def handler(_):
        pytest.fail("cancelled decision issued HTTP request")
    req = request()
    req.should_cancel = lambda: True
    with pytest.raises(ModelProviderError, match="cancelled"):
        provider(handler).decide(req)
