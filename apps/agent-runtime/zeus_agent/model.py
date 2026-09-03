"""ModelProvider seam and a structured OpenAI-compatible HTTP adapter.

The provider is deliberately synchronous: a Zeus decision boundary freezes
simulation time while the LangGraph node waits. Provider latency is measured
as wall time and never advances the C++ environment clock. Model output can
only select one of the candidate ids supplied by the environment; the
deterministic Action Guard remains authoritative.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from typing import Literal, Mapping, Protocol, Sequence

import httpx
from pydantic import BaseModel, ConfigDict, Field, ValidationError, model_validator

from zeus_agent.client import (
    AgentToolRegistry,
    RouteCandidate,
    VehicleObservation,
)
from zeus_agent.policy import Decision, Policy, RulePolicy


@dataclass
class DecisionRequest:
    observation: VehicleObservation
    candidates: Sequence[RouteCandidate]
    tools: AgentToolRegistry | None = None
    context: str = ""


@dataclass
class DecisionResponse:
    decision: Decision
    rationale: str = ""
    model: str = ""
    latency_ms: float = 0.0
    input_tokens: int = 0
    output_tokens: int = 0


class ModelProvider(Protocol):
    def decide(self, request: DecisionRequest) -> DecisionResponse: ...


class ModelProviderError(RuntimeError):
    """Provider transport, protocol or structured-output failure."""


class _DecisionEnvelope(BaseModel):
    model_config = ConfigDict(extra="forbid")

    action: Literal["keep_route", "commit_route"]
    candidate_id: str | None = None
    reason_code: str = Field(min_length=1, max_length=80)
    rationale: str = Field(default="", max_length=600)

    @model_validator(mode="after")
    def validate_candidate(self) -> _DecisionEnvelope:
        if self.action == "commit_route" and not self.candidate_id:
            raise ValueError("commit_route requires candidate_id")
        if self.action == "keep_route" and self.candidate_id is not None:
            raise ValueError("keep_route must not contain candidate_id")
        return self


def _decision_payload(request: DecisionRequest) -> dict:
    """Bounded, provider-neutral prompt payload; no raw world dump."""
    observation = request.observation
    return {
        "task": (
            "Select one environment-issued route candidate or keep the current "
            "route. Never invent a candidate id. Prefer stability unless the "
            "route is invalid or the ETA improvement is material."
        ),
        "observation": {
            "tick": observation.tick,
            "simulation_time_s": observation.simulation_time_s,
            "state_version": observation.state_version,
            "decision_reason": observation.decision_reason,
            "vehicle_state": observation.state,
            "current_edge_id": observation.position.edge_id,
            "destination_edge_id": observation.destination_edge_id,
            "remaining_eta_s": observation.remaining_eta_s,
            "route_invalidated": observation.route_invalidated,
            "remaining_route_edge_count": len(observation.remaining_edge_ids),
            "active_events": [
                {
                    "type": event.event_type,
                    "affected_edge_ids": event.affected_edge_ids[:16],
                }
                for event in observation.active_events[:16]
            ],
            "nearby_roads": [
                {
                    "edge_id": road.edge_id,
                    "speed_mps": road.speed_mps,
                    "occupancy_ratio": road.occupancy_ratio,
                    "closed": road.closed,
                }
                for road in observation.nearby_roads[:32]
            ],
        },
        "candidates": [
            {
                "candidate_id": candidate.candidate_id,
                "algorithm": candidate.algorithm,
                "effective_algorithm": candidate.effective_algorithm,
                "based_on_state_version": candidate.based_on_state_version,
                "ok": candidate.ok,
                "time_s": candidate.time_s,
                "length_m": candidate.length_m,
                "expanded_nodes": candidate.expanded_nodes,
                "failure_reason": candidate.reason,
            }
            for candidate in request.candidates[:16]
        ],
        "tools": [
            {
                "algorithm_id": capability.algorithm_id,
                "supports_dynamic_weights": capability.supports_dynamic_weights,
                "supports_incremental_repair": capability.supports_incremental_repair,
                "supports_k_candidates": capability.supports_k_candidates,
                "supports_time_dependency": capability.supports_time_dependency,
                "deterministic": capability.deterministic,
                "exact": capability.exact,
            }
            for capability in (request.tools.algorithms if request.tools else [])
        ],
        "context": request.context[:1000],
        "response_schema": {
            "action": "keep_route | commit_route",
            "candidate_id": "required only for commit_route",
            "reason_code": "short machine-readable code",
            "rationale": "brief decision summary, at most 600 characters",
        },
    }


@dataclass
class OpenAICompatibleModelProvider:
    """Strict JSON decision provider over a Chat Completions-compatible API.

    This adapter intentionally depends only on httpx. It works with providers
    that accept ``POST /chat/completions`` and return
    ``choices[0].message.content``. Secrets are injected by the caller and are
    never read from project files or written to traces.
    """

    api_key: str
    model: str
    base_url: str = "https://api.openai.com/v1"
    timeout_seconds: float = 60.0
    temperature: float = 0.0
    transport: httpx.BaseTransport | None = None

    def decide(self, request: DecisionRequest) -> DecisionResponse:
        allowed_candidates = {
            candidate.candidate_id
            for candidate in request.candidates
            if candidate.ok
        }
        started = time.monotonic()
        try:
            with httpx.Client(
                base_url=self.base_url.rstrip("/"),
                timeout=self.timeout_seconds,
                transport=self.transport,
            ) as client:
                response = client.post(
                    "/chat/completions",
                    headers={
                        "Authorization": f"Bearer {self.api_key}",
                        "Content-Type": "application/json",
                    },
                    json={
                        "model": self.model,
                        "temperature": self.temperature,
                        "response_format": {"type": "json_object"},
                        "messages": [
                            {
                                "role": "system",
                                "content": (
                                    "You are a navigation decision policy. Return one "
                                    "JSON object only. You may select only a candidate_id "
                                    "present in the request. Environment guards are final."
                                ),
                            },
                            {
                                "role": "user",
                                "content": json.dumps(
                                    _decision_payload(request),
                                    ensure_ascii=False,
                                    separators=(",", ":"),
                                ),
                            },
                        ],
                    },
                )
        except httpx.HTTPError as error:
            raise ModelProviderError(f"model transport failed: {error}") from error
        latency_ms = (time.monotonic() - started) * 1000.0
        if response.status_code >= 400:
            raise ModelProviderError(
                f"model HTTP {response.status_code}: {response.text[:300]}")
        try:
            body = response.json()
            raw = body["choices"][0]["message"]["content"]
            if not isinstance(raw, str):
                raise TypeError("message content is not text")
            envelope = _DecisionEnvelope.model_validate_json(raw)
        except (KeyError, IndexError, TypeError, ValueError, ValidationError) as error:
            raise ModelProviderError(f"invalid structured model response: {error}") from error
        if (
            envelope.action == "commit_route"
            and envelope.candidate_id not in allowed_candidates
        ):
            raise ModelProviderError(
                f"model selected unknown candidate {envelope.candidate_id!r}")
        usage = body.get("usage") or {}
        return DecisionResponse(
            decision=Decision(
                kind=envelope.action,
                candidate_id=envelope.candidate_id,
                reason=envelope.reason_code,
            ),
            rationale=envelope.rationale,
            model=str(body.get("model") or self.model),
            latency_ms=latency_ms,
            input_tokens=int(usage.get("prompt_tokens") or 0),
            output_tokens=int(usage.get("completion_tokens") or 0),
        )


@dataclass
class MockModelProvider:
    """Deterministic stand-in for a real LLM provider.

    `scripted` maps decision reasons (the observation's trigger, e.g.
    "route_invalidated" or "periodic") to forced Decisions for tests; when no
    script matches, the wrapped policy (RulePolicy by default) decides.
    """

    policy: Policy = field(default_factory=RulePolicy)
    scripted: Mapping[str, Decision] = field(default_factory=dict)

    def decide(self, request: DecisionRequest) -> DecisionResponse:
        trigger = request.observation.decision_reason or "periodic"
        if trigger in self.scripted:
            decision = self.scripted[trigger]
            return DecisionResponse(
                decision=decision,
                rationale=f"scripted:{trigger}",
                model="mock",
            )
        decision = self.policy.decide(
            request.observation, request.candidates, request.tools)
        return DecisionResponse(decision=decision, rationale=decision.reason, model="mock")
