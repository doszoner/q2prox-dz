# Launcher-owned session coordinator

This directory closes the process-lifecycle gap identified by the Windows runtime evidence. It does not replace the proven WebView2 transport and does not modify Q2PRO-X.

## Implemented

- one hub session containing one shared gateway and up to `256` room targets;
- direct child launch without shell expansion;
- one root Windows Job Object for hard session cleanup, with `KILL_ON_JOB_CLOSE`;
- the shared gateway starts suspended, is assigned to the Job, then resumed;
- bounded room configurations from stdin to child stdin only;
- a separate inherited owner pipe and event reader for the shared gateway;
- strict, non-sensitive gateway event protocol;
- Q2 starts only after all selected rooms have either produced `TRANSPORT_READY` or been removed, with validated loopback endpoints;
- Q2 starts with `+set game 3zb2`, session-only `cl_timeout 20` and a bounded
  `cl_serverbrowser_custom_source`; the coordinator sends no menu command and
  leaves Q2PRO-X's ordinary startup preference unchanged;
- a room failure closes only that room target; Q2, other targets and the common
  Job continue;
- Q2, owner stop or an empty hub stops all rooms;
- unique WebView2 user-data directory per hub session and stale-session cleanup;
- an out-of-Job cleanup guardian removes the session directory after hard owner termination;
- no CFG edits and no game installation changes;
- `/Brepro` and hardening flags;
- portable core tests, JavaScript readiness-probe tests and Windows fake-process lifecycle matrix.

## Build

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\build-release.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\run-lifecycle-matrix.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\check-reproducible.ps1 `
  -WebView2SdkDir C:\deps\webview2-sdk `
  -WebRtcNetReleaseDir C:\deps\webrtc-release
```

The exact tested WebView2 gateway must implement `MANAGED-GATEWAY-PROTOCOL.md`. No command-line/file fallback for ICE/TURN is supplied intentionally.

## Hub invocation

A launcher/catalog component writes fresh room updates to stdin before and
while Q2PRO-X is running:

```powershell
$roomUpdates | ./out/release/q2dz-session.exe `
  --gateway C:\path\q2dz-gateway.exe `
  --q2 C:\path\Q2PRO-X.exe
```

The coordinator itself has no room picker and no direct `+connect` mode. The
user opens Q2PRO-X `Join online` → `Custom`; Q2PRO-X owns room selection. The old
single-room direct-connect behavior is retained only in historical release
artifacts, not in the current executable.

Legacy `q2` rooms keep the existing tombstone policy. A `room.q2ded.*` target
may start twice more after failure or `ROOM_CLOSED`; successful readiness does
not reset this budget. Closing one room never closes Q2PRO-X or sibling rooms.
