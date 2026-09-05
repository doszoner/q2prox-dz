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
rc15 artifacts: pending rebuild after correction
```

Prepared Windows VM:

```text
vm: Windows 11 25H2 + LTSC (x64) 28in1
uuid: 9a57e185-2b98-458b-b58f-c9e23e666ce9
running VMs: none
current snapshot: q2prox-rc14-live-ready-20260905
current snapshot UUID: 8110ebf4-1666-427c-82a9-458ee95d297b
```

Local Linux GUI isolation:

```text
Xvfb: not installed
xvfb-run: not installed
```

## Gates still open

| Gate | Status | Why not closed | Smallest next action |
| --- | --- | --- | --- |
| Exact Q2PRO-X 40-endpoint Custom source/menu gate | `PENDING` | Needs real Q2PRO-X to load the 40-endpoint source, refresh `Custom`, send `status` to all endpoints and prove `status -> connect` source tuple. | Run the retained `controlled-room-set.py source --count 40` with Q2PRO-X in an isolated GUI or in the prepared Windows VM. |
| Linux fresh-package gameplay gate | `PENDING` | rc15 controlled gateway admission passes, but map entry, movement/fire, room switch, active/inactive close and cleanup have not been proven from the fresh package. | Install/use isolated X server, or run with explicit operator-owned GUI session. Do not run unattended on primary `DISPLAY=:1`. |
| Windows fresh portable/setup gameplay gate | `PENDING` | VM has rc14 staged and Q2PRO-X path set, but no fresh interactive Q2PRO-X/WebView2/Dos.Zone gameplay PASS is recorded for an rc15 rebuild. | Rebuild Windows rc15 first; then restore/start `q2prox-rc14-live-ready-20260905` or a successor snapshot and collect gameplay/process cleanup evidence. |
| Windows WebView2 two-target transport/runtime gate | `PENDING` | Node protocol tests pass, but a real WebView2/Q2PRO-X runtime with one gateway and two live targets has not been measured. | Use the prepared VM; prove one gateway/WebView2 tree, no 1 Hz pump gaps, isolated close and switch. |
| 30-minute shared-gateway resource/churn gate | `PENDING` | rc15 fixed the immediate controlled admission stall and reached `40/40 READY`, but the longer resource/churn measurement has not been rerun. | Run the retained controlled 40-target resource/churn gate with bounded RSS/CPU/fd evidence. |
| Final release acceptance | `PENDING` | Depends on the live/resource gates above. | After gates pass, rebuild release artifacts if any code/docs changed, update `CHANGELOG.md` and verification, then run final smoke/review. |

## Boundary

Do not mark the active goal complete from current evidence. rc15 is a
correction candidate, but final product acceptance still requires the live gates
above.

Related failure evidence:
[2026-09-05-shared-gateway-capacity-stall.md](2026-09-05-shared-gateway-capacity-stall.md).
