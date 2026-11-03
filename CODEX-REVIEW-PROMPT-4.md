You've reviewed Mighty Arp (this repository, firmware for the M-VAVE FM-1) five times; the last was at `36a52a0`.
Two things this time: a design question Nick (the developer, a musician) would like your view on, and a review of
what changed since. Report findings with file:line and a concrete failure case; don't rewrite large parts of the
code. The hardware limits are as before (AC791N pi32v2, single-precision FPU, 44.1 kHz in 256-frame blocks, no 64-bit
multiplies in the audio code, `app/dsp/engine.c` at -O2, two cores with the one-core fallback; `base/` is fm1-base,
not to be changed).

## 1. A design question: what should a saved USER patch hold?

Today (`app/app.c`, `SOUND_P`, `user_t`): a factory sound and a USER sound both set only the "sound" pages (SOUND,
VOICE, ENVELOPE, FILTER, FILTER ENVELOPE, LFO, LFO 2, EDIT, OP 1-4). The arp (ARP, SEQ, CHORDS), the effects (DELAY,
REVERB, CHORUS), TEMPO, CHANCE, hold and octave are global: they stay as they are when you change sound. SAVE on
most pages saves the whole current state for power-on; SAVE on the SOUNDS page stores the current sound into a USER
slot (U01-U16).

Nick's other app, Mighty Synth Sampler, saves everything in a preset. The proposal under consideration:

- A USER patch captures everything that makes the piece: the sound pages, plus ARP, SEQ, CHORDS, the effects and
  TEMPO. Not MIDI CH, CLOCK, VEL CURVE or octave (the player's rig, not the patch).
- Loading a USER patch sets all of that. Choosing a factory sound still sets only the sound, so the arp, chords and
  effects carry on: browsing factory tones over the same pattern.
- Existing USER saves are brought forward with their sound settings only.

Nick sees an argument for both ways. Please weigh in, for an instrument built around a chance arpeggiator for
generative playing: is the mixed rule (USER = everything, factory = sound only) clear to a player, or confusing? Is
there a better model (a separate "scene" or "performance" save, a per-load choice, factory sounds carrying their own
arp and effects defaults, something else)? Consider what a player expects when switching sounds mid-piece, how it
reads on a 240 x 240 screen with four knobs and no menus, and what it costs (the store object is at most 3840 bytes;
the bank today is about 1.1 KB). Recommend one, and say how you would make it obvious on the screen.

## 2. Review `git diff 36a52a0 2fb4dd1`

Your five findings at `36a52a0`, and what was done (commit `6cad877`):

1. MAR7 written by three banks (40, 47, 60 sounds) and MAR5 by two (37, 40): a number either bank could mean now goes
   to the candidate whose ENV, LFO, ALGO and FEEDBACK the saved record matches (`sound_match`, `user_match`); MAR5's
   37..39 load as the 40-sound bank's; a build-time check that NPRESET is 60 under the current magic (a new factory
   sound must bump it).
2. The stuck note: with no hold, a MIDI note-off clears its channel from `held_ch` (ownership follows the keys held
   down); All Sound Off also kills the voices of its channel's notes still releasing (`midi_played`).
3. The filter turned on or off under a held note: dry and filtered crossfade over 64 samples (`fwet`), the filter
   stops once faded out, and its envelope runs while bypassed.
4. KEY TRACK follows the voice's gliding pitch (`v->f`); LEGATO keeps the first strike's velocity on purpose.
5. Your two-saw fast path (`steady_saws`), applied by hand; all 60 sounds rendered byte-identical with and without it.

Found by Claude since, in fixing those: a test failure that was a release tail ringing into the next check, not a bug
(the test now waits); the modes test pressing ARP twice (ARP again now opens CHORDS).

New (commit `8a3635e`): CHORDS, on ARP's second press. CHORD: TRIAD, 7TH, LUSH, OPEN make each arp note the root of
the key's diatonic chord on it (`chord_of`, snapped down into the scale); TRIAD, 7TH and OMNI TONES make the held notes'
chords the notes the arp plays from (a pool of up to 128, sorted and deduplicated, in `arp_step`). LUSH, OPEN and OMNI
use Nick's own voicings from his Macro MIDI Pad where it had them for the scale (`LUSH_MAJ`, `OPEN_MAJ`, `OMNI_MAJ`...).
STRUM rolls a chord's notes low to high (`chord_play`, `strummed`, `strum_wait` in the render's event loop). KEY and
SCALE (five). Saves MARA, brought forward from MAR9.

Look especially for: chords and strum against HUMANIZE, SWING, a new step arriving mid-strum, MIDI out (`out_note` has
16 slots; a 5-note chord a step with long gates), the pool's size and its velocities, the CHORD arp mode with a TONES
pool (capped at 16 notes), and whether the migration matching can still pick wrong in a realistic case.

Still open from before (your advice is in hand; not yet built): MONO's fade-then-restart, and migration fixtures from
real old firmware.

## Checking

    ./build.sh --host && sh tests/run_tests.sh   # all test groups must pass
    ./build.sh                                   # the device build (Docker)
