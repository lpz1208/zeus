"""Agent decision graph: LangGraph primary, plain functions for fallback.

Every node is a plain callable dict -> partial-dict, so the same nodes run
under `run_nodes` (plain loop, zero extra dependencies) or `build_graph`
(LangGraph StateGraph). Trigger and Guard are deterministic code per the
design doc (§5/§7.4); only the decide node consults the Policy.

Cycle: advance -> observe -> select_tools -> plan -> compare -> decide ->
guard -> act -> advance ... until the run finishes or the cap hits.
"""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Callable, Mapping, Sequence, TypedDict

from zeus_agent.client import (
    AgentToolRegistry,
    EnvironmentClient,
    EnvironmentError,
    FALLBACK_ALGORITHMS,
    ActionRequest,
    RouteCandidate,
    StepResponse,
    VehicleObservation,
)
from zeus_agent.model import DecisionRequest, ModelProvider
from zeus_agent.policy import Decision, Policy, RulePolicy

Node = Callable[[dict], dict]

NODE_ORDER = (
    "advance", "observe", "select_tools", "plan", "compare", "decide", "guard", "act",
)
NODES_AFTER_ADVANCE = NODE_ORDER[1:]


class AgentState(TypedDict, total=False):
    session_id: str
    map_id: str
    vehicle_id: int
    step: StepResponse | None
    observation: VehicleObservation | None
    decision_id: str | None
    candidates: list[RouteCandidate]
    selected_algorithms: list[str]
    tool_registry: AgentToolRegistry | None
    decision: Decision | None
    guard_allowed: bool
    guard_reason: str
    action_error: str | None
    events: list[str]
    iterations: int
    commits: int
    guard_rejections: int
    fallbacks: int
    decisions_since_commit: int
    route_invalidated_events: int
    model_calls: int
    model_failures: int
    model_name: str
    model_rationale: str
    model_error: str | None
    model_latency_ms: float
    model_input_tokens: int
    model_output_tokens: int
    node_latency_ms: dict[str, float]
    route_tool_calls: int
    last_action: ActionRequest | None
    action_ack: object | None
    max_iterations: int
    environment_snapshot_id: str
    checkpoint_node: str
    done: bool


@dataclass(frozen=True)
class GuardConfig:
    # Commit requires the best candidate to be at least this fraction faster
    # than the current remaining ETA — unless the route was invalidated.
    min_improvement_ratio: float = 0.10
    # Minimum decisions between two commits (cooldown after a switch).
    cooldown_decisions: int = 1
    require_state_version_match: bool = True


@dataclass(frozen=True)
class GuardVerdict:
    allowed: bool
    reason: str


def evaluate_guard(
    decision: Decision,
    observation: VehicleObservation,
    candidates: list[RouteCandidate],
    commits_total: int,
    decisions_since_commit: int,
    config: GuardConfig,
) -> GuardVerdict:
    """Deterministic Action Guard: candidate validity, benefit, cooldown."""
    if decision.kind != "commit_route":
        return GuardVerdict(True, "keep_route")
    if not decision.candidate_id:
        return GuardVerdict(False, "commit_without_candidate")
    candidate = next(
        (c for c in candidates if c.candidate_id == decision.candidate_id), None)
    if candidate is None or not candidate.ok:
        return GuardVerdict(False, "candidate_not_in_set")
    if (
        config.require_state_version_match
        and candidate.based_on_state_version is not None
        and candidate.based_on_state_version != observation.state_version
    ):
        return GuardVerdict(False, "stale_state_version")
    if not observation.route_invalidated:
        if candidate.time_s is None or observation.remaining_eta_s <= 0:
            return GuardVerdict(False, "unknown_benefit")
        target = observation.remaining_eta_s * (1.0 - config.min_improvement_ratio)
        if candidate.time_s > target:
            return GuardVerdict(False, "below_min_improvement")
        # Cooldown only applies between voluntary switches; an invalidated
        # route must always be replaceable.
        if commits_total > 0 and decisions_since_commit <= config.cooldown_decisions:
            return GuardVerdict(False, "commit_cooldown")
    return GuardVerdict(True, "commit_allowed")


