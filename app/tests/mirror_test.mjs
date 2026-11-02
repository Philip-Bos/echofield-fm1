// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// The companion app's mirror decoder (vendor/fm1-base/fm1mirror.js) on ECHOFIELD's real mirror stream (the host
// simulator, at USB's pace) must rebuild the screen pixel for pixel.  node tests/mirror_test.mjs OUTDIR
import { readFileSync } from "node:fs";
import { createMirror } from "../../companion/vendor/fm1-base/fm1mirror.js";
const dir = process.argv[2];
const m = createMirror({});
m.feed(readFileSync(`${dir}/mirror.syx`));
const fb = readFileSync(`${dir}/fb.raw`);
let diff = 0;
for (let i = 0; i < 240 * 240; i++) if (m.screen[i] !== ((fb[2 * i] << 8) | fb[2 * i + 1])) diff++;
console.log(`mirror: ${m.stats.frames} frames, ${m.stats.rects} rects, ${(m.stats.bytes / 1024).toFixed(0)} kB, ` +
            `${m.stats.bad} bad, ${m.stats.gaps} gaps; ${diff} pixels differ`);
if (diff || m.stats.bad) { console.log("mirror_test: FAILED"); process.exit(1); }
console.log("mirror_test: the PC shows exactly the FM-1's screen");
