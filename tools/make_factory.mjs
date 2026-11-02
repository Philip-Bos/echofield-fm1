// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// The factory sound pack (companion/factory/): ECHOFIELD's own factory samples (synthesised by the R36S project's
// tools/gen_samples.py; original material, CC0) cut to a whole number of flash sectors each, at the rate that suits
// the sound, encoded with the companion app's encoder. The companion app installs them into free slots; every one
// can be overwritten or deleted. Deterministic: the same sources make the same files.
//   node tools/make_factory.mjs SAMPLES_DIR   (the source WAVs: the R36S ECHOFIELD's assets/samples; the encoded pack is
//                                               already in companion/factory, so this is only needed to remake it)
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { parseWav, prepare, encode, crc32, snr, SEC_SAMPLES } from "../companion/lib/efcodec.js";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const SRC = process.argv[2];
if (!SRC) { console.error("usage: node tools/make_factory.mjs SAMPLES_DIR"); process.exit(2); }
const OUT = join(ROOT, "companion", "factory");

// name on the FM-1 (11 characters), source, rate, sectors, in the starter set
const PACK = [
  ["FELT PIANO", "felt_piano.wav", 16000, 5, true],
  ["BOWED GLASS", "bowed_glass.wav", 22050, 6, true],
  ["MUSIC BOX", "music_box.wav", 22050, 6, true],
  ["WARM PAD", "warm_pad.wav", 16000, 5, true],
  ["STRINGS", "string_swell.wav", 16000, 5, true],
  ["RAIN FIELD", "rain_field.wav", 16000, 5, true],
  ["VOX CHOIR", "vox_choir.wav", 16000, 5, true],
  ["LOW DRONE", "low_drone.wav", 11025, 3, true],
  ["SUB BLOOM", "sub_bloom.wav", 11025, 3, false],
  ["KALIMBA", "kalimba_drops.wav", 22050, 5, false],
  ["BREATH", "breath_choir.wav", 16000, 4, false],
  ["GLASS TINES", "glass_tines.wav", 22050, 5, false],
  ["WIRE PLUCKS", "wire_plucks.wav", 22050, 5, false],
  ["DUST RHYTHM", "dust_rhythm.wav", 16000, 5, false],
];

// the window of the source (seconds) with the most energy, after its first 0.15 s
function bestWindow(mono, rate, seconds) {
  const n = Math.round(seconds * rate), hop = Math.round(rate / 20), skip = Math.round(0.15 * rate);
  if (mono.length <= n + skip) return 0;
  let best = skip, bestE = -1;
  for (let a = skip; a + n <= mono.length; a += hop) {
    let e = 0;
    for (let i = a; i < a + n; i += 4) e += mono[i] * mono[i];
    if (e > bestE) { bestE = e; best = a; }
  }
  return best / rate;
}

mkdirSync(OUT, { recursive: true });
const manifest = [];
let starter = 0, all = 0;
for (const [name, file, rate, sectors, inStarter] of PACK) {
  const { channels, rate: srcRate } = parseWav(readFileSync(join(SRC, file)));
  const frames = sectors * SEC_SAMPLES, seconds = frames / rate;
  const mono = channels[0].map((v, i) => channels.reduce((s, c) => s + c[i], 0) / channels.length);
  const start = bestWindow(mono, srcRate, seconds + 0.01);
  let pcm = prepare(channels, srcRate, { target: rate, trim: false, start, end: start + seconds + 0.05, fadeMs: 8 });
  pcm = pcm.length >= frames ? pcm.subarray(0, frames) : Int16Array.from({ length: frames }, (_, i) => pcm[i] || 0);
  for (let i = 0, f = Math.round(0.008 * rate); i < f; i++) pcm[frames - 1 - i] = Math.round(pcm[frames - 1 - i] * i / f);
  const pay = encode(pcm), id = name.toLowerCase().replace(/[^a-z0-9]+/g, "-");
  writeFileSync(join(OUT, id + ".efs"), pay);
  manifest.push({ id, name, file: id + ".efs", rate, frames, sectors, crc: crc32(pay), starter: inStarter,
                  seconds: Math.round(seconds * 100) / 100, snr: Math.round(snr(pcm, pay) * 10) / 10 });
  all += sectors;
  if (inStarter) starter += sectors;
  console.log(`${name.padEnd(12)} ${String(rate).padStart(5)} Hz  ${seconds.toFixed(2)} s  ${sectors} sectors  SNR ${snr(pcm, pay).toFixed(1)} dB`);
}
writeFileSync(join(OUT, "factory.json"), JSON.stringify({ format: "EFB1", licence: "CC0-1.0", sounds: manifest }, null, 1) + "\n");
console.log(`starter set ${starter} of 92 sectors (${Math.round(starter / 92 * 100)} %), everything ${all}`);
