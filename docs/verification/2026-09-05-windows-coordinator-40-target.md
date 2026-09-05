# Windows coordinator 40-target gate, rc15

Purpose: verify the product-level Windows path for 40 shared room targets:
packaged `q2dz-session.exe`, packaged WebView2 gateway, coordinator retry
heartbeats, and Custom source publication.

This is still a fixture gate. It does not prove Q2PRO-X menu rendering or
gameplay.

## Inputs

```text
VM: Windows 11 25H2 + LTSC (x64) 28in1
VM UUID: 9a57e185-2b98-458b-b58f-c9e23e666ce9
portable: C:\Users\user\q2dz-release\rc15\portable
session: portable\q2dz-session.exe
gateway: portable\gateway\q2-doszone-webview2-gateway.exe
fake Q2: C:\Users\user\q2dz-release\rc15\coordinator-fixture-build\Release\q2dz-fake-q2.exe
host peer harness: tests/tools/controlled-room-set.py webrtc --count 40
aliases: room.q2ded.testserver01..40
```

The fake Q2 fixture was rebuilt from the current source so it can accept up to
256 Custom source endpoints. The product coordinator and gateway came from the
rc15 portable package.

The harness simulated the launcher by sending a complete `Q2DZ/ROOMS/2`
heartbeat every 15 seconds.

## Result

Host controlled peers:

```text
peer_count=40
target_count=40
ready=40/40
```

Coordinator-level run:

```text
HEARTBEAT_SENT 1
HEARTBEAT_SENT 2
HEARTBEAT_SENT 3
HEARTBEAT_SENT 4
HEARTBEAT_SENT 5
HEARTBEAT_SENT 6
HEARTBEAT_SENT 7
GATE_STATUS PASS source40 heartbeat_count=7
SESSION_EXIT 0
```

Fake Q2 observed these Custom source sizes over time:

```text
0, 2, 7, 12, 14, 18, 19, 23, 25, 27, 29, 30, 31, 32, 33, 38, 39, 40
```

Cleanup:

```text
host controlled peer children_alive=0
guest NO_PRODUCT_OR_HARNESS_PROCESSES
guest NO_COORD_40_TEMP_DIRS
```

The raw GuestControl command returned a non-zero code because the temporary
Python harness removed its marker directory before the final `sys.exit`
expression. The harness output above is the useful evidence: it saw
`q2-source-40.marker`, then `q2dz-session.exe` exited with code `0`.

## Verdict

`PASS` for the Windows coordinator-level 40-target fixture gate.

Proven:

- product `q2dz-session.exe` can keep Q2 alive while the shared gateway fills
  the Custom source;
- product WebView2 gateway can eventually publish 40 target endpoints when the
  coordinator sends repeated launcher-style heartbeats;
- the previous raw-gateway `39/40` observation is recovered at product level;
- cleanup left no product or harness processes.

Not proven here:

- exact Q2PRO-X Custom source menu rendering;
- `status -> connect` source tuple;
- live gameplay;
- 30-minute Windows resource/churn behavior.
