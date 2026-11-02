// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Philip Bos
// START HERE: the quick start and the manual, in plain words. The pictures are the firmware's own screens (the host
// simulator draws them during the tests: app/tests/screens.txt), so they always match what the FM-1 shows.
import { $ } from "./app.js";
import { PARAMS, PAGES, MODES, MODE_BLURB } from "./lib/efparams.js";

const esc = (s) => String(s).replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" })[c]);
const shot = (f, cap) => `<figure><img src="manual/${f}.png" alt="${esc(cap)}" width="240" height="240" loading="lazy"><figcaption>${cap}</figcaption></figure>`;

// what each knob does, in a sentence anyone understands
const SAYS = {
  TEXTURE: "how long the little sound pieces (grains) are: low is crackly and broken, high is smooth and long",
  SCATTER: "how far apart in the sample the grains are picked: low is focused, high is a blurry cloud",
  DRIFT: "slow wandering of pitch and position: makes it feel alive",
  EVOLVE: "how much the sound moves by itself over time",
  PITCH: "transposes up or down in semitones",
  HARMONY: "adds copies an octave or a fifth away: a choir made of your sound",
  REVERB: "how much room / space",
  TAPE: "old-tape character: wobble, warmth and wear",
  ATTACK: "how slowly the sound fades in when you press a key",
  RELEASE: "how long it rings on after you let go",
  KEYS: "GATE: sounds while held · LATCH: press once to hold, again to stop",
  VELOCITY: "how much harder key presses play louder",
  SIZE: "makes all grains shorter or longer",
  DENSITY: "how many grains are born each second",
  ENVELOPE: "the shape of each grain's fade in and out",
  TONE: "darker or airier grains",
  SCAN: "moves the play position through the sample by itself (negative: backwards)",
  REVERSE: "how many grains play backwards",
  "PITCH RND": "random pitch per grain: shimmering detune",
  "SIZE RND": "random length per grain",
  FILTER: "the filter's type: low-pass (darker), band or high-pass (thinner)",
  CUTOFF: "how bright or dark",
  RESO: "a ring at the cutoff",
  DRIVE: "warm saturation",
  TIME: "the echo's delay time",
  FEEDBACK: "how many echoes",
  ECHO: "how loud the echoes are",
  PINGPONG: "echoes bounce left and right",
  DAMP: "softens the reverb's highs",
  SHIMMER: "an octave-up sparkle inside the reverb",
  WOW: "slow tape wobble",
  FLUTTER: "fast tape jitter",
  AGE: "worn tape: dull highs and little dropouts",
  HISS: "tape noise",
  WIDTH: "stereo width",
  LEVEL: "the sound's own volume (MASTER is the overall volume)",
  ORACLE: "turns the DRIFT ORACLE on: it picks notes and chords in a scale by itself",
  ROOT: "the key's root note",
  SCALE: "which scale the oracle uses",
  ACTIVITY: "how often the oracle changes notes and surprises you",
  AMBIGUITY: "low: safe root and fifth · high: colourful extra notes",
  RANGE: "how many octaves its notes spread over",
  GLIDE: "how slowly it slides to new notes",
  DRONE: "a deep root-and-fifth bed",
  START: "where the part of the sample used begins",
  END: "where it ends",
  POSITION: "where the grains come from (SELECT turns it too)",
};

