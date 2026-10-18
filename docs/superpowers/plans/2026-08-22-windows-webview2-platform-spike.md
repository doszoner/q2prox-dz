# Windows WebView2 Platform Spike Implementation Plan

> Исторический документ; не инструкция к текущей версии. Перенесён 2026-10-18.
> Отметки задач и результаты относятся к исходной дате; проверки сейчас не повторялись.
> Пути прототипов и локальных evidence могут отсутствовать. Актуальное состояние — в индексе docs/README.md.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove or reject the WebView2 transport backend on Windows x64 with the exact Platform Gate P contract.

**Architecture:** A Win32 gateway owns one loopback UDP socket and one hidden WebView2 controller. A packaged transport page loads `webrtcnet.mjs`/`webrtcnet.wasm`; JSON WebMessages carry one base64-encoded datagram per message and never persist ICE credentials. Q2PRO-X remains unchanged and connects to the gateway loopback port.

**Tech Stack:** C++17, Win32/Winsock, WebView2 Evergreen Runtime and SDK, CMake/MSVC, packaged WebRTC-NET v1.1.0 JS/WASM.

**Repository policy:** Do not commit plan, implementation, or evidence without a new explicit user instruction.

---

### Task 1: Prepare the Windows development VM

**Files:**
- Keep: `prototype/windows-spike/inventory.ps1`
- Create: `prototype/windows-spike/bootstrap.ps1`

- [x] Record Windows build, WebView2 version, tool versions, and `net.js-dos.com:444` reachability. Free-disk evidence remains to be added before final Gate P.
- [x] Install the smallest supported MSVC toolchain:

```powershell
winget install --exact --id Microsoft.VisualStudio.2022.BuildTools `
  --accept-package-agreements --accept-source-agreements `
  --override "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install --exact --id Kitware.CMake --accept-package-agreements --accept-source-agreements
winget install --exact --id Git.Git --accept-package-agreements --accept-source-agreements
```

- [x] Verify from `VsDevCmd.bat`:

```cmd
cl /Bv
cmake --version
git --version
```

Expected: MSVC x64 compiler, Windows SDK, CMake, and Git all resolve. The current modified VM is a development stand only; final PASS requires a clean snapshot/image.

### Task 2: Define the datagram WebMessage contract with a failing portable test

**Files:**
- Create: `prototype/tests/webview_bridge_protocol_test.cpp`
- Create: `prototype/src/webview_bridge_protocol.h`
- Create: `prototype/src/webview_bridge_protocol.cpp`
- Modify: `prototype/CMakeLists.txt`

- [x] Add a test that expects exact round-trips for `1, 32, 512, 1390, 1400` bytes and rejects `0` and `1401` bytes:

```cpp
for (const std::size_t size : {1, 32, 512, 1390, 1400}) {
    const auto input = deterministic_datagram(size);
    const auto message = encode_datagram_message(input);
    assert(decode_datagram_message(message) == input);
}
assert_throws([] { encode_datagram_message({}); });
assert_throws([] { encode_datagram_message(std::vector<std::uint8_t>(1401)); });
```

- [x] Build and run the new target before implementation.

```bash
cmake --build prototype/build --target webview_bridge_protocol_test
ctest --test-dir prototype/build -R webview_bridge_protocol_test --output-on-failure
```

Expected RED: compilation fails because the protocol functions do not exist.

- [x] Implement only two message forms:

```json
{"type":"udp","data":"<base64>"}
{"type":"state","value":"ready|failed|stopped","reason":"sanitized text"}
```

- [x] Re-run the new test and all existing prototype tests.

Expected GREEN: `4/4` tests pass on Linux.

### Task 3: Add the packaged WebRTC-NET transport page

**Files:**
- Create: `prototype/windows-spike/assets/transport.html`
- Copy exact release artifacts at execution time: `webrtcnet.mjs`, `webrtcnet.wasm`

- [x] On a single validated `start` WebMessage, set `window.netConfig`, instantiate `WebRTCNet`, call `connectTo`, wait for non-zero `myId`, and run exact alias query `=<alias>`.
- [x] Accept exactly one alias result, send one-byte connection warm-up until `sendto` succeeds, and post `state=ready`.
- [x] For each native `udp` message, decode one datagram and call WebRTC-NET `sendto` once. For each `recvfrom`, post one `udp` message without merging or splitting.
- [x] Reject a second `start`, malformed JSON, invalid aliases, payloads outside `1..1400`, additional alias matches, and messages from an unexpected peer.
- [x] Call `shutdown` on native `stop`; never write configuration, ICE servers, SDP, or candidates to DOM storage or console.

Expected: the page is entirely packaged and does not navigate to Dos.Zone or any arbitrary origin.

### Task 4: Add the hidden Win32 WebView2 gateway

**Files:**
- Create: `prototype/windows-spike/CMakeLists.txt`
- Create: `prototype/windows-spike/src/main.cpp`
- Create: `prototype/windows-spike/src/webview_host.cpp`
- Create: `prototype/windows-spike/src/webview_host.h`
- Create: `prototype/windows-spike/src/udp_bridge.cpp`
- Create: `prototype/windows-spike/src/udp_bridge.h`

