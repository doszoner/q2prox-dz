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
ROOM_A='{"alias":"q2.room-a","iceServers":[{"urls":["stun:example.invalid"]}],"ordered":false,"map":"","game":"baseq2"}'
ROOM_B='{"alias":"q2.room-b","iceServers":[{"urls":["stun:example.invalid"]}],"ordered":false,"map":"","game":"baseq2"}'
export Q2DZ_SYNTHETIC_CREDENTIAL='must-not-reach-child'
run_session() {
  "$SESSION" \
    --gateway "$MANAGED" \
    --legacy-gateway "$HERE/fake_legacy.py" \
    --adapter "$ADAPTER" \
    --q2 "$HERE/fake_q2.py" \
    --wine "$HERE/fake_wine.sh" \
    --game 3zb2 \
    --runtime-base "$TMP/runtime" \
    --ready-timeout-ms 4000 \
    --shutdown-grace-ms 500 < <(printf '%s' "$ROOM")
}
run_hub() {
  exec "$SESSION" \
    --gateway "$MANAGED" \
    --legacy-gateway "$HERE/fake_legacy.py" \
    --adapter "$ADAPTER" \
    --q2 "$HERE/fake_q2.py" \
    --wine "$HERE/fake_wine.sh" \
    --game 3zb2 \
    --runtime-base "$TMP/runtime" \
    --ready-timeout-ms 4000 \
    --shutdown-grace-ms 500 < <(printf '%s\n%s\n' "$ROOM_A" "$ROOM_B")
}
run_hub_input() {
  "$SESSION" \
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
managed_children() {
  pgrep -P "$1" -f "$MANAGED" || true
}
wait_for_managed_children() {
  local owner=$1
  local expected=$2
  local -n result=$3
  result=()
  for _ in $(seq 1 100); do
    mapfile -t result < <(managed_children "$owner")
    if [ "${#result[@]}" -eq "$expected" ]; then return 0; fi
    sleep 0.05
  done
  return 1
}
# Hub input is newline-delimited, bounded, and rejected before any child starts.
check_invalid_hub_input() {
  set +e
  invalid_hub_output=$(run_hub_input <<<"$1" 2>&1)
  rc=$?
  set -e
  [ "$rc" -eq 65 ]
  grep -Fq 'hub input' <<<"$invalid_hub_output"
}
check_invalid_hub_input ''
check_invalid_hub_input '[]'
check_invalid_hub_input "$(printf '%s\n%s\n%s' "$ROOM_A" "$ROOM_B" "$ROOM")"
oversized_hub_line=$(python3 - <<'PY'
print('{"alias":"q2.' + ('x' * 262144) + '"}')
PY
)
check_invalid_hub_input "$oversized_hub_line"
# L1: normal Q2 exit owns and cleans a two-room session.
export Q2DZ_TEST_MARKER="$TMP/normal"
unset Q2DZ_FAKE_LEGACY_MODE
export Q2DZ_FAKE_Q2_MODE=normal-delay
run_hub >"$TMP/normal.output" 2>&1 &
normal_owner=$!
for _ in $(seq 1 100); do
  [ -f "$TMP/normal.q2-pid" ] && [ -f "$TMP/normal.legacy-Q2_ROOM_A.pid" ] && [ -f "$TMP/normal.legacy-Q2_ROOM_B.pid" ] && break
  sleep 0.05
done
[ -f "$TMP/normal.q2-pid" ]
[ -f "$TMP/normal.legacy-Q2_ROOM_A.pid" ]
[ -f "$TMP/normal.legacy-Q2_ROOM_B.pid" ]
normal_q2_pid=$(<"$TMP/normal.q2-pid")
normal_wine_pid=$(<"$TMP/normal.wine-pid")
wait_for_managed_children "$normal_owner" 2 normal_managed
normal_legacy_a=$(<"$TMP/normal.legacy-Q2_ROOM_A.pid")
normal_legacy_b=$(<"$TMP/normal.legacy-Q2_ROOM_B.pid")
set +e
wait "$normal_owner"
normal_rc=$?
set -e
[ "$normal_rc" -eq 0 ]
grep -Fq 'HUB_READY gateways=2' "$TMP/normal.output"
[ -f "$TMP/normal.finished" ]
[ "$(wc -l < "$TMP/normal.legacy-endpoints")" -eq 2 ]
[ "$(<"$TMP/normal.q2-starts")" = 1 ]
[ "$(grep -Ec '^127\.0\.0\.1:[0-9]+$' "$TMP/normal.legacy-endpoints")" -eq 2 ]
for pid in "${normal_managed[@]}" "$normal_legacy_a" "$normal_legacy_b" "$normal_q2_pid" "$normal_wine_pid"; do
  ! kill -0 "$pid" 2>/dev/null
done
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
# L3: a gateway crash removes only that room. Q2 and the other room stay alive.
export Q2DZ_TEST_MARKER="$TMP/gateway-death"
export Q2DZ_FAKE_LEGACY_MODE='q2.room-a=exit-after-ack;q2.room-b=normal'
export Q2DZ_FAKE_Q2_MODE=sleep-no-probe
run_hub >"$TMP/gateway-death.output" 2>&1 &
gateway_death_owner=$!
for _ in $(seq 1 100); do
  [ -f "$TMP/gateway-death.q2-pid" ] && [ -f "$TMP/gateway-death.legacy-Q2_ROOM_B.pid" ] && break
  sleep 0.05
done
[ -f "$TMP/gateway-death.q2-pid" ]
[ -f "$TMP/gateway-death.legacy-Q2_ROOM_B.pid" ]
gateway_death_q2_pid=$(<"$TMP/gateway-death.q2-pid")
gateway_death_b_pid=$(<"$TMP/gateway-death.legacy-Q2_ROOM_B.pid")
for _ in $(seq 1 100); do
  if ! kill -0 "$(<"$TMP/gateway-death.legacy-Q2_ROOM_A.pid")" 2>/dev/null; then break; fi
  sleep 0.05
done
! kill -0 "$(<"$TMP/gateway-death.legacy-Q2_ROOM_A.pid")" 2>/dev/null
kill -0 "$gateway_death_q2_pid"
kill -0 "$gateway_death_b_pid"
kill -TERM "$gateway_death_owner"
set +e
wait "$gateway_death_owner"
gateway_death_rc=$?
set -e
[ "$gateway_death_rc" -eq 143 ]
! kill -0 "$gateway_death_q2_pid" 2>/dev/null
! kill -0 "$gateway_death_b_pid" 2>/dev/null
! find "$TMP/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
# Hub room A can close without taking down Q2PRO-X or room B. The next room
# remains available in the same Q2 process.
export Q2DZ_TEST_MARKER="$TMP/hub-room-close"
export Q2DZ_FAKE_LEGACY_MODE='q2.room-a=room-close-after-ack;q2.room-b=normal'
export Q2DZ_FAKE_Q2_MODE=sleep
run_hub >"$TMP/hub-room-close.output" 2>&1 &
hub_owner=$!
for _ in $(seq 1 100); do
  [ -f "$TMP/hub-room-close.q2-pid" ] && [ -f "$TMP/hub-room-close.legacy-Q2_ROOM_B.pid" ] && break
  sleep 0.05
done
[ -f "$TMP/hub-room-close.q2-pid" ]
[ -f "$TMP/hub-room-close.legacy-Q2_ROOM_B.pid" ]
hub_q2_pid=$(<"$TMP/hub-room-close.q2-pid")
hub_b_pid=$(<"$TMP/hub-room-close.legacy-Q2_ROOM_B.pid")
for _ in $(seq 1 100); do
  if ! kill -0 "$(<"$TMP/hub-room-close.legacy-Q2_ROOM_A.pid")" 2>/dev/null; then break; fi
  sleep 0.05
done
! kill -0 "$(<"$TMP/hub-room-close.legacy-Q2_ROOM_A.pid")" 2>/dev/null
kill -0 "$hub_q2_pid"
kill -0 "$hub_b_pid"
kill -TERM "$hub_owner"
set +e
wait "$hub_owner"
hub_room_close_rc=$?
set -e
[ "$hub_room_close_rc" -eq 143 ]
! kill -0 "$hub_q2_pid" 2>/dev/null
! kill -0 "$hub_b_pid" 2>/dev/null
! find "$TMP/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
# L4: after room A has closed, hard-killing the owner still cleans room B,
# both managed gateways and Q2/Wine through the guardian.
export Q2DZ_FAKE_LEGACY_MODE='q2.room-a=room-close-after-ack;q2.room-b=normal'
export Q2DZ_FAKE_Q2_MODE=sleep
export Q2DZ_TEST_MARKER="$TMP/hardkill"
run_hub >"$TMP/hardkill.output" 2>&1 &
hardkill_owner=$!
for _ in $(seq 1 100); do
  [ -f "$TMP/hardkill.q2-pid" ] && [ -f "$TMP/hardkill.wine-pid" ] && \
    [ -f "$TMP/hardkill.legacy-Q2_ROOM_A.pid" ] && [ -f "$TMP/hardkill.legacy-Q2_ROOM_B.pid" ] && break
  sleep 0.05
done
[ -f "$TMP/hardkill.q2-pid" ]
[ -f "$TMP/hardkill.wine-pid" ]
[ -f "$TMP/hardkill.legacy-Q2_ROOM_A.pid" ]
[ -f "$TMP/hardkill.legacy-Q2_ROOM_B.pid" ]
hardkill_q2_pid=$(<"$TMP/hardkill.q2-pid")
hardkill_wine_pid=$(<"$TMP/hardkill.wine-pid")
hardkill_legacy_a=$(<"$TMP/hardkill.legacy-Q2_ROOM_A.pid")
hardkill_legacy_b=$(<"$TMP/hardkill.legacy-Q2_ROOM_B.pid")
wait_for_managed_children "$hardkill_owner" 2 hardkill_managed
for _ in $(seq 1 100); do
  if ! kill -0 "$hardkill_legacy_a" 2>/dev/null; then break; fi
  sleep 0.05
done
! kill -0 "$hardkill_legacy_a" 2>/dev/null
kill -0 "$hardkill_q2_pid"
kill -0 "$hardkill_legacy_b"
kill -KILL "$hardkill_owner"
set +e
wait "$hardkill_owner"
hardkill_rc=$?
set -e
[ "$hardkill_rc" -eq 137 ]
for _ in $(seq 1 100); do
  if ! find "$TMP/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .; then break; fi
  sleep 0.05
done
! find "$TMP/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
for pid in "${hardkill_managed[@]}" "$hardkill_legacy_a" "$hardkill_legacy_b" "$hardkill_q2_pid" "$hardkill_wine_pid"; do
  ! kill -0 "$pid" 2>/dev/null
done
# Secret-shaped ICE values must not occur in process command lines or runtime files.
! grep -R --binary-files=without-match -E 'stun:example\.invalid|credential|secret' "$TMP/runtime" 2>/dev/null
printf 'fake lifecycle matrix: PASS\n'
