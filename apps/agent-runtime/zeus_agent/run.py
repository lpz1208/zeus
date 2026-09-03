"""CLI entry: run one RulePolicy episode against a live control plane.

    uv run python -m zeus_agent.run --map-id <id> \
        --origin 114.491 30.956 --destination 114.806 30.813

Exit code 0 only when the vehicle arrived and the run finished.
"""

from __future__ import annotations

import argparse
import os
import sys

from zeus_agent.client import HttpEnvironmentClient
from zeus_agent.graph import NODE_ORDER
from zeus_agent.model import OpenAICompatibleModelProvider
from zeus_agent.policy import RulePolicy
from zeus_agent.runner import Scenario, run_episode


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="zeus_agent.run")
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--map-id", required=True)
    parser.add_argument("--origin", nargs=2, type=float, metavar=("LON", "LAT"),
                        default=(114.4911555, 30.9567005))
    parser.add_argument("--destination", nargs=2, type=float, metavar=("LON", "LAT"),
                        default=(114.8064655, 30.8130008))
    parser.add_argument("--duration", type=float, default=1800.0)
    parser.add_argument("--max-decisions", type=int, default=100)
    parser.add_argument("--reroute-interval", type=float, default=30.0)
    parser.add_argument(
        "--driver", choices=("langgraph", "loop"), default="langgraph",
        help="decision graph executor (LangGraph is the default)")
    parser.add_argument(
        "--provider", choices=("rule", "openai-compatible"), default="rule",
        help="decision provider; rule is deterministic and requires no secret")
    parser.add_argument(
        "--model", default=os.getenv("ZEUS_MODEL", ""),
        help="model id for --provider openai-compatible (or ZEUS_MODEL)")
    parser.add_argument(
        "--model-base-url",
        default=os.getenv("ZEUS_MODEL_BASE_URL", "https://api.openai.com/v1"),
        help="Chat Completions-compatible API base URL")
    parser.add_argument(
        "--api-key-env", default="ZEUS_MODEL_API_KEY",
        help="environment variable containing the model API key")
    parser.add_argument("--model-timeout", type=float, default=60.0)
    parser.add_argument(
        "--checkpoint-db",
        help="SQLite LangGraph checkpoint database (required for resume/trace)")
    parser.add_argument(
        "--trace-db",
        help="SQLite DecisionTrace database derived from checkpoint history")
    parser.add_argument(
        "--thread-id",
        help="stable run id; generated automatically for a new run")
    parser.add_argument(
        "--resume", action="store_true",
        help="continue --thread-id from --checkpoint-db without creating a session")
    parser.add_argument(
        "--interrupt-after", choices=NODE_ORDER,
        help="persist and return after this graph node; resume with --resume")
    args = parser.parse_args(argv)

    model_provider = None
    if args.provider == "openai-compatible":
        api_key = os.getenv(args.api_key_env, "")
        if not api_key:
            parser.error(f"{args.api_key_env} is required for openai-compatible provider")
        if not args.model:
            parser.error("--model or ZEUS_MODEL is required for openai-compatible provider")
        model_provider = OpenAICompatibleModelProvider(
            api_key=api_key,
            model=args.model,
            base_url=args.model_base_url,
            timeout_seconds=args.model_timeout,
        )

    scenario = Scenario(
        map_id=args.map_id,
        origin=tuple(args.origin),
        destination=tuple(args.destination),
        duration_seconds=args.duration,
        reroute_interval_seconds=args.reroute_interval,
        max_decisions=args.max_decisions,
    )
    with HttpEnvironmentClient(args.map_id, base_url=args.base_url) as client:
        trace = run_episode(
            client,
            scenario,
            RulePolicy(),
            model=model_provider,
            use_langgraph=args.driver == "langgraph",
            checkpoint_path=args.checkpoint_db,
            trace_path=args.trace_db,
            thread_id=args.thread_id,
            resume=args.resume,
            interrupt_after=args.interrupt_after,
        )
    print(trace.summary())
    if trace.interrupted:
        print(
            "checkpoint saved; resume with --resume "
            f"--thread-id {trace.thread_id}")
        return 0
    return 0 if trace.arrived and trace.finished and not trace.error else 1


if __name__ == "__main__":
    sys.exit(main())
