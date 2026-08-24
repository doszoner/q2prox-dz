#!/usr/bin/env bash
set -euo pipefail
SESSION=$1
MANAGED=$2
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
ADAPTER="$TMP/adapter.json"
cat > "$ADAPTER" <<JSON
{
  "argv": ["--bind","{internal_endpoint}","--signal","wss://net.js-dos.com:444","--ice-file","{ice_memfd}","--alias","{alias}","--max-datagram","1400"],
  "probeTimeoutMs": 2500,
  "probeIntervalMs": 100,
  "roomCloseProbeIdleMs": 5000,
  "roomCloseFailures": 2,
  "maxDatagram": 1400
}
JSON
ROOM='{"alias":"q2.test","iceServers":[{"urls":["stun:example.invalid"]}],"ordered":false,"map":"","game":"baseq2"}'
export Q2DZ_SYNTHETIC_CREDENTIAL='must-not-reach-child'
run_session() {
  printf '%s' "$ROOM" | "$SESSION" \
    --gateway "$MANAGED" \
    --legacy-gateway "$HERE/fake_legacy.py" \
    --adapter "$ADAPTER" \
    --q2 "$HERE/fake_q2.py" \
    --wine "$HERE/fake_wine.sh" \
    --game 3zb2 \
    --runtime-base "$TMP/runtime" \
    --ready-timeout-ms 4000 \
    --shutdown-grace-ms 500
}
# L1: normal Q2 exit owns and cleans the complete session.
export Q2DZ_TEST_MARKER="$TMP/normal"
unset Q2DZ_FAKE_LEGACY_MODE Q2DZ_FAKE_Q2_MODE
normal_output=$(run_session 2>&1)
grep -Fq 'GATEWAY_STOP ' <<<"$normal_output"
[ -f "$TMP/normal.finished" ]
[ "$(wc -l < "$TMP/normal.legacy-endpoints")" -eq 1 ]
[ "$(<"$TMP/normal.q2-starts")" = 1 ]
[ "$(grep -Ec '^127\.0\.0\.1:[0-9]+$' "$TMP/normal.legacy-endpoints")" -eq 1 ]
! find "$TMP/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
# Missing, empty, and unsupported games fail before room input or child startup.
check_invalid_game() {
  set +e
  invalid_game_output=$("$SESSION" --gateway "$MANAGED" --legacy-gateway "$HERE/fake_legacy.py" --adapter "$ADAPTER" "$@" </dev/null 2>&1)
  rc=$?
  set -e
  [ "$rc" -eq 64 ]
  grep -Fq -- '--game must be 3zb2' <<<"$invalid_game_output"
}
check_invalid_game
check_invalid_game --game ''
check_invalid_game --game baseq2
# Fake Q2 rejects an earlier connect even when the required suffix is present.
set +e
env -u Q2DZ_SYNTHETIC_CREDENTIAL -u Q2DZ_TEST_MARKER "$HERE/fake_q2.py" \
  +connect 127.0.0.1:65535 +set game 3zb2 +connect 127.0.0.1:65535
rc=$?
set -e
[ "$rc" -eq 64 ]
# The real managed UDP path accepts 1400 bytes, drops 1401 in either
# direction without exiting, and records only sizes/counters at shutdown.
export Q2DZ_TEST_MARKER="$TMP/datagram-boundary"
export Q2DZ_FAKE_LEGACY_MODE=datagram-boundary
printf '%s' "$ROOM" | env -u Q2DZ_SYNTHETIC_CREDENTIAL \
  "$MANAGED" --managed --legacy "$HERE/fake_legacy.py" --adapter "$ADAPTER" --bind 127.0.0.1:0 \
  >"$TMP/datagram-boundary.output" 2>&1 &
gateway=$!
for _ in $(seq 1 80); do
  grep -q 'TRANSPORT_READY .*udp=' "$TMP/datagram-boundary.output" 2>/dev/null && break
  sleep 0.05
