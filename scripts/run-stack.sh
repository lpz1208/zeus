#!/usr/bin/env bash

set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_dir="$(CDPATH= cd -- "$script_dir/.." && pwd)"
cd "$repo_dir"

: ${ZEUS_ADDR:=127.0.0.1:8080}
control_connect_addr="$ZEUS_ADDR"
case "$control_connect_addr" in
  :*) control_connect_addr="127.0.0.1$control_connect_addr" ;;
  0.0.0.0:*) control_connect_addr="127.0.0.1:${control_connect_addr##*:}" ;;
  \[::\]:*) control_connect_addr="[::1]:${control_connect_addr##*:}" ;;
esac
: ${ZEUS_CONTROL_BASE_URL:=http://$control_connect_addr}
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
: ${ZEUS_STARTUP_TIMEOUT:=30}
case "$ZEUS_STARTUP_TIMEOUT" in
  ''|*[!0-9]*|0) printf '%s\n' 'ZEUS_STARTUP_TIMEOUT must be a positive integer.' >&2; exit 2 ;;
esac
# Local service traffic must remain direct even in a terminal with a proxy.
export NO_PROXY="127.0.0.1,localhost,::1${NO_PROXY:+,$NO_PROXY}"
export no_proxy="127.0.0.1,localhost,::1${no_proxy:+,$no_proxy}"

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
probe_dir="$(mktemp -d "${TMPDIR:-/tmp}/zeus-readiness.XXXXXX")"

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
  rm -rf "$probe_dir"
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
startup_deadline=$((SECONDS + ZEUS_STARTUP_TIMEOUT))
probe_url="${ZEUS_CONTROL_BASE_URL%/}/api/live"
while true; do
  probe_status=0
  probe_http=$(curl --disable --silent --show-error --fail --connect-timeout 1 --max-time 2 \
    --output "$probe_dir/body" --write-out '%{http_code}' "$probe_url" 2>"$probe_dir/error") || probe_status=$?
  if (( probe_status == 0 )) && [[ "$(cat "$probe_dir/body")" == *'"service":"zeus-control-server"'* ]] && kill -0 "$control_pid" 2>/dev/null; then
    break
  fi
  if ! kill -0 "$control_pid" 2>/dev/null || (( SECONDS >= startup_deadline )); then
    printf '%s\n' "Zeus startup failed: control API did not become available." \
      "Checked: $probe_url (listen=$ZEUS_ADDR, curl=$probe_status, HTTP=$probe_http)." >&2
    cat "$probe_dir/error" >&2
    if (( probe_status == 0 )); then
      printf '%s\n' 'The URL did not identify the expected running Zeus service; check the port and ZEUS_CONTROL_BASE_URL.' >&2
    fi
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


printf '%s\n' "Zeus stack starting: web=$ZEUS_CONTROL_BASE_URL benchmark=$ZEUS_BENCHMARK_URL"

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
