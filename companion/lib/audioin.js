// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
//
// Audio in: a microphone, an audio interface, or an FM-1 over USB (ECHOFIELD itself, or another firmware such as Sloop),
// opened the way MIDI Master does it (one exact device, the browser's voice processing off: no echo cancelling, no
// noise suppression, no automatic gain), then recorded without loss: an AudioWorklet hands over the raw samples (not
// MediaRecorder's compressed audio), so a take can become an FM-1 sample or a WAV file as it is.

const TAP = `class Tap extends AudioWorkletProcessor {
  constructor() { super(); this.on = false; this.port.onmessage = (e) => { this.on = e.data; }; this.n = 0; this.pk = [0, 0]; }
  process(inputs) {
    const ch = inputs[0];
    if (!ch || !ch.length) return true;
    const l = ch[0], r = ch[1] || ch[0];
    for (let i = 0; i < l.length; i++) { const a = Math.abs(l[i]), b = Math.abs(r[i]); if (a > this.pk[0]) this.pk[0] = a; if (b > this.pk[1]) this.pk[1] = b; }
    if (this.on) this.port.postMessage([l.slice(), r.slice()]);
    if ((this.n += l.length) >= 1024) { this.port.postMessage({ peak: this.pk }); this.pk = [0, 0]; this.n = 0; }
    return true;
  }
}
registerProcessor("ef-tap", Tap);`;

// the inputs, named (the browser only names them once the page may use the microphone: asked here once)
export async function listInputs() {
  if (!navigator.mediaDevices?.enumerateDevices) return [];
  let ins = (await navigator.mediaDevices.enumerateDevices()).filter((d) => d.kind === "audioinput");
  if (ins.length && !ins.some((d) => d.label)) {
    (await navigator.mediaDevices.getUserMedia({ audio: true })).getTracks().forEach((t) => t.stop());
    ins = (await navigator.mediaDevices.enumerateDevices()).filter((d) => d.kind === "audioinput");
  }
  return ins.filter((d) => d.deviceId !== "communications");
}

// an FM-1's USB audio, by its name (felucca-bsp names it after the board; some systems say "M-VAVE" or "FM-1")
export function isFm1(d) { return /felucca|fm-?1\b|m-?vave/i.test(d.label || ""); }

export function why(e) {
  return e?.name === "NotAllowedError" ? "The browser was not allowed to use audio inputs (the camera/microphone icon in the address bar)."
    : e?.name === "NotFoundError" ? "That audio input is gone: is it still plugged in?"
    : e?.name === "NotReadableError" ? "That audio input is busy: another program is using it."
    : !window.isSecureContext ? "Audio input needs https or localhost."
    : String(e?.message || e);
}

export class AudioIn {
  constructor() { this.ctx = null; this.stream = null; this.src = null; this.tap = null; this.mon = null; this.dev = ""; this.peak = [0, 0];
    this.take = null; this.onpeak = null; this.onchunk = null; }
  get open() { return !!this.stream; }
  get rate() { return this.ctx ? this.ctx.sampleRate : 48000; }

  async start(deviceId) {
    await this.stop();
    this.ctx = this.ctx || new AudioContext({ latencyHint: "interactive" });
    if (this.ctx.state !== "running") await this.ctx.resume();
    if (!this.tapReady) {
      const url = URL.createObjectURL(new Blob([TAP], { type: "text/javascript" }));
      await this.ctx.audioWorklet.addModule(url);
      URL.revokeObjectURL(url);
      this.tapReady = true;
    }
    this.stream = await navigator.mediaDevices.getUserMedia({ video: false, audio: {
      deviceId: deviceId ? { exact: deviceId } : undefined, channelCount: { ideal: 2 },
      echoCancellation: false, noiseSuppression: false, autoGainControl: false } });
    this.dev = deviceId || "";
    this.src = this.ctx.createMediaStreamSource(this.stream);
    this.tap = new AudioWorkletNode(this.ctx, "ef-tap", { numberOfInputs: 1, numberOfOutputs: 0, channelCount: 2, channelCountMode: "explicit" });
    this.tap.port.onmessage = (e) => {
      if (e.data.peak) { this.peak = e.data.peak; this.onpeak?.(this.peak); return; }
      this.onchunk?.(e.data);
      if (this.take) this.add(e.data);
    };
    this.src.connect(this.tap);
    this.mon = this.ctx.createGain();
    this.mon.gain.value = 0;
    this.src.connect(this.mon).connect(this.ctx.destination);
    this.stream.getAudioTracks()[0].addEventListener("ended", () => this.onended?.());
  }

