// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// SOUND: the FM-1's sound to play with. The stage (the sound itself: its waveform read from the FM-1, START, END and
// POSITION dragged on it, LISTEN, the 16 sounds as chips), the modes, the main controls as big knobs and an XY pad,
// every other page grouped as the FM-1's buttons (the effects first), presets. In step with the panel
// both ways: what is turned on the FM-1 moves here (proto.c NOTIFY), what is moved here goes to the FM-1.
import { app, $, el } from "./app.js";
import { PARAMS, PAGES, MODES, MODE_BLURB, P, quant, dequant } from "./lib/efparams.js";
import { formatParam } from "./lib/fmt.js";
import { decode, sectorsFor, SEC_PAYLOAD } from "./lib/efcodec.js";

const KCOL = ["var(--k1)", "var(--k2)", "var(--k3)", "var(--k4)"];
const val = PARAMS.map((p) => p.def);
let locks = new Uint8Array(8), play = false, freeze = false, knobs = new Map(), queue = new Map(), timer = 0, unNotify = null;
const store = { get: (k, d) => { try { return JSON.parse(localStorage.getItem("ef-sound-" + k)) ?? d; } catch { return d; } },
                set: (k, v) => { try { localStorage.setItem("ef-sound-" + k, JSON.stringify(v)); } catch { /* private window */ } } };
const on = () => app.connected;

function norm(id, v) { const p = PARAMS[id]; return (v - p.min) / (p.max - p.min); }
function clampP(id, v) { const p = PARAMS[id]; v = Math.min(p.max, Math.max(p.min, v)); return p.kind === "cont" ? v : Math.round(v); }

// ---------------------------------------------------------------- one change, everywhere
function change(id, v, from = "here") {
  v = clampP(id, v);
  if (v === val[id]) return;
  val[id] = v;
  for (const u of knobs.get(id) || []) u();
  if (id === P.MODE) renderModes();
  if (id === P.START || id === P.END || id === P.POSITION) drawWave();
  xyUpdate();
  if (from !== "here") return;
  queue.set(id, quant(PARAMS[id], v));
  if (!timer) timer = setTimeout(flush, 30);
}
async function flush() {
  timer = 0;
  if (!on() || !queue.size) { queue.clear(); return; }
  const pairs = [...queue.entries()];
  queue.clear();
  try { await app.ef.set(pairs); } catch { /* a lost frame: the next move sends again */ }
}
const cmd = (pairs) => (on() ? app.ef.set(pairs).catch(() => {}) : Promise.resolve());

