# DRIFT

An FM keys synth with a chance arpeggiator, a ping-pong delay and a reverb for the M-VAVE FM-1, for generative
playing: you choose the notes, the arp chooses when. Built on [fm1-base](https://github.com/charlesvestal/fm1-base)
(MPL-2.0), a git submodule in `base/`; the app is in `app/`.

## Controls

| Control | What it does |
| --- | --- |
| Keys | Notes into the arp (or played straight, with the arp off). With HOLD, a new chord after you let go replaces the last |
| PLAY | Arp on / off |
| REC | HOLD on / off (off by default: a key let go stops) |
| PRESETS | The sound: E.PIANO, BELLS, GLASS, KALIMBA, MARIMBA, PAD, MUSIC BOX, HARP (loads its envelope) |
| SELECT / ALGORITHM | Tempo / chance, on every page |
| SEL, ENV, ARP, FX, GLO | The page K1-K4 edit (below) |
| OCT- / OCT+ | Octave |
| SAVE | Save everything (lit when there are unsaved changes) |

| Page | K1 | K2 | K3 | K4 |
| --- | --- | --- | --- | --- |
| SEL: sound | Bright (FM index) | Tine level | Detune (width) | Velocity |
| ENV | Attack | Decay | Sustain | Release |
| ARP | Rate | Chance | Octaves | Mode (up, down, up-down, random) |
| FX | Delay time | Feedback | Delay mix | Reverb mix |
| GLO | Tempo | Reverb size | Reverb tone | Gate |

Every note the arp plays also goes out over USB MIDI; MIDI notes in feed the arp like the keys.

## Build

    git clone --recursive <this repo>
    ./build.sh --host && tests/run_tests.sh      # host simulator, no JieLi toolchain needed
    ./build.sh --web                             # browser emulator: app/build/emu/ (serve it over http)
    ./build.sh                                   # the device package: app/build/drift.fwsc

On this Mac `build.sh` finds an arm64 FriBiDi in `~/.local/fribidi-arm64` (FRIBIDI_LIB) and Emscripten in `~/emsdk`
(EMSDK).

## Before a release

An own `id_prefix` in `app/fm1.toml` (it still has the template's), the repository and the words `url:` in
`app/words.tsv`, the `# <- yours` lines in `.github/workflows/release.yml` (`pkg=drift`), and a manual
(`app/docs/MANUAL.md` is still the template's).