done
endpoint=$(sed -n 's/.*TRANSPORT_READY .*udp=\([^ ]*\).*/\1/p' "$TMP/datagram-boundary.output")
[ -n "$endpoint" ]
python3 - "$endpoint" <<'PY'
import socket, sys
host, port = sys.argv[1].rsplit(':', 1)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(.3)
endpoint = (host, int(port))
for payload in (b'A' * 1400,):
    s.sendto(payload, endpoint)
    assert s.recvfrom(65535)[0] == payload
s.sendto(b'B' * 1401, endpoint)
try:
    s.recvfrom(65535)
    raise AssertionError('1401-byte UDP datagram was forwarded')
except socket.timeout:
    pass
s.sendto(b'remote-oversize', endpoint)
try:
    s.recvfrom(65535)
    raise AssertionError('1401-byte transport datagram was forwarded')
except socket.timeout:
    pass
payload = b'C' * 1399
s.sendto(payload, endpoint)
assert s.recvfrom(65535)[0] == payload
PY
kill -TERM "$gateway"
wait "$gateway"
grep -Fq 'GATEWAY_STOP max_udp_to_transport_datagram=1400 max_transport_to_udp_datagram=1400 oversized_udp_drops=1 oversized_transport_drops=1' "$TMP/datagram-boundary.output"
! grep -Eq 'A{16}|B{16}|C{16}|R{16}' "$TMP/datagram-boundary.output"
# A pre-ready child exit is retried once with a fresh internal port, then the
# second child reaches readiness and Q2 starts normally.
export Q2DZ_TEST_MARKER="$TMP/early-exit-once"
export Q2DZ_FAKE_LEGACY_MODE=early-exit-once
unset Q2DZ_FAKE_Q2_MODE
run_session
[ "$(<"$TMP/early-exit-once.legacy-attempts")" = 2 ]
[ "$(wc -l < "$TMP/early-exit-once.legacy-endpoints")" -eq 2 ]
[ "$(<"$TMP/early-exit-once.q2-starts")" = 1 ]
[ "$(grep -Ec '^127\.0\.0\.1:[0-9]+$' "$TMP/early-exit-once.legacy-endpoints")" -eq 2 ]
[ -f "$TMP/early-exit-once.finished" ]
! find "$TMP/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
# An adapter that exits before readiness is retried at most three times and
# never causes Q2 to launch. Its existing gateway-failure status is retained.
export Q2DZ_TEST_MARKER="$TMP/early-exit-always"
export Q2DZ_FAKE_LEGACY_MODE=early-exit-always
set +e
early_exit_output=$(run_session 2>&1)
rc=$?
set -e
if [ "$rc" -ne 71 ]; then
  printf '%s\n' "$early_exit_output" >&2
  echo "expected coordinator gateway failure 71, got $rc" >&2
  exit 1
fi
[ "$(<"$TMP/early-exit-always.legacy-attempts")" = 3 ]
[ "$(wc -l < "$TMP/early-exit-always.legacy-endpoints")" -eq 3 ]
[ "$(grep -Ec '^127\.0\.0\.1:[0-9]+$' "$TMP/early-exit-always.legacy-endpoints")" -eq 3 ]
[ ! -e "$TMP/early-exit-always.q2-starts" ]
[ ! -e "$TMP/early-exit-always.started" ]
grep -Fq 'legacy child exited before readiness code=12' <<<"$early_exit_output"
! find "$TMP/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
# L2: no OOB ack means Q2 must never start.
export Q2DZ_TEST_MARKER="$TMP/noack"
export Q2DZ_FAKE_LEGACY_MODE=no-ack
set +e
run_session
rc=$?
set -e
[ "$rc" -eq 72 ]
[ ! -e "$TMP/noack.started" ]
[ "$(<"$TMP/noack.legacy-attempts")" = 1 ]
[ "$(wc -l < "$TMP/noack.legacy-endpoints")" -eq 1 ]
[ ! -e "$TMP/noack.q2-starts" ]
! find "$TMP/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
# A no-ACK child that ignores SIGTERM must still be reaped within the gateway
# itself; invoke the managed gateway directly so the session timeout cannot
# hide a blocking cleanup.
export Q2DZ_TEST_MARKER="$TMP/noack-ignore-term"
export Q2DZ_FAKE_LEGACY_MODE=no-ack-ignore-term
set +e
env -u Q2DZ_SYNTHETIC_CREDENTIAL timeout --signal=KILL 4s bash -c 'printf "%s" "$1" | "$2" --managed --legacy "$3" --adapter "$4" --bind 127.0.0.1:0' \
  _ "$ROOM" "$MANAGED" "$HERE/fake_legacy.py" "$ADAPTER" \
  >"$TMP/noack-ignore-term.output" 2>&1
