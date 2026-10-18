# Q2PRO-X live Custom source verification

Task 1, Step 4 evidence report. Date: 2026-08-29. This report uses the
retained Linux fixture/companion and valid Windows final21 artifacts; no VM
was run while preparing it.

## Candidate

The candidate is Q2PRO-X 1.5 by ly (2026-08-25), with the same executable on
both platforms:

```text
SHA-256: 831f42dc96e3399a8a8737943d6fde752e11f5e5f48ffc5957537c4e66cbf71d
```

The Windows bootstrap manifest (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/windows-final21/bootstrap-files.json`, не публикуется)
records all 10 transferred binaries, including `Q2PRO-X.exe` and the nine
direct/runtime renderer DLLs. The candidate details are also recorded in the
[baseline verification](2026-08-29-q2pro-x-custom-source.md).

## Gate results

| Gate | Result | Retained evidence |
|---|---|---|
| Linux/Wine | PASS | `result.txt` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/linux-final/result.txt`, не публикуется), `events.jsonl` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/linux-final/events.jsonl`, не публикуется) |
| Native Windows final21, unprivileged | PASS | `PASS` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/windows-final21/PASS`, не публикуется), `guest-identity.json` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/windows-final21/guest-identity.json`, не публикуется), `preflight.txt` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/windows-final21/preflight.txt`, не публикуется) |

The exact source URLs were:

```text
Linux:   http://127.0.0.1:43685/servers
Windows: http://127.0.0.1:56053/servers
```

The Linux URL is recorded in `ports.txt`. Windows `events.json` records
`httpPort=56053`; `/servers` is the fixed harness route.

## HTTP, UDP, PID, and screenshots

- Linux recorded exactly three HTTP GETs: `empty` once, `A` once with
  `127.0.0.1:37538`, and `B` once with `127.0.0.1:55768`. The corresponding
  UDP status requests reached `Q2DZ-A` and `Q2DZ-B` with peer source port
  `46184` for both. `q2.pid`, `q2.pid.A`, and `q2.pid.B` are all `19`.
- Windows final21 recorded exactly three HTTP GETs, ordinals 1–3: `empty`,
  `A` with `127.0.0.1:63006`, and `B` with `127.0.0.1:63007`. UDP status
  recorded `Q2DZ-A` twice (a client retry) and `Q2DZ-B` once; every request
  used peer source port `55576`, with no A request after B. The single recorded
  Q2 PID is `5140`; the gate passed with that process and did not restart it.
- The synchronized final21 framebuffer captures show the expected states:
  `empty.png` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/windows-final21/empty.png`, не публикуется)
  has no server row,
  `A.png` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/windows-final21/A.png`, не публикуется)
  has only `Q2DZ-A`/Alice, and
  `B.png` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/windows-final21/B.png`, не публикуется)
  has only `Q2DZ-B`/Bob.

The machine event traces and synchronized captures agree; both are retained
as final21 evidence.

## Profile, isolation, and cleanup

- Both gates used disposable private `q2home`/profile roots, removed during
  cleanup. No persistent profile was mounted or compared.
- The Linux isolation companion records host display `:1`, isolated display
  `:90`, `PASS: no X11 TCP listener on 6090`, an empty TCP-listener record,
  and owned-process `cleanup=complete`:
  `host-display.txt` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/linux-isolation-companion/host-display.txt`, не публикуется),
  `isolated-display.txt` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/linux-isolation-companion/isolated-display.txt`, не публикуется),
  `no-x11-tcp.txt` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/linux-isolation-companion/no-x11-tcp.txt`, не публикуется),
  `guard.ledger` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/linux-isolation-companion/guard.ledger`, не публикуется).
- Windows final21 records a headless guard PASS, and
  `guest-identity.json` records `isAdministrator: false` for
  `WINDOWS-GGFDKO9\user`. The Q2 path is under the user's
  `C:\Users\user\AppData\Local\Temp` tree.
- Windows guest cleanup completed (`cleanup.done` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/windows-final21/cleanup.done`, не публикуется));
  the final VM state is `VMState="saved"` (локальный материал `q2prox-dz-gates/20260829-live-custom-source-safe/windows-final21/vm-after.txt`, не публикуется).

## Chronology and historical attempts

Host-local artifact timestamps establish the valid order:

- Linux fixture: `q2.start` mtime `2026-08-29 21:53:11 +0300`;
- Windows final21: `started` mtime `2026-08-30 01:02:24 +0300`, PASS at
  `01:02:45 +0300`, and saved VM at `01:03:01 +0300`;
- Linux isolation companion: `host-display.txt` and `guard.ledger` mtime
  `2026-08-30 01:06:36 +0300`.

Thus the order was Linux fixture -> Windows final21 -> Linux isolation
companion. Final21 preflight found no concurrent host-user `app-q2dz`/`Q2PRO-X`
process; companion cleanup and the saved VM state complete the isolation
evidence.

Windows final19 remains historical harness/network-only PASS evidence and is
not used for the final Windows decision.

Final20 is excluded entirely. It is invalid solely because the host-user
`app-q2dz`/`Q2PRO-X` process started concurrently; the VM was saved. It was
not touched, and no final20 contents are used in this report.

## Wrapper note and stop rule

The corrected final21 wrapper exited 0 with the guest gate PASS. The earlier
wrapper exit 1 was solely a CRLF post-run check issue; its corrected wrapper
check passed without rerunning that guest gate.

All Step 4 fields are now supported: candidate/version/hash, Linux and
unprivileged Windows results, exact URLs, empty/A/B HTTP counts, ordered UDP
A+/B with one source port, unchanged PID/install, disposable private roots
removed by cleanup, display and no-TCP isolation, owned cleanup,
ordinary-user/headless start, saved VM, and non-concurrency. Task 1 Steps 1–4
are therefore checked in the work plan.
This remains bounded Custom-source evidence and does not claim a live
two-room production or browser gameplay run.
