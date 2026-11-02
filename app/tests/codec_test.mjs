// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// efcodec.js against the firmware's decoder (tests/efdec.c), bit for bit; the encoder's quality; the WAV reader.
import { encode, decode, snr, prepare, parseWav, wav16, sectorsFor, SEC_PAYLOAD, crc32 } from "../../companion/lib/efcodec.js";
import { execFileSync } from "node:child_process";
import { writeFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

let fails = 0;
const check = (c, m) => { if (!c) { fails++; console.log("  FAIL " + m); } };
const efdec = process.argv[2];
const dir = mkdtempSync(join(tmpdir(), "efcodec-"));

function signal(n, kind) {
  const x = new Int16Array(n);
  let seed = 1;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296) * 2 - 1;
  for (let i = 0; i < n; i++) {
    const t = i / 22050;
    let v = 0;
    if (kind === "chord") v = 0.3 * (Math.sin(2 * Math.PI * 220 * t) + Math.sin(2 * Math.PI * 277 * t) + Math.sin(2 * Math.PI * 330 * t)) * Math.exp(-(t % 0.7) * 3);
    if (kind === "noise") v = rnd() * 0.5;
    if (kind === "clicks") v = (i % 3000) < 3 ? 0.99 : 0.01 * Math.sin(i);
    if (kind === "full") v = Math.sin(2 * Math.PI * 9000 * t) * 0.999;
    x[i] = Math.round(Math.max(-1, Math.min(1, v)) * 32767);
  }
  return x;
}

for (const [kind, n] of [["chord", 22050 * 2 + 77], ["noise", 9000], ["clicks", 7424 * 2], ["full", 600]]) {
  const pcm = signal(n, kind);
  const pay = encode(pcm), greedy = encode(pcm, { lookahead: false });
  check(pay.length === sectorsFor(n) * SEC_PAYLOAD, `${kind}: payload length`);
  const js = decode(pay, n);
  const f = join(dir, kind + ".bin");
  writeFileSync(f, pay);
  const c = new Int16Array(new Uint8Array(execFileSync(efdec, [f, String(n)], { maxBuffer: 1 << 26 })).buffer);
  let diff = 0;
  for (let i = 0; i < n; i++) if (c[i] !== js[i]) diff++;
  check(c.length === n && diff === 0, `${kind}: C and JS decoders differ in ${diff} of ${n} samples`);
  const a = snr(pcm, pay), g = snr(pcm, greedy);
  console.log(`${kind.padEnd(7)} SNR lookahead ${a.toFixed(1)} dB, greedy ${g.toFixed(1)} dB`);
  check(a >= g - 0.05, `${kind}: the lookahead encoder is worse than greedy`);
}
// WAV round trip through prepare (stereo float -> mono 16 kHz)
{
  const L = new Float32Array(44100), R = new Float32Array(44100);
  for (let i = 0; i < 44100; i++) { L[i] = 0.5 * Math.sin(i * 0.05); R[i] = 0.25 * Math.sin(i * 0.05); }
  const pcm = prepare([L, R], 44100, { target: 16000 });
  check(Math.abs(pcm.length - 16000) < 400, `prepare: length ${pcm.length}`);
  let pk = 0;
  for (const v of pcm) pk = Math.max(pk, Math.abs(v));
  check(pk > 30000 && pk <= 32767, `prepare: normalised peak ${pk}`);
  const w = parseWav(wav16(pcm, 16000));
  check(w.rate === 16000 && w.channels.length === 1 && w.channels[0].length === pcm.length, "wav16 / parseWav round trip");
}
check(crc32(new TextEncoder().encode("123456789")) === 0xCBF43926, "crc32 check value");
console.log(fails ? `codec_test: ${fails} FAILED` : "codec_test: all passed");
process.exit(fails ? 1 : 0);