rc=$?
set -e
[ "$rc" -eq 72 ]
[ "$(wc -l < "$TMP/noack-ignore-term.legacy-endpoints")" -eq 1 ]
[ ! -e "$TMP/noack-ignore-term.q2-starts" ]
# L3: gateway transport death closes sleeping Q2.
export Q2DZ_TEST_MARKER="$TMP/gateway-death"
export Q2DZ_FAKE_LEGACY_MODE=exit-after-ack
export Q2DZ_FAKE_Q2_MODE=sleep
set +e
run_session
rc=$?
set -e
[ "$rc" -eq 71 ]
[ -f "$TMP/gateway-death.started" ]
! find "$TMP/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
# A transport-owned room-close exit is a normal ROOM_CLOSED event, not FATAL.
export Q2DZ_TEST_MARKER="$TMP/room-close"
export Q2DZ_FAKE_LEGACY_MODE=room-close-after-ack
export Q2DZ_FAKE_Q2_MODE=sleep
set +e
room_close_output=$(run_session 2>&1)
rc=$?
set -e
if [ "$rc" -ne 73 ]; then
  printf '%s\n' "$room_close_output" >&2
  echo "expected coordinator room-close exit 73, got $rc" >&2
  exit 1
fi
grep -Fq 'SESSION_CLOSED reason=room-closed' <<<"$room_close_output"
[ -f "$TMP/room-close.started" ]
! find "$TMP/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
# L4: hard-killing the owner leaves guardian responsible for processes and data.
unset Q2DZ_FAKE_LEGACY_MODE
export Q2DZ_FAKE_Q2_MODE=sleep
export Q2DZ_TEST_MARKER="$TMP/hardkill"
printf '%s' "$ROOM" | "$SESSION" \
  --gateway "$MANAGED" --legacy-gateway "$HERE/fake_legacy.py" --adapter "$ADAPTER" \
  --q2 "$HERE/fake_q2.py" --wine "$HERE/fake_wine.sh" --game 3zb2 --runtime-base "$TMP/runtime" \
  --ready-timeout-ms 4000 --shutdown-grace-ms 300 &
owner=$!
for _ in $(seq 1 80); do [ -f "$TMP/hardkill.started" ] && break; sleep 0.05; done
[ -f "$TMP/hardkill.started" ]
kill -KILL "$owner"
set +e; wait "$owner"; set -e
for _ in $(seq 1 100); do
  if ! find "$TMP/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .; then break; fi
  sleep 0.05
done
! find "$TMP/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
# No marked fake Q2 process may survive.
if pgrep -f "$HERE/fake_q2.py" >/dev/null 2>&1; then
  echo 'orphan fake Q2 found' >&2; exit 1
fi
# Secret-shaped ICE values must not occur in process command lines or runtime files.
! grep -R --binary-files=without-match -E 'stun:example\.invalid|credential|secret' "$TMP/runtime" 2>/dev/null
printf 'fake lifecycle matrix: PASS\n'
