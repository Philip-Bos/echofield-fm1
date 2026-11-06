// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
//
// ECHOFIELD FM-1's USB protocol, the host's side (the FM-1's: app/proto.c). Frames in the update's SysEx framing:
// F0 pack7(00 59 cmd len24 body ~sum8(body)) F7. One Link carries everything on the FM-1's port: these commands,
// the FM-1's read requests while a sample is sent (cmd 0x30, as the update loader's), its parameter notices, and the
// screen mirror's frames (handed to whoever listens).

export const CMD = { HELLO: 0x70, DIR: 0x71, UPLOAD: 0x72, ERASE: 0x73, RENAME: 0x74, PARAMS: 0x75, SET: 0x76,
                     PRESETS: 0x77, PRESET_OP: 0x78, READ: 0x79, WIPE: 0x7A, PRESET_GET: 0x7B, PRESET_PUT: 0x7C,
                     NOTIFY: 0x7F };
export const ERR = { 1: "bad request", 2: "transfer interrupted", 3: "not enough room in the bank", 7: "flash erase failed",
                     8: "flash write failed", 9: "verification failed", 10: "the FM-1 is recording", 11: "checksum mismatch",
                     12: "could not save the directory", 13: "flash not available" };
export const SLOT_STATE = ["empty", "ok", "damaged"];
const UP_OK = 0xC0000000, UP_FAIL = 0xCF000000, MAXDATA = 512, NAME_N = 12;

export function pack7(data) {
  const out = [];
  let acc = 0, nb = 0;
  for (const b of data) {
    acc |= b << nb; nb += 8;
    while (nb >= 7) { out.push(acc & 0x7F); acc >>>= 7; nb -= 7; }
  }
  if (nb) out.push(acc & 0x7F);
  return out;
}
export function unpack7(s) {
  const out = [];
  let acc = 0, nb = 0;
  for (const b of s) {
    acc |= (b & 0x7F) << nb; nb += 7;
    while (nb >= 8) { out.push(acc & 0xFF); acc >>>= 8; nb -= 8; }
  }
  return out;
}
export function frame(cmd, body = []) {
  let s = 0;
  for (const b of body) s += b;
  const n = body.length;
  return [0xF0, ...pack7([0x00, 0x59, cmd, n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF, ...body, ~s & 0xFF]), 0xF7];
}
// a frame -> {cmd, body} when its framing and checksum hold, else null
export function parse(pkt) {
  if (pkt[0] !== 0xF0 || pkt[pkt.length - 1] !== 0xF7) return null;
  const u = unpack7(pkt.slice(1, -1));
  if (u.length < 7 || u[0] !== 0 || u[1] !== 0x59) return null;
  const n = u[3] | (u[4] << 8) | (u[5] << 16);
  if (u.length < 7 + n) return null;
  let s = 0;
  for (let i = 6; i <= 6 + n; i++) s += u[i];
  if ((s & 0xFF) !== 0xFF) return null;
  return { cmd: u[2], body: u.slice(6, 6 + n) };
}
// the answer to one of the FM-1's read requests (cmd 0x30), as fm1ota.js answers the update's
export function readResponse(addr, data, fl = 0) {
  const len = data.length;
  const body = [0x00, 0x59, 0x30, (len + 8) & 0xFF, ((len + 8) >> 8) & 0xFF, 0, fl,
    addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF,
    len & 0xFF, (len >> 8) & 0xFF, 0, ...data];
  let s = 0;
  for (let i = 6; i < body.length; i++) s += body[i];
  body.push(~s & 0xFF);
  return [0xF0, ...pack7(body), 0xF7];
}
function parseReadRequest(pkt) {
  const u = unpack7(pkt.slice(1, -1));
  if (u.length !== 15 || u[0] !== 0 || u[1] !== 0x59 || u[2] !== 0x30) return null;
  let s = 0;
  for (let i = 6; i < 14; i++) s += u[i];
  if ((~s & 0xFF) !== u[14]) return null;
  return { fl: u[6], addr: (u[7] | (u[8] << 8) | (u[9] << 16) | (u[10] << 24)) >>> 0, len: u[11] | (u[12] << 8) | (u[13] << 16) };
}

