// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// SCREEN: the FM-1's display, pixel for pixel (fm1-base's mirror), and its panel played from here.
import { app, $, el, msg, download } from "./app.js";
import { createMirror, mrRequest, mrRemote } from "./vendor/fm1-base/fm1mirror.js";

const BTN = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"];
const ENC = ["SELECT", "ALGO", "PRESETS", "K1", "K2", "K3", "K4"];
const WK = [0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26], BK = [1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25];
const ROWS = [["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "OCT-"], ["HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT+"]];

let mirror = null, unlisten = null, dirty = false, lastHeard = 0, painted = 0, control = false, seq = 0;
let ctx = null, img = null, bytes0 = 0, painted0 = 0;
const rq = [];
let rbusy = false, rtimer = 0;

function paint() {
  if (dirty && mirror) {
    const s = mirror.screen, d = img.data;
    for (let i = 0; i < 240 * 240; i++) {
      const c = s[i], r = c >> 11, g = (c >> 5) & 63, b = c & 31;
      d[4 * i] = (r << 3) | (r >> 2); d[4 * i + 1] = (g << 2) | (g >> 4); d[4 * i + 2] = (b << 3) | (b >> 2); d[4 * i + 3] = 255;
    }
    ctx.putImageData(img, 0, 0);
    dirty = false;
    painted++;
  }
  requestAnimationFrame(paint);
}

// through the link's queue, a short gap after each: the FM-1 holds one incoming frame at a time
function send(bytes) {
  const link = app.link;
  if (link) link.exclusive(async () => { link.send(Array.from(bytes)); await new Promise((r) => setTimeout(r, 12)); });
}

function start() {
  if (!app.connected || unlisten) return;
  mirror = createMirror({
    rect: () => { dirty = true; },
    gap: () => send(mrRequest(2)),
    input: (s) => lights(s),
    hello: (h) => { if (h.version < 2) msg("mr-msg", "This firmware's mirror has no remote control."); },
    remote: (r) => { if (r.seq === (seq & 255)) { rbusy = false; clearTimeout(rtimer); pump(); } },
  });
  const fn = (pkt) => { lastHeard = performance.now(); mirror.feed(pkt); };
  app.link.listeners.add(fn);
  unlisten = () => app.link?.listeners.delete(fn);
  send(mrRequest(2));
  msg("mr-msg", "");
  $("panel").classList.toggle("off", !control);
}
function stop() {
  if (!unlisten) return;
  if (control) send(mrRemote(++seq, 0));
  send(mrRequest(0));
  unlisten();
  unlisten = null;
}

// the remote: one request at a time, the next after the FM-1's answer
function remote(op, a, b = 0) { if (control && unlisten) { rq.push([op, a, b]); pump(); } }
function pump() {
  if (rbusy || !rq.length) return;
  const [op, a, b] = rq.shift();
  rbusy = true;
  send(mrRemote(++seq, op, a, b));
  rtimer = setTimeout(() => { rbusy = false; pump(); }, 400);
}

function lights(s) {
  for (const b of $("panel").querySelectorAll("[data-btn]")) {
    const i = Number(b.dataset.btn), lit = (s.litButtons >> i) & 1;
    b.classList.toggle("lit", !!lit || !!((s.buttons >> i) & 1));
    b.classList.toggle("play", BTN[i] === "PLAY" && !s.playRed);
  }
  for (const k of $("panel").querySelectorAll("[data-key]")) {
    const i = Number(k.dataset.key);
    k.classList.toggle("lit", !!(((s.litKeys >>> i) & 1) || ((s.keys >>> i) & 1)));
  }
}

function buildPanel() {
  const p = $("panel");
  const encs = el("div", { class: "encs" });
  ENC.forEach((n, i) => {
    const dial = el("span", { class: "dial", tabindex: 0, role: "spinbutton", "aria-label": n });
    let y0 = 0, acc = 0;
    dial.addEventListener("pointerdown", (e) => { y0 = e.clientY; acc = 0; dial.setPointerCapture(e.pointerId); });
    dial.addEventListener("pointermove", (e) => {
      if (!dial.hasPointerCapture(e.pointerId)) return;
      acc += y0 - e.clientY; y0 = e.clientY;
      while (Math.abs(acc) >= 12) { remote(3, i, acc > 0 ? 1 : 255); acc -= Math.sign(acc) * 12; }
    });
    dial.addEventListener("wheel", (e) => { e.preventDefault(); remote(3, i, e.deltaY < 0 ? 1 : 255); }, { passive: false });
    dial.addEventListener("keydown", (e) => { if (e.key === "ArrowUp" || e.key === "ArrowRight") remote(3, i, 1); if (e.key === "ArrowDown" || e.key === "ArrowLeft") remote(3, i, 255); });
    encs.append(el("div", { class: "enc" }, dial, n));
  });
  const btns = el("div", { class: "btns" });
  for (const row of ROWS) for (const n of row) {
    const i = BTN.indexOf(n), b = el("button", { type: "button", "data-btn": i }, n);
    hold(b, 1, i);
    btns.append(b);
  }
  const keys = el("div", { class: "keys" }), ww = 100 / 16;
  WK.forEach((k, j) => { const b = el("button", { type: "button", class: "w", "data-key": k, "aria-label": `key ${k + 1}`, style: `left:${j * ww}%;width:${ww - 0.4}%` }); hold(b, 2, k); keys.append(b); });
  const after = [0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14];
  BK.forEach((k, j) => { const b = el("button", { type: "button", class: "b", "data-key": k, "aria-label": `key ${k + 1}`, style: `left:${(after[j] + 0.65) * ww}%;width:${ww * 0.7}%` }); hold(b, 2, k); keys.append(b); });
  p.append(encs, btns, keys);
}
function hold(e, op, id) {               // a button or a key: down while the pointer is
  let down = false;
  const up = () => { if (down) { down = false; remote(op, id, 0); } };
  e.addEventListener("pointerdown", (ev) => { ev.preventDefault(); down = true; e.setPointerCapture(ev.pointerId); remote(op, id, 1); });
  e.addEventListener("pointerup", up);
  e.addEventListener("pointercancel", up);
  e.addEventListener("lostpointercapture", up);
}

export function init() {
  const c = $("mr-canvas");
  ctx = c.getContext("2d");
  img = ctx.createImageData(240, 240);
  ctx.fillStyle = "#000"; ctx.fillRect(0, 0, 240, 240);
  buildPanel();
  requestAnimationFrame(paint);
  $("mr-control").addEventListener("change", (e) => {
    control = e.target.checked;
    $("panel").classList.toggle("off", !control);
    if (!control) send(mrRemote(++seq, 0));          // let everything go
  });
  $("panel").classList.add("off");
  $("mr-shot").addEventListener("click", () => {
    const big = document.createElement("canvas");
    big.width = big.height = 480;
    const g = big.getContext("2d");
    g.imageSmoothingEnabled = false;
    g.drawImage(c, 0, 0, 480, 480);
    big.toBlob((b) => download(`echofield-screen-${Date.now()}.png`, b, "image/png"));
  });
  app.on("tab", (t) => (t === "screen" ? start() : stop()));
  app.on("connected", () => { if (!$("tab-screen").hidden) start(); });
  app.on("disconnected", () => { unlisten = null; msg("mr-msg", "Disconnected."); });
  setInterval(() => {                     // the keep-alive (the FM-1 stops mirroring after 6 s without one)
    if (!unlisten || app.link?.busy) return;   // (never while a transfer has the FM-1: it holds one frame)
    if (performance.now() - lastHeard > 4000) { mirror.resync(); send(mrRequest(2)); } else send(mrRequest(1));
  }, 2000);
  setInterval(() => {
    if (!mirror) return;
    const b = mirror.stats.bytes - bytes0, f = painted - painted0;
    bytes0 = mirror.stats.bytes; painted0 = painted;
    $("mr-rate").textContent = unlisten ? `${f} fps · ${(b / 1024).toFixed(1)} kB/s` : "";
  }, 1000);
}
