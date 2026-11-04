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
| PRESETS | The sound (60 in six kinds, then your USER ones); it sets the SOUND, ENV, LFO, EDIT, OP and VOICE pages |
| SELECT / ALGORITHM | Tempo (BPM) / arp chance, on every page |
| HOME, SEL, ENV, LFO, EDIT, ARP, SEQ, FX, GLO | The page K1-K4 edit (below); SEL again: VOICE; FX again: DELAY, REVERB, CHORUS; EDIT again: OP 1-4 |
| OCT- / OCT+ | Octave |
| SAVE | Save everything; on the sounds page, a USER preset (everything but GLO's settings and Params) |

| Page | K1 | K2 | K3 | K4 |
| --- | --- | --- | --- | --- |
| HOME (above: the arp's notes; with the arp off, an oscilloscope) | Arp chance | Rate | Space (reverb mix + length) | Drift (vibrato) |
| HOME again: sounds (SAVE: into the slot) | Kind (keys, bells, plucks, bass, leads, pads, user) | Sound | Save to (U01-U16) | Params (keep: a sound change sets only the sound; change: a USER preset brings back its arp, chords, effects and tempo too, a factory sound the arp and effects as new) |
| SEL: sound | Tone (FM depth) | Strike (the attack, the tine) | Detune | Velocity |
| SEL again: voice | Voices (poly, mono, legato) | Glide (mono and legato) | | |
| ENV | Attack | Decay | Sustain | Release |
| ENV again: filter (a low-pass a voice; open on the FM sounds) | Cutoff | Resonance | Env amount (how far its envelope opens it) | Key track |
| ENV a third time: filter envelope | Attack | Decay | Sustain | Release |
| LFO | Rate | Shape (sine, tri, square, S&H, drift) | Vibrato | Tremolo (stereo) |
| LFO again: LFO 2 | PWM (the square's and pulses' width) | Filter (the cutoff) | | |
| EDIT | Algorithm (1-8, drawn below: carriers pink, modulators outlined) | Ratio (the modulators') | Feedback (0-7) | Mod wave (every modulator's: W1-W8 the TX81Z's; band-limited W9 saw, W10 square, W11-W12 pulse 25 and 12.5 %, W13 triangle) |
| EDIT again: OP 1-4 | Ratio (x0.5 .. x16) | Level | Wave | Decay (its own envelope; not shown for a carrier the ENV page shapes) |
| ARP again: chords | Chord (off, triad, 7th, lush, open; triad, 7th, omni tones) | Strum (2-250 ms a note) | Key | Scale (major, minor, dorian, mixolydian, harmonic minor) |
| ARP | Rate (1/1 .. 1/32, dotted and triplet) | Octaves | Mode (random, up, down, up-down, as played, converge, diverge, thumb up, thumb up-down, pinky up, pinky up-down, chord, walk) | Gate |
| SEQ | Humanize (steps up to 40 ms late) | Octave-jump chance | Velocity random | Swing |
| FX: delay | Mix | Time (1/16 .. 1/2, dotted and triplet) | Feedback | Width |
| FX again: reverb | Mix | Length (0.5-15 s) | Dark | Shimmer |
| FX again: chorus | Mix | Rate | Depth | |
| GLO | Clock (int, int sent out, ext MIDI clock) | MIDI channel | Velocity curve | |
| GLO again: resets | Params (keep; one detent right: the arp, seq, chords, chance and effects as new) | Factory (keep; turned right sure?, then SAVE: everything as new, the USER presets gone) | | |

Every note the arp plays also goes out over USB MIDI; MIDI notes in feed the arp like the keys. The mod wheel adds
vibrato; pitch bend is two semitones.

## Build

    git clone --recursive <this repo>
    ./build.sh --host && tests/run_tests.sh      # host simulator, no JieLi toolchain needed
    ./build.sh --web                             # browser emulator: app/build/emu/ (serve it over http)
    ./build.sh                                   # the device package: app/build/mightyarp.fwsc

On this Mac `build.sh` finds an arm64 FriBiDi in `~/.local/fribidi-arm64` (FRIBIDI_LIB) and Emscripten in `~/emsdk`
(EMSDK).

## Firmware ID

Mighty Arp's package identity is `FM-1_62791001` (`id_prefix` in `app/fm1.toml`): dev builds report
`FM-1_6279100100`, release 0.1 `FM-1_62791001000100`. It stays the same for every release, so updates install over
each other and keep what players saved.

## Before a release

The repository's address: `repo` in `app/fm1.toml`, the Source link in `app/site/landing.md`, and the words `url:`
in `app/words.tsv`. The rest is in RELEASE-0.1.md.
