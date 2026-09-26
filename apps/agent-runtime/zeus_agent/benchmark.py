"""Reproducible navigation benchmark runner and report exporters."""

from __future__ import annotations

import csv
import json
import math
import statistics
import hashlib
import random
import time
from contextlib import AbstractContextManager
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

from zeus_agent.client import EnvironmentClient, RoadControl, VehicleControl
from zeus_agent.model import ModelProvider
from zeus_agent.policy import FixedRoutePolicy, ReactiveAlgorithmPolicy, RulePolicy
from zeus_agent.runner import EpisodeTrace, Scenario, run_episode

SUPPORTED_ALGORITHMS = {"dijkstra", "astar", "bidijkstra", "biastar", "lpa", "dstar", "tddijkstra", "alt", "ch"}


class ManifestModel(BaseModel):
    model_config = ConfigDict(populate_by_name=True, extra="forbid", allow_inf_nan=False)


class RandomRoadEvents(ManifestModel):
    edge_ids: list[int] = Field(alias="edgeIds", min_length=1, max_length=10000)
    count: int = Field(default=1, ge=1, le=100)
    start_seconds: float = Field(default=1, alias="startSeconds", ge=0)
    end_seconds: float = Field(default=300, alias="endSeconds", ge=0)
    duration_seconds: float = Field(default=60, alias="durationSeconds", gt=0)
    action: Literal["close", "speedFactor"] = "close"
    value: float = Field(default=.5, ge=.05, le=1)

    @model_validator(mode="after")
    def validate_pool(self):
        if any(edge < 0 or edge > 0xffffffff for edge in self.edge_ids):
            raise ValueError("random event edge IDs must be uint32")
        if self.end_seconds < self.start_seconds or self.count > len(set(self.edge_ids)):
            raise ValueError("invalid random event window or insufficient distinct edges")
        return self


class BenchmarkScenario(ManifestModel):
    scenario_id: str = Field(alias="id", min_length=1)
    map_id: str = Field(alias="mapId", min_length=1)
    origin: tuple[float, float]
    destination: tuple[float, float]
    duration_seconds: float = Field(default=1800.0, alias="durationSeconds", gt=0)
    step_seconds: float = Field(default=1.0, alias="stepSeconds", gt=0)
    reroute_interval_seconds: float = Field(
        default=30.0, alias="rerouteIntervalSeconds", ge=0)
    reroute_cost_ratio: float = Field(
        default=1.25, alias="rerouteCostRatio", ge=1.01)
    reroute_recovery_interval_seconds: float = Field(default=0, alias="rerouteRecoveryIntervalSeconds", ge=0, le=3600)
    reroute_min_gain_seconds: float = Field(default=0, alias="rerouteMinGainSeconds", ge=0, le=3600)
    reroute_cooldown_seconds: float = Field(default=0, alias="rerouteCooldownSeconds", ge=0, le=3600)
    sample_interval_seconds: float = Field(
        default=10.0, alias="sampleIntervalSeconds", gt=0)
    max_decisions: int = Field(default=100, alias="maxDecisions", gt=0)
    seed: int = Field(default=0, ge=0)
    road_controls: list[RoadControl] = Field(default_factory=list, alias="roadControls")
    vehicle_controls: list[VehicleControl] = Field(
        default_factory=list, alias="vehicleControls")
    random_events: RandomRoadEvents | None = Field(default=None, alias="randomEvents")

    @model_validator(mode="after")
    def validate_events(self):
        if self.random_events:
            spec = self.random_events
            low = math.ceil(spec.start_seconds / self.step_seconds)
            high = math.floor(spec.end_seconds / self.step_seconds)
            if low > high:
                raise ValueError("random event window contains no simulation tick")
            if (high + math.ceil(spec.duration_seconds / self.step_seconds)) * self.step_seconds > self.duration_seconds:
                raise ValueError("random events must finish within the scenario duration")
            if set(spec.edge_ids) & {edge for event in self.road_controls for edge in event.edge_ids}:
                raise ValueError("random event edges must not overlap manually controlled edges")
        return self

    def to_scenario(self, algorithm: str = "astar", repetition: int = 1) -> Scenario:
        controls = list(self.road_controls)
        if self.random_events:
            spec = self.random_events
            generator = random.Random(self.seed + repetition - 1)
            low = math.ceil(spec.start_seconds / self.step_seconds)
            high = math.floor(spec.end_seconds / self.step_seconds)
            if low > high:
                raise ValueError("random event window contains no simulation tick")
            for edge in generator.sample(sorted(set(spec.edge_ids)), spec.count):
                tick = generator.randint(low, high)
                start = tick * self.step_seconds
                end = start + math.ceil(spec.duration_seconds / self.step_seconds) * self.step_seconds
                if end > self.duration_seconds:
                    raise ValueError("rounded random event duration exceeds scenario")
                controls.extend([
                    RoadControl(time_seconds=start, edge_ids=[edge], action=spec.action, value=spec.value),
                    RoadControl(time_seconds=end, edge_ids=[edge],
                                action="open" if spec.action == "close" else "speedFactor", value=1),
                ])
        controls.sort(key=lambda event: event.time_seconds)
        return Scenario(
            map_id=self.map_id,
            origin=self.origin,
            destination=self.destination,
            duration_seconds=self.duration_seconds,
            step_seconds=self.step_seconds,
            algorithm=algorithm,
            reroute_interval_seconds=self.reroute_interval_seconds,
            reroute_cost_ratio=self.reroute_cost_ratio,
            reroute_recovery_interval_seconds=self.reroute_recovery_interval_seconds,
            reroute_min_gain_seconds=self.reroute_min_gain_seconds,
            reroute_cooldown_seconds=self.reroute_cooldown_seconds,
            sample_interval_seconds=self.sample_interval_seconds,
            road_controls=tuple(controls),
            vehicle_controls=tuple(self.vehicle_controls),
            max_decisions=self.max_decisions,
        )


