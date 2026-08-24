#!/usr/bin/env bash
set -euo pipefail

SNAPSHOT=${1:?usage: test_capture_live_snapshot.sh SNAPSHOT_TOOL}
TMP=$(mktemp -d)
root_pid=
session_child_pid=
unrelated_pid=
cleanup() {
  for pid in "$session_child_pid" "$root_pid" "$unrelated_pid"; do
    if [[ -n "$pid" ]]; then
      kill "$pid" 2>/dev/null || true
    fi
  done
  rm -rf "$TMP"
}
trap cleanup EXIT

runtime="$TMP/runtime/q2dz"
session_dir="$runtime/session-tested"
sibling_dir="$runtime/session-unrelated"
mkdir -p "$session_dir" "$sibling_dir"

python3 -c 'import socket,time; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(("127.0.0.1",0)); time.sleep(30)' q2dz-test-unrelated &
unrelated_pid=$!

bash -c '
  python3 -c '\''import socket,time; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(("127.0.0.1",0)); time.sleep(30)'\'' q2dz-test-session-child &
  echo $! > "$1"
  wait
' q2dz-test-session-root "$TMP/session-child.pid" --alias q2dz-test-alias-secret &
root_pid=$!

for _ in $(seq 1 50); do
  [[ -s "$TMP/session-child.pid" ]] && break
  sleep 0.02
done
session_child_pid=$(<"$TMP/session-child.pid")

XDG_RUNTIME_DIR="$TMP/runtime" "$SNAPSHOT" "$TMP/out" "$root_pid" "$session_dir"

grep -Fq "q2dz-test-session-root" "$TMP/out/processes.txt"
grep -Fq "q2dz-test-session-child" "$TMP/out/processes.txt"
! grep -Fq "q2dz-test-unrelated" "$TMP/out/processes.txt"
grep -Fq -- "--alias [REDACTED]" "$TMP/out/processes.txt"
! grep -Fq "q2dz-test-alias-secret" "$TMP/out/processes.txt"
grep -Eq "pid=$session_child_pid([^0-9]|$)" "$TMP/out/udp-sockets.txt"
! grep -Eq "pid=$unrelated_pid([^0-9]|$)" "$TMP/out/udp-sockets.txt"
grep -Fq "$session_dir" "$TMP/out/runtime-tree.txt"
! grep -Fq "$sibling_dir" "$TMP/out/runtime-tree.txt"
