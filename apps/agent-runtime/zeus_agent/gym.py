"""Gymnasium-style adapter over the agent session API.

Implements the reset/step shape without requiring the gymnasium package;
`as_gymnasium()` lazily wraps this env in a real gymnasium.Env when the
library is installed.
"""

from __future__ import annotations

from typing import Any

from zeus_agent.client import (
    ActionRequest,
    CreateSessionRequest,
    AgentVehicleSpec,
    EnvironmentClient,
    EnvironmentError,
    StepResponse,
    VehicleObservation,
)
from zeus_agent.runner import Scenario


class ZeusEnv:
    """Single-agent navigation environment.

    step(action) resolves the currently open decision, advances to the next
    decision boundary, and returns (observation, reward, terminated,
    truncated, info). reward is the ETA reduction in simulation seconds — a
    deliberately simple v1 shaping signal.
    """

    def __init__(self, client: EnvironmentClient, scenario: Scenario) -> None:
        self._client = client
        self._scenario = scenario
        self._session_id: str | None = None
        self._vehicle_id = 0
        self._last: StepResponse | None = None
        self._decisions = 0

    def reset(self, seed: int | None = None) -> VehicleObservation:
        del seed  # the environment is deterministic; no seeding surface yet
        request = CreateSessionRequest(
            vehicles=[AgentVehicleSpec(
                from_lon=self._scenario.origin[0],
                from_lat=self._scenario.origin[1],
                to_lon=self._scenario.destination[0],
                to_lat=self._scenario.destination[1],
                depart_seconds=0.0,
                algorithm=self._scenario.algorithm,
                agent=True,
            )],
            duration_seconds=self._scenario.duration_seconds,
            step_seconds=self._scenario.step_seconds,
            sample_interval_seconds=self._scenario.sample_interval_seconds,
            reroute_interval_seconds=self._scenario.reroute_interval_seconds,
            road_controls=list(self._scenario.road_controls),
        )
        created = self._client.create_session(request)
        if self._session_id:
            try:
                self._client.close(self._session_id)
            except EnvironmentError:
                pass
        self._session_id = created.session_id
        self._vehicle_id = (created.agents or [0])[0]
        self._decisions = 0
        return self._client.observe_vehicle(self._session_id, self._vehicle_id)

    def step(
        self, action_kind: str, candidate_id: str | None = None,
    ) -> tuple[VehicleObservation, float, bool, bool, dict[str, Any]]:
        if not self._session_id or not self._last or not self._last.decision_id:
            raise RuntimeError("step() called without an open decision boundary")
        self._client.submit_action(self._session_id, ActionRequest(
            decision_id=self._last.decision_id,
            vehicle_id=self._vehicle_id,
            kind=action_kind,
            candidate_id=candidate_id if action_kind == "commit_route" else None,
            based_on_state_version=self._last.state.state_version,
        ))
        self._last = self._client.step_until_event(
            self._session_id, max_ticks=1_000_000)
        self._decisions += 1
        observation = self._client.observe_vehicle(
            self._session_id, self._vehicle_id)
        previous_eta = self._previous_eta
        self._previous_eta = observation.remaining_eta_s
        reward = previous_eta - observation.remaining_eta_s
        terminated = observation.state == "arrived" or observation.finished
        truncated = self._decisions >= self._scenario.max_decisions
        info = {
            "tick": observation.tick,
            "state_version": observation.state_version,
            "decision_id": self._last.decision_id,
            "decision_reason": observation.decision_reason,
        }
        return observation, reward, terminated, truncated, info

    @property
    def _previous_eta(self) -> float:
        return getattr(self, "_eta_cache", 0.0)

    @_previous_eta.setter
    def _previous_eta(self, value: float) -> None:
        self._eta_cache = value

    def observation_spec(self) -> dict[str, type]:
        return {
            "vehicle_id": int, "state": str, "position.edge_id": int,
            "position.offset_m": float, "remaining_eta_s": float,
            "route_invalidated": bool, "remaining_edge_ids": list,
            "nearby_roads": list, "active_events": list,
        }

    def action_spec(self) -> dict[str, type]:
        return {"kind": str, "candidate_id": str | None}

    def close(self) -> None:
        if self._session_id:
            try:
                self._client.close(self._session_id)
            except EnvironmentError:
                pass
            self._session_id = None

    def as_gymnasium(self):  # pragma: no cover - optional dependency
        import gymnasium  # noqa: F401 (raise with a clear hint if missing)

        class _GymZeusEnv(gymnasium.Env):
            metadata = {"render_modes": []}

            def __init__(inner) -> None:
                super().__init__()
                inner.env = ZeusEnv(self._client, self._scenario)

            def reset(inner, *, seed=None, options=None):
                super().reset(seed=seed)
                return inner.env.reset(seed=seed), {}

            def step(inner, action):
                return inner.env.step(
                    action["kind"], action.get("candidate_id"))

            def close(inner) -> None:
                inner.env.close()

        return _GymZeusEnv()
