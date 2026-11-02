# ECHOFIELD FM-1: design

ECHOFIELD (first a granular sampler for the R36S handheld) as native firmware for the M-VAVE FM-1 (JieLi AC791N, pi32v2 at
360 MHz with a single-precision FPU, 240×240 ST7789, 27 keys, 14 buttons, 7 encoders, 1 MiB flash, 96 KB RAM + 336 KB
pool).

## Foundation

| Layer | Source | Licence |
| --- | --- | --- |
| chip, board, update loader, package tools, rescue | felucca-bsp (`base/bsp`, pinned) | MPL-2.0 |
| platform: screen, panel, audio, MIDI, store, safe mode, mirror, backup, host simulator | fm1-base (`base/`, pinned) | MPL-2.0 |
| the app, the engine, the sample format, the companion | this repository | MIT |

The second core is used (fm1.toml `cpu2`, with `cpu2_fallback`: a failing core is given up, one core until
power-off, never a restart). Not used: boot_pack, Bluetooth, WiFi, USB host, expanded flash.

## Flash

| Region | Range | Use |
| --- | --- | --- |
| boot area | 0x00000–0x04000 | never written by anything here |
| app slot | 0x04000–0x93000 | the firmware (169 KB of 581 KB) |
| XDATA0 | 0x93000–0x97000 | bank sectors 0–3 |
| data area | 0x97000–0xD8000 | bank sectors 4–68 |
| store | 0xD8000–0xE0000 | session, presets, bank directory, panel calibration (A/B pairs) |
| update staging | 0xE0000–0xE5000 | felucca-bsp's, untouched |
| XDATA1 | 0xE5000–0xFC000 | bank sectors 69–91 |
| settings pair | 0xFC000 | settings |

## Samples (EFB1)

* Mono IMA ADPCM, rate per sample (8–48 kHz; the companion offers 32 / 22.05 / 16 / 11.025 kHz).
* Blocks of 256 samples: a 4-byte header with the decoder state before the block, 128 bytes of nibbles. Any block
  decodes on its own: random access, no index.
* A sector holds 29 blocks (3828 bytes, 7424 samples) and a trailer at 0xFE0 (magic, slot, index, CRC-32, gen); bytes
  0xF00–0xF07 stay erased so the update loader never takes a sector for an update record.
* The directory (one store object) gives each of 16 slots a name, frames, rate, CRC-32 and a sector bitmap: sectors are
  taken anywhere, so no compaction is ever needed. A sample is written into free sectors, read back, then entered in the
  directory; the one it replaces leaves the directory first. Boot checks every trailer and CRC.
* The companion's encoder (`companion/lib/efcodec.js`) is the only one; it tries several starting states and a
  two-sample look-ahead per block. The firmware's decoder matches its decoder bit for bit (tests).

## Engine (`app/dsp/engine.c`)

A port of the R36S version's engine: six modes, the DRIFT ORACLE, constrained RANDOM, keys and a gate envelope, eleven
performance effects, RESAMPLE. Grains read the flash in place through a one-block cache each (Q20.12 positions, 8-bit
interpolation, Q15 windows, Q14 pan and colour: integer throughout), up to 32 (default 20) with a decode budget. The
odd grains render on the second core while the first does the even ones. The grain loop and the effects chain run
from RAM (.ram_text, called through pointers: no call may leave RAM); everything they use is inlined, the ADPCM tables
copied to RAM. Measured on the FM-1 (us per 64 samples, before → after): grains 582 → ~350 (two cores), effects
465 → 341; per-sample divisions and sines in the effects moved to every 8–16 samples. Effects at 44.1 kHz: tape stage, SVF,
saturation; delay (1 s, int16) and an 8-line FDN reverb with shimmer at 22.05 kHz; width, auto-level (which never pulls
a dying tail back up), a soft limiter. The overload guard sheds grains when the render nears its budget.

Two DC blockers keep the output centred: one in the shimmer's feedback (an octave up of 0 Hz is still 0 Hz, and a long
reverb's gain at DC once let an offset grow until the limiter held it) and one on the output. After the engine
(`app_render`): OUTPUT, the ceiling under MASTER for everything the FM-1 puts out (50–100 %, 80 % by default; USB
carries what the headphones get), and a **side limiter**: (L − R) / 2 held under −17 dBFS. The FM-1's headphone
amplifier crackles when, loud and loaded by headphones, left and right swing far apart (a wide reverb); in mono, or on
a line input, it never does. The limiter only acts near full MASTER.

## Saving

Settings, user presets and the session are store objects (fm1-base: A/B sectors, CRC, torn-write safe). A flash write
stops the audio for tens of milliseconds, so the session is kept 3 s after the last change once nothing has sounded
for 1.5 s (at most 10 s into a tail); presets and settings are written with the sound faded around them.

## Companion protocol (`app/proto.c`, `companion/lib/efproto.js`)

fm1-base's SysEx framing, commands 0x70–0x7D: HELLO, DIR, UPLOAD (the FM-1 pulls the payload with read requests, as
the update loader does), ERASE, RENAME, PARAMS, SET, PRESETS, PRESET OP, READ, WIPE, PRESET GET / PUT, STATS, and
NOTIFY from the FM-1 when a knob turns. The screen mirror and backup are fm1-base's own. The FM-1 holds one incoming
frame at a time, so the companion sends one exchange at a time (`EfLink.exclusive`).

SET takes parameters by id, and a few actions above them: 0xF0 PLAY, 0xF1 FREEZE, 0xF2 select a slot, 0xF3 RANDOM,
0xF4 undo, 0xF6 LISTEN (0 stop, 1 once, 2 looped), plus the diagnostics below.

## Diagnostics

For finding problems on hardware; all off by default, none kept across a restart.

| | |
| --- | --- |
| SET 0xF5 (bits) | 1 tape, 2 filter, 4 delay / reverb / shimmer bypassed; 8 a 440 Hz test tone at MASTER (the engine out of the way, up to −1 dBFS); 32 key velocity sensing off; 64 the output in mono |
| SET 0xF7 | the side limiter's ceiling, in thousandths of full scale (1000: off) |
| STATS (0x7D) | render, grains, effects and spawn peaks (us), CPU %, xruns, grains, shed, the cap, the second core; then frames rendered, the longest wait between renders (a skipped block shows as two halves), waits over 1.5 halves, the time |

[docs/HARDWARE_TESTS.md](docs/HARDWARE_TESTS.md) has the measurements taken with them.
