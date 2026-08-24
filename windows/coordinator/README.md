# Launcher-owned session coordinator

This directory closes the process-lifecycle gap identified by the Windows runtime evidence. It does not replace the proven WebView2 transport and does not modify Q2PRO-X.

## Implemented

- strict session state machine;
- direct child launch without shell expansion;
- Windows Job Object with `KILL_ON_JOB_CLOSE`;
- gateway and Q2 start suspended, assigned to the Job, then resumed;
- room configuration from stdin to child stdin only;
- separate inherited owner pipe;
- strict, non-sensitive gateway event protocol;
- Q2 starts only after `TRANSPORT_READY` with a validated loopback endpoint;
- Q2 starts with exact `+set game 3zb2 +connect 127.0.0.1:<port>` ordering;
- shutdown when Q2, gateway, room or owner stops;
- unique WebView2 user-data directory and stale-session cleanup;
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

## Join invocation

A launcher/catalog component writes one fresh room JSON object to stdin:

```powershell
$freshRoomJson | ./out/release/q2dz-session.exe `
  --gateway C:\path\q2dz-gateway.exe `
  --q2 C:\path\Q2PRO-X.exe
```

The ordinary Q2PRO-X shortcut remains independent.
