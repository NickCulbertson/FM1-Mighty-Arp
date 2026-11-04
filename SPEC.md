# Mighty Arp: the spec

An FM synth with a chance arpeggiator for generative playing: the player picks the notes, the arp picks when.
This is the plan for the first complete version (was DRIFT). Agreed with Nick on 3 Nov 2026.

## Controls

| Control | Does |
| --- | --- |
| MASTER | volume |
| SELECT | tempo, shown as BPM (as now) |
| ALGO | arp chance, labelled CHANCE |
| PRESET | the sound |
| HOME | the performance screen: the arp's notes large, and four macros (below) |
| SEL | SOUND page; SEL again: VOICE |
| ENV | ENVELOPE page |
| LFO | LFO page |
| EDIT | EDIT page: the FM engine underneath; EDIT again: OP 1, OP 2, OP 3, OP 4 (and round) |
| ARP | ARP page |
| SEQ | SEQ page: the arp's extra chances |
| FX | DELAY page; FX again: REVERB, again CHORUS (and round) |
| GLO | GLOBAL page: MIDI and the keys |
| SAVE | saves everything (one slot); SAVE on SOUNDS: a USER preset (everything but GLOBAL and PARAMS) |
| PLAY | arp on / off (keys) |
| REC | hold: OFF -> HOLD -> LATCH -> OFF |
| OCT- / OCT+ | octave |

## Pages (four knobs each)

