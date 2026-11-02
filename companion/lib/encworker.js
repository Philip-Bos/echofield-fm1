// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// efcodec.encode off the page's thread: {id, pcm: Int16Array} -> {id, payload: Uint8Array, snr}
import { encode, snr } from "./efcodec.js";
self.onmessage = (e) => {
  const { id, pcm } = e.data, payload = encode(pcm);
  self.postMessage({ id, payload, snr: snr(pcm, payload) }, [payload.buffer]);
};
