#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Philip Bos
# A release: every test, the firmware built and checked, the emulator built, the package put in the companion app with
# its SHA-256, and dist/ with the companion (one folder to serve or put on a web site: the companion, flasher/ and
# emulator/ in it) and the package alone.
#   tools/release.sh 0.1        (needs the JieLi toolchain and the AC79 SDK: base/README.md)
set -e
V=${1:?usage: tools/release.sh VERSION}
cd "$(dirname "$0")/.."
[ -x .venv/bin/python ] && PATH=$PWD/.venv/bin:$PATH
export JIELI_TOOLCHAIN=${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain} AC79_SDK=${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}
tools/run_tests.sh
python3 tools/gen_efparams.py --check
python3 base/tools/build.py app --release "$V" | grep -E "^  ok|^  FAIL|ERROR|^package"
python3 base/tools/build.py app --web | grep -E "^emu|error"          # the emulator: the same firmware in WebAssembly
mkdir -p companion/emulator
cp app/build/emu/index.html app/build/emu/worklet.js app/build/emu/app.wasm app/web/emu/app.js app/web/emu/app_worklet.js companion/emulator/
PKG=app/build/echofield-$V.fwsc
[ -f "$PKG" ] || PKG=$(ls -t app/build/echofield-*.fwsc | head -1)
rm -f companion/firmware/*.fwsc
cp "$PKG" companion/firmware/
python3 - "$PKG" "$V" <<'PY'
import hashlib, json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = p.read_bytes()
blocks = [d[i * 0x30 + 0x2F] for i in range(20)]
product = "".join(chr((m - i - 1) & 0xFF) for i, m in enumerate(blocks) if m != 0x7D)
json.dump({"name": "ECHOFIELD", "version": sys.argv[2], "file": p.name, "product": product,
           "sha256": hashlib.sha256(d).hexdigest(), "size": len(d)},
          open("companion/firmware/manifest.json", "w"), indent=1)
print(f"companion/firmware: {p.name} {product} sha256 {hashlib.sha256(d).hexdigest()}")
PY
rm -rf dist && mkdir -p dist
cp "$PKG" dist/
(cd dist && shasum -a 256 "$(basename "$PKG")" > SHA256SUMS)
cp -R companion "dist/echofield-companion-$V"
(cd dist && zip -qr "echofield-companion-$V.zip" "echofield-companion-$V" && rm -rf "echofield-companion-$V")
echo "dist/: $(ls dist | tr '\n' ' ')"