- [x] Download and pin one `Microsoft.Web.WebView2` NuGet SDK version; record its SHA-256.
- [x] Build one x64 executable that binds only `127.0.0.1:<requested-or-random-port>` and captures only the first UDP source.
- [x] Map `https://q2doszone.local/` to the packaged assets directory and reject every other navigation, popup, permission, download, context menu, DevTools, and external-window request.
- [x] Read ICE JSON only from a named file, cap it at 64 KiB, send it once through `PostWebMessageAsJson`, then clear the native string after WebView acknowledgement.
- [x] Preserve datagram boundaries through the Task 2 contract; drop oversized packets and a second UDP source with sanitized counters.
- [ ] On normal exit or forced stop, close UDP, WebView2 controller/environment, user-data directory, and child Q2 process without orphaning `msedgewebview2.exe`.

Build command inside the MSVC environment:

```cmd
cmake -S prototype\windows-spike -B prototype\windows-spike\build -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build prototype\windows-spike\build --config Release --parallel
```

Expected: one Release gateway executable plus packaged HTML/JS/WASM; no portal content or game assets.

### Task 5: Run Windows transport Gates A-C

**Files:**
- Create per-run raw evidence only under ignored `runs/<UTC>/windows-platform-p/`
- Update after PASS/FAIL: `EVIDENCE-SUMMARY.md`

- [x] Start a controlled browser echo peer and capture its fresh alias plus fresh ICE configuration in a `0600`-equivalent private VM file.
- [x] Run alias readiness and binary echo for `1/32/512/1390/1400`, then the loopback UDP probe for the same sizes.
- [x] Require byte-identical payloads in both directions and increasing post-ready counters; `DataChannel open` alone is not PASS.
- [x] Record exact Windows, WebView2, WebRTC-NET, gateway, Q2PRO-X, and artifact hashes. Sanitize credentials before copying evidence out of the VM.

### Task 6: Run Q2PRO-X Gate D and lifecycle checks

**Files:**
- Create per-run raw evidence only under ignored `runs/<UTC>/windows-platform-p/`
- Update after PASS/FAIL: `EVIDENCE-SUMMARY.md`

- [x] Copy the exact known Q2PRO-X installation into the VM without adding it to Git.
- [x] Create a fresh Dos.Zone Q2 room through the real lobby and retain the browser host process.
- [x] Start the gateway, then launch existing `Q2PRO-X.exe +connect 127.0.0.1:<port>`.
- [x] Require protocol handshake, intended map entry, and a bounded gameplay action visible on both sides.

  Development-VM result for exact final artifact
  `de37ad5795e05e87d03c0a78f4372343e1d7a012b48b85bbffc6908a8cf5ab36`:
  protocol handshake and `q2dm1` entry passed; the browser-host scoreboard saw
  both players; chat action `WINDOWS_BIDIR_001` was visible in both Q2PRO-X and
  the browser-host console.
- [ ] Repeat normal Q2 exit, Q2 crash, gateway forced termination, and host-room close; require no orphan process, port, or control resource.

  Development-VM result: all cases were rerun on the exact final artifact.
  Normal owner stop cleans resources. Forced gateway termination removes its
  WebView2 processes and UDP port within 12 seconds but leaves Q2 alive. Forced
  Q2 termination leaves gateway/WebView2/UDP alive. Closing the browser host
  leaves Q2, gateway, six WebView2 processes and UDP alive after 40 seconds.
  Explicit owner stop cleans all resources. Keep this item open until the
  launcher owns and propagates both process exits and room-close shutdown.

### Task 7: Decide the ADR from exact evidence

**Files:**
- Modify: `docs/adr/0001-windows-webrtc-transport-backend.md`
- Modify: `GATEWAY-DESIGN.md`
- Modify: `README.md`
- Modify: `EVIDENCE-SUMMARY.md`

- [x] Scan args, environment, logs, approved crash artifacts, and evidence for ICE/TURN credentials.

  Development scope passed: no credential literal was found in tracked output,
  argv/environment or approved screenshots; raw evidence remains ignored and
  `0600`; the VM ICE file was removed. No crash dump was approved or retained.
  Source review also confirmed disabled DevTools/context menus/dialogs/host
  objects and blocked navigation/new-window/permission requests. CSP,
  installed-asset integrity/ACL and runtime negative tests remain open security
  work; this checkbox covers credential handling only.
- [ ] Produce SBOM/third-party notices and two reproducible Release builds with matching hashes.

  SPDX inventory and exact notice sources now exist under `docs/release/`.
  Linux builds match. Two ordinary Windows builds differ in COFF timestamp and
  checksum; two `/Brepro` builds match at SHA-256 `1a4c2f...`. Keep this open
  until production CMake enables the proven flag and the package installs full
  notice texts.
- [ ] If every Platform Gate P item passes on a clean Windows image, record the exact evidence and change ADR status to `Accepted`.
- [x] Decide whether a Candidate B plan is required.

  Not triggered: WebView2 transport and exact-client gameplay passed. The open
  lifecycle issue is launcher ownership shared by either transport backend.
- [ ] Run final repository gates:

```bash
cmake --build prototype/build
ctest --test-dir prototype/build --output-on-failure
git diff --check
```

Expected: ADR outcome matches evidence; no release claim is made from the modified development VM alone.
