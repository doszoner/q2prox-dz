#!/usr/bin/env bash
set -euo pipefail

SESSION=$1
MANAGED=$2
HERE=$(cd -- "$(dirname -- "$0")" && pwd -P)
TMP=$(mktemp -d -t q2dz-live-hub.XXXXXX)
trap 'rm -rf -- "$TMP"' EXIT
ADAPTER="$TMP/adapter.json"
cat >"$ADAPTER" <<JSON
{
  "argv": ["--bind", "{internal_endpoint}", "--signal", "wss://net.js-dos.com:444", "--ice-file", "{ice_memfd}", "--alias", "{alias}", "--max-datagram", "1400"],
  "probeTimeoutMs": 2500,
  "probeIntervalMs": 100,
  "roomCloseProbeIdleMs": 5000,
  "roomCloseFailures": 2,
  "maxDatagram": 1400
}
JSON

room_json() {
  local alias=$1
  printf '{"alias":"%s","iceServers":[{"urls":["stun:example.invalid"]}],"ordered":false,"map":"","game":"baseq2","credential":"q2dz-secret-canary"}' "$alias"
}

wait_file() {
  local path=$1
  for _ in $(seq 1 200); do
    [ -f "$path" ] && return 0
    sleep 0.025
  done
  echo "timed out waiting for $path" >&2
  return 1
}

read_source() {
  python3 - "$1" <<'PY'
import http.client, sys
url = sys.argv[1]
host_port = url[len('http://'):].split('/', 1)[0]
host, port = host_port.rsplit(':', 1)
connection = http.client.HTTPConnection(host, int(port), timeout=1)
connection.request('GET', '/servers', headers={'Host': host_port, 'Connection': 'close'})
response = connection.getresponse()
body = response.read()
connection.close()
if response.status != 200:
    raise SystemExit(1)
sys.stdout.buffer.write(body)
PY
}

wait_source_count() {
  local expected=$1
  for _ in $(seq 1 200); do
    if body=$(read_source "$SOURCE_URL" 2>/dev/null); then
      local count
      count=$(grep -Ec '^127.0.0.1:[0-9]+$' <<<"$body" || true)
      if [ "$count" -eq "$expected" ]; then return 0; fi
    fi
    sleep 0.025
  done
  echo "unexpected source count; expected $expected" >&2
  cat "$CASE/output" >&2 || true
  read_source "$SOURCE_URL" >&2 || true
  return 1
}

start_case() {
  local name=$1
  local legacy_mode=${2:-normal}
  local q2_mode=${3:-sleep}
  local grace_ms=${4:-300}
  CASE="$TMP/$name"
  mkdir -p "$CASE"
  mkfifo "$CASE/control"
  Q2DZ_TEST_MARKER="$CASE/mark"   Q2DZ_FAKE_LEGACY_MODE="$legacy_mode"   Q2DZ_FAKE_Q2_MODE="$q2_mode"   Q2DZ_SYNTHETIC_CREDENTIAL='q2dz-secret-canary'     "$SESSION" --gateway "$MANAGED" --legacy-gateway "$HERE/fake_legacy.py"       --adapter "$ADAPTER" --q2 "$HERE/fake_q2.py" --wine "$HERE/fake_wine.sh"       --game 3zb2 --runtime-base "$CASE/runtime" --ready-timeout-ms 1200       --shutdown-grace-ms "$grace_ms" <"$CASE/control" >"$CASE/output" 2>&1 &
  OWNER=$!
  exec {WRITER}>"$CASE/control"
  wait_file "$CASE/mark.q2-source-url"
  wait_file "$CASE/mark.q2-source-empty"
  [ "$(<"$CASE/mark.q2-source-empty")" = 1 ]
  SOURCE_URL=$(<"$CASE/mark.q2-source-url")
}

stop_case() {
  local expected=${1:-0}
  kill -TERM "$OWNER"
  set +e
  wait "$OWNER"
  local rc=$?
  set -e
  exec {WRITER}>&-
  [ "$rc" -eq "$expected" ]
  ! find "$CASE/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
}

send_room() {
  local slot=$1 room=$2 alias=$3
  printf '%s %s %s\n' "$slot" "$room" "$(room_json "$alias")" >&$WRITER
}

export Q2DZ_SYNTHETIC_CREDENTIAL='q2dz-secret-canary'

