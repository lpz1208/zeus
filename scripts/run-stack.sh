#!/usr/bin/env bash

set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_dir="$(CDPATH= cd -- "$script_dir/.." && pwd)"
cd "$repo_dir"

: ${ZEUS_ADDR:=127.0.0.1:8080}
: ${ZEUS_CONTROL_BASE_URL:=http://127.0.0.1:8080}
: ${ZEUS_DATA_DIR:=$repo_dir/data}
: ${ZEUS_WEB_DIR:=$repo_dir/apps/web/dist}
: ${ZEUS_MAP_BIN:=$repo_dir/build/zeus-map}
: ${ZEUS_BENCHMARK_HOST:=127.0.0.1}
: ${ZEUS_BENCHMARK_PORT:=8090}
: ${ZEUS_BENCHMARK_URL:=http://$ZEUS_BENCHMARK_HOST:$ZEUS_BENCHMARK_PORT}
: ${ZEUS_BENCHMARK_DB:=$ZEUS_DATA_DIR/benchmarks.sqlite}
: ${ZEUS_BENCHMARK_WORKERS:=2}
: ${ZEUS_BENCHMARK_MAX_PENDING:=100}
: ${ZEUS_BENCHMARK_MODEL_TIMEOUT:=60}
: ${ZEUS_UV_CACHE_DIR:=$repo_dir/.cache/uv}

command -v uv >/dev/null || {
  printf '%s\n' "Zeus startup failed: uv is required for the Benchmark Job Service." >&2
  exit 127
}

command -v curl >/dev/null || {
  printf '%s\n' "Zeus startup failed: curl is required for control readiness." >&2
  exit 127
}

mkdir -p "$ZEUS_DATA_DIR" "$ZEUS_UV_CACHE_DIR"

benchmark_pid=0
control_pid=0

stop_process() {
  local pid=$1
  if (( pid > 0 )) && kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null || true
  fi
}

cleanup() {
  trap - EXIT INT TERM HUP
  # Keep the control plane alive until interrupted episodes have submitted
  # their safe fallback and closed their C++ sessions.
  stop_process "$benchmark_pid"
  (( benchmark_pid > 0 )) && wait "$benchmark_pid" 2>/dev/null || true
  stop_process "$control_pid"
  (( control_pid > 0 )) && wait "$control_pid" 2>/dev/null || true
}

trap cleanup EXIT
trap 'exit 130' INT TERM HUP

./build/zeus-server \
  --addr "$ZEUS_ADDR" \
  --data-dir "$ZEUS_DATA_DIR" \
  --zeus-map "$ZEUS_MAP_BIN" \
  --web-dir "$ZEUS_WEB_DIR" \
  --benchmark-url "$ZEUS_BENCHMARK_URL" &
control_pid=$!

# Recovery enqueues persisted jobs immediately, so only start it after the
# control API can create sessions. Liveness is sufficient; benchmark readiness
# is expected to be false until the Python service starts.
startup_deadline=$((SECONDS + 30))
until curl --silent --fail --max-time 2 --output /dev/null "$ZEUS_CONTROL_BASE_URL/api/health"; do
  if ! kill -0 "$control_pid" 2>/dev/null || (( SECONDS >= startup_deadline )); then
    printf '%s\n' "Zeus startup failed: control API did not become available." >&2
    exit 1
  fi
  sleep 0.2
done

UV_CACHE_DIR="$ZEUS_UV_CACHE_DIR" uv run --project apps/agent-runtime \
  python -m zeus_agent.benchmark_service \
  --host "$ZEUS_BENCHMARK_HOST" \
  --port "$ZEUS_BENCHMARK_PORT" \
  --base-url "$ZEUS_CONTROL_BASE_URL" \
  --db "$ZEUS_BENCHMARK_DB" \
  --workers "$ZEUS_BENCHMARK_WORKERS" \
  --max-pending "$ZEUS_BENCHMARK_MAX_PENDING" \
  --model-timeout "$ZEUS_BENCHMARK_MODEL_TIMEOUT" &
benchmark_pid=$!


printf '%s\n' "Zeus stack starting: web=http://$ZEUS_ADDR benchmark=$ZEUS_BENCHMARK_URL"

while kill -0 "$control_pid" 2>/dev/null && kill -0 "$benchmark_pid" 2>/dev/null; do
  sleep 0.2
done

exit_status=1
if ! kill -0 "$control_pid" 2>/dev/null; then
  if wait "$control_pid"; then
    exit_status=1
  else
    exit_status=$?
  fi
  printf '%s\n' "Zeus stack stopped: control server exited (status=$exit_status)." >&2
else
  if wait "$benchmark_pid"; then
    exit_status=1
  else
    exit_status=$?
  fi
  printf '%s\n' "Zeus stack stopped: benchmark service exited (status=$exit_status)." >&2
fi

exit "$exit_status"
