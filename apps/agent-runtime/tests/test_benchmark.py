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
    assert payload["format_version"] == 5
    assert payload["name"] == manifest.name
    assert payload["manifest"]["scenarios"][0]["seed"] == 42
    assert payload["aggregates"][0]["runs"] == 2
    with csv_path.open(encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    assert len(rows) == 8
    assert rows[0]["scenario_id"] == "closure-1"
    assert payload["runs"][0]["action_attempts"] == 3
    assert payload["runs"][0]["route_change_requests"] == 0
    assert payload["aggregates"][0]["action_rejections"]["mean"] == 0
    assert rows[0]["action_attempts"] == "3"
    assert rows[0]["action_rejections"] == "0"
    assert payload["runs"][0]["native_route_applications"] is None  # Legacy worker fixture.
    assert rows[0]["route_overlap_ratio"] == ""


def test_quality_aggregation_includes_failed_episodes_but_excludes_missing_traces():
    from dataclasses import replace
    from zeus_agent.benchmark import _failed_run

    manifest = BenchmarkManifest.model_validate(manifest_payload())
    missing = _failed_run("missing", "s", manifest.strategies[0], 1, 0, RuntimeError("offline"))
    failed = replace(missing, run_id="failed", action_attempts=2, action_rejections=2,
                     action_failures=0, fallbacks=0, guard_rejections=0,
                     route_change_requests=0, unchanged_route_requests=0,
                     native_route_failures=1, applied_route_changes=0, route_overlap_ratio=None, route_reversals=None)
    success = replace(failed, run_id="ok", success=True, action_rejections=0,
                      route_change_requests=1, native_route_failures=0,
                      applied_route_changes=1, route_overlap_ratio=0.4)
    aggregate = aggregate_runs([missing, failed, success])[0]
    assert aggregate.runs == 3
    assert aggregate.action_rejections.mean == 1
    assert aggregate.action_rejections.p95 == pytest.approx(1.9)
    assert aggregate.route_change_requests.mean == 0.5
    assert aggregate_runs([missing])[0].action_attempts is None
    assert aggregate.native_route_failures.mean == 0.5
    assert aggregate.applied_route_changes.mean == 0.5
    assert aggregate.route_overlap_ratio.mean == 0.4


def test_native_metrics_use_application_evidence_and_ignore_other_vehicles(tmp_path):
    from zeus_agent.benchmark import _run_result, BenchmarkReport
    from zeus_agent.runner import EpisodeTrace

    manifest = BenchmarkManifest.model_validate(manifest_payload())
    trace = EpisodeTrace(session_id="s", commits=9, route_change_requests=8,
                         environment_result={"playback": {
        "route_comparison_version": 1,
        "reroutes": [
            {"vehicle_id": 0, "success": True, "route_changed": True, "remaining_overlap_ratio": .25},
            {"vehicle_id": 0, "success": True, "route_changed": False, "remaining_overlap_ratio": 1.0},
            {"vehicle_id": 0, "success": False, "route_changed": None, "remaining_overlap_ratio": None},
            {"vehicle_id": 1, "success": True, "route_changed": True, "remaining_overlap_ratio": 0.0},
        ],
    }})
    result = _run_result("r", "s", manifest.strategies[0], 1,
                         manifest.scenarios[0], trace, 5, 0, 0)
    assert result.replanning_count == 9
    assert result.native_route_applications == 2
    assert result.native_route_failures == 1
    assert result.applied_route_changes == result.applied_unchanged_routes == 1
    assert result.route_overlap_ratio == .625
    report = BenchmarkReport(3, "native", {}, "", "", 0, False,
                             [result], aggregate_runs([result]))
    export_report(report, tmp_path / "native.json", tmp_path / "native.csv")
    saved = json.loads((tmp_path / "native.json").read_text())
    assert saved["aggregates"][0]["native_route_failures"]["mean"] == 1
    with (tmp_path / "native.csv").open() as source:
        row = next(csv.DictReader(source))
    assert row["native_route_applications"] == "2"
    assert row["route_overlap_ratio"] == "0.625"


@pytest.mark.parametrize("playback", [None, {}, {"reroutes": []},
    {"route_comparison_version": 2, "reroutes": []},
    {"route_comparison_version": 1, "reroutes": "invalid"},
    {"route_comparison_version": 1, "reroutes": [{"success": "true", "vehicle_id": 0}]},
])
def test_native_metrics_missing_or_invalid_evidence_is_unknown(playback):
    from zeus_agent.benchmark import _native_route_metrics
    from zeus_agent.runner import EpisodeTrace

    values = _native_route_metrics(EpisodeTrace("s", environment_result={"playback": playback}))
    assert all(value is None for value in values.values())


@pytest.mark.parametrize("overlap", [None, True, -0.1, 1.1, float("nan"), float("inf")])
def test_invalid_comparison_preserves_counts_but_does_not_fabricate_overlap(overlap):
    from zeus_agent.benchmark import _native_route_metrics
    from zeus_agent.runner import EpisodeTrace

    values = _native_route_metrics(EpisodeTrace("s", environment_result={"playback": {
        "route_comparison_version": 1,
        "reroutes": [{"vehicle_id": 0, "success": True, "route_changed": True,
                      "remaining_overlap_ratio": overlap}],
    }}))
    assert values["native_route_applications"] == 1
    assert values["native_route_failures"] == 0
    assert values["applied_route_changes"] is None
    assert values["route_overlap_ratio"] is None


def test_no_route_application_has_zero_counts_but_no_overlap_sample():
    from zeus_agent.benchmark import _native_route_metrics
    from zeus_agent.runner import EpisodeTrace

    values = _native_route_metrics(EpisodeTrace("s", environment_result={"playback": {
        "route_comparison_version": 1, "reroutes": [],
    }}))
    assert values == dict(native_route_applications=0, native_route_failures=0,
                          applied_route_changes=0, applied_unchanged_routes=0,
                          route_overlap_ratio=None, route_reversals=None)


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


def test_route_reversals_aggregate_and_export_native_evidence(tmp_path):
    from dataclasses import replace
    from zeus_agent.benchmark import _run_result, BenchmarkReport
    from zeus_agent.runner import EpisodeTrace

    manifest = BenchmarkManifest.model_validate(manifest_payload())
    trace = EpisodeTrace('reversals', commits=99, environment_result={'playback': {
        'route_comparison_version': 1, 'route_reversal_version': 1,
        'reroutes': [
            {'vehicle_id': 0, 'success': True, 'route_changed': True,
             'remaining_overlap_ratio': .5, 'route_reversed': False},
            {'vehicle_id': 0, 'success': False, 'route_changed': None,
             'remaining_overlap_ratio': None, 'route_reversed': None},
            {'vehicle_id': 0, 'success': True, 'route_changed': False,
             'remaining_overlap_ratio': 1, 'route_reversed': False},
            {'vehicle_id': 0, 'success': True, 'route_changed': True,
             'remaining_overlap_ratio': .5, 'route_reversed': True},
            {'vehicle_id': 1, 'success': True, 'route_changed': True,
             'remaining_overlap_ratio': .5, 'route_reversed': True},
        ],
    }})
    run = _run_result('r', 's', manifest.strategies[0], 1, manifest.scenarios[0], trace, 5, 0, 0)
    assert run.route_reversals == 1
    assert run.native_route_applications == 3 and run.native_route_failures == 1
    assert run.applied_route_changes == 2 and run.applied_unchanged_routes == 1
    # Failed episodes still contribute evidence; missing traces must not dilute it.
    others = [replace(run, run_id='unknown', route_reversals=None),
              replace(run, run_id='successful', success=True, route_reversals=0)]
    aggregates = aggregate_runs([run, *others])
    assert aggregates[0].route_reversals.mean == .5
    assert aggregates[0].route_reversals.p95 == pytest.approx(.95)
    report = BenchmarkReport(5, 'reversals', {}, '', '', 0, False, [run, *others], aggregates)
    export_report(report, tmp_path / 'report.json', tmp_path / 'report.csv')
    saved = json.loads((tmp_path / 'report.json').read_text())
    assert saved['runs'][0]['route_reversals'] == 1
    assert saved['aggregates'][0]['route_reversals']['mean'] == .5
    with (tmp_path / 'report.csv').open() as stream:
        rows = list(csv.DictReader(stream))
    assert [row['route_reversals'] for row in rows] == ['1', '', '0']


@pytest.mark.parametrize('version,reversed_value,changed,expected', [
    (None, True, True, None), (0, True, True, None), (True, True, True, None),
    (2, True, True, None), (1, None, True, None), (1, 'true', True, None),
    (1, 1, True, None), (1, True, False, None), (1, True, True, 1),
    (1, False, True, 0), (1, False, False, 0),
])
def test_route_reversals_require_versioned_consistent_evidence(version, reversed_value, changed, expected):
    from zeus_agent.benchmark import _native_route_metrics
    from zeus_agent.runner import EpisodeTrace

    values = _native_route_metrics(EpisodeTrace('s', environment_result={'playback': {
        'route_comparison_version': 1, 'route_reversal_version': version,
        'reroutes': [{'vehicle_id': 0, 'success': True, 'route_changed': changed,
                     'remaining_overlap_ratio': 1, 'route_reversed': reversed_value}],
    }}))
    assert values['route_reversals'] == expected
    assert values['native_route_applications'] == 1
    assert values['applied_route_changes'] == int(changed)


def test_partial_reversal_evidence_stays_unknown_and_empty_native_history_is_zero():
    from zeus_agent.benchmark import _native_route_metrics
    from zeus_agent.runner import EpisodeTrace

    playback = {'route_comparison_version': 1, 'route_reversal_version': 1, 'reroutes': []}
    assert _native_route_metrics(EpisodeTrace('s', environment_result={'playback': playback}))['route_reversals'] == 0
    playback['reroutes'] = [
        {'vehicle_id': 0, 'success': True, 'route_changed': True,
         'remaining_overlap_ratio': .5, 'route_reversed': True},
        {'vehicle_id': 0, 'success': True, 'route_changed': True,
         'remaining_overlap_ratio': .5, 'route_reversed': None},
    ]
    values = _native_route_metrics(EpisodeTrace('s', environment_result={'playback': playback}))
    assert values['route_reversals'] is None
    assert values['applied_route_changes'] == 2