class BenchmarkStrategy(ManifestModel):
    strategy_id: str = Field(alias="id", min_length=1)
    kind: Literal["fixed", "reactive", "rule_agent", "model_agent", "custom_code"]
    algorithm: str = "astar"
    source: str | None = Field(default=None, max_length=32768)
    steps: int = Field(default=5_000_000, ge=1000, le=5_000_000)

    @model_validator(mode="after")
    def validate_algorithm(self) -> "BenchmarkStrategy":
        if self.kind == "custom_code" and (not self.source or not self.source.strip()
                                           or len(self.source.encode()) > 32768):
            raise ValueError("custom_code requires 1–32768 bytes of source")
        if self.kind != "custom_code" and self.source is not None:
            raise ValueError("source is only valid for custom_code strategies")
        if self.algorithm not in SUPPORTED_ALGORITHMS:
            raise ValueError(
                "algorithm must be dijkstra, astar, bidijkstra, biastar, lpa, dstar, tddijkstra, alt or ch")
        return self


class BenchmarkManifest(ManifestModel):
    name: str = Field(min_length=1)
    repetitions: int = Field(default=1, ge=1, le=1000)
    congestion_speed_threshold_mps: float = Field(
        default=5.0, alias="congestionSpeedThresholdMps", ge=0)
    model_input_usd_per_million_tokens: float = Field(
        default=0.0, alias="modelInputUsdPerMillionTokens", ge=0)
    model_output_usd_per_million_tokens: float = Field(
        default=0.0, alias="modelOutputUsdPerMillionTokens", ge=0)
    scenarios: list[BenchmarkScenario] = Field(min_length=1)
    strategies: list[BenchmarkStrategy] = Field(min_length=1)

    @model_validator(mode="after")
    def validate_unique_ids(self) -> "BenchmarkManifest":
        for label, identifiers in (
            ("scenario", [item.scenario_id for item in self.scenarios]),
            ("strategy", [item.strategy_id for item in self.strategies]),
        ):
            if len(identifiers) != len(set(identifiers)):
                raise ValueError(f"duplicate {label} id")
        return self


