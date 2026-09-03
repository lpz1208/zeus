"""Episode runner: drives the decision loop against a live environment."""

from __future__ import annotations

import time
import uuid
import sqlite3
from contextlib import ExitStack
from dataclasses import dataclass, field
from pathlib import Path

from zeus_agent.client import (
    CreateSessionRequest,
    EnvironmentClient,
    EnvironmentError,
    ActionAck,
    ActionRequest,
    AgentToolRegistry,
    AgentVehicleSpec,
    RoadControl,
    RouteCandidate,
    StepResponse,
    VehicleControl,
    VehicleObservation,
)
from zeus_agent.graph import build_graph, make_nodes, run_nodes
from zeus_agent.model import ModelProvider
from zeus_agent.persistence import DecisionTraceStore
from zeus_agent.policy import Decision, Policy, RulePolicy


@dataclass
class TraceEvent:
    node: str
    simulation_time_s: float
    state_version: int | None
    detail: str
    latency_ms: float


@dataclass
class EpisodeTrace:
    session_id: str
    thread_id: str = ""
    resumed: bool = False
    interrupted: bool = False
    environment_snapshot_id: str = ""
    environment_restored: bool = False
    events: list[TraceEvent] = field(default_factory=list)
    decisions: int = 0
    commits: int = 0
    keeps: int = 0
    guard_rejections: int = 0
    fallbacks: int = 0
    route_invalidated_events: int = 0
    arrived: bool = False
    finished: bool = False
    ticks: int = 0
    wall_seconds: float = 0.0
    execution_mode: str = "plain_loop"
    model_name: str = ""
    model_calls: int = 0
    model_failures: int = 0
    model_latency_ms: float = 0.0
    model_input_tokens: int = 0
    model_output_tokens: int = 0
    model_rationale: str = ""
    model_error: str | None = None
    graph_events: list[str] = field(default_factory=list)
    final_observation: VehicleObservation | None = None
    error: str | None = None

    def summary(self) -> str:
        lines = [
            f"session        {self.session_id}",
            f"thread         {self.thread_id or '-'}"
            f"  resumed {self.resumed}  interrupted {self.interrupted}",
            f"env snapshot   {self.environment_snapshot_id or '-'}"
            f"  restored {self.environment_restored}",
            f"arrived        {self.arrived}  finished {self.finished}  ticks {self.ticks}",
            f"decisions      {self.decisions} (commits {self.commits}, "
            f"keeps {self.keeps}, guard rejections {self.guard_rejections}, "
            f"fallbacks {self.fallbacks})",
            f"invalidations  {self.route_invalidated_events}",
            f"wall seconds   {self.wall_seconds:.2f}",
            f"runtime        {self.execution_mode}",
        ]
        if self.model_calls:
            lines.append(
                f"model          {self.model_name or 'unknown'}  calls {self.model_calls}  "
                f"failures {self.model_failures}  latency {self.model_latency_ms:.0f} ms  "
                f"tokens {self.model_input_tokens}/{self.model_output_tokens}")
        if self.error:
            lines.append(f"error          {self.error}")
        return "\n".join(lines)


@dataclass
class Scenario:
    map_id: str
    origin: tuple[float, float]
    destination: tuple[float, float]
    duration_seconds: float = 1800.0
    step_seconds: float = 1.0
    algorithm: str = "astar"
    reroute_interval_seconds: float = 30.0
    reroute_cost_ratio: float = 1.25
    sample_interval_seconds: float = 10.0
    road_controls: tuple[RoadControl, ...] = ()
    vehicle_controls: tuple[VehicleControl, ...] = ()
    max_decisions: int = 100


