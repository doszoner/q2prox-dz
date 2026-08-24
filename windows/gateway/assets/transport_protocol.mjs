const aliasPattern = /^[A-Za-z0-9._-]{1,255}$/;
export const maxDatagram = 1400;
export const reliableSendMode = 0;

export function createCadenceCounter() {
  const interval = {
    timerTicks: 0,
    timerMaxGapMs: 0,
    waitCalls: 0,
    aliasPollIterations: 0,
    probeSendCount: 0,
    probeAckCount: 0,
    dcToNativeDatagrams: 0,
    dcToNativeBytes: 0,
    dcToNativeMaxGapMs: 0,
    nativeToDcDatagrams: 0,
    nativeToDcBytes: 0,
    nativeToDcMaxGapMs: 0,
  };
  let lastTimer;
  let lastDcToNative;
  let lastNativeToDc;

  function gap(now, previous) {
    return previous === undefined ? 0 : Math.max(0, Math.round(now - previous));
  }

  return Object.freeze({
    timerTick(now = performance.now()) {
      ++interval.timerTicks;
      interval.timerMaxGapMs = Math.max(interval.timerMaxGapMs, gap(now, lastTimer));
      lastTimer = now;
    },
    waitCall() { ++interval.waitCalls; },
    aliasPoll() { ++interval.aliasPollIterations; },
    probeSend() { ++interval.probeSendCount; },
    probeAck() { ++interval.probeAckCount; },
    dcToNative(bytes, now = performance.now()) {
      ++interval.dcToNativeDatagrams;
      interval.dcToNativeBytes += bytes;
      interval.dcToNativeMaxGapMs = Math.max(
        interval.dcToNativeMaxGapMs, gap(now, lastDcToNative));
      lastDcToNative = now;
    },
    nativeToDc(bytes, now = performance.now()) {
      ++interval.nativeToDcDatagrams;
      interval.nativeToDcBytes += bytes;
      interval.nativeToDcMaxGapMs = Math.max(
        interval.nativeToDcMaxGapMs, gap(now, lastNativeToDc));
      lastNativeToDc = now;
    },
    snapshot() {
      const result = { type: "cadence", ...interval };
      for (const field of Object.keys(interval)) interval[field] = 0;
      return result;
    },
  });
}

export function parseStartMessage(value) {
  if (!value || value.version !== 1 ||
      typeof value.roomId !== "string" || value.roomId.length < 1 || value.roomId.length > 256 ||
      !aliasPattern.test(value.alias ?? "") ||
      typeof value.ordered !== "boolean" ||
      !Array.isArray(value.iceServers) || value.iceServers.length === 0) {
    throw new Error("invalid_start_message");
  }

  const iceJson = JSON.stringify(value.iceServers);
  if (iceJson.length > 65536) throw new Error("ice_configuration_too_large");
  for (const server of value.iceServers) {
    const urls = typeof server?.urls === "string" ? [server.urls] : server?.urls;
    if (!Array.isArray(urls) || urls.length === 0 ||
        urls.some((url) => typeof url !== "string" || url.length === 0)) {
      throw new Error("invalid_ice_server");
    }
  }

  return Object.freeze({
    alias: value.alias,
    ordered: value.ordered,
    iceServers: JSON.parse(iceJson),
  });
}

export function exactAliasQuery(alias) {
  if (!aliasPattern.test(alias)) throw new Error("invalid_alias");
  return `=${alias}`;
}

export function classifyIncomingDatagram(length) {
  if (!Number.isInteger(length) || length < 1) return "invalid";
  return length > maxDatagram ? "oversized" : "accepted";
}

export function bytesToMessage(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.length < 1 || bytes.length > maxDatagram) {
    throw new Error("invalid_datagram_size");
  }
  return {
    type: "udp",
    data: btoa(String.fromCharCode(...bytes)),
  };
}

export function messageToBytes(message) {
  if (!message || message.type !== "udp" || typeof message.data !== "string" ||
      message.data.length === 0 || message.data.length % 4 !== 0 ||
      !/^[A-Za-z0-9+/]*={0,2}$/.test(message.data)) {
    throw new Error("invalid_udp_message");
  }
  const binary = atob(message.data);
  if (btoa(binary) !== message.data || binary.length < 1 || binary.length > maxDatagram) {
    throw new Error("invalid_udp_payload");
  }
  return Uint8Array.from(binary, (character) => character.charCodeAt(0));
}
