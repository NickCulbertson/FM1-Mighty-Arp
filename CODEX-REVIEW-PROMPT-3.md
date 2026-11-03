You've reviewed Mighty Arp (this repository, firmware for the M-VAVE FM-1) four times; the last was at `945266c`. Claude
acted on that review in `36a52a0`. Three things, please. Report findings with file:line and a concrete failure case;
don't rewrite large parts of the code. The hardware limits are as before (AC791N pi32v2, single-precision FPU, 44.1 kHz
in 256-frame blocks, no 64-bit multiplies in the audio code, `app/dsp/engine.c` at -O2, two cores with the one-core
fallback; `base/` is fm1-base, not to be changed).

## 1. Review `git diff 945266c 36a52a0`

What was done for your five findings:

1. A MAR7 save's preset 47 and up is a USER slot: remapped to `preset - 47 + NPRESET` (that bank had 47 sounds).
2. The filter follows its knobs on held notes: `filter_follow()` recomputes the base cutoff (KEY TRACK on the voice's
   note now) and the envelope amount each block, `eng_set` turns the filter on under a held note (state reset) and
   off again, and the coefficients still ramp across the block.
3. `preset_load()` calls `voice_load()` (and so `filter_load()` / `fenv_fill()`) last, after the ENV page.
4. With no filter attack a note starts with its filter envelope at 1 and the coefficients set at once.
5. `midi_let_go()` clears the addressed channels from `held_ch[i]`, and removes the note only when no channel and no
   key holds it.

Also: a bypassed voice skips `filter_coefs()` at note-on; PWM skips its square roots at rest.

Look especially for: what turning the filter on and off under a held note does (clicks, the state reset, a
coefficient ramp starting from stale values), whether `fvel` / `fbase` stay right through legato and mono glides,
and whether any migration path (MAR3..MAR8, MAU1..MAU2) still lands on a different sound or knob value than the old
firmware had.

## 2. Performance, now measured on the FM-1

Measured on the device at `36a52a0` (GLOBAL's CPU readout, the render's share of its budget, both cores running):

| | 5 notes held | 10 notes held |
| --- | --- | --- |
| nothing playing | 17 % | |
| TINE EP (FM, filter off) | 23 % | 48 % |
| POLY SYNTH (two saws, FM, filter on) | 43 % | 68 % |

The overload handler sheds a voice above 90 %. The worst realistic case is ten VA notes with long releases, the arp
running and the reverb's shimmer up. Rank the remaining wins for the filtered VA path by expected saving, with your
estimate of cycles per voice-sample where you can: control-rate envelopes (your earlier suggestion), the table-wave
lookups (two saws per square or pulse), the filter's two channels (could a voice be filtered in mono before its
pan?), register pressure in `voice_loop` (the last disassembly showed a ~400-byte frame), and the effects at 17 %
idle. Say which you would do first, and give it as a small diff against `app/dsp/engine.c`.

## 3. Two open questions (advice, not code)

- MONO: an occupied slot keeps its envelope level, so a long-attack note played over a sounding one starts near the
  old level instead of swelling from silence. Resetting to zero would click. Is a short fade-then-restart worth it,
  and how would you do it in this voice structure?
- The settings and user-bank migrations have no fixtures from real old firmware. Is it worth building them (old
  firmware's flash images in `tests/`), and how would you structure that in the host simulator (its `data` and
  `reboot` commands)?

## Checking

    ./build.sh --host && sh tests/run_tests.sh   # all test groups must pass
    ./build.sh                                   # the device build (Docker)
