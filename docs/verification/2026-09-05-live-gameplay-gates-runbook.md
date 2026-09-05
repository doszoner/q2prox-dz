# Live gameplay gates runbook, 2026-09-05

Purpose: close the remaining package gates that require a real Q2PRO-X,
Dos.Zone room and human-visible gameplay. Fake Q2, fixture peers and headless
GuestControl are not valid evidence for this runbook.

Use neutral room and player names in shared evidence.

## Common PASS criteria

- the artifact SHA-256 matches its sidecar;
- Q2PRO-X exact path and version/hash are recorded;
- the package starts Q2PRO-X with `+set game 3zb2`;
- a real Dos.Zone room is visible in `Join online -> Custom` after `Refresh`;
- Q2PRO-X enters the map;
- movement and fire are confirmed after map entry;
- switching Room A -> disconnect/menu -> Room B works without restarting
  Q2PRO-X;
- closing an active room leaves Q2PRO-X recoverable through the menu;
- closing an inactive listed room removes it after `Refresh`;
- after exit, no q2prox-dz gateway, helper, Q2PRO-X, Wine or WebView2 session
  process remains;
- session directories are removed or contain only documented non-secret state;
- logs and package files contain no credential-shaped room, ICE, TURN, SDP or
  candidate data.

## Linux fresh-package gameplay

Run in an operator-owned GUI session. Do not run unattended on the user's
primary display.

```bash
cd /tmp
sha256sum -c /path/to/q2prox-dz-1.0.0-rc15-linux-x86_64.tar.gz.sha256
tar -xf /path/to/q2prox-dz-1.0.0-rc15-linux-x86_64.tar.gz
./q2prox-dz-1.0.0-rc15-linux-x86_64/install-user.sh
q2prox-dz
```

Evidence to keep under `noncommit/q2prox-dz-gates/<date>-linux-live/`:

- artifact path and SHA-256;
- Q2PRO-X path and hash;
- launcher/session output;
- sanitized room identifiers;
- notes for map entry, movement/fire, Room A -> Room B switch, active close,
  inactive close and cleanup process check.

Cleanup check:

```bash
pgrep -af 'q2prox|q2dz|Q2PRO-X|wine|WebView2' || true
find "${XDG_RUNTIME_DIR:-/tmp}" /tmp -maxdepth 2 -iname '*q2*dz*' -print 2>/dev/null
```

## Windows portable gameplay

Run in a logged-in Windows desktop session.

```cmd
certutil -hashfile q2prox-dz-1.0.0-rc15-windows-x64-portable.zip SHA256
powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive .\q2prox-dz-1.0.0-rc15-windows-x64-portable.zip .\portable-fresh"
cd portable-fresh
Run.cmd
```

Evidence to keep:

- artifact path and SHA-256;
- Q2PRO-X path selected by the package;
- `VERSION`;
- launcher/session output;
- sanitized room identifiers;
- notes for map entry, movement/fire, Room A -> Room B switch, active close,
  inactive close and cleanup process check.

Cleanup check:

```cmd
tasklist | findstr /I "q2prox q2dz Q2PRO-X msedgewebview2"
dir /S "%LOCALAPPDATA%\Q2PRO-X Dos.Zone"
```

`q2-path.txt` is allowed. Room credentials, ICE, TURN, SDP and candidates are
not allowed.

## Windows setup gameplay

Run after the portable gate, from a fresh extraction of the setup ZIP.

```cmd
certutil -hashfile q2prox-dz-1.0.0-rc15-windows-x64-setup.zip SHA256
powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive .\q2prox-dz-1.0.0-rc15-windows-x64-setup.zip .\setup-fresh"
cd setup-fresh
install.cmd
```

Start Q2PRO-X Dos.Zone from the installed shortcut or installed `Run.cmd`.
Repeat the same gameplay and cleanup checks as the portable gate.

Then verify uninstall is conservative:

```cmd
uninstall.cmd
dir "%LOCALAPPDATA%\Q2PRO-X Dos.Zone"
```

The saved Q2PRO-X path may remain. Installed owned files must be removed.

## What does not pass this runbook

- DataChannel open without map entry;
- map entry without movement/fire;
- fake Q2;
- synthetic room status only;
- headless GuestControl with no logged-in desktop;
- a package rebuilt after the evidence was collected.
