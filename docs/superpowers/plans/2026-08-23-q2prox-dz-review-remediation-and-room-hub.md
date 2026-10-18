# Q2PRO-X Dos.Zone Release Remediation and Room Hub Implementation Plan

> Исторический документ; не инструкция к текущей версии. Перенесён 2026-10-18.
> Отметки задач и результаты относятся к исходной дате; проверки сейчас не повторялись.
> Пути прототипов и локальных evidence могут отсутствовать. Актуальное состояние — в индексе docs/README.md.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close every applicable finding F-01 through F-18 from the 2026-08-23 review, produce reproducible and truthfully documented Windows and Linux single-room releases, prove the exact Q2PRO-X Custom-source contract, and only after those gates pass add the smallest room hub that lets one unchanged Q2PRO-X process switch between live Dos.Zone rooms.

**Architecture:** Preserve both proven transports: WebView2 on Windows and native WebRTC-NET plus Wine on Linux. Preserve the current single-room coordinators as the rollback product. Correct their defects and release chains first. The room hub is conditional: after exact-client feasibility passes, add one small native hub owner per platform, reuse the existing gateway and managed protocol, and use the platform launcher only for catalog/UI/source publication. Do not change Dos.Zone, Q2PRO-X, Quake II protocol, or WebRTC transport.

**Tech Stack:** C++20/CMake/CTest, WebView2, WebRTC-NET, Node's built-in test runner, PowerShell 5.1-compatible scripts and .NET BCL, Python 3 standard library, Bash, Wine, VirtualBox, tcpdump/tshark, Windows pktmon.

---

## 1. Execution contract

This plan is the operational checklist. Execute it continuously after the two required plan reviews below.

- [x] Run `technical-premortem` against this file and the live checkout.
- [x] Run `ponytail-review` against this file.
- [x] Apply all admitted pre-mortem mitigations and all valid simplifications to this file.
- [x] Recheck that every review finding F-01 through F-18 has exactly one owning task and one proving gate.
- [x] Start `superpowers:subagent-driven-development` immediately; do not pause for approval between tasks.
- [ ] For every implementation task: fresh implementer, spec-compliance review, then code-quality review; close all Important/Critical findings before proceeding.
- [ ] Use test-first RED → GREEN for every behavior change. Configuration-only and documentation-only edits use the smallest direct validation.
- [ ] Do not commit, push, merge, publish, or delete user material without a separate explicit instruction. The user's `OK` authorizes implementation and verification, not Git history changes.

### Required plan-review result

The pre-implementation reviews were run against the live checkout.

- `technical-premortem`: **REVISE**. Admitted mitigations are incorporated below: exact test seams for F-01/F-06/F-08/F-09/F-10, correct paths/commands, a 256 KiB downstream-aligned hub config limit, explicit launcher/owner topology, and platform-specific live evidence.
- `ponytail-review`: valid reductions are incorporated below: one root version file, reuse of existing build/repro/package entrypoints, one real-payload smoke entrypoint, no duplicate support-contract document, no new documentation-link tool, and no implementation of losing Custom-source formats.
- The suggested removal of per-task spec/code reviews was rejected because the user explicitly required `superpowers:subagent-driven-development` and that workflow requires both reviews.
- The suggested removal of source-format comparison was rejected because the source review explicitly requires text HTTP, binary HTTP, and `file://` feasibility comparison. Only the winning format is implemented.

### Working-copy rule

Work in the canonical checkout:

```text
/path/to/DosZone/native-quake2-client-doszone
```

The user has explicitly rejected hidden/global worktrees for this project. Do not create `.worktrees`, `worktrees`, or `~/.config/superpowers/worktrees`. Preserve the existing uncommitted review/input files and stage nothing during implementation.

### Stop rules

The task must keep advancing, but it must not manufacture PASS results.

1. A failed automated gate is fixed narrowly and rerun; it is not waived.
2. A failed exact Q2PRO-X Custom-source or UDP-source gate stops **room-hub production code only**. Continue and finish the single-room Windows/Linux releases.
3. A temporarily unavailable external room is recorded as `BLOCKED_EXTERNAL`, then other independent tasks continue. Retry the live gate later in this execution.
4. Do not weaken loopback source isolation, persist fresh credentials, synthesize fake production server status, or replace the proven transport to make a gate pass.
5. A fake-child test proves lifecycle only. It never substitutes for a live Dos.Zone gate.

## 2. Product completion definition

The project is ready for a small community test only when all applicable rows pass from newly built artifacts.

| Surface | Required proof |
|---|---|
| Windows portable | clean build → real payload → ZIP → fresh extract → live Dos.Zone join on Windows |
| Windows setup | transactional install/update/rollback/uninstall tests plus live Dos.Zone join from installed payload |
| Linux package | clean source/dependency verification → package → fresh extract/install → live Dos.Zone join locally |
| Both platforms | exact Q2PRO-X located or a clear install/update instruction; no Q2/CFG/PAK modification by the add-on |
| Both platforms | live map entry, owner-observed movement and fire, room close, normal Q2 exit, hard owner kill, no orphan processes, no credential persistence |
| Room hub, if feasible | Custom tab shows real room status; Room A → Disconnect → Room B in one unchanged Q2PRO-X process on Linux and Windows |
| Public repository | curated product source/docs only, locked dependencies, reproducible release command, no reviewer/input/evidence secrets |

Authenticode, MSI, auto-update, a new GUI framework, and Dos.Zone changes do not block the first closed/community ZIP release. The Windows setup package remains PowerShell-based. Add signing only when a publisher certificate exists.

## 3. Fixed facts and versioning

Use these facts as inputs and verify them again before writing release records:

```text
repository: /path/to/DosZone/native-quake2-client-doszone
current branch: master
current HEAD at planning: 214d880
remote: git@github.com:doszoner/q2prox-dz.git
Git identity policy: doszoner <doszoner@users.noreply.github.com>
next release candidate: 1.0.0-rc6

Linux exact Q2PRO-X:
  ~/Games/Quake2-PlayGround-2026/Q2PRO-X.exe
  SHA-256 4ee1cf3389a5ef9966c82d324a68a380ae9b601beb55fe93864eb63ea218501b
  version string: 1.4 by ly, 2026-07-05
  Wine: wine-11.15

WebRTC-NET:
  /path/to/DosZone/WebRTC-NET
  commit/tag: 1b8c145b8fb00e8a1599ec311118cd38aad44e7a / v1.1.0

Windows clean VM:
  Windows 11 25H2 + LTSC (x64) 28in1
  snapshot: clean-windows-25h2-pre-product-20260822
```

Canonical artifact names:

```text
q2prox-dz-1.0.0-rc6-windows-x64-portable.zip
q2prox-dz-1.0.0-rc6-windows-x64-setup.zip
q2prox-dz-1.0.0-rc6-linux-x86_64.tar.gz
```

## 4. Finding coverage

| Finding | Owner task | Release gate |
|---|---|---|
| F-01 WebView2 double-free | Task 2 | W-CORRECTNESS |
| F-02 post-ready send failure never closes | Task 3 | W-CORRECTNESS |
| F-03 coordinators cannot form a hub | Tasks 18-20 | HUB-OWNER |
| F-04 no exact Windows source-to-ZIP chain | Task 8 | W-PACKAGE |
| F-05 unsafe Windows update/uninstall | Task 9 | W-INSTALL |
| F-06 weak Windows catalog validation | Task 7 | W-CATALOG |
| F-07 terminal child event lost on exit | Task 4 | W-CORRECTNESS |
| F-08 unbounded child event buffer | Task 5 | W-CORRECTNESS |
| F-09 joinable thread can terminate owner | Task 6 | W-CORRECTNESS |
| F-10 Gate P rejects legitimate Q2 autosave | Task 10 | W-VM |
| F-11 Linux UDP port TOCTOU | Task 11 | L-PORT |
| F-12 Linux source install depends on stale cache | Task 12 | L-INSTALL |
| F-13 implicit support contract | Task 13 | CONTRACT |
| F-14 unproven 1400-byte profile ceiling | Task 14 | TRANSPORT-PROFILE |
| F-15 docs overclaim evidence | Tasks 1, 15, 23 | DOC-TRUTH |
| F-16 unlocked dependencies | Task 1 | REPRODUCIBILITY |
| F-17 unchecked WebView2 handler registration | Task 2 | W-CORRECTNESS |
| F-18 public/reviewer boundary and signed wording | Tasks 1, 15, 23 | PUBLIC-REPO |

