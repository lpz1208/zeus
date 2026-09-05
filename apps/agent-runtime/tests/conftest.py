"""Shared fixtures: an httpx.MockTransport fake of the agent session API.

The fake is a small scripted state machine — no real server needed:
- version advances by one per step / step(untilEvent)
- the Nth event step returns decisionDue with a scripted reason
- plan returns per-algorithm candidates (dijkstra/astar differ)
- actions enforce basedOnStateVersion == current version and can be
  scripted to fail once (409) to exercise the fallback path
"""

from __future__ import annotations

import json

import httpx
import pytest

from zeus_agent.client import HttpEnvironmentClient

TOOLS = {
    "registryVersion": "routing-tools-v1",
    "algorithms": [
        {"algorithmId": "dijkstra", "algorithmVersion": "1", "searchDirection": "forward"},
        {"algorithmId": "astar", "algorithmVersion": "1", "searchDirection": "forward"},
    ],
}


class FakeEnvironment:
    def __init__(self, *, fail_first_commit: bool = False) -> None:
        self.version = 1
        self.tick = 0
        self.finished = False
        self.vehicle_state = "waiting"
        self.eta = 900.0
        self.invalidated = False
        self.event_reasons = ["route_invalidated", "periodic", "periodic"]
        self.commits = 0
        self.keeps = 0
        self.actions: list[dict] = []
        self.planned_algorithms: list[str] = []
        self.requests: list[tuple[str, str]] = []
        self.fail_first_commit = fail_first_commit
        self.session_closed = False
        self.session_lost = False
        self.snapshot_created = False
        self.restores = 0

    # -- response fragments ---------------------------------------------------

    def state_header(self, due: bool = False, reason: str = "") -> dict:
        return {
            "tick": self.tick,
            "simulationTimeS": float(self.tick),
            "stateVersion": self.version,
            "finished": self.finished,
            "cancelled": False,
            "decisionDue": due,
            "decisionReason": reason,
            "agentVehicleIds": [0],
        }

    def vehicle_payload(self) -> dict:
        return {
            **self.state_header(self.invalidated, "route_invalidated"
                                if self.invalidated else ""),
            "vehicleId": 0,
            "state": self.vehicle_state,
            "position": {"edgeId": 7 if self.vehicle_state == "driving" else 4294967295,
                         "offsetM": 12.5},
            "destinationEdgeId": 99,
            "remainingEtaS": self.eta,
            "routeInvalidated": self.invalidated,
            "remainingEdgeIds": [7, 8, 99],
            "nearbyRoads": [],
            "activeEvents": [],
        }

    # -- handler ---------------------------------------------------------------

    def handle(self, request: httpx.Request) -> httpx.Response:
        path = request.url.path
        method = request.method
        self.requests.append((method, path))
        body = {}
        if request.content:
            body = json.loads(request.content)

        if path.endswith("/agent/tools"):
            return httpx.Response(200, json=TOOLS)

        if path.endswith("/agent/sessions") and method == "POST":
            self.version, self.tick = 1, 0
            self.finished = False
            self.vehicle_state = "waiting"
            self.eta = 900.0
            self.invalidated = False
            self.commits = self.keeps = 0
            self.actions = []
            self.planned_algorithms = []
            self.session_closed = False
            self.session_lost = False
            return httpx.Response(200, json={
                **self.state_header(),
                "sessionId": "ses_test",
                "ready": True, "paused": True,
                "vehicles": len(body.get("vehicles", [])),
                "agents": [0],
            })

        if path.endswith("/agent/snapshots/snp_test/restore") and method == "POST":
            self.session_lost = False
            self.restores += 1
            return httpx.Response(200, json={
                "snapshotId": "snp_test",
                "storage": "durable_replay_v1",
                "decisionId": "dec_restored",
                "state": {
                    **self.state_header(True, "route_invalidated"),
                    "sessionId": "ses_test",
                },
            })

        if "/agent/sessions/" not in path:
            return httpx.Response(404, json={"error": f"unmatched path {path}"})
        tail = path.split("/agent/sessions/", 1)[1]

        if self.session_lost:
            return httpx.Response(404, json={"error": "unknown session: ses_test"})

        if tail.startswith("ses_test/agent/"):
            return httpx.Response(200, json=self.vehicle_payload())

        if tail == "ses_test" and method == "GET":
            return httpx.Response(200, json={
                **self.state_header(),
                "counts": {"arrived": 0, "driving": 1, "waiting": 0, "unroutable": 0},
                "edges": [],
                "agents": [self.vehicle_payload() | {"vehicleId": 0, "routeId": 3}],
            })

        if tail == "ses_test" and method == "DELETE":
            self.session_closed = True
            return httpx.Response(200, json={"closed": True})

        if tail == "ses_test/snapshots" and method == "POST":
            self.snapshot_created = True
            return httpx.Response(200, json={
                "snapshotId": "snp_test",
                "sourceSessionId": "ses_test",
                "tick": self.tick,
                "simulationTimeS": float(self.tick),
                "stateVersion": self.version,
                "actionCount": len(self.actions),
                "storage": "durable_replay_v1",
            })

        if tail == "ses_test/result" and method == "GET":
            return httpx.Response(200, json={
                "summary": {
                    "ok": True,
                    "arrived": 1 if self.finished else 0,
                    "vehicles": 1,
                    "unroutable": 0,
                    "avgTravelS": float(self.tick),
                    "minTravelS": float(self.tick),
                    "maxTravelS": float(self.tick),
                    "totalDistanceM": 5000.0,
                    "ticks": self.tick,
                    "routePlans": 1 + self.commits,
                    "rerouteAttempts": self.commits,
                    "rerouteSucceeded": self.commits,
                    "rerouteFailed": 0,
                    "barrierWaitMs": 0.0,
                    "computeMs": 2.5,
                    "deadlock": False,
                },
                "geojson": {"type": "FeatureCollection", "features": []},
                "playback": {
                    "duration_s": float(self.tick),
                    "vehicles": [],
                    "edge_kpis": [
                        {
                            "edge_id": 7,
                            "vehicle_seconds_s": 12.0,
                            "mean_speed_mps": 3.0,
                        },
                        {
                            "edge_id": 8,
                            "vehicle_seconds_s": 20.0,
                            "mean_speed_mps": 8.0,
                        },
                    ],
                },
            })

        if tail == "ses_test/plan" and method == "POST":
            algorithm = body.get("algorithm", "dijkstra")
            self.planned_algorithms.append(algorithm)
            best = algorithm == "astar"
            return httpx.Response(200, json={
                "candidateId": f"cand-{algorithm}",
                "vehicleId": 0,
                "algorithm": algorithm,
                "effectiveAlgorithm": algorithm,
                "basedOnStateVersion": self.version,
                "ok": True,
                "timeS": 300.0 if best else 450.0,
                "lengthM": 5000.0 if best else 6000.0,
                "expandedNodes": 10,
                "edges": [7, 8, 99] if best else [7, 12, 99],
            })

        if tail == "ses_test/step" and method == "POST":
            self.version += 1
            self.tick += 10
            self.vehicle_state = "driving"
            if self.event_reasons:
                reason = self.event_reasons.pop(0)
                self.invalidated = reason == "route_invalidated"
                if self.invalidated:
                    self.eta = 1200.0  # the closure blew up the ETA
                return httpx.Response(200, json={
                    "state": self.state_header(True, reason),
                    "decisionId": f"dec_{self.version}",
                })
            self.finished = True
            self.vehicle_state = "arrived"
            self.eta = 0.0
            return httpx.Response(200, json={"state": self.state_header()})

        if tail == "ses_test/actions" and method == "POST":
            self.actions.append(body)
            if body.get("basedOnStateVersion") != self.version:
                return httpx.Response(
                    409, json={"error": "decision state version is stale"})
            if (
                self.fail_first_commit
                and body.get("kind") == "commit_route"
                and self.commits + self.keeps == 0
            ):
                return httpx.Response(409, json={"error": "decision conflict"})
            if body.get("kind") == "commit_route":
                self.commits += 1
                self.invalidated = False
                self.eta = 300.0
            else:
                self.keeps += 1
            return httpx.Response(200, json={
                "accepted": True, "reason": "", "appliesAtNextTick": True})

        return httpx.Response(404, json={"error": f"unmatched {method} {path}"})


@pytest.fixture
def fake_environment() -> FakeEnvironment:
    return FakeEnvironment()


@pytest.fixture
def fake_client(fake_environment: FakeEnvironment) -> HttpEnvironmentClient:
    transport = httpx.MockTransport(fake_environment.handle)
    return HttpEnvironmentClient(
        map_id="m1", base_url="http://testserver", transport=transport)