// ---------------------------------------------------------------- knobs
function arc(c, r, a0, a1) {
  const ang = (t) => (135 + 270 * t) * Math.PI / 180, pt = (t) => [c + r * Math.cos(ang(t)), c + r * Math.sin(ang(t))];
  const [x0, y0] = pt(a0), [x1, y1] = pt(a1);
  return `M${x0.toFixed(1)} ${y0.toFixed(1)} A${r} ${r} 0 ${a1 - a0 > 2 / 3 ? 1 : 0} 1 ${x1.toFixed(1)} ${y1.toFixed(1)}`;
}
function knobEl(id, col, size = "") {
  const p = PARAMS[id], S = size === "big" ? 80 : 54, c = S / 2, r = c - 5;
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", `0 0 ${S} ${S}`);
  svg.innerHTML = `<path d="${arc(c, r, 0, 1)}" fill="none" stroke="var(--line)" stroke-width="${S / 10}" stroke-linecap="round"/>` +
    `<path class="v" fill="none" stroke="${col}" stroke-width="${S / 10}" stroke-linecap="round"/>` +
    `<circle cx="${c}" cy="${c}" r="${r * 0.58}" fill="var(--card)" stroke="var(--line)"/>` +
    `<line class="ptr" x1="${c}" y1="${c}" stroke="var(--ink)" stroke-width="${S / 22}" stroke-linecap="round"/>`;
  const v = el("span", { class: "val" });
  const k = el("div", { class: "knob " + size, tabindex: 0, role: "slider", "aria-label": p.label, title: `${p.label}: drag up / down (Shift: fine), wheel, arrows; double-click: default` },
    svg, el("span", { class: "lab", style: `color:${col === "var(--k3)" ? "var(--muted)" : col}` }, p.label), v);
  const update = () => {
    const t = norm(id, val[id]), z = p.min < 0 && p.max > 0 ? norm(id, 0) : 0, a0 = Math.min(z, t), a1 = Math.max(z, t);
    svg.querySelector(".v").setAttribute("d", a1 - a0 > 0.004 ? arc(c, r, a0, a1) : "");
    const ang = (135 + 270 * t) * Math.PI / 180, ptr = svg.querySelector(".ptr");
    ptr.setAttribute("x2", (c + r * 0.5 * Math.cos(ang)).toFixed(1));
    ptr.setAttribute("y2", (c + r * 0.5 * Math.sin(ang)).toFixed(1));
    v.textContent = formatParam(id, val[id]);
    k.setAttribute("aria-valuenow", val[id]);
    k.setAttribute("aria-valuetext", v.textContent);
    k.classList.toggle("locked", !!((locks[id >> 3] >> (id & 7)) & 1));
  };
  let y0 = 0, v0 = 0;
  k.addEventListener("pointerdown", (e) => { y0 = e.clientY; v0 = val[id]; k.setPointerCapture(e.pointerId); k.focus(); k.classList.add("active"); });
  k.addEventListener("pointerup", () => k.classList.remove("active"));
  k.addEventListener("pointermove", (e) => {
    if (!k.hasPointerCapture(e.pointerId)) return;
    const dy = y0 - e.clientY;
    change(id, p.kind === "cont" ? v0 + dy / (e.shiftKey ? 800 : 180) * (p.max - p.min) : Math.round(v0 + dy / 22));
  });
  k.addEventListener("wheel", (e) => { e.preventDefault(); step(id, e.deltaY < 0 ? 1 : -1, e.shiftKey); }, { passive: false });
  k.addEventListener("keydown", (e) => {
    const d = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1, PageUp: 10, PageDown: -10 }[e.key];
    if (d) { e.preventDefault(); step(id, d, e.shiftKey); }
    if (e.key === "Home") change(id, p.min);
    if (e.key === "End") change(id, p.max);
  });
  k.addEventListener("dblclick", () => change(id, p.def));
  knobs.set(id, [...(knobs.get(id) || []), update]);
  update();
  return k;
}
function step(id, d, fine) { const p = PARAMS[id]; change(id, val[id] + d * (p.kind === "cont" ? p.step * (fine ? 1 : 3) : 1)); }

// ---------------------------------------------------------------- the stage: the sound itself
const waves = new Map();                 // slot|name|frames|rate -> {lo, hi}: the waveform, read once
let wave = null, waveKey = "", listening = 0, listenT0 = 0, listenDur = 0, raf = 0;
function current() { return app.dir && app.info ? app.dir[app.info.slot] : null; }

async function loadWave() {
  const d = current();
  $("stg-name").textContent = d && d.state === "ok" ? d.name : on() ? (app.info?.slot === -1 ? "The seed sound" : "No sound") : "No FM-1 connected";
  $("stg-meta").textContent = d && d.state === "ok" ? `slot ${d.slot + 1} · ${(d.frames / d.rate).toFixed(2)} s · ${Math.round(d.rate / 100) / 10} kHz${d.factory ? " · factory" : ""}` :
    app.info?.slot === -1 ? "the bank is empty: the FM-1 makes this one itself" : "";
  renderChips();
  if (!d || d.state !== "ok") { wave = null; drawWave(); return; }
  const key = `${d.slot}|${d.name}|${d.frames}|${d.rate}`;
  if (key === waveKey && wave) return drawWave();
  waveKey = key;
  if (waves.has(key)) { wave = waves.get(key); return drawWave(); }
  wave = null;
  drawWave();
  $("stg-loading").hidden = false;
  try {
    const pcm = decode(await app.ef.read(d.slot, sectorsFor(d.frames) * SEC_PAYLOAD), d.frames);
    const N = 600, lo = new Float32Array(N), hi = new Float32Array(N);
    for (let c = 0; c < N; c++) {
      const a = Math.floor(c * pcm.length / N), b = Math.max(a + 1, Math.floor((c + 1) * pcm.length / N));
      let l = 0, h = 0;
      for (let i = a; i < b; i++) { const v = pcm[i] / 32768; if (v < l) l = v; if (v > h) h = v; }
      lo[c] = l; hi[c] = h;
    }
    waves.set(key, { lo, hi });
    if (waveKey === key) wave = waves.get(key);
  } catch { /* the FM-1 busy: the outline waits for the next look */ waveKey = ""; }
  $("stg-loading").hidden = true;
  drawWave();
}

