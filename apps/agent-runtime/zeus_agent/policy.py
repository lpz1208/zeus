"""Decision policies.

RulePolicy is the deterministic regression baseline required by the design
doc (docs/geospatial-agent-environment.md §10.1): no randomness, no model
calls, and algorithm-agnostic candidate comparison so a future K-shortest-
paths registry entry plugs in without policy changes.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol, Sequence

from zeus_agent.client import AgentToolRegistry, RouteCandidate, VehicleObservation


@dataclass(frozen=True)
class Decision:
    kind: str  # "keep_route" | "commit_route"
    candidate_id: str | None = None
    reason: str = ""


KEEP = Decision(kind="keep_route", reason="noop")


class Policy(Protocol):
    def decide(
        self,
        observation: VehicleObservation,
        candidates: Sequence[RouteCandidate],
        tools: AgentToolRegistry | None,
    ) -> Decision: ...


@dataclass(frozen=True)
class RulePolicyConfig:
    # Commit only when the best candidate is at least this fraction faster
    # than the remaining ETA (unless the route was invalidated).
    eta_improvement_threshold: float = 0.10


class RulePolicy:
    """Deterministic baseline: keep unless invalidated or clearly better."""

    def __init__(self, config: RulePolicyConfig | None = None) -> None:
        self.config = config or RulePolicyConfig()

    def decide(
        self,
        observation: VehicleObservation,
        candidates: Sequence[RouteCandidate],
        tools: AgentToolRegistry | None,
    ) -> Decision:
        best = self.best_candidate(candidates)
        if best is None:
            return Decision("keep_route", reason="no_valid_candidate")
        if observation.route_invalidated:
            return Decision(
                "commit_route",
                candidate_id=best.candidate_id,
                reason="route_invalidated",
            )
        target = observation.remaining_eta_s * (1.0 - self.config.eta_improvement_threshold)
        if best.time_s is not None and best.time_s <= target:
            return Decision(
                "commit_route",
                candidate_id=best.candidate_id,
                reason="eta_improvement",
            )
        return Decision("keep_route", reason="within_threshold")

    @staticmethod
    def best_candidate(
        candidates: Sequence[RouteCandidate],
    ) -> RouteCandidate | None:
        """Min time, then min length, then candidate id (deterministic)."""
        valid = [c for c in candidates if c.ok and c.time_s is not None]
        if not valid:
            return None
        return min(
            valid,
            key=lambda c: (c.time_s, c.length_m or float("inf"), c.candidate_id),
        )


@dataclass(frozen=True)
class FixedRoutePolicy:
    """Static baseline: keep the route selected at session creation."""

    def decide(
        self,
        observation: VehicleObservation,
        candidates: Sequence[RouteCandidate],
        tools: AgentToolRegistry | None,
    ) -> Decision:
        del observation, candidates, tools
        return Decision("keep_route", reason="fixed_route_baseline")


@dataclass(frozen=True)
class ReactiveAlgorithmPolicy:
    """Traditional baseline: re-run one algorithm only after invalidation."""

    algorithm: str = "astar"

    def decide(
        self,
        observation: VehicleObservation,
        candidates: Sequence[RouteCandidate],
        tools: AgentToolRegistry | None,
    ) -> Decision:
        del tools
        if not observation.route_invalidated:
            return Decision("keep_route", reason="reactive_wait")
        candidate = next(
            (item for item in candidates
             if item.ok and item.algorithm == self.algorithm),
            None,
        )
        if candidate is None:
            return Decision("keep_route", reason="reactive_no_candidate")
        return Decision(
            "commit_route",
            candidate_id=candidate.candidate_id,
            reason="reactive_invalidation",
        )
