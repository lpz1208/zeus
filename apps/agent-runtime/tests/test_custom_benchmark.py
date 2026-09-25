from __future__ import annotations

import csv
import hashlib
import json

import httpx
import pytest
from pydantic import ValidationError

from conftest import FakeEnvironment
from zeus_agent.benchmark import BenchmarkManifest, export_report, run_benchmark
from zeus_agent.client import HttpEnvironmentClient

SOURCE = 'def route(ctx):\n    return ctx.keep()'


def payload():
    return {
        'name': 'custom-test', 'repetitions': 1,
        'scenarios': [{'id': 's', 'mapId': 'm1', 'origin': [114, 30],
                       'destination': [114.1, 30.1], 'seed': 42}],
        'strategies': [{'id': 'code', 'kind': 'custom_code', 'source': SOURCE}],
    }


class CodeEnvironment(FakeEnvironment):
    def __init__(self, *, phase='verified', reject=0, apply=True, cancel_after_plan=False):
        super().__init__()
        self.phase, self.reject, self.apply = phase, reject, apply
        self.cancel_after_plan, self.cancelled = cancel_after_plan, False
        self.code_calls = 0

    def handle(self, request):
        path = request.url.path
        body = json.loads(request.content) if request.content else {}
        if path.endswith('/decisions'):
            assert body['basedOnStateVersion'] == self.version
            return httpx.Response(200, json={'decisionId': 'initial', 'state': {
                **self.state_header(), 'agents': [self.vehicle_payload() | {'routeId': 3}]}})
        if path.endswith('/algorithms/plan'):
            self.code_calls += 1
            assert body['source'] == SOURCE
            assert body['basedOnStateVersion'] == self.version
            self.cancelled = self.cancel_after_plan
            result = {'ok': self.phase != 'execution', 'phase': self.phase,
                      'logs': ['decision'], 'codeRevision': hashlib.sha256(SOURCE.encode()).hexdigest()}
            if self.phase == 'execution':
                result.update(error='budget exceeded', line=2)
            candidate = {'ok': True, 'candidateId': 'custom', 'edges': [7, 12, 99],
                         'basedOnStateVersion': self.version}
            return httpx.Response(200, json={'result': result, 'candidate': candidate})
        if path.endswith('/actions') and self.reject:
            if self.reject == 200:
                return httpx.Response(200, json={'accepted': False, 'reason': 'rejected'})
            return httpx.Response(self.reject, json={'error': 'rejected'})
        response = super().handle(request)
        if path.endswith('/ses_test') and request.method == 'GET':
            value = response.json()
            value['agents'][0]['routeId'] = 3 + (self.commits if self.apply else 0)
            return httpx.Response(200, json=value)
        return response


def run(environment, data=None):
    return run_benchmark(BenchmarkManifest.model_validate(data or payload()),
        lambda _: HttpEnvironmentClient(map_id='m1', base_url='http://testserver',
            transport=httpx.MockTransport(environment.handle)),
        should_cancel=lambda: environment.cancelled)


@pytest.mark.parametrize('phase', ['verified', 'kept'])
def test_custom_episode_confirms_actions_and_exports_evidence(tmp_path, phase):
    env = CodeEnvironment(phase=phase)
    report = run(env)
    item = report.runs[0]
    assert item.success and env.session_closed
    assert item.execution_mode == 'custom_code'
    assert len(item.custom_decisions) == item.action_attempts == 4
    assert all(record['status'] == ('applied' if phase == 'verified' else 'kept')
               for record in item.custom_decisions)
    assert item.route_change_requests == (4 if phase == 'verified' else 0)
    assert item.source_revision == hashlib.sha256(SOURCE.encode()).hexdigest()
    export_report(report, tmp_path / 'report.json', tmp_path / 'report.csv')
    saved = json.loads((tmp_path / 'report.json').read_text())
    assert saved['manifest']['strategies'][0]['source'] == SOURCE
    with (tmp_path / 'report.csv').open() as stream:
        row = next(csv.DictReader(stream))
    assert json.loads(row['custom_decisions']) == saved['runs'][0]['custom_decisions']
    assert row['source_revision'] == item.source_revision


@pytest.mark.parametrize('options,status,rejections,failures', [
    ({'phase': 'execution'}, 'error', 0, 0),
    ({'phase': 'unreachable'}, 'error', 0, 0),
    ({'reject': 409}, 'error', 1, 0),
    ({'reject': 200}, 'error', 1, 0),
    ({'reject': 503}, 'error', 0, 1),
    ({'apply': False}, 'queued', 0, 0),
    ({'cancel_after_plan': True}, 'cancelled', 0, 0),
])
def test_custom_failure_stops_without_fallback(options, status, rejections, failures):
    env = CodeEnvironment(**options)
    item = run(env).runs[0]
    assert not item.success and env.session_closed
    assert env.code_calls == 1
    assert item.custom_decisions[0]['status'] == status
    assert item.action_rejections == rejections
    assert item.action_failures == failures
    assert item.fallbacks == 0
    if options.get('cancel_after_plan') or options.get('phase'):
        assert not env.actions


