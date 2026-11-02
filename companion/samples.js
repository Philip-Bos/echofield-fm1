// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// SAMPLES: the bank, a new sound (trim, quality, a preview of exactly what the FM-1 will play), the factory sounds,
// backup and restore.
import { app, $, el, msg, progress, download } from "./app.js";
import { prepare, decode, parseWav, wav16, crc32, slotName, sectorsFor, RATES, SEC_PAYLOAD, MIN_FRAMES } from "./lib/efcodec.js";
import { zip, unzip } from "./lib/zip.js";
import { PARAMS } from "./lib/efparams.js";
import { kb, secs } from "./lib/fmt.js";
import { AudioIn, listInputs, isFm1, why, drawMeter } from "./lib/audioin.js";

const NSEC = 92;
const SLOT_COL = ["#3d8bff", "#30d07a", "#9a9aa2", "#ff7a1a", "#7fb2ff", "#7fe3ab", "#c8c8cc", "#ffaa6b",
                  "#2a5fb0", "#1f8f53", "#6a6a72", "#b4520f", "#a879ff", "#ff4d5e", "#e0c341", "#3fc6c6"];
let dir = null, factory = null;

// ---------------------------------------------------------------- the bank
async function refresh() {
  if (!app.connected) return;
  try {
    await app.ef.hello();
    dir = await app.ef.dir();
  } catch (e) { $("bank-text").textContent = `could not read the bank: ${e.message}`; return; }
  app.dir = dir;
  app.emit("dir", dir);
  render();
}

function freeSectors(except = -1) {
  if (!dir) return NSEC;                 // not connected: sized against an empty bank, checked again before sending
  return NSEC - dir.reduce((s, d) => s + (d.state !== "empty" && d.slot !== except ? sectorsFor(d.frames) : 0), 0);
}

function render() {
  const map = $("bank-map"), list = $("slots");
  map.replaceChildren();
  list.replaceChildren();
  if (!dir) { $("bank-text").textContent = "connect the FM-1 to see its samples"; return; }
  const cells = [];
  dir.forEach((d) => { if (d.state !== "empty") for (let k = 0; k < sectorsFor(d.frames); k++) cells.push(d.state === "damaged" ? "var(--rec)" : SLOT_COL[d.slot]); });
  for (let i = 0; i < NSEC; i++) map.append(el("i", cells[i] ? { style: `background:${cells[i]}` } : {}));
  const free = freeSectors();
  $("bank-text").textContent = `${kb(free * 4096)} free of ${kb(NSEC * 4096)} · ${(free * 7424 / 22050).toFixed(1)} s at 22 kHz, ${(free * 7424 / 16000).toFixed(1)} s at 16 kHz`;
  for (const d of dir) {
    const empty = d.state === "empty", cur = app.info && app.info.slot === d.slot;
    const name = el("div", { class: "nm" }, empty ? "empty" : d.name,
      empty ? null : el("span", { class: "meta" }, `${secs(d.frames, d.rate)} · ${Math.round(d.rate / 100) / 10} kHz · ${kb(sectorsFor(d.frames) * 4096)}`),
      d.factory ? el("span", { class: "badge" }, "FACTORY") : null,
      d.state === "damaged" ? el("span", { class: "badge bad" }, "DAMAGED") : null);
    const acts = el("div", { class: "acts" },
      d.state === "ok" ? el("button", { type: "button", title: "listen to it on the FM-1, as it is (again: stop)",
        "aria-label": `listen to ${d.name}`, onclick: () => listen(d.slot) }, listening === d.slot ? "■" : "▶") : null,
      d.state === "ok" ? el("button", { type: "button", title: "save as WAV", onclick: () => exportWav(d) }, "⤓") : null,
      d.state === "ok" ? el("button", { type: "button", title: "rename", onclick: () => rename(d) }, "✎") : null,
      empty ? null : el("button", { type: "button", title: "delete", onclick: () => del(d) }, "✕"));
    list.append(el("li", { class: (empty ? "empty" : "") + (cur ? " current" : "") },
      el("span", { class: "num" }, String(d.slot + 1).padStart(2, "0")),
      el("span", { class: "sw", style: empty ? "" : `background:${d.state === "damaged" ? "var(--rec)" : SLOT_COL[d.slot]}` }),
      name, acts));
  }
  renderFactory();
  edUpdate();
}