# Q2 starts exactly once with an empty source; a later room is published on
# the same HTTP URL and the Q2 process is never restarted.
start_case empty-start
send_room 0 room.q2.a q2.room-a
wait_file "$CASE/mark.legacy-Q2_ROOM_A.pid"
wait_source_count 1
gateway_a_pid=$(<"$CASE/mark.legacy-Q2_ROOM_A.pid")
starts=$(<"$CASE/mark.q2-starts")
send_room 0 room.q2.a q2.room-a
sleep 0.1
[ "$(<"$CASE/mark.q2-starts")" = "$starts" ]
[ "$(<"$CASE/mark.legacy-Q2_ROOM_A.pid")" = "$gateway_a_pid" ]
send_room 1 room.q2.b q2.room-b
wait_file "$CASE/mark.legacy-Q2_ROOM_B.pid"
wait_source_count 2
printf '0 -\n' >&$WRITER
wait_source_count 1
stop_case 143

# Two valid roughly-140 KiB commands in one stream must be parsed independently;
# the aggregate input is larger than the per-line limit.
start_case large-lines
python3 - "$CASE/control" <<'PY'
import json, sys
rooms = []
for alias in ('q2.large-a', 'q2.large-b'):
    room = {
        'alias': alias,
        'iceServers': [{'urls': ['stun:example.invalid']}],
        'ordered': False,
        'map': '',
        'game': 'baseq2',
        'credential': 'q2dz-secret-canary',
        'padding': 'x' * 139000,
    }
    rooms.append(json.dumps(room, separators=(',', ':')))
assert all(130000 < len(line.encode()) < 150000 for line in rooms)
with open(sys.argv[1], 'w', encoding='utf-8') as pipe:
    pipe.write('0 room.q2.large-a ' + rooms[0] + '\n')
    pipe.write('1 room.q2.large-b ' + rooms[1] + '\n')
PY
wait_file "$CASE/mark.legacy-Q2_LARGE_A.pid"
wait_file "$CASE/mark.legacy-Q2_LARGE_B.pid"
wait_source_count 2
[ "$(<"$CASE/mark.q2-starts")" = 1 ]
stop_case 143

# A room replacement removes the old endpoint before publishing the new one;
# the Q2 process remains the same and the source URL stays stable.
start_case replace 'q2.room-b=no-ack' sleep-no-probe
send_room 0 room.q2.a q2.room-a
wait_file "$CASE/mark.legacy-Q2_ROOM_A.pid"
wait_source_count 1
q2_pid_before=$(<"$CASE/mark.q2-pid")
send_room 0 room.q2.b q2.room-b
wait_source_count 0
[ "$(<"$CASE/mark.q2-pid")" = "$q2_pid_before" ]
printf '0 -\n' >&$WRITER
wait_source_count 0
stop_case 143

# Q2 normal exit closes the whole hub and leaves no session directory.
start_case normal-exit normal normal-delay
set +e
wait "$OWNER"
normal_exit_rc=$?
set -e
exec {WRITER}>&-
[ "$normal_exit_rc" -eq 0 ]
[ -f "$CASE/mark.finished" ]
[ "$(<"$CASE/mark.q2-starts")" = 1 ]
! find "$CASE/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .

# Closing room A does not remove Q2 or the still-ready room B.
start_case room-close-isolation 'q2.room-a=room-close-after-ack;q2.room-b=normal' sleep-no-probe
send_room 0 room.q2.a q2.room-a
send_room 1 room.q2.b q2.room-b
wait_file "$CASE/mark.legacy-Q2_ROOM_A.pid"
wait_file "$CASE/mark.legacy-Q2_ROOM_B.pid"
wait_source_count 2
isolation_q2_pid=$(<"$CASE/mark.q2-pid")
isolation_b_pid=$(<"$CASE/mark.legacy-Q2_ROOM_B.pid")
for _ in $(seq 1 160); do grep -Fq 'reason=room-closed' "$CASE/output" && break; sleep 0.025; done
grep -Fq 'reason=room-closed' "$CASE/output"
wait_source_count 1
kill -0 "$isolation_q2_pid"
kill -0 "$isolation_b_pid"
stop_case 143

# A hard owner kill is recovered by the guardian, including Q2 and both rooms.
start_case hard-kill normal sleep
send_room 0 room.q2.a q2.room-a
send_room 1 room.q2.b q2.room-b
wait_file "$CASE/mark.legacy-Q2_ROOM_A.pid"
wait_file "$CASE/mark.legacy-Q2_ROOM_B.pid"
hard_q2_pid=$(<"$CASE/mark.q2-pid")
hard_wine_pid=$(<"$CASE/mark.wine-pid")
hard_a_pid=$(<"$CASE/mark.legacy-Q2_ROOM_A.pid")
hard_b_pid=$(<"$CASE/mark.legacy-Q2_ROOM_B.pid")
kill -KILL "$OWNER"
set +e
wait "$OWNER"
hard_rc=$?
set -e
exec {WRITER}>&-
[ "$hard_rc" -eq 137 ]
for _ in $(seq 1 200); do
  if ! find "$CASE/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .; then break; fi
  sleep 0.025
