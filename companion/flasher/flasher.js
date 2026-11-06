// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// The flasher: installing ECHOFIELD (or the stock firmware back), on a computer only, with fm1-base's proven update
// path (../vendor/fm1-base: the M-UPGRADE protocol and felucca-bsp's update loader) after checks of our own: the
// package's SHA-256 against the release, its identity, every part's checksum, the FM-1 found and named, the person's
// own checklist. After an install, the factory sounds go onto an FM-1 whose bank is empty.
import { Updater } from "../vendor/fm1-base/fm1ota.js";
import { productOf, logicalImage, checkUfw } from "../vendor/fm1-base/fm1pkg.js";
import { pairs, usualName } from "../vendor/fm1-base/fm1ports.js";
import { EfLink, EchoField, midiTransport } from "../lib/efproto.js";
import { crc32 } from "../lib/efcodec.js";

const $ = (id) => document.getElementById(id);
// ECHOFIELD: FM-1_3246…, and FM-1_4700… up to 0.2.7 (FM-1_47 is FRASIL's prefix, so only ECHOFIELD's own numbers)
const OURS = /^FM-1_(3246\d*|4700(\d{4})?)$/, STOCK = /^FM-1_0\d\d$/;
let pkg = null, stock = null, dev = null, busy = false, updater = null, access = null;

// ---------------------------------------------------------------- computers only
// Phones and tablets sleep, switch apps and lose USB power: no installs there. Every signal counts against it: the
// browser's own "mobile" flag, a phone or tablet user agent, an iPad that calls itself a Mac (touch points), and a
// device whose only pointer is a finger.
export function isMobile(nav = navigator, mm = (q) => matchMedia(q).matches) {
  const ua = nav.userAgent || "";
  if (nav.userAgentData?.mobile) return true;
  if (/Android|iPhone|iPad|iPod|Mobile|Silk|Kindle|webOS|BlackBerry|Opera Mini|IEMobile/i.test(ua)) return true;
  if (/Macintosh/.test(ua) && nav.maxTouchPoints > 1) return true;          // iPadOS asking for the desktop site
  if (mm("(pointer: coarse)") && !mm("(any-pointer: fine)")) return true;   // touch is the only pointer
  return false;
}

function msg(id, text, kind = "") { const m = $(id); m.textContent = text; m.className = "msg " + kind; }
function progress(frac) { const p = $("fw-prog"); p.hidden = frac == null; if (frac != null) p.firstElementChild.style.width = `${Math.round(Math.min(1, frac) * 100)}%`; }