// LISTEN on the FM-1: the sample as it is, START to END (the FM-1 also plays it from then on); again: stop
let listening = -1, listenTimer = 0;
async function listen(slot) {
  clearTimeout(listenTimer);
  if (listening === slot) {
    listening = -1;
    await app.ef.set([[0xF6, 0]]);
  } else {
    const d = dir[slot];
    listening = slot;
    await app.ef.set([[0xF2, slot], [0xF6, 1]]);   // always this sample: the FM-1 may have switched since (EDIT + a key)
    app.info.slot = slot;
    listenTimer = setTimeout(() => { listening = -1; render(); }, (d.frames / d.rate) * 1000 + 400);
  }
  render();
}
async function rename(d) {
  const n = prompt("New name (11 characters: A-Z 0-9 space - . _)", d.name);
  if (n == null) return;
  try { await app.ef.rename(d.slot, slotName(n)); } catch (e) { alert(e.message); }
  refresh();
}
async function del(d) {
  if (!confirm(`Delete ${d.name} from slot ${d.slot + 1}? This cannot be undone (back it up first if you want to keep it).`)) return;
  try { await app.ef.erase(d.slot); } catch (e) { alert(e.message); }
  refresh();
}
async function readPayload(d, onProgress) { return app.ef.read(d.slot, sectorsFor(d.frames) * SEC_PAYLOAD, onProgress); }
async function exportWav(d) {
  try {
    msg("ed-msg", `Reading ${d.name}…`);
    const pay = await readPayload(d);
    download(`${d.name.trim() || "sample"}.wav`, wav16(decode(pay, d.frames), d.rate), "audio/wav");
    msg("ed-msg", `${d.name} saved as WAV.`, "ok");
  } catch (e) { msg("ed-msg", e.message, "err"); }
}

// ---------------------------------------------------------------- a new sound
const worker = new Worker(new URL("./lib/encworker.js", import.meta.url), { type: "module" });
let jobs = 0;
const pending = new Map();
worker.onmessage = (e) => { const r = pending.get(e.data.id); pending.delete(e.data.id); r?.(e.data); };
function encodeOff(pcm) { const id = ++jobs; return new Promise((r) => { pending.set(id, r); worker.postMessage({ id, pcm }); }); }

const ed = { src: null, rate: 22050, a: 0, b: 1, name: "", pcm: null, enc: null, key: "" };
let actx = null, playing = null;

async function loadFile(file) {
  msg("ed-msg", `Opening ${file.name}…`);
  try {
    const buf = new Uint8Array(await file.arrayBuffer());
    let src;
    try { src = parseWav(buf); } catch {
      actx = actx || new AudioContext();
      const ab = await actx.decodeAudioData(buf.buffer.slice(0));
      src = { rate: ab.sampleRate, channels: Array.from({ length: ab.numberOfChannels }, (_, c) => ab.getChannelData(c)) };
    }
    loadSource(src, file.name);
  } catch (e) { msg("ed-msg", `Cannot read ${file.name}: ${e.message}`, "err"); }
}

// any sound into the editor: a file, a recording here, a take from the Sound page
export function loadSource(src, name) {
  ed.src = src;
  ed.a = 0; ed.b = 1;
  ed.key = ""; ed.enc = null;
  ed.name = slotName(name);
  $("ed-name").value = ed.name;
  $("editor").hidden = false;
  // a long sound starts with as much as fits, at 22 kHz
  const dur = src.channels[0].length / src.rate, fitS = Math.max(0.5, (freeSectors() * 7424) / 22050);
  if (dur > fitS) ed.b = fitS / dur;
  msg("ed-msg", "");
  drawWave();
  edUpdate();
  $("editor").scrollIntoView({ behavior: "smooth", block: "nearest" });
}

