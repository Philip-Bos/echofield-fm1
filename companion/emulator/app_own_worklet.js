// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// ECHOFIELD's part of the emulator's audio worklet (fm1-base loads it before worklet.js): the sample bank's commands
// for the editor under the panel (app.js), run here between two audio quanta through the firmware's own functions
// (app/web.c). The page asks { type: "app", id, op, ... }; the answer is { type: "app", id, ... }. When the bank or
// the sample playing changes, { type: "app", op: "gen", bank } tells the page, which keeps the samples (the
// emulator's flash is not kept).
globalThis.fm1App = {
  gen: -1,
  slot: -2,
  n: 0,
  process(fm1) {
    if (++this.n & 31) return;                         // (about ten times a second)
    const g = fm1.ex.ef_web_gen(), slot = fm1.ex.ef_web_slot();
    if (g !== this.gen || slot !== this.slot) {        // the bank changed, or the sample playing
      const bank = g !== this.gen;
      this.gen = g;
      this.slot = slot;
      fm1.port.postMessage({ type: "app", op: "gen", gen: g, slot, bank });
    }
  },
  message(fm1, m) {
    const ex = fm1.ex, mem = () => fm1.mem.buffer;
    const name = (s) => {
      const b = new Uint8Array(mem(), ex.ef_web_name(), 12);
      b.fill(0);
      for (let i = 0; i < Math.min(11, s.length); i++) b[i] = s.charCodeAt(i) & 0x7F;
    };
    const put = (bytes) => new Uint8Array(mem(), ex.ef_web_buf(), bytes.length).set(bytes);
    let r = {};
    switch (m.op) {
      case "dir":
        ex.ef_web_dir();
        r = { dir: new Uint8Array(mem(), ex.ef_web_buf(), 16 * 20).slice(), slot: ex.ef_web_slot(), gen: ex.ef_web_gen() };
        break;
      case "read": {
        const n = ex.ef_web_read(m.slot);
        r = n < 0 ? { rc: 1 } : { rc: 0, payload: new Uint8Array(mem(), ex.ef_web_buf(), n).slice() };
        break;
      }
      case "write":
        put(new Uint8Array(m.payload));
        name(m.name);
        r = { rc: ex.ef_web_write(m.slot, m.frames, m.rate, m.factory ? 1 : 0, m.crc >>> 0) };
        break;
      case "restore":                                   // the page's samples, all of them, after boot
        ex.ef_web_clear();
        for (const s of m.samples) {
          put(new Uint8Array(s.payload));
          name(s.name);
          ex.ef_web_write(s.slot, s.frames, s.rate, s.factory ? 1 : 0, 0);
        }
        r = { rc: 0 };
        break;
      case "factory": r = { n: ex.ef_web_factory() }; break;
      case "erase": r = { rc: ex.ef_web_erase(m.slot) }; break;
      case "rename": name(m.name); r = { rc: ex.ef_web_rename(m.slot) }; break;
      case "select": ex.ef_web_select(m.slot); break;
      case "listen": ex.ef_web_listen(m.how); break;
    }
    fm1.port.postMessage({ type: "app", id: m.id, ...r });
  },
};
