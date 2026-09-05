from __future__ import annotations

import threading
from http.server import ThreadingHTTPServer

import httpx
import pytest

from test_benchmark_jobs import fake_client_factory, manifest
from zeus_agent.benchmark_jobs import BenchmarkJobManager, BenchmarkJobStore
from zeus_agent.benchmark_service import make_handler


def test_benchmark_http_lifecycle(tmp_path) -> None:
    store = BenchmarkJobStore(tmp_path / "jobs.sqlite")
    manager = BenchmarkJobManager(store, fake_client_factory, max_workers=1)
    try:
        server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(manager))
    except PermissionError:
        manager.close()
        pytest.skip("sandbox does not permit binding a loopback test server")
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base_url = f"http://127.0.0.1:{server.server_port}"
    try:
        with httpx.Client(base_url=base_url) as client:
            health = client.get("/health")
            assert health.status_code == 200
            assert health.json()["service"] == "zeus-benchmark"

            invalid = client.post("/api/benchmarks", json={"name": "missing"})
            assert invalid.status_code == 422

            created = client.post(
                "/api/benchmarks",
                json=manifest().model_dump(by_alias=True, mode="json"),
            )
            assert created.status_code == 202
            job_id = created.json()["jobId"]
            assert created.headers["location"].endswith(job_id)

            completed = manager.wait(job_id)
            assert completed.status == "completed"
            status = client.get(f"/api/benchmarks/{job_id}")
            assert status.status_code == 200
            assert status.json()["completedRuns"] == 1

            result = client.get(f"/api/benchmarks/{job_id}/result")
            assert result.status_code == 200
            assert result.json()["name"] == "job-test"

            jobs = client.get("/api/benchmarks?limit=10")
            assert jobs.status_code == 200
            assert jobs.json()[0]["jobId"] == job_id

            missing = client.post("/api/benchmarks/not-found/cancel")
            assert missing.status_code == 404
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2.0)
        manager.close()
