#!/usr/bin/env bash
# Runs the same redis-benchmark workload against kvsd and against a stock
# redis-server, back to back on the same host, and prints both results.
#
# Same host, same flags, same session: the numbers are only comparable if nothing but
# the server under test changes between the two runs.
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR=${BUILD_DIR:-build}
KVSD_PORT=${KVSD_PORT:-6380}
REDIS_PORT=${REDIS_PORT:-6381}
REQUESTS=${REQUESTS:-100000}
CLIENTS=${CLIENTS:-50}
ROUNDS=${ROUNDS:-3}
PIPELINE=${PIPELINE:-1}

KVSD_BIN="$BUILD_DIR/kvsd"
[ -x "$KVSD_BIN" ] || { echo "no kvsd binary at $KVSD_BIN -- run scripts/verify.sh first" >&2; exit 1; }
command -v redis-server >/dev/null || { echo "redis-server not on PATH" >&2; exit 1; }
command -v redis-benchmark >/dev/null || { echo "redis-benchmark not on PATH" >&2; exit 1; }

# Plain variables rather than an array: macOS ships bash 3.2, which has no negative
# array subscripts, and this script has to run on the machine it benchmarks.
KVSD_PID=""
REDIS_PID=""
cleanup() {
  [ -n "$KVSD_PID" ] && kill "$KVSD_PID" 2>/dev/null || true
  [ -n "$REDIS_PID" ] && kill "$REDIS_PID" 2>/dev/null || true
}
trap cleanup EXIT

wait_for_port() {
  for _ in $(seq 100); do
    redis-cli -p "$1" ping >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  echo "server on port $1 never came up" >&2
  exit 1
}

bench() {  # bench <label> <port>
  local label=$1 port=$2
  echo "### $label (port $port)"
  for round in $(seq "$ROUNDS"); do
    # --csv gives one line per test: "test","rps",... Everything else redis-benchmark
    # prints is latency detail that belongs in the writeup, not in a comparison table.
    redis-benchmark -h 127.0.0.1 -p "$port" -t set,get -n "$REQUESTS" -c "$CLIENTS" \
      -P "$PIPELINE" --csv | sed "s/^/round $round: /"
  done
  echo
}

echo "requests=$REQUESTS clients=$CLIENTS pipeline=$PIPELINE rounds=$ROUNDS"
echo

"$KVSD_BIN" --port "$KVSD_PORT" >/tmp/kvsd-bench.log 2>&1 &
KVSD_PID=$!
wait_for_port "$KVSD_PORT"
bench "kvsd" "$KVSD_PORT"
# Stopped before the next server starts, so the two runs never share a CPU.
kill "$KVSD_PID" 2>/dev/null || true
wait "$KVSD_PID" 2>/dev/null || true
KVSD_PID=""

# Persistence off on both sides: kvsd has none yet (Phase 2), so leaving RDB snapshots
# on would measure a feature only one of the two has.
redis-server --port "$REDIS_PORT" --save '' --appendonly no --daemonize no \
  >/tmp/redis-bench.log 2>&1 &
REDIS_PID=$!
wait_for_port "$REDIS_PORT"
bench "redis-server $(redis-server --version | sed 's/.*v=\([^ ]*\).*/\1/')" "$REDIS_PORT"
