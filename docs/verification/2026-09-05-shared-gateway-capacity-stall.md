# Shared gateway controlled capacity stall, 2026-09-05

Purpose: record a failed non-GUI admission attempt for the Linux shared gateway
against controlled WebRTC peers.

This is not a Q2PRO-X gameplay test. It uses the real
`linux/build-release/q2dz-webrtc-gateway` and the existing controlled peers.
The run does not use the desktop.

## Setup

```text
gateway: linux/build-release/q2dz-webrtc-gateway
peer binary: linux/build-release/q2dz-controlled-peer
peer harness: tests/tools/controlled-room-set.py webrtc
aliases: room.q2ded.testserverNN
ICE input: one-line non-secret test JSON with stun:example.invalid
gateway stderr: discarded in the later runs to avoid preserving raw SDP/ICE
```

## Result

Controlled peers alone still registered successfully, but the shared gateway
did not make every target `READY` and did not emit `FAILED`/`CLOSED` for the
missing targets.

```text
attempt: 40 targets, 45-second gateway readiness window
controlled peers: 40/40 ready
gateway READY: 39/40
missing ids: 40
FAILED events: 0
CLOSED events: 0
last heartbeat: targets=40 ready=39
```

```text
attempt: 40 targets, 120-second gateway readiness window
controlled peers: 40/40 ready
gateway READY: 33/40
missing ids: 1, 4, 8, 11, 12, 16, 22
FAILED events: 0
CLOSED events: 0
last heartbeat: targets=40 ready=33
```

```text
attempt: 30 targets, 90-second gateway readiness window
controlled peers: 30/30 ready
gateway READY: 29/30
missing ids: 12
FAILED events: 0
CLOSED events: 0
peer cleanup: children_alive=0
last heartbeat: targets=30 ready=29
```

## Interpretation

The 30/40-target shared-gateway resource gate is blocked. The current evidence
does not prove a memory leak or CPU limit; it proves an admission/liveness
problem before the resource measurement can start.

The immediate correctness issue is that some targets remain counted in
`targets=N` without reaching `READY` and without a terminal `FAILED`/`CLOSED`
event inside the bounded window. That violates the design expectation that an
unresolved target eventually reports a stable failure such as `alias_timeout`.

Root cause is not yet proven. Plausible areas to inspect next:

- target resolve deadline starts too late or is not checked while the gateway
  is busy opening many routes;
- the resolve/probe scheduler can leave one target unscheduled under bursty
  multi-target startup;
- WebRTC-NET connection state can fail/close without being mapped to a target
  terminal event in this batch scenario.

## Next action

Do not tune capacity constants or mark the resource gate passed. Add a focused
regression around many resolving targets and bounded terminal events, then fix
the shared gateway so every admitted target reaches `READY` or a terminal event
inside the configured deadline.

## Cleanup

No `q2dz-controlled-peer`, `controlled-room-set.py` or
`q2dz-webrtc-gateway` process remained after the checks. Temporary raw run
directories were not committed; the first run contained raw dependency
diagnostics and must not be copied into the repository.
