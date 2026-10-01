#!/usr/bin/env bash
set -euo pipefail

SESSION=$1
HERE=$(cd -- "$(dirname -- "$0")" && pwd -P)
TMP=$(mktemp -d -t q2dz-live-hub.XXXXXX)
trap 'rm -rf -- "$TMP"' EXIT
ADAPTER="$TMP/adapter.json"
cat >"$ADAPTER" <<'JSON'
{"argv":["--bind","{internal_endpoint}","--signal","wss://net.js-dos.com:444","--ice-file","{ice_memfd}","--alias","{alias}","--max-datagram","1400"],"probeTimeoutMs":2500,"probeIntervalMs":100,"roomCloseProbeIdleMs":5000,"roomCloseFailures":2,"maxDatagram":1400}
JSON

cat >"$TMP/fake_shared.py" <<'PY'
#!/usr/bin/env python3
import json, os, selectors, socket, sys, time

marker = os.environ.get('Q2DZ_TEST_MARKER')
mode_spec = os.environ.get('Q2DZ_FAKE_LEGACY_MODE', 'normal')
attempts = 0
if marker:
    try:
        with open(marker + '.shared-attempts', encoding='ascii') as stream:
            attempts = int(stream.read() or '0')
    except FileNotFoundError:
        pass
    attempts += 1
    with open(marker + '.shared-attempts', 'w', encoding='ascii') as stream:
        stream.write(str(attempts))
    with open(marker + '.shared-pid', 'w', encoding='ascii') as stream:
        stream.write(str(os.getpid()))

def mode_for(room_id):
    for item in mode_spec.split(';'):
        key, sep, value = item.partition('=')
        if sep and key == room_id:
            return value
    return mode_spec

def append_marker(suffix, value):
    if not marker:
        return
    with open(marker + suffix, 'a', encoding='ascii') as stream:
        stream.write(value + '\n')

def increment_marker(suffix):
    if not marker:
        return
    path = marker + suffix
    try:
        with open(path, encoding='ascii') as stream:
            value = int(stream.read() or '0')
    except FileNotFoundError:
        value = 0
    with open(path, 'w', encoding='ascii') as stream:
        stream.write(str(value + 1))

def event(line):
    if ' READY id=' in line:
        append_marker('.shared-ready', line.split(' id=', 1)[1].split(' ', 1)[0])
    elif ' FAILED id=' in line:
        append_marker('.shared-failed', line.split(' id=', 1)[1].split(' ', 1)[0])
    elif ' CLOSED id=' in line:
        append_marker('.shared-closed', line.split(' id=', 1)[1].split(' ', 1)[0])
    sys.stdout.write(line + '\n')
    sys.stdout.flush()

sel = selectors.DefaultSelector()
control = os.fdopen(os.dup(sys.stdin.fileno()), 'rb', buffering=0)
sel.register(control, selectors.EVENT_READ, 'control')
targets = {}
dropped_stale = {}
pending_ready = []
pending_exit = None
while True:
    for key, _ in sel.select(0.05):
        if key.data != 'control':
            continue
        raw = key.fileobj.readline()
        if not raw:
            raise SystemExit(0)
        line = raw.decode('utf-8').rstrip('\r\n')
        if not line.startswith('Q2DZ/HUB/2 '):
            raise SystemExit(30)
        command = line[len('Q2DZ/HUB/2 '):]
        if command == 'STOP':
            raise SystemExit(0)
        if command.startswith('ICE '):
            continue
        if command.startswith('DROP id='):
            target_id = int(command[8:])
            old = targets.pop(target_id, None)
            if old is not None:
                append_marker('.shared-drops', str(target_id))
                sel.unregister(old[0])
                old[0].close()
                if old[1] == 'stale-event':
                    # Keep the old generation until the same room is PUT
                    # again.  The late event must not be emitted for DROP or
                    # for an unrelated room.
                    dropped_stale[old[2]] = target_id
                elif old[1] == 'timeout-late-ready':
                    event('Q2DZ/HUB/2 READY id=%d udp=127.0.0.1:27910 peer=999' % target_id)
            continue
        if not command.startswith('PUT id='):
            raise SystemExit(31)
        head, payload = command.split(' json=', 1)
        target_id = int(head[7:])
        config = json.loads(payload)
        room_id = config['roomId']
        increment_marker('.shared-puts')
        mode = mode_for(room_id)
        if mode == 'global-crash-after-ready' and attempts > 1:
            mode = 'normal'
        if mode == 'event-flood' and attempts > 1:
            mode = 'normal'
        if target_id in targets:
            continue
        stale_target_id = dropped_stale.pop(room_id, None) if mode == 'stale-event' else None
        if mode == 'global-crash':
            os._exit(21)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(('127.0.0.1', 0))
        sock.setblocking(False)
        targets[target_id] = (sock, mode, room_id)
        sel.register(sock, selectors.EVENT_READ, target_id)
        if mode in ('never-ready', 'timeout-late-ready'):
            continue
        if mode == 'malformed-ready':
            event('Q2DZ/HUB/2 READY id=%d udp=0.0.0.0:27910 peer=1' % target_id)
            continue
        port = sock.getsockname()[1]
        if mode == 'shuffled':
            pending_ready.append((target_id, port))
            if len(pending_ready) >= 2:
                for ready_id, ready_port in reversed(pending_ready):
                    event('Q2DZ/HUB/2 READY id=%d udp=127.0.0.1:%d peer=%d' % (ready_id, ready_port, ready_id))
                pending_ready.clear()
            continue
        if stale_target_id is not None:
            append_marker('.shared-replacement-endpoint', '127.0.0.1:%d' % port)
            event('Q2DZ/HUB/2 READY id=%d udp=127.0.0.1:27910 peer=999' % stale_target_id)
        event('Q2DZ/HUB/2 READY id=%d udp=127.0.0.1:%d peer=%d' % (target_id, port, target_id))
        if mode == 'target-failed':
            event('Q2DZ/HUB/2 FAILED id=%d stage=transport code=test_failure' % target_id)
            sel.unregister(sock)
            sock.close()
            del targets[target_id]
        elif mode == 'target-closed':
            event('Q2DZ/HUB/2 CLOSED id=%d reason=test_closed' % target_id)
            sel.unregister(sock)
            sock.close()
            del targets[target_id]
        elif mode == 'global-crash-after-ready':
            pending_exit = time.monotonic() + 0.25
        elif mode == 'event-flood':
            for _ in range(96):
                sys.stdout.write('Q2DZ/HUB/2 HEARTBEAT state=ok targets=1 ready=1\n')
            sys.stdout.flush()
            append_marker('.shared-flood', 'done')
    if pending_exit is not None and time.monotonic() >= pending_exit:
        os._exit(22)
    for key, _ in sel.select(0):
        if isinstance(key.data, int):
            data, address = key.fileobj.recvfrom(65535)
            key.fileobj.sendto(data, address)
