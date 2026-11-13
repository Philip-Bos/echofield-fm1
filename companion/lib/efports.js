// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// Finding the FM-1 that runs ECHOFIELD, for the companion's Connect and the flasher's sounds step.
//
// First the ports with an FM-1's name (fm1ports.js PORT_NAMES: "Felucca", "FM-1" ...) or ECHOFIELD's. Then, when none
// of them answers, every other port that answers the screen reader's HELLO (fm1ports.js askAll; every fm1-base firmware
// does, whatever its port is called), as fm1-base's installer finds an FM-1 too. macOS names a USB MIDI port from its
// own cache by the device's USB id, and the FM-1 firmwares on felucca-bsp share one: on a computer that has run
// another firmware, ECHOFIELD's port can carry that firmware's name. Never a Bluetooth port, never an update loader's.
import { pairs, usualName, askAll } from "../vendor/fm1-base/fm1ports.js";

const named = (n) => usualName(n) || /echofield/i.test(n || "");
const skipped = (n) => /bluetooth/i.test(n || "") || /update|^ota|uboot/i.test(n || "");

// tryPort({ name, input, output }) -> a result, or a falsy value to go on -> the first result, or null
export async function findEchofield(access, tryPort) {
  for (const p of pairs(access)) {
    if (!named(p.name) || skipped(p.name)) continue;
    const r = await tryPort(p);
    if (r) return r;
  }
  for (const g of await askAll(access, { skip: (n) => named(n) || skipped(n) })) {
    const r = await tryPort({ name: g.name, input: g.input, output: g.output });
    if (r) return r;
  }
  return null;
}
