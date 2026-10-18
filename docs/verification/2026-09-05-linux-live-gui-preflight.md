# Linux live gameplay gate preflight, 2026-09-05

Purpose: decide whether the remaining Linux fresh-package gameplay gate can be
run unattended without stealing the user's desktop.

## Current state

```text
repo: /path/to/DosZone/q2prox-dz-dedicated-compat
branch: feat/dedicated-compat
package: linux/dist/q2prox-dz-1.0.0-rc14-linux-x86_64.tar.gz
package smoke: PASS
DISPLAY: :1
WAYLAND_DISPLAY: wayland-0
```

Available tools:

```text
wine: /usr/bin/wine
xdotool: /usr/bin/xdotool
Xwayland: /usr/bin/Xwayland
Xorg: /usr/bin/Xorg
Xvfb: not installed
xvfb-run: not installed
Xephyr: not installed
weston/gamescope/cage/xpra/x11vnc: not installed
sudo without password: unavailable
```

No `q2dz`, `Q2PRO-X`, `wine`, `wineserver` or `VBoxHeadless` test process was
left running during this preflight.

## Decision

Do not run the remaining Linux gameplay gate unattended on the user's primary
`DISPLAY=:1`.

Reason: the current machine has no isolated headless X server. Running
Q2PRO-X/Wine on the primary display can grab keyboard/mouse focus and has
already been observed as an operator-disrupting failure mode in earlier local
Q2PRO-X gate work.

`Xwayland` is present, but using it for Wine/Q2PRO-X would still attach windows
to the current Wayland session. It is not equivalent to an isolated `Xvfb`
gate.

## Safe next paths

1. Install a headless X server, then run the Linux live gate under that display:

```bash
sudo pacman -S --needed xorg-server-xvfb
```

After that, use a dedicated display such as `:99` for Q2PRO-X/Wine live
validation.

2. Run the live gameplay gate intentionally in the user's GUI session, with the
operator aware that Q2PRO-X may take focus.

## Boundary

This preflight does not mark Linux fresh-package gameplay as passed. It only
prevents a known unsafe unattended GUI run and records the missing local
prerequisite for a clean headless gate.

## Local rc14 install prep

The current user's Linux add-on installation was updated from the exact rc14
Linux tarball without launching Q2PRO-X.

```text
archive:
  linux/dist/q2prox-dz-1.0.0-rc14-linux-x86_64.tar.gz
sha256 sidecar: PASS
installer: PASS
installed VERSION: 1.0.0-rc14
saved Q2PRO-X path:
  ~/Games/Quake2-PlayGround-2026/Q2PRO-X.exe
```

Installed entrypoints:

```text
~/.local/bin/q2dz-launch
~/.local/bin/q2dz-session
~/.local/bin/q2dz-webrtc-gateway
```

Byte-for-byte package payload comparison:

```text
VERSION:            PASS
q2dz-launch:        PASS
q2dz-session:       PASS
q2dz-webrtc-gateway: PASS
```

Non-GUI checks after install:

```text
linux/tools/doctor.sh --portable: PASS
linux/tools/doctor.sh --live: PASS
q2dz-launch --help: PASS
q2dz-session --help: PASS
q2dz-webrtc-gateway --help: PASS
leftover q2dz/Q2PRO-X/Wine/VBoxHeadless processes: none
```

Boundary: this prepares the local Linux installation for a manual or isolated
live gameplay gate. It does not prove map entry, movement/fire, room switching
or close handling.
