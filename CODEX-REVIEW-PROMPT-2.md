You reviewed Mighty Arp (this repository, firmware for the M-VAVE FM-1) at commit `78619df`. Claude has since acted on
that review in commit `d920e41`. Two things, please: check that work, and weigh in on a design question. Report
findings; don't rewrite large parts of the code.

The hardware limits from last time still hold: AC791N (pi32v2, ~320-360 MHz, single-precision FPU, no doubles), 44.1 kHz
in 256-frame blocks in an interrupt, 96 KB RAM plus a 336 KB pool, **no 64-bit multiplies in the audio code**
(base/bsp/docs/CPU.md), `app/dsp/engine.c` at -O2, the second core on with the one-core fallback. `base/` is fm1-base:
don't propose changes there.

## 1. Review the fixes: `git diff 78619df d920e41`

For each of your earlier findings, say whether the change fixes it, fixes it partly, or introduces something new.
What was done:

1. MIDI CH: `midi_ch_now` latches the channel in the render; on a change, MIDI-held notes are let go and what went out
   on the old channel gets its note-offs there (`modes_follow`).
2. Second-core failure: on a failed `c2_wait`, the segment's worker voices are dropped, not run again (`eng_voices`).
3. NaN: checked before the int16 narrowing in `reverb`; `eng_reset` now clears the buffers 16 KB a block
   (`clearing`, output silent meanwhile).
4. Preset change: `preset_load` inside `plat_lock` / `plat_unlock` (interrupts off for its dozen stores).
5. Clock out: `arp_restart` leaves `tick_acc` / `tick_n` alone while CLOCK is OUT.
6. MIDI gate: `out_end` is an absolute sample (`sample_clock + rend_i + gate`).
7. Ownership: `down[]` holds DN_KEY and DN_MIDI bits; each source lets go only its own; KEV_ALLOFF only the keys'.
8. Shedding: the second scan runs only when none is releasing, and takes the oldest.

And from your performance and sound sections: free voices go to the core with fewer sounding; `W()` takes `sin_t`
for a W1 operator inside mixed-wave sounds; whole-sample taps for reverb lines 1 and 3 and for the shimmer (which now
always runs); operators above 10 kHz are scaled down and above 20 kHz silenced at note-on; a one-pole low-pass
(0.63) before the half-rate delay; reverb lines round instead of truncating; `fm_flush` on the allpasses, the
reverb low-passes and the DC blocker. Not done: control-rate envelopes, MIDI clock-in timing inside the block.

Look especially for: anything the fixes broke (the tests in `app/tests/fixes.txt` cover 1, 7 and 8 only), races the
new `midi_ch_now` / `rend_i` / `clearing` state adds between the main loop, the audio interrupt and the second core,
and whether `plat_lock` around `preset_load` is safe and enough.

## 2. A design question: what should come next for the sound?

Nick (the developer, a musician) isn't sure, and would like your view. Today the 8 waves (W1-W8) are TX81Z-style
sine variants that apply only to the modulators, which is why the WAVE knob sounds subtle. The EDIT page draws the
wave; ALGO is just a number. Options:

- **A. An algorithm diagram** on EDIT: the four operators, who modulates whom, the carriers, feedback.
- **B. Real SAW, SQUARE, TRIANGLE waves** (W9-W11) that may also go on the carriers, band-limited (a table set per
  octave, in the pool), making Mighty Arp an FM/hybrid synth rather than pure FM.
- **C. Both**, or **D. neither** (keep it pure 4-op FM; the saw-like sound already comes from feedback).

For B, estimate the memory (tables per octave, size, int16 or float), the per-sample cost against the current
`sin_t`, and how it fits the per-algorithm `voice_loop` specialization. Say what you'd recommend for a generative,
"chance arp" instrument whose sounds are mostly E.Piano, bells and pads, and why.

## Checking

    ./build.sh --host && sh tests/run_tests.sh   # all 11 test groups must pass
    ./build.sh                                   # the device build (Docker)

Patches, if any, as diffs against `app/` only.
