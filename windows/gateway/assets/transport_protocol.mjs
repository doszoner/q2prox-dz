const aliasPattern = /^[A-Za-z0-9._-]{1,255}$/;
export const maxDatagram = 1400;
export const maxTargets = 256;
export const maxPumpPacketsPerTick = 64;
export const maxDeliveriesPerPeerPerTick = 8;
export const maxQueuedPacketsPerTarget = 64;
export const reliableSendMode = 0;

const maxUint64 = 18446744073709551615n;
const canonicalTargetId = /^[1-9][0-9]*$/;

export function normalizeIceGeneration(value) {
  if (typeof value !== "string" || !canonicalTargetId.test(value)) return null;
  try {
    const parsed = BigInt(value);
    return parsed <= maxUint64 ? parsed : null;
  } catch {
    return null;
  }
}

export function normalizeTargetId(value) {
  let text;
  if (typeof value === "number") {
    if (!Number.isSafeInteger(value) || value < 1) return null;
    text = String(value);
  } else if (typeof value === "string") {
    text = value;
  } else {
    return null;
  }
  if (!canonicalTargetId.test(text)) return null;
  try {
    return BigInt(text) <= maxUint64 ? text : null;
  } catch {
    return null;
  }
}

function validTargetId(value) {
  return normalizeTargetId(value) !== null;
}

function validRemotePeerId(value) {
  return Number.isSafeInteger(value) && value > 0 && value < 0x80000000;
}

export function createTargetRegistry() {
  const targets = new Map();
  const remotePeerToTarget = new Map();
  return {
    targets,
    remotePeerToTarget,
    put({ targetId, alias, ...rest } = {}) {
      const normalizedTargetId = normalizeTargetId(targetId);
      if (!normalizedTargetId || typeof alias !== "string" ||
          !aliasPattern.test(alias) || targets.size >= maxTargets ||
          targets.has(normalizedTargetId)) {
        return false;
      }
      targets.set(normalizedTargetId, {
        ...rest, targetId: normalizedTargetId, alias, remotePeerId: 0, state: "resolving",
      });
      return true;
    },
    bindPeer(targetId, remotePeerId) {
      const normalizedTargetId = normalizeTargetId(targetId);
      const target = targets.get(normalizedTargetId);
      if (!target || !validRemotePeerId(remotePeerId)) return "invalid_peer";
      const owner = remotePeerToTarget.get(remotePeerId);
      if (owner !== undefined && owner !== normalizedTargetId) return "duplicate_remote_peer";
      if (target.remotePeerId !== 0) remotePeerToTarget.delete(target.remotePeerId);
      target.remotePeerId = remotePeerId;
      target.state = "probing";
      remotePeerToTarget.set(remotePeerId, normalizedTargetId);
      return "bound";
    },
    targetForPeer(remotePeerId) {
      return remotePeerToTarget.get(remotePeerId);
    },
    send(targetId, datagram) {
      const target = targets.get(normalizeTargetId(targetId));
      if (!target || target.remotePeerId === 0) throw new Error("transport_not_ready");
      return { ...bytesToMessage(datagram), targetId: target.targetId };
    },
    receive(remotePeerId, datagram) {
      const targetId = remotePeerToTarget.get(remotePeerId);
      if (targetId === undefined) return { type: "drop", reason: "unknown_peer" };
      return { ...bytesToMessage(datagram), targetId };
    },
    close(targetId) {
      const normalizedTargetId = normalizeTargetId(targetId);
      const target = targets.get(normalizedTargetId);
      if (!target) return false;
      if (target.remotePeerId !== 0) remotePeerToTarget.delete(target.remotePeerId);
      targets.delete(normalizedTargetId);
      return true;
    },
    pump(receive, handle, limit = maxPumpPacketsPerTick) {
      let processed = 0;
      for (; processed < Math.min(limit, maxPumpPacketsPerTick); ++processed) {
        const packet = receive();
        if (!packet) break;
        handle(this.receive(packet.peer, packet.data));
      }
      return processed;
    },
  };
}

export function createFairReceiveScheduler({
  scanLimit = maxPumpPacketsPerTick,
  deliveryLimit = maxPumpPacketsPerTick,
  perPeerLimit = maxDeliveriesPerPeerPerTick,
  queueLimit = maxQueuedPacketsPerTarget,
} = {}) {
  const queues = new Map();
  let cursor = 0;
  const bounded = (value, fallback) => Number.isInteger(value) && value > 0 ? value : fallback;

  return {
    enqueue(targetId, datagram) {
      const normalizedTargetId = normalizeTargetId(targetId);
      if (!normalizedTargetId || !(datagram instanceof Uint8Array)) return false;
      let queue = queues.get(normalizedTargetId);
      if (!queue) queues.set(normalizedTargetId, queue = []);
      if (queue.length >= bounded(queueLimit, maxQueuedPacketsPerTarget)) return false;
      queue.push(datagram);
      return true;
    },
    drop(targetId) {
      const normalizedTargetId = normalizeTargetId(targetId);
      if (!normalizedTargetId) return false;
      return queues.delete(normalizedTargetId);
    },
    pending(targetId) {
      return queues.get(normalizeTargetId(targetId))?.length ?? 0;
    },
    pump(receive, resolveTarget, handle, onDrop = () => {}, limit = deliveryLimit) {
      const scans = bounded(scanLimit, maxPumpPacketsPerTick);
      let scanned = 0;
      for (; scanned < scans; ++scanned) {
        const packet = receive();
        if (!packet) break;
        const targetId = resolveTarget(packet.peer);
        if (!targetId || !this.enqueue(targetId, packet.data)) onDrop(packet, targetId);
      }

      const ids = [...queues.keys()];
      const deliveries = bounded(limit, maxPumpPacketsPerTick);
      const perTarget = new Map();
      let delivered = 0;
      if (ids.length !== 0) {
        const peerQuota = bounded(perPeerLimit, maxDeliveriesPerPeerPerTick);
        const round = (quota) => {
          let progress = false;
          for (let offset = 0; offset < ids.length && delivered < deliveries; ++offset) {
            const index = (cursor + offset) % ids.length;
            const targetId = ids[index];
            const queue = queues.get(targetId);
            const count = perTarget.get(targetId) ?? 0;
            if (!queue || queue.length === 0 || (quota !== undefined && count >= quota)) {
              continue;
            }
            handle(targetId, queue.shift());
            perTarget.set(targetId, count + 1);
            ++delivered;
            progress = true;
          }
          return progress;
        };
        while (delivered < deliveries && round(peerQuota)) {}
        while (delivered < deliveries && round(undefined)) {}
        cursor = (cursor + 1) % ids.length;
      }
      for (const targetId of ids) {
        if (queues.get(targetId)?.length === 0) queues.delete(targetId);
      }
      return { scanned, delivered };
    },
  };
}

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

export function parseTargetMessage(value) {
  const targetId = normalizeTargetId(value?.targetId);
  if (!value || typeof value !== "object" || !targetId ||
      !aliasPattern.test(value.alias ?? "") ||
      (value.ordered !== undefined && value.ordered !== false)) {
    throw new Error("invalid_target_message");
  }
  return Object.freeze({
    targetId,
    alias: value.alias,
    ordered: value.ordered === true,
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
