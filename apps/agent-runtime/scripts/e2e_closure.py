"""End-to-end closure scenario against a live control plane.

Flow: verify the map -> pre-plan the route with the HTTP route API to learn
a mid-route edge -> create an agent session whose road control closes that
edge at ~1/3 of the route time -> run the RulePolicy loop -> assert the
agent noticed the invalidation, committed a different route and arrived.

    uv run python scripts/e2e_closure.py --base-url http://127.0.0.1:8080
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

from zeus_agent.client import HttpEnvironmentClient, RoadControl
from zeus_agent.policy import RulePolicy
from zeus_agent.persistence import DecisionTraceStore
from zeus_agent.runner import Scenario, run_episode

DEFAULT_MAP = "map_71ad2c3c46bb59ae"
DEFAULT_ORIGIN = (114.4911555, 30.9567005)
DEFAULT_DESTINATION = (114.8064655, 30.8130008)


def http_json(base_url: str, method: str, path: str, payload: dict | None = None):
    data = json.dumps(payload).encode() if payload is not None else None
    request = urllib.request.Request(
        base_url + path, data=data, method=method,
        headers={"Content-Type": "application/json"} if data else {})
    with urllib.request.urlopen(request) as response:
        return json.loads(response.read())


def preplan(base_url: str, map_id: str, origin, destination, algorithm: str):
    body = {
        "fromLon": origin[0], "fromLat": origin[1],
        "toLon": destination[0], "toLat": destination[1],
        "algorithm": algorithm, "maxDistance": 100,
    }
    result = http_json(base_url, "POST", f"/api/maps/{map_id}/route", body)
    if not result.get("ok"):
        raise SystemExit(f"pre-plan failed: {result.get('message')}")
    features = (result.get("geojson") or {}).get("features") or []
    edges = [f["properties"]["EDGE_INDEX"] for f in features
             if "EDGE_INDEX" in f.get("properties", {})]
    if len(edges) < 3:
        raise SystemExit(f"pre-plan produced too few edges: {len(edges)}")
    return edges, float(result.get("timeS") or 0.0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--map-id", default=DEFAULT_MAP)
    parser.add_argument("--origin", nargs=2, type=float, metavar=("LON", "LAT"),
                        default=DEFAULT_ORIGIN)
    parser.add_argument("--destination", nargs=2, type=float, metavar=("LON", "LAT"),
                        default=DEFAULT_DESTINATION)
    parser.add_argument("--edge-index-fraction", type=float, default=0.34,
                        help="position of the closed edge along the route")
    parser.add_argument("--duration", type=float, default=1800.0)
    parser.add_argument("--max-decisions", type=int, default=2000)
    parser.add_argument(
        "--verify-resume", action="store_true",
        help="interrupt after the first observation, reopen SQLite, then resume")
    args = parser.parse_args()
    base = args.base_url.rstrip("/")

    maps = http_json(base, "GET", "/api/maps")
    if not any(m.get("id") == args.map_id for m in maps):
        raise SystemExit(f"map {args.map_id} not found; available: "
                         f"{[m.get('id') for m in maps]}")

    edges, route_time_s = preplan(
        base, args.map_id, args.origin, args.destination, "astar")
    index = min(len(edges) - 1, max(1, int(len(edges) * args.edge_index_fraction)))
    closed_edge = edges[index]
    closure_time = max(1.0, round(route_time_s / 3.0, 1))
    # The horizon must comfortably exceed the free-flow trip plus the detour
    # the closure forces; --duration is a floor, not a cap.
    duration = max(args.duration, route_time_s * 2.5 + 300.0)
    print(f"pre-plan: {len(edges)} edges, {route_time_s:.0f}s; "
          f"closing edge {closed_edge} (#{index}) at t={closure_time}s; "
          f"duration {duration:.0f}s")

    scenario = Scenario(
        map_id=args.map_id,
        origin=tuple(args.origin),
        destination=tuple(args.destination),
        duration_seconds=duration,
        reroute_interval_seconds=60.0,
        max_decisions=args.max_decisions,
        road_controls=(RoadControl(
            time_seconds=closure_time, edge_ids=[closed_edge], action="close"),),
    )
    with HttpEnvironmentClient(args.map_id, base_url=base) as client:
        if args.verify_resume:
            with tempfile.TemporaryDirectory(prefix="zeus-agent-e2e-") as directory:
                checkpoint = Path(directory) / "checkpoints.sqlite"
                audit = Path(directory) / "traces.sqlite"
                interrupted = run_episode(
                    client,
                    scenario,
                    RulePolicy(),
                    checkpoint_path=checkpoint,
                    trace_path=audit,
                    thread_id="e2e-closure-resume",
                    interrupt_after="act",
                )
                if not interrupted.interrupted:
                    raise SystemExit("E2E resume setup did not interrupt")
                # Remove the original environment session. Resume must fall
                # back to the durable environment snapshot and replay the
                # already committed action without reusing worker memory.
                client.close(interrupted.session_id)
                trace = run_episode(
                    client,
                    scenario,
                    RulePolicy(),
                    checkpoint_path=checkpoint,
                    trace_path=audit,
                    thread_id="e2e-closure-resume",
                    resume=True,
                )
                with DecisionTraceStore(audit) as store:
                    nodes = {
                        event["node"]
                        for event in store.list_events("e2e-closure-resume")
                    }
                missing = {"observe", "decide", "guard", "act"} - nodes
                if missing:
                    raise SystemExit(
                        f"E2E DecisionTrace missing nodes: {sorted(missing)}")
                if not trace.environment_restored:
                    raise SystemExit("E2E did not restore the environment snapshot")
        else:
            trace = run_episode(client, scenario, RulePolicy())

    print(trace.summary())
    failures = []
    if not trace.finished:
        failures.append("run did not finish")
    if not trace.arrived:
        failures.append("vehicle did not arrive")
    if trace.route_invalidated_events < 1:
        failures.append(
            "no route_invalidated decision fired (closure too early/late? "
            "try --edge-index-fraction)")
    if trace.commits < 1:
        failures.append("agent never committed a new route")
    if trace.final_observation is not None and trace.final_observation.route_invalidated:
        failures.append("final observation still reports an invalidated route")
    if trace.error:
        failures.append(f"episode error: {trace.error}")
    if failures:
        print("E2E FAILED:\n  - " + "\n  - ".join(failures))
        return 1
    suffix = " -> checkpoint/resume/audit" if args.verify_resume else ""
    print(
        "E2E OK: closure -> invalidation -> tool comparison -> commit -> arrival"
        + suffix)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except urllib.error.HTTPError as error:
        print(f"E2E FAILED: HTTP {error.code} {error.read().decode()[:200]}")
        sys.exit(1)