@dataclass
class BenchmarkRun:
    run_id: str
    scenario_id: str
    strategy_id: str
    strategy_kind: str
    algorithm: str
    execution_mode: str
    model_name: str
    repetition: int
    seed: int
    success: bool
    arrived: bool
    finished: bool
    travel_time_s: float | None
    route_length_m: float | None
    replanning_count: int
    environment_reroute_attempts: int
    congestion_exposure_s: float
    decisions: int
    route_tool_calls: int
    decision_wall_ms: float
    model_latency_ms: float
    model_calls: int
    model_failures: int
    input_tokens: int
    output_tokens: int
    model_cost_usd: float
    simulation_time_s: float
    wall_seconds: float
    real_time_factor: float
    compute_ms: float | None
    error: str | None
    # Missing trace data (including old reports) must not look like zero errors.
    guard_rejections: int | None = None
    action_attempts: int | None = None
    action_rejections: int | None = None
    action_failures: int | None = None
    fallbacks: int | None = None
    route_change_requests: int | None = None
    unchanged_route_requests: int | None = None
    native_route_applications: int | None = None
    native_route_failures: int | None = None
    applied_route_changes: int | None = None
    applied_unchanged_routes: int | None = None
    route_overlap_ratio: float | None = None
    route_reversals: int | None = None
    source_revision: str | None = None
    scenario_revision: str | None = None
    resolved_road_controls: list[dict] = field(default_factory=list)
    custom_decisions: list[dict] = field(default_factory=list)


@dataclass
class MetricSummary:
    mean: float
    p50: float
    p95: float
    minimum: float
    maximum: float


@dataclass
class BenchmarkAggregate:
    scenario_id: str
    strategy_id: str
    runs: int
    successes: int
    success_rate: float
    travel_time_s: MetricSummary | None
    route_length_m: MetricSummary | None
    replanning_count: MetricSummary | None
    congestion_exposure_s: MetricSummary | None
    route_tool_calls: MetricSummary | None
    decision_wall_ms: MetricSummary | None
    model_latency_ms: MetricSummary | None
    model_cost_usd: MetricSummary | None
    real_time_factor: MetricSummary | None
    guard_rejections: MetricSummary | None = None
    action_attempts: MetricSummary | None = None
    action_rejections: MetricSummary | None = None
    action_failures: MetricSummary | None = None
    fallbacks: MetricSummary | None = None
    route_change_requests: MetricSummary | None = None
    unchanged_route_requests: MetricSummary | None = None
    native_route_applications: MetricSummary | None = None
    native_route_failures: MetricSummary | None = None
    applied_route_changes: MetricSummary | None = None
    applied_unchanged_routes: MetricSummary | None = None
    route_overlap_ratio: MetricSummary | None = None
    route_reversals: MetricSummary | None = None


@dataclass
class BenchmarkReport:
    format_version: int
    name: str
    manifest: dict
    started_at: str
    completed_at: str
    wall_seconds: float
    cancelled: bool
    runs: list[BenchmarkRun]
    aggregates: list[BenchmarkAggregate]

    def to_dict(self) -> dict:
        return asdict(self)


ClientFactory = Callable[[str], AbstractContextManager[EnvironmentClient]]
ProgressCallback = Callable[[int, int, BenchmarkRun], None]


def _summary_payload(trace: EpisodeTrace) -> dict:
    result = trace.environment_result or {}
    summary = result.get("summary", {})
    return summary if isinstance(summary, dict) else {}


def _congestion_exposure(trace: EpisodeTrace, threshold_mps: float) -> float:
    result = trace.environment_result or {}
    playback = result.get("playback", {})
    if not isinstance(playback, dict):
        return 0.0
    edge_kpis = playback.get("edge_kpis", [])
    if not isinstance(edge_kpis, list):
        return 0.0
    exposure = 0.0
    for item in edge_kpis:
        if not isinstance(item, dict):
            continue
        speed = _number(item, "mean_speed_mps")
        vehicle_seconds = _number(item, "vehicle_seconds_s")
        if speed is not None and vehicle_seconds is not None and speed < threshold_mps:
            exposure += vehicle_seconds
    return exposure


def _number(payload: dict, key: str) -> float | None:
    value = payload.get(key)
    return float(value) if isinstance(value, (int, float)) else None


