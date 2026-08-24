import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

import * as transportProtocol from "../assets/transport_protocol.mjs";

const {
  bytesToMessage,
  createCadenceCounter,
  exactAliasQuery,
  messageToBytes,
  parseStartMessage,
  reliableSendMode,
} = transportProtocol;

test("cadence snapshot is numeric, redacted, and resets one interval together", () => {
  const cadence = createCadenceCounter();
  cadence.timerTick(100);
  cadence.waitCall();
  cadence.aliasPoll();
  cadence.probeSend();
  cadence.probeAck();
  cadence.dcToNative(4, 120);
  cadence.nativeToDc(3, 130);
  cadence.timerTick(1110);
  cadence.dcToNative(4, 1125);
  cadence.nativeToDc(3, 1140);

  assert.deepEqual(cadence.snapshot(), {
    type: "cadence",
    timerTicks: 2,
    timerMaxGapMs: 1010,
    waitCalls: 1,
    aliasPollIterations: 1,
    probeSendCount: 1,
    probeAckCount: 1,
    dcToNativeDatagrams: 2,
    dcToNativeBytes: 8,
    dcToNativeMaxGapMs: 1005,
    nativeToDcDatagrams: 2,
    nativeToDcBytes: 6,
    nativeToDcMaxGapMs: 1010,
  });

  const reset = cadence.snapshot();
  assert.deepEqual(Object.values(reset).slice(1), Array(12).fill(0));
  assert.deepEqual(Object.keys(reset), [
    "type", "timerTicks", "timerMaxGapMs", "waitCalls", "aliasPollIterations",
    "probeSendCount", "probeAckCount", "dcToNativeDatagrams", "dcToNativeBytes",
    "dcToNativeMaxGapMs", "nativeToDcDatagrams", "nativeToDcBytes",
    "nativeToDcMaxGapMs",
  ]);
  assert.ok(Object.values(reset).slice(1).every(Number.isInteger));
  const largest = Object.fromEntries(Object.keys(reset).map((field) => [
    field, field === "type" ? "cadence" : Number.MAX_SAFE_INTEGER,
  ]));
  assert.ok(JSON.stringify(largest).length <= 512);
});

test("validates startup and preserves supported datagram boundaries", () => {
  assert.equal(reliableSendMode, 0);
  const config = parseStartMessage({
    version: 1,
    roomId: "room.q2.example.1",
    alias: "q2.3573146018",
    ordered: false,
    iceServers: [{ urls: ["stun:example.invalid"] }],
  });
  assert.equal(exactAliasQuery(config.alias), "=q2.3573146018");

  for (const size of [1, 32, 512, 1390, 1400]) {
    const input = Uint8Array.from({ length: size }, (_, i) => (i * 31 + size) & 255);
    assert.deepEqual(messageToBytes(bytesToMessage(input)), input);
  }
});

test("rejects malformed startup and unsupported datagram sizes", () => {
  for (const value of [null, {}, { type: "start", alias: "../bad", iceServers: [] }]) {
    assert.throws(() => parseStartMessage(value));
  }
  assert.throws(() => bytesToMessage(new Uint8Array()));
  assert.throws(() => bytesToMessage(new Uint8Array(1401)));
  assert.throws(() => messageToBytes({ type: "udp", data: "%%%" }));
});

test("transport page gates readiness on the managed Quake OOB probe", () => {
  const html = readFileSync(new URL("../assets/transport.html", import.meta.url), "utf8");
  const protocol = readFileSync(new URL("../assets/transport_protocol.mjs", import.meta.url), "utf8");
  assert.match(html, /ManagedReadinessProbe/);
  assert.match(html, /probe\.channelOpened\(\)/);
  assert.match(html, /probe\.packetReceived\(datagram\)/);
  assert.match(protocol, /export const maxDatagram = 1400/);
  assert.match(html, /classifyIncomingDatagram\(length\)/);
  assert.doesNotMatch(html, /warmup/);
});

