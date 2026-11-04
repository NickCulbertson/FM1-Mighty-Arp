# Mighty Arp

Mighty Arp is custom firmware for the M-VAVE FM-1: a 4-op FM and subtractive hybrid synth built around a chance
arpeggiator. You choose the notes, the arp decides when they play. Hold a chord, set CHANCE, and let it run.

- **A chance arpeggiator** with 13 modes, chords in your key and scale (strummed if you like), swing, humanize,
  octave jumps and velocity spread.
- **A 32-step sequencer**: write steps from the keys, transpose them live, ties at GATE 100 %, and the same chance.
- **64 sounds** in eight kinds (keys, bells, plucks, bass, leads, pads, percussion, weird), from a 4-operator FM
  engine with analog-style waves, a filter and its envelope.
- **Effects**: chorus, ping-pong delay, and a reverb with shimmer.
- **16 USER presets**, each keeping its sound, arp, sequence and effects.
- **MIDI** over USB and TRS, in and out, with clock in or out. Over USB it is also a sound card.

**Install:** download `mightyarp-VERSION.fwsc` from [Releases](https://github.com/NickCulbertson/FM1-Mighty-Arp/releases),
connect the FM-1 by USB and run `python3 base/tools/fm1_install.py mightyarp-VERSION.fwsc` from a clone of this
repository, or use the browser installer on [the site](https://nickculbertson.github.io/FM1-Mighty-Arp/) (Chrome or
Edge) once it is published. The installer can put M-VAVE's own firmware back. Installing is at your own risk.

**Manual:** [app/docs/MANUAL.md](app/docs/MANUAL.md), one page. [SPEC.md](SPEC.md) is the full design, and
[SEQ-PLAN.md](SEQ-PLAN.md) the sequencer's.

Built on [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal (a git submodule in `base/`), and
felucca-bsp by Leo Kuroshita, Hügelton Instruments. The app is in `app/` (the sound in `app/dsp/`).

## Controls

| Control | What it does |
| --- | --- |
| Keys | Notes into the arp (or played straight, with the arp off) |
| PLAY | ARP, SEQ (the step sequencer, running on its own), KEYS |
| REC | Hold: NO HOLD (a key let go stops), HOLD (a new chord after you let go replaces the last), LATCH (each key on or off by itself) |
| PRESETS | The sound (64 in eight kinds, then your USER ones); it sets the SOUND, ENV, LFO, EDIT, OP and VOICE pages |
| SELECT / ALGORITHM | Tempo (BPM) / arp chance, on every page |
| HOME, SEL, ENV, LFO, EDIT, ARP, SEQ, FX, GLO | The page K1-K4 edit (below); SEL again: VOICE; FX again: DELAY, REVERB, CHORUS; EDIT again: OP 1-4; SEQ again: GROOVE |
| OCT- / OCT+ | Octave |
| SAVE | Save everything; on the sounds page, a USER preset (everything but GLO's settings) |

| Page | K1 | K2 | K3 | K4 |
| --- | --- | --- | --- | --- |
| HOME (above: the arp's notes; on KEYS, an oscilloscope, as on the other pages) | Arp chance | Rate | Space (reverb mix + length) | Drift (vibrato) |
| HOME again: sounds (SAVE: into the slot) | Kind (keys, bells, plucks, bass, leads, pads, perc, weird, user) | Sound | Save to (U01-U16) | |
| SEL: sound | Tone (FM depth) | Strike (the attack, the tine) | Detune | Velocity |
| SEL again: voice | Voices (poly, mono, legato) | Glide (mono and legato) | | |
| ENV | Attack | Decay | Sustain | Release |
| ENV again: filter (a low-pass a voice; open on the FM sounds) | Cutoff | Resonance | Env amount (how far its envelope opens it) | Key track |
| ENV a third time: filter envelope | Attack | Decay | Sustain | Release |
| LFO | Rate | Shape (sine, tri, square, S&H, drift) | Vibrato | Tremolo (stereo) |
| LFO again: LFO 2 | PWM (the square's and pulses' width) | Filter (the cutoff) | | |
| EDIT | Algorithm (1-8, drawn below: carriers pink, modulators outlined) | Ratio (the modulators') | Feedback (0-7) | Mod wave (every modulator's: W1-W8 the TX81Z's; band-limited W9 saw, W10 square, W11-W12 pulse 25 and 12.5 %, W13 triangle, W14 noise) |
| EDIT again: OP 1-4 | Ratio (x0.5 .. x16) | Level | Wave | Decay (its own envelope; not shown for a carrier the ENV page shapes) |
| ARP again: chords | Chord (off, triad, 7th, lush, open; triad, 7th, omni tones) | Strum (2-250 ms a note) | Key | Scale (major, minor, dorian, mixolydian, harmonic minor) |
| ARP | Rate (1/1 .. 1/32, dotted and triplet) | Octaves | Mode (random, up, down, up-down, as played, converge, diverge, thumb up, thumb up-down, pinky up, pinky up-down, chord, walk) | Gate |
| SEQ | Step (the keys write there; REC a rest) | Note | Velocity | Length (1-32) |
| GROOVE (SEQ again) | Humanize (steps up to 40 ms late) | Octave-jump chance | Velocity random | Swing |
| FX: delay | Mix | Time (1/16 .. 1/2, dotted and triplet) | Feedback | Width |
| FX again: reverb | Mix | Length (0.5-15 s) | Dark | Shimmer |
| FX again: chorus | Mix | Rate | Depth | |
| GLO | Clock (int, int sent out, ext MIDI clock) | MIDI channel | Velocity curve | Presets (keep: a sound change sets only the sound; change: a USER preset brings back its arp, chords, effects, sequence and tempo too, a factory sound them as new) |
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
`FM-1_6279100100`, release 1.0 `FM-1_62791001010000`. It stays the same for every release, so updates install over
each other and keep what players saved.

## Releasing

A `vX.Y` tag pushed to GitHub builds, tests, signs and publishes the release (`.github/workflows/release.yml`, on the
shared [`zvenson/fm1-build`](https://github.com/zvenson/fm1-build) builder). A tag with a suffix (`v1.0-beta1`) is
a pre-release. [RELEASE-1.0.md](RELEASE-1.0.md) is the checklist.

## Licence and credits

Mighty Arp is MPL-2.0 ([LICENSE](LICENSE)), as fm1-base is. Copyright (C) 2026 Charles Vestal, Nick Culbertson.

- [fm1-base](https://github.com/charlesvestal/fm1-base): Charles Vestal. Its own parts and licences:
  `base/LICENSING.md` and `base/CREDITS.md`.
- felucca-bsp and Felucca, which fm1-base grew from: Leo Kuroshita, Hügelton Instruments.
- Fonts: Barlow (The Barlow Project Authors) and Terminus (Dimitar Toshkov Zhekov), SIL OFL 1.1, from fm1-base.
- The chord voicings are Nick's own, from his Macro MIDI Pad (MacroPicoMIDI). Every sound is made by the engine: no
  samples.

M-VAVE and FM-1 are trademarks of their owners. Mighty Arp is not affiliated with M-VAVE.