## 5. Evidence rules

Raw runtime evidence is not source code. Store it outside Git:

```text
/path/to/DosZone/noncommit/q2prox-dz-gates/<UTC-run-id>/
```

Each run contains `manifest.json`, per-gate logs, hashes and screenshots/pcaps. Do not store room JSON, ICE/TURN credentials, cookies, join URLs, SDP, candidates, or unredacted command lines. Repository records contain only hashes, sanitized summaries, exact commands, environment identity and verdicts.

The final sanitized record is:

```text
docs/verification/1.0.0-rc6.md
```

## Task 1: Freeze baseline, release identity and dependency lock

**Status:** local implementation/spec/quality review PASS. External release gate remains open: the local checkout is exactly `v1.1.0` at the locked commit, but unauthenticated GitHub refs do not currently publish `refs/tags/v1.1.0`. No external push is authorized.

**Files:**

- Create: `VERSION`
- Create: `deps.lock.json`
- Create: `tests/contracts/doszone-api-cases.json`
- Create: `tools/verify-dependencies.py`
- Create: `tests/test_verify_dependencies.py`
- Modify: `linux/CMakeLists.txt`
- Modify: `linux/tools/build-package.sh`
- Modify: `linux/tests/package_smoke.sh`
- Delete: `linux/VERSION`
- Modify: `windows/coordinator/CMakeLists.txt`
- Modify: `windows/product/package-windows.ps1`
- Modify: `windows/product/tests/package-windows.tests.ps1`

- [x] Record the current status without changing it:

```bash
cd /path/to/DosZone/native-quake2-client-doszone
git status --short --branch
git diff --check
git rev-parse HEAD
git config user.name
git config user.email
```

Expected: HEAD `214d880` at plan start; identity is `doszoner`; existing `docs/`, `docs/adr/`, `docs/reviews/`, and `input/` changes remain visible and untouched.

- [x] Run the current portable test baseline and save stdout/stderr outside the repo:

```bash
run_root=/path/to/DosZone/noncommit/q2prox-dz-gates/$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$run_root/baseline"
cmake -S linux -B "$run_root/baseline/linux-build" -DBUILD_TESTING=ON -DQ2DZ_REQUIRE_WEBRTC_GATEWAY=OFF
cmake --build "$run_root/baseline/linux-build" -j2
ctest --test-dir "$run_root/baseline/linux-build" --output-on-failure | tee "$run_root/baseline/linux-ctest.txt"
node --test windows/coordinator/web/managed-readiness-probe.test.mjs | tee "$run_root/baseline/readiness-node.txt"
node --test windows/gateway/tests/transport_protocol.test.mjs | tee "$run_root/baseline/transport-node.txt"
```

Expected: baseline results are recorded exactly; a failure is preserved rather than relabeled.

- [x] Add root `VERSION` containing exactly:

```text
1.0.0-rc6
```

- [x] Delete `linux/VERSION`. Make artifact/package scripts and tests read root `VERSION`. Remove component `project(... VERSION ...)` values from both CMake projects because they are not separately released products. Make Linux install root `VERSION` via `${CMAKE_CURRENT_SOURCE_DIR}/../VERSION`. Do not add a version library.

- [x] Add a compact `deps.lock.json` schema consumed by both platforms:

```json
{
  "schema": 1,
  "webrtcNet": {
    "source": "https://github.com/doszoner/WebRTC-NET.git",
    "tag": "v1.1.0",
    "commit": "1b8c145b8fb00e8a1599ec311118cd38aad44e7a",
    "submodules": {
      "3rdparty/boringssl": "b34976cae99f8d1b864dbab31e20fc00d06acb09",
      "3rdparty/flatbuffers": "fb9afbafc7dfe226b9db54d4923bfb8839635274",
      "3rdparty/libwebsockets": "5ff9503f995fa692980d666de3984e000d610166",
      "3rdparty/webrtc-native-build": "d146ad1b017245cf1331a647c887259cbc6b8607"
    },
    "licenseSources": ["LICENSE.md", "NOTICES.md", "3rdparty/webrtc-native-build/LICENSE", "3rdparty/webrtc/license.txt"],
    "linux": {
      "humblenet_p2p.h": "cedd19cc48e7503f6e0f03f16d13686670b6dbdf5c78670c923acde77f224bb2",
      "libhumblenet.so": "60215739b82c61865717622f291e6d4d77e93e4e23122c008cdd0aae2e053b02",
      "libwebrtc.so": "ac3548454e189300c29ef9c9d1f5f8ea726438dc2cfa83a2c065a452c3ca8745"
    },
    "windows": {
      "webrtcnet.mjs": "c4c3eccc9753563e2a77d5b8e121568561fa1011164542c6662356db58b1b2a1",
      "webrtcnet.wasm": "0374c3c43d24e013ae0ef874eccbd17b0463d78ad8dbc385cb9853129c269118"
    }
  },
  "webView2": {
    "package": "Microsoft.Web.WebView2",
    "version": "1.0.4129.50",
    "source": "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4129.50/microsoft.web.webview2.1.0.4129.50.nupkg",
    "packageSha256": "d3934f482d484b89fb4825df720c710664e1143a1e90f7b3a60794ef33f473d2",
    "loaderX64Sha256": "a9a09232c25805323d4cfb3fc8f545a190a9c8a99c93262ea99d0b88df99ec90",
    "licenseSources": ["windows/product/licenses/WebView2-LICENSE.txt", "windows/product/licenses/WebView2-NOTICE.txt"]
  }
}
```

- [x] Before accepting those hashes, independently run `sha256sum`/`Get-FileHash` on every exact input. A mismatch is a lock correction task, not a waived gate.
- [ ] Verify the adjacent WebRTC-NET checkout with `git rev-parse HEAD` and `git describe --tags --exact-match`; verify unauthenticated availability with `git ls-remote https://github.com/doszoner/WebRTC-NET.git 'refs/tags/v1.1.0' 'refs/tags/v1.1.0^{}'`. Local checkout: PASS. Unauthenticated tag ref: FAIL/open external gate. The lock verifier checks the local source commit/tag when `--webrtc-source` is supplied.

- [x] Write `tools/verify-dependencies.py` with Python standard library only. It accepts explicit Linux or Windows dependency roots, rejects missing/unlisted files and mismatched hashes, and prints no file contents.

- [x] RED: add subprocess tests in `tests/test_verify_dependencies.py` for one valid tree, one missing file and one hash mismatch; run `python3 -m unittest tests/test_verify_dependencies.py -v` and observe missing verifier behavior.

- [x] GREEN: implement the smallest JSON/hash loop and rerun the tests.

- [x] Add one shared catalog-contract JSON file now, containing named valid/invalid cases rather than many fixture files. Both platform catalog tests will consume it in Task 7.

- [x] Rename artifact construction in `linux/tools/build-package.sh`, `linux/tests/package_smoke.sh`, `windows/product/package-windows.ps1`, and `windows/product/tests/package-windows.tests.ps1` to the three canonical `q2prox-dz-...` names declared above. Keep `dist/` ignored as it already is; do not edit `.gitignore` without a newly demonstrated missing pattern.

- [x] Verification:

```bash
python3 tools/verify-dependencies.py --help
python3 -m py_compile tools/verify-dependencies.py
git diff --check
```

Expected: verifier help exits 0, Python compilation exits 0, no whitespace errors.

## Task 2: Fix Windows WebView2 ownership and registration (F-01, F-17)

**Status:** implementation/spec/quality review PASS. Exact MSVC 19.44/WebView2 build and CTest PASS on the Windows dev VM; sanitized logs are outside Git under `noncommit/q2prox-dz-windows-share/task02-20260823T055152Z/`.

**Files:**

- Modify: `windows/gateway/src/main.cpp`
- Create: `windows/gateway/src/web_message_dispatch.hpp`
- Modify: `windows/gateway/tests/webview_bridge_protocol_test.cpp`
- Modify: `windows/gateway/CMakeLists.txt`

