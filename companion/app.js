// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// ECHOFIELD companion: the connection, the tabs, and what the pages share.
import { EfLink, EchoField, midiTransport } from "./lib/efproto.js";
import { pairs, usualName } from "./vendor/fm1-base/fm1ports.js";
import * as samples from "./samples.js";
import * as sound from "./sound.js";
import * as takes from "./takes.js";
import * as screen from "./screen.js";
import * as manual from "./manual.js";

export const $ = (id) => document.getElementById(id);
export const el = (tag, attrs = {}, ...kids) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) e.setAttribute(k, v === true ? "" : v);
  }
  for (const k of kids.flat()) if (k != null) e.append(k.nodeType ? k : String(k));
  return e;
};
export function msg(id, text, kind = "") { const m = $(id); m.textContent = text; m.className = "msg " + kind; }
export function progress(id, frac) {
  const p = $(id);
  p.hidden = frac == null;
  if (frac != null) p.firstElementChild.style.width = `${Math.round(Math.max(0, Math.min(1, frac)) * 100)}%`;
}
export function download(name, data, type = "application/octet-stream") {
  const a = el("a", { href: URL.createObjectURL(new Blob([data], { type })), download: name });
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}

// ---------------------------------------------------------------- shared state and events
export const app = {
  access: null, link: null, ef: null, port: "", busy: 0, wanted: false,
  get info() { return this.ef ? this.ef.info : null; },   // always the FM-1's latest HELLO (ef.hello refreshes it)
  fns: new Map(),
  on(ev, fn) { if (!this.fns.has(ev)) this.fns.set(ev, new Set()); this.fns.get(ev).add(fn); },
  emit(ev, ...a) { for (const fn of this.fns.get(ev) || []) try { fn(...a); } catch (e) { console.error(e); } },
  get connected() { return !!this.ef && !!this.info; },
};

function status(text, kind) { const s = $("status"); s.textContent = text; s.className = "pill " + kind; }

export async function midiAccess() {
  if (app.access) return app.access;
  if (!navigator.requestMIDIAccess) throw new Error("This browser has no Web MIDI: use Chrome, Edge or Opera on a computer.");
  if (!isSecureContext) throw new Error("Web MIDI needs a secure page: open this from https or from localhost (companion/serve.py).");
  app.access = await navigator.requestMIDIAccess({ sysex: true });
  app.access.onstatechange = (e) => {
    if (e.port.state === "disconnected" && app.port && e.port.name === app.port) lost();
    else if (e.port.state === "connected" && app.wanted && !app.connected && !app.busy) setTimeout(() => connect(true), 600);
  };
  return app.access;
}

function lost() {
  if (!app.link) return;
  try { app.link.close(); } catch { /* gone */ }
  app.link = app.ef = null;
  app.port = "";
  status("disconnected", "off");
  app.emit("disconnected");
}

// find the FM-1 running ECHOFIELD: a port with an FM-1's name (or ECHOFIELD's) that answers ECHOFIELD's HELLO
export async function connect(quiet = false) {
  if (app.connected || app.busy) return app.connected;   // (busy: an install owns the port)
  app.wanted = true;
  try {
    const access = await midiAccess();
    status("looking…", "off");
    for (const p of pairs(access)) {
      if (!usualName(p.name) && !/echofield/i.test(p.name)) continue;
      if (/update|^ota|uboot/i.test(p.name)) continue;   // an update loader's port is the installer's alone
      try { await p.input.open(); await p.output.open(); } catch { continue; }
      const link = new EfLink(midiTransport(p.input, p.output)), ef = new EchoField(link);
      const orig = link.ask.bind(link);
      link.ask = (cmd, body, o = {}) => orig(cmd, body, { ms: 700, tries: 2, ...o });
      const info = await ef.hello();
      link.ask = orig;
      if (info) {
        app.link = link; app.ef = ef; app.port = p.name;
        status(`ECHOFIELD ${info.version} · ${p.name}`, "on");
        app.emit("connected");
        return true;
      }
      link.close();
    }
    status("FM-1 not found", "warn");
    if (!quiet) app.emit("notfound");
  } catch (e) {
    status("no MIDI", "warn");
    if (!quiet) alert(e.message);
  }
  return false;
}

// a stretch of work that needs the port alone (an install): the link let go, then back
export function release() { if (app.link) { app.link.close(); app.link = app.ef = null; app.port = ""; status("busy: installing", "warn"); app.emit("disconnected"); } }

// ---------------------------------------------------------------- tabs
function tab(name) {
  for (const b of document.querySelectorAll(".tabs button")) b.setAttribute("aria-selected", b.dataset.tab === name);
  for (const s of document.querySelectorAll(".tab")) s.hidden = s.id !== "tab-" + name;
  app.emit("tab", name);
  history.replaceState(null, "", "#" + name);
}
for (const b of document.querySelectorAll(".tabs button")) b.addEventListener("click", () => tab(b.dataset.tab));
$("connect").addEventListener("click", () => connect());
addEventListener("hashchange", () => tab(location.hash.slice(1).replace(/[^a-z]/g, "") || "manual"));

// the flasher is for computers only: its link is not shown on phones and tablets
import("./flasher/flasher.js").then((f) => { if (f.isMobile()) for (const e of document.querySelectorAll(".desktop-only")) e.hidden = true; });
samples.init();
sound.init();
takes.init();
screen.init();
manual.init();
tab((location.hash || "#manual").slice(1).replace(/[^a-z]/g, "") || "manual");
// a quiet first look: if MIDI was allowed before, the FM-1 is found without a click
navigator.permissions?.query({ name: "midi", sysex: true }).then((p) => { if (p.state === "granted") connect(true); }).catch(() => {});
