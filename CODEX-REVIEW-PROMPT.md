You are reviewing Mighty Arp, a custom firmware for the M-VAVE FM-1 synth (this repository). Please do a code review
focused on (1) bugs and flaws and (2) DSP performance on the real chip. Report findings first; do not rewrite large
parts of the code.

## The hardware and its limits

- JieLi AC791N, pi32v2 core at about 320-360 MHz, single-precision FPU only (no doubles: the build rejects
  soft-double routines). A second core is available and used (fm1.toml cpu2, with a one-core fallback).
- Audio: 44.1 kHz, blocks of 256 stereo frames (5.8 ms each), rendered in an interrupt. RAM: 96 KB (.data+.bss about
  55 KB used) plus a 336 KB pool for big buffers (about 287 KB used).
- **No 64-bit multiplies in the audio code.** On this chip they corrupt a register now and then when they sit beside
  16-bit loads (base/bsp/docs/CPU.md). The build checks for it.
- The DSP unit `app/dsp/engine.c` is compiled at -O2; the rest (`app/app.c`, in a unity build with the platform) at -Os.

## What we measured on the device (before the latest round of optimization)

- About 18% of the render budget with no notes playing (effects only).
- About 57% with 5 notes held in keys mode, so roughly 8% per voice. Ten voices would overload it, and the overload
  handler (eng_shed) was audibly dropping notes.
- Since then (commit "Lighter DSP"): the voice loop is specialized per algorithm with a plain-sine fast path, the
  chorus and reverb modulation moved to block rate, tanh became a cubic, and voices 4-9 run on the second core.
  Not yet measured on the device.

## Where to look

- `app/dsp/engine.c`: the 4-operator FM voices (`voice_loop`, `wave`, `sin_t`, `eng_note_on`), the LFO and drift
  (`eng_set`), the effects (`eng_out`: tremolo, chorus, half-rate ping-pong delay, 4-line FDN reverb with int16 lines
  and a shimmer pitch shifter), the NaN guard, the overload shedding, the second-core split (`eng_voices`, `c2_job`).
- `app/app.c`: the render driver (`app_render`: a per-sample clock loop that renders voices in segments between arp
  events), the arp (`arp_step`, `pattern_place`), MIDI clock in/out, settings, UI.
- `SPEC.md` is the design. `base/` is fm1-base (a submodule, Charles Vestal's): don't propose changes there.

## Please answer

1. **Bugs and flaws**: anything that can produce NaN, clicks, stuck notes, wrong timing, races between the main loop
   and the audio interrupt, or between the two cores (they share caches; one writer per word; `c2_job` touches
   vc[4..9] and its own buffers while core 0 renders vc[0..3]). Give file:line and a concrete failure case.
2. **Performance**: the biggest wins left in `voice_loop` and `eng_out`, ranked by expected savings per sample. Think
   about register pressure (the voice loop has many locals and spills), branches per operator per sample (the
   envelope step), the per-operator wave switch, interpolated table reads, and whether control-rate envelopes
   (updated every 8-16 samples and ramped) would be worth it. Estimate cycles where you can.
3. **Sound quality traps**: aliasing at high FM indexes and high notes, the half-rate delay's imaging, int16 reverb
   lines at long decays, denormals.
4. Small, targeted patches for your top three performance findings, as diffs against `app/dsp/engine.c` only.

## Checking your changes

    ./build.sh --host && sh tests/run_tests.sh   # the host simulator's tests (all must still pass)
    ./build.sh                                   # the device build (Docker); prints its checks

The tests include every sound rendering finite and audible, the reverb at its longest staying bounded, the overload
shedding never dropping below four voices, MIDI clock sync, and the arp modes' note order.
