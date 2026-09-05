# Controlled 40-target harness, 2026-09-05

Purpose: verify the retained non-GUI harness used by the shared-gateway
capacity gates after replacing its default alias prefix with a valid neutral
Dos.Zone shape.

This is not a Q2PRO-X gameplay gate. It proves the controlled harness and
public signaling side can publish forty neutral `room.q2ded.*` aliases and
clean up every child process.

## Root cause fixed

The harness default alias prefix was `room.q2test.server`. The controlled peer
binary deliberately accepts only supported production-shaped aliases:

```text
q2.*
room.q2ded.*
```

Therefore `controlled-room-set.py webrtc --count 1` failed before readiness
with:

```text
{"event":"fatal","code":"invalid_options"}
```

The default prefix is now:

```text
room.q2ded.testserver
```

## Commands

```bash
python3 tests/tools/test_controlled_room_set.py
```

Result:

```text
controlled-room-set source self-check: PASS
```

```bash
python3 tests/tools/controlled-room-set.py source --count 40 --duration 10
```

Result:

```text
status: PASS
ready endpoint_count: 40
cleanup: complete
```

```bash
printf '[{"urls":["stun:example.invalid"]}]\n' |
  python3 tests/tools/controlled-room-set.py webrtc \
    --count 40 \
    --peer-binary linux/build-release/q2dz-controlled-peer \
    --duration 20
```

Result:

```text
status: PASS
started target_count: 40
peer_ready events: 40
unique ready aliases: 40
errors: 0
final cleanup children_alive: 0
```

## 60-second controlled resource smoke

After the rc14 bundle refresh, the WebRTC controlled-peer harness was run once
more for 60 seconds. This used public signaling and neutral
`room.q2ded.testserverNN` aliases. It did not start Q2PRO-X or use the desktop.

```bash
python3 tests/tools/controlled-room-set.py webrtc \
  --count 40 \
  --peer-binary linux/build-release/q2dz-controlled-peer \
  --duration 60 \
  --evidence <temp>/evidence.jsonl
```

Result:

```text
status: PASS
started target_count: 40
peer_ready events: 40
unique ready aliases: 40
errors: 0
final cleanup children_alive: 0
stderr: empty
resource samples: 12
sample max RSS sum: 667.7 MiB
sample max CPU sum: 562.3%
```

Boundary: this is a harness/process-fanout smoke. The high CPU sample is from
forty separate controlled peer processes during startup/idle, not from one
shared q2prox-dz gateway process. It does not replace the planned 30-minute
shared-gateway resource gate.

## Boundary

This evidence does not replace:

- fresh-package Q2PRO-X map-entry/gameplay checks;
- Windows WebView2/Q2PRO-X runtime checks;
- long resource measurement with a real Q2PRO-X Custom browser.