def make_nodes(
    client: EnvironmentClient,
    policy: Policy | None = None,
    config: GuardConfig | None = None,
    model: ModelProvider | None = None,
    algorithms: Sequence[str] | None = None,
    should_cancel: Callable[[], bool] | None = None,
) -> dict[str, Node]:
    """Builds the node table; dependencies close over the client."""
    policy = policy or RulePolicy()
    config = config or GuardConfig()
    algorithm_filter = (
        None if algorithms is None else tuple(dict.fromkeys(algorithms))
    )

    def advance(state: dict) -> dict:
        if should_cancel and should_cancel():
            return {
                "done": True,
                "action_error": "cancelled",
                "events": [*state.get("events", []), "advance:cancelled"],
            }
        if state.get("iterations", 0) >= state.get("max_iterations", 100):
            return {
                "done": True,
                "action_error": state.get("action_error") or "max_iterations",
                "events": [*state.get("events", []), "advance:max_iterations"],
            }
        if state.get("decision_id"):
            # A barrier is already open; advancing would 409.
            return {"events": [*state.get("events", []), "advance:pending"]}
        response = client.step_until_event(state["session_id"])
        invalidated = 1 if (
            response.decision_id is not None
            and response.state.decision_reason == "route_invalidated"
        ) else 0
        return {
            "step": response,
            "decision_id": response.decision_id,
            "route_invalidated_events":
                state.get("route_invalidated_events", 0) + invalidated,
            "events": [
                *state.get("events", []),
                f"advance:tick={response.state.tick}:due={response.state.decision_reason}",
            ],
            "done": response.state.finished,
            "iterations": state.get("iterations", 0) + 1,
        }

    def observe(state: dict) -> dict:
        observation = client.observe_vehicle(
            state["session_id"], state.get("vehicle_id", 0))
        return {
            "observation": observation,
            "events": [
                *state.get("events", []),
                f"observe:v{observation.state_version}:eta={observation.remaining_eta_s:.0f}s",
            ],
        }

    def select_tools(state: dict) -> dict:
        # Deterministic tool selection: the full registry, or the fallback
        # set when the registry is unreachable. The policy stays in charge
        # of which candidate actually wins.
        if algorithm_filter == ():
            return {
                "selected_algorithms": [],
                "tool_registry": None,
                "events": [*state.get("events", []), "tools:"],
            }
        registry = state.get("tool_registry")
        try:
            registry = registry or client.tools()
            available = [
                a.algorithm_id for a in registry.algorithms
            ] or list(FALLBACK_ALGORITHMS)
        except EnvironmentError:
            registry = None
            available = list(FALLBACK_ALGORITHMS)
        selected = (
            available
            if algorithm_filter is None
            else [algorithm for algorithm in algorithm_filter if algorithm in available]
        )
        return {
            "selected_algorithms": selected,
            "tool_registry": registry,
            "events": [*state.get("events", []), f"tools:{','.join(selected)}"],
        }

    def plan(state: dict) -> dict:
        selected = state.get("selected_algorithms")
        plan_algorithms = (
            selected if selected is not None else list(FALLBACK_ALGORITHMS)
        )
        candidates: list[RouteCandidate] = []
        for algorithm in plan_algorithms:
            try:
                candidates.append(client.plan(
                    state["session_id"], state.get("vehicle_id", 0), algorithm))
            except EnvironmentError as error:
                candidates.append(RouteCandidate(
                    candidate_id=f"failed-{algorithm}", algorithm=algorithm,
                    ok=False, reason=error.message))
        return {
            "candidates": candidates,
            "route_tool_calls": (
                state.get("route_tool_calls", 0) + len(plan_algorithms)
            ),
            "events": [
                *state.get("events", []),
                f"plan:{sum(1 for c in candidates if c.ok)}/{len(candidates)} ok",
            ],
        }

    def compare(state: dict) -> dict:
        best = RulePolicy.best_candidate(state.get("candidates", []))
        label = f"{best.algorithm}/{best.candidate_id}" if best else "none"
        return {"events": [*state.get("events", []), f"compare:best={label}"]}

    def decide(state: dict) -> dict:
        observation = state["observation"]
        candidates = state.get("candidates", [])
        registry = state.get("tool_registry")
        if model is not None:
            try:
                response = model.decide(DecisionRequest(
                    observation=observation,
                    candidates=candidates,
                    tools=registry,
                    context=(
                        "The C++ environment is paused at a decision boundary. "
                        "The submitted action must be safe for the current state version."
                    ),
                ))
                decision = response.decision
                return {
                    "decision": decision,
                    "model_calls": state.get("model_calls", 0) + 1,
                    "model_name": response.model,
                    "model_rationale": response.rationale,
                    "model_error": state.get("model_error"),
                    "model_latency_ms": (
                        state.get("model_latency_ms", 0.0) + response.latency_ms),
                    "model_input_tokens": (
                        state.get("model_input_tokens", 0) + response.input_tokens),
                    "model_output_tokens": (
                        state.get("model_output_tokens", 0) + response.output_tokens),
                    "events": [
                        *state.get("events", []),
                        f"decide:model:{response.model}:{decision.kind}:{decision.reason}",
                    ],
                }
            except Exception as error:
                # The model is never allowed to strand a barrier. Fall back to
                # the deterministic policy, then let the Action Guard decide.
                decision = policy.decide(observation, candidates, registry)
                return {
                    "decision": decision,
                    "model_calls": state.get("model_calls", 0) + 1,
                    "model_failures": state.get("model_failures", 0) + 1,
                    "model_error": str(error),
                    "model_rationale": "",
                    "events": [
                        *state.get("events", []),
                        f"decide:model_fallback:{type(error).__name__}:{decision.kind}",
                    ],
                }
        decision = policy.decide(observation, candidates, registry)
        return {
            "decision": decision,
            "events": [
                *state.get("events", []),
                f"decide:{decision.kind}:{decision.reason}",
            ],
        }

    def guard(state: dict) -> dict:
        decision = state["decision"] or Decision("keep_route", reason="noop")
        verdict = evaluate_guard(
            decision,
            state["observation"],
            state.get("candidates", []),
            state.get("commits", 0),
            state.get("decisions_since_commit", 0),
            config,
        )
        if verdict.allowed:
            return {
                "guard_allowed": True,
                "guard_reason": verdict.reason,
                "events": [*state.get("events", []), f"guard:allow:{verdict.reason}"],
            }
        return {
            "decision": Decision("keep_route", reason=f"guard_{verdict.reason}"),
            "guard_allowed": False,
            "guard_reason": verdict.reason,
            "guard_rejections": state.get("guard_rejections", 0) + 1,
            "events": [*state.get("events", []), f"guard:reject:{verdict.reason}"],
        }

    def act(state: dict) -> dict:
        if not state.get("decision_id"):
            # No open barrier (e.g. the run just finished): nothing to submit.
            return {"events": [*state.get("events", []), "act:idle"]}
        cancelled = bool(should_cancel and should_cancel())
        decision = (
            Decision("keep_route", reason="benchmark_cancelled")
            if cancelled else state["decision"]
        )
        observation = state["observation"]
        request = ActionRequest(
            decision_id=state["decision_id"],
            vehicle_id=state.get("vehicle_id", 0),
            kind=decision.kind,
            candidate_id=(
                decision.candidate_id if decision.kind == "commit_route" else None),
            based_on_state_version=observation.state_version,
            reason_code=decision.reason,
        )
        commits = state.get("commits", 0)
        since = state.get("decisions_since_commit", 0)
        try:
            ack = client.submit_action(state["session_id"], request)
        except EnvironmentError as error:
            # Deterministic fallback: keep the current valid route (§7.4).
            fallback = ActionRequest(
                decision_id=state["decision_id"],
                vehicle_id=state.get("vehicle_id", 0),
                kind="keep_route",
                based_on_state_version=observation.state_version,
                reason_code="agent_fallback_keep",
            )
            try:
                fallback_ack = client.submit_action(state["session_id"], fallback)
                return {
                    "fallbacks": state.get("fallbacks", 0) + 1,
                    "action_error": error.message,
                    "decision_id": None,
                    "last_action": fallback,
                    "action_ack": fallback_ack,
                    "events": [
                        *state.get("events", []),
                        f"act:fallback:{error.message}",
                    ],
                }
            except EnvironmentError as inner:
                return {
                    "action_error": inner.message,
                    "done": True,
                    "events": [
                        *state.get("events", []),
                        f"act:failed:{inner.message}",
                    ],
                }
        if decision.kind == "commit_route":
            commits += 1
            since = 0
        else:
            since += 1
        return {
            "commits": commits,
            "decisions_since_commit": since,
            "decision_id": None,
            "last_action": request,
            "action_ack": ack,
            "action_error": "cancelled" if cancelled else None,
            "done": cancelled,
            "events": [*state.get("events", []), f"act:{decision.kind}"],
        }

    def timed(name: str, node: Node) -> Node:
        def invoke(state: dict) -> dict:
            started = time.monotonic()
            result = node(state)
            latency = dict(state.get("node_latency_ms", {}))
            latency[name] = latency.get(name, 0.0) + (
                time.monotonic() - started
            ) * 1000.0
            result["node_latency_ms"] = latency
            return result

        return invoke

    return {
        name: timed(name, node)
        for name, node in zip(NODE_ORDER, (
            advance, observe, select_tools, plan, compare, decide, guard, act))
    }


