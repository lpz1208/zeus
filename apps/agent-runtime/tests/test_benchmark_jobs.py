from __future__ import annotations

import asyncio
import threading

import httpx
import pytest

from conftest import FakeEnvironment
from zeus_agent.benchmark import BenchmarkManifest, BenchmarkReport
from zeus_agent.benchmark_jobs import (
    BenchmarkJobManager,
    BenchmarkJobNotFound,
    BenchmarkJobStore,
    BenchmarkQueueFull,
)
from zeus_agent.client import HttpEnvironmentClient


def manifest(repetitions: int = 1) -> BenchmarkManifest:
    return BenchmarkManifest.model_validate({
        "name": "job-test",
        "repetitions": repetitions,
        "scenarios": [{
            "id": "s1",
            "mapId": "m1",
            "origin": [1.0, 2.0],
            "destination": [3.0, 4.0],
            "maxDecisions": 10,
        }],
        "strategies": [{
            "id": "fixed",
            "kind": "fixed",
            "algorithm": "astar",
        }],
    })


def fake_client_factory(map_id: str) -> HttpEnvironmentClient:
    environment = FakeEnvironment()
    return HttpEnvironmentClient(
        map_id=map_id,
        base_url="http://testserver",
        transport=httpx.MockTransport(environment.handle),
    )


def cancelled_report(spec: BenchmarkManifest) -> BenchmarkReport:
    return BenchmarkReport(
        format_version=1,
        name=spec.name,
        manifest=spec.model_dump(by_alias=True, mode="json"),
        started_at="2026-01-01T00:00:00+00:00",
        completed_at="2026-01-01T00:00:01+00:00",
        wall_seconds=1.0,
        cancelled=True,
        runs=[],
        aggregates=[],
    )


def test_job_manager_completes_and_persists_report(tmp_path) -> None:
    store = BenchmarkJobStore(tmp_path / "jobs.sqlite")
    manager = BenchmarkJobManager(store, fake_client_factory, max_workers=1)
    try:
        submitted = manager.submit(manifest())
        completed = manager.wait(submitted.job_id)
        assert completed.status == "completed"
        assert completed.completed_runs == 1
        assert completed.successful_runs == 1
        report = store.result(submitted.job_id)
        assert report is not None
        assert report["format_version"] == 1
        assert report["runs"][0]["route_tool_calls"] == 0
    finally:
        manager.close()


def test_running_job_can_be_cancelled(tmp_path) -> None:
    started = threading.Event()
    cancellation_seen = threading.Event()

    def blocking_runner(spec, client_factory, **kwargs):
        del client_factory
        started.set()
        should_cancel = kwargs["should_cancel"]
        while not should_cancel():
            cancellation_seen.wait(0.01)
        cancellation_seen.set()
        return cancelled_report(spec)

    store = BenchmarkJobStore(tmp_path / "jobs.sqlite")
    manager = BenchmarkJobManager(
        store,
        fake_client_factory,
        max_workers=1,
        runner=blocking_runner,
    )
    try:
        submitted = manager.submit(manifest())
        assert started.wait(1.0)
        requested = manager.cancel(submitted.job_id)
        assert requested.cancel_requested
        completed = manager.wait(submitted.job_id)
        assert cancellation_seen.is_set()
        assert completed.status == "cancelled"
        assert completed.error == "cancelled by user"
        assert store.result(submitted.job_id)["cancelled"] is True
    finally:
        cancellation_seen.set()
        manager.close()


def test_queue_limit_rejects_excess_jobs(tmp_path) -> None:
    release = threading.Event()
    started = threading.Event()

    def blocking_runner(spec, client_factory, **kwargs):
        del client_factory, kwargs
        started.set()
        release.wait(2.0)
        return cancelled_report(spec)

    store = BenchmarkJobStore(tmp_path / "jobs.sqlite")
    manager = BenchmarkJobManager(
        store,
        fake_client_factory,
        max_workers=1,
        max_pending=1,
        runner=blocking_runner,
    )
    try:
        first = manager.submit(manifest())
        assert started.wait(1.0)
        with pytest.raises(BenchmarkQueueFull):
            manager.submit(manifest())
        release.set()
        manager.wait(first.job_id)
    finally:
        release.set()
        manager.close()


def test_queued_job_is_cancelled_without_execution(tmp_path) -> None:
    release = threading.Event()
    started = threading.Event()
    calls = 0

    def blocking_runner(spec, client_factory, **kwargs):
        nonlocal calls
        del client_factory, kwargs
        calls += 1
        started.set()
        release.wait(2.0)
        return cancelled_report(spec)

    store = BenchmarkJobStore(tmp_path / "jobs.sqlite")
    manager = BenchmarkJobManager(
        store,
        fake_client_factory,
        max_workers=1,
        max_pending=2,
        runner=blocking_runner,
    )
    try:
        first = manager.submit(manifest())
        assert started.wait(1.0)
        queued = manager.submit(manifest())
        cancelled = manager.cancel(queued.job_id)
        assert cancelled.status == "cancelled"
        release.set()
        manager.wait(first.job_id)
        assert manager.wait(queued.job_id).status == "cancelled"
        assert calls == 1
    finally:
        release.set()
        manager.close()