// ---------------------------------------------------------------- recording one
const rin = new AudioIn();
let recN = 0, recTimer = 0, hold = [0, 0];
async function recInputs() {
  const sel = $("rec-in"), keep = sel.value;
  const ins = await listInputs();
  sel.replaceChildren(...ins.map((d) => el("option", { value: d.deviceId }, (d.label || "audio input") + (isFm1(d) ? "  (FM-1)" : ""))));
  if (keep && ins.some((d) => d.deviceId === keep)) sel.value = keep;
  return ins;
}
async function recOpen() {
  try {
    await recInputs();
    await rin.start($("rec-in").value);
    $("rec-on").hidden = true; $("rec-live").hidden = false;
    rin.monitor($("rec-mon").checked);
    rin.onpeak = (pk) => { hold = hold.map((h, k) => Math.max(h * 0.97, pk[k])); drawMeter($("rec-meter"), pk, hold); };
    msg("ed-msg", "");
  } catch (e) { msg("ed-msg", why(e), "err"); }
}
async function recClose() {
  if (rin.recording) recStop();
  await rin.stop();
  $("rec-on").hidden = false; $("rec-live").hidden = true;
}
function recUi() {
  const on = rin.recording, b = $("rec-go");
  b.setAttribute("aria-pressed", on);
  b.textContent = on ? "■ STOP" : "● REC";
  $("rec-time").textContent = on && rin.armed ? "waiting for sound…" : `${rin.seconds.toFixed(1)} s of 30`;
}
function recStart() {
  rin.record({ maxS: 30, wait: $("rec-wait").checked ? 10 ** (-40 / 20) : 0, onfull: () => recStop() });
  recTimer = setInterval(recUi, 100);
  recUi();
}
function recStop() {
  clearInterval(recTimer);
  const take = rin.finish();
  recUi();
  if (!take) { msg("ed-msg", "Nothing came in: is the input's level moving?", "err"); return; }
  loadSource(take, `REC ${++recN}`);
}
function recInit() {
  $("rec-on").onclick = recOpen;
  $("rec-off").onclick = recClose;
  $("rec-go").onclick = () => (rin.recording ? recStop() : recStart());
  $("rec-in").onchange = async () => { try { await rin.start($("rec-in").value); rin.monitor($("rec-mon").checked); } catch (e) { msg("ed-msg", why(e), "err"); } };
  $("rec-mon").onchange = () => rin.monitor($("rec-mon").checked);
  navigator.mediaDevices?.addEventListener?.("devicechange", () => { if (rin.open) recInputs(); });
  app.on("take", (take, name) => { location.hash = "samples"; loadSource(take, name); });   // a take from the Sound page
}

function edSeconds() { return ed.src ? (ed.b - ed.a) * ed.src.channels[0].length / ed.src.rate : 0; }
function targetSlot() { return Number($("ed-slot").value); }

function edUpdate() {
  if (!ed.src) return;
  const sel = $("ed-slot"), keep = sel.value;
  sel.replaceChildren();
  for (const d of dir || []) {
    sel.append(el("option", { value: d.slot }, `${d.slot + 1}: ${d.state === "empty" ? "empty" : "replace " + d.name}`));
  }
  const firstFree = (dir || []).find((d) => d.state === "empty");
  sel.value = keep !== "" && keep != null && sel.querySelector(`[value="${keep}"]`) ? keep : firstFree ? firstFree.slot : 0;
  const s = edSeconds(), room = freeSectors(targetSlot()), box = $("ed-rates");
  box.replaceChildren();
  for (const r of RATES) {
    const n = sectorsFor(Math.round(s * r)), fits = n <= room;
    const lab = el("label", { class: "rate" + (fits ? "" : " nofit") },
      el("input", { type: "radio", name: "rate", value: r, checked: r === ed.rate, onchange: () => { ed.rate = r; edUpdate(); } }),
      el("b", {}, `${r === 11025 ? "11" : r === 22050 ? "22" : r / 1000} kHz`),
      el("span", {}, `${kb(n * 4096)}${fits ? "" : " · no room"}`),
      el("span", {}, r === 32000 ? "brightest" : r === 22050 ? "clear" : r === 16000 ? "warm, the usual" : "lo-fi, longest"));
    box.append(lab);
  }
  const n = sectorsFor(Math.round(s * ed.rate));
  $("ed-len").textContent = `${s.toFixed(2)} s selected · ${n} sector${n === 1 ? "" : "s"} of ${room} free`;
  $("ed-send").disabled = !app.connected || n > room || Math.round(s * ed.rate) < MIN_FRAMES;
  drawWave();
}