async function sha256(buf) {
  const h = await crypto.subtle.digest("SHA-256", buf);
  return [...new Uint8Array(h)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function check(name, state, text) {
  const li = $("checks").querySelector(`[data-c="${name}"]`);
  li.classList.toggle("ok", state === true);
  li.classList.toggle("bad", state === false);
  if (text != null && name === "dev") $("dev-id").textContent = text;
}

// a package checked in every way we can before anything goes to the FM-1 -> {fwsc, image, product, sha}
async function readPackage(fwsc, want) {
  const product = productOf(fwsc), image = logicalImage(fwsc), sha = await sha256(fwsc);
  if (want.re && !want.re.test(product)) throw new Error(`this package is ${product}: ${want.what}`);
  if (want.sha && want.sha !== sha) throw new Error("the package's SHA-256 does not match the release: download it again");
  checkUfw(image);                       // the header, the list of parts, every part's CRC
  return { fwsc, image, product, sha };
}

async function loadBundled() {
  try {
    const m = await (await fetch("../firmware/manifest.json", { cache: "no-store" })).json();
    const fwsc = new Uint8Array(await (await fetch("../firmware/" + m.file, { cache: "no-store" })).arrayBuffer());
    pkg = await readPackage(fwsc, { re: OURS, sha: m.sha256, what: "not ECHOFIELD" });
    pkg.version = m.version;
    $("fw-ver").textContent = `${m.version} · ${pkg.product}`;
    $("fw-sha").textContent = "SHA-256 " + pkg.sha;
    check("pkg", true);
  } catch (e) {
    pkg = null;
    $("fw-ver").textContent = "no package bundled";
    $("fw-sha").textContent = e.message;
    check("pkg", false);
  }
  ready();
}

async function chooseFile(file) {
  try {
    pkg = await readPackage(new Uint8Array(await file.arrayBuffer()), { re: OURS, what: "not ECHOFIELD (for the stock firmware, use the section below)" });
    pkg.version = file.name;
    $("fw-ver").textContent = `${file.name} · ${pkg.product}`;
    $("fw-sha").textContent = "SHA-256 " + pkg.sha + " (a file you chose: compare it with its release)";
    check("pkg", true);
    msg("fw-msg", "");
  } catch (e) { pkg = null; check("pkg", false); msg("fw-msg", e.message, "err"); }
  ready();
}

async function midi() {
  if (access) return access;
  if (!navigator.requestMIDIAccess) throw new Error("This browser has no Web MIDI: use Chrome, Edge or Opera.");
  access = await navigator.requestMIDIAccess({ sysex: true });
  return access;
}

// who is on the USB port: ECHOFIELD, the stock firmware, another firmware, an update loader (an install to resume)
async function find() {
  if (busy) return;
  busy = true;
  $("fw-check").disabled = true;
  dev = null;
  try {
    await midi();
    check("midi", true);
    updater = updater || new Updater(access, (t) => console.log(t));
    const d = await updater.findAny(null);
    if (!d) { check("dev", false, "nothing answers (the cable? another app using it?)"); return; }
    d.link.close();
    dev = d.id;
    const loader = /^ota/i.test(dev.model);
    const what = OURS.test(dev.text) ? "ECHOFIELD" : STOCK.test(dev.text) ? "M-VAVE stock firmware"
      : loader ? "the update loader (an install was interrupted)" : "another firmware";
    check("dev", true, `${dev.text} — ${what}`);
    $("fw-resume").hidden = !loader;
    $("fw-backup").textContent = OURS.test(dev.text)
      ? "ECHOFIELD is already on it: an update keeps your samples, presets and settings."
      : loader ? "Press Resume to finish the install that was interrupted."
      : "Installing replaces this firmware. What it saved may be lost: back it up with its own tools first if you need it.";
  } catch (e) {
    check("midi", false);
    msg("fw-msg", e.message, "err");
  } finally {
    busy = false;
    $("fw-check").disabled = false;
    ready();
  }
}

function ready() {
  const manual = [...$("checks").querySelectorAll(".manual input")].every((c) => c.checked);
  const loader = /^ota/i.test(dev?.model || "");
  $("fw-install").disabled = busy || !pkg || !dev || loader || !manual;
  $("stock-install").disabled = busy || !stock || !dev || !manual;
}

const ORDER = ["start", "loader", "write", "reboot", "sounds", "done"];
const STEP = { start: "start", verify: "start", loader: "loader", write: "write", reboot: "reboot", sounds: "sounds", done: "done" };
// an install's steps (fm1ota.js Updater: name, then its numbers). "mode" says which install the loader takes: the fast
// one (only the sectors that changed, with its own percentage) or the classic one (every byte, counted here)
let fastMode = false;
function stepUi(name, b, total, pct) {
  if (name === "mode") {
    fastMode = b === "fast";
    $("fw-steps").querySelector(".pct").textContent = fastMode ? "(fast: only what changed)" : "";
    return;
  }
  const s = STEP[name];
  if (!s) return;
  $("fw-steps").hidden = false;
  for (const li of $("fw-steps").children) {
    const i = ORDER.indexOf(li.dataset.s), j = ORDER.indexOf(s);
    li.className = i < j ? "done" : i === j ? (s === "done" ? "done" : "now") : "";
  }
  if (name === "write" && (pct != null || total)) {
    const f = pct != null ? pct / 100 : Math.min(1, b / total);
    $("fw-steps").querySelector(".pct").textContent = `${Math.round(f * 100)} %${fastMode ? " (fast)" : ""}`;
    progress(f);
  }
}

const ERR = {
  notfound: "The FM-1 was not found. Check the cable and close other apps using it, then press Find the FM-1.",
  model: "That device is not an FM-1.",
  stopped: "The FM-1 stopped before anything was written. Nothing changed: try again.",
  lost: "The connection was lost during the install. Plug the FM-1 back in and press Resume: it is waiting in its update loader.",
  noloader: "The update loader did not appear. Replug the USB cable, press Find the FM-1, then Resume.",
  noreturn: "The firmware is written, but the FM-1 did not come back. Switch it off and on.",
  mismatch: "The FM-1 restarted with a different firmware than expected. Press Find the FM-1 to see what it runs.",
  badreq: "The FM-1 asked for data outside the package. Nothing more was sent: try again.",
  foreign: "The FM-1 is waiting in the update mode of M-VAVE's own firmware. Finish that update with M-UPGRADE first, or install M-VAVE's firmware below.",
};

async function guard(fn) {
  busy = true;
  ready();
  let lock = null;
  const stay = (e) => { e.preventDefault(); e.returnValue = ""; };
  addEventListener("beforeunload", stay);
  try { lock = await navigator.wakeLock?.request("screen"); } catch { /* not offered */ }
  try { await fn(); } finally {
    removeEventListener("beforeunload", stay);
    lock?.release?.();
    busy = false;
    ready();
  }
}

// ---------------------------------------------------------------- the sounds, after an install
async function echofield() {           // the FM-1 running ECHOFIELD, by its port and its answer
  for (let t = 0; t < 10; t++) {
    for (const p of pairs(access)) {
      if (!usualName(p.name) && !/echofield/i.test(p.name)) continue;
      if (/update|^ota|uboot/i.test(p.name)) continue;
      try { await p.input.open(); await p.output.open(); } catch { continue; }
      const link = new EfLink(midiTransport(p.input, p.output)), ef = new EchoField(link);
      if (await ef.hello()) return ef;
      link.close();
    }
    await new Promise((r) => setTimeout(r, 1000));
  }
  return null;
}

export async function installSounds(ef, choice, onProgress) {
  if (choice === "none") return { done: 0, why: "none chosen" };
  const dir = await ef.dir();
  if (dir.some((d) => d.state !== "empty")) return { done: 0, why: "the FM-1 already has samples: they are kept" };
  const all = (await (await fetch("../factory/factory.json")).json()).sounds;
  const list = all.filter((s) => choice === "all" || s.starter);
  let k = 0;
  for (const s of list) {
    const pay = new Uint8Array(await (await fetch("../factory/" + s.file)).arrayBuffer());
    if (crc32(pay) !== s.crc) throw new Error(`${s.file} is damaged: download the flasher again`);
    await ef.upload(k, pay, { frames: s.frames, rate: s.rate, name: s.name, factory: true, crc: s.crc },
      (b, t) => onProgress?.((k + b / t) / list.length, s.name));
    k++;
  }
  await ef.set([[0xF2, 0]]);
  return { done: k };
}

async function install(p, label) {
  await guard(async () => {
    msg("fw-msg", `Installing ${label}. Keep the cable in until this says Done.`);
    try {
      const total = p.image.length;
      const got = await updater.install(p.image, p.product, (name, n, b, pct) => stepUi(name, name === "mode" ? n : b, total, pct), p.sha);
      progress(null);
      dev = { text: got, model: "FM-1" };
      check("dev", true, got);
      if (OURS.test(got)) {
        stepUi("sounds");
        const choice = document.querySelector("input[name=snd]:checked").value;
        const ef = await echofield();
        if (!ef) msg("fw-msg", `Installed ${got}, but it did not answer for the sounds: put them on from the companion's Samples page.`, "err");
        else {
          const r = await installSounds(ef, choice, (f, name) => { progress(f); $("fw-steps").querySelector(".snd").textContent = name; });
          ef.link.close();
          progress(null);
          stepUi("done");
          msg("fw-msg", r.done ? `Done. ECHOFIELD ${got} runs, with ${r.done} sounds. Press PLAY.` : `Done. ECHOFIELD ${got} runs (sounds: ${r.why}).`, "ok");
        }
      } else {
        stepUi("done");
        msg("fw-msg", `Done. The FM-1 runs ${got}.`, "ok");
      }
    } catch (e) {
      progress(null);
      msg("fw-msg", ERR[e.code] || e.message, "err");
      if (e.code === "lost" || e.code === "noloader") $("fw-resume").hidden = false;
    }
  });
}

async function resume() {
  if (!pkg) { msg("fw-msg", "Choose the package that was being installed first.", "err"); return; }
  await guard(async () => {
    msg("fw-msg", "Resuming…");
    try {
      const ok = await updater.resume(pkg.image, (name, n, b, pct) => stepUi(name, name === "mode" ? n : b, pkg.image.length, pct), pkg.product);
      progress(null);
      if (ok) { msg("fw-msg", "Done.", "ok"); $("fw-resume").hidden = true; stepUi("done"); }
      else msg("fw-msg", "No update loader found. Press Find the FM-1.", "err");
    } catch (e) { msg("fw-msg", ERR[e.code] || e.message, "err"); }
  });
}

function init() {
  if (isMobile()) { $("refuse").hidden = false; return; }
  $("app").hidden = false;
  check("midi", navigator.requestMIDIAccess && isSecureContext ? null : false);
  for (const c of $("checks").querySelectorAll(".manual input")) c.addEventListener("change", ready);
  $("fw-check").addEventListener("click", find);
  $("fw-file").addEventListener("change", (e) => e.target.files[0] && chooseFile(e.target.files[0]));
  $("fw-install").addEventListener("click", () => pkg && install(pkg, `ECHOFIELD ${pkg.version}`));
  $("fw-resume").addEventListener("click", resume);
  $("stock-file").addEventListener("change", async (e) => {
    const f = e.target.files[0];
    stock = null;
    if (f) try {
      stock = await readPackage(new Uint8Array(await f.arrayBuffer()), { re: STOCK, what: "not M-VAVE's FM-1 firmware" });
      msg("stock-msg", `M-VAVE's FM-1 firmware, ${stock.product}. Press Find the FM-1 above, then Install.`, "ok");
    } catch (err) { msg("stock-msg", err.message, "err"); }
    ready();
  });
  $("stock-install").addEventListener("click", () => stock && install(stock, `the stock firmware ${stock.product}`));
  loadBundled();
}
if (typeof document !== "undefined" && document.getElementById("app")) init();
