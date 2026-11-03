/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Nick Culbertson */
/* Mighty Arp's sound: 4-operator FM voices, an LFO, a chorus, a ping-pong delay and a reverb with shimmer. A unit of
 * its own, compiled at -O2 (fm1.toml o2); app.c drives it from the render, all of it in the audio interrupt.
 *
 * No 64-bit multiplies in here: beside 16-bit loads they corrupt a register now and then (felucca-bsp docs/CPU.md). */
#pragma once
#include <stdint.h>

#define ENG_SR 44100.0f
#define ENG_NVOICE 10
#define ENG_BLOCK 256u
#define ENG_DLY_MAX 65528.0f             /* the delay's longest, in samples (1.49 s) */

/* an operator of a sound. ratio: its frequency, times the note's; det: cents; lvl: a carrier's amplitude, or a
 * modulator's depth (1 = a whole turn of phase); wave 0..7 (W1..W8). A modulator, or a carrier marked strike, runs
 * its own envelope (att, dec seconds; sus of 1); the other carriers follow the ENVELOPE page */
typedef struct {
    float ratio, det, lvl;
    uint8_t wave, strike;
    float att, dec, sus;
} eng_op_t;

typedef struct {
    eng_op_t op[4];                      /* op[3] is operator 4: the one with feedback */
    uint8_t mono;                        /* one voice, legato */
    float glide;                         /* mono: seconds to a new note */
} eng_sound_t;

enum { LFO_SINE, LFO_TRI, LFO_SQUARE, LFO_SH, LFO_DRIFT, LFO_NSHAPE };

/* the knobs, as the engine takes them: set once a block (eng_set) */
typedef struct {
    float tone;                          /* the modulators' depth, x0.25 .. x4 */
    float strike;                        /* 0 .. 2: the strike operators' level and the modulators' attack burst */
    float detune;                        /* 0 .. 1 */
    float velo;                          /* 0 .. 1: how much velocity changes level and brightness */
    float att, dec, sus, rel;            /* the carriers' envelope: seconds, and a level */
    uint8_t alg, fb;                     /* 0..7, 0..7 */
    uint8_t wave;                        /* the modulators' wave, 0..7; 0xFF: each its own (the sound's) */
    float ratio;                         /* the modulators' ratios times this */
    float lfo_hz;
    uint8_t lfo_shape;
    float vib;                           /* semitones */
    float trem;                          /* 0 .. 1 */
    float bend;                          /* semitones */
    float chorus;                        /* 0 .. 1 */
    float dly_samples, dly_fb, dly_mix;
    float rev_mix, rev_secs, rev_dark, shimmer;
} eng_params_t;

void eng_init(void);
void eng_set(const eng_sound_t *s, const eng_params_t *p, uint32_t n);   /* the sound and the knobs, at the start of
                                                                           * a block of n */
void eng_note_on(int32_t note, uint32_t vel, int32_t gate);   /* gate: samples before it releases; -1, a key holds */
void eng_note_off(int32_t note);         /* a key let go: the voices it holds release */
void eng_release_all(void);
void eng_voices(float *l, float *r, uint32_t a, uint32_t b);  /* the voices, added into l, r [a, b) */
/* the effects over a block of voices, then out: lr 24-bit stereo at gain. 1 = a number went bad (and everything
 * that feeds back was cleared) */
int eng_out(float *l, float *r, int32_t *lr, uint32_t n, float gain);
uint32_t eng_active(void);               /* voices sounding */
int32_t eng_sounding(int32_t note);      /* a voice plays it (not releasing) */
void eng_shed(void);                     /* the render ran late: a voice out, and one fewer for new notes */
uint32_t eng_cap(int up);                /* the voices new notes may take; up: one more back */
void eng_reset(void);                    /* silence, and every buffer clean */
void eng_silence(void);                  /* the voices off at once (the buffers as they are) */
void eng_poison(void);                   /* (the host's test) a NaN into the reverb */