function edPcm() {                       // the sound exactly as it will be sent (before encoding)
  const s = ed.src, len = s.channels[0].length;
  return prepare(s.channels, s.rate, { target: ed.rate, normalize: $("ed-norm").checked, trim: $("ed-trim").checked,
                                       start: ed.a * len / s.rate, end: ed.b * len / s.rate });
}
async function edEncode() {
  const key = [ed.a, ed.b, ed.rate, $("ed-norm").checked, $("ed-trim").checked].join();
  if (ed.key === key && ed.enc) return ed.enc;
  ed.pcm = edPcm();
  msg("ed-msg", "Encoding…");
  const r = await encodeOff(ed.pcm);
  ed.enc = r; ed.key = key;
  $("ed-snr").textContent = `ADPCM ${r.snr.toFixed(0)} dB SNR`;
  msg("ed-msg", "");
  return r;
}

function play(samples, rate) {
  actx = actx || new AudioContext();
  playing?.stop();
  const b = actx.createBuffer(1, samples.length, rate), d = b.getChannelData(0);
  for (let i = 0; i < samples.length; i++) d[i] = samples[i] / 32768;
  const s = actx.createBufferSource();
  s.buffer = b; s.connect(actx.destination); s.start();
  playing = s;
}

function drawWave() {
  const c = $("wave"), g = c.getContext("2d"), W = c.width, H = c.height;
  g.fillStyle = "#000"; g.fillRect(0, 0, W, H);
  if (!ed.src) return;
  const ch = ed.src.channels, n = ch[0].length, A = ed.a * W, B = ed.b * W;
  for (let x = 0; x < W; x++) {
    let lo = 0, hi = 0;
    const i0 = Math.floor(x / W * n), i1 = Math.max(i0 + 1, Math.floor((x + 1) / W * n)), st = Math.max(1, (i1 - i0) >> 6);
    for (let i = i0; i < i1; i += st) for (const c2 of ch) { const v = c2[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
    g.fillStyle = x >= A && x <= B ? "#c8c8cc" : "#3a3a40";
    g.fillRect(x, H / 2 - hi * H / 2, 1, Math.max(1, (hi - lo) * H / 2));
  }
  g.fillStyle = "#30d07a"; g.fillRect(A - 2, 0, 4, H);
  g.fillStyle = "#e6e6e6"; g.fillRect(B - 2, 0, 4, H);
}
function waveDrag() {
  const c = $("wave");
  let which = null;
  const at = (e) => { const r = c.getBoundingClientRect(); return Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)); };
  c.addEventListener("pointerdown", (e) => { const x = at(e); which = Math.abs(x - ed.a) < Math.abs(x - ed.b) ? "a" : "b"; c.setPointerCapture(e.pointerId); move(e); });
  const move = (e) => {
    if (!which || !ed.src) return;
    const x = at(e), min = MIN_FRAMES / ed.src.channels[0].length * (ed.src.rate / ed.rate) * 1.2;
    if (which === "a") ed.a = Math.min(x, ed.b - min); else ed.b = Math.max(x, ed.a + min);
    edUpdate();
  };
  c.addEventListener("pointermove", move);
  c.addEventListener("pointerup", () => { which = null; });
}

async function send() {
  const slot = targetSlot(), d = dir[slot];
  if (d.state !== "empty" && !confirm(`Replace ${d.name} in slot ${slot + 1}?`)) return;
  $("ed-send").disabled = true;
  try {
    const { payload } = await edEncode();
    const name = slotName($("ed-name").value || ed.name);
    msg("ed-msg", `Sending ${name}… (the FM-1 shows RECEIVING SAMPLE; its sound pauses meanwhile)`);
    progress("ed-prog", 0);
    await app.ef.upload(slot, payload, { frames: ed.pcm.length, rate: ed.rate, name, crc: crc32(payload) },
      (b, t) => progress("ed-prog", b / t));
    await app.ef.set([[0xF2, slot]]);
    msg("ed-msg", `${name} is in slot ${slot + 1} and playing.`, "ok");
    ed.src = null; $("editor").hidden = true;
  } catch (e) { msg("ed-msg", e.message, "err"); }
  progress("ed-prog", null);
  refresh();
}