const le16 = (v) => [v & 0xFF, (v >> 8) & 0xFF];
const le32 = (v) => [v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF];
const rd16 = (b, o) => b[o] | (b[o + 1] << 8);
const rd32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const str = (b, o, n) => String.fromCharCode(...b.slice(o, o + n)).replace(/\0[\s\S]*$/, "");
const name12 = (s) => { const a = new Array(NAME_N).fill(0); for (let i = 0; i < Math.min(11, s.length); i++) a[i] = s.charCodeAt(i) & 0x7F; return a; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// one in/out pair. transport: { send(bytes), onMessage(fn), close() } (Web MIDI, or a test's pipe)
export class EfLink {
  constructor(transport) {
    this.t = transport;
    this.waiters = [];                     // {cmd, resolve} for replies
    this.listeners = new Set();            // every SysEx frame (the mirror)
    this.notifyFns = new Set();            // parameter notices
    this.reqFns = null;                    // read requests, while an upload runs
    this.lost = false;
    this.chain = Promise.resolve();
    this.busy = false;
    transport.onMessage((data) => this.onData(data));
  }
  onData(data) {
    if (data[0] !== 0xF0) return;
    const pkt = Array.from(data);
    for (const fn of this.listeners) fn(pkt);
    const u = unpack7(pkt.slice(1, -1));
    if (u.length >= 3 && u[0] === 0 && u[1] === 0x59 && u[2] === 0x30) {
      const r = parseReadRequest(pkt);
      if (r && this.reqFns) this.reqFns(r);
      return;
    }
    const f = parse(pkt);
    if (!f) return;
    if (f.cmd === CMD.NOTIFY) {
      for (let i = 0; i + 3 <= f.body.length; i += 3) for (const fn of this.notifyFns) fn(f.body[i], rd16(f.body, i + 1));
      return;
    }
    const w = this.waiters.findIndex((x) => x.cmd === f.cmd);
    if (w >= 0) this.waiters.splice(w, 1)[0].resolve(f.body);
  }
  send(bytes) { try { this.t.send(bytes); return true; } catch { this.lost = true; return false; } }
  // one exchange with the FM-1 at a time: it holds one incoming frame, so a second one sent meanwhile is lost (and
  // during an upload it would overwrite a data reply). fn runs once everything sent before it is answered
  exclusive(fn) {
    const run = this.chain.then(() => { this.busy = true; return fn(); }).finally(() => { this.busy = false; });
    this.chain = run.catch(() => {});
    return run;
  }
  // a command and its reply (null: no answer). Asked again on silence: every command is safe to repeat
  ask(cmd, body = [], opts = {}) { return this.exclusive(() => this.askNow(cmd, body, opts)); }
  async askNow(cmd, body = [], { ms = 1500, tries = 3 } = {}) {
    for (let t = 0; t < tries; t++) {
      const p = new Promise((resolve) => this.waiters.push({ cmd, resolve }));
      if (!this.send(frame(cmd, body))) return null;
      const got = await Promise.race([p, sleep(ms).then(() => null)]);
      if (got) return got;
      this.waiters = this.waiters.filter((w) => w.cmd !== cmd);
    }
    return null;
  }
  close() { this.t.close(); }
}

// the FM-1 running ECHOFIELD
export class EchoField {
  constructor(link) { this.link = link; this.info = null; }

  async hello() {
    const b = await this.link.ask(CMD.HELLO);
    if (!b || b.length < 38 || str(b, 1, 4) !== "EFM1") return null;
    this.info = { proto: b[5], version: str(b, 6, 16), slots: b[22], sectors: b[23], free: b[24],
                  secSamples: rd16(b, 25), secPayload: rd16(b, 27), gen: rd32(b, 29), nparams: b[33],
                  slot: (b[34] << 24) >> 24, play: !!b[35], userPresets: b[36], factoryPresets: b[37] };
    return this.info;
  }
  async dir() {
    const b = await this.link.ask(CMD.DIR);
    if (!b || b[0] !== 0) throw new Error("the FM-1 did not send its sample list");
    const out = [];
    for (let i = 0; i < 16; i++) {
      const o = 1 + i * 20;
      out.push({ slot: i, state: SLOT_STATE[b[o]] || "empty", factory: !!b[o + 1], name: str(b, o + 2, NAME_N),
                 frames: rd32(b, o + 14), rate: rd16(b, o + 18) });
    }
    return out;
  }
  async status(cmd, body, what) {
    const b = await this.link.ask(cmd, body);
    if (!b) throw new Error(`${what}: no answer from the FM-1`);
    if (b[0]) throw new Error(`${what}: ${ERR[b[0]] || "error " + b[0]}`);
    return b;
  }
  erase(slot) { return this.status(CMD.ERASE, [slot], "delete"); }
  rename(slot, name) { return this.status(CMD.RENAME, [slot, ...name12(name)], "rename"); }
  wipe() { return this.status(CMD.WIPE, [0x57, 0x49, 0x50, 0x45], "wipe"); }
  async params() {
    const b = await this.link.ask(CMD.PARAMS);
    if (!b || b[0]) throw new Error("the FM-1 did not send its parameters");
    const n = b[1], q = [];
    for (let i = 0; i < n; i++) q.push(rd16(b, 2 + 2 * i));
    const o = 2 + 2 * n;
    return { q, slot: (b[o] << 24) >> 24, play: !!b[o + 1], freeze: !!b[o + 2], locks: b.slice(o + 3, o + 11) };
  }
  // pairs [[id, q16]...] (ids 0xF0 play, 0xF1 freeze, 0xF2 the slot)
  set(pairs) {
    const body = [];
    for (const [id, q] of pairs) body.push(id, ...le16(q));
    return this.status(CMD.SET, body, "set");
  }
  async presets() {
    const b = await this.link.ask(CMD.PRESETS);
    if (!b || b[0]) throw new Error("the FM-1 did not send its presets");
    const user = [], factory = [];
    let o = 1;
    for (let i = 0; i < (this.info?.userPresets || 16); i++, o += 1 + 2 * NAME_N)
      user.push({ index: i, used: !!b[o], name: str(b, o + 1, NAME_N), sample: str(b, o + 1 + NAME_N, NAME_N) });
    for (let i = 0; o + 16 <= b.length; i++, o += 16) factory.push({ index: i, name: str(b, o, 16) });
    return { user, factory };
  }
  presetOp(op, index, name = "") { return this.status(CMD.PRESET_OP, [op, index, ...name12(name)], "preset"); }
  async presetGet(index) {
    const b = await this.status(CMD.PRESET_GET, [index], "read preset");
    const n = b[3 + 2 * NAME_N], q = [];
    for (let i = 0; i < n; i++) q.push(rd16(b, 4 + 2 * NAME_N + 2 * i));
    return { index, used: !!b[1], slot: (b[2] << 24) >> 24, name: str(b, 3, NAME_N), sample: str(b, 3 + NAME_N, NAME_N), q };
  }
  presetPut(p) {
    return this.status(CMD.PRESET_PUT, [p.index, p.used ? 1 : 0, p.slot & 0xFF, ...name12(p.name || ""),
                                        ...name12(p.sample || ""), p.q.length, ...p.q.flatMap(le16)], "write preset");
  }
  // a sample's payload back from the FM-1 (an export, a backup)
  async read(slot, bytes, onProgress) {
    const out = new Uint8Array(bytes);
    for (let off = 0; off < bytes; off += MAXDATA) {
      const n = Math.min(MAXDATA, bytes - off);
      const b = await this.link.ask(CMD.READ, [slot, ...le32(off), ...le16(n)]);
      if (!b || b[0] || b.length !== 1 + n) throw new Error("reading the sample failed");
      out.set(b.slice(1), off);
      onProgress?.(off + n, bytes);
    }
    return out;
  }
  // a sample into a slot: the payload (efcodec.encode), its frames and rate, a name. The FM-1 pulls the payload with
  // read requests, writes and checks every sector, then puts the sample in its directory; it says so by reading
  // UP_OK (or UP_FAIL | code). onProgress(bytes, total)
  upload(slot, payload, meta, onProgress) { return this.link.exclusive(() => this.uploadNow(slot, payload, meta, onProgress)); }
  async uploadNow(slot, payload, { frames, rate, name, factory = false, crc }, onProgress) {
    const link = this.link;
    let last = Date.now(), done = null, sent = 0, asked = false;
    const finish = new Promise((resolve) => { done = resolve; });
    link.reqFns = (r) => {
      last = Date.now();
      asked = true;
      const top = (r.addr & 0xFF000000) >>> 0;
      if (top === UP_OK || top === UP_FAIL) {
        link.send(readResponse(r.addr, [...new TextEncoder().encode("success"), 0], r.fl));
        done(top === UP_OK ? 0 : (r.addr & 0xFFFF) || 99);
        return;
      }
      if (r.len > MAXDATA || r.addr + r.len > payload.length) { done(-1); return; }
      link.send(readResponse(r.addr, Array.from(payload.subarray(r.addr, r.addr + r.len)), r.fl));
      sent = Math.max(sent, r.addr + r.len);
      onProgress?.(sent, payload.length);
    };
    try {
      // the FM-1 holds one incoming frame: an UPLOAD it never saw (it still had the last one) is sent again while no read
      // request has come (nothing is written before the first); the next command waits until it answers again
      const head = frame(CMD.UPLOAD, [slot, factory ? 1 : 0, ...le16(rate), ...le32(frames), ...le32(crc >>> 0), ...name12(name)]);
      link.send(head);
      const t0 = Date.now();
      let again = 0;                       // (asked: a read request came)
      const timer = (async () => {
        for (;;) {
          await sleep(250);
          if (link.lost) return -2;
          if (!asked && again < 3 && Date.now() - t0 > 1500 * (again + 1)) { again++; link.send(head); }
          if (Date.now() - last > 15000) return -3;
        }
      })();
      const rc = await Promise.race([finish, timer]);
      if (rc === 0) { await this.link.askNow(CMD.HELLO, [], { ms: 1000, tries: 2 }); return; }
      if (rc === -1) throw new Error("the FM-1 asked for data outside the sample (stopped)");
      if (rc === -2) throw new Error("the FM-1 was disconnected");
      if (rc === -3) throw new Error("the FM-1 stopped asking for data");
      throw new Error(`the FM-1 did not keep the sample: ${ERR[rc] || "error " + rc}`);
    } finally { link.reqFns = null; }
  }
  onNotify(fn) { this.link.notifyFns.add(fn); return () => this.link.notifyFns.delete(fn); }
  // a note to the FM-1 (USB-MIDI, channel 1: the FM-1 listens on every channel unless GLO says otherwise)
  note(n, vel, on = true) { this.link.send(on && vel ? [0x90, n & 127, vel & 127] : [0x80, n & 127, 0]); }
}

// Web MIDI: the FM-1's port among the ports, as a transport
export function midiTransport(input, output) {
  return {
    send: (b) => output.send(b),
    onMessage: (fn) => { input.onmidimessage = (ev) => fn(ev.data); },
    close: () => { input.onmidimessage = null; },
  };
}