function drawWave() {
  const c = $("stg-wave"), g = c.getContext("2d"), W = c.width, H = c.height, mid = H / 2;
  const css = getComputedStyle(document.documentElement), col = (n) => css.getPropertyValue(n).trim();
  g.fillStyle = "#000"; g.fillRect(0, 0, W, H);
  const a = Math.min(val[P.START], val[P.END]) * W, b = Math.max(val[P.START], val[P.END]) * W, x = val[P.POSITION];
  if (wave) {
    const N = wave.lo.length;
    for (let i = 0; i < N; i++) {
      const px = i / N * W, inside = px >= a && px <= b;
      g.fillStyle = inside ? "#c8c8cc" : "#34343a";
      g.fillRect(px, mid - wave.hi[i] * mid * 0.92, Math.ceil(W / N), Math.max(1, (wave.hi[i] - wave.lo[i]) * mid * 0.92));
    }
  } else {
    g.fillStyle = "#26262b";
    for (let i = 0; i < 120; i++) { const h = 6 + 30 * Math.abs(Math.sin(i * 0.37) * Math.sin(i * 0.11)); g.fillRect(i * W / 120, mid - h, W / 120 - 3, 2 * h); }
  }
  g.fillStyle = "rgba(0,0,0,.45)"; g.fillRect(0, 0, a, H); g.fillRect(b, 0, W - b, H);
  // the position (where the grains come from), START and END
  const px = a + x * (b - a);
  g.fillStyle = col("--k4"); g.fillRect(px - 2, 0, 4, H);
  g.beginPath(); g.moveTo(px - 9, 0); g.lineTo(px + 9, 0); g.lineTo(px, 12); g.fill();
  g.fillStyle = col("--k2"); g.fillRect(a - 2, 0, 4, H); g.fillRect(a, 0, 14, 4); g.fillRect(a, H - 4, 14, 4);
  g.fillStyle = col("--k3s") || "#e6e6e6"; g.fillRect(b - 2, 0, 4, H); g.fillRect(b - 14, 0, 14, 4); g.fillRect(b - 14, H - 4, 14, 4);
  if (listening) {                       // LISTEN's playhead (timed here: the FM-1 plays it)
    const f = Math.min(1, (performance.now() - listenT0) / listenDur);
    g.fillStyle = "#fff"; g.fillRect(a + f * (b - a) - 1, 0, 3, H);
  }
}
function waveDrag() {
  const c = $("stg-wave");
  let which = null;
  const at = (e) => { const r = c.getBoundingClientRect(); return Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)); };
  c.addEventListener("pointerdown", (e) => {
    const x = at(e), a = Math.min(val[P.START], val[P.END]), b = Math.max(val[P.START], val[P.END]), px = a + val[P.POSITION] * (b - a);
    const near = (v) => Math.abs(x - v) * c.getBoundingClientRect().width < 14;
    which = near(a) ? "start" : near(b) ? "end" : "pos";
    c.setPointerCapture(e.pointerId);
    move(e);
    void px;
  });
  const move = (e) => {
    if (!which) return;
    const x = at(e), a = Math.min(val[P.START], val[P.END]), b = Math.max(val[P.START], val[P.END]);
    if (which === "start") change(P.START, Math.min(x, b - 0.01));
    else if (which === "end") change(P.END, Math.max(x, a + 0.01));
    else change(P.POSITION, b > a ? (x - a) / (b - a) : 0);
  };
  c.addEventListener("pointermove", (e) => c.hasPointerCapture(e.pointerId) && move(e));
  c.addEventListener("pointerup", () => { which = null; });
}

