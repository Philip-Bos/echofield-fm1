# Changelog

## 0.2.11

- **Installs from The Firmware Hub finish cleanly.** ECHOFIELD's identity is now **FM-1_324**, the same to every
  installer. Before, a plain identity request got FM-1_324 and only installers that asked for more got the whole
  FM-1_3246…, so the hub's installer wrote ECHOFIELD and then reported "mismatch: installed, but the device reports
  FM-1_324". The version is still in the package's name and on the FM-1's own screen.

## 0.2.10

- **Newer FM-1s save again.** FM-1s sold since the autumn (mostly from AliExpress) have a UC25HQ80 flash chip (JEDEC
  B36014) instead of the Puya P25Q80 (856014). ECHOFIELD only knew the Puya, so on those units it could not save
  presets, settings or samples, nor back up. It now takes both.
- **Found by M-UPGRADE 3.11 again.** M-VAVE's updater (3.11, 3 November) refuses an identity with more than three
  digits, so a plain identity request now gets FM-1_324 and the whole identity (FM-1_3246…) goes to installers that
  ask for it, as the flasher, tools/flash.py and the Firmware Hub do. The flasher already takes the fast install of
  firmwares whose update loader offers it.
- **Sounds go on reliably.** Putting several samples on in a row (the flasher's sounds step, tools/efsounds.py, the
  companion) could lose about one in nine ("stopped asking for data"): the FM-1 holds one incoming frame, and the
  next sample's request could arrive while it still had the last. The computer now sends a request again when the
  FM-1 did not take it, and waits for it before the next.
- Built on fm1-base of 5 November 2026 (35a0348, felucca-bsp 41a6e86), thanks to everyone who contributed to them.
  firmware.json describes ECHOFIELD for firmware hubs. fm1-base's newer update of felucca-bsp (its fast install, the
  SysEx input queue, the flash gate) waits: with it, the FM-1 refused the installers' package check every time on
  hardware, so nothing could be installed over it.
- The browser emulator plays in tune in browsers that run audio at 48 kHz (fm1-base's resampling).
- The emulator has the companion's **sample editor** under the panel: drop in or record a sound, trim it, pick a
  quality, hear it as the FM-1 will play it and put it in a slot; listen to, export, rename and delete samples, and put
  factory sounds back. The samples stay in the browser until **Factory reset**.
- The emulator always starts with **all 14 factory sounds**. Before, a second visit could find them all DAMAGED (the
  page kept the bank's directory but not the samples) and the bank stayed empty.

## 0.2.9

- **Follows MIDI clock.** START and STOP from a host (FM-1 Relay, a DAW, another FM-1) play and stop the endless
  sound, so ECHOFIELD starts and stops with the rest of a rig. While a 24 PPQN clock comes in, **PULSE** steps on the
  clock's grid (its 1/16, 1/8 or 1/4, whichever is nearest the pace EVOLVE sets) and is put back on the beat every
  quarter note, and the **delay** time snaps to the note value nearest the TIME knob (1/16 to 1/2, triplets and dotted
  included). Half a second without a clock and ECHOFIELD keeps its own time again. Nothing to set: any channel works.

## 0.2.8

- A package identity of its own: **FM-1_3246…** ("ECHO" on a phone keypad), so the Firmware Hub and the update tools can
  tell ECHOFIELD apart. Up to 0.2.7 it was FM-1_47…, a prefix FRASIL already uses. Nothing else changes: presets,
  settings and samples stay, and the flasher still recognises an FM-1 running an older ECHOFIELD.
- Releases are built and signed on GitHub from the tagged source (fm1-build), and listed on the Firmware Hub.

## 0.2.7

- No more crackle on the headphone output at high volume. The FM-1's headphone amplifier, loaded by headphones, breaks
  up when left and right swing far apart at high level, and ECHOFIELD's wide reverb does exactly that. A stereo-width
  limiter on the output keeps (L − R) / 2 under −17 dBFS. It only acts near full MASTER.
- Diagnostics for hardware testing (all off by default): a test tone that follows MASTER, a mono output, the side
  limiter's ceiling, and render timing in STATS. See [DESIGN.md](DESIGN.md).

## 0.2.5

- **OUTPUT** (GLO → SYSTEM, knob 4): a ceiling on everything the FM-1 puts out, 50–100 %, **80 %** by default. USB
  audio carries what the headphones get.

## 0.2.4

- Fixed: long reverbs with shimmer (RAIN CATHEDRAL, GLASS CHOIR) built up a hidden DC offset in the shimmer's
  feedback until the limiter held it. It used up the headroom, so recordings over USB clipped. There are now DC
  blockers in the shimmer loop and on the output, with a test.

## 0.2.3

- USB audio level setting (replaced by OUTPUT in 0.2.5).
- ▶ on the companion's Samples page always plays the sample you click, even if the FM-1 switched sample since.

## 0.2 – 0.2.2

- **Both cores** render the grains, the hot loops run from RAM, and the grain loop uses integer arithmetic: no more
  crackle when playing hard (render peak 28–54 % on hardware, no dropouts).
- **LISTEN:** hear a sample as it is, before the grains change it (EDIT on the SAMPLE page; EDIT + a white key while
  held).
- **The emulator:** the whole firmware compiled to WebAssembly, on a drawn FM-1.
- **The flasher** is its own page, on computers only, and puts the factory sounds on after installing.
- **The companion:** a manual with a quick start, a new Sound page (waveform, modes, big knobs, an XY pad, every
  effect on view), and recording (a new sample from any input; takes of the FM-1 itself as WAV files).
- 0.2.2 fixed LISTEN reading past the end of a sample.

## 0.1 – 0.1.1

- First release: the granular engine with its six modes, the DRIFT ORACLE, performance effects, RANDOM with locks,
  RESAMPLE, a 16-slot sample bank in its own ADPCM format, presets and the session kept across restarts, the companion
  app (samples, sound, screen mirror) and the safe installer.
