#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Philip Bos
"""ECHOFIELD's build: fm1-base's tools/build.py, unchanged, with the package identity fixed at FM-1_324.

fm1-base gives a release the identity id_prefix + XXYYZZ. A firmware on felucca-bsp's chip/ota.c answers the plain
identity request with the name and the first three digits only (M-UPGRADE 3.11 takes no more), and the whole identity
to the request with the body 01. Installers that send only the plain request (The Firmware Hub's) then read FM-1_324
after an install, not the package's FM-1_3246XXYYZZ, and report a mismatch. With an identity of three digits both
answers are the same, everywhere. The version is in the package's name and the firmware's own HELLO.

  python3 tools/build.py app [--release X.Y.Z | --host | --web | ...]     (the same arguments as base/tools/build.py)
"""
import sys
from pathlib import Path

IDENTITY = "FM-1_324"                   # "ECH(O)" on a phone keypad; FM-1_3246XXYYZZ from 0.2.8 to 0.2.10

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "base" / "tools"))
import build  # noqa: E402  (fm1-base's)

# build.main() sets build.PRODUCT from id_prefix and --release, then asks firmware_json.require before anything is
# compiled or packed: the identity is put in place there
_require = build.firmware_json.require


def _require_fixed(*a, **k):
    build.PRODUCT = IDENTITY
    return _require(*a, **k)


build.firmware_json.require = _require_fixed

if __name__ == "__main__":
    build.main()
