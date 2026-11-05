# Changelog

## Unreleased (the web emulator; the FM-1's firmware is unchanged)

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
