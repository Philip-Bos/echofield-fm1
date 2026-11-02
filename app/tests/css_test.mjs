// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// The companion's stylesheets are well-formed: every comment closed and every brace matched. (An unclosed comment
// once turned off every rule after it, silently: the pages worked, half of them unstyled.)
//   node app/tests/css_test.mjs
import { readFileSync, readdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const COMPANION = join(dirname(fileURLToPath(import.meta.url)), "../../companion");
let fails = 0;
const files = [];
(function walk(d) {
  for (const e of readdirSync(d, { withFileTypes: true }))
    if (e.isDirectory()) walk(join(d, e.name));
    else if (e.name.endsWith(".css") || e.name.endsWith(".html")) files.push(join(d, e.name));
})(COMPANION);
for (const f of files) {
  let css = readFileSync(f, "utf8");
  if (f.endsWith(".html")) css = [...css.matchAll(/<style[^>]*>([\s\S]*?)<\/style>/g)].map((m) => m[1]).join("\n");
  const open = (css.match(/\/\*/g) || []).length, close = (css.match(/\*\//g) || []).length;
  const bare = css.replace(/\/\*[\s\S]*?\*\//g, "");
  let depth = 0, neg = false;
  for (const c of bare) { if (c === "{") depth++; else if (c === "}" && --depth < 0) neg = true; }
  if (open !== close || depth !== 0 || neg || bare.includes("/*")) {
    console.log(`FAIL ${f.slice(COMPANION.length + 1)}: comments ${open} open / ${close} closed, braces ${depth}`);
    fails++;
  }
}
console.log(fails ? `css_test: ${fails} FAILED` : `css_test: ${files.length} files, all well-formed`);
process.exit(fails ? 1 : 0);
