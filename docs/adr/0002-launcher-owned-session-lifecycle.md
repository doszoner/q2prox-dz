# ADR 0002: Launcher-owned join-session lifecycle

- **Status:** Accepted
- **Date:** 2026-08-22
- **Supersedes:** ad-hoc independent gateway and Q2 process lifetime
- **Keeps:** ADR 0001 WebView2 Candidate A

## Context

Historical maintainer observations on development and clean Windows VMs showed
the WebView2 transport and an unchanged Q2PRO-X game session, but also showed
that gateway, WebView2 and Q2 lifetimes were independent. Those observations
predate exact `1.0.0-rc6` artifact revalidation. Closing one component could
leave the others and the UDP/session resources alive. Runtime room configuration
contains short-lived ICE/TURN credentials and must not be placed in command
lines, environment variables or files.

## Decision

A small per-session coordinator owns the gateway and Q2PRO-X as one operation.

- It starts the gateway suspended and assigns it to a Windows Job Object before the gateway can create WebView2 processes.
- `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` is the hard parent-death guarantee.
- A separate inherited owner pipe permits graceful gateway shutdown.
- Fresh room JSON moves from launcher stdin to gateway stdin and is cleared from coordinator memory after delivery.
- The gateway binds a random loopback port and emits a strict non-sensitive event.
- Q2PRO-X starts only after `TRANSPORT_READY`.
- Q2PRO-X starts with `+set game 3zb2 +connect 127.0.0.1:<port>` in that exact order.
- Readiness requires a normal Quake II OOB `ping`/`ack`, not merely signaling, ICE or DataChannel open.
- DataChannel close or bounded explicit probe failure closes the session. Gameplay idleness never does.
- Exit of Q2, gateway, room or launcher moves the operation to `Closing`; the coordinator first closes the owner pipe and then terminates the Job on timeout.
- Each operation has a unique WebView2 user-data directory. An exclusive owner lock distinguishes active and stale directories.

## Consequences

The normal Q2PRO-X shortcut remains untouched. No process injection, CFG edit,
portal automation, legacy Chromium backend, credential file or command-line
secret fallback is allowed. The current-user installer and room catalog are
implemented. Windows rc6 community artifacts are unsigned and closed-alpha-only
for correctness testing. Authenticode remains required for broad public trust
and distribution, not for the closed-alpha correctness gate.

## Required verification

1. portable and Windows fake-process tests;
2. normal exit, gateway crash, launcher crash, forced termination and room-close matrix against the exact gateway;
3. WebView2 process-tree and user-data cleanup;
4. two-build `/Brepro` identity;
5. clean Windows x64 Gate P with fresh room data;
6. content-profile checks and no persistent credential-shaped strings.

Historical maintainer evidence reported all six groups passing on development
and clean Windows VMs. Raw VM evidence remains outside the public source tree;
the exact rc6 results and their later Windows field BLOCK are preserved in
[`docs/verification/1.0.0-rc6.md`](../verification/1.0.0-rc6.md). The historical
hard-kill observation left only the expected stale session directory; the next
launch removed it before creating a new session.
