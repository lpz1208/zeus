from __future__ import annotations

import threading

import httpx
import pytest

from conftest import FakeEnvironment
from zeus_agent.benchmark import BenchmarkManifest, BenchmarkReport
from zeus_agent.benchmark_jobs import (
    BenchmarkJobManager,
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