  async stop() {
    this.tap?.port.postMessage(false);
    try { this.src?.disconnect(); this.mon?.disconnect(); } catch { /* gone already */ }
    this.stream?.getTracks().forEach((t) => t.stop());
    this.stream = this.src = this.tap = this.mon = null;
    this.peak = [0, 0];
  }

  monitor(on) { if (this.mon) this.mon.gain.setTargetAtTime(on ? 1 : 0, this.ctx.currentTime, 0.02); }

  // a take: up to maxS seconds; wait = a threshold: nothing is kept until the sound gets that loud (with 50 ms before it)
  record({ maxS = 30, wait = 0, onfull } = {}) {
    this.take = { l: [], r: [], n: 0, max: Math.round(maxS * this.rate), wait, pre: [], onfull, t0: performance.now() };
    this.tap.port.postMessage(true);
  }
  add([l, r]) {
    const t = this.take;
    if (t.wait) {                                 // armed: keep the last 50 ms, start when it gets loud
      t.pre.push([l, r]);
      while (t.pre.length * 128 > this.rate * 0.05) t.pre.shift();
      let pk = 0;
      for (let i = 0; i < l.length; i++) pk = Math.max(pk, Math.abs(l[i]), Math.abs(r[i]));
      if (pk < t.wait) return;
      t.wait = 0;
      const pre = t.pre; t.pre = [];
      for (const c of pre) this.add(c);
      return;
    }
    t.l.push(l); t.r.push(r); t.n += l.length;
    if (t.n >= t.max) { const f = t.onfull; f?.(); }
  }
  get recording() { return !!this.take; }
  get armed() { return !!this.take?.wait; }
  get seconds() { return this.take ? this.take.n / this.rate : 0; }

  finish() {                                       // the take: {rate, channels: [L, R]}, null when nothing came in
    const t = this.take;
    this.take = null;
    this.tap?.port.postMessage(false);
    if (!t || !t.n) return null;
    const n = Math.min(t.n, t.max), L = new Float32Array(n), R = new Float32Array(n);
    let o = 0;
    for (let k = 0; k < t.l.length && o < n; k++) {
      const m = Math.min(t.l[k].length, n - o);
      L.set(t.l[k].subarray(0, m), o); R.set(t.r[k].subarray(0, m), o); o += m;
    }
    const mono = this.stream?.getAudioTracks()[0]?.getSettings().channelCount === 1;
    return { rate: this.rate, channels: mono ? [L] : [L, R] };
  }
}

// a take as a WAV file: 24-bit, the take's own rate and channels
export function wav24(take) {
  const ch = take.channels, n = ch[0].length, nc = ch.length, bytes = n * nc * 3;
  const b = new Uint8Array(44 + bytes), v = new DataView(b.buffer);
  const str = (o, s) => { for (let i = 0; i < s.length; i++) b[o + i] = s.charCodeAt(i); };
  str(0, "RIFF"); v.setUint32(4, 36 + bytes, true); str(8, "WAVE"); str(12, "fmt ");
  v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, nc, true); v.setUint32(24, take.rate, true);
  v.setUint32(28, take.rate * nc * 3, true); v.setUint16(32, nc * 3, true); v.setUint16(34, 24, true);
  str(36, "data"); v.setUint32(40, bytes, true);
  let o = 44;
  for (let i = 0; i < n; i++) for (let c = 0; c < nc; c++) {
    const s = Math.max(-8388608, Math.min(8388607, Math.round(ch[c][i] * 8388608)));
    b[o++] = s & 255; b[o++] = (s >> 8) & 255; b[o++] = (s >> 16) & 255;
  }
  return b;
}

// a level meter: two bars (dBFS, -60 to 0), red over -1 dB
export function drawMeter(canvas, peak, hold) {
  const g = canvas.getContext("2d"), W = canvas.width, H = canvas.height, h = (H - 2) / 2;
  g.clearRect(0, 0, W, H);
  peak.forEach((p, k) => {
    const db = 20 * Math.log10(Math.max(p, 1e-6)), x = Math.max(0, Math.min(1, (db + 60) / 60)) * W;
    g.fillStyle = "rgba(128,128,136,.25)"; g.fillRect(0, k * (h + 2), W, h);
    g.fillStyle = db > -1 ? "#ff3c3c" : db > -9 ? "#ff7a1a" : "#30d07a"; g.fillRect(0, k * (h + 2), x, h);
    if (hold) { const hx = Math.max(0, Math.min(1, (20 * Math.log10(Math.max(hold[k], 1e-6)) + 60) / 60)) * W; g.fillStyle = "#ececec"; g.fillRect(hx - 1, k * (h + 2), 2, h); }
  });
}
