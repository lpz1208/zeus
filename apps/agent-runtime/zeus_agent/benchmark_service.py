"""HTTP service for durable, bounded benchmark jobs.

    uv run python -m zeus_agent.benchmark_service \
        --base-url http://127.0.0.1:8080 --port 8090
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, urlparse

from pydantic import ValidationError

from zeus_agent.benchmark import BenchmarkManifest
from zeus_agent.benchmark_jobs import (
    BenchmarkJob,
    BenchmarkJobManager,
    BenchmarkJobNotFound,
    BenchmarkJobStore,
    BenchmarkQueueFull,
)
from zeus_agent.client import HttpEnvironmentClient
from zeus_agent.model import OpenAICompatibleModelProvider

MAX_REQUEST_BYTES = 2 << 20


def _job_payload(job: BenchmarkJob) -> dict[str, Any]:
    return {
        "jobId": job.job_id,
        "status": job.status,
        "createdAt": job.created_at,
        "startedAt": job.started_at,
        "completedAt": job.completed_at,
        "totalRuns": job.total_runs,
        "completedRuns": job.completed_runs,
        "successfulRuns": job.successful_runs,
        "cancelRequested": job.cancel_requested,
        "manifest": job.manifest,
        "error": job.error,
    }


def make_handler(
    manager: BenchmarkJobManager,
    *,
    cors_origin: str = "*",
) -> type[BaseHTTPRequestHandler]:
    class BenchmarkHandler(BaseHTTPRequestHandler):
        server_version = "ZeusBenchmark/1"

        def _send(
            self,
            status: int,
            payload: Any,
            headers: dict[str, str] | None = None,
        ) -> None:
            body = (
                b"" if status == 204
                else json.dumps(payload, ensure_ascii=False).encode("utf-8")
            )
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", cors_origin)
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            for key, value in (headers or {}).items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body)

        def _error(self, status: int, message: str) -> None:
            self._send(status, {"error": message})

        def _json_body(self) -> Any:
            raw_length = self.headers.get("Content-Length", "0")
            try:
                length = int(raw_length)
            except ValueError as error:
                raise ValueError("invalid Content-Length") from error
            if length <= 0:
                raise ValueError("request body is required")
            if length > MAX_REQUEST_BYTES:
                raise OverflowError("request body exceeds 2 MiB")
            try:
                return json.loads(self.rfile.read(length))
            except json.JSONDecodeError as error:
                raise ValueError(f"invalid JSON: {error.msg}") from error

        def do_OPTIONS(self) -> None:  # noqa: N802
            self._send(204, {})

        def do_POST(self) -> None:  # noqa: N802
            path = urlparse(self.path).path.rstrip("/")
            if path == "/api/benchmarks":
                try:
                    manifest = BenchmarkManifest.model_validate(self._json_body())
                    job = manager.submit(manifest)
                except OverflowError as error:
                    self._error(413, str(error))
                    return
                except (ValueError, ValidationError) as error:
                    self._error(422, str(error))
                    return
                except BenchmarkQueueFull as error:
                    self._error(429, str(error))
                    return
                self._send(
                    202,
                    _job_payload(job),
                    {"Location": f"/api/benchmarks/{job.job_id}"},
                )
                return

            parts = path.split("/")
            if len(parts) == 5 and parts[1:3] == ["api", "benchmarks"] \
                    and parts[4] == "cancel":
                try:
                    job = manager.cancel(parts[3])
                except BenchmarkJobNotFound:
                    self._error(404, "benchmark job not found")
                    return
                self._send(202, _job_payload(job))
                return
            self._error(404, "not found")

        def do_GET(self) -> None:  # noqa: N802
            parsed = urlparse(self.path)
            path = parsed.path.rstrip("/")
            if path == "/health":
                self._send(200, {"ok": True, "service": "zeus-benchmark"})
                return
            if path == "/api/benchmarks":
                query = parse_qs(parsed.query)
                try:
                    limit = int(query.get("limit", ["50"])[0])
                except ValueError:
                    self._error(400, "limit must be an integer")
                    return
                self._send(200, [_job_payload(job) for job in manager.store.list(limit)])
                return

            parts = path.split("/")
            if len(parts) not in (4, 5) or parts[1:3] != ["api", "benchmarks"]:
                self._error(404, "not found")
                return
            job_id = parts[3]
            job = manager.store.get(job_id)
            if job is None:
                self._error(404, "benchmark job not found")
                return
            if len(parts) == 4:
                self._send(200, _job_payload(job))
                return
            if parts[4] != "result":
                self._error(404, "not found")
                return
            try:
                result = manager.store.result(job_id)
            except BenchmarkJobNotFound:
                self._error(404, "benchmark job not found")
                return
            if result is None:
                self._error(409, f"benchmark job is {job.status}")
                return
            self._send(200, result)

        def log_message(self, format: str, *args: object) -> None:
            print(
                f"benchmark-http {self.address_string()} " + format % args,
                file=sys.stderr,
            )

    return BenchmarkHandler


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="zeus_agent.benchmark_service")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--db", default=".runs/benchmarks.sqlite")
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--max-pending", type=int, default=100)
    parser.add_argument("--cors-origin", default="*")
    parser.add_argument("--model", default=os.getenv("ZEUS_MODEL", ""))
    parser.add_argument(
        "--model-base-url",
        default=os.getenv("ZEUS_MODEL_BASE_URL", "https://api.openai.com/v1"),
    )
    parser.add_argument("--api-key-env", default="ZEUS_MODEL_API_KEY")
    parser.add_argument("--model-timeout", type=float, default=60.0)
    args = parser.parse_args(argv)
    if args.workers < 1 or args.max_pending < args.workers:
        parser.error("--max-pending must be >= --workers >= 1")

    api_key = os.getenv(args.api_key_env, "")
    model_provider = None
    if args.model or api_key:
        if not args.model or not api_key:
            parser.error(
                "both --model/ZEUS_MODEL and the configured API key are required"
            )
        model_provider = OpenAICompatibleModelProvider(
            api_key=api_key,
            model=args.model,
            base_url=args.model_base_url,
            timeout_seconds=args.model_timeout,
        )

    store = BenchmarkJobStore(args.db)
    manager = BenchmarkJobManager(
        store,
        lambda map_id: HttpEnvironmentClient(map_id, base_url=args.base_url),
        model=model_provider,
        max_workers=args.workers,
        max_pending=args.max_pending,
    )
    server = ThreadingHTTPServer(
        (args.host, args.port),
        make_handler(manager, cors_origin=args.cors_origin),
    )
    print(
        f"Zeus Benchmark Job Service listening on "
        f"http://{args.host}:{args.port} (workers={args.workers})"
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        manager.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