def run_nodes(
    nodes: Mapping[str, Node],
    state: dict,
    max_iterations: int = 100,
) -> dict:
    """Plain-loop driver: executes the cycle, merging partial dicts.

    While a decision barrier is pending (decision_id set), advance is
    skipped so the loop re-observes and resolves the open decision.
    """
    while not state.get("done"):
        state.setdefault("max_iterations", max_iterations)
        if not state.get("decision_id"):
            state = {**state, **nodes["advance"](state)}
            if state.get("done"):
                break
        for name in NODES_AFTER_ADVANCE:
            state = {**state, **nodes[name](state)}
    return state


def build_graph(
    nodes: Mapping[str, Node],
    *,
    checkpointer=None,
    interrupt_before: list[str] | None = None,
    interrupt_after: list[str] | None = None,
):
    """Assembles a langgraph StateGraph over the same node functions.

    LangGraph is a project dependency. ImportError remains explicit so an
    incomplete deployment can safely select the plain `run_nodes` fallback.
    """
    try:
        from langgraph.graph import END, START, StateGraph
    except ImportError as error:  # pragma: no cover - depends on extras
        raise ImportError(
            "langgraph is not installed; run `uv sync` or use run_nodes "
            "as the deterministic emergency fallback"
        ) from error

    graph = StateGraph(AgentState)
    for name in NODE_ORDER:
        graph.add_node(name, nodes[name])
    graph.add_edge(START, "advance")
    # The run finishing at advance goes straight to END; otherwise the
    # linear decision chain runs to act.
    graph.add_conditional_edges(
        "advance",
        lambda state: END if state.get("done") else "observe",
        {END: END, "observe": "observe"},
    )
    for before, after in zip(NODE_ORDER[1:], NODE_ORDER[2:]):
        graph.add_edge(before, after)
    graph.add_conditional_edges(
        "act",
        lambda state: END if state.get("done") else "advance",
        {END: END, "advance": "advance"},
    )
    return graph.compile(
        checkpointer=checkpointer,
        interrupt_before=interrupt_before,
        interrupt_after=interrupt_after,
    )
