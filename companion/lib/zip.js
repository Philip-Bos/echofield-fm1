// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// ZIP files without compression (the samples are ADPCM already): enough for a backup that any unzip tool opens, and
// for reading those backups back. Every entry's CRC-32 is checked on reading.
import { crc32 } from "./efcodec.js";

const enc = new TextEncoder();

export function zip(files) {             // [{name, data: Uint8Array}] -> Uint8Array
  const parts = [], central = [];
  let off = 0;
  for (const f of files) {
    const name = enc.encode(f.name), crc = crc32(f.data), n = f.data.length;
    const h = new DataView(new ArrayBuffer(30));
    h.setUint32(0, 0x04034B50, true); h.setUint16(4, 20, true); h.setUint16(8, 0, true);
    h.setUint32(14, crc, true); h.setUint32(18, n, true); h.setUint32(22, n, true); h.setUint16(26, name.length, true);
    parts.push(new Uint8Array(h.buffer), name, f.data);
    const c = new DataView(new ArrayBuffer(46));
    c.setUint32(0, 0x02014B50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true);
    c.setUint32(16, crc, true); c.setUint32(20, n, true); c.setUint32(24, n, true); c.setUint16(28, name.length, true);
    c.setUint32(42, off, true);
    central.push(new Uint8Array(c.buffer), name);
    off += 30 + name.length + n;
  }
  const csize = central.reduce((s, p) => s + p.length, 0);
  const e = new DataView(new ArrayBuffer(22));
  e.setUint32(0, 0x06054B50, true); e.setUint16(8, files.length, true); e.setUint16(10, files.length, true);
  e.setUint32(12, csize, true); e.setUint32(16, off, true);
  const all = [...parts, ...central, new Uint8Array(e.buffer)], out = new Uint8Array(all.reduce((s, p) => s + p.length, 0));
  let o = 0;
  for (const p of all) { out.set(p, o); o += p.length; }
  return out;
}

export function unzip(buf) {             // -> Map(name -> Uint8Array); stored entries only
  const u8 = new Uint8Array(buf), dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), out = new Map();
  let e = -1;
  for (let i = u8.length - 22; i >= 0 && i >= u8.length - 65557; i--) if (dv.getUint32(i, true) === 0x06054B50) { e = i; break; }
  if (e < 0) throw new Error("not a ZIP file");
  let o = dv.getUint32(e + 16, true);
  const n = dv.getUint16(e + 10, true);
  for (let k = 0; k < n; k++) {
    if (dv.getUint32(o, true) !== 0x02014B50) throw new Error("damaged ZIP file");
    const method = dv.getUint16(o + 10, true), crc = dv.getUint32(o + 16, true), size = dv.getUint32(o + 20, true);
    const nl = dv.getUint16(o + 28, true), xl = dv.getUint16(o + 30, true), cl = dv.getUint16(o + 32, true);
    const lo = dv.getUint32(o + 42, true), name = new TextDecoder().decode(u8.subarray(o + 46, o + 46 + nl));
    if (method !== 0) throw new Error(`${name}: compressed entries are not supported`);
    const start = lo + 30 + dv.getUint16(lo + 26, true) + dv.getUint16(lo + 28, true), data = u8.slice(start, start + size);
    if (crc32(data) !== crc) throw new Error(`${name}: damaged (checksum)`);
    out.set(name, data);
    o += 46 + nl + xl + cl;
  }
  return out;
}