def run_episode(
    client: EnvironmentClient,
    scenario: Scenario,
    policy: Policy | None = None,
    *,
    model: ModelProvider | None = None,
    use_langgraph: bool = True,
    checkpoint_path: str | Path | None = None,
    trace_path: str | Path | None = None,
    thread_id: str | None = None,
    resume: bool = False,
    interrupt_after: str | None = None,
) -> EpisodeTrace:
    """Runs one full episode: create -> decision loop -> final observation.

    When ``use_langgraph`` is true the compiled StateGraph owns the cycle. If
    LangGraph is expected in normal deployments; if an incomplete deployment
    cannot import it, the exact same node table runs through the deterministic
    plain-loop fallback. With a SQLite checkpointer, an interrupted graph
    deliberately keeps its environment Session alive so the same ``thread_id``
    can continue it. Completed non-interrupted sessions are closed.
    """
    persistent_options = checkpoint_path or trace_path or resume or interrupt_after
    if persistent_options and not use_langgraph:
        raise ValueError("checkpointing and interruption require LangGraph")
    if resume and (not checkpoint_path or not thread_id):
        raise ValueError("resume requires checkpoint_path and thread_id")
    if interrupt_after and not checkpoint_path:
        raise ValueError("interrupt_after requires checkpoint_path")
    if trace_path and not checkpoint_path:
        raise ValueError("trace_path requires checkpoint_path")

    policy = policy or RulePolicy()
    started = time.monotonic()
    request = CreateSessionRequest(
        vehicles=[AgentVehicleSpec(
            from_lon=scenario.origin[0],
            from_lat=scenario.origin[1],
            to_lon=scenario.destination[0],
            to_lat=scenario.destination[1],
            depart_seconds=0.0,
            algorithm=scenario.algorithm,
            agent=True,
        )],
        duration_seconds=scenario.duration_seconds,
        step_seconds=scenario.step_seconds,
        sample_interval_seconds=scenario.sample_interval_seconds,
        reroute_interval_seconds=scenario.reroute_interval_seconds,
        reroute_cost_ratio=scenario.reroute_cost_ratio,
        road_controls=list(scenario.road_controls),
        vehicle_controls=list(scenario.vehicle_controls),
    )
    run_thread_id = thread_id or uuid.uuid4().hex
    trace: EpisodeTrace | None = None
    close_session = True

    with ExitStack() as stack:
        checkpointer = None
        if checkpoint_path:
            try:
                from langgraph.checkpoint.sqlite import SqliteSaver
                from langgraph.checkpoint.serde.jsonplus import JsonPlusSerializer
            except ImportError as error:  # pragma: no cover - packaging failure
                raise ImportError(
                    "langgraph-checkpoint-sqlite is required for persistent runs"
                ) from error
            checkpoint_file = Path(checkpoint_path)
            checkpoint_file.parent.mkdir(parents=True, exist_ok=True)
            checkpoint_connection = sqlite3.connect(
                checkpoint_file, check_same_thread=False)
            stack.callback(checkpoint_connection.close)
            checkpointer = SqliteSaver(
                checkpoint_connection,
                serde=JsonPlusSerializer(allowed_msgpack_modules=[
                    StepResponse,
                    VehicleObservation,
                    RouteCandidate,
                    AgentToolRegistry,
                    ActionRequest,
                    ActionAck,
                    Decision,
                ]),
            )

        store = (
            stack.enter_context(DecisionTraceStore(trace_path))
            if trace_path else None
        )
        nodes = make_nodes(client, policy, model=model)
        graph = None
        graph_config = {
            "configurable": {"thread_id": run_thread_id},
            "recursion_limit": max(50, scenario.max_decisions * 10 + 10),
        }

        if (
            checkpointer is not None
            and not resume
            and checkpointer.get_tuple(graph_config) is not None
        ):
            raise ValueError(
                f"thread_id {run_thread_id} already exists; use resume or a new id")

        if resume:
            graph = build_graph(nodes, checkpointer=checkpointer)
            snapshot = graph.get_state(graph_config)
            if not snapshot.values:
                raise ValueError(f"no checkpoint found for thread_id {run_thread_id}")
            initial_state = None
            session_id = str(snapshot.values.get("session_id", ""))
            vehicle_id = int(snapshot.values.get("vehicle_id", 0))
            environment_snapshot_id = str(
                snapshot.values.get("environment_snapshot_id", ""))
            environment_restored = False
            checkpoint_map_id = snapshot.values.get("map_id")
            if checkpoint_map_id and checkpoint_map_id != scenario.map_id:
                raise ValueError(
                    f"checkpoint map_id {checkpoint_map_id} does not match "
                    f"requested map_id {scenario.map_id}")
            try:
                client.observe_vehicle(session_id, vehicle_id)
            except EnvironmentError as error:
                if error.status_code != 404 or not environment_snapshot_id:
                    raise
                restored = client.restore_snapshot(environment_snapshot_id)
                session_id = restored.state.session_id or session_id
                checkpoint_node = snapshot.values.get("checkpoint_node")
                graph.update_state(
                    graph_config,
                    {
                        "session_id": session_id,
                        "decision_id": restored.decision_id,
                    },
                    as_node=checkpoint_node,
                )
                environment_restored = True
            trace = EpisodeTrace(
                session_id=session_id,
                thread_id=run_thread_id,
                resumed=True,
                execution_mode="langgraph",
                environment_snapshot_id=environment_snapshot_id,
                environment_restored=environment_restored,
            )
        else:
            created = client.create_session(request)
            session_id = created.session_id or ""
            vehicle_id = (created.agents or [0])[0]
            trace = EpisodeTrace(
                session_id=session_id,
                thread_id=run_thread_id,
            )
            initial_state = {
                "session_id": session_id,
                "map_id": scenario.map_id,
                "vehicle_id": vehicle_id,
                "max_iterations": scenario.max_decisions,
            }

        if store:
            store.start_run(
                thread_id=run_thread_id,
                map_id=scenario.map_id,
                session_id=trace.session_id,
                execution_mode="langgraph",
                scenario=scenario,
            )

        try:
            if use_langgraph:
                try:
                    if graph is None:
                        graph = build_graph(
                            nodes,
                            checkpointer=checkpointer,
                            interrupt_after=[interrupt_after] if interrupt_after else None,
                        )
                    state = graph.invoke(initial_state, graph_config)
                    trace.execution_mode = "langgraph"
                    if checkpointer:
                        trace.interrupted = bool(graph.get_state(graph_config).next)
                except ImportError:
                    if checkpointer:
                        raise
                    state = run_nodes(
                        nodes, initial_state or {},
                        max_iterations=scenario.max_decisions)
                    trace.execution_mode = "plain_loop_fallback"
            else:
                state = run_nodes(
                    nodes, initial_state or {},
                    max_iterations=scenario.max_decisions)
                trace.execution_mode = "plain_loop"

            _populate_trace(trace, state)
            if trace.interrupted and graph:
                environment_snapshot = client.create_snapshot(trace.session_id)
                trace.environment_snapshot_id = environment_snapshot.snapshot_id
                graph.update_state(
                    graph_config,
                    {
                        "environment_snapshot_id": environment_snapshot.snapshot_id,
                        "checkpoint_node": interrupt_after,
                    },
                    as_node=interrupt_after,
                )
                if store:
                    store.set_environment_snapshot(
                        run_thread_id, environment_snapshot.snapshot_id)
            try:
                final = client.observe_vehicle(trace.session_id, vehicle_id)
                trace.final_observation = final
                trace.arrived = final.state == "arrived"
                trace.finished = final.finished
                trace.ticks = final.tick
            except EnvironmentError as error:
                trace.error = trace.error or error.message
            close_session = not trace.interrupted
            trace.wall_seconds = time.monotonic() - started

            if store and graph:
                store.record_history(
                    run_thread_id, graph.get_state_history(graph_config))
                status = (
                    "interrupted" if trace.interrupted
                    else "completed" if trace.finished and not trace.error
                    else "failed"
                )
                store.finish_run(run_thread_id, status, trace)
            return trace
        except Exception as error:
            close_session = not bool(checkpointer)
            if store:
                if graph:
                    store.record_history(
                        run_thread_id, graph.get_state_history(graph_config))
                store.finish_run(run_thread_id, "failed", {"error": str(error)})
            raise
        finally:
            if trace is not None and close_session and trace.session_id:
                try:
                    client.close(trace.session_id)
                except EnvironmentError:
                    pass


def _populate_trace(trace: EpisodeTrace, state: dict) -> None:
    trace.decisions = state.get("iterations", 0)
    trace.commits = state.get("commits", 0)
    trace.keeps = max(
        0, trace.decisions - trace.commits - state.get("fallbacks", 0))
    trace.guard_rejections = state.get("guard_rejections", 0)
    trace.fallbacks = state.get("fallbacks", 0)
    trace.route_invalidated_events = state.get("route_invalidated_events", 0)
    trace.error = state.get("action_error")
    trace.model_name = state.get("model_name", "")
    trace.model_calls = state.get("model_calls", 0)
    trace.model_failures = state.get("model_failures", 0)
    trace.model_latency_ms = state.get("model_latency_ms", 0.0)
    trace.model_input_tokens = state.get("model_input_tokens", 0)
    trace.model_output_tokens = state.get("model_output_tokens", 0)
    trace.model_rationale = state.get("model_rationale", "")
    trace.model_error = state.get("model_error")
    trace.graph_events = list(state.get("events", []))
