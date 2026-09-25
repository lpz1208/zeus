"""Sequential, server-executed user-code episodes for durable Benchmark jobs."""
from __future__ import annotations

import time
from typing import Callable

from zeus_agent.client import ActionRequest, AgentVehicleSpec, CreateSessionRequest, EnvironmentClient, EnvironmentError
from zeus_agent.runner import EpisodeTrace, Scenario


def run_custom_episode(client: EnvironmentClient, scenario: Scenario, source: str, steps: int,
                       should_cancel: Callable[[], bool] | None = None) -> EpisodeTrace:
    started = time.monotonic()
    created = client.create_session(CreateSessionRequest(
        vehicles=[AgentVehicleSpec(from_lon=scenario.origin[0], from_lat=scenario.origin[1],
            to_lon=scenario.destination[0], to_lat=scenario.destination[1],
            algorithm=scenario.algorithm, agent=True)],
        duration_seconds=scenario.duration_seconds, step_seconds=scenario.step_seconds,
        sample_interval_seconds=scenario.sample_interval_seconds,
        reroute_interval_seconds=scenario.reroute_interval_seconds,
        reroute_cost_ratio=scenario.reroute_cost_ratio,
        reroute_recovery_interval_seconds=scenario.reroute_recovery_interval_seconds,
        reroute_min_gain_seconds=scenario.reroute_min_gain_seconds,
        reroute_cooldown_seconds=scenario.reroute_cooldown_seconds,
        road_controls=list(scenario.road_controls), vehicle_controls=list(scenario.vehicle_controls)))
    session = created.session_id
    if not session:
        raise ValueError("environment did not create a session")
    vehicle_id = (created.agents or [0])[0]
    trace = EpisodeTrace(session, execution_mode="custom_code")
    cancelled = lambda: bool(should_cancel and should_cancel())
    try:
        observed = client.observe(session)
        decision_id = observed.decision_id
        initial = True
        while not observed.finished:
            if cancelled():
                trace.error = "cancelled"
                break
            if initial and not decision_id:
                decision_id = client.open_decision(session, vehicle_id, observed.state_version).decision_id
            initial = False
            if not decision_id:
                advanced = client.step_until_event(session, max_ticks=16)
                decision_id = advanced.decision_id
                observed = client.observe(session)
                continue
            if trace.decisions >= scenario.max_decisions:
                trace.error = "max_decisions"
                break
            vehicle = client.observe_vehicle(session, vehicle_id)
            previous = next(item for item in observed.agents if item.vehicle_id == vehicle_id)
            begin = time.monotonic()
            trace.decisions += 1
            trace.route_tool_calls += 1
            record = {"stateVersion": vehicle.state_version, "tick": vehicle.tick,
                      "decisionId": decision_id, "status": "planning"}
            trace.custom_decisions.append(record)
            planned = client.plan_code(session, vehicle_id, vehicle.state_version, source, steps)
            result = planned.get("result", {})
            candidate = planned.get("candidate")
            record.update(result={key: result.get(key) for key in (
                "ok", "phase", "error", "line", "logs", "computeMs", "snapshotId",
                "codeRevision", "steps", "expandedNodes")}, candidate=candidate)
            if vehicle.route_invalidated:
                trace.route_invalidated_events += 1
            if not result.get("ok") or result.get("phase") not in ("verified", "kept"):
                raise ValueError(result.get("error") or f"custom route: {result.get('phase', 'invalid')}")
            if cancelled():
                record["status"] = "cancelled"
                trace.error = "cancelled"
                break
            keep = result["phase"] == "kept"
            if not keep and (not candidate or not candidate.get("ok") or
                             candidate.get("basedOnStateVersion") != vehicle.state_version):
                raise ValueError("custom candidate missing or stale")
            trace.action_attempts += 1
            try:
                ack = client.submit_action(session, ActionRequest(
                    decision_id=decision_id, vehicle_id=vehicle_id,
                    kind="keep_route" if keep else "commit_route",
                    candidate_id=None if keep else candidate["candidateId"],
                    based_on_state_version=vehicle.state_version, reason_code="benchmark_custom_code"))
                if not ack.accepted:
                    raise EnvironmentError(409, ack.reason or "action rejected")
            except EnvironmentError as error:
                if 400 <= error.status_code < 500:
                    trace.action_rejections += 1
                else:
                    trace.action_failures += 1
                raise
            if keep:
                trace.keeps += 1
            else:
                trace.commits += 1
                edges = candidate.get("edges", [])
                if edges and vehicle.remaining_edge_ids:
                    if edges == vehicle.remaining_edge_ids:
                        trace.unchanged_route_requests += 1
                    else:
                        trace.route_change_requests += 1
            record["status"] = "queued"
            # Complete an acknowledged action before allowing cancellation.
            advanced = client.step(session, ticks=1)
            observed = client.observe(session)
            after = next(item for item in observed.agents if item.vehicle_id == vehicle_id)
            if not keep and after.route_id == previous.route_id:
                raise ValueError("accepted route was not applied at the next tick")
            record["status"] = "kept" if keep else "applied"
            trace.decision_latency_ms += (time.monotonic() - begin) * 1000
            decision_id = advanced.decision_id
    except Exception as error:
        trace.error = str(error)
        if trace.custom_decisions and trace.custom_decisions[-1]["status"] not in ("applied", "kept"):
            record = trace.custom_decisions[-1]
            record.update(error=str(error))
            if record["status"] != "queued":
                record["status"] = "error"
    finally:
        try:
            final = client.observe_vehicle(session, vehicle_id)
            trace.final_observation, trace.finished = final, final.finished
            trace.arrived, trace.ticks = final.state == "arrived", final.tick
            if trace.finished:
                trace.environment_result = client.result(session)
        except Exception as error:
            trace.result_error = str(error)
            if trace.final_observation is None and trace.error is None:
                trace.error = str(error)
        trace.wall_seconds = time.monotonic() - started
        try:
            client.close(session)
        except Exception:
            pass
    return trace