PY
chmod +x "$TMP/fake_shared.py"

room_json() {
  local room=$1
  printf '{"roomId":"%s","alias":"q2.%s","iceServers":[{"urls":["stun:example.invalid"]}],"ordered":false,"map":"","game":"baseq2","credential":"q2dz-secret-canary"}' "$room" "$room"
}

wait_file() {
  local path=$1
  for _ in $(seq 1 240); do
    [ -f "$path" ] && return 0
    sleep 0.025
  done
  echo "timed out waiting for $path" >&2
  cat "$(dirname "$path")/output" >&2 2>/dev/null || true
  return 1
}

read_source() {
  python3 - "$1" <<'PY'
import http.client, sys
url = sys.argv[1]
host, port = url[len('http://'):].split('/', 1)[0].rsplit(':', 1)
connection = http.client.HTTPConnection(host, int(port), timeout=1)
connection.request('GET', '/servers', headers={'Host': host + ':' + port, 'Connection': 'close'})
response = connection.getresponse()
body = response.read()
connection.close()
if response.status != 200:
    raise SystemExit(1)
sys.stdout.buffer.write(body)
PY
}

read_rooms() {
  python3 - "$SOURCE_URL" <<'PY'
import http.client, sys
url = sys.argv[1]
host, port = url[len('http://'):].split('/', 1)[0].rsplit(':', 1)
connection = http.client.HTTPConnection(host, int(port), timeout=1)
connection.request('GET', '/q2dz/rooms', headers={'Host': host + ':' + port, 'Connection': 'close'})
response = connection.getresponse()
body = response.read()
connection.close()
if response.status != 200:
    raise SystemExit(1)
sys.stdout.buffer.write(body)
PY
}

wait_rooms_count() {
  local expected=$1
  for _ in $(seq 1 240); do
    if body=$(read_rooms 2>/dev/null); then
      if python3 - "$expected" "$body" <<'PY'
import json, sys
expected = int(sys.argv[1])
payload = json.loads(sys.argv[2])
raise SystemExit(0 if payload.get('version') == 1 and len(payload.get('targets', [])) == expected else 1)
PY
      then return 0; fi
    fi
    sleep 0.025
  done
  echo "unexpected mapping count; expected $expected" >&2
  read_rooms >&2 || true
  return 1
}

assert_rooms_rejected() {
  python3 - "$SOURCE_URL" <<'PY'
import socket, sys
url = sys.argv[1]
host, port = url[len('http://'):].split('/', 1)[0].rsplit(':', 1)
port = int(port)

def request(raw):
    sock = socket.create_connection((host, port), timeout=1)
    sock.settimeout(1)
    sock.sendall(raw)
    chunks = []
    try:
        while True:
            data = sock.recv(4096)
            if not data:
                break
            chunks.append(data)
    except socket.timeout:
        pass
    finally:
        sock.close()
    return b''.join(chunks)

valid = f'GET /q2dz/rooms HTTP/1.1\r\nHost:{host}:{port}\r\nConnection: close\r\n\r\n'.encode()
response = request(valid)
assert response.startswith(b'HTTP/1.1 200 OK'), response[:80]
assert b'Access-Control-Allow-Origin' not in response
ows_valid = f'GET /q2dz/rooms HTTP/1.1\r\nHost:\t{host}:{port}\t\r\nConnection: close\r\n\r\n'.encode()
assert request(ows_valid).startswith(b'HTTP/1.1 200 OK')
for raw in (
    f'GET /q2dz/rooms HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n'.encode(),
    f'GET /q2dz/rooms HTTP/1.1\r\nHost: {host}:{port}\r\nOrigin: http://example.invalid\r\nConnection: close\r\n\r\n'.encode(),
    f'POST /q2dz/rooms HTTP/1.1\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n'.encode(),
    f'GET /q2dz/rooms?x=1 HTTP/1.1\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n'.encode(),
    f'GET /q2dz/rooms HTTP/1.1\r\nX-Malformed\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n'.encode(),
    f'GET /q2dz/rooms HTTP/1.1\r\nHost: {host}:{port}\r\nX: \x00\r\nConnection: close\r\n\r\n'.encode(),
    f'GET /q2dz/rooms HTTP/1.1\r\nHost: {host}:{port}\r\nX: abc\rdef\r\nConnection: close\r\n\r\n'.encode(),
    f'GET /q2dz/rooms HTTP/1.1\r\nHost: {host}:{port}\r\nX: \x7f\r\nConnection: close\r\n\r\n'.encode(),
    f'GET /q2dz/rooms HTTP/1.1\r\nHost: {host}:{port}\r\nX: '.encode() + b'x' * (8 * 1024),
):
    assert not request(raw).startswith(b'HTTP/1.1 200 OK'), raw[:40]
PY
}

