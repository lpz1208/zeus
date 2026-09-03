"""Typed HTTP client for the Zeus agent environment.

Sync httpx by design: the decision loop is strictly sequential
request/response, step/untilEvent may block for seconds, and sync node
functions drop directly into a LangGraph graph without async checkpointing.
An async swap is confined behind the EnvironmentClient Protocol.

Field lists mirror the C++ worker handlers (session_worker.cc: commandReset,
writeStateHeader, writeSnapshot, commandAgentObserve, commandPlan) and the Go
mirror sessionStateFields in apps/control-server/agent_sessions.go.
"""

from __future__ import annotations

from typing import Protocol, runtime_checkable

import httpx
from pydantic import BaseModel, ConfigDict, Field
from pydantic.alias_generators import to_camel


class CamelModel(BaseModel):
    model_config = ConfigDict(
        populate_by_name=True,
        alias_generator=to_camel,
        extra="ignore",
    )


class EnvironmentError(Exception):
    """Non-2xx response from the control plane."""

    def __init__(self, status_code: int, message: str) -> None:
        super().__init__(f"{status_code}: {message}")
        self.status_code = status_code
        self.message = message


# --------------------------------------------------------------------- models


class AgentAlgorithmCapability(CamelModel):
    algorithm_id: str
    algorithm_version: str
    supported_objectives: list[str] = Field(default_factory=list)
    search_direction: str = ""
    supports_dynamic_weights: bool = False
    supports_incremental_repair: bool = False
    supports_k_candidates: bool = False
    supports_time_dependency: bool = False
    deterministic: bool = True
    exact: bool = True
    uses_heuristic: bool = False


class AgentToolRegistry(CamelModel):
    registry_version: str = ""
    algorithms: list[AgentAlgorithmCapability] = Field(default_factory=list)


class RoadControl(CamelModel):
    time_seconds: float
    edge_ids: list[int]
    action: str = "close"
    value: float = 1.0


class VehicleControl(CamelModel):
    time_seconds: float
    vehicle_id: int
    action: str
    value: float = 1.0


class AgentVehicleSpec(CamelModel):
    from_lon: float
    from_lat: float
    to_lon: float
    to_lat: float
    depart_seconds: float = 0.0
    algorithm: str = "dijkstra"
    agent: bool = False


class CreateSessionRequest(CamelModel):
    vehicles: list[AgentVehicleSpec]
    duration_seconds: float = 900.0
    step_seconds: float = 1.0
    sample_interval_seconds: float = 15.0
    exit_headway_ff_seconds: float = 1.4
    exit_headway_jam_seconds: float = 2.0
    reroute_interval_seconds: float = 0.0
    reroute_cost_ratio: float = 1.25
    min_speed_ratio: float = 0.0
    road_controls: list[RoadControl] = Field(default_factory=list)
    vehicle_controls: list[VehicleControl] = Field(default_factory=list)


class SessionState(CamelModel):
    """State header; the create response adds session/vehicle fields."""

    tick: int = 0
    simulation_time_s: float = 0.0
    state_version: int = 0
    finished: bool = False
    cancelled: bool = False
    decision_due: bool = False
    decision_reason: str = ""
    agent_vehicle_ids: list[int] = Field(default_factory=list)
    # create/reset only
    session_id: str | None = None
    ready: bool | None = None
    paused: bool | None = None
    vehicles: int | None = None
    agents: list[int] = Field(default_factory=list)


class EdgeState(CamelModel):
    edge_id: int
    occupancy: int = 0
    capacity: int = 1
    closed: bool = False
    speed_factor: float = 1.0
    cost_factor: float = 1.0
    mean_speed_mps: float = 0.0


class AgentVehicleState(CamelModel):
    vehicle_id: int
    state: str = "waiting"
    edge_id: int = -1
    offset_m: float = 0.0
    route_id: int = 0
    destination_edge_id: int = -1
    remaining_eta_s: float = 0.0
    route_invalidated: bool = False
    held: bool = False
    remaining_edge_ids: list[int] = Field(default_factory=list)


class SessionCounts(CamelModel):
    arrived: int = 0
    driving: int = 0
    waiting: int = 0
    unroutable: int = 0


class SessionObservation(SessionState):
    """observe response: state header + hot edges + full agent states.

    Overrides the inherited `agents` field: the create response lists agent
    vehicle indices, observe lists full AgentVehicleState objects.
    """

    counts: SessionCounts = Field(default_factory=SessionCounts)
    edges: list[EdgeState] = Field(default_factory=list)
    agents: list[AgentVehicleState] = Field(default_factory=list)  # type: ignore[assignment]