test("transport page reports one-second cadence at existing call sites", () => {
  const html = readFileSync(new URL("../assets/transport.html", import.meta.url), "utf8");
  assert.match(html, /createCadenceCounter/);
  assert.match(html, /const cadenceTimer = setInterval/);
  assert.match(html, /clearInterval\(cadenceTimer\)/);
  for (const call of [
    "timerTick", "waitCall", "aliasPoll", "probeSend", "probeAck",
    "dcToNative", "nativeToDc",
  ]) {
    assert.match(html, new RegExp(`cadence\\.${call}\\(`));
  }
});

test("incoming datagram size rejection is bounded", () => {
  assert.equal(typeof transportProtocol.classifyIncomingDatagram, "function");
  assert.equal(transportProtocol.classifyIncomingDatagram(1400), "accepted");
  assert.equal(transportProtocol.classifyIncomingDatagram(1401), "oversized");
  assert.equal(transportProtocol.classifyIncomingDatagram(1399), "accepted");
});

test("native gateway exposes only the managed non-sensitive startup contract", () => {
  const source = readFileSync(new URL("../src/main.cpp", import.meta.url), "utf8");
  const cmake = readFileSync(new URL("../CMakeLists.txt", import.meta.url), "utf8");
  for (const option of ["--managed-session", "--udp-port", "--owner-handle", "--user-data-dir"]) {
    assert.match(source, new RegExp(option));
  }
  assert.match(source, /record_oversized_datagram\(\s*static_cast<std::size_t>\(length\)/);
  assert.match(source, /record_accepted_datagram\(datagram\.size\(\)/);
  assert.match(source, /record_oversized_transport_drop\(message/);
  assert.doesNotMatch(source, /\+\+oversized_(?:udp|transport)_drops_/);
  assert.doesNotMatch(source, /--alias|--ice-file|read_ice_file/);
  assert.match(cmake, /managed-readiness-probe\.mjs/);
  assert.match(cmake, /CMAKE_MSVC_RUNTIME_LIBRARY/);
});

test("native gateway keeps WebView2 foreground without production browser flags", () => {
  const source = readFileSync(new URL("../src/main.cpp", import.meta.url), "utf8");
  const protocol = readFileSync(
    new URL("../src/webview_bridge_protocol.cpp", import.meta.url), "utf8");
  assert.match(source, /parse_cadence_message\(message\)/);
  assert.match(source, /Q2DZ_CADENCE/);
  assert.match(source, /std::ostringstream/);
  assert.match(source, /std::cerr << line\.str\(\)/);
  assert.match(source, /std::exchange\(native_cadence_, NativeCadence\{\}\)/);
  assert.doesNotMatch(source, /take_native_cadence|std::cerr\s*<<\s*"Q2DZ_CADENCE"/);
  assert.match(source, /invalid_cadence_drops_/);
  assert.match(source, /" invalid_cadence_drops="/);
  assert.match(source, /message\.rfind\("\{\\"type\\":\\"cadence\\""/);
  assert.match(protocol, /message\.size\(\) > 512/);
  assert.match(source, /GetTickCount64\(\)/);
  assert.match(source, /constexpr BOOL controller_visible = TRUE;/);
  assert.match(source, /put_IsVisible\(controller_visible\)/);
  assert.match(source,
    /L" controller_visible="\s*<<\s*\(controller_visible \? L"true" : L"false"\)/);
  assert.doesNotMatch(source,
    /controller_visible=(?:true|false)|put_IsVisible\((?:FALSE|TRUE)\)/);
  assert.match(source, /diagnostic_background_timer_flag_exact=/);
  assert.match(source, /diagnostic_background_timer_flag_exact\(\)/);
  assert.doesNotMatch(source, /diagnostic_background_timer_flag=/);
  assert.doesNotMatch(source, /SetEnvironmentVariable/);
  assert.doesNotMatch(source, /<<\s*message|<<\s*configuration_/);
});