wait_source_count() {
  local expected=$1
  for _ in $(seq 1 240); do
    if body=$(read_source "$SOURCE_URL" 2>/dev/null); then
      local count
      count=$(grep -Ec '^127\.0\.0\.1:[0-9]+$' <<<"$body" || true)
      [ "$count" -eq "$expected" ] && return 0
    fi
    sleep 0.025
  done
  echo "unexpected source count; expected $expected" >&2
  cat "$CASE/output" >&2 || true
  read_source "$SOURCE_URL" >&2 || true
  return 1
}

assert_source_lacks() {
  local endpoint=$1
  for _ in $(seq 1 240); do
    if body=$(read_source "$SOURCE_URL" 2>/dev/null); then
      ! grep -Fqx "$endpoint" <<<"$body" && return 0
    fi
    sleep 0.025
  done
  echo "unexpected endpoint in source: $endpoint" >&2
  read_source "$SOURCE_URL" >&2 || true
  return 1
}

start_case() {
  local name=$1 mode=${2:-normal} q2_mode=${3:-sleep}
  CASE="$TMP/$name"
  mkdir -p "$CASE"
  mkfifo "$CASE/control"
  Q2DZ_TEST_MARKER="$CASE/mark" Q2DZ_FAKE_LEGACY_MODE="$mode" \
    Q2DZ_FAKE_Q2_MODE="$q2_mode" Q2DZ_SYNTHETIC_CREDENTIAL='q2dz-secret-canary' \
    "$SESSION" --gateway "$TMP/fake_shared.py" --legacy-gateway "$HERE/fake_legacy.py" \
      --adapter "$ADAPTER" --q2 "$HERE/fake_q2.py" --wine "$HERE/fake_wine.sh" \
      --game 3zb2 --runtime-base "$CASE/runtime" --ready-timeout-ms 1200 \
      --shutdown-grace-ms 300 <"$CASE/control" >"$CASE/output" 2>&1 &
  OWNER=$!
  exec {WRITER}>"$CASE/control"
  wait_file "$CASE/mark.q2-source-url"
  SOURCE_URL=$(<"$CASE/mark.q2-source-url")
  for _ in $(seq 1 240); do
    [ -f "$CASE/mark.q2-source-empty" ] && break
    if [ -f "$CASE/mark.q2-rooms-failed" ]; then
      echo "fake Q2 room mapping failed: $(<"$CASE/mark.q2-rooms-failed")" >&2
      cat "$CASE/output" >&2 || true
      return 1
    fi
    sleep 0.025
  done
  [ -f "$CASE/mark.q2-source-empty" ]
  [ "$(<"$CASE/mark.q2-source-empty")" = 1 ]
  expect_event 'SESSION start build=[A-Za-z0-9._-]+ session=[0-9a-f]{32} instance=[0-9]+ gateway_restarts=0$'
}

# Coordinator SESSION/TARGET/GATEWAY events land on the merged child output.
# The fake's marker file is written before the coordinator consumes the event,
# so the grep is retried until the event is drained.
expect_event() {
  for _ in $(seq 1 240); do
    grep -Eq "$1" "$CASE/output" && return 0
    sleep 0.025
  done
  echo "missing event in $CASE: $1" >&2
  cat "$CASE/output" >&2
  return 1
}

stop_case() {
  local expected=${1:-143}
  kill -TERM "$OWNER"
  set +e
  wait "$OWNER"
  local rc=$?
  set -e
  exec {WRITER}>&-
  [ "$rc" -eq "$expected" ]
  expect_event 'SESSION stop reason=[A-Za-z0-9._-]+ rc=143$'
  ! find "$CASE/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
  ! find "$CASE" -type f -exec grep -q -F 'q2dz-secret-canary' {} +
}

send_ice() {
  printf '%s\n' 'Q2DZ/ROOMS/2 ICE generation=1 json=[{"urls":["stun:example.invalid"]}]' >&$WRITER
}

send_room() {
  local room=$1
  printf 'Q2DZ/ROOMS/2 PUT room=%s json=%s\n' "$room" "$(room_json "$room")" >&$WRITER
}

send_drop() {
  printf 'Q2DZ/ROOMS/2 DROP room=%s\n' "$1" >&$WRITER
}

send_room_fragment() {
  local room=$1
  printf 'Q2DZ/ROOMS/2 PUT room=%s json=%s' "$room" "$(room_json "$room")" >&$WRITER
}

expect_protocol_failure() {
  local expected_message=${1:-}
  exec {WRITER}>&-
  set +e
  wait "$OWNER"
  local rc=$?
  set -e
  [ "$rc" -eq 65 ]
  expect_event 'SESSION stop reason=[A-Za-z0-9._-]+ rc=65$'
  if [ -n "$expected_message" ]; then
    grep -Fq "$expected_message" "$CASE/output"
  fi
  ! find "$CASE/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
  ! find "$CASE" -type f -exec grep -q -F 'q2dz-secret-canary' {} +
}

