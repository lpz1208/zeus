"""Persistent run metadata and queryable LangGraph decision traces."""

from __future__ import annotations

import json
import sqlite3
from dataclasses import asdict, is_dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def jsonable(value: Any) -> Any:
    """Converts graph values to stable JSON without losing typed models."""
    if hasattr(value, "model_dump"):
        return value.model_dump(mode="json", by_alias=False)
    if is_dataclass(value) and not isinstance(value, type):
        return jsonable(asdict(value))
    if isinstance(value, dict):
        return {str(key): jsonable(item) for key, item in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [jsonable(item) for item in value]
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    return str(value)


def _compact_result(result: dict[str, Any]) -> dict[str, Any]:
    """Drops cumulative graph fields while retaining the node's new event."""
    payload = dict(result)
    events = payload.pop("events", None)
    if events:
        payload["event"] = events[-1]
    return jsonable(payload)


def _state_coordinates(
    result: dict[str, Any], values: dict[str, Any]
) -> tuple[int | None, float | None, int | None, str | None]:
    decision_id = result.get("decision_id") or values.get("decision_id")
    observation = result.get("observation") or values.get("observation")
    if observation is not None:
        return (
            getattr(observation, "tick", None),
            getattr(observation, "simulation_time_s", None),
            getattr(observation, "state_version", None),
            decision_id,
        )
    step = result.get("step") or values.get("step")
    state = getattr(step, "state", None)
    if state is not None:
        return (
            getattr(state, "tick", None),
            getattr(state, "simulation_time_s", None),
            getattr(state, "state_version", None),
            decision_id,
        )
    return (None, None, None, decision_id)


class DecisionTraceStore:
    """SQLite store for runs and exact per-node writes from checkpoint history."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.connection = sqlite3.connect(self.path)
        self.connection.row_factory = sqlite3.Row
        self.connection.execute("PRAGMA journal_mode=WAL")
        self.connection.executescript("""
            CREATE TABLE IF NOT EXISTS zeus_runs (
                thread_id TEXT PRIMARY KEY,
                map_id TEXT NOT NULL,
                session_id TEXT NOT NULL,
                status TEXT NOT NULL,
                execution_mode TEXT NOT NULL,
                environment_snapshot_id TEXT,
                scenario_json TEXT NOT NULL,
                summary_json TEXT,
                started_at TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                completed_at TEXT
            );
            CREATE TABLE IF NOT EXISTS zeus_decision_traces (
                thread_id TEXT NOT NULL,
                checkpoint_step INTEGER NOT NULL,
                task_id TEXT NOT NULL,
                node TEXT NOT NULL,
                tick INTEGER,
                simulation_time_s REAL,
                state_version INTEGER,
                decision_id TEXT,
                payload_json TEXT NOT NULL,
                PRIMARY KEY (thread_id, checkpoint_step, task_id)
            );
            CREATE INDEX IF NOT EXISTS zeus_trace_thread_node
                ON zeus_decision_traces(thread_id, node, checkpoint_step);
        """)
        columns = {
            row[1]
            for row in self.connection.execute("PRAGMA table_info(zeus_runs)")
        }
        if "environment_snapshot_id" not in columns:
            self.connection.execute(
                "ALTER TABLE zeus_runs ADD COLUMN environment_snapshot_id TEXT")
        self.connection.commit()

    def set_environment_snapshot(self, thread_id: str, snapshot_id: str) -> None:
        self.connection.execute(
            """
            UPDATE zeus_runs SET environment_snapshot_id=?, updated_at=?
            WHERE thread_id=?
            """,
            (snapshot_id, _utc_now(), thread_id),
        )
        self.connection.commit()

    def __enter__(self) -> "DecisionTraceStore":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

    def close(self) -> None:
        self.connection.close()

    def start_run(
        self,
        *,
        thread_id: str,
        map_id: str,
        session_id: str,
        execution_mode: str,
        scenario: Any,
    ) -> None:
        now = _utc_now()
        scenario_json = json.dumps(jsonable(scenario), ensure_ascii=False, sort_keys=True)
        self.connection.execute(
            """
            INSERT INTO zeus_runs (
                thread_id, map_id, session_id, status, execution_mode,
                scenario_json, started_at, updated_at
            ) VALUES (?, ?, ?, 'running', ?, ?, ?, ?)
            ON CONFLICT(thread_id) DO UPDATE SET
                map_id=excluded.map_id,
                session_id=excluded.session_id,
                status='running',
                execution_mode=excluded.execution_mode,
                scenario_json=excluded.scenario_json,
                summary_json=NULL,
                completed_at=NULL,
                updated_at=excluded.updated_at
            """,
            (thread_id, map_id, session_id, execution_mode, scenario_json, now, now),
        )
        self.connection.commit()

    def record_history(self, thread_id: str, history: Iterable[Any]) -> int:
        """Upserts every completed node write found in LangGraph history."""
        written = 0
        snapshots = list(history)
        for snapshot in reversed(snapshots):
            step = int((snapshot.metadata or {}).get("step", -1))
            if step < 0:
                continue
            values = dict(snapshot.values or {})
            for task in snapshot.tasks or ():
                result = task.result
                if not isinstance(result, dict):
                    continue
                tick, simulation_time, version, decision_id = _state_coordinates(
                    result, values)
                payload = json.dumps(
                    _compact_result(result), ensure_ascii=False, sort_keys=True)
                self.connection.execute(
                    """
                    INSERT INTO zeus_decision_traces (
                        thread_id, checkpoint_step, task_id, node, tick,
                        simulation_time_s, state_version, decision_id, payload_json
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT(thread_id, checkpoint_step, task_id) DO UPDATE SET
                        node=excluded.node,
                        tick=excluded.tick,
                        simulation_time_s=excluded.simulation_time_s,
                        state_version=excluded.state_version,
                        decision_id=excluded.decision_id,
                        payload_json=excluded.payload_json
                    """,
                    (
                        thread_id, step, str(task.id), task.name, tick,
                        simulation_time, version, decision_id, payload,
                    ),
                )
                written += 1
        self.connection.commit()
        return written

    def finish_run(self, thread_id: str, status: str, summary: Any) -> None:
        now = _utc_now()
        completed_at = now if status in {"completed", "failed"} else None
        self.connection.execute(
            """
            UPDATE zeus_runs SET status=?, summary_json=?, updated_at=?, completed_at=?
            WHERE thread_id=?
            """,
            (
                status,
                json.dumps(jsonable(summary), ensure_ascii=False, sort_keys=True),
                now,
                completed_at,
                thread_id,
            ),
        )
        self.connection.commit()

    def get_run(self, thread_id: str) -> dict[str, Any] | None:
        row = self.connection.execute(
            "SELECT * FROM zeus_runs WHERE thread_id=?", (thread_id,)
        ).fetchone()
        if row is None:
            return None
        result = dict(row)
        result["scenario"] = json.loads(result.pop("scenario_json"))
        summary = result.pop("summary_json")
        result["summary"] = json.loads(summary) if summary else None
        return result

    def list_events(self, thread_id: str) -> list[dict[str, Any]]:
        rows = self.connection.execute(
            """
            SELECT checkpoint_step, task_id, node, tick, simulation_time_s,
                   state_version, decision_id, payload_json
            FROM zeus_decision_traces WHERE thread_id=?
            ORDER BY checkpoint_step, task_id
            """,
            (thread_id,),
        ).fetchall()
        events: list[dict[str, Any]] = []
        for row in rows:
            event = dict(row)
            event["payload"] = json.loads(event.pop("payload_json"))
            events.append(event)
        return events

    def export(self, thread_id: str) -> dict[str, Any]:
        run = self.get_run(thread_id)
        if run is None:
            raise KeyError(f"unknown thread_id: {thread_id}")
        return {"run": run, "events": self.list_events(thread_id)}
