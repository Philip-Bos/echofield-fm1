// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// The companion app's protocol client (companion/lib/efproto.js) against the FM-1's own code (app/proto.c, bank.c, on
// the host: tests/proto_host.c): uploads, read-back, the directory, errors, presets, parameters, wipe.
//   node tests/proto_test.mjs build/proto_host
import { spawn } from "node:child_process";
import { EfLink, EchoField } from "../../companion/lib/efproto.js";
import { encode, decode, crc32, sectorsFor, SEC_PAYLOAD } from "../../companion/lib/efcodec.js";
import { PARAMS, P, quant } from "../../companion/lib/efparams.js";

let fails = 0;
const check = (c, m) => { if (!c) { fails++; console.log("  FAIL " + m); } else console.log("  ok " + m); };
const hex = (b) => Array.from(b, (v) => v.toString(16).padStart(2, "0")).join("");

const child = spawn(process.argv[2], [], { stdio: ["pipe", "pipe", "inherit"] });
let fn = null, buf = "";
child.stdout.on("data", (d) => {
  buf += d;
  let i;
  while ((i = buf.indexOf("\n")) >= 0) {
    const line = buf.slice(0, i); buf = buf.slice(i + 1);
    if (line.startsWith("sx ") && fn) fn(Uint8Array.from(line.slice(3).match(/../g).map((h) => parseInt(h, 16))));
  }
});
const transport = { send: (b) => child.stdin.write("sx " + hex(b) + "\n"), onMessage: (f) => { fn = f; }, close: () => child.stdin.end() };
const ef = new EchoField(new EfLink(transport));

function tone(n, f) {
  const x = new Int16Array(n);
  for (let i = 0; i < n; i++) x[i] = Math.round(12000 * Math.sin(2 * Math.PI * f * i / 22050) * Math.exp(-(i % 11025) / 8000));
  return x;
}
async function expectError(p, re, what) {
  try { await p; check(false, what + " (no error)"); } catch (e) { check(re.test(e.message), `${what}: "${e.message}"`); }
}

try {
  const info = await ef.hello();
  check(info && info.slots === 16 && info.sectors === 92 && info.free === 92, "HELLO: 16 slots, 92 free sectors");
  check(info.nparams === PARAMS.length, `HELLO: ${info.nparams} parameters, as efparams.js`);
  let dir = await ef.dir();
  check(dir.every((d) => d.state === "empty"), "DIR: an empty bank");

  // an upload, read back byte for byte
  const pcm = tone(33000, 220), pay = encode(pcm), nsec = sectorsFor(pcm.length);
  let prog = 0;
  await ef.upload(0, pay, { frames: pcm.length, rate: 22050, name: "Test tone!", crc: crc32(pay) }, (b) => { prog = b; });
  check(prog === pay.length, `UPLOAD: every byte asked for (${prog} of ${pay.length})`);
  dir = await ef.dir();
  check(dir[0].state === "ok" && dir[0].frames === pcm.length && dir[0].rate === 22050, "DIR: the sample is there");
  check(dir[0].name === "TEST TONE?", `DIR: its name made safe ("${dir[0].name}")`);
  check((await ef.hello()).free === 92 - nsec, `HELLO: ${nsec} sectors taken`);
  const back = await ef.read(0, nsec * SEC_PAYLOAD);
  check(back.every((v, i) => v === pay[i]), "READ: the payload comes back unchanged");
  const dec = decode(back, pcm.length);
  check(dec.length === pcm.length, "the read-back decodes");

  // a damaged transfer (wrong CRC) leaves nothing behind
  await expectError(ef.upload(1, pay, { frames: pcm.length, rate: 22050, name: "BAD", crc: crc32(pay) ^ 1 }), /checksum/, "UPLOAD with a wrong CRC is refused");
  dir = await ef.dir();
  check(dir[1].state === "empty", "...and slot 2 stays empty");
  check((await ef.hello()).free === 92 - nsec, "...and no sector is lost");

  // replacing a sample frees the old one's sectors
  const pcm2 = tone(9000, 330), pay2 = encode(pcm2);
  await ef.upload(0, pay2, { frames: pcm2.length, rate: 16000, name: "SHORT", crc: crc32(pay2) });
  dir = await ef.dir();
  check(dir[0].name === "SHORT" && dir[0].rate === 16000, "UPLOAD over a sample replaces it");
  check((await ef.hello()).free === 92 - sectorsFor(pcm2.length), "...and its old sectors are free again");

  // the bank fills, and says so
  const big = new Int16Array(7424 * 80), payBig = encode(big);
  await ef.upload(2, payBig, { frames: big.length, rate: 22050, name: "BIG", crc: crc32(payBig) });
  await expectError(ef.upload(3, payBig, { frames: big.length, rate: 22050, name: "TOO BIG", crc: crc32(payBig) }), /room/, "UPLOAD into a full bank is refused");
  dir = await ef.dir();
  check(dir[3].state === "empty", "...and leaves nothing");

  await ef.rename(0, "renamed");
  check((await ef.dir())[0].name === "RENAMED", "RENAME");
  await ef.erase(2);
  check((await ef.dir())[2].state === "empty", "ERASE");

  // parameters both ways
  const tex = PARAMS[P.TEXTURE];
  await ef.set([[P.TEXTURE, quant(tex, 0.5)], [0xF0, 1]]);
  const pr = await ef.params();
  check(pr.q[P.TEXTURE] === quant(tex, 0.5) && pr.play, "SET / PARAMS: TEXTURE 0.5, PLAY on");

  // a user preset: written, read back, deleted
  const q = PARAMS.map((p) => quant(p, p.def));
  q[P.PITCH] = quant(PARAMS[P.PITCH], 7);
  await ef.presetPut({ index: 3, used: true, slot: 0, name: "my sound", sample: "RENAMED", q });
  const got = await ef.presetGet(3);
  check(got.used && got.name === "MY SOUND" && got.sample === "RENAMED" && got.q[P.PITCH] === q[P.PITCH], "PRESET PUT / GET");
  const list = await ef.presets();
  check(list.user[3].used && list.user[3].name === "MY SOUND" && list.factory.length === 15, "PRESETS: users and the 15 factory names");
  await ef.presetOp(2, 3);
  check(!(await ef.presetGet(3)).used, "PRESET OP delete");

  await ef.wipe();
  check((await ef.dir()).every((d) => d.state === "empty") && (await ef.hello()).free === 92, "WIPE: everything gone, every sector free");
} catch (e) {
  fails++;
  console.log("  FAIL " + e.stack);
}
transport.close();
console.log(fails ? `proto_test: ${fails} FAILED` : "proto_test: all passed");
process.exit(fails ? 1 : 0);