def test_store_recovers_interrupted_job_from_beginning(tmp_path) -> None:
    store = BenchmarkJobStore(tmp_path / "jobs.sqlite")
    job = store.create(manifest(repetitions=2))
    assert store.claim(job.job_id)
    store.update_progress(job.job_id, 1, 1)

    recovered = store.recover()
    assert recovered == [job.job_id]
    state = store.get(job.job_id)
    assert state is not None
    assert state.status == "queued"
    assert state.started_at is None
    assert state.completed_runs == 0
    assert state.successful_runs == 0

    manager = BenchmarkJobManager(store, fake_client_factory, max_workers=1)
    try:
        completed = manager.wait(job.job_id)
        assert completed.status == "completed"
        assert completed.completed_runs == 2
    finally:
        manager.close()


def test_manager_close_preserves_running_and_queued_jobs_for_restart(tmp_path) -> None:
    started = threading.Event()
    observed_cancel = threading.Event()

    def cancellable_runner(spec, client_factory, **kwargs):
        del client_factory
        started.set()
        should_cancel = kwargs["should_cancel"]
        while not should_cancel():
            observed_cancel.wait(0.01)
        observed_cancel.set()
        return cancelled_report(spec)

    store = BenchmarkJobStore(tmp_path / "jobs.sqlite")
    manager = BenchmarkJobManager(
        store,
        fake_client_factory,
        max_workers=1,
        max_pending=2,
        runner=cancellable_runner,
    )
    job = manager.submit(manifest())
    assert started.wait(1.0)
    queued = manager.submit(manifest())

    manager.close()

    assert observed_cancel.is_set()
    state = store.get(job.job_id)
    assert state is not None
    assert state.status == "running"
    assert not state.cancel_requested
    queued_state = store.get(queued.job_id)
    assert queued_state is not None
    assert queued_state.status == "queued"
    assert not queued_state.cancel_requested
    with pytest.raises(RuntimeError, match="shutting down"):
        manager.submit(manifest())

    restarted = BenchmarkJobManager(store, fake_client_factory, max_workers=1)
    try:
        assert restarted.wait(job.job_id).status == "completed"
        assert restarted.wait(queued.job_id).status == "completed"
    finally:
        restarted.close()


def test_store_raises_for_unknown_job(tmp_path) -> None:
    store = BenchmarkJobStore(tmp_path / "jobs.sqlite")
    with pytest.raises(BenchmarkJobNotFound):
        store.request_cancel("job_missing")
    with pytest.raises(BenchmarkJobNotFound):
        store.result("job_missing")
    manager = BenchmarkJobManager(
        store, fake_client_factory, max_workers=1, max_pending=2)
    try:
        with pytest.raises(BenchmarkJobNotFound):
            manager.wait("job_missing", timeout=0.1)
    finally:
        manager.close()


@pytest.mark.parametrize("stop_service", [False, True])
def test_pending_model_request_stops_and_resolves_environment(tmp_path, stop_service):
    from zeus_agent.model import OpenAICompatibleModelProvider

    entered = threading.Event()
    aborted = threading.Event()
    environment = FakeEnvironment()

    async def model_handler(_):
        entered.set()
        try:
            await asyncio.sleep(30)
        finally:
            aborted.set()

    model = OpenAICompatibleModelProvider(
        api_key="test", model="test", timeout_seconds=20,
        transport=httpx.MockTransport(model_handler))
    store = BenchmarkJobStore(tmp_path / "jobs.sqlite")
    manager = BenchmarkJobManager(store, lambda map_id: HttpEnvironmentClient(
        map_id, base_url="http://testserver", transport=httpx.MockTransport(environment.handle)),
        model=model, max_workers=1)
    spec = manifest()
    spec.strategies[0].kind = "model_agent"
    try:
        job = manager.submit(spec)
        assert entered.wait(2)
        if stop_service:
            manager.close(wait=False)
        else:
            manager.cancel(job.job_id)
        state = manager.wait(job.job_id, timeout=2)
        assert aborted.is_set()
        assert environment.session_closed
        assert environment.actions[-1]["kind"] == "keep_route"
        assert state.status == ("running" if stop_service else "cancelled")
        assert state.cancel_requested is (not stop_service)
    finally:
        manager.close()


def test_user_cancellation_survives_simultaneous_shutdown(tmp_path):
    entered, release = threading.Event(), threading.Event()

    def runner(spec, client_factory, **kwargs):
        entered.set()
        assert release.wait(2)
        return cancelled_report(spec)

    store = BenchmarkJobStore(tmp_path / "jobs.sqlite")
    manager = BenchmarkJobManager(store, fake_client_factory, runner=runner, max_workers=1)
    try:
        job = manager.submit(manifest())
        assert entered.wait(2)
        manager.cancel(job.job_id)
        manager.close(wait=False)
        release.set()
        assert manager.wait(job.job_id).status == "cancelled"
        assert store.recover() == []
    finally:
        release.set()
        manager.close()