# 0 targets: Q2 starts with an empty source; the first heartbeat is dynamic.
start_case empty-start
python3 - "$(read_rooms)" <<'PY'
import json, sys
payload = json.loads(sys.argv[1])
assert payload['version'] == 1 and isinstance(payload['session'], str) and payload['session']
assert payload['targets'] == []
PY
assert_rooms_rejected
send_ice
send_room room-a
wait_source_count 1
wait_rooms_count 1
python3 - "$(read_rooms)" <<'PY'
import json, sys
payload = json.loads(sys.argv[1])
target = payload['targets'][0]
assert target['room'] == 'room-a' and target['kind'] == 'q2'
assert target['target'].isdigit() and target['endpoint'].startswith('127.0.0.1:')
PY
expect_event 'TARGET room=room-a id=1 state=pending reason=none recovery=0 retry=none published_ready=0$'
expect_event 'TARGET room=room-a id=1 state=ready reason=none recovery=0 retry=none published_ready=1$'
shared_pid=$(<"$CASE/mark.shared-pid")
send_room room-a
sleep 0.1
[ "$(<"$CASE/mark.shared-pid")" = "$shared_pid" ]
send_room room-b
wait_source_count 2
wait_rooms_count 2
python3 - "$(read_rooms)" <<'PY'
import json, sys
payload = json.loads(sys.argv[1])
targets = {item['room']: item for item in payload['targets']}
assert set(targets) == {'room-a', 'room-b'}
assert {item['kind'] for item in targets.values()} == {'q2'}
PY
[ "$(<"$CASE/mark.shared-attempts")" = 1 ]
send_drop room-a
wait_source_count 1
wait_rooms_count 1
python3 - "$(read_rooms)" <<'PY'
import json, sys
payload = json.loads(sys.argv[1])
assert [item['room'] for item in payload['targets']] == ['room-b']
PY
first_session=$(python3 - "$(read_rooms)" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['session'])
PY
)
stop_case

# Dedicated and ordinary targets keep distinct kinds, while a new coordinator
# instance gets a new opaque session marker.
start_case session-change
second_session=$(python3 - "$(read_rooms)" <<'PY'
import json, sys
print(json.loads(sys.argv[1])['session'])
PY
)
[ "$first_session" != "$second_session" ]
send_ice
send_room room.q2ded.server
wait_source_count 1
wait_rooms_count 1
python3 - "$(read_rooms)" <<'PY'
import json, sys
target = json.loads(sys.argv[1])['targets'][0]
assert target['room'] == 'room.q2ded.server' and target['kind'] == 'q2ded'
PY
stop_case

# The bounded snapshot carries all 256 ready targets, with no truncation and
# the same endpoint set as the legacy source.
start_case max-targets
send_ice
for index in $(seq -w 1 256); do send_room "max-$index"; done
wait_source_count 256
wait_rooms_count 256
rooms_body=$(read_rooms)
servers_body=$(read_source "$SOURCE_URL")
python3 - "$rooms_body" "$servers_body" <<'PY'
import json, re, sys
rooms_body, servers_body = sys.argv[1:]
payload = json.loads(rooms_body)
targets = payload['targets']
assert set(payload) == {'version', 'session', 'targets'}
assert payload['version'] == 1 and isinstance(payload['session'], str) and payload['session']
assert len(targets) == 256
assert len(rooms_body.encode()) <= 256 * 1024
assert not re.search(r'credential|ice|sdp|peer|secret', rooms_body, re.I)
assert len({item['room'] for item in targets}) == 256
assert len({item['target'] for item in targets}) == 256
assert all(set(item) == {'target', 'endpoint', 'room', 'kind'} for item in targets)
assert all(item['kind'] == 'q2' and isinstance(item['target'], str) for item in targets)
mapping_endpoints = sorted(item['endpoint'] for item in targets)
source_endpoints = sorted(line for line in servers_body.splitlines() if line)
assert source_endpoints == mapping_endpoints
assert len(set(mapping_endpoints)) == 256
PY
stop_case

# Room IDs remain case-sensitive and therefore do not overwrite one another.
start_case case-sensitive
send_ice
send_room room-a
send_room Room-A
wait_source_count 2
wait_rooms_count 2
python3 - "$(read_rooms)" <<'PY'
import json, sys
rooms = {item['room'] for item in json.loads(sys.argv[1])['targets']}
assert rooms == {'room-a', 'Room-A'}
PY
stop_case