async function listen() {
  const d = current();
  if (listening) {
    listening = 0;
    $("snd-listen").classList.remove("on");
    await cmd([[0xF6, 0]]);
    return drawWave();
  }
  if (!on()) return;
  const len = d && d.state === "ok" ? d.frames / d.rate : 2.7;
  listenDur = Math.max(0.05, Math.abs(val[P.END] - val[P.START]) * len) * 1000;
  await cmd([[0xF6, 1]]);
  listening = 1;
  listenT0 = performance.now();
  $("snd-listen").classList.add("on");
  const tick = () => {
    if (!listening) return;
    if (performance.now() - listenT0 > listenDur) { listening = 0; $("snd-listen").classList.remove("on"); drawWave(); return; }
    drawWave();
    raf = requestAnimationFrame(tick);
  };
  cancelAnimationFrame(raf);
  tick();
}

function renderChips() {
  const box = $("stg-chips");
  box.replaceChildren();
  const dir = app.dir || Array.from({ length: 16 }, (_, i) => ({ slot: i, state: "empty" }));
  for (const d of dir) {
    const ok = d.state === "ok", cur = app.info && app.info.slot === d.slot;
    box.append(el("button", { type: "button", role: "radio", class: "chip" + (ok ? "" : " empty"), "aria-checked": !!cur, disabled: !ok || !on(),
      title: ok ? `${d.name}: play this sound (EDIT + white key ${d.slot + 1})` : "empty slot",
      onclick: async () => {
        listening = 0;
        $("snd-listen").classList.remove("on");
        await cmd([[0xF2, d.slot]]);
        if (app.info) app.info.slot = d.slot;
        app.emit("slot", d.slot);
        loadWave();
      } }, el("b", {}, String(d.slot + 1)), ok ? d.name : "—"));
  }
}

// ---------------------------------------------------------------- modes
const MODE_ICON = [
  '<circle cx="9" cy="14" r="5"/><circle cx="16" cy="10" r="6"/><circle cx="23" cy="14" r="5"/><rect x="9" y="14" width="14" height="5"/>',
  '<circle cx="6" cy="8" r="1.5"/><circle cx="13" cy="15" r="1.5"/><circle cx="20" cy="6" r="1.5"/><circle cx="26" cy="17" r="1.5"/><circle cx="9" cy="20" r="1.5"/><circle cx="22" cy="12" r="1"/>',
  '<circle cx="9" cy="12" r="6" fill="none" stroke-width="2.5"/><circle cx="23" cy="12" r="6" fill="none" stroke-width="2.5"/><path d="M9 18h14" stroke-width="2.5"/>',
  '<path d="M2 20 Q16 -4 30 20Z"/>',
  '<rect x="3" y="8" width="3" height="12"/><rect x="10" y="14" width="3" height="6"/><rect x="17" y="6" width="3" height="14"/><rect x="24" y="12" width="3" height="8"/>',
  '<path d="M2 14 Q8 4 14 14 T26 14 T32 10" fill="none" stroke-width="2.5"/>',
];
function renderModes() {
  const box = $("modes");
  if (!box.childElementCount)
    MODES.forEach((m, i) => box.append(el("button", { type: "button", role: "radio", class: "mode m" + i, onclick: () => change(P.MODE, i) },
      (() => { const s = document.createElementNS("http://www.w3.org/2000/svg", "svg"); s.setAttribute("viewBox", "0 0 32 24"); s.innerHTML = `<g fill="currentColor" stroke="currentColor">${MODE_ICON[i]}</g>`; return s; })(),
      el("b", {}, m), el("span", {}, MODE_BLURB[i]))));
  [...box.children].forEach((b, i) => b.setAttribute("aria-checked", Math.round(val[P.MODE]) === i));
}

