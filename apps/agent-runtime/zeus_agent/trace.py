"""Print one persisted DecisionTrace as JSON.

    uv run python -m zeus_agent.trace --db .runs/traces.sqlite \
        --thread-id <thread-id>
"""

from __future__ import annotations

import argparse
import json
import sys

from zeus_agent.persistence import DecisionTraceStore


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="zeus_agent.trace")
    parser.add_argument("--db", required=True, help="DecisionTrace SQLite database")
    parser.add_argument("--thread-id", required=True)
    parser.add_argument(
        "--node",
        help="only include one graph node (observe, decide, guard, act, ...)")
    args = parser.parse_args(argv)

    with DecisionTraceStore(args.db) as store:
        try:
            document = store.export(args.thread_id)
        except KeyError as error:
            parser.error(str(error))
    if args.node:
        document["events"] = [
            event for event in document["events"]
            if event["node"] == args.node
        ]
    print(json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