- [x] Inspect every caller in `Gateway::configure_webview()` before editing. Keep all fixes in the shared callback/registration path.

- [x] RED: add `with_owned_web_message(wchar_t*, Deleter, Callback)` as the wished-for API in the existing bridge test. The callback throws and the fake deleter must run exactly once. Compile the test and observe that the helper is missing.

- [x] GREEN: own WebView2's `LPWSTR` exactly once with the standard library:

```cpp
struct CoTaskMemDeleter {
    void operator()(wchar_t* value) const noexcept { CoTaskMemFree(value); }
};
using CoTaskMemString = std::unique_ptr<wchar_t, CoTaskMemDeleter>;
```

`web_message_dispatch.hpp` contains only that template and `<memory>`. Production calls it immediately after `args->get_WebMessageAsJson(&raw)`, converts/dispatches inside the callback, and never manually frees `raw` in `catch`.

- [x] Wrap each `add_NavigationStarting`, `add_NewWindowRequested`, `add_PermissionRequested`, `add_ProcessFailed`, and `add_WebMessageReceived` call with existing `require_hr()`.

- [x] Use distinct `EventRegistrationToken` variables. Store tokens only if the existing shutdown path removes handlers; do not invent a registration manager.

- [x] Run on Windows build VM (portable GCC/Clang regression checks also PASS):

```powershell
cmake -S .\windows\gateway -B .\build\gateway-raii -A x64 `
  -DBUILD_TESTING=ON `
  -DWEBVIEW2_SDK_DIR=C:\deps\webview2-sdk-1.0.4129.50 `
  -DWEBRTC_NET_RELEASE_DIR=C:\deps\webrtc-net-v1.1.0
cmake --build .\build\gateway-raii --config Release
ctest --test-dir .\build\gateway-raii -C Release --output-on-failure
```

Expected: bridge regression test passes; all handler registration failures are fail-closed; no new dependency.

## Task 3: Make Windows room-close probing bounded (F-02)

**Status:** implementation/spec/quality review PASS; readiness tests `7/7` and transport protocol tests `4/4` PASS.

**Files:**

- Modify: `windows/coordinator/web/managed-readiness-probe.mjs`
- Modify: `windows/coordinator/web/managed-readiness-probe.test.mjs`
- Modify: `windows/gateway/assets/transport.html`

- [x] RED: add a test where readiness ACK succeeds and every later `send()` returns `false`. Assert exactly one `onRoomClosed` after `maxMisses`.
- [x] RED: add the same case for `send()` throwing.
- [x] Verify both new tests fail for the reviewed reason, while pre-ready retry still passes.
- [x] GREEN: before readiness, preserve retry semantics. After readiness, treat `false` or exception as one missed bounded probe; reset misses only after a successful probe/ACK; emit room closed once at the existing maximum.
- [x] Treat the bounded post-ready probe as the normative close path. Inspect the exact WebRTC-NET exports used by `transport.html`; connect a direct close/error event only if that exact runtime already exposes a tested signal. Do not invent a DataChannel object or change WebRTC-NET merely to add a second close mechanism. Keep gameplay inactivity non-fatal. No exact direct signal exists, so `transport.html` remains unchanged.
- [x] Verification:

```bash
node --test windows/coordinator/web/managed-readiness-probe.test.mjs
node --test windows/gateway/tests/transport_protocol.test.mjs
```

Expected: all existing plus post-ready-false and post-ready-throw cases pass; no arbitrary idle timeout appears.

## Task 4: Preserve terminal gateway events across process exit (F-07)

**Status:** implementation/spec/quality review PASS. Fresh Windows MSVC build, CTest `2/2`, and full lifecycle matrix PASS; sanitized logs are outside Git under `noncommit/q2prox-dz-windows-share/task04-20260823T061345Z/`.

**Files:**

- Modify: `windows/coordinator/src/windows/main.cpp`
- Modify: `windows/coordinator/tests/windows/fake_gateway.cpp`
- Modify: `windows/coordinator/tools/run-lifecycle-matrix.ps1`

- [x] RED: add fake modes that write a complete `ROOM_CLOSED` or `FATAL` line, flush it, and exit immediately.
- [x] RED: matrix cases require exit 10 for immediate room close and exit 13 for immediate fatal; observe current generic gateway-exit classification.
- [x] GREEN: when the gateway process handle signals, close the coordinator's writer/owner side as appropriate, wait for reader EOF, drain already framed events, and prioritize a valid terminal event. Only then fall back to generic child-exit classification.
- [x] Do not use sleeps as synchronization and do not change stable exit codes.
- [x] Verification:

```powershell
pwsh .\windows\coordinator\tools\build-release.ps1
pwsh .\windows\coordinator\tools\run-lifecycle-matrix.ps1
```

Expected output includes `PASS room-close-immediate`, `PASS fatal-immediate`, and `LIFECYCLE MATRIX PASS`.

## Task 5: Bound the Windows child event stream (F-08)

**Status:** implementation/spec/quality review PASS. Fresh Windows MSVC 19.44 Release build, CTest `2/2`, and full lifecycle matrix PASS, including deterministic `no-newline-flood` and `line-flood`; sanitized logs are outside Git under `noncommit/q2prox-dz-windows-share/task05-20260823T065214Z/`.

**Files:**

- Modify: `windows/coordinator/src/windows/main.cpp`
- Modify: `windows/coordinator/src/child_protocol.cpp`
- Modify: `windows/coordinator/include/q2dz/child_protocol.hpp`
- Modify: `windows/coordinator/tests/core_tests.cpp`
- Modify: `windows/coordinator/tests/windows/fake_gateway.cpp`
- Modify: `windows/coordinator/tools/run-lifecycle-matrix.ps1`

- [x] RED: test 100 KiB without newline, an oversized newline-terminated line, multiple legal lines, and queue count/byte exhaustion.
- [x] Observe bounded tests fail or demonstrate current unbounded growth.
- [x] GREEN: add one small `ChildEventFramer` in the existing child-protocol header/source so `reader()` and `core_tests.cpp` exercise the same code. Bound pending bytes before substring/copy and bound queued line count and bytes. Reuse the protocol's existing 4096-byte line ceiling; do not add a generic stream framework.
- [x] On the first violation, enqueue one sanitized `FATAL stage=protocol code=line_too_long` or `queue_overflow`, stop reading, and never copy the oversized payload into the queue.
- [x] Run:

```powershell
pwsh .\windows\coordinator\tools\build-release.ps1
pwsh .\windows\coordinator\tools\run-lifecycle-matrix.ps1
```

The matrix must include and pass `no-newline-flood` and `line-flood`.

Expected: bounded cases exit through protocol failure, memory remains bounded, ordinary events still parse.

## Task 6: Make Windows coordinator cleanup exception-safe (F-09)

**Status:** implementation/spec/quality review PASS. Fresh Windows MSVC 19.44 Release build, CTest `2/2`, and full lifecycle matrix PASS after `CancelSynchronousIo` ordering review; the real post-readiness Q2 `CreateProcessW` failure returned `2`, launched no Q2, and left no owned process/session data. Sanitized logs are outside Git under `noncommit/q2prox-dz-windows-share/task06b-20260823T071203Z/`.

**Files:**

- Modify: `windows/coordinator/src/windows/main.cpp`
- Modify: `windows/coordinator/tests/windows/fake_gateway.cpp`
- Modify: `windows/coordinator/tools/run-lifecycle-matrix.ps1`

- [x] RED: in the matrix, copy fake Q2 into the case directory and set `Q2DZ_TEST_DELETE_Q2_AFTER_READY` to that path. The fake gateway alone reads this test variable, writes/flushed `TRANSPORT_READY`, deletes that copy, and exits only through its normal owner wait. Assert coordinator failure and no surviving gateway/session directory. This reaches `CreateProcessW` after the reader thread exists without adding a production runtime flag.
- [x] Add the narrowest available Job assignment/resume failure cases only if they can be triggered without a fake WinAPI framework. No narrower trustworthy trigger exists without a fake WinAPI layer, so no speculative fault-injection framework was added.
- [x] GREEN: replace the raw joinable reader thread with C++20 `std::jthread` and one no-throw cleanup sequence: signal/close owner pipe, terminate remaining Job members, cancel a pending synchronous reader operation, join, then close the readable pipe.
- [x] Route normal completion and post-reader exceptions through that cleanup sequence. Do not add a process abstraction layer.
- [x] Rerun the full Windows lifecycle matrix and check process trees after every case.

Expected: no `std::terminate`, stable diagnostic exit, no orphan gateway/WebView2/Q2, session directory removed or classified as cleanup failure.

## Task 7: Give both catalog clients one strict contract (F-06)

**Files:**

- Create: `windows/product/catalog.ps1`
- Modify: `windows/product/q2dz-launch.ps1`
- Create: `windows/product/tests/catalog.tests.ps1`
- Modify: `linux/tools/q2dz-launch`
- Modify: `linux/tests/test_launcher.py`
- Use: `tests/contracts/doszone-api-cases.json`

- [x] RED on Windows: consume the shared cases and demonstrate rejection gaps for redirects, oversized bodies, malformed JSON, booleans used as integers, invalid ranges, alias/domain mismatch and malformed ICE.
- [x] GREEN: move only bounded fetch and schema functions into `windows/product/catalog.ps1`; the launcher dot-sources it and the test invokes the same functions against a disposable localhost `HttpListener`. Use `System.Net.Http.HttpClient` only:
  - exact HTTPS endpoint allowlist;
  - `AllowAutoRedirect = $false`;
  - 15-second overall timeout;
  - bounded streaming read with a 2 MiB decompressed ceiling;
  - successful status only;
  - UTF-8 JSON;
  - strict room/alias/RTC/ICE types, lengths and ranges;
  - full selected-room revalidation immediately before join.
- [x] Do not create a class/client framework. `catalog.ps1` is one function file needed because dot-sourcing the current top-level interactive launcher would execute it during tests.
- [x] Make Linux consume the same cases and preserve its existing stricter behavior.
- [x] Run:

```powershell
pwsh -NoProfile -File .\windows\product\tests\catalog.tests.ps1
```

```bash
python3 -m unittest discover -s linux/tests -p test_launcher.py -v
```

Expected: identical accept/reject verdict for every named shared case.

## Task 8: Build exact Windows source-to-ZIP artifacts (F-04, F-16)

**Files:**

- Create: `windows/product/build-payload.ps1`
- Create: `windows/product/tests/release-smoke.tests.ps1`
- Modify: `windows/product/package-windows.ps1`
- Modify: `windows/product/tests/package-windows.tests.ps1`
- Modify: `windows/coordinator/tools/check-reproducible.ps1`
- Modify: `windows/README.md`
- Modify: `docs/RELEASE.md`

- [x] RED: real-payload smoke must reject fixture text named `.exe`, a payload with one altered byte, missing WebView2/WebRTC input, and mismatched version.
- [x] Implement one release command:

```powershell
pwsh .\windows\product\build-payload.ps1 `
  -WebView2Package C:\deps\Microsoft.Web.WebView2.1.0.4129.50.nupkg `
  -WebRtcNetSourceDir C:\deps\WebRTC-NET `
  -WebRtcNetReleaseDir C:\deps\webrtc-net-v1.1.0 `
  -OutputDir .\dist
