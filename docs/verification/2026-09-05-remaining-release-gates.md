# Remaining release gates, 2026-09-05

Purpose: keep the active shared multiroom gateway goal honest after the rc14
automated checks, bundle refresh and VM cleanup.

## Current verified baseline

```text
checkout: /home/ssolovey/Projects/Q2/DosZone/q2prox-dz-dedicated-compat
branch: feat/dedicated-compat
HEAD: ab69666762938d5bdd0ee4cff21d68f3c6c589ec
VERSION: 1.0.0-rc14
git identity: doszoner <doszoner@users.noreply.github.com>
status: clean before this note
```

Artifact integrity:

```text
dist/q2prox-dz-1.0.0-rc14-test-bundle.zip: SHA-256 OK, unzip -t PASS
dist/q2prox-dz-1.0.0-rc14-windows-x64-portable.zip: SHA-256 OK
dist/q2prox-dz-1.0.0-rc14-windows-x64-setup.zip: SHA-256 OK
linux/dist/q2prox-dz-1.0.0-rc14-linux-x86_64.tar.gz: SHA-256 OK
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
| Linux fresh-package gameplay gate | `PENDING` | rc14 is installed locally and non-GUI checks passed, but map entry, movement/fire, room switch, active/inactive close and cleanup have not been proven from the fresh package. | Install/use isolated X server, or run with explicit operator-owned GUI session. Do not run unattended on primary `DISPLAY=:1`. |
| Windows fresh portable/setup gameplay gate | `PENDING` | VM has rc14 staged and Q2PRO-X path set, but no fresh interactive Q2PRO-X/WebView2/Dos.Zone gameplay PASS is recorded for this rebuild. | Restore/start `q2prox-rc14-live-ready-20260905`, log in, run the desktop portable command, collect gameplay and process cleanup evidence. |
| Windows WebView2 two-target transport/runtime gate | `PENDING` | Node protocol tests pass, but a real WebView2/Q2PRO-X runtime with one gateway and two live targets has not been measured. | Use the prepared VM; prove one gateway/WebView2 tree, no 1 Hz pump gaps, isolated close and switch. |
| 30-minute shared-gateway resource/churn gate | `PENDING` | A 60-second controlled peer process-fanout smoke passed, but it measures 40 controlled peer processes, not one shared q2prox-dz gateway under churn. | Run the planned 30-minute shared-gateway resource test and record RSS/CPU/fd/handles once per minute. |
| Final release acceptance | `PENDING` | Depends on the live/resource gates above. | After gates pass, rebuild release artifacts if any code/docs changed, update `CHANGELOG.md` and verification, then run final smoke/review. |

## Boundary

Do not mark the active goal complete from current evidence. rc14 is suitable as
a test-group bundle, but final product acceptance still requires the live gates
above.
