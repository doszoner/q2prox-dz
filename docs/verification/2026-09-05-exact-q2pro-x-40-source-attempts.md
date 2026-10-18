# Exact Q2PRO-X 40-endpoint Custom source attempts, rc15

Date: 2026-09-05.

Purpose: try to close the remaining exact Q2PRO-X Custom-source/menu gate for
the shared multiroom gateway. The expected proof is:

1. exact Q2PRO-X 1.5 receives a 40-row `cl_serverbrowser_custom_source`;
2. `Join online -> Custom` / refresh causes ordinary Quake II `status` traffic
   to all 40 loopback endpoints;
3. the evidence records the real client behavior, not a fake Q2 fixture.

The verified exact executable remains:

```text
~/Games/Quake2-PlayGround-2026/Q2PRO-X.exe
SHA-256: 831f42dc96e3399a8a8737943d6fde752e11f5e5f48ffc5957537c4e66cbf71d
Q2PRO-X 1.5 by ly, 2026-08-25
```

## Linux isolated Xvnc attempts

Three isolated Linux runs were attempted under private Xvnc displays and
private `homedir` roots:

```text
noncommit/q2prox-dz-gates/20260905-q2pro-40-source-run1
noncommit/q2prox-dz-gates/20260905-q2pro-40-source-run2
noncommit/q2prox-dz-gates/20260905-q2pro-40-source-run3
```

The fixture was the retained 40-row source harness:

```text
tests/tools/controlled-room-set.py source --count 40
```

Observed results:

```text
run1: http_gets=0 status_target_count=0 window_found=false
run2: http_gets=0 status_target_count=0 window_found=false
run3: http_gets=0 status_target_count=0 window_found=true
```

The Q2 logs show slow Wine/Xvnc renderer startup. In the useful run, Q2 reached
`R_Init`, but the client had not completed initialization and did not request
the Custom source before cleanup.

Conclusion: these attempts do not prove a product failure. They prove that the
current Linux/Xvnc automation is not a valid unattended exact-client gate for
this check.

## Linux isolated command-refresh attempts

Two more bounded attempts checked whether the exact client can be driven
without manual menu input by using built-in commands found in the executable:

```text
serverbrowser_open
serverbrowser_refresh
serverbrowser_connect
```

Attempt 1 used startup arguments:

```text
+serverbrowser_open +serverbrowser_refresh
```

Evidence:

```text
run: noncommit/q2prox-dz-gates/20260905-q2pro-40-command-refresh
Q2PRO-X initialized: yes
HTTP GETs: 0
UDP status targets: 0
```

Attempt 2 used an isolated `Xvnc` display plus `twm`, bound F12 to:

```text
serverbrowser_open; serverbrowser_refresh
```

and tried to send F12 with `xdotool` inside the isolated display.

Evidence:

```text
run: noncommit/q2prox-dz-gates/20260905-q2pro-40-twm-f12-refresh
HTTP GETs: 0
UDP requests: 0
result: invalid automation attempt
```

The second attempt timed out before reliable key delivery and had to be
cleaned up by exact process group. It is not counted as product behavior.

Conclusion: the exact-client 40-row gate should stay manual/interactive. The
known supported user contract is the normal menu path: open `Join online`,
select `Custom`, press `Refresh`, then select a row. Command-driven refresh is
not sufficient evidence for this gate.

## Linux isolated Xvnc attempt 4

A fourth bounded Xvnc attempt used the same 40-row source fixture, a private
`homedir`, `twm` and the exact Q2PRO-X executable:

```text
run: noncommit/q2prox-dz-gates/20260905-exact-q2pro-x-xvnc-attempt4
source fixture: ready, 40 endpoints
Q2PRO-X args: +set homedir <run>/q2home +set game 3zb2 +set cl_timeout 20
  +set s_initsound 0 +set cl_serverbrowser_default_source custom
  +set cl_serverbrowser_custom_source http://127.0.0.1:<port>/servers
Q2PRO-X log: reached FS_Init and R_Init
HTTP GETs: 0
UDP status targets: 0
cleanup: owned processes stopped
```

This attempt does not prove a product failure. It also does not prove an Xvnc
server failure: a later diagnostic command and cleanup were started in parallel,
so `xwininfo`/screenshot errors from that moment are not reliable display-state
evidence. The useful observation is only that this unattended Xvnc shape again
failed to produce ordinary `Custom` menu traffic before cleanup.

## Windows VM non-interactive attempt

The prepared Windows VM was started headless:

```text
VM: Windows 11 25H2 + LTSC (x64) 28in1
UUID: 9a57e185-2b98-458b-b58f-c9e23e666ce9
snapshot: q2prox-rc15-fixture-ready-20260905
```

The guest had no logged-on interactive user:

```text
query user
No User exists for *
```

The 40-row fixture and exact Q2PRO-X were launched through GuestControl. The
guest-side summary was copied back to:

```text
noncommit/q2prox-dz-gates/20260905-windows-q2pro-40-source/summary.json
```

Result:

```json
{
  "result": "FAIL",
  "q2WindowSeen": false,
  "httpGets": 0,
  "statusRequestCount": 0,
  "statusTargetCount": 0,
  "statusSourceCount": 0
}
```

This is a GuestControl-session limitation, not a product gate result:
non-interactive Q2PRO-X process start is insufficient evidence for the menu
contract.

Post-run process check found no remaining `Q2PRO-X`, fixture Python process,
`q2dz-session`, or WebView2 gateway process.

## Current verdict

`PENDING` for the exact Q2PRO-X 40-endpoint Custom-source/menu gate.

Already proven nearby:

- exact Q2PRO-X Custom source/status/connect behavior for two endpoints:
  [2026-08-29-q2pro-x-custom-source.md](2026-08-29-q2pro-x-custom-source.md);
- Windows coordinator-level 40-target source publication with fake Q2:
  [2026-09-05-windows-coordinator-40-target.md](2026-09-05-windows-coordinator-40-target.md);
- Windows 30-minute 40-target resource/churn fixture gate:
  [2026-09-05-windows-resource-40-target.md](2026-09-05-windows-resource-40-target.md).

The smallest valid next action is an interactive exact-client gate in a logged
in Windows session or an operator-owned Linux GUI session. It should reuse the
existing 40-row fixture and collect both screenshot and UDP evidence.
