// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// The flasher's sounds step (companion/flasher/flasher.js installSounds) against the FM-1's own bank and protocol code
// (tests/proto_host.c): an empty bank gets the starter set, every sound checked; a bank with samples is left alone.
//   node tests/sounds_test.mjs build/proto_host
import { spawn } from "node:child_process";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
const C = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "companion");
globalThis.fetch = async (u) => { const b = readFileSync(join(C, String(u).replace(/^\.\.\//, ""))); return { json: async () => JSON.parse(b), arrayBuffer: async () => b.buffer.slice(b.byteOffset, b.byteOffset + b.length) }; };
const { EfLink, EchoField } = await import(join(C, "lib/efproto.js"));
const { installSounds } = await import(join(C, "flasher/flasher.js"));
const child = spawn(process.argv[2], [], { stdio: ["pipe", "pipe", "inherit"] });
let fn = null, buf = "", fails = 0;
child.stdout.on("data", (d) => { buf += d; let i; while ((i = buf.indexOf("\n")) >= 0) { const l = buf.slice(0, i); buf = buf.slice(i + 1); if (l.startsWith("sx ") && fn) fn(Uint8Array.from(l.slice(3).match(/../g).map((h) => parseInt(h, 16)))); } });
const hex = (b) => Array.from(b, (v) => v.toString(16).padStart(2, "0")).join("");
const link = new EfLink({ send: (b) => child.stdin.write("sx " + hex(b) + "\n"), onMessage: (f) => { fn = f; }, close: () => child.stdin.end() });
const ef = new EchoField(link);
const check = (c, m) => { if (!c) fails++; console.log(`  ${c ? "ok" : "FAIL"} ${m}`); };
const starter = JSON.parse(readFileSync(join(C, "factory/factory.json"))).sounds.filter((s) => s.starter);
let r = await installSounds(ef, "starter");
let dir = await ef.dir();
check(r.done === starter.length, `empty bank: ${r.done} starter sounds installed`);
check(starter.every((s, i) => dir[i].state === "ok" && dir[i].name === s.name && dir[i].frames === s.frames), "each in its slot, checked by the FM-1");
r = await installSounds(ef, "all");
check(r.done === 0 && /kept/.test(r.why), "a bank with samples is left alone");
await ef.wipe();
check((await installSounds(ef, "none")).done === 0 && (await ef.dir()).every((d) => d.state === "empty"), "none: nothing installed");
r = await installSounds(ef, "all");
check(r.done === 14 && (await ef.hello()).free === 92 - 67, "all 14: 67 sectors");
link.close(); child.kill();
console.log(fails ? `sounds_test: ${fails} FAILED` : "sounds_test: all passed");
process.exit(fails ? 1 : 0);
