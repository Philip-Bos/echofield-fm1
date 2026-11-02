// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// How a parameter reads, as on the FM-1's screen (app/params.c pformat).
import { PARAMS, P } from "./efparams.js";

const dec1 = (v) => (Math.round(v * 10) / 10).toFixed(1);

export function formatParam(id, v) {
  const p = PARAMS[id];
  if (p.names) return p.names[Math.max(0, Math.min(p.names.length - 1, Math.round(v)))];
  switch (id) {
    case P.PITCH: return `${v > 0 ? "+" : ""}${Math.round(v)} ST`;
    case P.PITCH_RAND: return `${dec1(v)} ST`;
    case P.SIZE_MUL: case P.DENS_MUL: { const m = 2 ** v; return m < 0.95 ? `X${m.toFixed(2)}` : `X${dec1(m)}`; }
    case P.SCAN: return `${v > 0 ? "+" : ""}${dec1(v)}X`;
    case P.GRAIN_TONE: return v < -0.02 ? `DARK ${Math.round(-v * 100)}` : v > 0.02 ? `AIRY ${Math.round(v * 100)}` : "PURE";
    case P.CUTOFF: { const hz = 30 * 600 ** v; return hz >= 1000 ? `${dec1(hz / 1000)}K` : `${Math.floor(hz)}HZ`; }
    case P.DLY_TIME: return `${Math.round(v)}MS`;
    case P.OR_RANGE: return `${Math.round(v)} OCT`;
    case P.ATTACK: case P.RELEASE: {
      const s = (id === P.ATTACK ? 0.005 : 0.01) * 800 ** v;
      return s < 1 ? `${Math.floor(s * 1000)}MS` : `${dec1(s)}S`;
    }
    case P.START: case P.END: case P.POSITION: return `${dec1(v * 100)}%`;
    default: return `${Math.round(v * 100)}%`;
  }
}

export function kb(bytes) { return `${Math.round(bytes / 1024)} KB`; }
export function secs(frames, rate) { return `${(frames / rate).toFixed(1)} s`; }
