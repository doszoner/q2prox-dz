import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";
import * as vm from "node:vm";

import * as transportProtocol from "../assets/transport_protocol.mjs";
import {
  ManagedReadinessProbe,
  quakeOob,
} from "../../coordinator/web/managed-readiness-probe.mjs";

const {
  bytesToMessage,
  createCadenceCounter,
  createFairReceiveScheduler,
  exactAliasQuery,
  messageToBytes,
  createTargetRegistry,
  maxQueuedPacketsPerTarget,
  maxPumpPacketsPerTick,
  normalizeIceGeneration,
  normalizeTargetId,
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
  assert.equal(normalizeIceGeneration("9007199254740993"), 9007199254740993n);
  assert.equal(normalizeIceGeneration("18446744073709551615"),
    18446744073709551615n);
  for (const generation of [1, "01", "0", "18446744073709551616"]) {
    assert.equal(normalizeIceGeneration(generation), null);
  }

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
  assert.match(html, /timer = setInterval\(pump, 5\)/);
  assert.match(html, /clearInterval\(timer\)/);
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

test("routes tagged datagrams by virtual peer and guards duplicate concrete peers", () => {
  const registry = createTargetRegistry();
  const largeTarget = "9007199254740993";
  const firstRoute = 0x80000001;
  const secondRoute = 0x80000002;
  assert.equal(normalizeTargetId(largeTarget), largeTarget);
  assert.equal(transportProtocol.parseTargetMessage({
    targetId: "18446744073709551615", alias: "q2.max",
  }).targetId, "18446744073709551615");
  assert.equal(normalizeTargetId("09007199254740993"), null);
  assert.equal(normalizeTargetId("18446744073709551616"), null);
  assert.equal(registry.put({ targetId: largeTarget, alias: "q2.alpha" }), true);
  assert.equal(registry.put({ targetId: "12", alias: "room.q2ded.beta" }), true);
  assert.equal(registry.bindRoute(largeTarget, firstRoute), "bound");
  assert.equal(registry.bindRoute("12", secondRoute), "bound");
  assert.equal(registry.reserveConcrete(largeTarget, 101), "reserved");
  assert.equal(registry.reserveConcrete("12", 202), "reserved");
  assert.equal(registry.reserveConcrete("12", 0x80000001), "invalid_peer");
  assert.equal(registry.reserveConcrete("12", 101), "duplicate_remote_peer");

  const payload = Uint8Array.from([1, 2, 3]);
  assert.deepEqual(registry.send(largeTarget, payload), {
    type: "udp", targetId: largeTarget, data: bytesToMessage(payload).data,
  });
  assert.deepEqual(registry.receive(202, payload), {
    type: "drop", reason: "unknown_peer",
  });
  assert.deepEqual(registry.receive(secondRoute, payload), {
    type: "udp", targetId: "12", data: bytesToMessage(payload).data,
  });
  assert.deepEqual(registry.receive(999, payload), {
    type: "drop", reason: "unknown_peer",
  });

  assert.equal(registry.close(largeTarget), true);
  assert.equal(registry.targetForRoute(firstRoute), undefined);
  assert.equal(registry.targetForConcrete(101), undefined);
  assert.equal(registry.targetForRoute(secondRoute), "12");
  assert.equal(registry.targetForConcrete(202), "12");
  assert.equal(registry.targets.size, 1);
});

test("keeps OOB probes and close state independent for two targets", () => {
  const timers = [];
  const ready = [];
  const closed = [];
  const makeProbe = (targetId) => new ManagedReadinessProbe({
    send: () => true,
    onReady: () => ready.push(targetId),
    onRoomClosed: (reason) => closed.push([targetId, reason]),
    setTimer: (callback) => {
      timers.push(callback);
      return callback;
    },
    clearTimer: () => {},
  });
  const first = makeProbe(11);
  const second = makeProbe(12);
  first.channelOpened();
  second.channelOpened();
  assert.equal(first.packetReceived(quakeOob("ack")), true);
  assert.deepEqual(ready, [11]);
  first.channelClosed("probe_timeout");
  assert.deepEqual(closed, [[11, "probe_timeout"]]);
  assert.equal(second.packetReceived(quakeOob("ack")), true);
  assert.deepEqual(ready, [11, 12]);
  assert.equal(second.closed, false);
  assert.ok(timers.length >= 2);
});

test("pump work is bounded while per-target probes and ICE precede connections", () => {
  const registry = createTargetRegistry();
  registry.put({ targetId: 1, alias: "q2.alpha" });
  registry.bindRoute(1, 0x80000001);
  let received = 0;
  assert.equal(registry.pump(
    () => ({ peer: 101, data: Uint8Array.of(received++) }),
    () => {},
  ), maxPumpPacketsPerTick);

  const html = readFileSync(new URL("../assets/transport.html", import.meta.url), "utf8");
  assert.equal((html.match(/import WebRTCNet/g) ?? []).length, 1);
  assert.equal((html.match(/setInterval\(/g) ?? []).length, 1);
  assert.match(html, /new ManagedReadinessProbe/);
  assert.match(html, /const targets = new Map/);
  assert.match(html, /const routePeerToTarget = new Map/);
  assert.match(html, /const concretePeerToTarget = new Map/);
  assert.ok(html.indexOf("setIceServers") < html.indexOf("connectTo"));
});

test("production scheduler gives a second target a delivery slot under a flood", () => {
  const single = Array.from({ length: maxPumpPacketsPerTick }, () => ({
    peer: 303, data: Uint8Array.of(3),
  }));
  const singleDelivered = [];
  const singleResult = createFairReceiveScheduler().pump(
    () => single.shift(),
    () => "303",
    (targetId, data) => singleDelivered.push([targetId, data[0]]),
  );
  assert.equal(singleResult.scanned, maxPumpPacketsPerTick);
  assert.equal(singleResult.delivered, maxPumpPacketsPerTick);
  assert.equal(singleDelivered.length, maxPumpPacketsPerTick);

  const scheduler = createFairReceiveScheduler();
  const input = [
    ...Array.from({ length: maxPumpPacketsPerTick - 1 }, () => ({
      peer: 101, data: Uint8Array.of(1),
    })),
    { peer: 202, data: Uint8Array.of(2) },
  ];
  const delivered = [];
  const drops = [];
  const result = scheduler.pump(
    () => input.shift(),
    (peer) => peer === 101 ? "9007199254740993" : peer === 202 ? "12" : undefined,
    (targetId, data) => delivered.push([targetId, data[0]]),
    (packet, targetId) => drops.push([packet.peer, targetId]),
  );
  assert.equal(result.scanned, maxPumpPacketsPerTick);
  assert.ok(delivered.some(([targetId, value]) => targetId === "12" && value === 2));
  assert.equal(delivered.filter(([targetId]) => targetId === "9007199254740993").length,
    maxPumpPacketsPerTick - 1);
  assert.deepEqual(drops, []);

  const interleaved = createFairReceiveScheduler();
  const fortyPackets = Array.from({ length: 40 }, (_, index) => ({
    peer: 1000 + index,
    data: Uint8Array.of(index + 1),
  }));
  const fortyRouted = [];
  const fortyResult = interleaved.pump(
    () => fortyPackets.shift(),
    (peer) => peer === 1000 ? "9007199254740993" : String(peer - 988),
    (targetId, data) => fortyRouted.push([targetId, data[0]]),
  );
  assert.equal(fortyResult.scanned, 40);
  assert.equal(fortyRouted.length, 40);
  assert.deepEqual(fortyRouted.map(([, value]) => value),
    Array.from({ length: 40 }, (_, index) => index + 1));

  const html = readFileSync(new URL("../assets/transport.html", import.meta.url), "utf8");
  assert.match(html, /createFairReceiveScheduler/);
  assert.match(html, /receiveScheduler\.pump\(/);
  assert.match(html, /maxPumpPacketsPerTick/);
  assert.equal(maxQueuedPacketsPerTarget, 64);
});

test("startup burst capacity admits ICE plus all bounded targets before page-ready", () => {
  const source = readFileSync(new URL("../src/main.cpp", import.meta.url), "utf8");
  const lineBytes = 64 * 1024;
  const queueCapacity = 256 + 2;
  const burst = ["I".repeat(lineBytes), ...Array.from(
    { length: 256 }, (_, index) => "P".repeat(lineBytes - String(index + 1).length))];
  assert.equal(burst.length, 257);
  assert.ok(burst.reduce((sum, line) => sum + line.length, 0) <= lineBytes * queueCapacity);
  assert.match(source, /max_control_queue_commands = max_targets \+ 2/);
  assert.match(source, /max_control_queue_bytes = max_control_bytes \* max_control_queue_commands/);
  assert.match(source, /control_lines_\.size\(\) >= max_control_queue_commands/);
  assert.match(source, /lines\.size\(\) < max_control_commands_per_tick/);
});

test("native gateway exposes only the managed non-sensitive startup contract", () => {
  const source = readFileSync(new URL("../src/main.cpp", import.meta.url), "utf8");
  const cmake = readFileSync(new URL("../CMakeLists.txt", import.meta.url), "utf8");
  const wasmSourcePath = process.env.WEBRTC_NET_SOURCE_DIR
    ? join(process.env.WEBRTC_NET_SOURCE_DIR, "webrtcnet", "src", "main.cpp")
    : new URL("../../../../WebRTC-NET-q2prox-shared/webrtcnet/src/main.cpp", import.meta.url);
  const wasmSource = readFileSync(wasmSourcePath, "utf8");
  for (const option of ["--managed-session", "--udp-port", "--owner-handle", "--user-data-dir"]) {
    assert.match(source, new RegExp(option));
  }
  assert.match(source, /record_oversized_datagram\(\s*static_cast<std::size_t>\(length\)/);
  assert.match(source, /record_accepted_datagram\(datagram\.size\(\)/);
  assert.match(source, /record_oversized_transport_drop\(message/);
  assert.match(source, /record_unknown_peer_drop\(message/);
  assert.match(source, /fail_target\(target->second, "udp", "udp_async_error"\)/);
  assert.match(source, /fail_target\(id, "udp", "udp_receive_failed"\)/);
  assert.doesNotMatch(source, /fail\("udp_async_error"\)/);
  assert.ok(source.includes('targetId\\":\\"'));
  assert.doesNotMatch(source, /\+\+oversized_(?:udp|transport)_drops_/);
  assert.doesNotMatch(source, /--alias|--ice-file|read_ice_file/);
  assert.match(source, /control_mode_ = mode;/);
  assert.match(source, /control_mode_ == ControlMode::hub/);
  assert.match(source, /Q2DZ\/HUB\/2 FATAL stage=gateway code=/);
  assert.doesNotMatch(source, /legacy_mode_ \|\| targets_\.empty\(\)/);
  assert.match(source, /generation\\":\\"/);
  assert.match(source, /void fail_target\([\s\S]*?post_hub_command\(drop\);[\s\S]*?close_target\(target_id\)/);
  assert.match(source, /constexpr UINT retire_socket_message = WM_APP \+ 3;/);
  assert.match(source, /WSAAsyncSelect\(target->second\.socket, window_, 0, 0\)/);
  assert.match(source, /retiring_sockets_\.insert/);
  assert.match(source, /if \(message == retire_socket_message\)/);
  assert.match(source, /read_hub_bytes\(pending, hub_pending_\);\s*secure_clear\(pending\);/);
  const readHub = source.slice(source.indexOf("void read_hub_bytes"),
    source.indexOf("void start_control_reader"));
  assert.match(readHub, /std::string remainder = pending\.substr\(newline \+ 1U\)/);
  assert.match(readHub, /secure_clear\(pending\);\s*pending = std::move\(remainder\)/);
  assert.doesNotMatch(readHub, /pending\.erase\(0/);
  assert.match(source, /SecureZeroMemory\(value,/);
  assert.match(source, /secure_clear\(wide\);/);
  assert.match(source, /ControlMode mode;\s*\{\s*std::lock_guard lock\(control_mutex_\);\s*mode = control_mode_;/);
  assert.match(cmake, /managed-readiness-probe\.mjs/);
  assert.match(cmake, /CMAKE_MSVC_RUNTIME_LIBRARY/);
  assert.match(wasmSource,
    /EM_JS\(void, aliasQueryAdd,\s*\(const char\* query, const char\* alias, uint32_t peer, uint32_t token\)/);
  assert.match(wasmSource,
    /Module\.aliasQueryAdd\(UTF8ToString\(query\), UTF8ToString\(alias\), peer, token\)/);
  assert.match(wasmSource,
    /EM_JS\(void, aliasQueryEnd,\s*\(const char\* query, uint32_t token\)/);
  assert.match(wasmSource,
    /Module\.onAliasQueryEnd\(UTF8ToString\(query\), token\)/);
  assert.match(wasmSource, /queryAliases\(const char\* cquery, uint32_t token\)/);
});

test("native gateway keeps WebView2 foreground without production browser flags", () => {
  const source = readFileSync(new URL("../src/main.cpp", import.meta.url), "utf8");
  const protocol = readFileSync(
    new URL("../src/webview_bridge_protocol.cpp", import.meta.url), "utf8");
  assert.match(source, /parse_cadence_message\(message\)/);
  assert.match(source, /GATEWAY stats/);
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

test("native gateway maps resolution failures and established close states", () => {
  const source = readFileSync(new URL("../src/main.cpp", import.meta.url), "utf8");
  assert.match(source, /std::string_view transport_failure_stage\(std::string_view reason\)/);
  for (const reason of ["peer_bind_failed", "alias_timeout", "duplicate_remote_peer"]) {
    assert.match(source, new RegExp(`reason == "${reason}"`));
  }
  assert.match(source,
    /emit_failed\(state->target_id, transport_failure_stage\(state->reason\), state->reason\)/);
  assert.match(source, /state->value == "room-closed"\) emit_closed\(state->target_id, state->reason\)/);
  assert.match(source, /emit_failed\(1, transport_failure_stage\(\*reason\), \*reason\)/);
});

test("keeps v1 startup dispatch alongside HUB/2 control dispatch", () => {
  const html = readFileSync(new URL("../assets/transport.html", import.meta.url), "utf8");
  const source = readFileSync(new URL("../src/main.cpp", import.meta.url), "utf8");
  assert.match(html, /value\?\.version === 1/);
  assert.match(html, /startLegacy\(value\)/);
  assert.match(html, /value\?\.type === "put"/);
  assert.match(html, /value\?\.type === "drop"/);
  assert.match(html, /value\?\.type === "ice"/);
  assert.ok(source.indexOf("bind_target_socket(1)") <
    source.indexOf("post_legacy_configuration(std::move(legacy))"));
  assert.match(source, /if \(legacy_mode_\) \{\s*failed_ = true;/);
});

function createTransportPageHarness() {
  const html = readFileSync(new URL("../assets/transport.html", import.meta.url), "utf8");
  const script = html.slice(html.indexOf('<script type="module">') + '<script type="module">'.length)
    .replace(/<\/script>\s*$/, "")
    .replace(/import WebRTCNet from "\.\/webrtcnet\.mjs";\n/, "")
    .replace(/import \{[\s\S]*?\} from "\.\/transport_protocol\.mjs";\n/, "")
    .replace(/import \{ ManagedReadinessProbe \} from "\.\/managed-readiness-probe\.mjs";\n/, "");
  const posts = [];
  const pumpTimers = [];
  const probeTimers = [];
  const receiveQueue = [];
  const calls = [];
  const virtualPeers = new Map();
  const resolvedPeers = new Map();
  let nextVirtualPeer = 0x80000001;
  let now = 100;
  const memory = new ArrayBuffer(64 * 1024);
  let nextAllocation = 8;
  const runtime = {
    HEAPU8: new Uint8Array(memory),
    HEAP32: new Int32Array(memory),
    _malloc(size) {
      const pointer = nextAllocation;
      nextAllocation += size;
      return pointer;
    },
    cwrap(name) {
      if (name === "setIceServers") return (value) => {
        calls.push([name, value]);
        return 1;
      };
      if (name === "connectTo") return (...args) => {
        calls.push([name, ...args]);
        return 1;
      };
      if (name === "wait") return () => {};
      if (name === "virtualPeerForAlias") return (alias) => {
        calls.push([name, alias]);
        if (!virtualPeers.has(alias)) virtualPeers.set(alias, nextVirtualPeer++);
        return virtualPeers.get(alias);
      };
      if (name === "resolvedPeerForVirtual") return (peer) => {
        calls.push([name, peer]);
        return resolvedPeers.get(peer) ?? 0;
      };
      if (name === "queryAliases") return (query, token) => calls.push([name, query, token]);
      if (name === "sendto") return (pointer, length, peer) => {
        calls.push([name, peer, runtime.HEAPU8.slice(pointer, pointer + length)]);
        return length;
      };
      if (name === "recvfrom") return (pointer, length, peerPointer) => {
        const packet = receiveQueue.shift();
        if (!packet) return 0;
        runtime.HEAP32[peerPointer >> 2] = packet.peer;
        if (packet.length !== undefined) return packet.length;
        runtime.HEAPU8.set(packet.data.subarray(0, length), pointer);
        return packet.data.length;
      };
      if (name === "disconnect") return (peer) => calls.push([name, peer]);
      if (name === "shutdown") return () => calls.push([name]);
      throw new Error(`unexpected cwrap ${name}`);
    },
  };
  function fakeWebRTCNet() {
    return Promise.resolve(runtime);
  }
  const fakeSetTimeout = (callback, delay) => {
    const timer = { callback, delay, active: true };
    probeTimers.push(timer);
    return timer;
  };
  const fakeClearTimeout = (timer) => { if (timer) timer.active = false; };
  function TestProbe(options) {
    return new ManagedReadinessProbe({
      ...options, setTimer: fakeSetTimeout, clearTimer: fakeClearTimeout,
    });
  }
  const webview = {
    addEventListener(_name, callback) { this.listener = callback; },
    postMessage(value) { posts.push(value); },
  };
  const context = {
    WebRTCNet: fakeWebRTCNet,
    ManagedReadinessProbe: TestProbe,
    bytesToMessage,
    classifyIncomingDatagram: transportProtocol.classifyIncomingDatagram,
    createCadenceCounter,
    createFairReceiveScheduler,
    exactAliasQuery,
    maxDatagram: transportProtocol.maxDatagram,
    maxTargets: transportProtocol.maxTargets,
    maxPumpPacketsPerTick,
    messageToBytes,
    normalizeIceGeneration,
    normalizeTargetId,
    parseStartMessage,
    parseTargetMessage: transportProtocol.parseTargetMessage,
    reliableSendMode,
    chrome: { webview },
    window: {},
    performance: { now: () => now },
    setInterval(callback) { pumpTimers.push(callback); return callback; },
    clearInterval() {},
    setTimeout: fakeSetTimeout,
    clearTimeout: fakeClearTimeout,
    atob,
    btoa,
    TextEncoder,
    TextDecoder,
  };
  vm.runInNewContext(script, context, { filename: "transport.html" });
  assert.equal(JSON.stringify(posts.shift()),
    JSON.stringify({ type: "state", value: "page-ready", reason: "" }));
  return {
    calls,
    posts,
    probeTimers,
    receiveQueue,
    pumpTimers,
    resolvedPeers,
    virtualPeers,
    webview,
    advanceTime(milliseconds) { now += milliseconds; },
    now() { return now; },
    async flush() {
      await new Promise((resolve) => setImmediate(resolve));
      await new Promise((resolve) => setImmediate(resolve));
    },
  };
}

async function runLegacyPageAdmission() {
  const harness = createTransportPageHarness();
  const { calls, posts, pumpTimers, receiveQueue, resolvedPeers, virtualPeers, webview } = harness;
  const routePeer = 0x80000001;
  virtualPeers.set("q2.alpha", routePeer);
  resolvedPeers.set(routePeer, 77);
  webview.listener({ data: {
    version: 1,
    roomId: "room.q2.example.1",
    alias: "q2.alpha",
    ordered: false,
    iceServers: [{ urls: ["stun:example.invalid"] }],
  } });
  await harness.flush();
  assert.equal(calls[0][0], "setIceServers");
  assert.equal(calls[1][0], "connectTo");
  assert.equal(pumpTimers.length, 1);
  pumpTimers[0]();
  assert.equal(calls.filter(([name]) => name === "queryAliases").length, 0);
  assert.ok(calls.some(([name, alias]) => name === "virtualPeerForAlias" && alias === "q2.alpha"));
  assert.ok(calls.some(([name, peer]) => name === "resolvedPeerForVirtual" && peer === routePeer));
  receiveQueue.push({ peer: routePeer, data: quakeOob("ack") });
  pumpTimers[0]();
  const ready = posts.find((value) => value.value === "transport-ready");
  assert.equal(JSON.stringify(ready), JSON.stringify({
    type: "state", value: "transport-ready", targetId: "1", peer: 77,
  }));
  return { calls, posts };
}

test("v1 page dispatch admits a legacy target and emits tagged ready state", async () => {
  const result = await runLegacyPageAdmission();
  assert.ok(result.calls.some(([name, peer]) => name === "sendto" && peer === 0x80000001));
});

test("Task 7A resolves a target through a virtual route without AliasQuery", async () => {
  const harness = createTransportPageHarness();
  const { calls, pumpTimers, resolvedPeers, virtualPeers, webview } = harness;
  const alias = "q2.virtual";
  const routePeer = 0x80000001;
  virtualPeers.set(alias, routePeer);
  resolvedPeers.set(routePeer, 77);
  webview.listener({ data: {
    type: "ice", generation: "1", iceServers: [{ urls: ["stun:example.invalid"] }],
  } });
  await harness.flush();
  webview.listener({ data: {
    type: "put", targetId: "1", config: { alias, ordered: false },
  } });
  await harness.flush();
  pumpTimers[0]();

  assert.equal(calls.filter(([name]) => name === "queryAliases").length, 0);
  assert.deepEqual(calls.filter(([name]) => name === "virtualPeerForAlias"), [
    ["virtualPeerForAlias", alias],
  ]);
  assert.ok(calls.some(([name, peer]) => name === "sendto" && peer === routePeer));
  assert.ok(calls.some(([name, peer]) => name === "resolvedPeerForVirtual" && peer === routePeer));
});

test("unresolved targets remain resolving until the monotonic 30-second deadline", async () => {
  const harness = createTransportPageHarness();
  const { calls, posts, pumpTimers, resolvedPeers, virtualPeers, webview } = harness;
  const alias = "q2.resolve-deadline";
  const routePeer = 0x80000001;
  virtualPeers.set(alias, routePeer);
  webview.listener({ data: {
    type: "ice", generation: "1", iceServers: [{ urls: ["stun:example.invalid"] }],
  } });
  await harness.flush();
  webview.listener({ data: {
    type: "put", targetId: "1", config: { alias, ordered: false },
  } });
  await harness.flush();

  pumpTimers[0]();
  assert.equal(posts.some((value) => value.value === "failed"), false);
  harness.advanceTime(29999);
  pumpTimers[0]();
  assert.equal(posts.some((value) => value.value === "failed"), false);
  harness.advanceTime(1);
  pumpTimers[0]();
  assert.deepEqual(posts.filter((value) => value.value === "failed")
    .map(({ targetId, reason }) => [targetId, reason]), [["1", "alias_timeout"]]);
  assert.ok(calls.some(([name, peer]) => name === "disconnect" && peer === routePeer));
  assert.equal(resolvedPeers.has(routePeer), false);
});

test("pre-concrete probe misses keep a target resolving and retry the virtual route", async () => {
  const harness = createTransportPageHarness();
  const { calls, posts, probeTimers, pumpTimers, virtualPeers, webview } = harness;
  const alias = "q2.pre-concrete-miss";
  const routePeer = 0x80000001;
  virtualPeers.set(alias, routePeer);
  webview.listener({ data: {
    type: "ice", generation: "1", iceServers: [{ urls: ["stun:example.invalid"] }],
  } });
  await harness.flush();
  webview.listener({ data: {
    type: "put", targetId: "1", config: { alias, ordered: false },
  } });
  await harness.flush();
  pumpTimers[0]();
  const initialSends = calls.filter(([name]) => name === "sendto").length;
  for (let miss = 0; miss < 3; ++miss) {
    harness.advanceTime(5000);
    probeTimers.at(-1).callback();
  }

  assert.equal(posts.some((value) => value.value === "failed" || value.value === "room-closed"), false);
  assert.ok(calls.filter(([name]) => name === "sendto").length > initialSends);
  assert.equal(calls.some(([name, peer]) => name === "disconnect" && peer === routePeer), false);
});

test("negative receive stays resolving before concrete reservation", async () => {
  const harness = createTransportPageHarness();
  const { calls, posts, pumpTimers, receiveQueue, virtualPeers, webview } = harness;
  const alias = "q2.pre-concrete-close";
  const routePeer = 0x80000001;
  virtualPeers.set(alias, routePeer);
  webview.listener({ data: {
    type: "ice", generation: "1", iceServers: [{ urls: ["stun:example.invalid"] }],
  } });
  await harness.flush();
  webview.listener({ data: {
    type: "put", targetId: "1", config: { alias, ordered: false },
  } });
  await harness.flush();
  pumpTimers[0]();
  harness.advanceTime(1);
  receiveQueue.push({ peer: routePeer, length: -1 });
  pumpTimers[0]();

  assert.equal(posts.some((value) => value.value === "failed" || value.value === "room-closed"), false);
  assert.equal(calls.some(([name, peer]) => name === "disconnect" && peer === routePeer), false);
});

test("negative receive closes an established target as transport_closed", async () => {
  const harness = createTransportPageHarness();
  const { calls, posts, pumpTimers, receiveQueue, resolvedPeers, virtualPeers, webview } = harness;
  const alias = "q2.established-close";
  const routePeer = 0x80000001;
  virtualPeers.set(alias, routePeer);
  resolvedPeers.set(routePeer, 77);
  webview.listener({ data: {
    type: "ice", generation: "1", iceServers: [{ urls: ["stun:example.invalid"] }],
  } });
  await harness.flush();
  webview.listener({ data: {
    type: "put", targetId: "1", config: { alias, ordered: false },
  } });
  await harness.flush();
  pumpTimers[0]();
  harness.advanceTime(1);
  receiveQueue.push({ peer: routePeer, length: -1 });
  pumpTimers[0]();

  assert.deepEqual(posts.filter((value) => value.value === "room-closed")
    .map(({ targetId, reason }) => [targetId, reason]), [["1", "transport_closed"]]);
  assert.equal(posts.some((value) => value.value === "failed"), false);
  assert.ok(calls.some(([name, peer]) => name === "disconnect" && peer === routePeer));
});

test("initial unresolved AliasLookup ownership is serialized while ready routes continue", async () => {
  const harness = createTransportPageHarness();
  const { calls, pumpTimers, resolvedPeers, virtualPeers, webview } = harness;
  const firstRoute = 0x80000001;
  const secondRoute = 0x80000002;
  virtualPeers.set("q2.serial-first", firstRoute);
  virtualPeers.set("q2.serial-second", secondRoute);
  webview.listener({ data: {
    type: "ice", generation: "1", iceServers: [{ urls: ["stun:example.invalid"] }],
  } });
  await harness.flush();
  for (const [targetId, alias] of [["1", "q2.serial-first"], ["2", "q2.serial-second"]]) {
    webview.listener({ data: { type: "put", targetId, config: { alias, ordered: false } } });
  }
  await harness.flush();

  pumpTimers[0]();
  assert.deepEqual(calls.filter(([name]) => name === "virtualPeerForAlias"), [
    ["virtualPeerForAlias", "q2.serial-first"],
  ]);

  resolvedPeers.set(firstRoute, 77);
  harness.advanceTime(1);
  pumpTimers[0]();
  assert.deepEqual(calls.filter(([name]) => name === "virtualPeerForAlias"), [
    ["virtualPeerForAlias", "q2.serial-first"],
    ["virtualPeerForAlias", "q2.serial-second"],
  ]);
  assert.equal(calls.filter(([name]) => name === "sendto").length, 2);
  pumpTimers[0]();
  assert.equal(calls.filter(([name]) => name === "virtualPeerForAlias").length, 2);
});

test("queued targets start their resolve deadline only after route allocation", async () => {
  const harness = createTransportPageHarness();
  const { calls, posts, pumpTimers, receiveQueue, resolvedPeers, virtualPeers, webview } = harness;
  const firstRoute = 0x80000001;
  const secondRoute = 0x80000002;
  virtualPeers.set("q2.queued-first", firstRoute);
  virtualPeers.set("q2.queued-second", secondRoute);
  webview.listener({ data: {
    type: "ice", generation: "1", iceServers: [{ urls: ["stun:example.invalid"] }],
  } });
  await harness.flush();
  for (const [targetId, alias] of [["1", "q2.queued-first"], ["2", "q2.queued-second"]]) {
    webview.listener({ data: { type: "put", targetId, config: { alias, ordered: false } } });
  }
  await harness.flush();

  pumpTimers[0]();
  harness.advanceTime(30000);
  pumpTimers[0]();
  assert.deepEqual(posts.filter((value) => value.value === "failed")
    .map(({ targetId, reason }) => [targetId, reason]), [["1", "alias_timeout"]]);
  assert.deepEqual(calls.filter(([name]) => name === "virtualPeerForAlias"), [
    ["virtualPeerForAlias", "q2.queued-first"],
    ["virtualPeerForAlias", "q2.queued-second"],
  ]);

  harness.advanceTime(29999);
  pumpTimers[0]();
  assert.deepEqual(posts.filter((value) => value.value === "failed")
    .map(({ targetId, reason }) => [targetId, reason]), [["1", "alias_timeout"]]);
  resolvedPeers.set(secondRoute, 77);
  pumpTimers[0]();
  receiveQueue.push({ peer: secondRoute, data: quakeOob("ack") });
  pumpTimers[0]();
  assert.deepEqual(posts.filter((value) => value.value === "transport-ready")
    .map(({ targetId, peer }) => [targetId, peer]), [["2", 77]]);
});

test("synthetic Node vm HUB/2 page routes 40 targets and keeps a DROP survivor", async () => {
  const harness = createTransportPageHarness();
  const { calls, posts, pumpTimers, receiveQueue, webview } = harness;
  const aliases = Array.from({ length: 40 }, (_, index) =>
    index % 2 === 0 ? `q2.room.${index + 1}` : `room.q2ded.room.${index + 1}`);
  const routePeers = aliases.map((_, index) => 0x80000001 + index);
  const peers = aliases.map((_, index) => 100 + index + 1);
  const iceServers = [{ urls: ["stun:example.invalid"] }];
  for (let index = 0; index < aliases.length; ++index) {
    harness.virtualPeers.set(aliases[index], routePeers[index]);
    harness.resolvedPeers.set(routePeers[index], peers[index]);
  }

  webview.listener({ data: {
    type: "ice", generation: "9007199254740993", iceServers,
  } });
  await harness.flush();
  for (let index = 0; index < aliases.length; ++index) {
    webview.listener({ data: {
      type: "put", targetId: String(index + 1),
      config: { alias: aliases[index], ordered: false },
    } });
  }
  await harness.flush();

  assert.equal(pumpTimers.length, 1);
  assert.equal(calls[0][0], "setIceServers");
  assert.equal(calls[0][1], JSON.stringify(iceServers));
  assert.equal(calls[1][0], "connectTo");
  pumpTimers[0]();
  assert.equal(calls.filter(([name]) => name === "queryAliases").length, 0);
  assert.deepEqual(calls.filter(([name]) => name === "virtualPeerForAlias")
    .map(([, alias]) => alias), aliases);
  assert.deepEqual(calls.filter(([name]) => name === "resolvedPeerForVirtual")
    .map(([, peer]) => peer), routePeers);
  assert.equal(calls.filter(([name]) => name === "sendto").length, 40);

  // Put an ACK and a uniquely tagged payload beside each other for every
  // target.  The production scheduler must deliver each ACK before its
  // payload while its bounded scan splits the 80-packet burst over ticks.
  for (let index = 0; index < aliases.length; ++index) {
    receiveQueue.push({ peer: routePeers[index], data: quakeOob("ack") });
    receiveQueue.push({
      peer: routePeers[index], data: Uint8Array.of(index + 1, 255 - index),
    });
  }
  pumpTimers[0]();
  pumpTimers[0]();

  const ready = posts.filter((value) => value.value === "transport-ready");
  assert.equal(ready.length, 40);
  assert.deepEqual(ready.map(({ targetId, peer }) => [targetId, peer]).sort(
    ([left], [right]) => Number(left) - Number(right)),
    peers.map((peer, index) => [String(index + 1), peer]));
  const routed = posts.filter((value) => value.type === "udp");
  assert.equal(routed.length, 40);
  assert.equal(JSON.stringify(routed.find(({ targetId }) => targetId === "1")),
    `{"type":"udp","targetId":"1","data":"${bytesToMessage(Uint8Array.of(1, 255)).data}"}`,
    "the production sender uses the native decoder field order");
  assert.deepEqual(routed.map(({ targetId, data }) => [targetId, data]).sort(
    ([left], [right]) => Number(left) - Number(right)),
    aliases.map((_, index) => [
      String(index + 1), bytesToMessage(Uint8Array.of(index + 1, 255 - index)).data,
    ]));

  const beforeDrop = posts.length;
  webview.listener({ data: { type: "drop", targetId: "1" } });
  await harness.flush();
  assert.ok(calls.some(([name, peer]) => name === "disconnect" && peer === routePeers[0]));
  assert.equal(calls.some(([name, peer]) => name === "disconnect" && peer === peers[0]), false);
  receiveQueue.push({ peer: routePeers[0], data: Uint8Array.of(0xd1) });
  receiveQueue.push({ peer: routePeers[1], data: Uint8Array.of(0xd2) });
  pumpTimers[0]();
  const survivor = posts.slice(beforeDrop).filter((value) => value.type === "udp");
  assert.deepEqual(survivor.map(({ type, targetId, data }) => [type, targetId, data]), [[
    "udp", "2", bytesToMessage(Uint8Array.of(0xd2)).data,
  ]]);
  assert.ok(posts.slice(beforeDrop).some((value) =>
    value.type === "drop" && value.reason === "unknown_peer"));
});

test("production page holds an early ACK until concrete reservation", async () => {
  const harness = createTransportPageHarness();
  const { calls, posts, pumpTimers, receiveQueue, resolvedPeers, virtualPeers, webview } = harness;
  const alias = "q2.early-ack";
  const routePeer = 0x80000001;
  virtualPeers.set(alias, routePeer);
  webview.listener({ data: {
    type: "ice", generation: "1", iceServers: [{ urls: ["stun:example.invalid"] }],
  } });
  await harness.flush();
  webview.listener({ data: {
    type: "put", targetId: "1", config: { alias, ordered: false },
  } });
  await harness.flush();
  pumpTimers[0]();
  receiveQueue.push({ peer: routePeer, data: quakeOob("ack") });
  pumpTimers[0]();
  assert.equal(posts.filter((value) => value.value === "transport-ready").length, 0);
  assert.ok(calls.some(([name, peer]) => name === "resolvedPeerForVirtual" && peer === routePeer));

  resolvedPeers.set(routePeer, 77);
  pumpTimers[0]();
  assert.deepEqual(posts.filter((value) => value.value === "transport-ready")
    .map(({ targetId, peer }) => [targetId, peer]), [["1", 77]]);
  assert.ok(calls.filter(([name]) => name === "sendto").every(([, peer]) => peer === routePeer));
  assert.equal(calls.filter(([name]) => name === "queryAliases").length, 0);
});

test("duplicate concrete resolution fails only the newcomer", async () => {
  const harness = createTransportPageHarness();
  const { calls, posts, pumpTimers, receiveQueue, resolvedPeers, virtualPeers, webview } = harness;
  const firstRoute = 0x80000001;
  const secondRoute = 0x80000002;
  virtualPeers.set("q2.first", firstRoute);
  virtualPeers.set("q2.second", secondRoute);
  resolvedPeers.set(firstRoute, 0);
  resolvedPeers.set(secondRoute, 77);
  webview.listener({ data: {
    type: "ice", generation: "1", iceServers: [{ urls: ["stun:example.invalid"] }],
  } });
  await harness.flush();
  webview.listener({ data: {
    type: "put", targetId: "1", config: { alias: "q2.first", ordered: false },
  } });
  webview.listener({ data: {
    type: "put", targetId: "2", config: { alias: "q2.second", ordered: false },
  } });
  await harness.flush();
  pumpTimers[0]();
  assert.deepEqual(calls.filter(([name]) => name === "virtualPeerForAlias"), [
    ["virtualPeerForAlias", "q2.first"],
  ]);
  resolvedPeers.set(firstRoute, 77);
  harness.advanceTime(1);
  pumpTimers[0]();
  assert.deepEqual(posts.filter((value) => value.value === "failed")
    .map(({ targetId, reason }) => [targetId, reason]), [["2", "duplicate_remote_peer"]]);
  assert.ok(calls.some(([name, peer]) => name === "disconnect" && peer === secondRoute));
  assert.equal(calls.some(([name, peer]) => name === "disconnect" && peer === firstRoute), false);

  receiveQueue.push({ peer: firstRoute, data: quakeOob("ack") });
  pumpTimers[0]();
  assert.deepEqual(posts.filter((value) => value.value === "transport-ready")
    .map(({ targetId, peer }) => [targetId, peer]), [["1", 77]]);
});

test("stale virtual route traffic cannot bind a replacement target", async () => {
  const harness = createTransportPageHarness();
  const { calls, posts, pumpTimers, receiveQueue, resolvedPeers, virtualPeers, webview } = harness;
  const alias = "q2.stale";
  const oldRoute = 0x80000001;
  const newRoute = 0x80000002;
  virtualPeers.set(alias, oldRoute);
  resolvedPeers.set(oldRoute, 11);
  webview.listener({ data: {
    type: "ice", generation: "1", iceServers: [{ urls: ["stun:example.invalid"] }],
  } });
  await harness.flush();
  webview.listener({ data: {
    type: "put", targetId: "1", config: { alias, ordered: false },
  } });
  await harness.flush();
  pumpTimers[0]();
  webview.listener({ data: { type: "drop", targetId: "1" } });
  await harness.flush();
  assert.ok(calls.some(([name, peer]) => name === "disconnect" && peer === oldRoute));

  virtualPeers.set(alias, newRoute);
  resolvedPeers.set(newRoute, 22);
  webview.listener({ data: {
    type: "put", targetId: "2", config: { alias, ordered: false },
  } });
  await harness.flush();
  pumpTimers[0]();
  receiveQueue.push({ peer: oldRoute, data: quakeOob("ack") });
  receiveQueue.push({ peer: newRoute, data: quakeOob("ack") });
  pumpTimers[0]();
  assert.deepEqual(posts.filter((value) => value.value === "transport-ready")
    .map(({ targetId, peer }) => [targetId, peer]), [["2", 22]]);
  assert.ok(posts.some((value) => value.type === "drop" && value.reason === "unknown_peer"));
});

test("production page clears a failed route before same-id retry", async () => {
  const harness = createTransportPageHarness();
  const { calls, posts, pumpTimers, receiveQueue, resolvedPeers, virtualPeers, webview } = harness;
  const alias = "q2.retry";
  const routePeer = 0x80000001;
  virtualPeers.set(alias, 0);
  webview.listener({ data: {
    type: "ice", generation: "1", iceServers: [{ urls: ["stun:example.invalid"] }],
  } });
  await harness.flush();
  webview.listener({ data: {
    type: "put", targetId: "1", config: { alias, ordered: false },
  } });
  await harness.flush();
  pumpTimers[0]();
  assert.ok(posts.some((value) => value.type === "state" && value.value === "failed" &&
    value.targetId === "1" && value.reason === "peer_bind_failed"));

  virtualPeers.set(alias, routePeer);
  resolvedPeers.set(routePeer, 22);
  webview.listener({ data: {
    type: "put", targetId: "1", config: { alias, ordered: false },
  } });
  await harness.flush();
  pumpTimers[0]();
  receiveQueue.push({ peer: routePeer, data: quakeOob("ack") });
  pumpTimers[0]();
  assert.deepEqual(posts.filter((value) => value.value === "transport-ready")
    .map(({ targetId, peer }) => [targetId, peer]), [["1", 22]]);
  assert.equal(calls.filter(([name]) => name === "virtualPeerForAlias").length, 2);
});

test("production page normalizes signed virtual route ids", async () => {
  const harness = createTransportPageHarness();
  const { calls, posts, pumpTimers, receiveQueue, resolvedPeers, virtualPeers, webview } = harness;
  const signedRoute = -2147483647;
  const routePeer = signedRoute >>> 0;
  const alias = "q2.signed-route";
  virtualPeers.set(alias, signedRoute);
  resolvedPeers.set(routePeer, 801);
  webview.listener({ data: {
    type: "ice", generation: "1", iceServers: [{ urls: ["stun:example.invalid"] }],
  } });
  await harness.flush();
  webview.listener({ data: {
    type: "put", targetId: "1", config: { alias, ordered: false },
  } });
  await harness.flush();
  pumpTimers[0]();
  receiveQueue.push({ peer: routePeer, data: quakeOob("ack") });
  pumpTimers[0]();
  assert.deepEqual(posts.filter((value) => value.value === "transport-ready")
    .map(({ targetId, peer }) => [targetId, peer]), [["1", 801]]);
  assert.ok(calls.some(([name, peer]) => name === "sendto" && peer === routePeer));
});