export function init() {
  const pages = PAGES.map((pg) => `<details><summary>${pg.name} <span class="muted">(${pg.group === "home" ? "HOME" : pg.group === "keys" ? "ENV" : pg.group === "grain" ? "LFO" : pg.group === "fx" ? "FX" : pg.group === "oracle" ? "SEL" : "EDIT"} button)</span></summary><ul>` +
    pg.cells.map((c, k) => c === -1 ? "" : `<li><b class="k${k + 1}">K${k + 1} ${c === -2 ? "SAMPLE" : PARAMS[c].label}</b> — ${c === -2 ? "which of your 16 sounds plays" : esc(SAYS[PARAMS[c].label] || "")}</li>`).join("") +
    "</ul></details>").join("");
  const modes = MODES.map((m, i) => `<li><b>${m}</b> — ${esc(MODE_BLURB[i])}</li>`).join("");
  const ccs = PARAMS.filter((p) => p.cc).map((p) => `<tr><td>${p.cc}</td><td>${p.label}</td></tr>`).join("");
  $("manual").innerHTML = `
<section>
  <h2>New in 0.2</h2>
  <ul>
    <li><b>LISTEN</b>: hear a sound as it is, plain, before the grains change it (below: "Listen to a sound as it is").</li>
    <li><b>Both of the FM-1's cores</b> make the sound now: no more crackle when you play hard. <b>GLO</b> shows <b>2 CORES</b>.</li>
    <li><b>A new Sound page</b> in this app: the sound's waveform to drag, big knobs, an XY pad and every effect on view.</li>
    <li><b>The emulator</b>: the whole firmware in your browser, on a drawn FM-1 — try it with no FM-1 at all.</li>
    <li><b>Installing</b> is its own page now (<b>Firmware…</b>), on computers only, and it puts the factory sounds on by itself.</li>
    <li><b>Cleaner output</b>: long reverbs with shimmer no longer build up a hidden offset that took the headroom
      (0.2.4); no more crackle on headphones at high volume (0.2.7: the widest stereo is held in a little near full
      MASTER); a new <b>OUTPUT</b> ceiling, 80 % at first (GLO → SYSTEM).</li>
    <li><b>Recording</b>: record a new sound straight from a microphone, an audio interface or another FM-1 (Samples), and
      record what ECHOFIELD plays, take after take, as WAV files (Sound). Below: "Record a sound" and "Record the FM-1".</li>
  </ul>
</section>

<section>
  <h2>Quick start</h2>
  <p>ECHOFIELD turns a few seconds of any sound into slowly moving clouds, sparkles, tape loops and swells. Five minutes
  is all it takes:</p>
  <ol class="steps">
    <li><b>Install it</b> (once, on a computer — phones and tablets can't install): press <b>Firmware…</b> at the top, plug in
      the FM-1 with a USB cable, press <b>Find the FM-1</b>, tick the three boxes, press <b>Install</b>. The factory sounds
      go on by themselves (your own sounds are never touched).
      <span class="muted">Just want to try it? Open the <a href="emulator/">emulator</a>: it runs right here in the browser.</span></li>
    <li><b>Press PLAY.</b> A slow cloud of sound starts. Turn <b>MASTER</b> (top left) for the volume.</li>
    <li><b>Turn the four knobs</b> at the top right. Their colours —
      <b class="k1">blue</b>, <b class="k2">green</b>, <b class="k3">white</b>, <b class="k4">orange</b> — match the dials at the
      bottom of the screen, so you always see what each one does.</li>
    <li><b>Turn ALGORITHM</b> for another mode (CLOUD, DUST, TAPE, BLOOM, PULSE, DRIFT) and <b>SELECT</b> to move through the sample.
      <b>Turn PRESETS</b> to try the ready-made sounds.</li>
    <li><b>Play the keys.</b> Hold <b>EDIT</b> and press a white key to switch to another of your 16 sounds — you hear that
      sound as it is for as long as you hold the key.
      Press <b>ARP</b> for a surprise (a random sound that still sounds good); <b>OCT-</b> and <b>OCT+</b> together undo it.</li>
  </ol>
  <p class="tip">Lost? <b>Hold HOME</b> on the FM-1: the screen explains the page you are on.</p>
  <div class="shots">${shot("02-play", "The PLAY page: the sample's outline, the playhead and every grain as a dot; the four dials below")}
    ${shot("16-help", "Hold HOME: what this page's knobs do")}</div>
</section>

<section>
  <h2>The screen</h2>
  <ul>
    <li><b>Top left:</b> the page you are on; the little dots show how many pages this button has (press it again for the next).</li>
    <li><b>Top right:</b> the mode, in its own colour. A small dot means "not saved yet"; <b>FRZ</b> means FREEZE is on; a red <b>●</b> means recording.</li>
    <li><b>The middle:</b> your sample. The coloured line is where the sound is coming from, the band around it how spread out,
      the dots are the grains (higher dots are higher notes).</li>
    <li><b>The bottom:</b> the four knobs' values.</li>
  </ul>
  <h3>Listen to a sound as it is</h3>
  <p>The grains change a sound a lot. To hear the sample itself, plain, at its own pitch:</p>
  <ul>
    <li>On the <b>SAMPLE</b> page (the <b>EDIT</b> button), press <b>EDIT</b> again: it plays from START to END, with a white
      line moving across it. Press <b>EDIT</b> once more to stop. The cloud fades out meanwhile and comes back after.</li>
    <li>Hold <b>EDIT</b> and hold a white key: that sound, plain, until you let go.</li>
    <li>On the storage page (<b>GLO</b> twice): pick a sound with K1, press <b>OCT+</b>.</li>
    <li>In this app: the <b>▶</b> next to a sound on the Samples page.</li>
  </ul>
  <div class="shots">${shot("03-sample", "EDIT: the sample, its start (green) and end (white) brackets")}
    ${shot("03b-listen", "Press EDIT again: the sample plays as it is")}
    ${shot("05-filter", "FX pages draw what the effect does")}
    ${shot("14-pulse", "PULSE plays notes on a slow 16-step rhythm")}</div>
</section>

<section>
  <h2>The buttons</h2>
  <ul>
    <li><b>HOME · ENV · LFO · FX · SEL · EDIT</b> open pages of knobs (below). <b>GLO</b>: settings and storage. <b>SAVE</b>: presets.</li>
    <li><b>PLAY</b> starts or stops the endless sound (the keys play either way).</li>
    <li><b>REC</b> records up to 5 seconds of what you hear into a new sound (press again to stop) — resampling, like the OP-1.</li>
    <li><b>SEQ</b> freezes everything in place.</li>
    <li><b>EDIT</b> on the SAMPLE page: LISTEN — the sound as it is, plain (press again to stop).</li>
    <li><b>ARP</b> randomises this page; hold it for everything. Hold <b>ARP</b> and turn a knob to <b>lock</b> it (a lock shows on its dial).</li>
    <li><b>OCT- / OCT+</b> shift the keys an octave. Both together: undo.</li>
  </ul>
  <h3>Hold one button, press a key</h3>
  <ul>
    <li><b>FX + a black key</b>: an effect for as long as you hold it — freeze, reverse, octave up, octave down, stutter, hush,
      bloom, shimmer, tape stop, filter sweep, echo throw.</li>
    <li><b>EDIT + a white key</b>: sound 1 to 16, heard as it is while you hold the key.</li>
    <li><b>SAVE + a white key</b>: your preset 1 to 16. <b>SAVE + a black key</b>: a factory preset.</li>
    <li><b>SEL + any key</b>: the oracle's key (and the oracle switches on).</li>
  </ul>
  <div class="shots">${shot("15-punch", "Hold FX: the black keys' effects")}</div>
</section>

<section>
  <h2>The pages</h2>
  <p>Each page has four knobs. Tap the button again for the next page of the same button.</p>
  ${pages}
  <h3>Modes (ALGORITHM)</h3><ul>${modes}</ul>
</section>

<section>
  <h2>Playing from this app (Sound)</h2>
  <p>Connect the FM-1, open <b>Sound</b>. Everything there moves the FM-1's sound at once, and what you turn on the FM-1 moves
  here too.</p>
  <ul>
    <li><b>The waveform</b> at the top is the sound itself, read from the FM-1. Drag the <b class="k2">green</b> START and the
      <b class="k3">white</b> END to choose the part the grains use, and the <b class="k4">orange</b> line for POSITION.</li>
    <li><b>The 16 chips</b> under it are your sounds: tap one to switch.</li>
    <li><b>▶ PLAY</b> the endless sound · <b>◉ LISTEN</b> the sound as it is (a white line runs across it) · <b>❄ FREEZE</b> ·
      <b>⚄ RANDOM</b> · <b>↺</b> undo.</li>
    <li><b>Mode</b>: six tiles, the same as the ALGORITHM knob.</li>
    <li><b>Perform</b>: the eight main knobs, big. Drag up and down; Shift for fine steps; double-click for the default.</li>
    <li><b>XY pad</b>: drag the orange dot to change two things at once (choose which with X and Y). Lovely on SCATTER and TEXTURE,
      or REVERB and SHIMMER.</li>
    <li>Then every other page, all on view, grouped as the FM-1's buttons: <b>Effects</b> (FX: filter, delay, reverb, tape,
      output), <b>Grain</b> (LFO), <b>Oracle</b> (SEL) and <b>Keys</b> (ENV).</li>
    <li><b>Presets</b>: load, store, rename and delete yours; the factory's are one tap away.</li>
  </ul>
  <h3>Record the FM-1</h3>
  <p>Plugged into a computer, the FM-1 is also an audio input: its sound comes in over the same cable. Under the waveform,
  press <b>● Hear and record the FM-1 here</b> (the browser asks once to use audio inputs); the FM-1 is picked by itself.</p>
  <ul>
    <li><b>🎧 MONITOR</b> plays it through this computer, a little behind the FM-1's own headphones.</li>
    <li><b>● REC</b> starts a take, <b>■ STOP</b> ends it (up to 5 minutes). Each take is listed: <b>▶</b> play, <b>⤓ WAV</b>
      download (24-bit, as it came in), <b>→ Sample</b> make it a new sound on the Samples page, <b>✕</b> forget it.
      With two or more, <b>⤓ All takes</b> saves them in one ZIP.</li>
    <li>The level: <b>MASTER</b>, under <b>GLO → SYSTEM → OUTPUT</b>, the most the FM-1 puts out (USB, headphones and
      speaker alike): <b>80 %</b> at first, 50–100 %. Clipping in a take: turn MASTER or OUTPUT down.</li>
    <li>Takes are kept until you close the page: download the ones you want.</li>
  </ul>
</section>

<section>
  <h2>Your own sounds</h2>
  <ol>
    <li>Plug in the FM-1 and press <b>Connect FM-1</b> (top right of this page).</li>
    <li>On <b>Samples</b>, drop in an audio file (from a phone or a computer), or record one (below). Drag the green and white handles to keep the part you want.</li>
    <li>Choose a quality: <b>16 kHz</b> is a good default; 11 kHz fits more, 32 kHz sounds brightest. <b>▶ As on the FM-1</b> lets you hear it first.</li>
    <li>Pick a slot and press <b>Send to FM-1</b>. It plays straight away. Press <b>▶</b> next to it to hear it as it is.</li>
  </ol>
  <p>Storage is small (about 30–60 seconds in all), so short sounds work best — a chord, a note, a breath, a bit of a song.
  The factory sounds are ordinary sounds: delete them whenever you need room (Samples page, or <b>GLO → GLO</b> on the FM-1).</p>
  <div class="shots">${shot("12-storage", "GLO twice: the storage, and every sound in it")}</div>
  <h3>Record a sound</h3>
  <p>Next to the drop area, press <b>● Record one</b>. Pick the input: a microphone, an audio interface, or another FM-1 over
  USB (an FM-1 running Sloop or the stock firmware is an ordinary audio input when it is plugged in, so whatever it plays
  can become an ECHOFIELD sound). The meter shows the level: keep it out of the red.</p>
  <ul>
    <li><b>● REC</b>, then <b>■ STOP</b> (up to 30 seconds). <b>Start on sound</b> waits until something is heard, so you
      don't record the silence before it.</li>
    <li><b>Monitor</b> plays the input through this computer: use headphones with a microphone, or it will feed back.</li>
    <li>The recording opens in the editor as <b>REC 1</b>, <b>REC 2</b>…: trim it, choose a quality, send it.</li>
  </ul>
</section>

<section>
  <h2>Presets and saving</h2>
  <ul>
    <li>Turn <b>PRESETS</b> to browse; the one you stop on loads.</li>
    <li>To keep a sound: press <b>SAVE</b>, pick a <b>U</b> slot with K1, press <b>SAVE</b> again (and once more to overwrite).</li>
    <li>The FM-1 also remembers your last sound by itself, a few seconds after the sound stops. Saving pauses the audio for a moment, so it waits for quiet.</li>
  </ul>
  <div class="shots">${shot("10-presets", "SAVE: your presets (U) and the factory's (F)")}</div>
</section>

<section>
  <h2>Problems?</h2>
  <details><summary>No sound</summary><p>Turn MASTER up, press PLAY. Still nothing: a preset may have its LEVEL low (FX → OUTPUT page).</p></details>
  <details><summary>It crackles</summary><p>Look at <b>GLO</b>: it should say <b>2 CORES</b> and a CPU figure well under 80 %. If it says
    1 CORE, switch the FM-1 off and on. Very dense settings (32 grains, DENSITY ×4) can still be a lot: ECHOFIELD then plays
    fewer grains rather than crackle. On headphones, only near full volume: update to 0.2.7 or later (it holds the
    widest stereo in a little, which the FM-1's headphone output needs at high level).</p></details>
  <details><summary>A preset says "NEEDS …"</summary><p>That preset was made for a factory sound that isn't on your FM-1. Install it on the Samples page (Factory sounds).</p></details>
  <details><summary>This page can't find the FM-1</summary><p>Use Chrome, Edge or Opera; plug the FM-1 straight in with a data cable; close other apps
    that use MIDI (and other tabs of this page); press Connect again.</p></details>
  <details><summary>Recording: no input, or silence</summary><p>Allow audio inputs for this page (the microphone icon in
    the address bar). Plug the FM-1 in with a data cable and choose it in <b>Input</b>; on a Mac it can take a few seconds to
    appear. Silence from the FM-1: turn MASTER up. Clipping: turn MASTER or <b>GLO → SYSTEM → OUTPUT</b> down. Now and then the
    FM-1's USB audio can drop a tiny piece (a known issue in the platform's USB audio, being looked at upstream): for a
    flawless take, record its headphone output with an audio interface instead.</p></details>
  <details><summary>Updating ECHOFIELD</summary><p>Open <b>Firmware…</b> on a computer and install: your sounds and presets stay.
    (On a phone or tablet the button is hidden: installing there is too easy to interrupt.)</p></details>

  <details><summary>Back to the original firmware</summary><p>In <b>Firmware…</b>, under "Back to the stock firmware", or with M-VAVE's own M-UPGRADE.</p></details>
</section>

<section>
  <h2>MIDI</h2>
  <p>Notes in over USB and the 3.5 mm jack play the keys; the keys send notes out. Set the channel in <b>GLO</b>.
  CC 64 holds notes, CC 102 changes the mode, 103 freeze, 104 play, Program Change loads a preset (0–15 yours, then the factory's).
  These CCs move the knobs:</p>
  <table><tr><th>CC</th><th>Knob</th></tr>${ccs}</table>
  <h3>Licences</h3>
  <p class="small muted">ECHOFIELD FM-1 and this app: MIT. The platform (fm1-base, felucca-bsp), the installer, the mirror and the emulator code:
  MPL-2.0, by Charles Vestal, Leo Kuroshita (Hügelton Instruments), Chris Farrell and Kerem Kilic. Factory sounds: CC0.
  Not affiliated with M-VAVE; FM-1 is M-VAVE's trademark.</p>
</section>`;
}
