from __future__ import annotations

import csv
import json

import httpx
import pytest
from pydantic import ValidationError

from conftest import FakeEnvironment
from zeus_agent.benchmark import (
    BenchmarkManifest,
    aggregate_runs,
    export_report,
    run_benchmark,
)
from zeus_agent.client import HttpEnvironmentClient
from zeus_agent.model import MockModelProvider


def manifest_payload() -> dict:
    return {
        "name": "closure-comparison",
        "repetitions": 2,
        "congestionSpeedThresholdMps": 5.0,
        "modelInputUsdPerMillionTokens": 1.0,
        "modelOutputUsdPerMillionTokens": 4.0,
        "scenarios": [{
            "id": "closure-1",
            "mapId": "m1",
            "origin": [114.1, 30.1],
            "destination": [114.2, 30.2],
            "seed": 42,
        }],
        "strategies": [
            {"id": "fixed-a-star", "kind": "fixed", "algorithm": "astar"},
            {"id": "reactive-a-star", "kind": "reactive", "algorithm": "astar"},
            {"id": "rule", "kind": "rule_agent", "algorithm": "astar"},
            {"id": "model", "kind": "model_agent", "algorithm": "astar"},
        ],
    }


def test_manifest_rejects_duplicates_and_unknown_algorithm() -> None:
    payload = manifest_payload()
    payload["strategies"][1]["id"] = "fixed-a-star"
    with pytest.raises(ValidationError, match="duplicate strategy id"):
        BenchmarkManifest.model_validate(payload)

    payload = manifest_payload()
    payload["strategies"][0]["algorithm"] = "unknown"
    with pytest.raises(ValidationError, match="algorithm must be"):
        BenchmarkManifest.model_validate(payload)


def test_benchmark_runs_matrix_and_exports_reports(tmp_path) -> None:
    manifest = BenchmarkManifest.model_validate(manifest_payload())
    environments: list[FakeEnvironment] = []

    def factory(map_id: str) -> HttpEnvironmentClient:
        assert map_id == "m1"
        environment = FakeEnvironment()
        environments.append(environment)
        return HttpEnvironmentClient(
            map_id=map_id,
            base_url="http://testserver",
            transport=httpx.MockTransport(environment.handle),
        )

    progress: list[tuple[int, int, bool]] = []
    report = run_benchmark(
        manifest,
        factory,
        model=MockModelProvider(),
        progress=lambda index, total, run: progress.append(
            (index, total, run.success)
        ),
    )

    assert len(report.runs) == 8
    assert len(report.aggregates) == 4
    assert all(run.success for run in report.runs)
    assert all(run.travel_time_s == 40.0 for run in report.runs)
    assert all(run.route_length_m == 5000.0 for run in report.runs)
    assert all(run.congestion_exposure_s == 12.0 for run in report.runs)
    assert all(run.seed == 42 for run in report.runs)
    assert all(run.decision_wall_ms > 0 for run in report.runs)
    assert report.runs[0].route_tool_calls == 0
    assert report.runs[2].route_tool_calls == 3
    assert report.runs[4].route_tool_calls == 6
    assert progress[-1] == (8, 8, True)

    # Fixed routing uses only the initial session route. Reactive routing asks
    # only A*, while both agents may compare the full advertised registry.
    assert environments[0].planned_algorithms == []
    assert not any(path.endswith("/agent/tools") for _, path in environments[0].requests)
    assert set(environments[2].planned_algorithms) == {"astar"}
    assert set(environments[4].planned_algorithms) == {"dijkstra", "astar"}
    assert set(environments[6].planned_algorithms) == {"dijkstra", "astar"}

    json_path = tmp_path / "report.json"
    csv_path = tmp_path / "report.csv"
    export_report(report, json_path, csv_path)
    payload = json.loads(json_path.read_text(encoding="utf-8"))
    assert payload["format_version"] == 1
    assert payload["name"] == manifest.name
    assert payload["manifest"]["scenarios"][0]["seed"] == 42
    assert payload["aggregates"][0]["runs"] == 2
    with csv_path.open(encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    assert len(rows) == 8
    assert rows[0]["scenario_id"] == "closure-1"


def test_model_strategy_requires_provider() -> None:
    manifest = BenchmarkManifest.model_validate(manifest_payload())
    with pytest.raises(ValueError, match="requires a ModelProvider"):
        run_benchmark(manifest, lambda _: None)  # type: ignore[arg-type]


def test_benchmark_stops_between_runs_when_cancelled() -> None:
    payload = manifest_payload()
    payload["strategies"] = payload["strategies"][:1]
    manifest = BenchmarkManifest.model_validate(payload)
    environment = FakeEnvironment()
    cancelled = False

    def factory(map_id: str) -> HttpEnvironmentClient:
        return HttpEnvironmentClient(
            map_id=map_id,
            base_url="http://testserver",
            transport=httpx.MockTransport(environment.handle),
        )

    def progress(index, total, run) -> None:
        nonlocal cancelled
        del index, total, run
        cancelled = True

    report = run_benchmark(
        manifest,
        factory,
        progress=progress,
        should_cancel=lambda: cancelled,
    )
    assert report.cancelled
    assert len(report.runs) == 1


def test_aggregate_empty_input() -> None:
    assert aggregate_runs([]) == []
