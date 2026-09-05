# Remaining release gates, 2026-09-05

Purpose: keep the active shared multiroom gateway goal honest after the rc15
Linux capacity/liveness correction.

## Current verified baseline

```text
checkout: /home/ssolovey/Projects/Q2/DosZone/q2prox-dz-dedicated-compat
branch: feat/dedicated-compat
VERSION: 1.0.0-rc15
git identity: doszoner <doszoner@users.noreply.github.com>
status: updated by the rc15 correction work
```

Artifact integrity:

```text
dist/q2prox-dz-1.0.0-rc15-windows-x64-portable.zip: SHA-256 OK
dist/q2prox-dz-1.0.0-rc15-windows-x64-setup.zip: SHA-256 OK
linux/dist/q2prox-dz-1.0.0-rc15-linux-x86_64.tar.gz: SHA-256 OK
dist/q2prox-dz-1.0.0-rc15-test-bundle.zip: SHA-256 sidecar OK
```

Prepared Windows VM:

```text
vm: Windows 11 25H2 + LTSC (x64) 28in1
uuid: 9a57e185-2b98-458b-b58f-c9e23e666ce9
running VMs: none
current snapshot: q2prox-rc15-fixture-ready-20260905
current snapshot UUID: a8badfdb-6b06-4bf8-b37f-1cd7857c8229
prepared portable: C:\Users\user\q2dz-release\rc15\portable
prepared Q2PRO-X: C:\Users\user\q2-runtime-20260905\Quake2-PlayGround-2026\Q2PRO-X.exe
prepared fake Q2: C:\Users\user\q2dz-release\rc15\coordinator-fixture-build\Release\q2dz-fake-q2.exe
portable internal SHA256SUMS: PASS
```

Local Linux GUI isolation:

```text
Xvfb: not installed
xvfb-run: not installed
```

## Gates still open

| Gate | Status | Why not closed | Smallest next action |
| --- | --- | --- | --- |
| 40-endpoint Custom source fixture | `PASS` | `controlled-room-set.py source --count 40 --duration 10` served 40 loopback endpoints and completed cleanup. This proves the retained source/status fixture still works, not Q2PRO-X menu behavior. | None for the fixture. |
| Exact Q2PRO-X 40-endpoint Custom source/menu gate | `PENDING` | Needs real Q2PRO-X to load the 40-endpoint source, refresh `Custom`, send `status` to all endpoints and prove `status -> connect` source tuple. Linux/Xvnc and non-interactive Windows attempts did not produce valid client evidence; see [2026-09-05-exact-q2pro-x-40-source-attempts.md](2026-09-05-exact-q2pro-x-40-source-attempts.md). | Run [2026-09-05-exact-q2pro-x-40-source-runbook.md](2026-09-05-exact-q2pro-x-40-source-runbook.md) in a logged-in Windows session or an operator-owned Linux GUI session. |
| Linux fresh-package gameplay gate | `PENDING` | rc15 controlled gateway admission passes, but map entry, movement/fire, room switch, active/inactive close and cleanup have not been proven from the fresh package. | Install/use isolated X server, or run with explicit operator-owned GUI session. Do not run unattended on primary `DISPLAY=:1`. |
| Windows fresh portable/setup gameplay gate | `PENDING` | rc15 portable/setup package chain passes, but no fresh interactive Q2PRO-X/WebView2/Dos.Zone gameplay PASS is recorded for these rc15 archives. | Stage rc15 in the prepared Windows VM or on a tester machine, then collect gameplay/process cleanup evidence. |
| Windows WebView2 two-target transport/runtime gate | `PASS` | The packaged rc15 WebView2 gateway in the prepared VM reached `READY` for two controlled WebRTC targets and exited cleanly after `STOP`; see [2026-09-05-windows-webview2-two-target.md](2026-09-05-windows-webview2-two-target.md). | None for the two-target transport gate. |
| Windows raw WebView2 40-target burst | `OBSERVED FAIL` | A direct gateway-only harness reached `39/40 READY`; target `5` failed with `alias_timeout`. Timers were normal, so this is not the 1 Hz WebView2 issue. See [2026-09-05-windows-webview2-40-target-observation.md](2026-09-05-windows-webview2-40-target-observation.md). The later coordinator-level gate passed, so this remains a raw-harness observation, not a release blocker by itself. | None unless a future raw-gateway diagnostic needs the same burst shape. |
| Windows coordinator-level 40-target fixture gate | `PASS` | Product `q2dz-session.exe` plus packaged WebView2 gateway eventually published 40 Custom source endpoints after repeated launcher-style heartbeats; see [2026-09-05-windows-coordinator-40-target.md](2026-09-05-windows-coordinator-40-target.md). | None for the coordinator fixture gate. |
| Linux 30-minute shared-gateway resource/churn gate | `PASS` | Controlled 40-target hold ran for 1800 seconds after `40/40 READY`; gateway stdout was drained continuously; cleanup was normal. | None for the Linux controlled resource gate. |
| Windows 30-minute WebView2 resource/churn gate | `PASS` | Product-level rc15 Windows path held 40 controlled targets for 1800 seconds with bounded process count/memory and clean stop; see [2026-09-05-windows-resource-40-target.md](2026-09-05-windows-resource-40-target.md). | None for the fixture resource gate. |
| Final release acceptance | `PENDING` | Depends on the live/resource gates above. | After gates pass, rebuild release artifacts if any code/docs changed, update `CHANGELOG.md` and verification, then run final smoke/review. |

## Boundary

Do not mark the active goal complete from current evidence. rc15 is a
correction candidate, but final product acceptance still requires the live gates
above.

Related failure evidence:
[2026-09-05-shared-gateway-capacity-stall.md](2026-09-05-shared-gateway-capacity-stall.md).
