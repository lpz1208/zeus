"""Durable benchmark job queue with bounded in-process execution."""

from __future__ import annotations

import json
import sqlite3
import threading
import uuid
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

from zeus_agent.benchmark import (
    BenchmarkManifest,
    BenchmarkReport,
    BenchmarkRun,
    ClientFactory,
    run_benchmark,
)
from zeus_agent.model import ModelProvider

TERMINAL_STATUSES = {"completed", "failed", "cancelled"}


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


@dataclass(frozen=True)
class BenchmarkJob:
    job_id: str
    status: str
    created_at: str
    started_at: str | None
    completed_at: str | None
    total_runs: int
    completed_runs: int
    successful_runs: int
    cancel_requested: bool
    manifest: dict
    error: str | None

    def to_dict(self) -> dict:
        return asdict(self)


class BenchmarkJobNotFound(KeyError):
    pass


class BenchmarkQueueFull(RuntimeError):
    pass


class BenchmarkJobStore:
    """SQLite source of truth; each operation owns its connection."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self._connect() as connection:
            connection.execute("PRAGMA journal_mode=WAL")
            connection.execute("""
                CREATE TABLE IF NOT EXISTS benchmark_jobs (
                    job_id TEXT PRIMARY KEY,
                    status TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    started_at TEXT,
                    completed_at TEXT,
                    total_runs INTEGER NOT NULL,
                    completed_runs INTEGER NOT NULL DEFAULT 0,
                    successful_runs INTEGER NOT NULL DEFAULT 0,
                    cancel_requested INTEGER NOT NULL DEFAULT 0,
                    manifest_json TEXT NOT NULL,
                    report_json TEXT,
                    error TEXT
                )
            """)

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.path, timeout=5.0)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA busy_timeout=5000")
        return connection

    @staticmethod
    def _job(row: sqlite3.Row) -> BenchmarkJob:
        return BenchmarkJob(
            job_id=row["job_id"],
            status=row["status"],
            created_at=row["created_at"],
            started_at=row["started_at"],
            completed_at=row["completed_at"],
            total_runs=row["total_runs"],
            completed_runs=row["completed_runs"],
            successful_runs=row["successful_runs"],
            cancel_requested=bool(row["cancel_requested"]),
            manifest=json.loads(row["manifest_json"]),
            error=row["error"],
        )

    def create(self, manifest: BenchmarkManifest) -> BenchmarkJob:
        job_id = f"bench_{uuid.uuid4().hex}"
        created_at = _now()
        total_runs = (
            len(manifest.scenarios) * len(manifest.strategies) * manifest.repetitions
        )
        manifest_json = manifest.model_dump_json(by_alias=True)
        with self._connect() as connection:
            connection.execute(
                """INSERT INTO benchmark_jobs (
                    job_id, status, created_at, total_runs, manifest_json
                ) VALUES (?, 'queued', ?, ?, ?)""",
                (job_id, created_at, total_runs, manifest_json),
            )
        job = self.get(job_id)
        assert job is not None
        return job

    def get(self, job_id: str) -> BenchmarkJob | None:
        with self._connect() as connection:
            row = connection.execute(
                "SELECT * FROM benchmark_jobs WHERE job_id = ?", (job_id,)
            ).fetchone()
        return self._job(row) if row else None

    def list(self, limit: int = 50) -> list[BenchmarkJob]:
        with self._connect() as connection:
            rows = connection.execute(
                """SELECT * FROM benchmark_jobs
                   ORDER BY created_at DESC LIMIT ?""",
                (max(1, min(limit, 500)),),
            ).fetchall()
        return [self._job(row) for row in rows]

    def active_count(self) -> int:
        with self._connect() as connection:
            row = connection.execute(
                """SELECT COUNT(*) AS count FROM benchmark_jobs
                   WHERE status IN ('queued', 'running')"""
            ).fetchone()
        return int(row["count"])

    def claim(self, job_id: str) -> bool:
        with self._connect() as connection:
            cursor = connection.execute(
                """UPDATE benchmark_jobs
                   SET status = 'running', started_at = ?, completed_at = NULL,
                       completed_runs = 0, successful_runs = 0, error = NULL
                   WHERE job_id = ? AND status = 'queued'
                     AND cancel_requested = 0""",
                (_now(), job_id),
            )
        return cursor.rowcount == 1

    def update_progress(self, job_id: str, completed: int, successful: int) -> None:
        with self._connect() as connection:
            connection.execute(
                """UPDATE benchmark_jobs
                   SET completed_runs = ?, successful_runs = ?
                   WHERE job_id = ? AND status = 'running'""",
                (completed, successful, job_id),
            )

    def cancel_requested(self, job_id: str) -> bool:
        job = self.get(job_id)
        return bool(job and job.cancel_requested)

    def request_cancel(self, job_id: str) -> BenchmarkJob:
        with self._connect() as connection:
            row = connection.execute(
                "SELECT status FROM benchmark_jobs WHERE job_id = ?", (job_id,)
            ).fetchone()
            if row is None:
                raise BenchmarkJobNotFound(job_id)
            status = row["status"]
            if status not in TERMINAL_STATUSES:
                if status == "queued":
                    connection.execute(
                        """UPDATE benchmark_jobs
                           SET status = 'cancelled', cancel_requested = 1,
                               completed_at = ?, error = 'cancelled by user'
                           WHERE job_id = ?""",
                        (_now(), job_id),
                    )
                else:
                    connection.execute(
                        """UPDATE benchmark_jobs SET cancel_requested = 1
                           WHERE job_id = ?""",
                        (job_id,),
                    )
        job = self.get(job_id)
        assert job is not None
        return job

    def finish(
        self,
        job_id: str,
        status: str,
        *,
        report: BenchmarkReport | None = None,
        error: str | None = None,
    ) -> None:
        report_json = (
            json.dumps(report.to_dict(), ensure_ascii=False)
            if report is not None else None
        )
        with self._connect() as connection:
            connection.execute(
                """UPDATE benchmark_jobs
                   SET status = ?, completed_at = ?, report_json = ?, error = ?
                   WHERE job_id = ?""",
                (status, _now(), report_json, error, job_id),
            )

    def result(self, job_id: str) -> dict | None:
        with self._connect() as connection:
            row = connection.execute(
                "SELECT report_json FROM benchmark_jobs WHERE job_id = ?", (job_id,)
            ).fetchone()
        if row is None:
            raise BenchmarkJobNotFound(job_id)
        return json.loads(row["report_json"]) if row["report_json"] else None

    def recover(self) -> list[str]:
        """Requeue interrupted work; cancelled queued jobs stay cancelled."""
        with self._connect() as connection:
            connection.execute(
                """UPDATE benchmark_jobs
                   SET status = 'cancelled', completed_at = ?,
                       error = 'cancelled before service restart'
                   WHERE status IN ('queued', 'running') AND cancel_requested = 1""",
                (_now(),),
            )
            connection.execute(
                """UPDATE benchmark_jobs
                   SET status = 'queued', started_at = NULL,
                       completed_runs = 0, successful_runs = 0,
                       report_json = NULL, error = NULL
                   WHERE status = 'running' AND cancel_requested = 0"""
            )
            rows = connection.execute(
                """SELECT job_id FROM benchmark_jobs
                   WHERE status = 'queued' AND cancel_requested = 0
                   ORDER BY created_at"""
            ).fetchall()
        return [row["job_id"] for row in rows]


BenchmarkRunner = Callable[..., BenchmarkReport]


class BenchmarkJobManager:
    def __init__(
        self,
        store: BenchmarkJobStore,
        client_factory: ClientFactory,
        *,
        model: ModelProvider | None = None,
        max_workers: int = 2,
        max_pending: int = 100,
        runner: BenchmarkRunner = run_benchmark,
        recover: bool = True,
    ) -> None:
        if max_workers < 1 or max_pending < max_workers:
            raise ValueError("max_pending must be >= max_workers >= 1")
        self.store = store
        self.client_factory = client_factory
        self.model = model
        self.max_pending = max_pending
        self.runner = runner
        self._executor = ThreadPoolExecutor(
            max_workers=max_workers, thread_name_prefix="zeus-benchmark"
        )
        self._lock = threading.Lock()
        self._events: dict[str, threading.Event] = {}
        self._futures: dict[str, Future] = {}
        if recover:
            for job_id in self.store.recover():
                self._enqueue(job_id)

    def submit(self, manifest: BenchmarkManifest) -> BenchmarkJob:
        if (
            any(strategy.kind == "model_agent" for strategy in manifest.strategies)
            and self.model is None
        ):
            raise ValueError("model_agent strategy requires a configured model provider")
        with self._lock:
            if self.store.active_count() >= self.max_pending:
                raise BenchmarkQueueFull("benchmark queue is full")
            job = self.store.create(manifest)
            self._enqueue_locked(job.job_id)
        return job

    def _enqueue(self, job_id: str) -> None:
        with self._lock:
            self._enqueue_locked(job_id)

    def _enqueue_locked(self, job_id: str) -> None:
        event = threading.Event()
        self._events[job_id] = event
        self._futures[job_id] = self._executor.submit(
            self._execute, job_id, event
        )

    def _execute(self, job_id: str, event: threading.Event) -> None:
        if not self.store.claim(job_id):
            self._forget(job_id)
            return
        successful = 0

        def cancelled() -> bool:
            return event.is_set() or self.store.cancel_requested(job_id)

        def progress(index: int, total: int, run: BenchmarkRun) -> None:
            nonlocal successful
            del total
            successful += int(run.success)
            self.store.update_progress(job_id, index, successful)

        try:
            job = self.store.get(job_id)
            if job is None:
                return
            manifest = BenchmarkManifest.model_validate(job.manifest)
            report = self.runner(
                manifest,
                self.client_factory,
                model=self.model,
                progress=progress,
                should_cancel=cancelled,
            )
            if report.cancelled or cancelled():
                self.store.finish(
                    job_id,
                    "cancelled",
                    report=report,
                    error="cancelled by user",
                )
            else:
                self.store.finish(job_id, "completed", report=report)
        except Exception as error:
            status = "cancelled" if cancelled() else "failed"
            self.store.finish(job_id, status, error=str(error))
        finally:
            self._forget(job_id)

    def _forget(self, job_id: str) -> None:
        with self._lock:
            self._events.pop(job_id, None)
            self._futures.pop(job_id, None)

    def cancel(self, job_id: str) -> BenchmarkJob:
        job = self.store.request_cancel(job_id)
        with self._lock:
            event = self._events.get(job_id)
            future = self._futures.get(job_id)
            if event:
                event.set()
            if future and job.status == "cancelled":
                if future.cancel():
                    self._events.pop(job_id, None)
                    self._futures.pop(job_id, None)
        updated = self.store.get(job_id)
        assert updated is not None
        return updated

    def wait(self, job_id: str, timeout: float = 10.0) -> BenchmarkJob:
        with self._lock:
            future = self._futures.get(job_id)
        if future:
            future.result(timeout=timeout)
        job = self.store.get(job_id)
        if job is None:
            raise BenchmarkJobNotFound(job_id)
        return job

    def close(self, wait: bool = True) -> None:
        self._executor.shutdown(wait=wait, cancel_futures=False)
