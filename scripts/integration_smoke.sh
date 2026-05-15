#!/usr/bin/env bash
set -euo pipefail
BUILD_DIR=${BUILD_DIR:-build}
OUT_DIR=$(mktemp -d)
SERVER_LOG="$OUT_DIR/server.log"
A_LOG="$OUT_DIR/a.log"
B_LOG="$OUT_DIR/b.log"
MODULE_LOG="$OUT_DIR/module.log"

"$BUILD_DIR/accel_server" --accel-port 5500 --b-port 5501 --result-port 5502 --log-file "$SERVER_LOG" &
server_pid=$!
sleep 0.5
"$BUILD_DIR/accel_node_b" --server-host 127.0.0.1 --server-port 5501 --log-file "$B_LOG" &
b_pid=$!
"$BUILD_DIR/accel_node_a" --server-host 127.0.0.1 --accel-port 5500 --result-port 5502 --module-file "$MODULE_LOG" --log-file "$A_LOG" &
a_pid=$!

cleanup() {
  kill "$a_pid" "$b_pid" "$server_pid" 2>/dev/null || true
  sleep 0.5
  kill -KILL "$a_pid" "$b_pid" "$server_pid" 2>/dev/null || true
  wait "$a_pid" "$b_pid" "$server_pid" 2>/dev/null || true
}
trap cleanup EXIT

for _ in {1..30}; do
  if [[ -s "$MODULE_LOG" ]] && (( $(wc -l < "$MODULE_LOG") >= 5 )); then
    echo "smoke test passed; module log: $MODULE_LOG"
    exit 0
  fi
  sleep 0.2
done

echo "module log was not populated" >&2
cat "$SERVER_LOG" >&2 || true
cat "$A_LOG" >&2 || true
cat "$B_LOG" >&2 || true
exit 1
