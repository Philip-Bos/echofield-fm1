// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
//
// ECHOFIELD FM-1 sample codec (EFB1): the one encoder, used by the companion app in the browser and by the tools in
// Node. Blocked IMA ADPCM, mono: blocks of 256 samples, each a 4-byte header (predictor int16 LE, step index, 0: the
// decoder's state BEFORE the block's first sample) and 128 bytes of nibbles, low nibble first. 29 blocks make a
// sector's 3828-byte payload. The firmware's decoder (app/dsp/adpcm.h) matches decode() here bit for bit.
//
// The encoder searches two samples ahead (each nibble against the best next one), which buys several dB over the
// usual greedy IMA encoder with no change to the decoder.

export const BLK_SAMPLES = 256;
export const BLK_BYTES = 132;
export const SEC_BLOCKS = 29;
export const SEC_PAYLOAD = SEC_BLOCKS * BLK_BYTES;          // 3828
export const SEC_SAMPLES = SEC_BLOCKS * BLK_SAMPLES;        // 7424
export const RATES = [32000, 22050, 16000, 11025];
export const MIN_FRAMES = 512;

const STEP = new Int16Array([
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
  118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
  1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
  6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
  32767]);
const INDEX = new Int8Array([-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]);

// one decoder step: [pred, index] -> new pred (index updated through the returned pair)
function stepPred(pred, index, nib) {
  const step = STEP[index];
  let d = step >> 3;
  if (nib & 4) d += step;
  if (nib & 2) d += step >> 1;
  if (nib & 1) d += step >> 2;
  pred += nib & 8 ? -d : d;
  return pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
}
function stepIndex(index, nib) {
  index += INDEX[nib];
  return index < 0 ? 0 : index > 88 ? 88 : index;
}

export function sectorsFor(frames) { return Math.ceil(frames / SEC_SAMPLES); }
export function bytesFor(frames) { return sectorsFor(frames) * 4096; }   // what it takes in the FM-1's flash

// one block from a starting state: its nibbles into nib[], -> {err, pred, index} at its end
function encodeBlock(x, pred, index, lookahead, nib) {
  let err = 0;
  for (let j = 0; j < BLK_SAMPLES; j++) {
    const v = x[j];
    let best = 0, bestErr = Infinity;
    if (lookahead && j + 1 < BLK_SAMPLES) {
      const v1 = x[j + 1];
      for (let a = 0; a < 16; a++) {
        const p0 = stepPred(pred, index, a), e0 = (p0 - v) * (p0 - v);
        if (e0 >= bestErr) continue;
        const i0 = stepIndex(index, a);
        let e1 = Infinity;
        for (let c = 0; c < 16; c++) {
          const p1 = stepPred(p0, i0, c), d = (p1 - v1) * (p1 - v1);
          if (d < e1) e1 = d;
        }
        if (e0 + e1 < bestErr) { bestErr = e0 + e1; best = a; }
      }
    } else {
      for (let a = 0; a < 16; a++) {
        const p0 = stepPred(pred, index, a), e0 = (p0 - v) * (p0 - v);
        if (e0 < bestErr) { bestErr = e0; best = a; }
      }
    }
    pred = stepPred(pred, index, best);
    index = stepIndex(index, best);
    err += (pred - v) * (pred - v);
    nib[j] = best;
  }
  return { err, pred, index };
}

// the step index whose step suits a block's typical sample-to-sample change
function fitIndex(x, before) {
  let s = 0, prev = before;
  for (let j = 0; j < x.length; j++) { s += Math.abs(x[j] - prev); prev = x[j]; }
  const want = (s / x.length) * 1.5;
  let k = 0;
  while (k < 88 && STEP[k] < want) k++;
  return k;
}