// ---------------------------------------------------------------- perform: the main controls, the XY pad
function renderMacros() {
  const box = $("macros");
  box.replaceChildren();
  for (const pg of PAGES.filter((p) => p.group === "home"))
    pg.cells.forEach((id, k) => id >= 0 && box.append(knobEl(id, KCOL[k], "big")));
}
const CONT = PARAMS.filter((p) => p.kind === "cont" && p.id !== P.START && p.id !== P.END);
let xyX = store.get("xyx", P.SCATTER), xyY = store.get("xyy", P.TEXTURE);
function xyUpdate() {
  const pad = $("xy"), dot = $("xy-dot");
  if (!pad) return;
  dot.style.left = `${norm(xyX, val[xyX]) * 100}%`;
  dot.style.top = `${(1 - norm(xyY, val[xyY])) * 100}%`;
  $("xy-read").textContent = `${PARAMS[xyX].label} ${formatParam(xyX, val[xyX])} · ${PARAMS[xyY].label} ${formatParam(xyY, val[xyY])}`;
}
function xySetup() {
  for (const [sel, cur, k] of [[$("xy-x"), xyX, "xyx"], [$("xy-y"), xyY, "xyy"]]) {
    for (const p of CONT) sel.append(el("option", { value: p.id }, p.label));
    sel.value = cur;
    sel.addEventListener("change", () => { if (k === "xyx") xyX = +sel.value; else xyY = +sel.value; store.set(k, +sel.value); xyUpdate(); });
  }
  const pad = $("xy");
  const at = (e) => {
    const r = pad.getBoundingClientRect(), fx = Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)), fy = Math.max(0, Math.min(1, 1 - (e.clientY - r.top) / r.height));
    const px = PARAMS[xyX], py = PARAMS[xyY];
    change(xyX, px.min + fx * (px.max - px.min));
    change(xyY, py.min + fy * (py.max - py.min));
  };
  pad.addEventListener("pointerdown", (e) => { pad.setPointerCapture(e.pointerId); pad.classList.add("active"); at(e); });
  pad.addEventListener("pointermove", (e) => pad.hasPointerCapture(e.pointerId) && at(e));
  pad.addEventListener("pointerup", () => pad.classList.remove("active"));
  pad.addEventListener("keydown", (e) => {
    const d = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, 1], ArrowDown: [0, -1] }[e.key];
    if (!d) return;
    e.preventDefault();
    if (d[0]) step(xyX, d[0], e.shiftKey);
    if (d[1]) step(xyY, d[1], e.shiftKey);
  });
  xyUpdate();
}

// ---------------------------------------------------------------- the pages, grouped as the FM-1's buttons
const GROUPS = [["fx", "Effects", "FX"], ["grain", "Grain", "LFO"], ["oracle", "Oracle", "SEL"], ["keys", "Keys", "ENV"]];
function renderGroups() {                // every page at once, under its group's heading
  const box = $("groups");
  box.replaceChildren();
  knobs = new Map();
  renderMacros();
  for (const [g, name, btn] of GROUPS) {
    const pages = el("div", { class: "pages" });
    for (const pg of PAGES.filter((p) => p.group === g)) {
      const row = el("div", { class: "knobs" });
      pg.cells.forEach((id, k) => row.append(id >= 0 ? knobEl(id, KCOL[k]) : el("div", { class: "knob empty" })));
      pages.append(el("div", { class: "card page" }, el("h3", {}, pg.name), row));
    }
    box.append(el("section", { class: "card group g-" + g, "aria-label": name },
      el("div", { class: "row between wrap gap" }, el("h2", {}, name), el("span", { class: "muted small" }, `the ${btn} button`)), pages));
  }
}

