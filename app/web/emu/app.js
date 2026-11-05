// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// ECHOFIELD's part of the emulator page (fm1-base imports it when the FM-1 is switched on): the companion's Samples
// editor under the panel, on the emulated FM-1. The editor is the companion's own (../samples.js); what it calls on
// an FM-1 over USB (efproto.js EchoField: hello, dir, read, upload, erase, rename, set) is answered here by the
// firmware's bank code in the worklet (app_worklet.js, app/web.c).
//
// The emulator's flash is not kept between visits (fm1-base's page keeps the store objects only), so the samples are
// kept here, in this browser (localStorage, BANK_KEY), and put back after boot; with none kept, every factory sound
// goes into its own slot. The page's Factory reset forgets them too (index.html).
const BANK_KEY = "echofield-emu-bank";
const SLOT_STATE = ["empty", "ok", "damaged"];

let node = null, nextId = 0, restored = false, saveTimer = 0;
const waiting = new Map(), fns = new Map();
const call = (op, args = {}, transfer = []) => new Promise((resolve) => {
  const id = ++nextId;
  waiting.set(id, resolve);
  node.port.postMessage({ type: "app", id, op, ...args }, transfer);
});
const str = (b, o, n) => String.fromCharCode(...b.slice(o, o + n)).replace(/\0[\s\S]*$/, "");

// the companion's app object, as samples.js uses it
const app = {
  info: { slot: -1, version: "emulator" },
  dir: null,
  connected: false,
  on(ev, fn) { if (!fns.has(ev)) fns.set(ev, new Set()); fns.get(ev).add(fn); },
  emit(ev, ...a) { for (const fn of fns.get(ev) || []) try { fn(...a); } catch (e) { console.error(e); } },
  ef: {
    async hello() { const r = await call("dir"); app.info.slot = r.slot; return app.info; },
    async dir() {
      const { dir: b, slot } = await call("dir");
      app.info.slot = slot;
      const out = [];
      for (let i = 0; i < 16; i++) {
        const o = i * 20;
        out.push({ slot: i, state: SLOT_STATE[b[o]] || "empty", factory: !!b[o + 1], name: str(b, o + 2, 12),
                   frames: (b[o + 14] | (b[o + 15] << 8) | (b[o + 16] << 16) | (b[o + 17] << 24)) >>> 0,
                   rate: b[o + 18] | (b[o + 19] << 8) });
      }
      return out;
    },
    async read(slot, bytes, onProgress) {
      const r = await call("read", { slot });
      if (r.rc) throw new Error("reading the sample failed");
      onProgress?.(bytes, bytes);
      return r.payload.subarray(0, bytes);
    },
    async upload(slot, payload, { frames, rate, name, factory = false, crc }, onProgress) {
      const p = payload.slice();
      const r = await call("write", { slot, payload: p.buffer, frames, rate, name, factory, crc }, [p.buffer]);
      onProgress?.(payload.length, payload.length);
      if (r.rc) throw new Error(`the emulator did not keep the sample (error ${r.rc})`);
    },
    async erase(slot) { const r = await call("erase", { slot }); if (r.rc) throw new Error(`delete: error ${r.rc}`); },
    async rename(slot, name) { const r = await call("rename", { slot, name }); if (r.rc) throw new Error(`rename: error ${r.rc}`); },
    async set(pairs) {                      // the editor's: 0xF2 a slot, 0xF6 LISTEN
      for (const [id, q] of pairs) {
        if (id === 0xF2) await call("select", { slot: q });
        else if (id === 0xF6) await call("listen", { how: q });
      }
    },
  },
};

// ---------------------------------------------------------------- keeping the samples in this browser
function b64(u8) { let s = ""; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode(...u8.subarray(i, i + 0x8000)); return btoa(s); }
function unb64(s) { return Uint8Array.from(atob(s), (c) => c.charCodeAt(0)); }
function kept() {
  try { return JSON.parse(localStorage.getItem(BANK_KEY) || "null"); } catch { return null; }
}
async function save() {
  try {
    const dir = await app.ef.dir(), samples = [];
    for (const d of dir.filter((x) => x.state === "ok")) {
      const r = await call("read", { slot: d.slot });
      if (!r.rc) samples.push({ slot: d.slot, name: d.name, frames: d.frames, rate: d.rate, factory: d.factory, payload: b64(r.payload) });
    }
    localStorage.setItem(BANK_KEY, JSON.stringify({ v: 1, samples }));
  } catch { /* storage unavailable or full: this visit only */ }
}
async function restore() {
  const k = kept();
  if (k && k.v === 1 && Array.isArray(k.samples)) {
    const samples = k.samples.map((s) => ({ ...s, payload: unb64(s.payload).buffer }));
    await call("restore", { samples }, samples.map((s) => s.payload));
  } else {
    await call("factory");               // a first visit, or forgotten: every factory sound, as after the flasher
  }
  restored = true;
}

// ---------------------------------------------------------------- the page's side (fm1-base's index.html calls these)
let samples = null, attached = null;
export function attach(ctx) { attached = attachNow(ctx); return attached; }
async function attachNow(ctx) {
  node = ctx.node;
  const box = document.getElementById("samples");
  try {
    samples = await import("../samples.js");   // the companion's editor (the emulator is published inside it)
  } catch {
    if (box) box.hidden = true;           // (a build of the emulator on its own: no editor)
    return;
  }
  const $ = (id) => document.getElementById(id);
  const el = (tag, attrs = {}, ...kids) => {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
      if (k === "class") e.className = v;
      else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
      else if (v !== false && v != null) e.setAttribute(k, v === true ? "" : v);
    }
    for (const k of kids.flat()) if (k != null) e.append(k.nodeType ? k : String(k));
    return e;
  };
  const msg = (id, text, kind = "") => { const m = $(id); m.textContent = text; m.className = "msg " + kind; };
  const progress = (id, frac) => {
    const p = $(id);
    p.hidden = frac == null;
    if (frac != null) p.firstElementChild.style.width = `${Math.round(Math.max(0, Math.min(1, frac)) * 100)}%`;
  };
  const download = (name, data, type = "application/octet-stream") => {
    const a = el("a", { href: URL.createObjectURL(new Blob([data], { type })), download: name });
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  };
  samples.init({ app, $, el, msg, progress, download });
  $("emu-off")?.setAttribute("hidden", "");
  $("samples-body")?.removeAttribute("hidden");
}

export function message(m) {
  if (m.id && waiting.has(m.id)) {
    waiting.get(m.id)(m);
    waiting.delete(m.id);
    return;
  }
  if (m.op !== "gen") return;
  if (!restored) {                         // the first word from the worklet: booted. The samples back, then the editor
    Promise.all([attached, restore()]).then(() => { app.connected = true; app.emit("connected"); });
    return;
  }
  app.info.slot = m.slot;
  app.emit("refresh-bank");                // the bank changed (the editor, or the panel: RESAMPLE, a delete), or the sample
  if (!m.bank) return;                     // playing (the list marks it)
  clearTimeout(saveTimer);
  saveTimer = setTimeout(save, 1000);
}