// Int16Array -> Uint8Array payload (sectorsFor(n) * SEC_PAYLOAD bytes; past the end: silence). Each block's header
// may hold any starting state, so each block is encoded from a few (the state carried on, the true previous sample, a
// step fitted to the block) with both searches, and the closest result kept.
export function encode(pcm, { lookahead = true } = {}) {
  const n = pcm.length, nsec = sectorsFor(n), nblk = nsec * SEC_BLOCKS;
  const out = new Uint8Array(nsec * SEC_PAYLOAD), x = new Int32Array(BLK_SAMPLES);
  const nib = new Uint8Array(BLK_SAMPLES), bestNib = new Uint8Array(BLK_SAMPLES);
  let pred = 0, index = 0;
  for (let b = 0; b < nblk; b++) {
    const o = b * BLK_BYTES, i0 = b * BLK_SAMPLES, before = i0 > 0 && i0 - 1 < n ? pcm[i0 - 1] : 0;
    for (let j = 0; j < BLK_SAMPLES; j++) x[j] = i0 + j < n ? pcm[i0 + j] : 0;
    const fit = fitIndex(x, before), starts = [[pred, index]];
    if (b > 0) starts.push([before, index], [before, fit], [pred, fit]);
    let best = null;
    for (const [p, k] of starts) for (const la of lookahead ? [true, false] : [false]) {
      const r = encodeBlock(x, p, k, la, nib);
      if (!best || r.err < best.err) { best = { ...r, p, k }; bestNib.set(nib); }
    }
    out[o] = best.p & 0xFF; out[o + 1] = (best.p >> 8) & 0xFF; out[o + 2] = best.k; out[o + 3] = 0;
    for (let j = 0; j < BLK_SAMPLES; j += 2) out[o + 4 + (j >> 1)] = bestNib[j] | (bestNib[j + 1] << 4);
    pred = best.pred;
    index = best.index;
  }
  return out;
}

// payload -> Int16Array of frames samples
export function decode(payload, frames) {
  const out = new Int16Array(frames);
  for (let i = 0, b = 0; i < frames; b++) {
    const o = b * BLK_BYTES;
    let pred = (payload[o] | (payload[o + 1] << 8)) << 16 >> 16, index = Math.min(88, payload[o + 2]);
    for (let j = 0; j < BLK_SAMPLES && i < frames; j++, i++) {
      const byte = payload[o + 4 + (j >> 1)], nib = j & 1 ? byte >> 4 : byte & 15;
      pred = stepPred(pred, index, nib);
      index = stepIndex(index, nib);
      out[i] = pred;
    }
  }
  return out;
}

// ---- preparing audio ----