// ---------------------------------------------------------------- presets
async function presets() {
  const u = $("pre-user"), f = $("pre-fac");
  u.replaceChildren(); f.replaceChildren();
  if (!on()) { u.append(el("li", { class: "unused" }, el("span", { class: "n" }, "connect the FM-1 to see its presets"))); return; }
  let list;
  try { list = await app.ef.presets(); } catch { return; }
  const op = async (o, i, name = "") => {
    try { await app.ef.presetOp(o, i, name); } catch (e) { alert(e.message); }
    await pull();
    presets();
  };
  for (const p of list.user) {
    u.append(el("li", { class: p.used ? "" : "unused" },
      el("span", { class: "n" }, el("b", {}, `U${String(p.index + 1).padStart(2, "0")}`), " ", p.used ? p.name : "empty"),
      p.used ? el("button", { type: "button", class: "primary", onclick: () => op(0, p.index) }, "Load") : null,
      el("button", { type: "button", title: "keep the sound as it is now in this slot", onclick: () => {
        if (p.used && !confirm(`Overwrite ${p.name}?`)) return;
        const n = prompt("Name (11 characters)", p.used ? p.name : "");
        if (n != null) op(1, p.index, n.toUpperCase());
      } }, "Store"),
      p.used ? el("button", { type: "button", title: "rename", onclick: () => { const n = prompt("New name", p.name); if (n) op(4, p.index, n.toUpperCase()); } }, "✎") : null,
      p.used ? el("button", { type: "button", title: "delete", onclick: () => confirm(`Delete ${p.name}?`) && op(2, p.index) }, "✕") : null));
  }
  for (const p of list.factory)
    f.append(el("button", { type: "button", class: "chip", onclick: () => op(3, p.index) }, el("b", {}, `F${p.index + 1}`), p.name));
}

async function pull() {                  // everything from the FM-1
  if (!on()) return;
  try {
    const pr = await app.ef.params();
    pr.q.forEach((q, i) => { if (PARAMS[i]) val[i] = dequant(PARAMS[i], q); });
    locks = Uint8Array.from(pr.locks);
    play = pr.play; freeze = pr.freeze;
    if (app.info) app.info.slot = pr.slot;
  } catch { return; }
  for (const [, us] of knobs) for (const u of us) u();
  renderModes();
  xyUpdate();
  transport();
  loadWave();
}
function transport() {
  $("snd-play").setAttribute("aria-pressed", play);
  $("snd-freeze").setAttribute("aria-pressed", freeze);
  $("snd-offline").hidden = on();
}

export function init() {
  renderModes();
  renderGroups();                        // (draws the big knobs too)
  xySetup();
  waveDrag();
  renderChips();
  drawWave();
  transport();
  $("snd-play").addEventListener("click", () => { play = !play; transport(); cmd([[0xF0, play ? 1 : 0]]); });
  $("snd-freeze").addEventListener("click", () => { freeze = !freeze; transport(); cmd([[0xF1, freeze ? 1 : 0]]); });
  $("snd-random").addEventListener("click", async () => { await cmd([[0xF3, 1]]); pull(); });
  $("snd-undo").addEventListener("click", async () => { await cmd([[0xF4, 0]]); pull(); });
  $("snd-listen").addEventListener("click", listen);
  app.on("connected", () => {
    unNotify?.();
    unNotify = app.ef.onNotify((id, q) => {
      if (!PARAMS[id] || queue.has(id)) return;
      change(id, dequant(PARAMS[id], q), "fm1");
    });
    transport();
    pull();
    presets();
  });
  app.on("disconnected", () => { transport(); renderChips(); presets(); });
  app.on("dir", () => loadWave());
  app.on("slot", () => app.emit("refresh-bank"));
  app.on("tab", (t) => { if (t === "sound") { pull(); presets(); } });
  addEventListener("resize", drawWave);
  // the FM-1 forgets a quiet host after 20 s: ask now and then while this page is open, so its knobs keep coming
  setInterval(() => { if (on() && !app.link.busy && !$("tab-sound").hidden) app.ef.hello(); }, 10000);
}