# A late event from a removed target is retained until the same room is
# replaced; it cannot repopulate the source or affect an unrelated room.
start_case stale stale-event
send_ice
send_room room-a
wait_source_count 1
stale_initial=$(read_rooms)
send_drop room-a
wait_source_count 0
wait_rooms_count 0
send_room room-b
wait_source_count 1
[ "$(tr '\n' ' ' <"$CASE/mark.shared-ready")" = "1 2 " ]
[ "$(<"$CASE/mark.shared-attempts")" = 1 ]
send_room room-a
wait_source_count 2
wait_rooms_count 2
stale_recreated=$(read_rooms)
python3 - "$stale_initial" "$stale_recreated" <<'PY'
import json, sys
before, after = (json.loads(item) for item in sys.argv[1:])
old_target = next(item['target'] for item in before['targets'] if item['room'] == 'room-a')
new_targets = [item['target'] for item in after['targets'] if item['room'] == 'room-a']
assert after['session'] == before['session']
assert len(new_targets) == 1 and new_targets[0] != old_target
assert old_target not in {item['target'] for item in after['targets']}
PY
wait_file "$CASE/mark.shared-replacement-endpoint"
replacement_endpoint=$(<"$CASE/mark.shared-replacement-endpoint")
grep -Fqx "$replacement_endpoint" <<<"$(read_source "$SOURCE_URL")"
! grep -Fqx 127.0.0.1:27910 <<<"$(read_source "$SOURCE_URL")"
[ "$(tr '\n' ' ' <"$CASE/mark.shared-ready")" = "1 2 1 3 " ]
[ "$(<"$CASE/mark.shared-attempts")" = 1 ]
stop_case

# One target timeout/failure is isolated from a healthy target and Q2.
start_case isolated 'room-a=never-ready;room-b=normal'
send_ice
send_room room-a
send_room room-b
wait_source_count 1
wait_rooms_count 1
python3 - "$(read_rooms)" <<'PY'
import json, sys
targets = json.loads(sys.argv[1])['targets']
assert [item['room'] for item in targets] == ['room-b']
PY
wait_file "$CASE/mark.shared-drops"
wait_source_count 1
wait_rooms_count 1
expect_event 'TARGET room=room-a id=1 state=terminal reason=ready_timeout recovery=1 retry=none'
stop_case

# A timeout may produce a late READY for the tombstoned target; it must not
# restart the shared gateway or disturb the healthy sibling.
start_case timeout-stale 'room-a=timeout-late-ready;room-b=normal'
send_ice
send_room room-a
send_room room-b
wait_source_count 1
wait_file "$CASE/mark.shared-drops"
wait_source_count 1
[ "$(<"$CASE/mark.shared-attempts")" = 1 ]
assert_source_lacks 127.0.0.1:27910
[ "$(<"$CASE/mark.shared-attempts")" = 1 ]
send_room room-a
sleep 0.2
[ "$(<"$CASE/mark.shared-puts")" = 2 ]
wait_source_count 1
stop_case

# Target CLOSED/FAILED are local events, not a global shutdown.
for terminal in target-closed target-failed; do
  start_case "$terminal" "room-a=$terminal;room-b=normal"
  send_ice
  send_room room-a
  send_room room-b
  wait_source_count 1
  terminal_reason=test_closed
  terminal_retry=none
  [ "$terminal" = target-failed ] && { terminal_reason=test_failure; terminal_retry=immediate; }
  expect_event "TARGET room=room-a id=1 state=terminal reason=$terminal_reason .* retry=$terminal_retry"
  kill -0 "$OWNER"
  stop_case
done

# Preserve bounded target recovery across target IDs: q2 gets one retry after
# a ready failure, while q2ded keeps the existing two-recovery-start policy.
start_case bounded-q2 'room-a=target-failed'
send_ice
send_room room-a
wait_file "$CASE/mark.shared-failed"
expect_event 'TARGET room=room-a id=1 state=terminal reason=test_failure recovery=0 retry=immediate'
[ "$(<"$CASE/mark.shared-puts")" = 1 ]
sleep 0.2
send_room room-a
for _ in $(seq 1 120); do [ "$(wc -l <"$CASE/mark.shared-failed")" -ge 2 ] && break; sleep 0.025; done
[ "$(wc -l <"$CASE/mark.shared-failed")" -ge 2 ]
[ "$(tr '\n' ' ' <"$CASE/mark.shared-failed")" = "1 2 " ]
expect_event 'TARGET room=room-a id=2 state=terminal reason=test_failure recovery=1 retry=none'
send_room room-a
sleep 0.2
[ "$(<"$CASE/mark.shared-puts")" = 2 ]
# The exhausted tombstone logs the stuck PUT again instead of staying silent.
for _ in $(seq 1 240); do
  [ "$(grep -c 'TARGET room=room-a id=2 state=terminal' "$CASE/output")" -ge 2 ] && break
  sleep 0.025
done
[ "$(grep -c 'TARGET room=room-a id=2 state=terminal' "$CASE/output")" -ge 2 ]
stop_case

start_case bounded-q2ded 'room.q2ded.server=target-failed'
send_ice
send_room room.q2ded.server
wait_file "$CASE/mark.shared-failed"
sleep 0.2
send_room room.q2ded.server
for _ in $(seq 1 120); do [ "$(wc -l <"$CASE/mark.shared-failed")" -ge 2 ] && break; sleep 0.025; done
[ "$(wc -l <"$CASE/mark.shared-failed")" -ge 2 ]
sleep 0.2
send_room room.q2ded.server
for _ in $(seq 1 120); do [ "$(wc -l <"$CASE/mark.shared-failed")" -ge 3 ] && break; sleep 0.025; done
[ "$(wc -l <"$CASE/mark.shared-failed")" -ge 3 ]
[ "$(tr '\n' ' ' <"$CASE/mark.shared-failed")" = "1 2 3 " ]
send_room room.q2ded.server
sleep 0.2
[ "$(<"$CASE/mark.shared-puts")" = 3 ]
stop_case