// ---------------------------------------------------------------- factory sounds
async function loadFactory() {
  try { factory = await (await fetch("factory/factory.json")).json(); } catch { factory = { sounds: [] }; }
  const box = $("factory");
  box.replaceChildren();
  for (const s of factory.sounds) {
    box.append(el("label", {}, el("input", { type: "checkbox", value: s.id, checked: s.starter, onchange: renderFactory }),
      el("span", {}, s.name), el("span", { class: "meta", "data-meta": s.id }, `${s.seconds} s · ${kb(s.sectors * 4096)}`)));
  }
  renderFactory();
}
function renderFactory() {
  if (!factory) return;
  const have = new Set((dir || []).filter((d) => d.state === "ok").map((d) => d.name));
  let need = 0, n = 0;
  for (const cb of $("factory").querySelectorAll("input")) {
    const s = factory.sounds.find((x) => x.id === cb.value), there = have.has(s.name);
    const meta = $("factory").querySelector(`[data-meta="${s.id}"]`);
    meta.className = there ? "there" : "meta";
    meta.textContent = there ? "on the FM-1" : `${s.seconds} s · ${kb(s.sectors * 4096)}`;
    cb.disabled = there;
    if (cb.checked && !there) { need += s.sectors; n++; }
  }
  const room = freeSectors();
  $("fac-size").textContent = !n ? "" : dir ? `${n} sounds · ${kb(need * 4096)} of ${kb(room * 4096)} free` : `${n} sounds · ${kb(need * 4096)}`;
  $("fac-install").disabled = !app.connected || !n || need > room || (dir || []).filter((d) => d.state === "empty").length < n;
}
async function installFactory() {
  const todo = [...$("factory").querySelectorAll("input:checked:not(:disabled)")].map((cb) => factory.sounds.find((x) => x.id === cb.value));
  $("fac-install").disabled = true;
  let i = 0;
  try {
    for (const s of todo) {
      const free = dir.find((d) => d.state === "empty");
      if (!free) throw new Error("no free slot left");
      msg("fac-msg", `${s.name} (${++i} of ${todo.length})…`);
      const pay = new Uint8Array(await (await fetch("factory/" + s.file)).arrayBuffer());
      if (crc32(pay) !== s.crc) throw new Error(`${s.file} is damaged (download the companion again)`);
      await app.ef.upload(free.slot, pay, { frames: s.frames, rate: s.rate, name: s.name, factory: true, crc: s.crc },
        (b, t) => progress("fac-prog", (i - 1 + b / t) / todo.length));
      free.state = "ok"; free.name = s.name;
    }
    msg("fac-msg", `Installed ${todo.length} sound${todo.length === 1 ? "" : "s"}.`, "ok");
  } catch (e) { msg("fac-msg", e.message, "err"); }
  progress("fac-prog", null);
  refresh();
}

// ---------------------------------------------------------------- backup and restore
async function backup() {
  $("bk-save").disabled = true;
  try {
    const files = [], used = dir.filter((d) => d.state === "ok"), total = used.reduce((s, d) => s + sectorsFor(d.frames), 0);
    let done = 0;
    const presets = [];
    for (let i = 0; i < 16; i++) presets.push(await app.ef.presetGet(i));
    const params = await app.ef.params();
    for (const d of used) {
      msg("bk-msg", `Reading ${d.name}…`);
      const pay = await readPayload(d, (b, t) => progress("bk-prog", (done + (b / t) * sectorsFor(d.frames)) / total));
      done += sectorsFor(d.frames);
      const base = `samples/${String(d.slot + 1).padStart(2, "0")}-${d.name.trim().replace(/[^A-Z0-9]+/g, "_") || "SAMPLE"}`;
      files.push({ name: base + ".efs", data: pay }, { name: base + ".wav", data: wav16(decode(pay, d.frames), d.rate) });
    }
    const meta = { format: "echofield-backup", version: 1, firmware: app.info.version, made: new Date().toISOString(),
                   samples: used.map((d) => ({ slot: d.slot, name: d.name, frames: d.frames, rate: d.rate, factory: d.factory,
                     file: `samples/${String(d.slot + 1).padStart(2, "0")}-${d.name.trim().replace(/[^A-Z0-9]+/g, "_") || "SAMPLE"}.efs` })),
                   presets: presets.filter((p) => p.used), params: { keys: PARAMS.map((p) => p.key), q: params.q, slot: params.slot } };
    files.unshift({ name: "echofield.json", data: new TextEncoder().encode(JSON.stringify(meta, null, 1)) });
    download(`echofield-backup-${new Date().toISOString().slice(0, 10)}.zip`, zip(files), "application/zip");
    msg("bk-msg", `Backed up ${used.length} samples and ${meta.presets.length} presets.`, "ok");
  } catch (e) { msg("bk-msg", e.message, "err"); }
  progress("bk-prog", null);
  $("bk-save").disabled = !app.connected;
}