| Page | K1 | K2 | K3 | K4 |
| --- | --- | --- | --- | --- |
| HOME | CHANCE | RATE | SPACE (reverb mix and length together) | DRIFT (= LFO vibrato) |
| SOUND | TONE (modulation: dark .. bright) | STRIKE (the attack's bite, the tine) | DETUNE | VELOCITY |
| VOICE (SEL again) | VOICES (POLY, MONO, LEGATO) | GLIDE (not shown with POLY) | | |
| ENVELOPE | ATTACK | DECAY | SUSTAIN | RELEASE |
| FILTER (ENV again) | CUTOFF (30 Hz .. open) | RESONANCE | ENV AMOUNT (up to 8 octaves) | KEY TRACK |
| FILTER ENVELOPE (ENV a third time) | ATTACK | DECAY | SUSTAIN | RELEASE |
| LFO | RATE | SHAPE (SINE, TRI, SQUARE, S&H, DRIFT) | VIBRATO | TREMOLO (stereo, Rhodes style) |
| LFO 2 (LFO again) | PWM | FILTER (up to 3 octaves) | | |
| EDIT | ALGO (1-8, drawn as a diagram) | RATIO (the modulators' x1/2 .. x7) | FEEDBACK (0-7) | MOD WAVE (W1-W8, W9 SAW) |
| OP 1-4 (EDIT again) | RATIO | LEVEL | WAVE | DECAY |
| ARP | RATE (1/1 .. 1/32, dotted and triplet) | OCTAVES | MODE (RANDOM, UP, DOWN, UP-DN, AS PLAYED, CONVERGE, DIVERGE, THUMB UP, THUMB U/D, PINKY UP, PINKY U/D, CHORD, WALK) | GATE |
| CHORDS (ARP again) | CHORD (OFF; TRIAD, 7TH, LUSH, OPEN: a diatonic chord a step; TRIAD, 7TH, OMNI TONES: the chords' tones the notes) | STRUM (2 .. 250 ms a note, the chord within its step) | KEY | SCALE |
| SEQ | HUMANIZE (up to 40 ms late) | OCT JUMP (chance) | VEL RAND | SWING |
| DELAY | MIX | TIME (1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2) | FEEDBACK | WIDTH |
| REVERB | MIX | LENGTH (0.5 s .. 15 s) | DARK | SHIMMER |
| CHORUS (FX again) | MIX | RATE | DEPTH | |
| GLOBAL | CLOCK (INT, OUT, EXT) | MIDI CH (ALL, 1-16) | VEL CURVE (SOFT, LINEAR, HARD, FIXED) | |
| RESETS (GLO again) | PARAMS (KEEP; a detent right, RESET: ARP, SEQ, CHORDS, CHANCE and the effects as new; the sound and TEMPO kept) | FACTORY (KEEP; a detent right SURE?, then SAVE: the settings as new, every USER preset gone, both saved) | | |

PRESET and SOUNDS go through the 64 sounds kind by kind (KEYS, BELLS, PLUCKS, BASS, LEADS, PADS, PERC), like sounds
together (`ORDER` in app.c; the sounds' numbers, which saves hold, never move). A bass loads MONO with no glide, a
lead LEGATO.
A USER preset is named when first saved, an animal and a hairdo ("FROG MULLET"; saved over, it keeps its name).
A USER preset keeps everything: the sound pages, ARP, SEQ, CHORDS, the effects, CHANCE and TEMPO (not GLOBAL's
CLOCK, MIDI CH, VEL CURVE, or PARAMS). PARAMS (SOUNDS's fourth knob) says what a sound change does to the rest:

- KEEP (the default): it sets the sound pages only. ARP, SEQ, CHORDS and the effects stay as they are, so changing sound
  mid-piece keeps its rhythm and its space.
- CHANGE: a USER preset brings back everything it kept; a factory sound starts from the arp and effects as new (TEMPO
  kept). A USER preset from before 3 Nov 2026 (MAU3) kept its sound only, and loads with the arp and effects as new.

## Engine

- 4 operators a voice, 8 algorithms (DX21 / TX81Z style), feedback on operator 4, 8 waveforms (W1 sine .. W8, as the
  TX81Z's), computed from the sine table; W9 a saw from band-limited tables (30 bands, three an octave, 512 points, int16,
  made at boot), on carriers too (SAW PAD, SAW LEAD, SAW BASS).
- Carriers follow the ENVELOPE page; modulators and STRIKE operators have the sound's own envelopes.
- TONE scales the modulators; STRIKE scales the STRIKE operators and the modulators' attack burst.
- Mono with glide, per sound (basses, leads).
- One LFO for vibrato and tremolo; DRIFT gives each voice its own slow wander. The mod wheel (CC1) adds vibrato.
- 10 voices. The DSP in its own unit at -O2 (fm1.toml o2); APP_OVERLOAD sheds voices if the render runs late.
- No 64-bit multiplies in the audio code (felucca-bsp docs/CPU.md: they corrupt registers beside 16-bit loads).

## Arp

- The arp is on a fixed grid, as in Mighty Synth: a new chord starts its pattern on the next step, never restarting
  the clock, so every note is in time.
- A 24 PPQN clock drives every step: internal from BPM, or MIDI clock in (CLOCK EXT; Start resets, Stop pauses).
  CLOCK OUT runs the internal one and sends it.
- HUMANIZE: each step up to 40 ms late, at random. OCT JUMP: a note an octave up or down. VEL RAND: velocity spread.
  SWING: every second step late.
- Hold: OFF (a key let go stops), HOLD (a new chord replaces the last), LATCH (each key on / off by itself).
- CHORDS: LUSH, OPEN and OMNI use Nick's voicings from his Macro MIDI Pad (MacroPicoMIDI) where it had them.
- MIDI in on MIDI CH; the arp's notes out on it (ALL: channel 1).

## Effects

Voices -> tremolo -> chorus -> ping-pong delay -> reverb (with shimmer) -> out.

- Chorus: stereo, two taps moving in opposite directions.
- Delay: ping-pong, int16 at half the rate, up to 1.49 s; a time longer than that at a slow tempo is halved (still in
  time).
- Reverb: the same 4-line feedback network, lines about 3x longer, stored 16-bit, slowly modulated; LENGTH as a
  decay time up to 15 s. SHIMMER: an octave-up pitch shifter in the reverb's feedback.

## Screen

- Synthwave colours, CMYK: yellow for what is happening this moment (the note just played, the knob being turned);
  a black ground, cyan labels, hot pink values, knobs and new notes, electric purple for
  the sound, the tempo and notes as they age, faint violet ghosts for the steps the chance skipped.
- Knobs as smooth arcs round a dark knob cap with a pointer; on HOME, the arcs alone. The name: MIGHTY in the labels' cyan, ARP in pink.
- The boot screen: "Mighty" brushed in pink, tilted across a chrome "ARP" with a curved horizon, speed stripes behind (tools/make_splash.py, from Barlow).
- HOME: the arp's notes over the last seconds, large; the held notes; the macros. With the arp off, an
  oscilloscope of the output in their place.
- The state line: PLAY ARP/KEYS, REC hold mode, BPM, CHANCE.

## Sounds (about 32)

Mostly E.Piano and tines, then keys, bells and mallets, plucks, basses, synths and pads. Listed in app.c.

## Testing

The host simulator's scripts (tests/run_tests.sh): every sound sounds and stays finite; hold modes; ratchets; MIDI
clock in; the reverb at its longest with shimmer stays bounded; save and reboot. Then the browser emulator and a
device build (size, RAM, pool). Installing on the FM-1 waits for Nick.

## Not now

User preset slots (one save, as now), reverb freeze, Bluetooth MIDI, samples.
