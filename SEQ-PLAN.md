# Mighty Arp: the step sequencer (plan)

Nick's brief: 16 or 32 steps, a simple grid on the screen, connected notes held when GATE is 100%, and the chance
still at work. Nick's picks: a held key transposes the sequence; the steps live in USER presets and follow PARAMS;
the grid is on the SEQ button.

## What it is

- **PLAY: ARP -> SEQ -> KEYS** (changed after Nick's first try: SEQ was a MODE, and it ran only while a key was
  held). On SEQ the arp plays the sequence's steps instead of walking the held notes. Everything
  else the arp has applies as it is: RATE, CHANCE (a step rests at random), GATE, SWING, HUMANIZE, OCT JUMP,
  VEL RAND, CHORDS (each step's note a chord; the TONES kinds don't apply: there's no pool) and STRUM. OCTAVES
  doesn't apply.
- **When it plays:** on its own, from step 1, as soon as PLAY reaches SEQ; it stops when PLAY moves on to KEYS.
- **Transpose:** the last-pressed held note (keys or MIDI), against the sequence's first note, from the next step; it
  stays when the key is let go. A key never restarts it. Holding the first note plays it as written.
- **Steps:** up to 32. Each step is a note (with its velocity) or a rest. LENGTH is 1–32, default 16.
- **Ties (GATE 100%):** a step whose next step has the same note joins it into one long note (no retrigger, one
  chance roll for the whole run). A different note follows with a small overlap, so LEGATO glides into it. Below
  100%, every step is struck.

## The pages

- **SEQ** (SEQ button): K1 STEP (the cursor), K2 NOTE (REST, C-1 … G9), K3 VELOCITY, K4 LENGTH. Below, the grid:
  each step a column, pitch upward, the cursor yellow, the step playing pink, ties drawn joined.
- **Writing steps:** on the SEQ page the keys write instead of playing. A key writes its note and velocity at the
  cursor, sounds it briefly, and moves the cursor on (round to step 1 after LENGTH). REC writes a rest and moves on.
  MIDI notes still transpose, so a keyboard can play it while you write.
- **GROOVE** (SEQ again): HUMANIZE, OCT JUMP, VEL RAND, SWING (the old SEQ page, renamed).

## Saves

- Steps and LENGTH are in the settings and in each USER preset. Formats bumped: settings MARF, presets MAU6
  (no saves exist from before 0.1). The bank is 16 × 200 bytes, under the 3840-byte object limit.
- **PARAMS CHANGE:** a USER preset brings back its steps. **KEEP:** the steps stay.
- **RESET (PARAMS) and factory sounds:** the steps are left alone. They're what you wrote, not a setting.
- **FACTORY:** the default sequence again.

## Tests (host)

A `seq` test group: write steps from the keys, rests from REC, the cursor wrapping, LENGTH, playback order,
transpose, a tie at 100% (one note, not two), no tie below 100%, chance at 0% (nothing plays), saved in a USER
preset and loaded back with CHANGE, kept with KEEP, kept through RESET. The other groups' SEQ-page steps move to
GROOVE.

## For the review (GPT)

Review the commit that adds this plan: `git diff HEAD~1` (app/app.c is most of it). Worth a hard look:

- `seq_take()` and the tie logic in `arp_step()`: a run's length, the overlap, a tie at the sequence's end, LENGTH
  changed while playing, the transpose from the last-pressed held note.
- The panic paths (`midi_let_go`, All Notes/Sound Off) with SEQ's notes: each note's root is the held note that
  transposes it.
- The SEQ page's keys: `input()` writes instead of pushing KEV_ON. A key held across a page change, the octave
  changing, LATCH and HOLD.
- `KEV_TAP`, the written step heard once, from the render (not held, no MIDI out).
- Saves: settings `MARF` and presets `MAU6`; the bank at 3204 of the 3840 bytes; old records load as new (no saves
  before 0.1); USER presets' steps cleaned on load; PARAMS CHANGE and KEEP; RESET and FACTORY.
- Things left out on purpose: OCTAVES and the CHORDS TONES kinds in SEQ, live recording, per-step probability.