async function restore(file) {
  try {
    const z = unzip(await file.arrayBuffer()), meta = JSON.parse(new TextDecoder().decode(z.get("echofield.json") || new Uint8Array()));
    if (meta.format !== "echofield-backup") throw new Error("not an ECHOFIELD backup");
    if (!confirm(`Restore ${meta.samples.length} samples and ${meta.presets.length} presets? Each goes back into its own slot, replacing what is there.`)) return;
    let i = 0;
    for (const s of meta.samples) {
      const pay = z.get(s.file);
      if (!pay || pay.length !== sectorsFor(s.frames) * SEC_PAYLOAD) throw new Error(`${s.file}: missing or the wrong size`);
      msg("bk-msg", `${s.name} (${++i} of ${meta.samples.length})…`);
      await app.ef.upload(s.slot, pay, { frames: s.frames, rate: s.rate, name: s.name, factory: s.factory, crc: crc32(pay) },
        (b, t) => progress("bk-prog", (i - 1 + b / t) / meta.samples.length));
    }
    for (const p of meta.presets) await app.ef.presetPut(p);
    if (meta.params?.keys) {             // by key, so a backup from another version lands on the right parameters
      const pairs = [];
      meta.params.keys.forEach((k, j) => { const id = PARAMS.findIndex((p) => p.key === k); if (id >= 0) pairs.push([id, meta.params.q[j]]); });
      await app.ef.set(pairs);
    }
    msg("bk-msg", "Restored.", "ok");
  } catch (e) { msg("bk-msg", e.message, "err"); }
  progress("bk-prog", null);
  refresh();
}

// ---------------------------------------------------------------- wiring
export function init() {
  recInit();
  const drop = $("drop"), file = $("file");
  file.addEventListener("change", () => file.files[0] && loadFile(file.files[0]));
  for (const ev of ["dragenter", "dragover"]) drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add("over"); });
  for (const ev of ["dragleave", "drop"]) drop.addEventListener(ev, () => drop.classList.remove("over"));
  drop.addEventListener("drop", (e) => { e.preventDefault(); if (e.dataTransfer.files[0]) loadFile(e.dataTransfer.files[0]); });
  waveDrag();
  $("ed-slot").addEventListener("change", edUpdate);
  for (const id of ["ed-norm", "ed-trim"]) $(id).addEventListener("change", edUpdate);
  $("ed-play-orig").addEventListener("click", () => {
    const s = ed.src, len = s.channels[0].length, a = Math.floor(ed.a * len), b = Math.floor(ed.b * len);
    play(Int16Array.from(s.channels[0].subarray(a, b), (v) => v * 32767), s.rate);
  });
  $("ed-play-fm1").addEventListener("click", async () => { const r = await edEncode(); play(decode(r.payload, ed.pcm.length), ed.rate); });
  $("ed-send").addEventListener("click", send);
  $("ed-cancel").addEventListener("click", () => { ed.src = null; $("editor").hidden = true; playing?.stop(); });
  $("fac-install").addEventListener("click", installFactory);
  $("bk-save").addEventListener("click", backup);
  $("bk-load").addEventListener("change", (e) => e.target.files[0] && restore(e.target.files[0]));
  app.on("connected", () => { $("bk-save").disabled = false; $("bk-load").disabled = false; refresh(); });
  app.on("disconnected", () => { dir = null; app.dir = null; $("bk-save").disabled = $("bk-load").disabled = true; render(); });
  app.on("refresh-bank", refresh);
  // right after a fresh install (Flash): the starter sounds, as if they came with the firmware
  app.on("install-starter", async () => {
    await refresh();
    for (const cb of $("factory").querySelectorAll("input")) cb.checked = !!factory.sounds.find((x) => x.id === cb.value)?.starter;
    renderFactory();
    if (!$("fac-install").disabled) await installFactory();
  });
  loadFactory();
  render();
}
