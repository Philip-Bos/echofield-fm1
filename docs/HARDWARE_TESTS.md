# The first FM-1: what to check

Everything here has passed on a computer: the engine, the app in fm1-base's FM-1 simulator, the protocol, the mirror,
the installer's checks, the firmware build and its toolchain checks. What only an FM-1 can show is below, in the order
to try it. Write down what you see; the companion's Screen tab saves PNGs.

Before: note the stock firmware version (M-UPGRADE shows it) and have its package at hand (docs/RECOVERY.md).

## 1. Install

- [ ] Flash tab: Find the FM-1 names the stock firmware; Install runs every step to **Done**; the FM-1 shows the
      ECHOFIELD splash, then the PLAY page with SEED at the bottom left.
- [ ] Install again over ECHOFIELD (an update): Done, and nothing it kept is lost.
- [ ] Unplug halfway through a write, plug back in: Find says "update loader", **Resume** finishes.
- [ ] The Firmware Hub's checks, on the release package (the one GitHub built and signed):
  - [ ] From the stock V15: installs, starts, and the FM-1 answers with ECHOFIELD's identity (FM-1_3246…).
  - [ ] From another firmware (Sloop, say): installs and starts.
  - [ ] Leaving it: another firmware installs over ECHOFIELD (from the hub or its own page), and so does the stock
        firmware.
  - [ ] Over an older ECHOFIELD (0.2.7 answers FM-1_47000207): the flasher still names it ECHOFIELD and updates it,
        and the presets, settings and samples stay.

## 2. Sound and load

- [ ] PLAY: the seed sound plays through the headphones and the speaker, no crackle.
- [ ] GLO → SYSTEM shows CPU %. Note it in each mode with PLAY on (default settings): CLOUD ___ DUST ___ TAPE ___
      BLOOM ___ PULSE ___ DRIFT ___. Then GRAINS 32, DENSITY x4, SIZE x4 in CLOUD: ___ % and whether GRAINS n/m drops
      (the overload guard) and whether it crackles.
- [ ] MASTER turns the level smoothly.
- [ ] USB audio: the computer records the FM-1 (its level: MASTER under GLO → SYSTEM → OUTPUT, 80 % by default), no clipping.

## 3. Panel

- [ ] Every page button opens its page; again: the next page. Hold HOME: the help.
- [ ] K1–K4 follow their dials; fast turns go further. SELECT moves the playhead, ALGORITHM changes the mode,
      PRESETS loads presets.
- [ ] Keys play notes with velocity; C4 sounds at the sample's pitch; OCT- / OCT+.
- [ ] FX + each black key: its effect while held, gone on release.
- [ ] ARP: RANDOM; hold: everything; ARP + knob: the lock icon; OCT- + OCT+: undo.
- [ ] SEQ freezes; PLAY lights green.

## 4. Samples

- [ ] Samples tab: install the factory sounds. The FM-1 shows RECEIVING SAMPLE; afterwards GLO → STORAGE lists them.
      Time for the 8 starter sounds: ___ s.
- [ ] Drop in your own WAV at each rate; play it; "As on the FM-1" in the companion sounds the same as the FM-1.
- [ ] EDIT + white keys switch samples without a click.
- [ ] Rename, delete, export as WAV from the companion.
- [ ] Switch off and on: every sample is there, the last sound and sample come back (wait ~5 s after stopping before
      switching off: the session is kept once the sound has stopped).
- [ ] REC records 2–5 s, stops, saves REC 1 and plays it.
- [ ] Fill the bank: the companion refuses the next sample and says why; nothing half-written appears.

## 5. Companion

- [ ] Sound tab: knobs move the FM-1's sound; turning K1 on the FM-1 moves the knob in the app.
- [ ] Screen tab: the screen shows live; "Play it from here" works the buttons, keys and knobs.
- [ ] Backup, then restore onto an empty bank: samples and presets back.

## 6. Recovery (optional, careful)

- [ ] Back to the stock firmware from the Flash tab; then ECHOFIELD again.

## Results so far (one FM-1, 2026-11-01, firmware 0.2 code)

- Install, update, interrupted install resumed: done (the web flasher and `tools/flash.py`).
- Second core: running ("2 CORES"); the render's peak, us of its 5,805 us (CLOUD / DUST / TAPE / BLOOM / PULSE / DRIFT):
  2,453 / 1,927 / 1,966 / 1,998 / 1,674 / 2,296 (28–44 %); a 4-note chord with HARMONY 60 % and DENSITY x2: 3,138 (54 %).
  Dropouts (xruns): none in any of these.
- Listening (headphones): a pure test tone clean; CLOUD clean; the heavy chord clean.
- Samples: the 14 factory sounds installed from the companion; the flasher's sounds step leaves a bank with samples alone.
- Known, upstream: recording the FM-1 over USB audio can splice whole 256-sample blocks (seen with a plain test tone,
  ECHOFIELD's engine bypassed): fm1-base / felucca-bsp's USB audio path. The headphones and speaker are not affected.

## Results, 0.2.3–0.2.4 (one FM-1, 2026-11-02)

- 0.2.5: OUTPUT (50–100 %, 80 % by default) is a ceiling on everything the FM-1 puts out, MASTER below it; fm1-base's
  separate USB level (plat_usb_audio_full) is no longer used. (It was first blamed for the crackle below; wrongly.)

## The headphone crackle (0.2.7)

Crackle on the 3.5 mm output near full MASTER, with headphones; 0.2.2 had it too. Found by elimination:
- clean: a pure tone at any level up to −1 dBFS; all effects bypassed; the reverb summed to mono; the jack recorded
  on a line input (Scarlett Solo); USB.
- crackles: the reverb in stereo, on headphones, loud. Not timing (every block on time, 0 xruns, one core or two),
  not clipping (peaks −4 to −9 dBFS), not the code (the same signal is clean on a line input).
- So: the FM-1's headphone amplifier, loaded by headphones, does not like left and right swinging far apart at high
  level, which ECHOFIELD's wide reverb does and most firmwares never do.
- Fix: a side limiter on the output, (L − R) / 2 held under −17 dBFS, the sum untouched. Measured over USB at full
  MASTER: side peaks −14 → −17 dBFS, side RMS 1–2 dB lower. On headphones: −20 and −17 clean, without it crackles.
- Found and fixed in 0.2.4: a long reverb with shimmer (RAIN CATHEDRAL, GLASS CHOIR) let a DC offset build up in the
  shimmer's feedback until the limiter held it (about −0.8 of full scale, measured over USB). It stayed after the
  sound stopped and through preset changes, and it took most of the headroom, so takes clipped. A DC blocker in the
  shimmer loop and one on the output; dsp_test checks it. After the fix, measured over USB: RAIN CATHEDRAL 25 s, DC
  within ±0.005; silence after it fades to −66 dB.
- ▶ on the Samples page plays the sample on the FM-1 (recorded over USB, its whole length).

