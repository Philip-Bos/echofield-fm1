// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Charles Vestal
// The screen mirror's decoder, verbatim from fm1-base web/mirror/index.html (the part between "decoder: begin" and
// "decoder: end"), made a module for ECHOFIELD's companion app. Upstream: github.com/charlesvestal/fm1-base
// the mirror's frames (fm1-base device/mirror.c): F0 pack7(00 59 cmd len24 body ~sum8(body)) F7.
const MR_REQ = 0x50, MR_RECT = 0x51, MR_FILL = 0x52, MR_INPUT = 0x53, MR_REMOTE = 0x54;
function mrPack7(bytes) {
  const out = [];
  let acc = 0, nb = 0;
  for (const b of bytes) {
    acc |= b << nb; nb += 8;
    while (nb >= 7) { out.push(acc & 0x7F); acc >>= 7; nb -= 7; }
  }
  if (nb) out.push(acc & 0x7F);
  return out;
}
function mrUnpack7(src) {
  const out = [];
  let acc = 0, nb = 0;
  for (const b of src) {
    acc |= (b & 0x7F) << nb; nb += 7;
    if (nb >= 8) { out.push(acc & 0xFF); acc >>= 8; nb -= 8; }
  }
  return out;
}
// a request to the FM-1: 0 off, 1 on (also the keep-alive), 2 refresh
function mrRequest(op) {
  return Uint8Array.from([0xF0, ...mrPack7([0x00, 0x59, MR_REQ, 1, 0, 0, op, ~op & 0xFF]), 0xF7]);
}
// the remote (mirror version 2): seq, op (1 button, 2 key, 3 turn, 4 MASTER, 0 let go), a, b; answered with seq, op, done
function mrRemote(seq, op, a = 0, b = 0) {
  const body = [seq & 255, op & 255, a & 255, b & 255, 0], sum = body.reduce((x, v) => x + v, 0);
  return Uint8Array.from([0xF0, ...mrPack7([0x00, 0x59, MR_REMOTE, 5, 0, 0, ...body, ~sum & 0xFF]), 0xF7]);
}
const mrHash = (c) => (Math.imul(c, 40503) >>> 10) & 63;
// on: { rect(x, y, w, h), input(state), hello(info), gap() }; screen: 240 x 240 RGB565
function createMirror(on = {}) {
  const screen = new Uint16Array(240 * 240);
  const stats = { bytes: 0, frames: 0, rects: 0, fills: 0, inputs: 0, bad: 0, gaps: 0 };
  let buf = [], inSx = false, seq = -1;
  function rect(b) {
    const x = b[0], y = b[1], w = b[2], h = b[3], total = w * h, cache = new Uint16Array(64);
    let i = 5, p = 0, cur = 0;
    if (!w || !h || x + w > 240 || y + h > 240) return false;
    const put = (c, n) => {
      if (p + n > total) return false;
      for (let k = 0; k < n; k++, p++) screen[(y + (p / w | 0)) * 240 + x + p % w] = c;
      return true;
    };
    while (i < b.length) {
      const op = b[i++];
      if (op < 0x80) { if (!put(cur, op + 1)) return false; }
      else if (op < 0xC0) { cur = cache[op & 63]; if (!put(cur, 1)) return false; }
      else {
        if (i + 2 > b.length) return false;
        cur = b[i] << 8 | b[i + 1]; i += 2;
        cache[mrHash(cur)] = cur;
        if (!put(cur, (op & 63) + 1)) return false;
      }
    }
    return p === total;
  }
  function frame(raw) {
    const m = mrUnpack7(raw);
    if (m.length < 7 || m[0] !== 0 || m[1] !== 0x59) return;          // not the mirror's
    const n = m[3] | m[4] << 8 | m[5] << 16;
    let sum = 0;
    for (let i = 6; i < 6 + n; i++) sum += m[i];
    if (n + 7 !== m.length || (~sum & 0xFF) !== m[6 + n]) { stats.bad++; return; }
    const cmd = m[2], b = m.slice(6, 6 + n);
    stats.frames++;
    if (cmd === MR_RECT || cmd === MR_FILL) {
      if (seq >= 0 && b[4] !== seq) { stats.gaps++; on.gap?.(); }
      seq = (b[4] + 1) & 255;
    }
    if (cmd === MR_RECT) {
      if (!rect(b)) { stats.bad++; on.gap?.(); }
      stats.rects++;
      on.rect?.(b[0], b[1], b[2], b[3]);
    } else if (cmd === MR_FILL && n >= 7) {
      const c = b[5] << 8 | b[6], x = b[0], y = b[1], w = b[2], h = b[3];
      for (let j = y; j < Math.min(240, y + h); j++) screen.fill(c, j * 240 + x, j * 240 + Math.min(240, x + w));
      stats.fills++;
      on.rect?.(x, y, w, h);
    } else if (cmd === MR_INPUT && n >= 21) {
      const u32 = (o) => (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0;
      const lit = b[15] | b[16] << 8;
      stats.inputs++;
      on.input?.({ buttons: b[0] | b[1] << 8, keys: u32(2), enc: Array.from(b.slice(6, 13), (v) => (v << 24) >> 24),
                   master: b[13] | b[14] << 8, litButtons: lit & 0x7FFF, playRed: !!(lit & 0x8000), litKeys: u32(17) });
    } else if (cmd === MR_REQ && n >= 4) {
      on.hello?.({ version: b[0], on: !!b[1], w: b[2] + 1, h: b[3] + 1 });
    } else if (cmd === MR_REMOTE && n >= 3) {
      on.remote?.({ seq: b[0], op: b[1], ok: !!b[2] });
    }
  }
  function feed(data) {
    for (const v of data) {
      stats.bytes++;
      if (v === 0xF0) { inSx = true; buf = []; }
      else if (!inSx || v >= 0xF8) continue;                          // (real-time bytes may come inside a SysEx)
      else if (v === 0xF7) { inSx = false; frame(buf); }
      else if (v & 0x80) inSx = false;                                // a status byte: that SysEx is cut
      else buf.push(v);
    }
  }
  return { feed, screen, stats, resync() { seq = -1; } };
}
export { MR_REQ, MR_RECT, MR_FILL, MR_INPUT, MR_REMOTE, mrRequest, mrRemote, createMirror };