```

- [x] The command must, in order:
  1. verify `deps.lock.json` against exact inputs;
  2. verify the NuGet package hash, extract it to a temporary directory, and use its exact SDK/loader rather than trusting a caller-prepared SDK directory;
  3. call the existing coordinator build/repro scripts and build the gateway from new temporary directories; extend those scripts only where they currently omit the gateway;
  4. run both CTest suites and both Node suites;
  5. assemble a temporary allowlisted payload only from produced binaries and repository assets, including `catalog.ps1`;
  6. remove the nonexistent/redundant `licenses/third-party-notices.md` allowlist entry and use `windows/product/licenses/`;
  7. run full-candidate two-build `/Brepro` comparison including gateway;
  8. invoke existing `package-windows.ps1` internally instead of duplicating its allowlist/archive logic;
  9. invoke `release-smoke.tests.ps1` as the single fresh-extract/manifest/real-EXE smoke entrypoint for both ZIPs;
  10. leave only the two named ZIPs and adjacent `.sha256` files in `OutputDir`.
- [x] Remove the manual external `PayloadDir` workflow from release documentation. It may remain an internal packager parameter only if `build-payload.ps1` is its sole documented caller.
- [x] Verification: run the one command twice from clean source copies and compare archive content manifests. ZIP container bytes need not match if the archive tool timestamps cannot be normalized; every contained release file must match and `/Brepro` binaries must match.

Expected: package gate proves actual PE files from source, not text fixtures.

## Task 9: Make Windows setup transactional and uninstall owned-only (F-05)

**Files:**

- Modify: `windows/product/install.ps1`
- Modify: `windows/product/uninstall.ps1`
- Create: `windows/product/launch-installed.ps1`
- Create: `windows/product/tests/install-windows.tests.ps1`
- Modify: `windows/product/q2dz-launch.ps1`
- Modify: `windows/product/package-windows.ps1`
- Modify: `windows/product/README.md`

- [x] RED with temporary `LOCALAPPDATA`/`APPDATA`: interrupted update must preserve old current version; normal uninstall must preserve `state/q2-path.txt` and an unknown sentinel; a reparse point must not be followed.
- [x] Use the native filesystem, no installer framework. Layout:

```text
%LOCALAPPDATA%\Q2PRO-X Dos.Zone\
├── current.json
├── versions\<version>-<payload-digest>\
│   ├── .q2dz-owned
│   └── <payload>
├── state\q2-path.txt
├── launch.ps1
└── uninstall.ps1
```

- [x] Install algorithm: verify setup manifest; copy to a unique same-volume staging directory; verify copied hashes; write `.q2dz-owned`; atomically replace `current.json`; retain the prior complete version for rollback; shortcuts target stable `launch.ps1`.
- [x] Default uninstall removes only exact shortcuts, stable scripts/pointer and directories with a valid `.q2dz-owned` marker. It preserves `state` and unknown files and refuses reparse points.
- [x] Add explicit `-ResetState`; never imply it in ordinary uninstall.
- [x] Migrate a legacy root `q2-path.txt` once to `state/q2-path.txt` without deleting an already valid newer state file.
- [x] Run setup tests plus install/update/uninstall in a disposable Windows profile:

```powershell
pwsh -NoProfile -File .\windows\product\tests\install-windows.tests.ps1
```

Expected: mixed-version trees cannot become active; rollback is pointer replacement; ordinary uninstall honors documented ownership.

## Task 10: Correct clean-Windows Gate P (F-10)

**Files:**

- Modify: `windows/coordinator/tools/gate-p.ps1`
- Create: `windows/coordinator/tools/gate-p-config-policy.ps1`
- Create: `windows/coordinator/tools/gate-p.tests.ps1`

- [x] RED: fixtures show that ordinary `q2config.cfg`/Q2PRO-X autosave is rejected by current policy.
- [x] GREEN: put one pure CFG-diff classifier in `gate-p-config-policy.ps1`; both the real Gate P and its fixtures call it. Allow normal Q2-owned autosave. Reject add-on modification of Favorites, Custom source outside the explicit hub lifecycle, Q2 binaries/PAKs, credential-shaped content, or add-on-owned material left inside the Q2 installation.
- [x] Preserve process, session-root and credential-persistence checks.
- [x] Emit separate `cfg-allowed-diff.txt` and `cfg-forbidden-diff.txt`.
- [x] Run the pure policy test:

```powershell
pwsh -NoProfile -File .\windows\coordinator\tools\gate-p.tests.ps1
```

- [x] Then restore the clean VM snapshot and execute the integration gate with fresh room data kept only in memory:

```powershell
Get-Content -LiteralPath C:\Users\Public\q2dz-evidence\fresh-room.json -Raw |
  pwsh -NoProfile -File .\windows\coordinator\tools\gate-p.ps1 `
    -Coordinator C:\Users\Public\q2dz-rc6\q2dz-session.exe `
    -Gateway C:\Users\Public\q2dz-rc6\gateway\q2-doszone-webview2-gateway.exe `
    -Q2 $q2Path `
    -EvidenceDir C:\Users\Public\q2dz-evidence\gate-p