done
! find "$CASE/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
for pid in "$hard_q2_pid" "$hard_wine_pid" "$hard_a_pid" "$hard_b_pid"; do
  ! kill -0 "$pid" 2>/dev/null
done

# A no-ACK readiness failure reaps its legacy child and never starts Q2.
noack_marker="$TMP/noack"
set +e
env -u Q2DZ_SYNTHETIC_CREDENTIAL Q2DZ_TEST_MARKER="$noack_marker" Q2DZ_FAKE_LEGACY_MODE=no-ack \
  timeout --signal=KILL 4s bash -c 'printf "%s" "$1" | "$2" --managed --legacy "$3" --adapter "$4" --bind 127.0.0.1:0' \
  _ "$(room_json q2.test)" "$MANAGED" "$HERE/fake_legacy.py" "$ADAPTER" \
  >"$TMP/noack.output" 2>&1
noack_rc=$?
set -e
[ "$noack_rc" -eq 72 ]
[ "$(<"$noack_marker.legacy-attempts")" = 1 ]
[ "$(wc -l < "$noack_marker.legacy-endpoints")" -eq 1 ]
! kill -0 "$(<"$noack_marker.legacy-Q2_TEST.pid")" 2>/dev/null

# The managed UDP boundary accepts 1400 bytes and drops 1401 in both directions.
boundary_marker="$TMP/datagram-boundary"
env -u Q2DZ_SYNTHETIC_CREDENTIAL Q2DZ_TEST_MARKER="$boundary_marker" Q2DZ_FAKE_LEGACY_MODE=datagram-boundary \
  "$MANAGED" --managed --legacy "$HERE/fake_legacy.py" --adapter "$ADAPTER" --bind 127.0.0.1:0 \
  < <(room_json q2.test) >"$TMP/datagram-boundary.output" 2>&1 &
boundary_gateway=$!
for _ in $(seq 1 100); do
  grep -q 'TRANSPORT_READY .*udp=' "$TMP/datagram-boundary.output" 2>/dev/null && break
  sleep 0.025
done
boundary_endpoint=$(sed -n 's/.*TRANSPORT_READY .*udp=\([^ ]*\).*/\1/p' "$TMP/datagram-boundary.output")
[ -n "$boundary_endpoint" ]
python3 - "$boundary_endpoint" <<'PY'
import socket, sys
host, port = sys.argv[1].rsplit(':', 1)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(.5)
endpoint = (host, int(port))
sock.sendto(b'A' * 1400, endpoint)
assert sock.recvfrom(65535)[0] == b'A' * 1400
sock.sendto(b'B' * 1401, endpoint)
try:
    sock.recvfrom(65535)
    raise AssertionError('1401-byte UDP datagram was forwarded')
except socket.timeout:
    pass
sock.sendto(b'remote-oversize', endpoint)
try:
    sock.recvfrom(65535)
    raise AssertionError('1401-byte transport datagram was forwarded')
except socket.timeout:
    pass
sock.sendto(b'C' * 1399, endpoint)
assert sock.recvfrom(65535)[0] == b'C' * 1399
PY
kill -TERM "$boundary_gateway"
wait "$boundary_gateway"
grep -Fq 'GATEWAY_STOP max_udp_to_transport_datagram=1400 max_transport_to_udp_datagram=1400 oversized_udp_drops=1 oversized_transport_drops=1' "$TMP/datagram-boundary.output"
! grep -Eq 'A{16}|B{16}|C{16}|R{16}' "$TMP/datagram-boundary.output"

# A zero-request source still shuts down promptly.
start_case zero-request normal no-source-request
stop_case 143

# A post-ready failure gets one later replacement, then the room is tombstoned.
start_case retry 'q2.room-a=exit-after-ack' sleep-no-probe
send_room 0 room.q2.a q2.room-a
wait_file "$CASE/mark.legacy-attempts"
for _ in $(seq 1 120); do [ "$(<"$CASE/mark.legacy-attempts")" -ge 1 ] && break; sleep 0.025; done
for _ in $(seq 1 160); do grep -Eq 'reason=(gateway-fatal|gateway-exited|gateway-control-eof)' "$CASE/output" && break; sleep 0.025; done
if ! grep -Eq 'reason=(gateway-fatal|gateway-exited|gateway-control-eof)' "$CASE/output"; then
  cat "$CASE/output" >&2 || true
  cat "$CASE/mark.legacy-attempts" >&2 || true
  exit 1