def _native_route_metrics(trace: EpisodeTrace) -> dict:
    """Use application records, never the policy's accepted-request counters."""
    unknown = dict.fromkeys((
        "native_route_applications", "native_route_failures", "applied_route_changes",
        "applied_unchanged_routes", "route_overlap_ratio", "route_reversals",
    ))
    playback = (trace.environment_result or {}).get("playback")
    if not isinstance(playback, dict) or type(playback.get("route_comparison_version")) is not int \
            or playback["route_comparison_version"] != 1:
        return unknown
    records = playback.get("reroutes")
    if not isinstance(records, list) or any(
        not isinstance(item, dict) or type(item.get("success")) is not bool
        or type(item.get("vehicle_id")) is not int for item in records
    ):
        return unknown
    vehicle_id = trace.final_observation.vehicle_id if trace.final_observation else 0
    records = [item for item in records if item["vehicle_id"] == vehicle_id]
    applied = [item for item in records if item["success"]]
    metrics = {**unknown, "native_route_applications": len(applied),
               "native_route_failures": len(records) - len(applied)}
    if any(type(item.get("route_changed")) is not bool
           or type(item.get("remaining_overlap_ratio")) not in (int, float)
           or not math.isfinite(item["remaining_overlap_ratio"])
           or not 0 <= item["remaining_overlap_ratio"] <= 1 for item in applied):
        return metrics
    changed = sum(item["route_changed"] for item in applied)
    reversals = None
    if type(playback.get("route_reversal_version")) is int and playback["route_reversal_version"] == 1 \
            and all(type(item.get("route_reversed")) is bool
                    and (not item["route_reversed"] or item["route_changed"]) for item in applied):
        reversals = sum(item["route_reversed"] for item in applied)
    return {**metrics, "applied_route_changes": changed,
            "applied_unchanged_routes": len(applied) - changed,
            "route_reversals": reversals,
            "route_overlap_ratio": statistics.fmean(
                item["remaining_overlap_ratio"] for item in applied) if applied else None}


def _run_result(
    run_id: str,
    scenario_id: str,
    strategy: BenchmarkStrategy,
    repetition: int,
    scenario: BenchmarkScenario,
    trace: EpisodeTrace,
    congestion_threshold: float,
    input_price: float,
    output_price: float,
) -> BenchmarkRun:
    summary = _summary_payload(trace)
    simulation_time = (
        trace.final_observation.simulation_time_s
        if trace.final_observation else trace.ticks * scenario.step_seconds
    )
    decision_wall_ms = (
        trace.decision_latency_ms / trace.decisions if trace.decisions else 0.0
    )
    success = trace.arrived and trace.finished and not trace.error
    return BenchmarkRun(
        run_id=run_id,
        scenario_id=scenario_id,
        strategy_id=strategy.strategy_id,
        strategy_kind=strategy.kind,
        algorithm=strategy.algorithm,
        execution_mode=trace.execution_mode,
        model_name=trace.model_name,
        repetition=repetition,
        seed=scenario.seed,
        success=success,
        arrived=trace.arrived,
        finished=trace.finished,
        travel_time_s=_number(summary, "avgTravelS"),
        route_length_m=_number(summary, "totalDistanceM"),
        replanning_count=trace.commits,
        environment_reroute_attempts=int(summary.get("rerouteAttempts", 0) or 0),
        congestion_exposure_s=_congestion_exposure(trace, congestion_threshold),
        decisions=trace.decisions,
        route_tool_calls=trace.route_tool_calls,
        decision_wall_ms=decision_wall_ms,
        model_latency_ms=(
            trace.model_latency_ms / trace.model_calls if trace.model_calls else 0.0
        ),
        model_calls=trace.model_calls,
        model_failures=trace.model_failures,
        input_tokens=trace.model_input_tokens,
        output_tokens=trace.model_output_tokens,
        model_cost_usd=(
            trace.model_input_tokens * input_price
            + trace.model_output_tokens * output_price
        ) / 1_000_000.0,
        simulation_time_s=simulation_time,
        wall_seconds=trace.wall_seconds,
        real_time_factor=(
            simulation_time / trace.wall_seconds if trace.wall_seconds > 0 else 0.0),
        compute_ms=_number(summary, "computeMs"),
        error=trace.error or trace.result_error,
        guard_rejections=trace.guard_rejections,
        action_attempts=trace.action_attempts,
        action_rejections=trace.action_rejections,
        action_failures=trace.action_failures,
        fallbacks=trace.fallbacks,
        route_change_requests=trace.route_change_requests,
        unchanged_route_requests=trace.unchanged_route_requests,
        **_native_route_metrics(trace),
    )