start_case bounded-q2ded-closed 'room.q2ded.server=target-closed'
send_ice
send_room room.q2ded.server
wait_file "$CASE/mark.shared-closed"
sleep 0.2
send_room room.q2ded.server
for _ in $(seq 1 120); do [ "$(wc -l <"$CASE/mark.shared-closed")" -ge 2 ] && break; sleep 0.025; done
[ "$(wc -l <"$CASE/mark.shared-closed")" -ge 2 ]
sleep 0.2
send_room room.q2ded.server
for _ in $(seq 1 120); do [ "$(wc -l <"$CASE/mark.shared-closed")" -ge 3 ] && break; sleep 0.025; done
[ "$(wc -l <"$CASE/mark.shared-closed")" -ge 3 ]
[ "$(tr '\n' ' ' <"$CASE/mark.shared-closed")" = "1 2 3 " ]
expect_event 'TARGET room=room.q2ded.server id=3 state=terminal reason=test_closed recovery=2 retry=none'
send_room room.q2ded.server
sleep 0.2
[ "$(<"$CASE/mark.shared-puts")" = 3 ]
for _ in $(seq 1 240); do
  [ "$(grep -c 'TARGET room=room.q2ded.server id=3 state=terminal' "$CASE/output")" -ge 2 ] && break
  sleep 0.025
done
[ "$(grep -c 'TARGET room=room.q2ded.server id=3 state=terminal' "$CASE/output")" -ge 2 ]
stop_case

# READY may arrive in a different order than PUT; source publication remains
# deterministic and both targets survive.
start_case shuffled 'room-a=shuffled;room-b=shuffled'
send_ice
send_room room-a
sleep 0.1
send_room room-b
wait_source_count 2
[ "$(tr '\n' ' ' <"$CASE/mark.shared-ready")" = "2 1 " ]
stop_case

# A shared gateway crash is restarted once without replay; the next heartbeat repopulates it.
start_case global-restart global-crash-after-ready
send_ice
send_room room-a
wait_file "$CASE/mark.shared-ready"
wait_source_count 1
[ "$(<"$CASE/mark.shared-puts")" = 1 ]
for _ in $(seq 1 120); do [ "$(<"$CASE/mark.shared-attempts")" -ge 2 ] && break; sleep 0.025; done
[ "$(<"$CASE/mark.shared-attempts")" = 2 ]
expect_event 'GATEWAY event=stop reason=gateway-control-eof restarts=0$'
expect_event 'GATEWAY event=restart reason=none restarts=1$'
wait_source_count 0
[ "$(<"$CASE/mark.shared-puts")" = 1 ]
send_ice
send_room room-a
for _ in $(seq 1 120); do [ "$(<"$CASE/mark.shared-puts")" -ge 2 ] && break; sleep 0.025; done
[ "$(<"$CASE/mark.shared-puts")" = 2 ]
wait_source_count 1
stop_case

# A pre-crash partial command is discarded through its newline.  Its suffix
# cannot be replayed to the replacement gateway, while a later ICE heartbeat
# can repopulate the source normally.
start_case recovery-discard global-crash-after-ready
send_ice
send_room room-a
wait_file "$CASE/mark.shared-ready"
wait_source_count 1
send_room_fragment room-old
sleep 0.1
for _ in $(seq 1 120); do [ "$(<"$CASE/mark.shared-attempts")" -ge 2 ] && break; sleep 0.025; done
[ "$(<"$CASE/mark.shared-attempts")" = 2 ]
wait_source_count 0
printf '\n' >&$WRITER
sleep 0.2
[ "$(<"$CASE/mark.shared-puts")" = 1 ]
wait_source_count 0
send_ice
send_room room-b
for _ in $(seq 1 120); do [ "$(<"$CASE/mark.shared-puts")" -ge 2 ] && break; sleep 0.025; done
[ "$(<"$CASE/mark.shared-puts")" = 2 ]
wait_source_count 1
stop_case

# The recovery discard has the same 256 KiB bound as a normal command line.
start_case recovery-discard-oversized global-crash-after-ready
send_ice
send_room room-a
wait_file "$CASE/mark.shared-ready"
wait_source_count 1
send_room_fragment room-old
sleep 0.1
for _ in $(seq 1 120); do [ "$(<"$CASE/mark.shared-attempts")" -ge 2 ] && break; sleep 0.025; done
[ "$(<"$CASE/mark.shared-attempts")" = 2 ]
wait_source_count 0
set +e
python3 - "$WRITER" <<'PY'
import os
import sys

fd = int(sys.argv[1])
payload = b'x' * (256 * 1024 + 1)
while payload:
    try:
        count = os.write(fd, payload)
    except BrokenPipeError:
        break
    payload = payload[count:]
PY
set -e
expect_protocol_failure 'hub recovery remainder exceeds 256 KiB'

# EOF while resynchronising is a protocol failure, not an implicit command
# terminator.
start_case recovery-discard-eof global-crash-after-ready
send_ice
send_room room-a
wait_file "$CASE/mark.shared-ready"
wait_source_count 1
send_room_fragment room-old
sleep 0.1
for _ in $(seq 1 120); do [ "$(<"$CASE/mark.shared-attempts")" -ge 2 ] && break; sleep 0.025; done
[ "$(<"$CASE/mark.shared-attempts")" = 2 ]
wait_source_count 0
exec {WRITER}>&-
expect_protocol_failure 'hub input ended while discarding recovery remainder'

