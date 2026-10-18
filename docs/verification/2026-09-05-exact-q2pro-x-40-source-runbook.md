# Exact Q2PRO-X 40-endpoint Custom source runbook

Purpose: close the exact-client gate that cannot be proven by fake Q2 or by a
non-interactive VM session.

This runbook uses the retained fixture included in the repository and in the
test-group bundle:

```text
tests/tools/controlled-room-set.py source --count 40
```

It does not contact Dos.Zone and does not change the user's Q2PRO-X install.
Use a temporary `homedir`.

## PASS criteria

- Q2PRO-X starts with exact arguments including:
  - `+set game 3zb2`;
  - `+set cl_serverbrowser_default_source custom`;
  - `+set cl_serverbrowser_custom_source http://127.0.0.1:<port>/servers`.
- In Q2PRO-X, `Join online -> Custom -> Refresh` shows the fixture rows.
- The fixture records `status` from Q2PRO-X to all 40 endpoints.
- All `status` packets use one UDP source address.
- For one selected row, the fixture records `status`, `getchallenge`, and
  `connect` from the same UDP source address.
- Q2PRO-X exits; the fixture exits; no Q2PRO-X or fixture process remains.

## Linux

From the repository root or extracted test-group bundle root:

```bash
mkdir -p ../noncommit/q2prox-dz-gates/manual-q2pro-40
python3 tests/tools/controlled-room-set.py source \
  --count 40 \
  --duration 300 \
  --evidence ../noncommit/q2prox-dz-gates/manual-q2pro-40/source-events.jsonl \
  | tee ../noncommit/q2prox-dz-gates/manual-q2pro-40/source.log
```

Copy the `http_url` value from the first JSON line. In another terminal:

```bash
Q2=~/Games/Quake2-PlayGround-2026/Q2PRO-X.exe
ROOT=~/Games/Quake2-PlayGround-2026
EVID=/path/to/DosZone/noncommit/q2prox-dz-gates/manual-q2pro-40
URL='http://127.0.0.1:<port>/servers'

WINEDEBUG=-all wine "$Q2" \
  +set homedir "$EVID/q2home" \
  +set game 3zb2 \
  +set cl_timeout 20 \
  +set s_initsound 0 \
  +set cl_serverbrowser_default_source custom \
  +set cl_serverbrowser_custom_source "$URL"
```

In Q2PRO-X:

1. open `Join online`;
2. select `Custom`;
3. press `Refresh`;
4. select any fixture row and press `Enter`;
5. wait until the connection attempt is visible, then exit Q2PRO-X.

Check the evidence:

```bash
python3 tests/tools/controlled-room-set.py source-check \
  --count 40 \
  --evidence ../noncommit/q2prox-dz-gates/manual-q2pro-40/source-events.jsonl
```

## Windows

Run this only in a logged-in Windows desktop session. GuestControl without a
logged-in user is not valid for this gate.

1. Copy `tests/tools/controlled-room-set.py` from the extracted test-group
   bundle or repository checkout to a temporary directory.
2. Start it in `cmd.exe`:

   ```cmd
   python controlled-room-set.py source --count 40 --duration 300 --evidence source-events.jsonl
   ```

3. Copy the printed `http_url`.
4. Start Q2PRO-X from another `cmd.exe`:

   ```cmd
   Q2PRO-X.exe +set homedir C:\Temp\q2dz-q2home +set game 3zb2 +set cl_timeout 20 +set cl_serverbrowser_default_source custom +set cl_serverbrowser_custom_source http://127.0.0.1:<port>/servers
   ```

5. In Q2PRO-X, open `Join online -> Custom`, press `Refresh`, select one row,
   press `Enter`, then exit.
6. Run:

   ```cmd
   python controlled-room-set.py source-check --count 40 --evidence source-events.jsonl
   ```

## Notes

Do not use `+serverbrowser_open_modern` for this gate. Earlier rc11 checks
showed that forced auto-opening can leave a black screen with some source
states. The product contract is the ordinary user path: start Q2PRO-X, open
`Join online -> Custom`, press `Refresh`.

Do not replace the manual refresh with startup `+serverbrowser_open
+serverbrowser_refresh` or a bound-key script. The 2026-09-05 isolated attempts
did not produce Custom-source HTTP traffic. The release gate is the ordinary
visible menu flow above.
