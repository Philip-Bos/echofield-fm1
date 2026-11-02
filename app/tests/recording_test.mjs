// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// The companion's recording, in a real browser: headless Chrome with its fake audio input (a test tone), driven over
// the DevTools protocol. Samples page: open an input, record, the take opens in the editor. Sound page: record the
// "FM-1" twice, the takes list, → Sample, the WAV writer. With a folder, it also saves screenshots of both (dark).
//   node --experimental-websocket app/tests/recording_test.mjs [SHOTS_DIR]   (the companion served on :8747)
//   REAL_AUDIO=1 ...: the computer's own audio inputs (for screenshots with a real FM-1; its checks may then fail)
import { spawn } from "node:child_process";
import { mkdtempSync, writeFileSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const CHROME = process.env.CHROME || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const URL0 = process.env.COMPANION || "http://localhost:8747/";
const SHOTS = process.argv[2];
const PORT = 9333;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0;
const check = (ok, what) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) fails++; };

const REAL = !!process.env.REAL_AUDIO;   // the computer's own inputs (an FM-1 plugged in) instead of Chrome's test tone
const chrome = spawn(CHROME, ["--headless=new", `--remote-debugging-port=${PORT}`, `--user-data-dir=${mkdtempSync(join(tmpdir(), "efrec-"))}`,
  ...(REAL ? [] : ["--use-fake-device-for-media-stream"]), "--use-fake-ui-for-media-stream", "--autoplay-policy=no-user-gesture-required",
  "--hide-scrollbars", "--window-size=1920,1080", "about:blank"], { stdio: "ignore" });
try {
  let target;
  for (let k = 0; k < 50 && !target; k++) {
    await sleep(200);
    try { target = (await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json()).find((t) => t.type === "page"); } catch { /* not up yet */ }
  }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => ws.addEventListener("open", r, { once: true }));
  let id = 0;
  const wait = new Map();
  ws.addEventListener("message", (e) => { const m = JSON.parse(e.data); if (m.id && wait.has(m.id)) { wait.get(m.id)(m); wait.delete(m.id); } });
  const send = (method, params = {}) => new Promise((r) => { wait.set(++id, r); ws.send(JSON.stringify({ id, method, params })); });
  const js = async (expr) => {
    const r = await send("Runtime.evaluate", { expression: `(async () => { ${expr} })()`, awaitPromise: true, returnByValue: true });
    if (r.result?.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description || "page error");
    return r.result?.result?.value;
  };
  const click = (sel) => js(`document.querySelector(${JSON.stringify(sel)}).click()`);
  const shot = async (name, sel) => {
    if (!SHOTS) return;
    await js(`document.getElementById("tk-msg").textContent = ""`);   // the fake input is no FM-1: its warning, not shown
    await js(`document.querySelector(${JSON.stringify(sel)}).scrollIntoView({ block: "center" })`);
    await sleep(400);
    const r = await send("Page.captureScreenshot", { format: "png" });
    mkdirSync(SHOTS, { recursive: true });
    writeFileSync(join(SHOTS, name), Buffer.from(r.result.data, "base64"));
    console.log(`     shot ${name}`);
  };

  await send("Emulation.setDeviceMetricsOverride", { width: 1920, height: 1080, deviceScaleFactor: 1, mobile: false });
  if (SHOTS) await send("Emulation.setEmulatedMedia", { features: [{ name: "prefers-color-scheme", value: "dark" }] });
  await send("Page.navigate", { url: URL0 + "#samples" });
  await sleep(1500);

  // ---- the Samples page: record one
  await js(`document.getElementById("rec-wait").checked = false`);
  await click("#rec-on");
  await sleep(1500);
  check(await js(`return !document.getElementById("rec-live").hidden`), "samples: the input opens");
  check(await js(`return document.getElementById("rec-in").options.length > 0`), "samples: the inputs are listed");
  await click("#rec-go");
  await sleep(2200);
  check(await js(`return document.getElementById("rec-go").getAttribute("aria-pressed") === "true"`), "samples: recording");
  await shot("companion-record-sample-live.png", "#add-card");
  await click("#rec-go");
  await sleep(500);
  check(await js(`return !document.getElementById("editor").hidden`), "samples: the take opens in the editor");
  check(await js(`return document.getElementById("ed-name").value.startsWith("REC 1")`), "samples: named REC 1");
  const secs = Number(await js(`return parseFloat(document.getElementById("ed-len").textContent)`));
  check(secs > 1.5 && secs < 3.5, `samples: about two seconds recorded (${secs} s)`);
  // the tone is in it: the editor's waveform is not flat
  check(await js(`const c = document.getElementById("wave"), d = c.getContext("2d").getImageData(0, 0, c.width, c.height).data;
    let lit = 0; for (let i = 0; i < d.length; i += 4) if (d[i] > 100) lit++; return lit > 500`), "samples: the take has sound in it");
  await shot("companion-record-sample.png", "#add-card");

  // ---- the Sound page: record the FM-1, twice
  await js(`location.hash = "sound"`);
  await sleep(500);
  await click("#tk-on");
  await sleep(1500);
  check(await js(`return !document.getElementById("tk-live").hidden`), "sound: the input opens");
  await click("#tk-mon");
  check(await js(`return document.getElementById("tk-mon").getAttribute("aria-pressed") === "true"`), "sound: monitor on");
  for (const ms of [1800, 1200]) { await click("#tk-rec"); await sleep(ms); await click("#tk-rec"); await sleep(300); }
  check(await js(`return document.querySelectorAll("#tk-list li").length === 2`), "sound: two takes listed");
  check(await js(`return !document.getElementById("tk-all").hidden`), "sound: download all offered");
  check(await js(`return /TAKE 2/.test(document.querySelector("#tk-list li").textContent)`), "sound: the newest first");
  await click("#tk-rec");
  await sleep(2600);
  await shot("companion-record-fm1-live.png", "#tk-card");
  await click("#tk-rec");
  await sleep(300);
  await click("#tk-mon");
  await shot("companion-record-fm1.png", "#tk-card");
  // the WAV writer: 24-bit, the right header and length
  check(await js(`const m = await import("/lib/audioin.js");
    const b = m.wav24({ rate: 48000, channels: [new Float32Array([0.5, -0.5, 0]), new Float32Array([0, 0.25, -1])] });
    const v = new DataView(b.buffer), s = (o) => String.fromCharCode(...b.slice(o, o + 4));
    return s(0) === "RIFF" && s(8) === "WAVE" && v.getUint16(22, true) === 2 && v.getUint32(24, true) === 48000 &&
      v.getUint16(34, true) === 24 && v.getUint32(40, true) === 18 && b.length === 62 && b[44] === 0 && b[45] === 0 && b[46] === 0x40`),
    "wav24: header and samples");
  // → Sample: the take opens on the Samples page
  await js(`[...document.querySelectorAll("#tk-list li")][1].querySelector("button[title^='make it']").click()`);
  await sleep(600);
  check(await js(`return location.hash === "#samples" && document.getElementById("ed-name").value.startsWith("TAKE 2")`),
    "sound: → Sample opens the take in the editor");
  ws.close();
} finally {
  chrome.kill();
}
console.log(fails ? `${fails} FAILED` : "recording: all passed");
process.exit(fails ? 1 : 0);