class NearbyRoad(CamelModel):
    edge_id: int
    speed_mps: float = 0.0
    free_flow_speed_mps: float = 0.0
    occupancy_ratio: float = 0.0
    estimated_travel_time_s: float = 0.0
    closed: bool = False


class ActiveEvent(CamelModel):
    event_id: str
    event_type: str = Field(default="", alias="type")
    affected_edge_ids: list[int] = Field(default_factory=list)


class VehiclePosition(CamelModel):
    edge_id: int = -1
    offset_m: float = 0.0


class VehicleObservation(SessionState):
    """agent-observe response for one vehicle (NavigationObservation shape)."""

    vehicle_id: int = 0
    state: str = "waiting"
    position: VehiclePosition = Field(default_factory=VehiclePosition)
    destination_edge_id: int = -1
    remaining_eta_s: float = 0.0
    route_invalidated: bool = False
    remaining_edge_ids: list[int] = Field(default_factory=list)
    nearby_roads: list[NearbyRoad] = Field(default_factory=list)
    active_events: list[ActiveEvent] = Field(default_factory=list)
    available_algorithms: list[AgentAlgorithmCapability] = Field(default_factory=list)

    def nearby_road(self, edge_id: int) -> NearbyRoad | None:
        for road in self.nearby_roads:
            if road.edge_id == edge_id:
                return road
        return None


class RouteCandidate(CamelModel):
    candidate_id: str
    vehicle_id: int = 0
    algorithm: str = "dijkstra"
    effective_algorithm: str | None = None
    based_on_state_version: int | None = None
    ok: bool = True
    reason: str | None = None
    message: str | None = None
    time_s: float | None = None
    length_m: float | None = None
    expanded_nodes: int | None = None
    edges: list[int] = Field(default_factory=list)


class StepResponse(CamelModel):
    state: SessionState
    decision_id: str | None = None


class ActionRequest(CamelModel):
    decision_id: str
    agent_id: str = "default"
    vehicle_id: int
    kind: str  # keep_route | commit_route
    candidate_id: str | None = None
    based_on_state_version: int
    valid_until_simulation_time: float | None = None
    reason_code: str = ""


class ActionAck(CamelModel):
    accepted: bool = False
    reason: str = ""
    applies_at_next_tick: bool = False


class SnapshotInfo(CamelModel):
    snapshot_id: str
    source_session_id: str = ""
    tick: int = 0
    simulation_time_s: float = 0.0
    state_version: int = 0
    action_count: int = 0
    storage: str = ""


class SnapshotRestore(CamelModel):
    snapshot_id: str
    state: SessionState
    decision_id: str | None = None


class ResumeAck(CamelModel):
    accepted: bool = False
    tick: int = 0
    simulation_time_s: float = 0.0
    state_version: int = 0
    finished: bool = False


# -------------------------------------------------------------------- client


@runtime_checkable
class EnvironmentClient(Protocol):
    """Transport seam: HTTP today, gRPC later."""

    def tools(self) -> AgentToolRegistry: ...
    def create_session(self, request: CreateSessionRequest) -> SessionState: ...
    def observe(self, session_id: str) -> SessionObservation: ...
    def observe_vehicle(self, session_id: str, vehicle_id: int) -> VehicleObservation: ...
    def plan(self, session_id: str, vehicle_id: int, algorithm: str) -> RouteCandidate: ...
    def step(self, session_id: str, ticks: int = 1) -> StepResponse: ...
    def step_until_event(self, session_id: str, max_ticks: int = 100_000) -> StepResponse: ...
    def submit_action(self, session_id: str, action: ActionRequest) -> ActionAck: ...
    def resume(self, session_id: str) -> ResumeAck: ...
    def pause(self, session_id: str) -> SessionState: ...
    def create_snapshot(self, session_id: str) -> SnapshotInfo: ...
    def restore_snapshot(self, snapshot_id: str) -> SnapshotRestore: ...
    def delete_snapshot(self, snapshot_id: str) -> None: ...
    def result(self, session_id: str) -> dict: ...
    def close(self, session_id: str) -> None: ...


FALLBACK_ALGORITHMS = ("dijkstra", "astar", "bidijkstra", "biastar")