def test_last_permitted_decision_can_reach_destination():
    class LongFinalLeg(CodeEnvironment):
        def handle(self, request):
            response = super().handle(request)
            if request.url.path.endswith('/step') and self.tick == 10:
                # One decision, then several ticks of driving without a new event.
                self.finished, self.vehicle_state = False, 'driving'
                return httpx.Response(200, json={'state': self.state_header()})
            return response

    env = LongFinalLeg()
    env.event_reasons = []
    data = payload()
    data['scenarios'][0]['maxDecisions'] = 1
    assert run(env, data).runs[0].success


def test_custom_job_persists_source_and_decisions_across_store_reopen(tmp_path):
    from zeus_agent.benchmark_jobs import BenchmarkJobManager, BenchmarkJobStore

    store = BenchmarkJobStore(tmp_path / 'jobs.sqlite')
    env = CodeEnvironment()
    manager = BenchmarkJobManager(store, lambda _: HttpEnvironmentClient(
        map_id='m1', base_url='http://testserver', transport=httpx.MockTransport(env.handle)))
    try:
        job = manager.submit(BenchmarkManifest.model_validate(payload()))
        completed = manager.wait(job.job_id)
        assert completed.status == 'completed' and completed.successful_runs == 1
        report = store.result(job.job_id)
        assert report['manifest']['strategies'][0]['source'] == SOURCE
        assert len(report['runs'][0]['custom_decisions']) == 4
    finally:
        manager.close()
    assert BenchmarkJobStore(tmp_path / 'jobs.sqlite').result(job.job_id) == report


def random_payload():
    data = payload()
    data['repetitions'] = 2
    data['strategies'].append({'id': 'fixed', 'kind': 'fixed'})
    data['scenarios'][0]['randomEvents'] = {
        'edgeIds': [1, 2, 3, 4, 5], 'count': 2, 'startSeconds': 3,
        'endSeconds': 12, 'durationSeconds': 5, 'action': 'speedFactor', 'value': .3,
    }
    data['scenarios'][0]['stepSeconds'] = 2
    return data


def test_random_events_are_repeatable_aligned_and_shared_between_strategies():
    manifest = BenchmarkManifest.model_validate(random_payload())
    scenario = manifest.scenarios[0]
    first = scenario.to_scenario('astar', 1).road_controls
    assert first == scenario.to_scenario('dijkstra', 1).road_controls
    assert first != scenario.to_scenario('astar', 2).road_controls
    assert all(event.time_seconds % 2 == 0 for event in first)
    assert [event.time_seconds for event in first] == sorted(event.time_seconds for event in first)
    for edge in {event.edge_ids[0] for event in first}:
        events = [event for event in first if edge in event.edge_ids]
        assert len(events) == 2 and events[1].time_seconds - events[0].time_seconds == 6
        assert events[0].value == .3 and events[1].value == 1
    report = run(CodeEnvironment(), random_payload())
    assert [item.seed for item in report.runs] == [42, 43, 42, 43]
    assert report.runs[0].resolved_road_controls == report.runs[2].resolved_road_controls
    assert report.runs[1].resolved_road_controls == report.runs[3].resolved_road_controls


@pytest.mark.parametrize('change', [
    {'edgeIds': [-1]}, {'count': 6}, {'startSeconds': 13},
    {'startSeconds': 3, 'endSeconds': 3}, {'endSeconds': 1800},
    {'endSeconds': float('inf')}, {'durationSeconds': float('nan')},
])
def test_random_events_reject_invalid_windows_and_pools(change):
    data = random_payload()
    data['scenarios'][0]['randomEvents'].update(change)
    with pytest.raises(ValidationError):
        BenchmarkManifest.model_validate(data)


def test_random_events_reject_manual_control_overlap():
    data = random_payload()
    data['scenarios'][0]['roadControls'] = [{'timeSeconds': 1, 'edgeIds': [1], 'action': 'close'}]
    with pytest.raises(ValidationError, match='overlap'):
        BenchmarkManifest.model_validate(data)


@pytest.mark.parametrize('source', ['', ' ', '汉' * 11000])
def test_custom_strategy_requires_bounded_utf8_source(source):
    data = payload()
    data['strategies'][0]['source'] = source
    with pytest.raises(ValidationError):
        BenchmarkManifest.model_validate(data)


def test_recovery_settings_reach_custom_session_and_report():
    data = payload()
    settings = {'rerouteRecoveryIntervalSeconds': 5, 'rerouteMinGainSeconds': 12,
                'rerouteCooldownSeconds': 30}
    data['scenarios'][0].update(settings)
    env = CodeEnvironment()
    base = env.handle
    captured = []

    def handle(request):
        if request.method == 'POST' and request.url.path.endswith('/sessions'):
            captured.append(json.loads(request.content))
        return base(request)

    env.handle = handle
    report = run(env, data)
    assert report.runs[0].success
    assert len(captured) == 1
    assert all(captured[0][key] == value for key, value in settings.items())
    saved = report.manifest['scenarios'][0]
    assert all(saved[key] == value for key, value in settings.items())


@pytest.mark.parametrize('key', ['rerouteRecoveryIntervalSeconds', 'rerouteMinGainSeconds', 'rerouteCooldownSeconds'])
@pytest.mark.parametrize('value', [-1, 3601, float('nan'), float('inf')])
def test_recovery_settings_reject_invalid_manifest(key, value):
    data = payload()
    data['scenarios'][0][key] = value
    with pytest.raises(ValidationError):
        BenchmarkManifest.model_validate(data)
