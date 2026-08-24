# Windows product packaging

The canonical release command and gates live only in
[`docs/RELEASE.md`](../../docs/RELEASE.md). This directory supplies the
packager for two layouts with byte-identical runtime files:

- portable: extract and run `Run.cmd`;
- setup: extract and run `Install.cmd` for Start Menu integration.

The internal `package-windows.ps1` still rejects missing or unexpected payload
files, writes an internal manifest for each layout and an adjacent SHA-256 file
for each ZIP. It is not a standalone release command.

## Per-user setup

The release builder places release binaries, gateway assets and this launcher
under `payload/`; the packager adds the reviewed files from
`windows/product/licenses/`. Windows RC community artifacts are unsigned and
closed-alpha-only until the Authenticode gate is run. The rc8 native WebView2
cadence correction and exact package gates are recorded in
`docs/verification/1.0.0-rc8.md`.
`install.ps1` verifies the setup manifest, stages a complete version under
`%LOCALAPPDATA%\Q2PRO-X Dos.Zone\versions\`, verifies copied hashes, and atomically
updates `current.json`. The stable `launch.ps1` and `uninstall.ps1` files are the
only targets used by the Start Menu shortcuts.

The user extracts the release and double-clicks `Install.cmd`. It installs the
add-on and immediately opens the first run, which asks for the existing
`Q2PRO-X.exe`. Later launches show current public Dos.Zone Quake II rooms.
`Join` revalidates room, alias and RTC data, checks the map locally, then starts
the launcher-owned managed session. The game, PAKs, CFG and normal Q2PRO-X
shortcut are not modified.

`uninstall.ps1` removes only complete add-on versions carrying a valid
`.q2dz-owned` marker, the stable pointer/scripts, and matching shortcuts. It
preserves `state\q2-path.txt` and unknown files. Use `uninstall.ps1 -ResetState`
only when the saved Q2PRO-X path should also be removed. A legacy
`%LOCALAPPDATA%\Q2PRO-X Dos.Zone\q2-path.txt` is migrated once when no newer
`state\q2-path.txt` exists.

The end-user `README.txt` files included in both archives cover first install,
an absent or moved Q2PRO-X, separate Q2PRO-X/add-on updates, WebView2 setup,
missing maps, stale rooms, unsigned-RC warnings and removal.