fi
wait_source_count 0
send_room 0 room.q2.a q2.room-a
for _ in $(seq 1 120); do [ "$(<"$CASE/mark.legacy-attempts")" -ge 2 ] && break; sleep 0.025; done
[ "$(<"$CASE/mark.legacy-attempts")" = 2 ]
sleep 1
send_room 0 room.q2.a q2.room-a
sleep 0.1
[ "$(<"$CASE/mark.legacy-attempts")" = 2 ]
stop_case 143

# Pre-ready failure and semantic rejection are tombstoned without retries.
start_case pre-ready q2.room-a=early-exit-always sleep-no-probe
send_room 0 room.q2.a q2.room-a
wait_file "$CASE/mark.legacy-attempts"
for _ in $(seq 1 120); do grep -Fq 'HUB_GATEWAY_STOP' "$CASE/output" && break; sleep 0.025; done
[ "$(<"$CASE/mark.legacy-attempts")" = 3 ]
for _ in $(seq 1 5); do send_room 0 room.q2.a q2.room-a; done
sleep 0.1
[ "$(<"$CASE/mark.legacy-attempts")" = 3 ]
stop_case 143

start_case semantic
printf '%s %s\n' 0 'room.q2.bad {"alias":"q2.bad","credential":"q2dz-secret-canary"}' >&$WRITER
sleep 0.1
[ ! -e "$CASE/mark.legacy-attempts" ]
for _ in $(seq 1 5); do printf '%s %s %s\n' 0 room.q2.bad "$(room_json q2.bad)" >&$WRITER; done
sleep 0.1
[ ! -e "$CASE/mark.legacy-attempts" ]
stop_case 143

# ROOM_CLOSED tombstones the ID; clear resets it and permits a replacement.
start_case room-close 'q2.room-a=room-close-after-ack' sleep-no-probe
send_room 0 room.q2.a q2.room-a
wait_file "$CASE/mark.legacy-attempts"
for _ in $(seq 1 160); do grep -Fq 'reason=room-closed' "$CASE/output" && break; sleep 0.025; done
grep -Fq 'reason=room-closed' "$CASE/output"
attempts=$(<"$CASE/mark.legacy-attempts")
for _ in $(seq 1 5); do send_room 0 room.q2.a q2.room-a; done
sleep 0.1
[ "$(<"$CASE/mark.legacy-attempts")" = "$attempts" ]
send_room 0 room.q2.b q2.room-b
wait_file "$CASE/mark.legacy-Q2_ROOM_B.pid"
wait_source_count 1
b_attempts=$(<"$CASE/mark.legacy-attempts")
printf '0 -\n' >&$WRITER
send_room 0 room.q2.a q2.room-a
for _ in $(seq 1 160); do [ "$(<"$CASE/mark.legacy-attempts")" -gt "$b_attempts" ] && break; sleep 0.025; done
[ "$(<"$CASE/mark.legacy-attempts")" -gt "$b_attempts" ]
wait_source_count 1
stop_case 143

# Malformed framing, oversized input, and EOF after partial input are fatal.
start_case malformed
printf '2 -
' >&$WRITER
exec {WRITER}>&-
set +e
wait "$OWNER"
rc=$?
set -e
[ "$rc" -eq 65 ]
! find "$CASE/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .

start_case oversized
python3 - "$CASE/control" <<'PY'
import sys
with open(sys.argv[1], 'w', encoding='utf-8') as pipe:
    pipe.write('0 room.q2.big {"alias":"q2.big","iceServers":[{"urls":["stun:example.invalid"]}],"x":"' + ('x' * 263000) + '"}\n')
PY
exec {WRITER}>&-
set +e
wait "$OWNER"
rc=$?
set -e
[ "$rc" -eq 65 ]

start_case partial
printf '0 room.q2.partial ' >&$WRITER
exec {WRITER}>&-
set +e
wait "$OWNER"
rc=$?
set -e
[ "$rc" -eq 65 ]

# A half-sent HTTP header cannot keep shutdown blocked.
start_case half-header
python3 - "$SOURCE_URL" <<'PY' &
import socket, sys, time
url = sys.argv[1]
host, port = url[len('http://'):].split('/', 1)[0].rsplit(':', 1)
sock = socket.create_connection((host, int(port)), timeout=1)
sock.sendall(b'GET /servers HTTP/1.1\r\n')
time.sleep(5)
PY
CLIENT=$!
sleep 0.1
kill -TERM "$OWNER"
exec {WRITER}>&-
set +e
wait "$OWNER"
rc=$?
set -e
[ "$rc" -eq 143 ]
kill "$CLIENT" 2>/dev/null || true
wait "$CLIENT" 2>/dev/null || true
! find "$CASE/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .

! find "$TMP" -type f -exec grep -H --binary-files=without-match -F 'q2dz-secret-canary' {} + >/dev/null 2>&1
printf 'fake lifecycle matrix: PASS\n'