def _failed_run(
    run_id: str,
    scenario_id: str,
    strategy: BenchmarkStrategy,
    repetition: int,
    seed: int,
    error: Exception,
) -> BenchmarkRun:
    return BenchmarkRun(
        run_id=run_id,
        scenario_id=scenario_id,
        strategy_id=strategy.strategy_id,
        strategy_kind=strategy.kind,
        algorithm=strategy.algorithm,
        execution_mode="",
        model_name="",
        repetition=repetition,
        seed=seed,
        success=False,
        arrived=False,
        finished=False,
        travel_time_s=None,
        route_length_m=None,
        replanning_count=0,
        environment_reroute_attempts=0,
        congestion_exposure_s=0.0,
        decisions=0,
        route_tool_calls=0,
        decision_wall_ms=0.0,
        model_latency_ms=0.0,
        model_calls=0,
        model_failures=0,
        input_tokens=0,
        output_tokens=0,
        model_cost_usd=0.0,
        simulation_time_s=0.0,
        wall_seconds=0.0,
        real_time_factor=0.0,
        compute_ms=None,
        error=str(error),
    )


def _percentile(values: list[float], percentile: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * percentile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def _metric(values: list[float | int | None]) -> MetricSummary | None:
    present = [float(value) for value in values if value is not None]
    if not present:
        return None
    return MetricSummary(
        mean=statistics.fmean(present),
        p50=_percentile(present, 0.50),
        p95=_percentile(present, 0.95),
        minimum=min(present),
        maximum=max(present),
    )


def aggregate_runs(runs: list[BenchmarkRun]) -> list[BenchmarkAggregate]:
    keys = sorted({(run.scenario_id, run.strategy_id) for run in runs})
    aggregates: list[BenchmarkAggregate] = []
    for scenario_id, strategy_id in keys:
        group = [
            run for run in runs
            if run.scenario_id == scenario_id and run.strategy_id == strategy_id
        ]
        successful = [run for run in group if run.success]
        aggregates.append(BenchmarkAggregate(
            scenario_id=scenario_id,
            strategy_id=strategy_id,
            runs=len(group),
            successes=len(successful),
            success_rate=len(successful) / len(group) if group else 0.0,
            travel_time_s=_metric([run.travel_time_s for run in successful]),
            route_length_m=_metric([run.route_length_m for run in successful]),
            replanning_count=_metric([run.replanning_count for run in group]),
            congestion_exposure_s=_metric([
                run.congestion_exposure_s for run in successful]),
            route_tool_calls=_metric([run.route_tool_calls for run in group]),
            decision_wall_ms=_metric([run.decision_wall_ms for run in group]),
            model_latency_ms=_metric([run.model_latency_ms for run in group]),
            model_cost_usd=_metric([run.model_cost_usd for run in group]),
            real_time_factor=_metric([run.real_time_factor for run in group]),
            **{name: _metric([getattr(run, name) for run in group]) for name in (
                "guard_rejections", "action_attempts", "action_rejections",
                "action_failures", "fallbacks", "route_change_requests",
                "unchanged_route_requests",
                "native_route_applications", "native_route_failures", "applied_route_changes",
                "applied_unchanged_routes", "route_overlap_ratio", "route_reversals",
            )},
        ))
    return aggregates


def run_benchmark(
    manifest: BenchmarkManifest,
    client_factory: ClientFactory,
    *,
    model: ModelProvider | None = None,
    progress: ProgressCallback | None = None,
    should_cancel: Callable[[], bool] | None = None,
) -> BenchmarkReport:
    if any(item.kind == "model_agent" for item in manifest.strategies) and model is None:
        raise ValueError("model_agent strategy requires a ModelProvider")
    started_wall = time.monotonic()
    started_at = datetime.now(timezone.utc).isoformat()
    total = len(manifest.scenarios) * len(manifest.strategies) * manifest.repetitions
    runs: list[BenchmarkRun] = []
    cancelled = False
    index = 0
    for scenario_spec in manifest.scenarios:
        for strategy in manifest.strategies:
            for repetition in range(1, manifest.repetitions + 1):
                if should_cancel and should_cancel():
                    cancelled = True
                    break
                index += 1
                run_id = (
                    f"{manifest.name}:{scenario_spec.scenario_id}:"
                    f"{strategy.strategy_id}:{repetition}")
                scenario = scenario_spec.to_scenario(strategy.algorithm, repetition)
                if strategy.kind == "fixed":
                    policy = FixedRoutePolicy()
                    # The environment computes the initial route with the
                    # selected algorithm. A fixed baseline never asks for a
                    # dynamic candidate afterwards.
                    algorithm_ids = ()
                    provider = None
                elif strategy.kind == "reactive":
                    policy = ReactiveAlgorithmPolicy(strategy.algorithm)
                    algorithm_ids = (strategy.algorithm,)
                    provider = None
                else:
                    policy = RulePolicy()
                    algorithm_ids = None
                    provider = model if strategy.kind == "model_agent" else None
                trace = None
                try:
                    with client_factory(scenario.map_id) as client:
                        if strategy.kind == "custom_code":
                            from zeus_agent.custom_episode import run_custom_episode
                            trace = run_custom_episode(client, scenario, strategy.source,
                                                       strategy.steps, should_cancel)
                        else:
                            trace = run_episode(client, scenario, policy, model=provider,
                                algorithm_ids=algorithm_ids, collect_result=True,
                                should_cancel=should_cancel)
                    result = _run_result(
                        run_id, scenario_spec.scenario_id, strategy, repetition,
                        scenario_spec, trace,
                        manifest.congestion_speed_threshold_mps,
                        manifest.model_input_usd_per_million_tokens,
                        manifest.model_output_usd_per_million_tokens,
                    )
                except Exception as error:
                    result = _failed_run(
                        run_id, scenario_spec.scenario_id, strategy, repetition,
                        scenario_spec.seed, error)
                result.seed = scenario_spec.seed + repetition - 1 if scenario_spec.random_events else scenario_spec.seed
                result.source_revision = hashlib.sha256(strategy.source.encode()).hexdigest() if strategy.source else None
                result.scenario_revision = hashlib.sha256(scenario_spec.model_dump_json(by_alias=True).encode()).hexdigest()
                result.resolved_road_controls = [event.model_dump(by_alias=True) for event in scenario.road_controls]
                if trace is not None and strategy.kind == "custom_code":
                    result.custom_decisions = trace.custom_decisions
                runs.append(result)
                if progress:
                    progress(index, total, result)
                if should_cancel and should_cancel():
                    cancelled = True
                    break
            if cancelled:
                break
        if cancelled:
            break
    return BenchmarkReport(
        format_version=5,
        name=manifest.name,
        manifest=manifest.model_dump(by_alias=True, mode="json"),
        started_at=started_at,
        completed_at=datetime.now(timezone.utc).isoformat(),
        wall_seconds=time.monotonic() - started_wall,
        cancelled=cancelled,
        runs=runs,
        aggregates=aggregate_runs(runs),
    )


def export_report(
    report: BenchmarkReport,
    json_path: str | Path,
    csv_path: str | Path | None = None,
) -> None:
    json_file = Path(json_path)
    json_file.parent.mkdir(parents=True, exist_ok=True)
    json_file.write_text(
        json.dumps(report.to_dict(), ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    if csv_path is None:
        return
    csv_file = Path(csv_path)
    csv_file.parent.mkdir(parents=True, exist_ok=True)
    rows = [{key: json.dumps(value, ensure_ascii=False) if isinstance(value, (dict, list)) else value
             for key, value in asdict(run).items()} for run in report.runs]
    with csv_file.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]) if rows else [])
        if rows:
            writer.writeheader()
            writer.writerows(rows)