# Recovery resets the command parser's ICE gate.  A PUT received before a
# fresh ICE is rejected without reaching the replacement gateway.
start_case recovery-requires-ice global-crash-after-ready
send_ice
send_room room-a
wait_file "$CASE/mark.shared-ready"
wait_source_count 1
for _ in $(seq 1 120); do [ "$(<"$CASE/mark.shared-attempts")" -ge 2 ] && break; sleep 0.025; done
[ "$(<"$CASE/mark.shared-attempts")" = 2 ]
wait_source_count 0
send_room room-a
sleep 0.2
expect_protocol_failure 'PUT requires a preceding ICE command'
[ "$(<"$CASE/mark.shared-puts")" = 1 ]

# Validate an entire bounded input batch before executing any of it.  A bad
# command after 32 valid commands must not allow the first 32 to reach the
# gateway.
start_case fail-closed-batch normal
send_ice
sleep 0.1
python3 - "$WRITER" <<'PY'
import os
import sys

fd = int(sys.argv[1])
lines = []
for index in range(1, 33):
    room = f'batch-room-{index}'
    lines.append(
        f'Q2DZ/ROOMS/2 PUT room={room} '
        f'json={{"roomId":"{room}","alias":"q2.{room}","iceServers":[{{}}]}}\n'.encode()
    )
lines.append(b'Q2DZ/ROOMS/2 NOT_A_COMMAND\n')
payload = b''.join(lines)
assert len(payload) < 4096
written = os.write(fd, payload)
assert written == len(payload)
PY
expect_protocol_failure 'unknown hub command'
if [ -f "$CASE/mark.shared-puts" ]; then
  if [ "$(<"$CASE/mark.shared-puts")" != 0 ]; then
    echo "fail-closed batch forwarded unexpected PUTs: $(<"$CASE/mark.shared-puts")" >&2
    cat "$CASE/output" >&2 || true
    false
  fi
fi

# A child that floods valid events cannot starve Q2 or make the coordinator
# retain an unbounded event vector.  The bounded reader keeps the session
# usable until the normal owner stop.
start_case event-flood 'room-a=event-flood'
send_ice
send_room room-a
wait_source_count 1
wait_file "$CASE/mark.shared-flood"
kill -0 "$(<"$CASE/mark.q2-pid")"
stop_case

# A continuously-fed command pipe cannot starve the shared gateway.  The
# source must become ready before the producer finishes its bounded run.
start_case bounded-input-backlog normal
send_ice
backlog_line="Q2DZ/ROOMS/2 PUT room=backlog-room json=$(room_json backlog-room)"
(
  end=$((SECONDS + 3))
  while [ "$SECONDS" -lt "$end" ]; do
    printf '%s\n' "$backlog_line" >&$WRITER || break
  done
  : >"$CASE/backlog-done"
) &
backlog_writer=$!
wait_source_count 1
[ ! -f "$CASE/backlog-done" ]
wait "$backlog_writer"
stop_case

# A second shared crash leaves Q2 alive and rejects transport commands.
start_case global-no-retry global-crash
send_ice
send_room room-a
for _ in $(seq 1 120); do [ "$(<"$CASE/mark.shared-attempts")" -ge 2 ] && break; sleep 0.025; done
[ "$(<"$CASE/mark.shared-attempts")" = 2 ]
send_ice
send_room room-b
sleep 0.2
[ "$(<"$CASE/mark.shared-attempts")" = 2 ]
expect_event 'GATEWAY event=fatal reason=transport_disabled restarts=1$'
kill -0 "$(<"$CASE/mark.q2-pid")"
stop_case

# A normal Q2 exit still owns the complete process tree.
start_case q2-exit normal normal-delay
q2_exit_shared_pid=$(<"$CASE/mark.shared-pid")
set +e
wait "$OWNER"
rc=$?
set -e
exec {WRITER}>&-
[ "$rc" -eq 0 ]
expect_event 'SESSION stop reason=q2-normal-exit rc=0$'
! find "$CASE/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
! kill -0 "$q2_exit_shared_pid" 2>/dev/null

# Owner hard-kill is recovered by the guardian and leaves no child process.
start_case hard-kill normal
send_ice
send_room room-a
send_room room-b
wait_source_count 2
q2_pid=$(<"$CASE/mark.q2-pid")
shared_pid=$(<"$CASE/mark.shared-pid")
kill -KILL "$OWNER"
set +e
wait "$OWNER"
rc=$?
set -e
exec {WRITER}>&-
[ "$rc" -eq 137 ]
for _ in $(seq 1 240); do
  if ! find "$CASE/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q . && ! kill -0 "$q2_pid" 2>/dev/null && ! kill -0 "$shared_pid" 2>/dev/null; then break; fi
  sleep 0.025
done
! find "$CASE/runtime" -maxdepth 1 -type d -name 'session-*' | grep -q .
! kill -0 "$q2_pid" 2>/dev/null
! kill -0 "$shared_pid" 2>/dev/null

