// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// RECORD THE FM-1 (on the Sound page): ECHOFIELD's sound comes into the computer over USB (the FM-1 is an audio input);
// here it is monitored and recorded, take after take, each one a WAV to download or to send to the Samples page
// as a new sound. The input is opened as in MIDI Master: the FM-1's own device, the browser's voice processing off.
import { app, $, el, msg, download } from "./app.js";
import { AudioIn, listInputs, isFm1, why, drawMeter, wav24 } from "./lib/audioin.js";
import { zip } from "./lib/zip.js";

const MAX_S = 300;                        // five minutes a take (held in memory as it is recorded)
const tin = new AudioIn();
const list = [];
let n = 0, timer = 0, hold = [0, 0], playing = null;

const mmss = (s) => `${String(Math.floor(s / 60)).padStart(2, "0")}:${String(Math.floor(s % 60)).padStart(2, "0")}`;
const stamp = () => new Date().toISOString().slice(0, 19).replace(/[-:]/g, "").replace("T", "-");

async function inputs() {
  const sel = $("tk-in"), keep = sel.value;
  const ins = await listInputs();
  sel.replaceChildren(...ins.map((d) => el("option", { value: d.deviceId }, (d.label || "audio input") + (isFm1(d) ? "  (FM-1)" : ""))));
  const fm1 = ins.find(isFm1);
  sel.value = keep && ins.some((d) => d.deviceId === keep) ? keep : fm1 ? fm1.deviceId : ins[0]?.deviceId || "";
  return { ins, fm1 };
}

async function open() {
  try {
    const { fm1 } = await inputs();
    await tin.start($("tk-in").value);
    tin.onpeak = (pk) => { hold = hold.map((h, k) => Math.max(h * 0.97, pk[k])); drawMeter($("tk-meter"), pk, hold); };
    tin.onended = () => { if (tin.recording) stop(); msg("tk-msg", "The input went away (unplugged?).", "err"); };
    $("tk-on").hidden = true; $("tk-live").hidden = false;
    msg("tk-msg", fm1 ? "" : "No FM-1 among the audio inputs: plug it in with a data cable, then pick it above.", fm1 ? "" : "err");
  } catch (e) { msg("tk-msg", why(e), "err"); }
}

async function close() {
  if (tin.recording) stop();
  monitor(false);
  await tin.stop();
  $("tk-on").hidden = false; $("tk-live").hidden = true;
}

function monitor(on) {
  tin.monitor(on);
  $("tk-mon").setAttribute("aria-pressed", on);
}

function ui() {
  const on = tin.recording, b = $("tk-rec");
  b.setAttribute("aria-pressed", on);
  b.textContent = on ? "■ STOP" : "● REC";
  $("tk-time").textContent = on ? `${mmss(tin.seconds)} / ${mmss(MAX_S)}` : "00:00";
}

function start() {
  tin.record({ maxS: MAX_S, onfull: () => stop() });
  timer = setInterval(ui, 250);
  ui();
}

function stop() {
  clearInterval(timer);
  const take = tin.finish();
  ui();
  if (!take) return;
  const shown = $("stg-name").textContent.trim(), preset = /no sound/i.test(shown) ? "" : shown;   // the sound on the stage
  list.unshift({ name: `echofield-${stamp()}${preset ? "-" + preset.toLowerCase().replace(/[^a-z0-9]+/g, "-") : ""}`, take,
                 secs: take.channels[0].length / take.rate, n: ++n });
  render();
}

function play(t) {
  if (playing) { playing.src.stop(); const was = playing.t; playing = null; render(); if (was === t) return; }
  const ctx = tin.ctx || new AudioContext();
  const b = ctx.createBuffer(t.take.channels.length, t.take.channels[0].length, t.take.rate);
  t.take.channels.forEach((c, k) => b.copyToChannel(c, k));
  const src = ctx.createBufferSource();
  src.buffer = b; src.connect(ctx.destination); src.start();
  src.onended = () => { if (playing?.src === src) { playing = null; render(); } };
  playing = { src, t };
  render();
}

function render() {
  $("tk-list").replaceChildren(...list.map((t) => el("li", {},
    el("span", { class: "num" }, `TAKE ${t.n}`),
    el("span", { class: "nm" }, t.name, el("span", { class: "meta" }, `${mmss(t.secs)} · ${Math.round(t.take.rate / 100) / 10} kHz · ${t.take.channels.length === 2 ? "stereo" : "mono"}`)),
    el("div", { class: "acts" },
      el("button", { type: "button", title: "play it here", onclick: () => play(t) }, playing?.t === t ? "■" : "▶"),
      el("button", { type: "button", title: "download as WAV (24-bit)", onclick: () => download(`${t.name}.wav`, wav24(t.take), "audio/wav") }, "⤓ WAV"),
      el("button", { type: "button", title: "make it a new sound on the Samples page (trim it there)",
        onclick: () => app.emit("take", t.take, `TAKE ${t.n}`) }, "→ Sample"),
      el("button", { type: "button", title: "forget this take", onclick: () => { if (playing?.t === t) play(t); list.splice(list.indexOf(t), 1); render(); } }, "✕")))));
  $("tk-all").hidden = list.length < 2;
}

export function init() {
  $("tk-on").onclick = open;
  $("tk-off").onclick = close;
  $("tk-rec").onclick = () => (tin.recording ? stop() : start());
  $("tk-mon").onclick = () => monitor($("tk-mon").getAttribute("aria-pressed") !== "true");
  $("tk-in").onchange = async () => {
    try { await tin.start($("tk-in").value); tin.monitor($("tk-mon").getAttribute("aria-pressed") === "true"); }
    catch (e) { msg("tk-msg", why(e), "err"); }
  };
  $("tk-zip").onclick = () => download(`echofield-takes-${stamp()}.zip`, zip(list.map((t) => ({ name: `${t.name}.wav`, data: wav24(t.take) }))), "application/zip");
  navigator.mediaDevices?.addEventListener?.("devicechange", () => { if (tin.open) inputs(); });
  addEventListener("beforeunload", (e) => { if (list.length || tin.recording) e.preventDefault(); });
}
