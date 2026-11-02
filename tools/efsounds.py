#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Philip Bos
"""The factory sounds onto an FM-1 running ECHOFIELD whose bank is empty, over USB-MIDI (the flasher's sounds step,
companion/flasher/flasher.js installSounds, from the command line). A bank with samples is left alone.
  python3 tools/efsounds.py [starter|all|none] [--port NAME]      (needs mido with python-rtmidi)"""
import json
import sys
import time
import zlib
from pathlib import Path

import mido

ROOT = Path(__file__).resolve().parents[1]
FAC = ROOT / "companion/factory"


def pack7(d):
    out, acc, nb = [], 0, 0
    for b in d:
        acc |= b << nb
        nb += 8
        while nb >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            nb -= 7
    if nb:
        out.append(acc & 0x7F)
    return out


def unpack7(s):
    out, acc, nb = [], 0, 0
    for b in s:
        acc |= (b & 0x7F) << nb
        nb += 7
        while nb >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nb -= 8
    return out


class FM1:
    def __init__(self, port=None):
        ins = [n for n in mido.get_input_names() if (port and n == port) or (not port and not any(x in n.lower() for x in ("update", "ota")))]
        for name in ins:
            if name not in mido.get_output_names():
                continue
            self.i, self.o = mido.open_input(name), mido.open_output(name)
            if self.ask(0x70, [], 1.0) and bytes(self.last[1:5]) == b"EFM1":
                return
            self.i.close(), self.o.close()
        raise SystemExit("no FM-1 running ECHOFIELD answers")

    def send(self, cmd, body):
        n = len(body)
        self.o.send(mido.Message("sysex", data=pack7([0, 0x59, cmd, n & 255, (n >> 8) & 255, 0, *body, (~sum(body)) & 255])))

    def recv(self, t):
        end = time.time() + t
        while time.time() < end:
            for m in self.i.iter_pending():
                if m.type == "sysex":
                    u = unpack7(m.data)
                    if len(u) >= 7 and u[0] == 0 and u[1] == 0x59:
                        return u
            time.sleep(0.001)
        return None

    def ask(self, cmd, body, t=2.0):
        for _ in range(3):
            self.send(cmd, body)
            end = time.time() + t
            while time.time() < end:
                u = self.recv(end - time.time())
                if u and u[2] == cmd:
                    self.last = u[6:6 + (u[3] | u[4] << 8)]
                    return self.last
        return None

    def upload(self, slot, pay, s):
        name = list(s["name"].encode()[:11]) + [0] * (12 - len(s["name"][:11]))
        self.send(0x72, [slot, 1, s["rate"] & 255, s["rate"] >> 8, *s["frames"].to_bytes(4, "little"),
                         *s["crc"].to_bytes(4, "little"), *name])
        last = time.time()
        while time.time() - last < 15:
            u = self.recv(1.0)
            if not u or u[2] != 0x30:
                continue
            last = time.time()
            addr = int.from_bytes(bytes(u[7:11]), "little")
            ln = u[11] | u[12] << 8
            top = addr & 0xFF000000
            data = list(b"success\0") if top in (0xC0000000, 0xCF000000) else list(pay[addr:addr + ln])
            bd = [0, 0x59, 0x30, (len(data) + 8) & 255, (len(data) + 8) >> 8, 0, u[6], *addr.to_bytes(4, "little"),
                  len(data) & 255, len(data) >> 8, 0, *data]
            bd.append((~sum(bd[6:])) & 255)
            self.o.send(mido.Message("sysex", data=pack7(bd)))
            if top == 0xC0000000:
                return
            if top == 0xCF000000:
                raise SystemExit(f"{s['name']}: the FM-1 refused it (code {addr & 0xFFFF})")
        raise SystemExit(f"{s['name']}: the FM-1 stopped asking for data")


def main():
    choice = next((a for a in sys.argv[1:] if a in ("starter", "all", "none")), "starter")
    port = sys.argv[sys.argv.index("--port") + 1] if "--port" in sys.argv else None
    if choice == "none":
        return print("sounds: none chosen")
    fm = None
    for _ in range(10):                  # (the FM-1 restarting after an install)
        try:
            fm = FM1(port)
            break
        except SystemExit:
            time.sleep(1)
    if not fm:
        raise SystemExit("sounds: no FM-1 running ECHOFIELD answers; put them on from the companion's Samples page")
    d = fm.ask(0x71, [])
    if any(d[1 + i * 20] for i in range(16)):
        return print("sounds: the FM-1 already has samples: they are kept")
    sounds = [s for s in json.loads((FAC / "factory.json").read_text())["sounds"] if choice == "all" or s["starter"]]
    for k, s in enumerate(sounds):
        pay = (FAC / s["file"]).read_bytes()
        if zlib.crc32(pay) != s["crc"]:
            raise SystemExit(f"{s['file']} is damaged")
        fm.upload(k, pay, s)
        print(f"sounds: {s['name']} ({k + 1}/{len(sounds)})")
    fm.ask(0x76, [0xF2, 0, 0])
    print(f"sounds: {len(sounds)} installed")


if __name__ == "__main__":
    main()
