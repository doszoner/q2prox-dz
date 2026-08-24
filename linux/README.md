# Q2PRO-X — Dos.Zone add-on for Linux

## Быстрый старт

Установите Wine и Python 3, распакуйте release-архив и выполните
`./install.sh`. Затем откройте **Q2PRO-X — Dos.Zone** из меню приложений,
укажите существующий `Q2PRO-X.exe`, выберите комнату и нажмите Join. Подробные
требования и команды приведены ниже.

This package lets an existing Windows x64 `Q2PRO-X.exe` join a currently
running public Quake II room on Dos.Zone through Wine. It does not include
Q2PRO-X, Quake II game data, maps or PAK files, and it does not create rooms.

```text
Q2PRO-X.exe under Wine
  -> UDP 127.0.0.1:<random-session-port>
  -> bundled managed UDP/WebRTC gateway
  -> existing Dos.Zone browser-host room
```

## Requirements

- Linux x86-64 with glibc 2.38 or newer and libstdc++ providing
  `GLIBCXX_3.4.32`;
- Python 3 and Wine;
- native OpenSSL 3, libcap, zlib, Brotli, Zstandard, X11/XCB and Expat runtime
  libraries (`libssl.so.3`, `libcrypto.so.3`, `libcap.so.2`, `libz.so.1`,
  `libbrotli*.so.1`, `libzstd.so.1`, `libX11.so.6`, `libxcb.so.1` and
  `libexpat.so.1`); the installer runs `ldd` and stops before writing files if
  one is missing;
- Zenity for the graphical room picker; a headless/terminal launch uses the
  terminal picker even when Zenity happens to be installed;
- an existing legal Quake II installation with Q2PRO-X 1.4 by ly, build
  2026-07-05, and the map used by the selected room.

Maintainer-observed rc8 evidence records the exact release/package chain,
current-user install, real `q2dm1` join with server-sourced `game = 3zb2`,
client movement/fire input, application-level chat round trip and normal
managed-session cleanup on the Manjaro validation host. The owner browser
registered the connected player. Historical rc6/rc7 results remain preserved
in their own verification records and are not promoted to rc8 by inference.
Exact rc8 package hashes and results are recorded in
`docs/verification/1.0.0-rc8.md`.

If Q2PRO-X is not installed, get the current release from the official landing
page: <https://q2pro-x.com/>. Install or update it separately, start it once
through Wine, then return to this add-on. The add-on never downloads or updates
the game client.

## Install or update for one user

Keep the archive and its adjacent `.sha256` file in the same directory:

```bash
sha256sum -c q2prox-dz-1.0.0-rc8-linux-x86_64.tar.gz.sha256
tar -xzf q2prox-dz-1.0.0-rc8-linux-x86_64.tar.gz
cd q2prox-dz-1.0.0-rc8
./install.sh
```

No administrator privileges, Git checkout, compiler or system service are
required. Re-running `./install.sh` updates only the owned add-on directory.

## Join a room

Open **Q2PRO-X — Dos.Zone** from the desktop application menu, or run:

```bash
q2dz-launch
```

On the first run, select the existing `Q2PRO-X.exe`. The launcher stores only
that path. It then shows current joinable Quake II rooms whose aliases are still
registered, checks that the required map is already installed, obtains fresh
room credentials in memory, starts the local gateway, and launches Q2PRO-X only
after a real Quake II readiness round trip.

After installing or moving Q2PRO-X, select the new executable with:

```bash
q2dz-launch --select-q2
```

The ordinary Q2PRO-X shortcut remains independent. Creating a Dos.Zone room is
still done through the Dos.Zone browser UI.

## Uninstall

```bash
"${XDG_DATA_HOME:-$HOME/.local/share}/q2dz/uninstall.sh"
```

The uninstaller verifies an exact owned-path manifest and refuses to run if an
unknown file exists below the add-on root. The installer and
helper do not edit Q2PRO-X, the Wine prefix, CFG files or PAK files. Q2PRO-X
itself may update its normal `q2config.cfg` autosave when the game runs.

## Security and lifecycle

- the game-facing UDP socket binds only to loopback;
- room JSON enters through stdin and is never passed in arguments, environment
  variables or persistent files;
- gateway and Wine/Q2 children receive an explicit desktop/Wine runtime
  environment allowlist; arbitrary parent variables are not inherited;
- ICE, TURN, SDP and candidate diagnostics are redacted;
- Q2PRO-X starts only after transport readiness;
- a guardian removes the owned gateway/Wine process group and marked runtime
  directory after normal failure or coordinator hard-kill;
- missing content and invalid/stale rooms fail before Q2PRO-X starts.

## Local source development

From `linux/` in the source checkout:

```bash
export Q2DZ_WEBRTC_NET_RELEASE_DIR=/path/to/pinned/webrtc-release
tools/build-release.sh
tools/run-sanitizers.sh
```

The canonical package/release chain and publication gates live in
<https://github.com/doszoner/q2prox-dz/blob/master/docs/RELEASE.md>.

`Q2DZ_WEBRTC_NET_RELEASE_DIR` is the built WebRTC-NET release directory used
for headers and shared libraries.

The authoritative external input is the public WebRTC-NET repository
`https://github.com/doszoner/WebRTC-NET.git` at immutable commit
`1b8c145b8fb00e8a1599ec311118cd38aad44e7a`; no tag is required. The release
directory must contain the matching `humblenet_p2p.h`, `libhumblenet.so` and
`libwebrtc.so`.

Project and third-party license texts are included in the source tree and are
required in the release package.