```

Delete `fresh-room.json` after Gate P and scan the evidence/session roots without printing matched secret values.

Expected: legitimate client autosave is recorded but not a failure; add-on mutation remains fail-closed.

Evidence (2026-08-23): current Windows PowerShell 5.1 fixtures passed with exit 0; the clean-VM live Dos.Zone run entered `q2dm1`, moved, fired, exited normally and left empty forbidden/process/session reports. Runtime evidence remains outside Git under `noncommit/q2dz-vm-share/uploads/task10-rc6-live-20260823T144800/returned/`.

## Task 11: Close Linux internal-port TOCTOU with bounded retry (F-11)

**Files:**

- Modify: `linux/src/managed_gateway_main.cpp`
- Modify: `linux/tests/fake_legacy.py`
- Modify: `linux/tests/fake_lifecycle.sh`

- [x] RED: fake legacy mode `early-exit-once` must require two attempts and eventually reach ready; `early-exit-always` must attempt exactly three times, never launch Q2, and return existing gateway-failure status; no-ACK must remain one attempt.
- [x] GREEN: keep the temporary reservation socket open until immediately before spawning the known legacy child. Because the current adapter maps bind and other pre-ready startup failures to the same exit code, retry any child exit before readiness, reap it, reserve a fresh port and retry at most three times. Preserve the child exit code in sanitized diagnostics. Do not claim bind-only classification without changing the locked adapter protocol.
- [x] Do not retry readiness ACK timeout, do not add FD-passing changes to the locked legacy adapter, do not add backoff/config knobs, and do not relax UDP source validation.
- [x] Remove `SO_REUSEADDR` from the reservation socket unless a current caller proves it is required.
- [x] Keep the public UDP socket and sealed ICE `memfd` alive across attempts.
- [x] Run the focused lifecycle script, then the full Linux CTest matrix:

```bash
port_build=$(mktemp -d -t q2dz-port-test.XXXXXX)
BUILD_DIR="$port_build" linux/tools/run-fake-lifecycle.sh
ctest --test-dir "$port_build" --output-on-failure
```

Expected: bind collision is bounded and recoverable; all other lifecycle semantics remain unchanged.

Evidence (2026-08-23): focused lifecycle, full CTest 3/3 and sanitizer builds passed. Tests prove 1/2/3 gateway attempts, exact Q2 start counts, and bounded cleanup of a no-ACK child that ignores SIGTERM.

## Task 12: Make Linux clean source install and repeats deterministic (F-12)

**Files:**

- Modify: `linux/tools/install-user.sh`
- Modify: `linux/tools/check-reproducible.sh`
- Modify: `linux/tools/build-package.sh`
- Modify: `linux/tools/run-fake-lifecycle.sh`
- Modify: `linux/tests/package_smoke.sh`
- Use: `tools/verify-dependencies.py`

- [x] RED: run `install-user.sh` in a fresh source copy with no CMake cache and without `Q2DZ_WEBRTC_NET_RELEASE_DIR`; require an immediate clear failure before configure.
- [x] GREEN: require, canonicalize and dependency-lock-verify `Q2DZ_WEBRTC_NET_RELEASE_DIR`, pass it explicitly as `-DQ2DZ_WEBRTC_NET_RELEASE_DIR=...`, and use a fresh build directory.
- [x] Remove unused `DATA_HOME` and any dependency on prior CMake state.
- [x] Export one `SOURCE_DATE_EPOCH` in `build-package.sh`; normalize copied source mtimes before reproducibility builds.
- [x] Make fake lifecycle repeat 20 times by default, keep per-run logs, stop at the first failure and propagate its status.
- [x] Read root `VERSION` in package smoke; remove hard-coded rc5.
- [x] Verification:

```bash
tmp_install=$(mktemp -d -t q2dz-install-test.XXXXXX)
HOME="$tmp_install/home" \
XDG_DATA_HOME="$tmp_install/home/.local/share" \
XDG_CONFIG_HOME="$tmp_install/home/.config" \
BUILD_DIR="$tmp_install/build" \
Q2DZ_WEBRTC_NET_RELEASE_DIR=/path/to/DosZone/noncommit/q2prox-dz-pre-cleanup-20260823/runs/20260821T162440Z/release \
  linux/tools/install-user.sh
linux/tools/run-fake-lifecycle.sh
linux/tools/check-reproducible.sh
```

Expected: clean install gets exact dependency path, 20/20 lifecycle runs pass, repeat builds have identical release file hashes.

Evidence (2026-08-23): clean install RED/GREEN, lifecycle 20/20, reproducible staged hashes and full package smoke passed. The package boundary verifies both the locked runtime and exact source/license bytes; stale symlinked build directories and mismatched inputs fail before CMake.

## Task 13: State the narrow supported product contract (F-13)

**Files:**

- Modify: `docs/ARCHITECTURE.md` (normative Supported Product Contract section)
- Modify: `README.md`
- Modify: `windows/product/q2dz-launch.ps1`
- Modify: `linux/tools/q2dz-launch`
- Modify: `windows/product/tests/catalog.tests.ps1`
- Modify: `linux/tests/test_launcher.py`

- [x] Write the supported input contract, not a generic promise:

```text
client: exact Q2PRO-X compatible with 1.4 by ly (2026-07-05)
Dos.Zone room prefix: q2
room alias: q2.<owner>, derived from catalog owner and matched exactly in fresh lookup
game: 3zb2
transport: ordered=false
maps/content: selected game directory with baseq2 fallback
```

- [x] Launchers reject unsupported game/order/profile before obtaining or passing credentials.
- [x] RED/GREEN contract cases exist in the shared catalog fixture.
- [x] Document Q2PRO-X not found/outdated flows: official project/download location verified at implementation time, install beside existing Quake II content, select the EXE on first launch, and update by replacing Q2PRO-X files without replacing PAKs/config unless upstream requires it.
- [x] Do not claim support for arbitrary Quake II forks or Dos.Zone games.

Evidence (2026-08-23): Linux launcher tests 27/27 and Windows catalog tests 21 shared cases PASS. Explicit unsupported `game`/`profile`/`ordered` values stop before RTC credential retrieval; missing optional fields retain compatibility with the observed live catalog. Task-specific spec and quality reviews PASS.

## Task 14: Prove the existing 1400-byte transport profile (F-14)

**Files:**

- Modify: `linux/src/managed_gateway_main.cpp`
- Modify: `linux/transport/q2_doszone_gateway.cpp`
- Modify: `linux/transport/gateway_options.cpp`
- Modify: `linux/tests/unit_tests.cpp`
- Modify: `linux/tests/fake_legacy.py`
- Modify: `linux/tests/fake_lifecycle.sh`
- Modify: `windows/gateway/src/main.cpp`
- Modify: `windows/gateway/assets/transport.html`
- Modify: `windows/gateway/assets/transport_protocol.mjs`
- Modify: `windows/gateway/src/webview_bridge_protocol.cpp`
- Modify: `windows/gateway/tests/transport_protocol.test.mjs`
- Modify: `windows/gateway/tests/webview_bridge_protocol_test.cpp`
- Modify: `docs/ARCHITECTURE.md`

- [x] RED: add Windows encode/decode boundary tests and a Linux fake-lifecycle UDP integration case; 1400 bytes is accepted and 1401 bytes is rejected/count-recorded through the real managed gateway path, not only option parsing.
- [x] GREEN: keep the 1400-byte ceiling. Add counters only for packet sizes and drops; never log packet payloads or credentials.
- [x] Confirm identical definition on Windows and Linux.
- [ ] In live gates, record maximum observed datagram for representative `baseq2` and `3zb2` sessions, including map entry, movement and firing.
- [ ] If live traffic exceeds 1400, stop the release and correct the shared documented ceiling based on evidence; do not increase it speculatively.

Evidence (2026-08-23): Linux release CTest 4/4, focused CTest 3/3 and ASan/UBSan 2/2 PASS; Windows Node 5/5 and portable C++ bridge PASS. Both platforms expose the same four numeric max/drop fields, accept 1400, reject/count 1401 and remain usable afterward. Task-specific spec and quality reviews PASS. Representative live maxima remain assigned to Task 16.

## Task 15: Make documentation claims match artifacts (F-15, F-18)

**Files:**

- Modify: `README.md`
- Modify: `linux/README.md`
- Modify: `windows/README.md`
- Modify: `windows/product/README.md`
- Modify: `docs/RELEASE.md`
- Modify: `docs/ARCHITECTURE.md`
- Modify: `docs/adr/0002-launcher-owned-session-lifecycle.md`
- Modify: `docs/adr/0003-linux-managed-session-and-guardian.md`
- Delete from public product docs: `windows/product/START-HERE.txt`

- [x] Before live artifact evidence exists, replace absolute success claims with exact status (`unit-tested`, `maintainer-observed`, or `not yet reverified from rc6 artifact`).
- [x] Make `docs/RELEASE.md` the sole release-chain authority; other READMEs link to it.
- [x] Replace “signed binaries” with “release binaries”; state that rc6 community artifacts are unsigned.
- [x] Remove unused/misleading `START-HERE.txt`; the package READMEs remain the entry points.
- [x] Keep `input/` and `docs/reviews/` outside the future public boundary. Do not delete the user's local copies in this task.
- [x] Audit truth claims directly:

```bash
rg -n "verified|verification|PASS|passed|готов|провер" README.md INSTALLATION.md docs linux/README.md windows/README.md windows/product/README.md
```

Every positive release claim must point to the exact rc6 verification record; historical ADR decisions must be labeled historical/maintainer evidence where applicable.

- [x] Run `git diff --check`, Bash syntax, Python compile and PowerShell parser checks.

Expected: no documentation says a gate passed unless `docs/verification/1.0.0-rc6.md` names the exact artifact and evidence hash.

Evidence (2026-08-23): public product docs now distinguish source/unit results, historical maintainer observations and pending exact rc6 artifact gates; `docs/RELEASE.md` is the sole release-chain authority; rc6 Windows wording is unsigned/closed-alpha; obsolete `START-HERE.txt` was removed without touching local review/input files. Static syntax/parser gates, task-specific spec review and quality review PASS.

## Task 16: Build and prove the single-room releases on both platforms

**Files:**

- Create/update: `docs/verification/1.0.0-rc6.md`
- No raw evidence in Git

### Linux live Dos.Zone gate

- [x] Use `doszone-q2-testing` to create/own a real Quake II room at `https://dos.zone/mp/?lobby=q2` with the supported 3zb2 profile.
- [x] Build and smoke the rc6 Linux package from verified WebRTC-NET artifacts:

```bash
export Q2DZ_WEBRTC_NET_SOURCE_DIR=/path/to/DosZone/WebRTC-NET
export Q2DZ_WEBRTC_NET_RELEASE_DIR=/path/to/DosZone/noncommit/q2prox-dz-pre-cleanup-20260823/runs/20260821T162440Z/release
linux/tools/build-package.sh dist
linux/tests/package_smoke.sh
```

- [x] Verify, extract and install the exact archive in a disposable first-run directory before installing it for the real local user:

```bash
(cd dist && sha256sum -c q2prox-dz-1.0.0-rc6-linux-x86_64.tar.gz.sha256)
linux_unpack=$(mktemp -d -t q2prox-dz-rc6.XXXXXX)
tar -C "$linux_unpack" -xzf dist/q2prox-dz-1.0.0-rc6-linux-x86_64.tar.gz
"$linux_unpack/q2prox-dz-1.0.0-rc6/install.sh"
```
- [x] Use exact local Q2PRO-X and a clean add-on profile; let normal Q2 autosave occur.
- [x] Join the live room, enter the map, move, fire and verify those actions from the owner browser.
- [x] Close the room from the owner side. Require bounded room-close, gateway/Q2 lifecycle according to the current single-room contract, and zero orphan gateway/Wine helper owned by the add-on.
- [x] Repeat normal Q2 exit and hard launcher kill; inspect PGIDs, guardian cleanup, session directory and credential-shaped persistence.
- [x] Run the packaged launcher with the exact local client, then verify cleanup:

```bash
Q2DZ_Q2_EXE=~/Games/Quake2-PlayGround-2026/Q2PRO-X.exe \
  "$HOME/.local/share/q2dz/bin/q2dz-launch"
linux/tools/assert-no-orphans.sh
ps -u "$USER" -eo pid,ppid,pgid,rss,pcpu,etime,args | rg 'q2dz|Q2PRO-X|wine' || true
```

### Windows live Dos.Zone gate

- [x] Build rc6 on the Windows build VM using the one release command.
- [x] Restore and start the clean test VM; do not install build tools there:

```bash
VBoxManage snapshot 'Windows 11 25H2 + LTSC (x64) 28in1' restore 'clean-windows-25h2-pre-product-20260822'
VBoxManage startvm 'Windows 11 25H2 + LTSC (x64) 28in1' --type gui
```
- [x] Discover Q2PRO-X or install/update the documented current version; record EXE hash/version and WebView2 Runtime version.
- [x] Copy only the produced portable ZIP, verify its adjacent SHA-256, extract fresh, and follow the end-user guide.
- [x] Join a real supported Dos.Zone room, enter the map, move, fire and verify in the owner browser.
- [x] Close the owner room and run normal Q2 exit; use the exact packaged lifecycle matrix for gateway crash, Q2 crash and hard launcher kill. Require no orphan q2dz/Q2PRO-X/WebView2 session processes and no credentials in args/env/logs/session root.
- [x] After each executed Windows lifecycle case save and inspect this process view:

```powershell
$sessionRoot = Join-Path $env:LOCALAPPDATA 'Q2PRO-X Dos.Zone\sessions'
Get-CimInstance Win32_Process |
  Where-Object {
    $_.Name -match '^(q2dz|q2-doszone|Q2PRO-X)' -or
    ($_.Name -eq 'msedgewebview2.exe' -and $_.CommandLine -like "*$sessionRoot*")
  } |
  Select-Object ProcessId,ParentProcessId,Name,CommandLine
```

Expected: no rows after the session ends.
- [ ] Restore snapshot; repeat the live join through map/movement/fire from the setup ZIP installation. Install, same-version update and owned-only uninstall passed; the installed launcher reached `Active`, but the separate live run ended while the minimal VM Q2 installation was downloading missing content.

### Record

- [x] In `docs/verification/1.0.0-rc6.md`, record source SHA, artifact SHA-256, OS/build, toolchain, exact Q2PRO-X hash, dependency hashes, commands, gate verdicts and hashes of sanitized evidence.
- [x] Keep movement/fire screenshots and process observations outside Git; no packet capture was required for this single-room gate.

Recorded result: Linux and Windows portable are independently PASS. Windows setup packaging/ownership is PASS, while its separate installed-copy live gameplay repetition remains open. One platform's evidence does not stand in for the other.

## Task 17: Prove exact Q2PRO-X Custom-source and socket behavior before hub code

**Files:**

- Create: `tools/experiments/q2pro_custom_source_fixture.py`
- Create: `tools/experiments/test_q2pro_custom_source_fixture.py`
- Modify: `docs/adr/0004-q2pro-x-custom-source-room-hub.md`
- Raw evidence remains outside Git

- [ ] RED/GREEN a disposable Python-stdlib localhost fixture that serves:
  - newline-separated endpoints;
  - six-byte IPv4 endpoints for `raw=2`;
  - an add-on-owned file source;
  - two UDP Quake II OOB status responders;
  - request and source-tuple metadata only, never payload contents.
- [ ] Run the fixture unit check and start one evidence instance:

```bash
python3 -m unittest tools/experiments/test_q2pro_custom_source_fixture.py -v
gate17=/path/to/DosZone/noncommit/q2prox-dz-gates/$(date -u +%Y%m%dT%H%M%SZ)/gate-17
mkdir -p "$gate17"
python3 tools/experiments/q2pro_custom_source_fixture.py --evidence-dir "$gate17"
```