// zlib's CRC-32
const CRC_T = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return t;
})();
export function crc32(d, c = 0) {
  c = (c ^ 0xFFFFFFFF) >>> 0;
  for (let i = 0; i < d.length; i++) c = CRC_T[(c ^ d[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

function besselI0(x) {
  let s = 1, t = 1;
  for (let k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; }
  return s;
}

// windowed-sinc (Kaiser, beta 8.6) resampling of a Float32Array; the low-pass at 0.9 of the lower Nyquist
export function resample(x, from, to) {
  if (from === to) return Float32Array.from(x);
  const ratio = to / from, n = Math.max(1, Math.round(x.length * ratio)), out = new Float32Array(n);
  const cut = 0.9 * Math.min(1, ratio), half = 24 / Math.min(1, ratio) | 0, beta = 8.6, i0b = besselI0(beta);
  for (let k = 0; k < n; k++) {
    const c = k / ratio, i0 = Math.floor(c);
    let acc = 0, wsum = 0;
    for (let i = i0 - half + 1; i <= i0 + half; i++) {
      const t = c - i, u = t / half;
      if (u <= -1 || u >= 1) continue;
      const w = besselI0(beta * Math.sqrt(1 - u * u)) / i0b;
      const s = t === 0 ? cut : Math.sin(Math.PI * cut * t) / (Math.PI * t);
      const v = i >= 0 && i < x.length ? x[i] : 0;
      acc += v * s * w;
      wsum += s * w;
    }
    out[k] = wsum ? acc / wsum : 0;
  }
  return out;
}

// channels (Float32Array[]) at rate -> Int16Array ready to encode:
//   mono mix, optional trim of silence at the ends, optional region (seconds), resampling, normalising (to -0.5 dBFS
//   peak), short fades at both ends so a grain never starts on a click
export function prepare(channels, rate, opts = {}) {
  const { target = 22050, normalize = true, trim = true, start = 0, end = null, fadeMs = 4, maxSeconds = null } = opts;
  const len = channels[0].length;
  let mono = new Float32Array(len);
  for (const ch of channels) for (let i = 0; i < len; i++) mono[i] += ch[i] / channels.length;
  let a = Math.max(0, Math.floor(start * rate)), b = end == null ? len : Math.min(len, Math.floor(end * rate));
  if (trim) {
    let peak = 0;
    for (let i = a; i < b; i++) peak = Math.max(peak, Math.abs(mono[i]));
    const th = peak * 0.003;
    while (a < b && Math.abs(mono[a]) <= th) a++;
    while (b > a && Math.abs(mono[b - 1]) <= th) b--;
  }
  if (maxSeconds != null) b = Math.min(b, a + Math.floor(maxSeconds * rate));
  mono = mono.subarray(a, b);
  let y = resample(mono, rate, target);
  let peak = 0;
  for (const v of y) peak = Math.max(peak, Math.abs(v));
  const g = normalize && peak > 0 ? 0.944 / peak : 1;
  const fade = Math.min(Math.floor(y.length / 4), Math.round((fadeMs / 1000) * target));
  const out = new Int16Array(y.length);
  for (let i = 0; i < y.length; i++) {
    let v = y[i] * g;
    if (i < fade) v *= i / fade;
    if (i >= y.length - fade) v *= (y.length - 1 - i) / fade;
    out[i] = Math.max(-32767, Math.min(32767, Math.round(v * 32767)));
  }
  return out;
}

// signal-to-noise of an encoding, dB
export function snr(pcm, payload) {
  const d = decode(payload, pcm.length);
  let s = 0, e = 0;
  for (let i = 0; i < pcm.length; i++) { s += pcm[i] * pcm[i]; e += (pcm[i] - d[i]) * (pcm[i] - d[i]); }
  return e ? 10 * Math.log10(s / e) : 99;
}

// a short, safe name for the FM-1's screen: upper case, A-Z 0-9 space - . _, 11 characters
export function slotName(s) {
  return (String(s).replace(/\.[^.]*$/, "").toUpperCase().replace(/[^A-Z0-9 ._-]+/g, " ").replace(/\s+/g, " ").trim().slice(0, 11)) || "SAMPLE";
}

// a WAV file (PCM 8/16/24/32, float 32/64, extensible; any channels) -> {channels, rate}
export function parseWav(buf) {
  const u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf), dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const tag = (o) => String.fromCharCode(u8[o], u8[o + 1], u8[o + 2], u8[o + 3]);
  if (u8.length < 12 || tag(0) !== "RIFF" || tag(8) !== "WAVE") throw new Error("not a WAV file");
  let fmt = null, data = null;
  for (let o = 12; o + 8 <= u8.length;) {
    const id = tag(o), sz = dv.getUint32(o + 4, true);
    if (id === "fmt ") fmt = { o: o + 8, sz };
    if (id === "data") data = { o: o + 8, sz: Math.min(sz, u8.length - o - 8) };
    o += 8 + sz + (sz & 1);
  }
  if (!fmt || !data) throw new Error("WAV without fmt or data");
  let format = dv.getUint16(fmt.o, true);
  const nch = dv.getUint16(fmt.o + 2, true), rate = dv.getUint32(fmt.o + 4, true), bits = dv.getUint16(fmt.o + 14, true);
  if (format === 0xFFFE && fmt.sz >= 26) format = dv.getUint16(fmt.o + 24, true);
  const bps = bits / 8, frames = Math.floor(data.sz / (bps * nch));
  if (!nch || !rate || !frames) throw new Error("empty WAV");
  const ch = Array.from({ length: nch }, () => new Float32Array(frames));
  for (let i = 0; i < frames; i++) for (let c = 0; c < nch; c++) {
    const o = data.o + (i * nch + c) * bps;
    let v;
    if (format === 3) v = bits === 64 ? dv.getFloat64(o, true) : dv.getFloat32(o, true);
    else if (bits === 8) v = (u8[o] - 128) / 128;
    else if (bits === 16) v = dv.getInt16(o, true) / 32768;
    else if (bits === 24) v = ((u8[o] | (u8[o + 1] << 8) | (u8[o + 2] << 16)) << 8 >> 8) / 8388608;
    else if (bits === 32) v = dv.getInt32(o, true) / 2147483648;
    else throw new Error(`unsupported WAV: ${bits}-bit`);
    ch[c][i] = v;
  }
  return { channels: ch, rate };
}

// Int16Array -> a 16-bit mono WAV (exports, previews)
export function wav16(pcm, rate) {
  const b = new ArrayBuffer(44 + pcm.length * 2), dv = new DataView(b);
  const w = (o, s) => { for (let i = 0; i < 4; i++) dv.setUint8(o + i, s.charCodeAt(i)); };
  w(0, "RIFF"); dv.setUint32(4, 36 + pcm.length * 2, true); w(8, "WAVE"); w(12, "fmt ");
  dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, 1, true); dv.setUint32(24, rate, true);
  dv.setUint32(28, rate * 2, true); dv.setUint16(32, 2, true); dv.setUint16(34, 16, true); w(36, "data");
  dv.setUint32(40, pcm.length * 2, true);
  new Int16Array(b, 44).set(pcm);
  return new Uint8Array(b);
}
