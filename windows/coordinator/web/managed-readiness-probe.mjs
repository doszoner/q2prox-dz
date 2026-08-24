const OOB = new Uint8Array([0xff, 0xff, 0xff, 0xff]);
const encoder = new TextEncoder();

export function quakeOob(command) {
  const body = encoder.encode(`${command}\n`);
  const result = new Uint8Array(OOB.length + body.length);
  result.set(OOB, 0);
  result.set(body, OOB.length);
  return result;
}

export function quakeOobCommand(packet) {
  const p = packet instanceof Uint8Array ? packet : new Uint8Array(packet);
  if (p.length < 5 || p[0] !== 0xff || p[1] !== 0xff || p[2] !== 0xff || p[3] !== 0xff) return null;
  let end = 4;
  while (end < p.length && p[end] !== 0 && p[end] !== 10 && p[end] !== 13) end++;
  try { return new TextDecoder('ascii', { fatal: true }).decode(p.subarray(4, end)).trim(); }
  catch { return null; }
}

export class ManagedReadinessProbe {
  constructor({send, onReady, onRoomClosed,
               setTimer = (callback, delay) => setTimeout(callback, delay),
               clearTimer = (timer) => clearTimeout(timer),
               initialTimeoutMs = 5000, intervalMs = 5000, retryMs = 50, maxMisses = 3}) {
    if (typeof send !== 'function' || typeof onReady !== 'function' || typeof onRoomClosed !== 'function') throw new TypeError('callbacks are required');
    this.send = send; this.onReady = onReady; this.onRoomClosed = onRoomClosed;
    this.setTimer = setTimer; this.clearTimer = clearTimer;
    this.initialTimeoutMs = initialTimeoutMs; this.intervalMs = intervalMs; this.retryMs = retryMs; this.maxMisses = maxMisses;
    this.timer = null; this.pending = false; this.ready = false; this.closed = false; this.misses = 0;
  }
  channelOpened() { if (!this.closed && !this.pending) this.#probe(this.initialTimeoutMs); }
  packetReceived(packet) {
    if (this.closed) return false;
    if (this.pending && quakeOobCommand(packet) === 'ack') {
      this.pending = false; this.misses = 0; if (this.timer !== null) this.clearTimer(this.timer);
      if (!this.ready) { this.ready = true; this.onReady(); }
      this.timer = this.setTimer(() => this.#probe(this.initialTimeoutMs), this.intervalMs);
      return true;
    }
    if (this.ready) {
      this.pending = false; this.misses = 0; if (this.timer !== null) this.clearTimer(this.timer);
      this.timer = this.setTimer(() => this.#probe(this.initialTimeoutMs), this.intervalMs);
    }
    return false;
  }
  channelClosed(reason = 'data_channel_closed') { if (!this.closed) { this.closed = true; if (this.timer !== null) this.clearTimer(this.timer); this.onRoomClosed(reason); } }
  stop() { this.closed = true; if (this.timer !== null) this.clearTimer(this.timer); }
  #probe(timeout) {
    if (this.closed) return;
    let sent = false;
    try { sent = this.send(quakeOob('ping')) !== false; } catch {}
    if (!sent) {
      this.pending = false;
      if (this.ready) {
        this.misses++;
        if (this.misses >= this.maxMisses) { this.channelClosed('probe_timeout'); return; }
      }
      this.timer = this.setTimer(() => this.#probe(timeout), this.retryMs);
      return;
    }
    this.pending = true;
    this.timer = this.setTimer(() => {
      if (this.closed || !this.pending) return;
      this.pending = false; this.misses++;
      if (this.misses >= this.maxMisses) this.channelClosed('probe_timeout'); else this.#probe(timeout);
    }, timeout);
  }
}