The fixture writes `manifest.json`, `capture-filter.txt`, and sanitized request/tuple logs before printing its three source URIs.
- [ ] Gate 17.1 on Linux and Windows: record exact Q2PRO-X EXE SHA/version and clean profile before tests.
- [ ] Gate 17.2 on both: compare text HTTP, binary HTTP `+...raw=2`, and `file://`; prove Custom rows, Refresh refetch, quoting, autosave behavior, byte order and real status/ping.
- [ ] Compare all three as required by the review, then choose in this order: newline-separated HTTP if it works; binary HTTP `raw=2` only if text HTTP fails or loses required behavior; `file://` only if both HTTP forms fail and exact Q2PRO-X safely rereads an add-on-owned file. Implement only the selected format later.
- [ ] Gate 17.3 on Linux with `tcpdump`/`tshark`, and Windows with `pktmon`: trace `status A → status B → connect A → Disconnect → status B → connect B`. Fixture chooses dynamic ports and writes them to a manifest; capture filters read that manifest, so no hard-coded port is required.
- [ ] Linux capture commands:

```bash
sudo tcpdump -i lo -U -F "$gate17/capture-filter.txt" -w "$gate17/linux-loopback.pcap"
tshark -r "$gate17/linux-loopback.pcap" -Y udp -T fields \
  -e frame.time_epoch -e ip.src -e udp.srcport -e ip.dst -e udp.dstport \
  > "$gate17/linux-tuples.tsv"
```

- [ ] Windows capture commands after copying the fixture manifest to `C:\Users\Public\q2dz-gate17`:

```powershell
$gate17 = 'C:\Users\Public\q2dz-gate17'
$ports = @((Get-Content -LiteralPath "$gate17\manifest.json" -Raw | ConvertFrom-Json).udpPorts)
pktmon filter remove
foreach ($port in $ports) { pktmon filter add "q2dz-$port" -p $port }
pktmon start --capture --pkt-size 0 --file-name "$gate17\windows-loopback.etl"
# Perform Refresh, Room A connect/disconnect, Refresh, Room B connect.
pktmon stop
pktmon etl2pcap "$gate17\windows-loopback.etl" -o "$gate17\windows-loopback.pcapng"
```
- [ ] Require source ownership within each room flow. Do not require one UDP port for Q2's entire lifetime if the trace shows a safe change between rooms.
- [ ] Gate 17.4: create two real Dos.Zone rooms and run one Q2 PID through Room A movement/fire → Disconnect → Room B movement/fire on Linux, then Windows.
- [ ] Gate 17.5: measure local and host resource impact at 1, 4 and 8 ready gateways; record WebView2 tree/RSS/CPU/UDP endpoints and whether idle ready peers consume game slots. Choose and document the measured cap.
- [ ] Gate 17.6: close inactive and active rooms separately; prove inactive close leaves active play intact and active close disconnects without exiting Q2.
- [ ] Gate 17.7: test room disappearance/reappearance, alias/PeerId generation change and expired credentials; old endpoints/configs must not be reused.

Decision:

- [ ] If source Gate 17.2 or tuple Gate 17.3 fails on either platform, keep ADR 0004 `Proposed`, record the failure, skip Tasks 18-22, and retain the finalized single-room picker as the release.
- [ ] If Gates 17.1-17.7 pass, update ADR 0004 with the measured source format, URI behavior, tuple rule, resource cap and credential-generation rule; set it `Accepted` and continue.

## Task 18: Define and test the minimum hub-owner protocol (F-03)

**Files:**

- Create: `docs/HUB-OWNER-PROTOCOL.md`
- Create: `windows/hub-session/src/hub_protocol.hpp`
- Create: `windows/hub-session/src/hub_protocol.cpp`
- Create: `windows/hub-session/tests/hub_state_tests.cpp`
- Create: `linux/src/hub_protocol.hpp`
- Create: `linux/src/hub_protocol.cpp`
- Create: `linux/tests/hub_state_tests.cpp`
- Do not create a cross-platform runtime library

- [ ] Specify one bounded stdin/stdout protocol:

```text
Q2DZ-HUB/1 ADD r0001 1234\n
<1234 opaque room-config bytes containing the current room generation>
Q2DZ-HUB/1 REMOVE r0001\n
Q2DZ-HUB/1 START\n
Q2DZ-HUB/1 STOP\n

Q2DZ-HUB/1 READY slot=r0001 udp=127.0.0.1:31001
Q2DZ-HUB/1 REMOVED slot=r0001 reason=room_closed
Q2DZ-HUB/1 Q2_STARTED
Q2DZ-HUB/1 FATAL stage=protocol code=invalid_command
```

`r0001` is launcher-local and non-secret. Room config remains opaque and stdin-only. Lines are bounded at 4096 bytes, config at 256 KiB to match the existing downstream limit, and slot count at the measured cap. No credentials appear in stdout/logs/args/env/files. The launcher registry, not the native opaque owner, rejects stale `(roomId, alias, rtcServer, generation)` values before `ADD`; the owner rejects duplicate live slot IDs.

- [ ] RED in both named test files: add two slots; remove A without stopping B/Q2; active B close retains Q2; Q2 exit stops all; owner EOF stops all; duplicate slot and bounds rejected. Stale generation remains a launcher-registry test in Task 21.
- [ ] GREEN only each platform's small parser/state transitions. Reuse existing child-event validation patterns; do not create a plugin system, RPC library or generic process supervisor.
- [ ] Build the state tests through the platform targets introduced in Tasks 19 and 20; expected test names are `q2dz-hub-state-tests` on both platforms.

## Task 19: Implement the Windows native hub owner

**Files:**

- Create: `windows/hub-session/CMakeLists.txt`
- Create: `windows/hub-session/src/main.cpp`
- Create: `windows/hub-session/tests/fake_hub_gateway.cpp`
- Create: `windows/hub-session/tests/fake_hub_q2.cpp`
- Create: `windows/hub-session/tools/run-hub-lifecycle-matrix.ps1`
- Reuse by compiling `windows/coordinator/src/windows/win32_support.cpp` and `windows/coordinator/src/child_protocol.cpp` into this target with include paths `windows/coordinator/src/windows` and `windows/coordinator/include`; do not move them or create a common framework

- [ ] RED fake lifecycle: one fake Q2 and two fake gateways in one Job; A room-close removes only A; B continues; Q2 exit kills B; owner hard-kill kills Q2/gateways; immediate terminal events are not lost; command/event buffers are bounded.
- [ ] GREEN: one Job Object with `KILL_ON_JOB_CLOSE`; create every gateway and Q2 suspended, assign before resume; keep per-gateway pipes/events; one gateway failure never closes the Job; Q2/owner exit closes the Job.
- [ ] Use exact managed gateway contract and fresh room config per `ADD`; never invoke existing single-room coordinators as children.
- [ ] Run fake matrix repeatedly and inspect for orphan processes.

```powershell
cmake -S .\windows\hub-session -B .\windows\hub-session\out\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build .\windows\hub-session\out\release --config Release
ctest --test-dir .\windows\hub-session\out\release -C Release --output-on-failure
pwsh .\windows\hub-session\tools\run-hub-lifecycle-matrix.ps1 -BuildDir .\windows\hub-session\out\release
```

Expected: one small executable `q2dz-hub-session.exe`; existing `q2dz-session.exe` remains unchanged as fallback.

## Task 20: Implement the Linux native hub owner

**Files:**

- Create: `linux/src/hub_session_main.cpp`
- Create: `linux/src/process_support.cpp`
- Create: `linux/src/process_support.hpp`
- Modify: `linux/CMakeLists.txt`
- Create: `linux/tests/fake_hub_lifecycle.sh`
- Modify: `linux/src/session_main.cpp`

- [ ] RED fake lifecycle equivalent to Task 19, including owner `SIGKILL` cleanup.
- [ ] GREEN: extract only existing spawn, environment-allowlist and guardian/session-marker primitives from the anonymous namespace in `session_main.cpp` into `process_support.{hpp,cpp}` and use them from both owners. Then use one native owner, one Q2 process group, one process group per gateway, and one guardian/session marker. Individual room removal kills only that gateway PGID. Q2/owner exit uses marker-based cleanup for all remaining groups.
- [ ] Keep sealed anonymous `memfd`, environment allowlist and loopback sockets. No Python-owned lifecycle boundary.
- [ ] Repeat until stable with per-case logs; no swallowed failures.

