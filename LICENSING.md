# Licensing

| What | Licence | Where |
| --- | --- | --- |
| ECHOFIELD FM-1: the app, the engine, the codec, the tools, the tests | MIT | `app/`, `tools/`, `docs/` |
| The companion app's own code | MIT | `companion/*.js`, `companion/lib/`, `companion/*.html`, `*.css` |
| The factory sounds | CC0-1.0 | `companion/factory/` (synthesised by the R36S ECHOFIELD's sample generator, encoded by `tools/make_factory.mjs`) |
| fm1-base (platform, simulator, build) and felucca-bsp (chip, board, update loader) | MPL-2.0 | `base/` (git submodule, pinned) |
| fm1-base's installer, package, port and mirror code, unchanged | MPL-2.0 | `companion/vendor/fm1-base/` (`UPSTREAM_COMMIT`, `LICENSE-MPL-2.0`) |
| The emulator's page and worklet (fm1-base's, the panel restyled) and the WebAssembly build of the firmware | MPL-2.0 (page), the firmware as above | `app/web/emu/`, `companion/emulator/` |
| JieLi AC79 SDK files the package build takes | Apache-2.0 | fetched by the build, never committed |
| Fonts in the firmware (fm1-base's) | SIL OFL 1.1 | `base/assets/fonts/` |

MPL-2.0 applies file by file: changes to those files are published under MPL-2.0; ECHOFIELD's own files stay MIT. A
firmware package (`.fwsc`) contains all of the above compiled together; its licence texts are these files.

Felucca, SLOOP, JIANT, Skiff and Melodee (GPL-3.0) were read for their design; none of their code is in ECHOFIELD.
The companion's recording (`companion/lib/audioin.js`, `companion/takes.js`) follows MIDI Master's way of opening an
FM-1's USB audio in the browser (MIT, FM-1 Relay contributors); the code is ECHOFIELD's own.