# A gateway FATAL event whose stage/code contains a sensitivity-filter
# substring (e.g. "ice", as managed_gateway_main.cpp's
# emit_fatal("ice", "memfd-failed", 70) does) must be mapped to a safe code
# before it reaches GATEWAY/TARGET logging, not leak the raw substring and
# not get the whole line suppressed as a CHILD line (A7).
cat >"$TMP/fake_fatal_gateway.py" <<'PY'
#!/usr/bin/env python3
import sys
sys.stdin.readline()
sys.stdout.write("Q2DZ/1 FATAL stage=ice code=memfd-failed\n")
sys.stdout.flush()
sys.stdin.read()
PY
chmod +x "$TMP/fake_fatal_gateway.py"
FATAL_CASE="$TMP/fatal-ice"
mkdir -p "$FATAL_CASE"
mkfifo "$FATAL_CASE/control"
Q2DZ_TEST_MARKER="$FATAL_CASE/mark" Q2DZ_FAKE_LEGACY_MODE=normal \
  Q2DZ_FAKE_Q2_MODE=sleep Q2DZ_SYNTHETIC_CREDENTIAL='q2dz-secret-canary' \
  "$SESSION" --gateway "$TMP/fake_fatal_gateway.py" --legacy-gateway "$HERE/fake_legacy.py" \
    --adapter "$ADAPTER" --q2 "$HERE/fake_q2.py" --wine "$HERE/fake_wine.sh" \
    --game 3zb2 --runtime-base "$FATAL_CASE/runtime" --ready-timeout-ms 1200 \
    --shutdown-grace-ms 300 <"$FATAL_CASE/control" >"$FATAL_CASE/output" 2>&1 &
FATAL_OWNER=$!
exec {FATAL_WRITER}>"$FATAL_CASE/control"
printf '%s\n' 'Q2DZ/ROOMS/2 ICE generation=1 json=[{"urls":["stun:example.invalid"]}]' >&$FATAL_WRITER
for _ in $(seq 1 240); do
  grep -Eq 'GATEWAY event=fatal' "$FATAL_CASE/output" && break
  sleep 0.025
done
grep -Eq 'GATEWAY event=fatal reason=transport_startup_failed restarts=0' "$FATAL_CASE/output" \
  || { echo "fatal-ice: safe-mapped GATEWAY line missing" >&2; cat "$FATAL_CASE/output" >&2; exit 1; }
grep -q 'memfd-failed' "$FATAL_CASE/output" && { echo "fatal-ice: raw code leaked" >&2; cat "$FATAL_CASE/output" >&2; exit 1; }
grep -Eq '(^|[^A-Za-z])ice([^A-Za-z]|$)' "$FATAL_CASE/output" && { echo "fatal-ice: raw ice substring leaked" >&2; cat "$FATAL_CASE/output" >&2; exit 1; }
exec {FATAL_WRITER}>&-
kill "$FATAL_OWNER" 2>/dev/null || true
wait "$FATAL_OWNER" 2>/dev/null || true

# Same as above, but with a code the mapper previously omitted
# (local-description/remote-description/session-description/aliaserror):
# must still be mapped to a safe code, not leak the raw substring.
cat >"$TMP/fake_fatal_gateway_omitted.py" <<'PY'
#!/usr/bin/env python3
import sys
sys.stdin.readline()
sys.stdout.write("Q2DZ/1 FATAL stage=local-description code=local-description\n")
sys.stdout.flush()
sys.stdin.read()
PY
chmod +x "$TMP/fake_fatal_gateway_omitted.py"
FATAL_OMITTED_CASE="$TMP/fatal-local-description"
mkdir -p "$FATAL_OMITTED_CASE"
mkfifo "$FATAL_OMITTED_CASE/control"
Q2DZ_TEST_MARKER="$FATAL_OMITTED_CASE/mark" Q2DZ_FAKE_LEGACY_MODE=normal \
  Q2DZ_FAKE_Q2_MODE=sleep Q2DZ_SYNTHETIC_CREDENTIAL='q2dz-secret-canary' \
  "$SESSION" --gateway "$TMP/fake_fatal_gateway_omitted.py" --legacy-gateway "$HERE/fake_legacy.py" \
    --adapter "$ADAPTER" --q2 "$HERE/fake_q2.py" --wine "$HERE/fake_wine.sh" \
    --game 3zb2 --runtime-base "$FATAL_OMITTED_CASE/runtime" --ready-timeout-ms 1200 \
    --shutdown-grace-ms 300 <"$FATAL_OMITTED_CASE/control" >"$FATAL_OMITTED_CASE/output" 2>&1 &
FATAL_OMITTED_OWNER=$!
exec {FATAL_OMITTED_WRITER}>"$FATAL_OMITTED_CASE/control"
printf '%s\n' 'Q2DZ/ROOMS/2 ICE generation=1 json=[{"urls":["stun:example.invalid"]}]' >&$FATAL_OMITTED_WRITER
for _ in $(seq 1 240); do
  grep -Eq 'GATEWAY event=fatal' "$FATAL_OMITTED_CASE/output" && break
  sleep 0.025
done
grep -Eq 'GATEWAY event=fatal reason=transport_reason_redacted restarts=0' "$FATAL_OMITTED_CASE/output" \
  || { echo "fatal-local-description: safe-mapped GATEWAY line missing" >&2; cat "$FATAL_OMITTED_CASE/output" >&2; exit 1; }
grep -q 'local-description' "$FATAL_OMITTED_CASE/output" && { echo "fatal-local-description: raw code leaked" >&2; cat "$FATAL_OMITTED_CASE/output" >&2; exit 1; }
exec {FATAL_OMITTED_WRITER}>&-
kill "$FATAL_OMITTED_OWNER" 2>/dev/null || true
wait "$FATAL_OMITTED_OWNER" 2>/dev/null || true

# Control framing remains fail-closed.
start_case malformed
printf 'Q2DZ/ROOMS/2 PUT room=bad json={}\n' >&$WRITER
exec {WRITER}>&-
set +e
wait "$OWNER"
rc=$?
set -e
[ "$rc" -eq 65 ]

printf 'fake shared lifecycle matrix: PASS\n'