class HttpEnvironmentClient:
    """Talks to the Go control plane's /api/maps/{map_id}/agent/* surface."""

    def __init__(
        self,
        map_id: str,
        base_url: str = "http://127.0.0.1:8080",
        timeout: float = 30.0,
        step_timeout: float = 180.0,
        transport: httpx.BaseTransport | None = None,
    ) -> None:
        self._prefix = f"/api/maps/{map_id}/agent"
        self._client = httpx.Client(
            base_url=base_url,
            timeout=httpx.Timeout(timeout),
            transport=transport,
        )
        self._step_timeout = httpx.Timeout(step_timeout)

    def __enter__(self) -> HttpEnvironmentClient:
        return self

    def __exit__(self, *exc: object) -> None:
        self._client.close()

    def _request(
        self,
        method: str,
        path: str,
        *,
        payload: BaseModel | None = None,
        timeout: httpx.Timeout | None = None,
    ) -> httpx.Response:
        response = self._client.request(
            method,
            path,
            json=payload.model_dump(by_alias=True, exclude_none=True)
            if payload is not None
            else None,
            timeout=timeout,
        )
        if response.status_code >= 400:
            message = str(response.text)
            try:
                body = response.json()
                message = body.get("error", message)
            except ValueError:
                pass
            raise EnvironmentError(response.status_code, message)
        return response

    def tools(self) -> AgentToolRegistry:
        return AgentToolRegistry.model_validate(
            self._request("GET", f"{self._prefix}/tools").json())

    def create_session(self, request: CreateSessionRequest) -> SessionState:
        return SessionState.model_validate(
            self._request("POST", f"{self._prefix}/sessions", payload=request).json())

    def observe(self, session_id: str) -> SessionObservation:
        return SessionObservation.model_validate(
            self._request("GET", f"{self._prefix}/sessions/{session_id}").json())

    def observe_vehicle(self, session_id: str, vehicle_id: int) -> VehicleObservation:
        return VehicleObservation.model_validate(
            self._request(
                "GET", f"{self._prefix}/sessions/{session_id}/agent/{vehicle_id}").json())

    def plan(self, session_id: str, vehicle_id: int, algorithm: str) -> RouteCandidate:
        return RouteCandidate.model_validate(
            self._request(
                "POST",
                f"{self._prefix}/sessions/{session_id}/plan",
                payload=AgentPlanRequest(vehicle_id=vehicle_id, algorithm=algorithm),
            ).json())

    def step(self, session_id: str, ticks: int = 1) -> StepResponse:
        return StepResponse.model_validate(
            self._request(
                "POST",
                f"{self._prefix}/sessions/{session_id}/step",
                payload=AgentStepRequest(ticks=ticks),
            ).json())

    def step_until_event(self, session_id: str, max_ticks: int = 100_000) -> StepResponse:
        return StepResponse.model_validate(
            self._request(
                "POST",
                f"{self._prefix}/sessions/{session_id}/step",
                payload=AgentStepRequest(until_event=True, max_ticks=max_ticks),
                timeout=self._step_timeout,
            ).json())

    def submit_action(self, session_id: str, action: ActionRequest) -> ActionAck:
        return ActionAck.model_validate(
            self._request(
                "POST",
                f"{self._prefix}/sessions/{session_id}/actions",
                payload=action,
            ).json())

    def resume(self, session_id: str) -> ResumeAck:
        return ResumeAck.model_validate(
            self._request("POST", f"{self._prefix}/sessions/{session_id}/run").json())

    def pause(self, session_id: str) -> SessionState:
        return SessionState.model_validate(
            self._request("POST", f"{self._prefix}/sessions/{session_id}/pause").json())

    def create_snapshot(self, session_id: str) -> SnapshotInfo:
        return SnapshotInfo.model_validate(
            self._request("POST", f"{self._prefix}/sessions/{session_id}/snapshots").json())

    def restore_snapshot(self, snapshot_id: str) -> SnapshotRestore:
        return SnapshotRestore.model_validate(
            self._request("POST", f"{self._prefix}/snapshots/{snapshot_id}/restore").json())

    def delete_snapshot(self, snapshot_id: str) -> None:
        self._request("DELETE", f"{self._prefix}/snapshots/{snapshot_id}")

    def result(self, session_id: str) -> dict:
        return self._request(
            "GET", f"{self._prefix}/sessions/{session_id}/result").json()

    def close(self, session_id: str) -> None:
        self._request("DELETE", f"{self._prefix}/sessions/{session_id}")


class AgentPlanRequest(CamelModel):
    vehicle_id: int
    algorithm: str


class AgentStepRequest(CamelModel):
    ticks: int | None = None
    until_event: bool | None = None
    max_ticks: int | None = None
