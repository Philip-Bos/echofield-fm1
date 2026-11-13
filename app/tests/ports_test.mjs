// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// companion/lib/efports.js: the FM-1 running ECHOFIELD is found whatever its port is called (issue #5: macOS named it
// after another firmware, and the companion and the flasher's sounds step never asked it), by name first, never a
// Bluetooth port or an update loader's.
//   node app/tests/ports_test.mjs
import { findEchofield } from "../../companion/lib/efports.js";
import { srFrame, SR_HELLO } from "../../companion/vendor/fm1-base/fm1ports.js";

let fails = 0;
const check = (ok, what) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) fails++; };

// a Web MIDI port pair; fm1: answers the screen reader's HELLO as an fm1-base firmware does
function port(name, fm1, log) {
  const ears = new Set();
  const input = {
    name, state: "connected", async open() {},
    addEventListener(t, f) { if (t === "midimessage") ears.add(f); },
    removeEventListener(t, f) { ears.delete(f); },
  };
  const output = {
    name, state: "connected", async open() {},
    send(d) {
      log.push(name);
      const hello = srFrame(SR_HELLO, 1);
      if (fm1 && d.length === hello.length && d.every((b, i) => b === hello[i])) {
        const txt = [..."ECHOFIELD\0https://philip-bos.github.io/echofield-fm1/words/107c1fe0ed.json"].map((c) => c.charCodeAt(0));
        const ans = new Uint8Array([...srFrame(SR_HELLO, 1).slice(0, -1), ...txt, 0xF7]);
        setTimeout(() => { for (const f of ears) f({ data: ans }); }, 5);
      }
    },
  };
  return [input, output];
}
function access(list, log) {
  const ins = new Map(), outs = new Map();
  list.forEach(([name, fm1], i) => { const [a, b] = port(name, fm1, log); ins.set("i" + i, a); outs.set("o" + i, b); });
  return { inputs: ins, outputs: outs };
}
const tries = [];
const ours = (want) => async (p) => { tries.push(p.name); return want.includes(p.name) ? p.name : null; };

// 1. the port has the usual name: found at once, nothing else asked
let log = [];
tries.length = 0;
let got = await findEchofield(access([["IAC Driver Bus 1", false], ["Felucca", true]], log), ours(["Felucca"]));
check(got === "Felucca", "a port called Felucca is found");
check(log.length === 0 && tries.join() === "Felucca", "by its name alone: no HELLO to other gear");

// 2. issue #5: the port carries another firmware's name
log = [];
tries.length = 0;
got = await findEchofield(access([["IAC Driver Bus 1", false], ["OberVave", true], ["Bluetooth MIDI", true],
                                  ["Arturia KeyStep", false]], log), ours(["OberVave"]));
check(got === "OberVave", "a port called OberVave that answers the HELLO is found");
check(!log.includes("Bluetooth MIDI"), "a Bluetooth port is never asked");

// 3. an update loader's port is left to the installer
log = [];
tries.length = 0;
got = await findEchofield(access([["FM-1 update", true]], log), ours(["FM-1 update"]));
check(got === null && !tries.length, "an update loader's port is not tried");

// 4. nothing there
got = await findEchofield(access([["IAC Driver Bus 1", false]], []), ours([]));
check(got === null, "no FM-1: null");

console.log(fails ? `ports_test: ${fails} failed` : "ports_test: all passed");
process.exit(fails ? 1 : 0);
