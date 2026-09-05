# Windows WebView2 40-target observation, rc15

Purpose: check whether the packaged rc15 Windows WebView2 gateway can bring
40 controlled room targets to `READY` in one direct HUB/2 burst.

This run bypassed `q2dz-session.exe`. Therefore it tests the raw shared
gateway behavior, not the full product retry loop owned by the coordinator.

## Inputs

```text
VM: Windows 11 25H2 + LTSC (x64) 28in1
VM UUID: 9a57e185-2b98-458b-b58f-c9e23e666ce9
portable: C:\Users\user\q2dz-release\rc15\portable
gateway: portable\gateway\q2-doszone-webview2-gateway.exe
host peer harness: tests/tools/controlled-room-set.py webrtc --count 40
aliases: room.q2ded.testserver01..40
```

The host peer harness reached `40/40 ready` before the gateway run. Fresh ICE
was passed through stdin and not recorded here.

## Result

```text
Q2DZ/HUB/2 HEARTBEAT state=ok targets=40 ready=0
READY_PROGRESS 1/40
Q2DZ/HUB/2 HEARTBEAT state=ok targets=40 ready=4
...
Q2DZ/HUB/2 FAILED id=5 stage=resolve code=alias_timeout
Q2DZ/HUB/2 HEARTBEAT state=ok targets=39 ready=8
READY_PROGRESS 10/40
READY_PROGRESS 20/40
READY_PROGRESS 30/40
Q2DZ/HUB/2 HEARTBEAT state=ok targets=39 ready=39
...
GATE_STATUS FAIL ready=39 exit=None
```

Cadence snapshots during the run showed normal JavaScript timer cadence:

```text
timerTicks≈200 per second
timerMaxGapMs=6..7
```

Process cleanup after the forced harness stop:

```text
NO_PRODUCT_OR_HARNESS_PROCESSES
GUEST_40_TEMP_REMOVED
```

## Interpretation

This is not the old Windows 1 Hz/WebView2 background throttling symptom:
`timerMaxGapMs` stayed around 6..7 ms.

The visible failure is one alias-resolution timeout during a 40-target burst.
The gateway intentionally resolves at most one unresolved alias route at a
time. After target `5` timed out, the remaining targets continued and reached
`READY`.

The full product may still recover this target, because the coordinator treats
dedicated-room failures as retryable and can re-send desired rooms on a later
heartbeat. This direct gateway harness did not include that coordinator retry
loop.

## Verdict

`FAIL` for a direct raw-gateway 40-target burst.

`INCONCLUSIVE` for the product-level 40-target gate.

Smallest next check: run a coordinator-level 40-target gate with a fake/stub Q2
that stays alive and tolerates more than two Custom source endpoints. Do not
run a 30-minute Windows resource gate until the coordinator-level 40-target
readiness gate passes.