```bash
hub_build=$(mktemp -d -t q2dz-hub-build.XXXXXX)
cmake -S linux -B "$hub_build" -G Ninja -DBUILD_TESTING=ON -DQ2DZ_REQUIRE_WEBRTC_GATEWAY=OFF
cmake --build "$hub_build" --parallel
ctest --test-dir "$hub_build" -R 'hub-state|fake-hub-lifecycle' --output-on-failure
```

Expected: one `q2dz-hub-session` executable; current `q2dz-session` remains the single-room fallback.

## Task 21: Add the smallest Custom-source room hub launcher

**Files:**

- Modify: `windows/product/q2dz-launch.ps1`
- Modify: `windows/product/catalog.ps1`
- Modify: `windows/product/tests/catalog.tests.ps1`
- Modify: `linux/tools/q2dz-launch`
- Modify: `linux/tests/test_launcher.py`
- Modify: `windows/product/package-windows.ps1`
- Modify: `windows/product/tests/package-windows.tests.ps1`
- Modify: `linux/tools/build-package.sh`
- Modify: `linux/tests/package_smoke.sh`

- [ ] Keep the existing single-room picker behind an explicit `--single-room`/documented shortcut.
- [ ] Exact long-lived topology: the PowerShell/Python launcher owns catalog reconciliation and the selected HTTP/file source; it starts the native hub owner as a child and keeps its stdin/stdout open. The native owner owns Q2 and all gateways. Launcher exit/pipe EOF makes the native owner kill its complete process domain. The native owner starts one Q2 only after the first gateway is READY. A gateway close removes only that source row and child.
- [ ] Default hub flow: locate Q2PRO-X; start the selected loopback HTTP server or create the selected add-on-owned source file; start native hub owner with non-secret CLI arguments `--q2`, the resolved Q2 path, `--custom-source`, and the measured source URI; fetch/validate catalog; send `ADD` up to measured cap; publish only READY loopback endpoints; update the source atomically when rooms appear/disappear.
- [ ] Poll at one fixed 30-second interval; no configurable scheduler. Obtain fresh join config only when creating/restarting a gateway.
- [ ] If the selected format requires localhost HTTP, use the platform runtime's standard library and bind only `127.0.0.1` to an ephemeral port. If `file://` won, atomically replace one add-on-owned file. Do not implement the losing formats.
- [ ] After Task 17 fixes exact quoting, native owner appends `+set cl_serverbrowser_custom_source`, the measured source URI, and `+set cl_serverbrowser_default_source custom` (or the exact measured fork spelling) to the Q2 argv vector. Restore only add-on-owned temporary state on exit. Do not occupy `adr0..adr15` Favorites.
- [ ] Show concise stages/errors in the existing terminal UI; do not add a GUI framework.
- [ ] Tests: catalog refresh, endpoint add/remove, generation replacement, cap, owner EOF, Q2 not found, missing content, room close, credential non-persistence and single-room fallback.

## Task 22: Package and live-test the hub on Linux and Windows Dos.Zone

**Files:**

- Modify: `windows/product/build-payload.ps1`
- Modify: `windows/product/package-windows.ps1`
- Modify: `windows/product/tests/package-windows.tests.ps1`
- Modify: `windows/product/tests/release-smoke.tests.ps1`
- Modify: `linux/CMakeLists.txt`
- Modify: `linux/tools/build-package.sh`
- Modify: `linux/tests/package_smoke.sh`
- Modify: `README.md`
- Modify: `INSTALLATION.md`
- Modify: `windows/product/README.txt`
- Modify: `windows/product/README-PORTABLE.txt`
- Modify: `linux/README.md`
- Update: `docs/verification/1.0.0-rc6.md`

- [ ] Add only the new hub executable and selected source adapter files to each existing artifact. Do not create a fourth distribution.
- [ ] Re-run exact source-to-artifact chains and fresh-extract smoke on both platforms.
- [ ] Linux live gate: create two real Dos.Zone rooms; from fresh rc6 package run one Q2 PID; Custom Refresh; Room A movement/fire; Disconnect; Room B movement/fire; close inactive/active rooms; exit Q2; inspect guardian/process/session cleanup.
- [ ] Windows live gate: restore clean VM; install/extract rc6; repeat the same two-room scenario in one Q2 PID; inspect the Job/WebView2 process trees and setup ownership.
- [ ] For both platforms run 1/4/8 room resource measurements or the lower measured cap, source refresh, room generation replacement, credential expiry and hard owner kill.
- [ ] Scan args, environment, logs, artifacts and session residue for credential-shaped fields without printing matched secrets.
- [ ] If hub live gate fails, remove hub from default/package, retain single-room artifacts, record the hub failure and leave ADR accepted only if feasibility itself remains proven.

Expected: packaged live switching is PASS on both Linux and Windows. Otherwise rc6 ships only the proven single-room product.

## Task 23: Final verification, repository boundary and handoff

**Files:**

- Update: `docs/verification/1.0.0-rc6.md`
- Update: `docs/RELEASE.md`
- Update: `README.md`
- Update: `INSTALLATION.md`
- Update: `windows/README.md`
- Update: `windows/product/README.md`
- Update: `windows/product/README.txt`
- Update: `windows/product/README-PORTABLE.txt`
- Update: `linux/README.md`
- No raw review/input/evidence files in the curated public set

- [ ] Run full Linux source tests, repeated lifecycle, dependency verification, reproducibility, package smoke and live Dos.Zone gate.
- [ ] Run full Windows gateway/coordinator/hub tests as applicable, lifecycle matrices, `/Brepro`, catalog/setup/Gate P tests, exact release build and both clean-VM live Dos.Zone gates.
- [ ] Run repository hygiene:

```bash
git diff --check
for f in linux/tools/*.sh linux/packaging/*.sh linux/tests/*.sh; do bash -n "$f"; done
python3 -m py_compile tools/*.py linux/tools/*.py linux/tools/q2dz-launch linux/tests/*.py
git status --short --branch
git ls-files
```

- [ ] On Windows parse every tracked `.ps1` and fail on syntax errors:

```powershell
$parseErrors = [System.Collections.Generic.List[object]]::new()
git ls-files '*.ps1' | ForEach-Object {
  $tokens = $null
  $errors = $null
  [void][System.Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path $_), [ref]$tokens, [ref]$errors)
  foreach ($error in $errors) { $parseErrors.Add($error) }
}
if ($parseErrors.Count) { $parseErrors | Format-List; throw 'PowerShell parse gate failed' }
```
- [ ] Compare every F-01…F-18 row to its fresh command output and evidence hash. No “passed previously” substitution.
- [ ] Verify end-user docs cover: Windows portable, Windows setup/update/uninstall, Linux package/source install, Q2PRO-X absent/outdated, WebView2 absent, missing map/content, single-room fallback, room hub/Custom behavior, diagnostics and clean removal.
- [ ] Verify the future public file list excludes `input/`, reviewer requests, raw evidence, VM files, build trees, credentials and local paths.
- [ ] Do not use `git add -A`. If a later separate instruction authorizes the first public commit, stage the curated file list only, verify `git diff --cached --check`, reconstruct one commit authored by `doszoner` with exact message `initial commit`, and push only after another explicit push instruction.

## 6. Final plan self-review checklist

- [ ] Every F-01…F-18 has one owner and one observable gate.
- [ ] Windows and Linux each have real Dos.Zone map/movement/fire/room-close verification from packaged artifacts.
- [ ] Hub code is forbidden until exact Q2PRO-X source and socket gates pass on both platforms.
- [ ] Single-room product remains shippable and documented if hub feasibility fails.
- [ ] No new runtime dependency, GUI framework, installer framework, generic RPC, cross-platform supervisor library or transport rewrite was introduced.
- [ ] Secrets remain stdin/memfd-only and absent from release/evidence surfaces.
- [ ] Public docs cannot outrun exact artifact evidence.
- [ ] No placeholder (`TODO`, `TBD`, `<port>`, `<path>`, invented hash) remains in executable instructions.
- [ ] Plan changes and implementation remain uncommitted until separately authorized.
