#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Philip Bos
"""Install ECHOFIELD from the command line, with the companion app's checks first: the package is ECHOFIELD (its
identity), matches the release's SHA-256 (companion/firmware/manifest.json, or --sha), then fm1-base's installer
(base/tools/fm1_install.py: the same M-UPGRADE protocol and update loader as the web page).
  python3 tools/flash.py [PACKAGE.fwsc] [--sha HEX] [--yes] [--sounds starter|all|none]
Then the factory sounds go onto an FM-1 whose bank is empty (tools/efsounds.py).        (needs: pip install mido python-rtmidi)"""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith("--") and sys.argv[i - 1] not in ("--sha", "--sounds")]
man = json.loads((ROOT / "companion/firmware/manifest.json").read_text())
pkg = Path(args[0]) if args else ROOT / "companion/firmware" / man["file"]
want = sys.argv[sys.argv.index("--sha") + 1] if "--sha" in sys.argv else (man["sha256"] if pkg.name == man["file"] else None)
d = pkg.read_bytes()
blocks = [d[i * 0x30 + 0x2F] for i in range(20)]
product = "".join(chr((m - i - 1) & 0xFF) for i, m in enumerate(blocks) if m != 0x7D)
if not product.startswith("FM-1_47"):
    sys.exit(f"{pkg.name} is {product}, not ECHOFIELD: nothing done")
sha = hashlib.sha256(d).hexdigest()
if want and sha != want.lower():
    sys.exit(f"{pkg.name}: SHA-256 {sha} is not the release's {want}: download it again. Nothing done")
print(f"{pkg.name}: {product}, SHA-256 {sha}" + (" (matches the release)" if want else " (no release SHA to compare: --sha)"))
print("Keep the USB cable in until it says the FM-1 is back.")
rc = subprocess.call([sys.executable, str(ROOT / "base/tools/fm1_install.py"), str(pkg)] +
                     (["--yes"] if "--yes" in sys.argv else []))
if rc:
    sys.exit(rc)
# the sounds, onto an empty bank (yours are never touched): --sounds starter (the default) | all | none
snd = sys.argv[sys.argv.index("--sounds") + 1] if "--sounds" in sys.argv else "starter"
sys.exit(subprocess.call([sys.executable, str(ROOT / "tools/efsounds.py"), snd]))
