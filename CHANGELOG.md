# Changelog

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
