"""Command-line entry point for reproducible navigation benchmarks."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from pydantic import ValidationError

from zeus_agent.benchmark import (
    BenchmarkManifest,
    BenchmarkRun,
    export_report,
    run_benchmark,
)
from zeus_agent.client import HttpEnvironmentClient
from zeus_agent.model import OpenAICompatibleModelProvider


def _progress(index: int, total: int, run: BenchmarkRun) -> None:
    status = "OK" if run.success else "FAIL"
    print(
        f"[{index:>3}/{total}] {status:<4} "
        f"{run.scenario_id} / {run.strategy_id} / repetition {run.repetition}"
    )
    if run.error:
        print(f"          {run.error}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="zeus_agent.benchmark_cli",
        description="Run scenario × strategy × repetition benchmark matrices.",
    )
    parser.add_argument("--manifest", required=True, help="benchmark JSON manifest")
    parser.add_argument("--output", required=True, help="JSON report path")
    parser.add_argument("--csv", help="optional per-run CSV report path")
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument(
        "--model", default=os.getenv("ZEUS_MODEL", ""),
        help="model id when the manifest contains model_agent",
    )
    parser.add_argument(
        "--model-base-url",
        default=os.getenv("ZEUS_MODEL_BASE_URL", "https://api.openai.com/v1"),
    )
    parser.add_argument("--api-key-env", default="ZEUS_MODEL_API_KEY")
    parser.add_argument("--model-timeout", type=float, default=60.0)
    parser.add_argument(
        "--fail-on-unsuccessful",
        action="store_true",
        help="exit 1 when any navigation run is unsuccessful",
    )
    args = parser.parse_args(argv)

    try:
        manifest = BenchmarkManifest.model_validate_json(
            Path(args.manifest).read_text(encoding="utf-8")
        )
    except (OSError, ValidationError, ValueError) as error:
        parser.error(f"cannot load manifest: {error}")

    model_provider = None
    if any(strategy.kind == "model_agent" for strategy in manifest.strategies):
        api_key = os.getenv(args.api_key_env, "")
        if not api_key:
            parser.error(
                f"{args.api_key_env} is required when manifest contains model_agent"
            )
        if not args.model:
            parser.error("--model or ZEUS_MODEL is required for model_agent")
        model_provider = OpenAICompatibleModelProvider(
            api_key=api_key,
            model=args.model,
            base_url=args.model_base_url,
            timeout_seconds=args.model_timeout,
        )

    report = run_benchmark(
        manifest,
        lambda map_id: HttpEnvironmentClient(map_id, base_url=args.base_url),
        model=model_provider,
        progress=_progress,
    )
    export_report(report, args.output, args.csv)

    print(f"\nreport: {args.output}")
    if args.csv:
        print(f"csv:    {args.csv}")
    for aggregate in report.aggregates:
        travel = (
            f"{aggregate.travel_time_s.mean:.2f}s"
            if aggregate.travel_time_s else "-"
        )
        print(
            f"{aggregate.scenario_id} / {aggregate.strategy_id}: "
            f"success {aggregate.successes}/{aggregate.runs}, mean travel {travel}"
        )
    if args.fail_on_unsuccessful and not all(run.success for run in report.runs):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
