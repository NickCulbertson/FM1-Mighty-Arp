# Mighty Arp

An FM synth with a chance arpeggiator, a chorus, a ping-pong delay and a reverb with shimmer for the M-VAVE FM-1,
for generative playing: you choose the notes, the arp chooses when. Built on
[fm1-base](https://github.com/charlesvestal/fm1-base) (MPL-2.0), a git submodule in `base/`; the app is in `app/`
(the sound in `app/dsp/`). [SPEC.md](SPEC.md) is the design.

## Controls

| Control | What it does |
| --- | --- |
| Keys | Notes into the arp (or played straight, with the arp off) |
| PLAY | Arp on / off |
| REC | Hold: NO HOLD (a key let go stops), HOLD (a new chord after you let go replaces the last), LATCH (each key on or off by itself) |
| PRESETS | The sound (37: E.pianos, keys, bells, plucks, basses, synths, pads); it sets the ENV, LFO and EDIT pages |
| SELECT / ALGORITHM | Tempo (BPM) / arp chance, on every page |
| HOME, SEL, ENV, LFO, EDIT, ARP, SEQ, FX, GLO | The page K1-K4 edit (below); FX again flips DELAY and REVERB |
| OCT- / OCT+ | Octave |
| SAVE | Save everything (lit when there are unsaved changes) |

| Page | K1 | K2 | K3 | K4 |
| --- | --- | --- | --- | --- |
| HOME | Arp chance | Rate | Space (reverb mix + length) | Drift (vibrato) |
| SEL: sound | Tone (FM depth) | Strike (the attack, the tine) | Detune | Velocity |
| ENV | Attack | Decay | Sustain | Release |
| LFO | Rate | Shape (sine, tri, square, S&H, drift) | Vibrato | Tremolo (stereo) |
| EDIT | Algorithm (1-8) | Ratio (the modulators') | Feedback (0-7) | Wave (W1-W8) |
| ARP | Rate | Octaves | Mode (up, down, up-down, as played, converge, diverge, thumb, pinky, chord, random, walk) | Gate |
| SEQ | Ratchet chance | Octave-jump chance | Velocity random | Swing |
| FX: delay | Time (1/16 .. 1/2, dotted and triplet) | Feedback | Mix | Chorus |
| FX again: reverb | Mix | Length (0.5-100 s) | Dark | Shimmer |
| GLO | Sync (int / ext MIDI clock) | Clock out | MIDI channel | Velocity curve |

Every note the arp plays also goes out over USB MIDI; MIDI notes in feed the arp like the keys. The mod wheel adds
vibrato; pitch bend is two semitones.

## Build

    git clone --recursive <this repo>
    ./build.sh --host && tests/run_tests.sh      # host simulator, no JieLi toolchain needed
    ./build.sh --web                             # browser emulator: app/build/emu/ (serve it over http)
    ./build.sh                                   # the device package: app/build/mightyarp.fwsc

On this Mac `build.sh` finds an arm64 FriBiDi in `~/.local/fribidi-arm64` (FRIBIDI_LIB) and Emscripten in `~/emsdk`
(EMSDK).

## Before a release

An own `id_prefix` in `app/fm1.toml` (it still has the template's), the repository and the words `url:` in
`app/words.tsv`, the `# <- yours` lines in `.github/workflows/release.yml` (`pkg=mightyarp`), and a manual
(`app/docs/MANUAL.md` is still the template's).
