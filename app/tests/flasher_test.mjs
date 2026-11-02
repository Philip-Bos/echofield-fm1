// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// The flasher refuses phones and tablets (every kind we know of) and runs on computers.
import { isMobile } from "../../companion/flasher/flasher.js";
let fails = 0;
const t = (ua, extra, want, name) => {
  const nav = { userAgent: ua, maxTouchPoints: 0, ...extra };
  const mm = (q) => (extra.coarseOnly ? q.includes("coarse") : q.includes("any-pointer: fine"));
  const got = isMobile(nav, mm);
  if (got !== want) { fails++; console.log(`  FAIL ${name}: ${got}`); } else console.log(`  ok ${name}: ${want ? "refused" : "allowed"}`);
};
t("Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 Chrome/130 Mobile Safari/537.36", {}, true, "Android phone, Chrome");
t("Mozilla/5.0 (Linux; Android 14; SM-X710) AppleWebKit/537.36 Chrome/130 Safari/537.36", { userAgentData: { mobile: false } , coarseOnly: true }, true, "Android tablet (desktop-style UA, touch only)");
t("Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 Mobile/15E148", {}, true, "iPhone");
t("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 Version/18 Safari/605.1.15", { maxTouchPoints: 5 }, true, "iPad asking for the desktop site");
t("Mozilla/5.0 (Linux; Android 14) Chrome/130", { userAgentData: { mobile: true } }, true, "UA-CH mobile flag");
t("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 Chrome/130 Safari/537.36", { userAgentData: { mobile: false } }, false, "Mac, Chrome");
t("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/130 Safari/537.36 Edg/130", { maxTouchPoints: 10 }, false, "Windows touch laptop, Edge");
t("Mozilla/5.0 (X11; CrOS x86_64 15000.0.0) AppleWebKit/537.36 Chrome/130 Safari/537.36", {}, false, "Chromebook");
console.log(fails ? `flasher_test: ${fails} FAILED` : "flasher_test: all passed");
process.exit(fails ? 1 : 0);
