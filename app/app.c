/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal, Nick Culbertson */
/* Mighty Arp: an FM synth with a chance arpeggiator, a chorus, a ping-pong delay and a reverb with shimmer, for
 * generative playing: the player picks the notes, the arp picks when. From fm1-base's template. SPEC.md is the plan.
 *
 * The sound is dsp/engine.c, a unit of its own at -O2. This file is the rest: the settings, the sounds, the arp and
 * its clock, MIDI, and the screen. The audio interrupt owns all the note state: the keys reach it through a queue
 * (ev_push), MIDI in is read there, and the clock, the arp and the engine all run there. The main loop owns the
 * settings and the screen; the render reads the settings as they are (single values).
 *
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in
 * scope. */
#include "fastmath.h"
#include "dsp/engine.h"

#define SR ENG_SR
#define NHELD 16

/* ------------------------------------------------------------- parameters --- */
enum { P_TONE, P_STRIKE, P_DETUNE, P_VELO,            /* SOUND */
       P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE,       /* ENVELOPE */
       P_LRATE, P_LSHAPE, P_VIB, P_TREM,              /* LFO */
       P_ALGO, P_RATIO, P_FDBK, P_WAVE,               /* EDIT */
       P_RATE, P_OCTAVES, P_MODE, P_GATE,             /* ARP */
       P_RATCHET, P_OJUMP, P_VRAND, P_SWING,          /* SEQ */
       P_DTIME, P_DFDBK, P_DMIX, P_CHORUS,            /* DELAY */
       P_RMIX, P_RLEN, P_RDARK, P_SHIMMER,            /* REVERB */
       P_SYNC, P_CLKOUT, P_MIDICH, P_VCURVE,          /* GLOBAL */
       P_TEMPO, P_CHANCE,                             /* SELECT and ALGO */
       NP,
       PX_SPACE = NP };                               /* HOME's macro: the reverb's mix and length together */
enum { U_PCT, U_TIME, U_DIV, U_OCT, U_MODE, U_DDIV, U_BPM, U_HZ, U_SHAPE, U_NUM1, U_RATIO, U_NUM, U_WAVE, U_SECS,
       U_SYNC, U_ONOFF, U_CH, U_CURVE };
/* the arp's modes */
enum { M_UP, M_DOWN, M_UPDN, M_PLAYED, M_CONVERGE, M_DIVERGE, M_THUMB, M_PINKY, M_CHORD, M_RANDOM, M_WALK, NMODE };
typedef struct {
    const char *name, *id;               /* id: the host tests' name for it */
    int16_t lo, hi, def;
    uint8_t unit;
} param_t;
static const param_t PAR[NP] = {
    {"TONE", "tone", 0, 100, 50, U_PCT},         {"STRIKE", "strike", 0, 100, 50, U_PCT},
    {"DETUNE", "detune", 0, 100, 25, U_PCT},     {"VELOCITY", "velocity", 0, 100, 60, U_PCT},
    {"ATTACK", "attack", 0, 100, 0, U_TIME},     {"DECAY", "decay", 0, 100, 70, U_TIME},
    {"SUSTAIN", "sustain", 0, 100, 30, U_PCT},   {"RELEASE", "release", 0, 100, 82, U_TIME},
    {"RATE", "lfo_rate", 0, 100, 40, U_HZ},      {"SHAPE", "lfo_shape", 0, LFO_NSHAPE - 1, 0, U_SHAPE},
    {"VIBRATO", "vibrato", 0, 100, 0, U_PCT},    {"TREMOLO", "tremolo", 0, 100, 0, U_PCT},
    {"ALGO", "algo", 0, 7, 4, U_NUM1},           {"RATIO", "ratio", 0, 7, 1, U_RATIO},
    {"FEEDBACK", "fm_fdbk", 0, 7, 0, U_NUM},     {"WAVE", "wave", 0, 7, 0, U_WAVE},
    {"RATE", "rate", 0, 10, 5, U_DIV},            {"OCTAVES", "octaves", 1, 4, 2, U_OCT},
    {"MODE", "mode", 0, NMODE - 1, M_RANDOM, U_MODE},           {"GATE", "gate", 5, 100, 50, U_PCT},
    {"RATCHET", "ratchet", 0, 100, 0, U_PCT},    {"OCT JUMP", "oct_jump", 0, 100, 0, U_PCT},
    {"VEL RAND", "vel_rand", 0, 100, 20, U_PCT}, {"SWING", "swing", 0, 100, 0, U_PCT},
    {"TIME", "dly_time", 0, 8, 4, U_DDIV},       {"FEEDBACK", "dly_fdbk", 0, 95, 45, U_PCT},
    {"MIX", "dly_mix", 0, 100, 30, U_PCT},       {"CHORUS", "chorus", 0, 100, 25, U_PCT},
    {"MIX", "rev_mix", 0, 100, 40, U_PCT},       {"LENGTH", "rev_len", 0, 100, 55, U_SECS},
    {"DARK", "rev_dark", 0, 100, 40, U_PCT},     {"SHIMMER", "shimmer", 0, 100, 0, U_PCT},
    {"SYNC", "sync", 0, 1, 0, U_SYNC},           {"CLOCK OUT", "clock_out", 0, 1, 0, U_ONOFF},
    {"MIDI CH", "midi_ch", 0, 16, 0, U_CH},      {"VEL CURVE", "vel_curve", 0, 3, 1, U_CURVE},
    {"TEMPO", "tempo", 40, 200, 90, U_BPM},      {"CHANCE", "chance", 0, 100, 33, U_PCT},
};

/* the pages: each button's four knobs */
enum { PG_HOME, PG_SOUND, PG_ENV, PG_LFO, PG_EDIT, PG_ARP, PG_SEQ, PG_DELAY, PG_REVERB, PG_GLOBAL, NPAGE };
typedef struct {
    const char *name;
    uint8_t btn, p[4];
} page_t;
static const page_t PAGE[NPAGE] = {
    {"HOME", B_HOME, {P_CHANCE, P_RATE, PX_SPACE, P_VIB}},
    {"SOUND", B_SEL, {P_TONE, P_STRIKE, P_DETUNE, P_VELO}},
    {"ENVELOPE", B_ENV, {P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE}},
    {"LFO", B_LFO, {P_LRATE, P_LSHAPE, P_VIB, P_TREM}},
    {"EDIT", B_EDIT, {P_ALGO, P_RATIO, P_FDBK, P_WAVE}},
    {"ARP", B_ARP, {P_RATE, P_OCTAVES, P_MODE, P_GATE}},
    {"SEQ", B_SEQ, {P_RATCHET, P_OJUMP, P_VRAND, P_SWING}},
    {"DELAY", B_FX, {P_DTIME, P_DFDBK, P_DMIX, P_CHORUS}},
    {"REVERB", B_FX, {P_RMIX, P_RLEN, P_RDARK, P_SHIMMER}},
    {"GLOBAL", B_GLO, {P_SYNC, P_CLKOUT, P_MIDICH, P_VCURVE}},
};

/* the arp's steps (in MIDI clocks: 24 a beat) and the delay's times (in beats) */
static const char *const DIV_NAME[11] = {"1/1", "1/2", "1/4D", "1/4", "1/8D", "1/8", "1/16D", "1/8T", "1/16", "1/16T",
                                         "1/32"};
static const uint8_t DIV_TICKS[11] = {96, 48, 36, 24, 18, 12, 9, 8, 6, 4, 3};
#define CLK_WRAP 576u                    /* six bars of clocks: every step, and every pair of steps, divides it */
static const char *const DDIV_NAME[9] = {"1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2"};
static const float DDIV_BEATS[9] = {0.25f, 0.375f, 1.0f / 3.0f, 0.5f, 0.75f, 2.0f / 3.0f, 1.0f, 1.5f, 2.0f};
static const char *const MODE_NAME[NMODE] = {"UP", "DOWN", "UP-DN", "AS PLAYED", "CONVERGE", "DIVERGE", "THUMB",
                                             "PINKY", "CHORD", "RANDOM", "WALK"};
static const char *const SHAPE_NAME[LFO_NSHAPE] = {"SINE", "TRI", "SQUARE", "S&H", "DRIFT"};
static const char *const RATIO_NAME[8] = {"X1/2", "X1", "X2", "X3", "X4", "X5", "X6", "X7"};
static const float RATIO_MUL[8] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
static const char *const CURVE_NAME[4] = {"SOFT", "LINEAR", "HARD", "FIXED"};
enum { H_OFF, H_HOLD, H_LATCH };
static const char *const HOLD_NAME[3] = {"NO HOLD", "HOLD", "LATCH"};

/* ----------------------------------------------------------------- sounds --- */
/* an operator: ratio, cents, level (a carrier's amplitude, a modulator's depth in turns), wave (0 = W1), strike, and
 * its own envelope (attack and decay in seconds, sustain): a modulator's, or a strike carrier's. The other carriers
 * follow the ENVELOPE page, which the sound sets (a, d, s, r: the knobs). alg 0..7 is ALGO 1..8 */
#define OP(r, c, l, w, st, a, d, s) {(r), (c), (l), (w), (st), (a), (d), (s)}
#define CARRIER(r, c, l) OP(r, c, l, 0, 0, 0.0f, 0.0f, 0.0f)
typedef struct {
    const char *name;
    uint8_t alg, fb;
    eng_sound_t s;
    int8_t a, d, su, r;                  /* the ENVELOPE page */
    int8_t lrate, lshape, vib, trem;     /* the LFO page */
} preset_t;
static const preset_t PRESET[] = {
    /* electric pianos: two stacks (ALGO 5), the body and the tine, which STRIKE rides */
    {"TINE EP", 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.25f, 0, 0, 0, 1.2f, 0.15f),
                        OP(1, 3, 0.35f, 0, 1, 0, 2.0f, 0.08f), OP(14, 0, 0.21f, 0, 0, 0, 0.07f, 0)}, 0, 0},
     0, 74, 25, 82, 45, LFO_SINE, 0, 0},
    {"BRIGHT EP", 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.36f, 0, 0, 0, 0.9f, 0.2f),
                          OP(1, 3, 0.5f, 0, 1, 0, 1.5f, 0.08f), OP(14, 0, 0.3f, 0, 0, 0, 0.12f, 0)}, 0, 0},
     0, 76, 22, 80, 45, LFO_SINE, 0, 0},
    {"SOFT TINE", 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.15f, 0, 0, 0, 1.6f, 0.15f),
                          OP(1, 2, 0.22f, 0, 1, 0, 2.5f, 0.1f), OP(14, 0, 0.12f, 0, 0, 0, 0.06f, 0)}, 0, 0},
     1, 80, 25, 86, 45, LFO_SINE, 0, 0},
    {"SUITCASE", 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.28f, 0, 0, 0, 1.0f, 0.18f),
                         OP(1, 4, 0.3f, 0, 1, 0, 1.8f, 0.1f), OP(14, 0, 0.16f, 0, 0, 0, 0.05f, 0)}, 0, 0},
     0, 76, 25, 80, 70, LFO_SINE, 0, 55},
    {"DYNO EP", 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.32f, 0, 0, 0, 0.7f, 0.15f),
                        OP(1, 3, 0.55f, 0, 1, 0, 1.2f, 0.05f), OP(14, 0, 0.36f, 0, 0, 0, 0.1f, 0.02f)}, 0, 0},
     0, 74, 20, 80, 72, LFO_TRI, 0, 40},
    {"BELL EP", 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.2f, 0, 0, 0, 1.4f, 0.15f),
                        OP(2, 2, 0.4f, 0, 1, 0, 2.6f, 0.02f), OP(7, 0, 0.18f, 0, 0, 0, 0.5f, 0.05f)}, 0, 0},
     0, 80, 20, 86, 45, LFO_SINE, 0, 0},
    {"WURLY", 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.32f, 1, 0, 0, 0.6f, 0.3f),
                      OP(1, 0, 0.3f, 0, 1, 0, 1.0f, 0.2f), OP(3, 0, 0.12f, 0, 0, 0, 0.25f, 0.05f)}, 0, 0},
     0, 72, 35, 74, 74, LFO_TRI, 0, 35},
    {"LOFI EP", 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.2f, 0, 0, 0, 1.2f, 0.15f),
                        OP(1, 5, 0.3f, 0, 1, 0, 2.0f, 0.08f), OP(14, 0, 0.12f, 0, 0, 0, 0.07f, 0)}, 0, 0},
     2, 78, 25, 84, 30, LFO_DRIFT, 22, 0},
    {"DREAM EP", 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.18f, 0, 0, 0, 2.5f, 0.2f),
                         OP(2, 5, 0.25f, 0, 1, 0, 3.5f, 0.15f), OP(14, 0, 0.08f, 0, 0, 0, 0.1f, 0)}, 0, 0},
     8, 86, 35, 92, 25, LFO_DRIFT, 14, 15},
    {"GLASS EP", 4, 0, {{CARRIER(1, 0, 1.0f), OP(3, 0, 0.12f, 0, 0, 0, 1.5f, 0.2f),
                         OP(1, 0, 0.45f, 0, 1, 0, 2.0f, 0.05f), OP(7, 0, 0.2f, 0, 0, 0, 0.4f, 0.05f)}, 0, 0},
     0, 78, 22, 84, 45, LFO_SINE, 0, 0},
    {"FM PIANO", 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.3f, 0, 0, 0, 0.8f, 0.1f),
                         OP(2, 3, 0.3f, 0, 1, 0, 1.0f, 0.05f), OP(5, 0, 0.08f, 0, 0, 0, 0.2f, 0)}, 0, 0},
     0, 78, 10, 70, 45, LFO_SINE, 0, 0},
    /* keys */
    {"CLAV", 4, 3, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.55f, 1, 0, 0, 0.25f, 0.25f),
                     OP(3, 0, 0.3f, 0, 1, 0, 0.3f, 0.1f), OP(1, 0, 0.4f, 0, 0, 0, 0.15f, 0.1f)}, 0, 0},
     0, 60, 30, 42, 45, LFO_SINE, 0, 0},
    {"FM ORGAN", 7, 2, {{CARRIER(0.5f, 0, 0.35f), CARRIER(1, 0, 0.45f), CARRIER(2, 0, 0.3f), CARRIER(4, 0, 0.18f)},
                        0, 0},
     1, 50, 100, 40, 82, LFO_SINE, 30, 15},
    {"VIBES", 4, 0, {{CARRIER(1, 0, 1.0f), OP(4, 0, 0.12f, 0, 0, 0, 0.4f, 0),
                      OP(4, 0, 0.14f, 0, 1, 0, 0.5f, 0), OP(1, 0, 0.05f, 0, 0, 0, 0.3f, 0)}, 0, 0},
     0, 84, 0, 82, 76, LFO_SINE, 0, 45},
    /* bells and mallets */
    {"GLASS BELL", 4, 0, {{CARRIER(1, 0, 1.0f), OP(3.5f, 0, 0.38f, 0, 0, 0, 2.5f, 0.2f),
                           OP(1, 0, 0.5f, 0, 1, 0, 3.0f, 0), OP(1.41f, 0, 0.16f, 0, 0, 0, 1.5f, 0.1f)}, 0, 0},
     0, 86, 0, 88, 45, LFO_SINE, 0, 0},
    {"TUBULAR", 4, 0, {{CARRIER(1, 0, 1.0f), OP(3.5f, 0, 0.3f, 0, 0, 0, 3.0f, 0.2f),
                        OP(2.76f, 0, 0.35f, 0, 1, 0, 4.0f, 0), OP(5.4f, 0, 0.2f, 0, 0, 0, 1.5f, 0.05f)}, 0, 0},
     0, 90, 0, 90, 45, LFO_SINE, 0, 0},
    {"MARIMBA", 4, 0, {{CARRIER(1, 0, 1.0f), OP(4, 0, 0.32f, 0, 0, 0, 0.03f, 0),
                        OP(1, 0, 0.6f, 0, 1, 0, 0.5f, 0), OP(1, 0, 0.08f, 0, 0, 0, 0.12f, 0)}, 0, 0},
     0, 58, 0, 66, 45, LFO_SINE, 0, 0},
    {"KALIMBA", 4, 0, {{CARRIER(1, 0, 1.0f), OP(5, 0, 0.29f, 0, 0, 0, 0.05f, 0),
                        OP(1, 0, 0.5f, 0, 1, 0, 0.6f, 0), OP(1, 0, 0.065f, 0, 0, 0, 0.3f, 0)}, 0, 0},
     0, 62, 0, 70, 45, LFO_SINE, 0, 0},
    {"MUSIC BOX", 4, 0, {{CARRIER(1, 0, 1.0f), OP(7, 0, 0.24f, 0, 0, 0, 0.15f, 0.05f),
                          OP(3, 0, 0.3f, 0, 1, 0, 0.8f, 0), OP(1, 0, 0.03f, 0, 0, 0, 0.4f, 0)}, 0, 0},
     0, 68, 0, 82, 45, LFO_SINE, 0, 0},
    {"CELESTA", 4, 0, {{CARRIER(1, 0, 1.0f), OP(6, 0, 0.15f, 0, 0, 0, 0.2f, 0),
                        OP(4, 0, 0.22f, 0, 1, 0, 0.35f, 0), OP(1, 0, 0.05f, 0, 0, 0, 0.3f, 0)}, 0, 0},
     0, 72, 0, 80, 45, LFO_SINE, 0, 0},
    {"CRYSTAL", 4, 0, {{CARRIER(1, 0, 1.0f), OP(7, 0, 0.2f, 0, 0, 0, 0.08f, 0.02f),
                        OP(3, 0, 0.3f, 0, 1, 0, 1.2f, 0), OP(9, 0, 0.15f, 0, 0, 0, 0.3f, 0)}, 0, 0},
     0, 76, 0, 90, 45, LFO_SINE, 0, 0},
    /* plucks */
    {"HARP", 4, 0, {{CARRIER(1, 0, 1.0f), OP(2, 0, 0.24f, 0, 0, 0, 0.15f, 0.1f),
                     OP(1, 0, 0.5f, 0, 1, 0, 1.2f, 0), OP(1, 0, 0.04f, 0, 0, 0, 0.5f, 0)}, 0, 0},
     0, 64, 0, 80, 45, LFO_SINE, 0, 0},
    {"KOTO", 4, 0, {{CARRIER(1, 0, 1.0f), OP(3, 0, 0.45f, 0, 0, 0, 0.12f, 0.05f),
                     OP(1, 0, 0.35f, 0, 1, 0, 0.5f, 0), OP(5, 0, 0.2f, 0, 0, 0, 0.05f, 0)}, 0, 0},
     0, 66, 0, 72, 45, LFO_SINE, 0, 0},
    {"NYLON", 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.25f, 0, 0, 0, 0.3f, 0.05f),
                      OP(3, 0, 0.15f, 0, 1, 0, 0.15f, 0), OP(1, 0, 0.1f, 0, 0, 0, 0.1f, 0)}, 0, 0},
     0, 70, 0, 70, 45, LFO_SINE, 0, 0},
    {"PLUCK", 0, 6, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.4f, 0, 0, 0, 0.25f, 0.05f),
                      OP(1, 0, 0.3f, 0, 0, 0, 0.2f, 0), OP(1, 0, 0.3f, 0, 0, 0, 0.2f, 0)}, 0, 0},
     0, 60, 0, 70, 45, LFO_SINE, 0, 0},
    /* basses: an octave down (ratio 1/2) */
    {"FM BASS", 2, 5, {{CARRIER(0.5f, 0, 1.0f), OP(0.5f, 0, 0.3f, 0, 0, 0, 0.3f, 0.2f),
                        OP(1.5f, 0, 0.15f, 0, 0, 0, 0.2f, 0), OP(0.5f, 0, 0.25f, 0, 0, 0, 0.15f, 0.05f)}, 1, 0.03f},
     0, 62, 35, 30, 45, LFO_SINE, 0, 0},
    {"SLAP BASS", 4, 0, {{CARRIER(0.5f, 0, 1.0f), OP(0.5f, 0, 0.5f, 0, 0, 0, 0.12f, 0.15f),
                          OP(1.5f, 0, 0.3f, 0, 1, 0, 0.1f, 0), OP(3.5f, 0, 0.3f, 0, 0, 0, 0.05f, 0)}, 1, 0},
     0, 60, 30, 30, 45, LFO_SINE, 0, 0},
    {"SUB BASS", 4, 0, {{CARRIER(0.5f, 0, 1.0f), OP(0.5f, 0, 0.05f, 0, 0, 0, 0.2f, 0.02f),
                         CARRIER(1, 0, 0), OP(1, 0, 0, 0, 0, 0, 0.1f, 0)}, 1, 0.06f},
     1, 70, 60, 35, 45, LFO_SINE, 0, 0},
    {"WOOD BASS", 4, 0, {{CARRIER(0.5f, 0, 1.0f), OP(0.5f, 0, 0.2f, 0, 0, 0, 0.15f, 0.05f),
                          OP(1, 0, 0.2f, 0, 1, 0, 0.08f, 0), OP(2, 0, 0.15f, 0, 0, 0, 0.04f, 0)}, 0, 0},
     0, 70, 0, 50, 45, LFO_SINE, 0, 0},
    /* synths */
    {"FM BRASS", 0, 4, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.3f, 0, 0, 0.06f, 0.4f, 0.6f),
                         OP(1, 0, 0.12f, 0, 0, 0.05f, 0.5f, 0.5f), OP(1, 0, 0.08f, 0, 0, 0, 0.4f, 0.5f)}, 0, 0},
     12, 70, 80, 45, 76, LFO_SINE, 25, 0},
    {"SYNC LEAD", 0, 6, {{CARRIER(1, 0, 1.0f), OP(2, 0, 0.35f, 0, 0, 0, 0.6f, 0.6f),
                          OP(1, 0, 0.2f, 0, 0, 0, 0.5f, 0.5f), OP(1, 0, 0.2f, 0, 0, 0, 0.5f, 0.5f)}, 1, 0.05f},
     0, 60, 70, 40, 76, LFO_SINE, 30, 0},
    {"FM SAW", 4, 5, {{CARRIER(1, -8, 0.7f), OP(1, 0, 0.32f, 0, 0, 0, 1.0f, 0.8f),
                       CARRIER(1, 8, 0.7f), OP(1, 0, 0.3f, 0, 0, 0, 1.0f, 0.8f)}, 0, 0},
     0, 60, 85, 50, 45, LFO_SINE, 0, 0},
    /* pads */
    {"GLASS PAD", 4, 0, {{CARRIER(1, 0, 0.7f), OP(1, 0, 0.19f, 0, 0, 0, 4.0f, 0.6f),
                          CARRIER(1, 5, 0.55f), OP(2, 0, 0.13f, 0, 0, 0, 3.0f, 0.5f)}, 0, 0},
     72, 82, 80, 88, 30, LFO_DRIFT, 22, 0},
    {"STRINGS", 4, 3, {{CARRIER(1, -6, 0.7f), OP(1, 0, 0.18f, 0, 0, 0.3f, 1.0f, 0.9f),
                        CARRIER(1, 6, 0.7f), OP(1, 0, 0.15f, 0, 0, 0.3f, 1.0f, 0.9f)}, 0, 0},
     70, 80, 90, 85, 76, LFO_SINE, 25, 0},
    {"CHOIR", 5, 0, {{CARRIER(1, 0, 0.8f), CARRIER(3, 0, 0.25f), CARRIER(5, 0, 0.1f),
                      OP(1, 0, 0.1f, 0, 0, 0.4f, 2.0f, 0.9f)}, 0, 0},
     75, 80, 90, 86, 30, LFO_DRIFT, 25, 0},
    {"DRIFT PAD", 4, 0, {{CARRIER(1, 0, 1.0f), OP(0.5f, 0, 0.1f, 0, 0, 1.0f, 4.0f, 1.0f),
                          CARRIER(2, 5, 0.5f), OP(3, 0, 0.08f, 0, 0, 0, 3.0f, 0.7f)}, 0, 0},
     80, 85, 85, 92, 25, LFO_DRIFT, 40, 25},
    {"HALO", 4, 0, {{CARRIER(1, 0, 1.0f), OP(4, 0, 0.1f, 0, 0, 0.5f, 3.0f, 0.8f),
                     CARRIER(2, 0, 0.5f), OP(7, 0, 0.05f, 0, 0, 1.0f, 3.0f, 0.5f)}, 0, 0},
     78, 86, 70, 94, 20, LFO_DRIFT, 20, 20},
};
#define NPRESET (sizeof PRESET / sizeof PRESET[0])

/* the wave EDIT shows for a sound: its first modulator's (the knob, turned, gives every modulator one) */
static uint32_t preset_wave(const preset_t *pr)
{
    static const uint8_t car[8] = {0x1, 0x1, 0x1, 0x1, 0x5, 0x7, 0x7, 0xF};
    uint32_t k;
    for (k = 0; k < 4u; k++)
        if (!(car[pr->alg & 7u] >> k & 1u))
            return pr->s.op[k].wave & 7u;
    return 0;
}

/* --------------------------------------------------------------- settings --- */
#define SET_MAGIC 0x3352414Du            /* "MAR3": the settings record; another magic loads defaults */
typedef struct {
    uint32_t magic;
    int16_t p[NP];
    uint8_t preset;
    int8_t octave;                       /* -2 .. +2 */
    uint8_t arp_on, hold;                /* hold: H_OFF, H_HOLD, H_LATCH */
} settings_t;
static settings_t set;
static uint8_t set_dirty;                /* changed since the last save: SAVE is lit */

static uint32_t drift_rng = 0x9E3779B9u;
static inline uint32_t rnd(void)         /* xorshift32 */
{
    drift_rng ^= drift_rng << 13;
    drift_rng ^= drift_rng >> 17;
    drift_rng ^= drift_rng << 5;
    return drift_rng;
}
/* 0 .. n-1, n up to 65536: 32-bit only (a 64-bit multiply beside 16-bit loads corrupts a register: bsp docs/CPU.md) */
static inline uint32_t rnd_n(uint32_t n) { return ((rnd() >> 16) * n) >> 16; }

/* a 0..100 time knob: 1 ms .. 10 s, exponential */
static float knob_secs(int32_t v) { return 0.001f * fm_exp2f((float)v * 0.1328771f); }   /* 10000 ^ (v / 100) */
static float lfo_hz(int32_t v) { return 0.05f * fm_exp2f((float)v * 0.0864386f); }        /* 0.05 .. 20 Hz */
static float rev_secs(int32_t v) { return 0.5f * fm_exp2f((float)v * 0.0490689f); }      /* 0.5 .. 15 s */

/* ---------------------------------------------------------- notes and arp --- */
/* the keys' events, main loop -> render (one writer, one reader) */
enum { KEV_ON, KEV_OFF, KEV_ALLOFF };
#define EVQ_N 64u
static volatile uint32_t evq[EVQ_N];
static volatile uint32_t evq_w, evq_r;
static void ev_push(uint32_t type, uint32_t note, uint32_t vel)
{
    uint32_t w = evq_w;
    if (w - evq_r >= EVQ_N)
        return;
    evq[w % EVQ_N] = type | note << 8 | vel << 16;
    evq_w = w + 1u;
}

/* the notes the arp plays from (or the keys hold, with the arp off): sorted, with their velocities */
static int32_t held[NHELD];
static uint8_t held_vel[NHELD];
static uint32_t held_seq[NHELD], held_seq_n;   /* when each was added: AS PLAYED's order */
static uint32_t nheld;
static uint8_t down[128];                /* physically down (keys and MIDI in): HOLD and LATCH keep the rest */
static uint32_t ndown;
static uint8_t arp_on_now, hold_now, clk_out_now, sync_now;   /* what the render last acted on */

/* published to the screen: the held set, and the steps the arp has taken (played or skipped) */
static volatile uint32_t held_mask[4];
#define VIZ_N 64u
static volatile uint32_t viz[VIZ_N];     /* note | vel << 8 (0: the chance skipped it) | the time in 1/100 s << 16 */
static volatile uint32_t viz_w, arp_count;
static volatile uint32_t sample_clock;   /* samples rendered: the falling notes' clock */

static uint32_t out_ch(void) { return set.p[P_MIDICH] ? (uint32_t)set.p[P_MIDICH] - 1u : 0u; }
static void midi_note(uint32_t note, uint32_t vel)
{
    plat_midi_send(0x09u | (0x90u | out_ch()) << 8 | note << 16 | vel << 24, TO_USB);
}
static void midi_off(uint32_t note) { plat_midi_send(0x08u | (0x80u | out_ch()) << 8 | note << 16, TO_USB); }
static void midi_rt(uint32_t st) { plat_midi_send(0x0Fu | st << 8, TO_USB); }   /* clock, start, stop */

/* the arp's MIDI out: each note it sent and the samples left before its note-off */
static int16_t out_note[NHELD];
static int32_t out_left[NHELD];
static void out_all_off(void)
{
    uint32_t i;
    for (i = 0; i < NHELD; i++)
        if (out_note[i] >= 0) {
            midi_off((uint32_t)out_note[i]);
            out_note[i] = -1;
        }
}

static void held_publish(void)
{
    uint32_t m[4] = {0, 0, 0, 0}, i;
    for (i = 0; i < nheld; i++)
        m[held[i] >> 5] |= 1u << (held[i] & 31);
    for (i = 0; i < 4; i++)
        held_mask[i] = m[i];
}

static const preset_t *cur_preset(void) { return &PRESET[set.preset < NPRESET ? set.preset : 0]; }

static void held_add(int32_t note, uint32_t vel)
{
    uint32_t i, j;
    for (i = 0; i < nheld; i++)
        if (held[i] == note) {
            held_vel[i] = (uint8_t)vel;
            return;
        }
    if (nheld >= NHELD)
        return;
    for (i = 0; i < nheld && held[i] < note; i++)
        ;
    for (j = nheld; j > i; j--) {
        held[j] = held[j - 1];
        held_vel[j] = held_vel[j - 1];
        held_seq[j] = held_seq[j - 1];
    }
    held[i] = note;
    held_vel[i] = (uint8_t)vel;
    held_seq[i] = ++held_seq_n;
    nheld++;
    if (!arp_on_now) {
        eng_note_on(note, vel, -1);
        midi_note((uint32_t)note, vel);
    }
}

static void held_remove(int32_t note)
{
    uint32_t i;
    for (i = 0; i < nheld && held[i] != note; i++)
        ;
    if (i == nheld)
        return;
    for (; i + 1 < nheld; i++) {
        held[i] = held[i + 1];
        held_vel[i] = held_vel[i + 1];
        held_seq[i] = held_seq[i + 1];
    }
    nheld--;
    if (!arp_on_now) {
        if (cur_preset()->s.mono && nheld && eng_sounding(note))   /* mono: back to the top note still held */
            eng_note_on(held[nheld - 1], held_vel[nheld - 1], -1);
        eng_note_off(note);
        midi_off((uint32_t)note);
    }
}

static void held_clear(void)
{
    while (nheld)
        held_remove(held[nheld - 1]);
}

/* ---- the clock: 24 a beat, from the tempo or from MIDI clock in (SYNC EXT); every step lands on it */
static float tick_acc;                   /* the internal clock: fractions of a tick */
static uint32_t tick_n = CLK_WRAP - 1u;  /* the clock's tick, 0 .. CLK_WRAP - 1 */
static int32_t swing_wait, rat_wait;     /* samples to a swung step, and to the next ratchet hit */
static int32_t rat_left, rat_gate, rat_sub;
static int32_t arp_s = -1, arp_k;        /* steps since the pattern began; WALK's place */
static int32_t st_note[NHELD];           /* the step's notes (one, or CHORD's), for its ratchet hits */
static uint8_t st_vel[NHELD];
static uint32_t st_n;
static volatile int32_t last_played = -1;
static uint8_t ext_run = 1;              /* EXT: Stop holds the clock, Start and Continue run it */
static uint32_t ext_last, ext_sc0, ext_cnt;   /* the last tick's sample clock; the period's measure */
static float ext_per = 44100.0f * 60.0f / (120.0f * 24.0f);   /* samples a tick, measured */
static volatile uint32_t ext_bpm_x10;    /* to the screen: the tempo coming in, 0 = none */

static float tick_samples(void)          /* how long a tick is now */
{
    return sync_now ? ext_per : SR * 60.0f / ((float)set.p[P_TEMPO] * 24.0f);
}
static float step_samples(void) { return tick_samples() * (float)DIV_TICKS[set.p[P_RATE]]; }

static void play(int32_t note, uint32_t vel, int32_t gate)   /* one of the arp's notes: the engine, MIDI, the screen */
{
    uint32_t i;
    eng_note_on(note, vel, gate);
    for (i = 0; i < NHELD; i++)          /* its MIDI: the same note sounding is ended first */
        if (out_note[i] == note) {
            midi_off((uint32_t)note);
            out_note[i] = -1;
        }
    for (i = 0; i < NHELD && out_note[i] >= 0; i++)
        ;
    if (i < NHELD) {
        out_note[i] = (int16_t)note;
        out_left[i] = gate;
        midi_note((uint32_t)note, vel);
    }
    viz[viz_w % VIZ_N] = (uint32_t)note | vel << 8 | (sample_clock / 441u) << 16;
    viz_w = viz_w + 1u;
    arp_count = arp_count + 1u;
    last_played = note;
}

static uint32_t played_index(uint32_t j, uint32_t n)   /* the held note pressed j-th (of those still held) */
{
    uint32_t i, m, c;
    for (i = 0; i < n; i++) {
        for (m = 0, c = 0; m < n; m++)
            c += held_seq[m] < held_seq[i];
        if (c == j)
            return i;
    }
    return 0;
}

/* the place in the run (the held notes low to high, then an octave up, ...) that step s of the pattern plays */
static uint32_t pattern_place(uint32_t s, uint32_t L)
{
    uint32_t t, P;
    switch (set.p[P_MODE]) {
    case M_DOWN:
        return L - 1u - s % L;
    case M_UPDN:                         /* no repeats at the ends */
        if (L < 2u)
            return 0;
        P = 2u * L - 2u;
        t = s % P;
        return t < L ? t : P - t;
    case M_CONVERGE:                     /* the outside in: low, high, next low, next high */
        t = s % L;
        return t & 1u ? L - 1u - t / 2u : t / 2u;
    case M_DIVERGE:                      /* the inside out */
        t = L - 1u - s % L;
        return t & 1u ? L - 1u - t / 2u : t / 2u;
    case M_THUMB:                        /* the lowest between each of the others */
        if (L < 2u)
            return 0;
        t = s % (2u * L - 2u);
        return t & 1u ? 1u + t / 2u : 0;
    case M_PINKY:                        /* the highest between each of the others */
        if (L < 2u)
            return 0;
        t = s % (2u * L - 2u);
        return t & 1u ? t / 2u : L - 1u;
    case M_RANDOM:
        return rnd_n(L);
    case M_WALK:                         /* a step up or down from the last, at random */
        if (L < 2u || s == 0 || arp_k >= (int32_t)L)
            arp_k = 0;
        else {
            arp_k += rnd() & 1u ? 1 : -1;
            if (arp_k < 0)
                arp_k = 1;
            else if (arp_k >= (int32_t)L)
                arp_k = (int32_t)L - 2;
        }
        return (uint32_t)arp_k;
    default:                             /* UP, AS PLAYED */
        return s % L;
    }
}

static void arp_step(void)
{
    uint32_t n = nheld, oct = (uint32_t)set.p[P_OCTAVES], s, i, k, idx;
    int32_t gate, vr, j;
    float step = step_samples();
    if (!n)
        return;
    s = (uint32_t)++arp_s;               /* the pattern moves every step, played or not */
    if (set.p[P_MODE] == M_CHORD) {      /* CHORD: every held note at once, an octave higher each step */
        for (i = 0; i < n; i++) {
            st_note[i] = held[i] + 12 * (int32_t)(s % oct);
            st_vel[i] = held_vel[i];
        }
        st_n = n;
    } else {
        k = pattern_place(s, n * oct);
        idx = set.p[P_MODE] == M_PLAYED ? played_index(k % n, n) : k % n;
        st_note[0] = held[idx] + 12 * (int32_t)(k / n);
        st_vel[0] = held_vel[idx];
        st_n = 1;
    }
    for (i = 0; i < st_n; i++)
        while (st_note[i] > 127)
            st_note[i] -= 12;
    rat_wait = 0;
    if (rnd_n(100) >= (uint32_t)set.p[P_CHANCE]) {   /* the chance: this step rests (ghosts on the screen) */
        for (i = 0; i < st_n; i++) {
            viz[viz_w % VIZ_N] = (uint32_t)st_note[i] | (sample_clock / 441u) << 16;
            viz_w = viz_w + 1u;
        }
        return;
    }
    if (rnd_n(100) < (uint32_t)set.p[P_OJUMP]) {     /* OCT JUMP: an octave away, up or down */
        j = rnd() & 1u ? 12 : -12;
        for (i = 0; i < st_n; i++)
            if (st_note[i] + j < 0 || st_note[i] + j > 127)
                j = -j;
        for (i = 0; i < st_n; i++)
            if (st_note[i] + j >= 0 && st_note[i] + j <= 127)
                st_note[i] += j;
    }
    vr = set.p[P_VRAND] * 6 / 10;                    /* VEL RAND: up to +-60 */
    for (i = 0; i < st_n; i++)
        st_vel[i] = (uint8_t)clamp((int32_t)st_vel[i] + (vr ? (int32_t)rnd_n((uint32_t)(2 * vr + 1)) - vr : 0), 1, 127);
    gate = (int32_t)(step * (float)set.p[P_GATE] / 100.0f);
    if (rnd_n(100) < (uint32_t)set.p[P_RATCHET]) {   /* RATCHET: the step in 2 or 3 quick hits */
        int32_t hits = 2 + (int32_t)rnd_n(2);
        rat_sub = (int32_t)(step / (float)hits);
        gate = gate / hits;
        rat_left = hits - 1;
        rat_wait = rat_sub;
    }
    rat_gate = gate < 64 ? 64 : gate;
    for (i = 0; i < st_n; i++)
        play(st_note[i], st_vel[i], rat_gate);
}

static void ratchet_hit(void)
{
    uint32_t i;
    for (i = 0; i < st_n; i++) {
        st_vel[i] = (uint8_t)(st_vel[i] * 4u / 5u < 1u ? 1u : st_vel[i] * 4u / 5u);
        play(st_note[i], st_vel[i], rat_gate);
    }
    if (--rat_left > 0)
        rat_wait = rat_sub;
}

static void on_tick(void)                /* a clock tick: MIDI out, and a step where one falls */
{
    uint32_t L = DIV_TICKS[set.p[P_RATE]];
    tick_n = (tick_n + 1u) % CLK_WRAP;
    if (clk_out_now && !sync_now && plat_midi_out_room(TO_USB))
        midi_rt(0xF8u);
    if (!arp_on_now || !nheld || tick_n % L)
        return;
    if ((tick_n / L) & 1u && set.p[P_SWING]) {   /* SWING: every second step late, up to half a step */
        int32_t w = (int32_t)(step_samples() * (float)set.p[P_SWING] * 0.005f);
        swing_wait = w < 1 ? 1 : w;
    } else
        arp_step();
}

static void arp_restart(void)            /* the first note of a new chord: the clock starts with it */
{
    tick_n = 0;
    tick_acc = 0.0f;
    swing_wait = rat_wait = 0;
    arp_s = -1;                          /* the pattern from its top */
    arp_step();
}

static uint32_t vel_curve(uint32_t v)
{
    switch (set.p[P_VCURVE]) {
    case 0:                              /* SOFT: a light touch goes further */
        return (uint32_t)clamp((int32_t)(fm_sqrtf((float)v / 127.0f) * 127.0f + 0.5f), 1, 127);
    case 2:                              /* HARD */
        return (uint32_t)clamp((int32_t)(v * v / 127u), 1, 127);
    case 3:                              /* FIXED */
        return 100u;
    default:
        return v;
    }
}

static void note_down(int32_t note, uint32_t vel)
{
    uint32_t i, was;
    if (note < 0 || note > 127)
        return;
    vel = vel_curve(vel);
    if (hold_now == H_HOLD && ndown == 0)   /* HOLD: a new chord after every key was let go replaces the last */
        held_clear();
    was = nheld;
    if (!down[note]) {
        down[note] = 1;
        ndown++;
    }
    if (hold_now == H_LATCH) {           /* LATCH: each key on or off by itself */
        for (i = 0; i < nheld && held[i] != note; i++)
            ;
        if (i < nheld) {
            held_remove(note);
            held_publish();
            return;
        }
    }
    held_add(note, vel);
    held_publish();
    if (was == 0 && nheld && arp_on_now && !sync_now)   /* the first note plays at once */
        arp_restart();
}

static void note_up(int32_t note)
{
    if (note < 0 || note > 127 || !down[note])
        return;
    down[note] = 0;
    ndown--;
    if (hold_now == H_OFF)
        held_remove(note);
    held_publish();
}

/* the arp, HOLD, SYNC or CLOCK OUT switched: what sounds follows */
static void modes_follow(void)
{
    uint32_t i;
    if (set.arp_on != arp_on_now) {
        arp_on_now = set.arp_on;
        eng_release_all();               /* every note let go into its release */
        out_all_off();
        swing_wait = rat_wait = 0;
        if (!arp_on_now)                 /* the arp stopped: the notes held sound again */
            for (i = 0; i < nheld; i++) {
                eng_note_on(held[i], held_vel[i], -1);
                midi_note((uint32_t)held[i], held_vel[i]);
            }
        else {
            for (i = 0; i < nheld; i++)
                midi_off((uint32_t)held[i]);
            if (nheld && !sync_now)
                arp_restart();
        }
    }
    if (set.hold != hold_now) {
        hold_now = set.hold;
        if (hold_now == H_OFF) {         /* hold off: what no key holds goes */
            for (i = nheld; i-- > 0;)
                if (!down[held[i]])
                    held_remove(held[i]);
            held_publish();
        }
    }
    if ((uint8_t)set.p[P_SYNC] != sync_now) {
        sync_now = (uint8_t)set.p[P_SYNC];
        ext_run = 1;
        swing_wait = rat_wait = 0;
    }
    if ((uint8_t)(set.p[P_CLKOUT] && !sync_now) != clk_out_now) {
        clk_out_now = (uint8_t)(set.p[P_CLKOUT] && !sync_now);
        midi_rt(clk_out_now ? 0xFAu : 0xFCu);   /* Start, Stop */
    }
}

static float modwheel, bend;             /* CC1 (0..1), the pitch bend (semitones) */
static volatile uint32_t midi_count;
static void ext_tick(void)               /* a MIDI clock in */
{
    if (!sync_now)
        return;
    if (ext_cnt && sample_clock != ext_sc0) {   /* the period: over the ticks since the last block that had one */
        float per = (float)(sample_clock - ext_sc0) / (float)ext_cnt;
        if (per > 20.0f && per < 4000.0f)
            ext_per += (per - ext_per) * (sample_clock - ext_last < (uint32_t)SR ? 0.1f : 1.0f);
        ext_sc0 = sample_clock;
        ext_cnt = 0;
    } else if (!ext_cnt)
        ext_sc0 = sample_clock;
    ext_cnt++;
    ext_last = sample_clock;
    if (ext_run)
        on_tick();
}

static void midi_in(void)                /* audio interrupt: the packets that came in */
{
    uint32_t pkt, ch_want = set.p[P_MIDICH] ? (uint32_t)set.p[P_MIDICH] - 1u : 0xFFu;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24 & 0x7Fu, ty = st & 0xF0u;
        if (st == 0xF8u) {
            ext_tick();
            continue;
        }
        if (st == 0xFAu || st == 0xFBu || st == 0xFCu) {   /* Start (from the top), Continue, Stop */
            if (sync_now) {
                ext_run = st != 0xFCu;
                if (st == 0xFAu) {
                    tick_n = CLK_WRAP - 1u;   /* the next clock is the first step */
                    arp_s = -1;
                }
                swing_wait = rat_wait = 0;
            }
            continue;
        }
        if (st < 0x80u || st >= 0xF0u || (ch_want != 0xFFu && (st & 0x0Fu) != ch_want))
            continue;
        if (ty == 0x90u && d2) {
            note_down((int32_t)d1, d2);
            midi_count++;
        } else if (ty == 0x80u || ty == 0x90u) {
            note_up((int32_t)d1);
        } else if (ty == 0xB0u && d1 == 1u) {
            modwheel = (float)d2 / 127.0f;
        } else if (ty == 0xE0u) {
            bend = (float)((int32_t)(d2 << 7 | d1) - 8192) * (2.0f / 8192.0f);
        } else if (ty == 0xB0u && (d1 == 123u || d1 == 120u)) {   /* all notes off, all sound off */
            ndown = 0;
            for (d1 = 0; d1 < 128u; d1++)
                down[d1] = 0;
            held_clear();
            held_publish();
        }
    }
    if (sync_now && sample_clock - ext_last < (uint32_t)SR && ext_per > 0.0f)
        ext_bpm_x10 = (uint32_t)(600.0f * SR / (24.0f * ext_per) + 0.5f);
    else
        ext_bpm_x10 = 0;
}

static void events_in(void)
{
    while (evq_r != evq_w) {
        uint32_t e = evq[evq_r % EVQ_N], note = e >> 8 & 0xFFu;
        evq_r = evq_r + 1u;
        switch (e & 0xFFu) {
        case KEV_ON:
            note_down((int32_t)note, e >> 16 & 0xFFu);
            break;
        case KEV_OFF:
            note_up((int32_t)note);
            break;
        case KEV_ALLOFF:                  /* the octave changed: the keys' notes would not find their offs */
            for (note = 0; note < 128u; note++)
                if (down[note])
                    note_up((int32_t)note);
            break;
        }
    }
}

/* ----------------------------------------------------------------- render --- */
static float buf_l[ENG_BLOCK], buf_r[ENG_BLOCK];
static volatile uint32_t dsp_resets;     /* how often the guard had to clear (GLOBAL shows it; the tests read it) */
static volatile uint32_t sheds;          /* how often the render ran late and a voice went (GLOBAL shows it) */
static uint32_t cap_back_at;             /* the sample clock at which a voice shed comes back */

static void engine_params(uint32_t n)    /* the knobs, as the engine takes them */
{
    const preset_t *pr = cur_preset();
    eng_params_t ep;
    float beat = tick_samples() * 24.0f, v = (float)set.p[P_VIB] / 100.0f;
    ep.tone = fm_exp2f(((float)set.p[P_TONE] - 50.0f) / 25.0f);   /* x0.25 .. x4 */
    ep.strike = (float)set.p[P_STRIKE] / 50.0f;
    ep.detune = (float)set.p[P_DETUNE] / 100.0f;
    ep.velo = (float)set.p[P_VELO] / 100.0f;
    ep.att = knob_secs(set.p[P_ATTACK]);
    ep.dec = knob_secs(set.p[P_DECAY]);
    ep.sus = (float)set.p[P_SUSTAIN] / 100.0f;
    ep.rel = knob_secs(set.p[P_RELEASE]);
    ep.alg = (uint8_t)set.p[P_ALGO];
    ep.fb = (uint8_t)set.p[P_FDBK];
    ep.wave = (uint32_t)set.p[P_WAVE] == preset_wave(pr) ? 0xFFu : (uint8_t)set.p[P_WAVE];
    ep.ratio = RATIO_MUL[set.p[P_RATIO]];
    ep.lfo_hz = lfo_hz(set.p[P_LRATE]);
    ep.lfo_shape = (uint8_t)set.p[P_LSHAPE];
    ep.vib = v * v + modwheel * 0.5f;    /* up to a semitone; the mod wheel adds half */
    ep.trem = (float)set.p[P_TREM] / 100.0f;
    ep.bend = bend;
    ep.chorus = (float)set.p[P_CHORUS] / 100.0f;
    ep.dly_samples = beat * DDIV_BEATS[set.p[P_DTIME]];
    while (ep.dly_samples > ENG_DLY_MAX)    /* longer than the line at a slow tempo: half, still in time */
        ep.dly_samples *= 0.5f;
    ep.dly_fb = (float)set.p[P_DFDBK] / 100.0f;
    ep.dly_mix = (float)set.p[P_DMIX] / 100.0f;
    ep.rev_mix = (float)set.p[P_RMIX] / 100.0f * 0.6f;
    ep.rev_secs = rev_secs(set.p[P_RLEN]);
    ep.rev_dark = (float)set.p[P_RDARK] / 100.0f;
    ep.shimmer = (float)set.p[P_SHIMMER] / 100.0f;
    eng_set(&pr->s, &ep, n);
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i, k, seg = 0;
    float gain = (float)master_q12 / 4096.0f * 8300000.0f, tps;   /* MASTER, read once a block */
    if (n > ENG_BLOCK)
        n = ENG_BLOCK;
    engine_params(n);
    modes_follow();
    midi_in();
    events_in();
    tps = 1.0f / tick_samples();
    for (i = 0; i < n; i++)
        buf_l[i] = buf_r[i] = 0.0f;
    for (i = 0; i < n; i++) {            /* the clock, a sample at a time; the voices rendered up to each event */
        uint32_t ev = 0;
        if (!sync_now && (tick_acc += tps) >= 1.0f) {
            tick_acc -= 1.0f;
            ev |= 1u;
        }
        if (swing_wait > 0 && --swing_wait == 0)
            ev |= 2u;
        if (rat_wait > 0 && --rat_wait == 0)
            ev |= 4u;
        if (!ev)
            continue;
        eng_voices(buf_l, buf_r, seg, i);
        seg = i;
        if (ev & 1u)
            on_tick();
        if (ev & 2u && arp_on_now)
            arp_step();
        if (ev & 4u && arp_on_now && nheld)
            ratchet_hit();
    }
    eng_voices(buf_l, buf_r, seg, n);
    if (eng_out(buf_l, buf_r, lr, n, gain))   /* a bad number: silence, then everything starts clean */
        dsp_resets = dsp_resets + 1u;
    if (eng_cap(0) < ENG_NVOICE && sample_clock - cap_back_at < 0x80000000u) {   /* 2 s without running late: a
                                                                                  * voice back */
        eng_cap(1);
        cap_back_at = sample_clock + (uint32_t)(2.0f * SR);
    }
    for (k = 0; k < NHELD; k++)          /* the arp's note-offs, to the block */
        if (out_note[k] >= 0 && (out_left[k] -= (int32_t)n) <= 0) {
            midi_off((uint32_t)out_note[k]);
            out_note[k] = -1;
        }
    sample_clock += n;
}

void app_panic(void)
{
    eng_silence();
    nheld = 0;
}

void app_overload(void)                  /* the render ran late (APP_OVERLOAD): a voice out, back after 2 s */
{
    eng_shed();
    sheds = sheds + 1u;
    cap_back_at = sample_clock + (uint32_t)(2.0f * SR);
}


/* --------------------------------------------------------------- settings --- */
static void preset_load(uint32_t p)      /* a sound: its envelope, LFO and EDIT pages, and SOUND's two at the middle */
{
    const preset_t *pr = &PRESET[p];
    set.preset = (uint8_t)p;
    set.p[P_ATTACK] = pr->a;
    set.p[P_DECAY] = pr->d;
    set.p[P_SUSTAIN] = pr->su;
    set.p[P_RELEASE] = pr->r;
    set.p[P_LRATE] = pr->lrate;
    set.p[P_LSHAPE] = pr->lshape;
    set.p[P_VIB] = pr->vib;
    set.p[P_TREM] = pr->trem;
    set.p[P_ALGO] = pr->alg;
    set.p[P_FDBK] = pr->fb;
    set.p[P_WAVE] = (int16_t)preset_wave(pr);
    set.p[P_RATIO] = 1;
    set.p[P_TONE] = 50;
    set.p[P_STRIKE] = 50;
}

static void settings_defaults(void)
{
    uint32_t i;
    set.magic = SET_MAGIC;
    for (i = 0; i < NP; i++)
        set.p[i] = PAR[i].def;
    preset_load(0);
    set.octave = 0;
    set.arp_on = 1;
    set.hold = H_OFF;                    /* a key let go stops (REC: HOLD, LATCH) */
}

static int settings_ok(void)
{
    uint32_t i;
    if (set.magic != SET_MAGIC || set.preset >= NPRESET || set.octave < -2 || set.octave > 2 || set.arp_on > 1u
        || set.hold > H_LATCH)
        return 0;
    for (i = 0; i < NP; i++)
        if (set.p[i] < PAR[i].lo || set.p[i] > PAR[i].hi)
            return 0;
    return 1;
}

void app_init(void)
{
    uint32_t i;
    eng_init();
    for (i = 0; i < NHELD; i++)
        out_note[i] = -1;
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || !settings_ok())
        settings_defaults();
#ifndef PLAT_HOST
    drift_rng ^= plat_ticks24() | 1u;    /* a different piece every power-on (the host's tests repeat) */
#endif
}

/* --------------------------------------------------------------------- UI --- */
/* the colours: synthwave. A black ground (the FM-1's screen reads best on it), cyan labels, hot pink values and new
 * notes, electric purple for the rest */
#define C_BG C_BLACK
#define C_GHOST RGB(84, 62, 136)
static void theme(void)
{
    pal[0] = RGB(50, 32, 82);            /* C_LINE: rules, the knobs' tracks */
    pal[1] = RGB(104, 78, 156);          /* C_DIM: inactive, units */
    pal[2] = RGB(110, 225, 255);         /* C_GRAY: labels and the held notes, cyan */
    pal[3] = RGB(178, 96, 255);          /* C_AMB: the sound, the tempo, notes as they age: electric purple */
    pal[4] = RGB(255, 46, 151);          /* C_HI: values, the knobs, new notes: hot pink */
}

static struct {
    char msg[2][24];                     /* a short message over the header */
    uint32_t msg_until;
    uint32_t keys, btn;                  /* held at the last input */
    uint32_t page;
    uint32_t turned_at[4];               /* when each knob was last turned: its value lit for a moment */
    uint32_t tempo_at, chance_at;        /* the same, SELECT and ALGO */
    uint8_t redraw;
} ui;

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)     /* signed, no padding */
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static void dec_text(float x, int places, char *t)   /* x with one or two decimals */
{
    int32_t m = places == 2 ? 100 : 10, d = (int32_t)(x * (float)m + 0.5f);
    char f[4] = {'.', 0, 0, 0};
    int_text(d / m, t);
    if (places == 2) {
        f[1] = (char)('0' + d / 10 % 10);
        f[2] = (char)('0' + d % 10);
    } else
        f[1] = (char)('0' + d % 10);
    text_cat(t, f);
}

static const char *par_name(uint32_t p) { return p == PX_SPACE ? "SPACE" : p == P_VIB && ui.page == PG_HOME ? "DRIFT" : PAR[p].name; }

static void param_text(uint32_t p, char *t)  /* a parameter's value as the screen shows it */
{
    int32_t v;
    t[0] = 0;
    if (p == PX_SPACE)
        p = P_RMIX;
    v = set.p[p];
    switch (PAR[p].unit) {
    case U_TIME: {
        float s = knob_secs(v);
        if (s < 1.0f) {
            int_text((int32_t)(s * 1000.0f + 0.5f), t);
            text_cat(t, " MS");
        } else {
            dec_text(s, 1, t);
            text_cat(t, " S");
        }
        break;
    }
    case U_SECS: {
        float s = rev_secs(v);
        if (s < 10.0f)
            dec_text(s, 1, t);
        else
            int_text((int32_t)(s + 0.5f), t);
        text_cat(t, " S");
        break;
    }
    case U_HZ: {
        float h = lfo_hz(v);
        dec_text(h, h < 10.0f ? 2 : 1, t);
        text_cat(t, " HZ");
        break;
    }
    case U_DIV:
        text_cat(t, DIV_NAME[v]);
        break;
    case U_DDIV:
        text_cat(t, DDIV_NAME[v]);
        break;
    case U_MODE:
        text_cat(t, MODE_NAME[v]);
        break;
    case U_SHAPE:
        text_cat(t, SHAPE_NAME[v]);
        break;
    case U_RATIO:
        text_cat(t, RATIO_NAME[v]);
        break;
    case U_CURVE:
        text_cat(t, CURVE_NAME[v]);
        break;
    case U_SYNC:
        text_cat(t, v ? "EXT" : "INT");
        break;
    case U_ONOFF:
        text_cat(t, v ? "ON" : "OFF");
        break;
    case U_CH:
        if (v)
            int_text(v, t);
        else
            text_cat(t, "ALL");
        break;
    case U_WAVE:
        text_cat(t, "W");
        int_text(v + 1, t + 1);
        break;
    case U_NUM1:
        int_text(v + 1, t);
        break;
    case U_NUM:
        int_text(v, t);
        break;
    case U_OCT:
        int_text(v, t);
        text_cat(t, v == 1 ? " OCT" : " OCTS");
        break;
    case U_BPM:
        int_text(v, t);
        text_cat(t, " BPM");
        break;
    default:
        int_text(v, t);
        text_cat(t, "%");
        break;
    }
}

static int32_t par_frac(uint32_t p, int32_t w)   /* how far the knob is turned, 0 .. w */
{
    if (p == PX_SPACE)
        p = P_RMIX;
    return (set.p[p] - PAR[p].lo) * w / (PAR[p].hi - PAR[p].lo);
}

/* the load, for GLOBAL: now and the most since the page was opened, and the two counts */
static uint32_t cpu_peak;
static void load_text(char *t)
{
    uint32_t c = plat_cpu_pct();
    char v[12];
    if (c > cpu_peak)
        cpu_peak = c;
    t[0] = 0;
    text_cat(t, "CPU ");
    int_text((int32_t)c, v);
    text_cat(t, v);
    text_cat(t, "% PEAK ");
    int_text((int32_t)cpu_peak, v);
    text_cat(t, v);
    text_cat(t, "% DROPS ");
    int_text((int32_t)sheds, v);
    text_cat(t, v);
    text_cat(t, " RESETS ");
    int_text((int32_t)dsp_resets, v);
    text_cat(t, v);
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw = 1;
    if (ann_on()) {                          /* and to a screen reader (announce.c, fm1-reader) */
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

/* a screen reader that has just started listening (or asks) hears the whole screen */
static void speak(void)
{
    char t[240], v[16];
    uint32_t i;
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, APP_NAME ". ");
        text_cat(t, cur_preset()->name);
        text_cat(t, set.arp_on ? ". ARP ON. " : ". ARP OFF. ");
        text_cat(t, HOLD_NAME[set.hold]);
        text_cat(t, ". ");
        text_cat(t, PAGE[ui.page].name);
        for (i = 0; i < 4u; i++) {
            text_cat(t, ". ");
            text_cat(t, par_name(PAGE[ui.page].p[i]));
            text_cat(t, " ");
            param_text(PAGE[ui.page].p[i], v);
            text_cat(t, v);
        }
        if (ui.page == PG_GLOBAL) {
            char l[48];
            load_text(l);
            text_cat(t, ". ");
            text_cat(t, l);
        }
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

static int set_par(uint32_t p, int32_t v)    /* 1 = changed */
{
    v = clamp(v, PAR[p].lo, PAR[p].hi);
    if (v == set.p[p])
        return 0;
    set.p[p] = (int16_t)v;
    return 1;
}

static void param_turn(uint32_t p, int32_t d)
{
    char t[16];
    int changed;
    if (p == PX_SPACE) {                     /* SPACE: the reverb's mix and its length together */
        changed = set_par(P_RMIX, set.p[P_RMIX] + d);
        changed |= set_par(P_RLEN, set.p[P_RLEN] + d);
    } else
        changed = set_par(p, set.p[p] + d);
    if (!changed)
        return;
    set_dirty = 1;
    ui.redraw = 1;
    if (ann_on()) {
        param_text(p, t);
        app_say(par_name(p), t);
    }
}

static void octave_step(int32_t d)
{
    int32_t o = clamp(set.octave + d, -2, 2);
    char t[4];
    if (o == set.octave)
        return;
    ev_push(KEV_ALLOFF, 0, 0);
    set.octave = (int8_t)o;
    set_dirty = 1;
    t[0] = o < 0 ? '-' : '+';
    t[1] = (char)('0' + (o < 0 ? -o : o));
    t[2] = 0;
    app_say("OCTAVE", t);
}

static void page_go(uint32_t pg)
{
    ui.page = pg;
    cpu_peak = 0;
    ui.turned_at[0] = ui.turned_at[1] = ui.turned_at[2] = ui.turned_at[3] = 0;
    ui.redraw = 1;
    app_say(PAGE[pg].name, "");
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k, now = plat_ms();
    int32_t e;
    touch_poll();                            /* the sensed keys' presses and velocities */
    keys = touch_keys();
    for (k = 0; k < NKEYS; k++) {            /* key 0 is F3 (MIDI 53) */
        uint32_t bit = 1u << k, note = (uint32_t)(53 + (int32_t)k + 12 * set.octave);
        if ((keys & bit) && !(ui.keys & bit))
            ev_push(KEV_ON, note, clamp((int32_t)touch_vel(k), 1, 127));
        else if (!(keys & bit) && (ui.keys & bit))
            ev_push(KEV_OFF, note, 0);
    }
    for (k = 0; k < NB; k++) {               /* a button pressed */
        uint32_t bit = 1u << k, pg;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_FX) {                     /* FX: DELAY, again REVERB, and back */
            page_go(ui.page == PG_DELAY ? PG_REVERB : PG_DELAY);
            continue;
        }
        for (pg = 0; pg < NPAGE && PAGE[pg].btn != k; pg++)
            ;
        if (pg < NPAGE) {
            page_go(pg);
        } else if (k == B_OCTDN) {
            octave_step(-1);
        } else if (k == B_OCTUP) {
            octave_step(1);
        } else if (k == B_PLAY) {
            set.arp_on = (uint8_t)!set.arp_on;
            set_dirty = 1;
            app_say("ARP", set.arp_on ? "ON" : "OFF");
        } else if (k == B_REC) {             /* REC: NO HOLD -> HOLD -> LATCH */
            set.hold = (uint8_t)((set.hold + 1u) % 3u);
            set_dirty = 1;
            app_say(HOLD_NAME[set.hold], "");
        } else if (k == B_SAVE) {
            if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
                set_dirty = 0;
                app_say("SAVED", "");
            } else {
                app_say("SAVE", "FAILED");
            }
        }
    }
    for (k = 0; k < NE; k++) {
        if ((e = plat_enc((int)k)) == 0)
            continue;
        if (k == EN_PRESET) {
            int32_t p = ((int32_t)set.preset + e) % (int32_t)NPRESET;
            preset_load((uint32_t)(p < 0 ? p + (int32_t)NPRESET : p));
            set_dirty = 1;
            app_say("SOUND", cur_preset()->name);
        } else if (k == EN_SELECT) {         /* the two left encoders: tempo and chance, on every page */
            param_turn(P_TEMPO, e);
            ui.tempo_at = now;
        } else if (k == EN_ALGO) {
            param_turn(P_CHANCE, e * 2);
            ui.chance_at = now;
        } else {
            param_turn(PAGE[ui.page].p[k - EN_K1], e);
            ui.turned_at[k - EN_K1] = now;
        }
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
}

static void lights(void)
{
    uint32_t btn = 1u << PAGE[ui.page].btn, mid = 0, keys = 0, dim = 0, k, now_cs = sample_clock / 441u, w = viz_w, i;
    if (set.arp_on)
        btn |= LED_PLAY_GREEN;
    if (set.hold == H_LATCH)                 /* REC: lit for LATCH, half for HOLD */
        btn |= 1u << B_REC;
    else if (set.hold == H_HOLD)
        mid |= 1u << B_REC;
    if (set_dirty)
        btn |= 1u << B_SAVE;
    for (k = 0; k < NKEYS; k++) {
        int32_t note = 53 + (int32_t)k + 12 * set.octave;
        if (note >= 0 && note < 128 && (held_mask[note >> 5] >> (note & 31) & 1u))
            dim |= 1u << k;
    }
    for (i = 0; i < 8u && i < w; i++) {      /* the notes the arp just played, bright for a moment */
        uint32_t e = viz[(w - 1u - i) % VIZ_N], t = e >> 16;
        int32_t note = (int32_t)(e & 0xFFu) - 53 - 12 * set.octave;
        if ((e >> 8 & 0xFFu) && ((now_cs - t) & 0xFFFFu) < 12u && note >= 0 && note < (int32_t)NKEYS)
            keys |= 1u << note;
    }
    keys |= ui.keys;
    plat_lights(btn, keys, 0, 0, 1);
    plat_lights_mid(mid, dim & ~keys);       /* the held notes steady at the mid level (the glow is too faint) */
}

void app_input_only(void) { input(); }

/* a knob: a smooth arc of 270 degrees, from the lower left round to the lower right, its track dim and its value
 * pink. Each size's pixels are worked out once: where along the sweep each lies (0..240; 241.. the gap at the
 * bottom) and how far it is from the ring's centre line (x 16) */
#define KN_GLOW 1                            /* the box's margin past the ring */
#define KN_MAX (2 * (15 + KN_GLOW + 1) + 1)
typedef struct {
    int32_t r, n;
    uint8_t pos[KN_MAX * KN_MAX], dist[KN_MAX * KN_MAX];
} knob_map_t;
static knob_map_t kn_big, kn_small;          /* the pages' (r 15), HOME's (r 12) */

static float atan2_approx(float y, float x)  /* radians, -pi .. pi (to 0.004) */
{
    float ax = fm_fabsf(x), ay = fm_fabsf(y), z, a;
    if (ax < 1e-6f && ay < 1e-6f)
        return 0.0f;
    z = ax > ay ? ay / ax : ax / ay;
    a = z * (0.785398f + 0.273f * (1.0f - z));
    if (ay > ax)
        a = 1.570796f - a;
    if (x < 0.0f)
        a = FM_PI - a;
    return y < 0.0f ? -a : a;
}

static void knob_map_init(knob_map_t *m, int32_t r)
{
    int32_t x, y, c;
    m->r = r;
    m->n = 2 * (r + KN_GLOW + 1) + 1;
    c = m->n / 2;
    for (y = 0; y < m->n; y++)
        for (x = 0; x < m->n; x++) {
            float dx = (float)(x - c), dy = (float)(y - c), d = fm_sqrtf(dx * dx + dy * dy) - (float)r + 1.0f;
            float deg = atan2_approx(dy, dx) * (180.0f / FM_PI) - 135.0f;   /* y down: clockwise from the lower left */
            uint32_t i = (uint32_t)(y * m->n + x);
            while (deg < 0.0f)
                deg += 360.0f;
            if (deg <= 270.0f)
                m->pos[i] = (uint8_t)(deg * (240.0f / 270.0f));
            else                             /* the gap: how far from the nearer end, 0 .. 45 degrees */
                m->pos[i] = (uint8_t)(241.0f + fm_minf(deg - 270.0f, 360.0f - deg) * (14.0f / 45.0f));
            m->dist[i] = (uint8_t)fm_minf(fm_fabsf(d) * 16.0f, 255.0f);
        }
}

static void knobs_init(void)
{
    knob_map_init(&kn_big, 15);
    knob_map_init(&kn_small, 12);
}

static uint16_t mix565(uint16_t a, uint16_t b, uint32_t t)   /* a to b, t of 256 */
{
    uint32_t r = ((a >> 11) * (256u - t) + (b >> 11) * t) >> 8;
    uint32_t g = ((a >> 5 & 63u) * (256u - t) + (b >> 5 & 63u) * t) >> 8;
    uint32_t bl = ((a & 31u) * (256u - t) + (b & 31u) * t) >> 8;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

/* frac: how far it is turned, 0..240 */
static void knob_draw(const knob_map_t *m, int32_t cx, int32_t cy, int32_t frac, uint16_t c)
{
    int32_t x, y, h = m->n / 2;
    for (y = 0; y < m->n; y++)
        for (x = 0; x < m->n; x++) {
            uint32_t i = (uint32_t)(y * m->n + x), d = m->dist[i], pos = m->pos[i];
            uint16_t col = C_BG;
            if (d < 40u && pos <= 240u) {    /* the ring: about 4 px wide, its edges soft */
                uint16_t ring = (int32_t)pos <= frac && frac > 0 ? c : C_LINE;
                uint32_t cov = d <= 24u ? 256u : (40u - d) * 16u;
                col = mix565(col, ring, cov);
            }
            if (col != C_BG)
                cv_pset(cx - h + x, cy - h + y, col);
        }
}

static void header(uint32_t now)
{
    cv_begin(240, 32, C_BG);
    if (now < ui.msg_until) {
        int32_t x = cv_text(10, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], C_HI);
    } else {
        cv_text(10, 6, &FONT_B, "MIGHTY", C_GRAY);
        cv_text(10 + text_w(&FONT_B, "MIGHTY "), 6, &FONT_B, "ARP", C_HI);
        cv_text(230 - text_w(&FONT_B, cur_preset()->name), 6, &FONT_B, cur_preset()->name, C_AMB);
    }
    cv_rect(10, 30, 220, 1, C_LINE);
    cv_blit(0, 0);
}

/* the arp's steps over the last seconds: time across (newest at the right), pitch up; played notes pink, then
 * purple as they age; the steps the chance skipped as faint lavender ghosts. The held notes at the right edge. The
 * pitch range stays put: the keys at this octave, and the octaves the arp climbs above them */
static void notes_view(int32_t h, uint32_t secs_cs, int big)
{
    uint32_t i, now_cs = sample_clock / 441u, w = viz_w;
    int32_t lo = 53 + 12 * set.octave, hi = lo + (int32_t)NKEYS - 1 + 12 * (set.p[P_OCTAVES] - 1), span = hi - lo;
    for (i = 0; i < 128u; i++)               /* the pool: the held notes, lavender at the right */
        if (held_mask[i >> 5] >> (i & 31) & 1u) {
            int32_t y = h - 6 - clamp((int32_t)i - lo, 0, span) * (h - 12) / span;
            cv_rect(226, y - 1, 6, 3, C_GRAY);
        }
    for (i = 0; i < VIZ_N && i < w; i++) {
        uint32_t e = viz[(w - 1u - i) % VIZ_N], age = (now_cs - (e >> 16)) & 0xFFFFu, vel = e >> 8 & 0xFFu;
        int32_t x, y, s;
        if (age > secs_cs)
            break;
        x = 220 - (int32_t)(age * 210u / secs_cs);
        y = h - 6 - clamp((int32_t)(e & 0xFFu) - lo, 0, span) * (h - 12) / span;   /* an octave jump past it: the edge */
        if (!vel) {                          /* a ghost: the chance skipped it */
            s = big ? 3 : 2;
            cv_rect(x - s, y - s, 2 * s + 1, 1, C_GHOST);
            cv_rect(x - s, y + s, 2 * s + 1, 1, C_GHOST);
            cv_rect(x - s, y - s, 1, 2 * s + 1, C_GHOST);
            cv_rect(x + s, y - s, 1, 2 * s + 1, C_GHOST);
            continue;
        }
        s = (big ? 2 : 1) + (int32_t)(vel / 43u);
        cv_rect(x - s, y - s, 2 * s + 1, 2 * s + 1, age < 25u ? C_WHITE : age < secs_cs / 4u ? C_HI : age < secs_cs / 2u ? C_AMB : C_DIM);
    }
}

static void state_line(uint32_t now)     /* the arp and hold under PLAY and REC, then tempo and chance */
{
    char t[24];
    cv_begin(240, 28, C_BG);
    cv_text(10, 0, &FONT_XS, "PLAY", C_DIM);
    cv_text(10, 12, &FONT_B, set.arp_on ? "ARP" : "KEYS", set.arp_on ? C_HI : C_GRAY);
    cv_text(52, 0, &FONT_XS, "REC", C_DIM);
    cv_text(52, 12, &FONT_B, HOLD_NAME[set.hold], set.hold ? C_HI : C_GRAY);
    cv_text(120, 0, &FONT_XS, "TEMPO", C_DIM);
    if (set.p[P_SYNC]) {
        t[0] = 0;
        if (ext_bpm_x10) {
            int_text((int32_t)((ext_bpm_x10 + 5u) / 10u), t);
            text_cat(t, " EXT");
        } else
            text_cat(t, "EXT WAIT");
    } else
        param_text(P_TEMPO, t);
    cv_text(120, 12, &FONT_B, t, ui.tempo_at && now - ui.tempo_at < 400u ? C_WHITE : C_AMB);
    cv_text(184, 0, &FONT_XS, "CHANCE", C_DIM);
    int_text(set.p[P_CHANCE], t);
    text_cat(t, "%");
    cv_text(184, 12, &FONT_B, t, ui.chance_at && now - ui.chance_at < 400u ? C_WHITE : C_AMB);
    cv_blit(0, 212);
}

static void draw(void)
{
    char t[48];
    uint32_t i, now = plat_ms();
    const page_t *pg = &PAGE[ui.page];
    header(now);
    if (ui.page == PG_HOME) {
        /* HOME: the arp large, then the four macros in a row */
        cv_begin(240, 110, C_BG);
        notes_view(110, 600u, 1);
        cv_blit(0, 34);
        cv_begin(240, 68, C_BG);
        cv_rect(10, 0, 220, 1, C_LINE);
        for (i = 0; i < 4u; i++) {
            uint32_t p = pg->p[i];
            int32_t cx = 37 + (int32_t)i * 55;
            int lit = ui.turned_at[i] && now - ui.turned_at[i] < 400u;
            const char *nm = par_name(p);
            knob_draw(&kn_small, cx, 19, par_frac(p, 240), lit ? C_WHITE : C_HI);
            param_text(p, t);
            cv_text(cx - text_w(&FONT_B, t) / 2, 34, &FONT_B, t, lit ? C_WHITE : C_HI);
            cv_text(cx - text_w(&FONT_XS, nm) / 2, 52, &FONT_XS, nm, C_GRAY);
        }
        cv_blit(0, 144);
    } else {
        /* a page: its four knobs, two by two */
        cv_begin(240, 108, C_BG);
        cv_text(10, 0, &FONT_B, pg->name, C_GRAY);
        {
            const char *h = ui.page == PG_DELAY ? "FX AGAIN: REVERB" : ui.page == PG_REVERB ? "FX AGAIN: DELAY" : "";
            cv_text(230 - text_w(&FONT_XS, h), 1, &FONT_XS, h, C_DIM);
        }
        cv_rect(10, 18, 220, 1, C_LINE);
        for (i = 0; i < 4u; i++) {
            uint32_t p = pg->p[i];
            int32_t x = 10 + (int32_t)(i % 2u) * 116, y = 23 + (int32_t)(i / 2u) * 43;
            int lit = ui.turned_at[i] && now - ui.turned_at[i] < 400u;
            knob_draw(&kn_big, x + 16, y + 20, par_frac(p, 240), lit ? C_WHITE : C_HI);
            cv_text(x + 38, y + 1, &FONT_S, par_name(p), C_GRAY);
            param_text(p, t);
            if (text_w(&FONT_M, t) <= (i % 2u ? 66 : 74))   /* a long one (AS PLAYED, LINEAR) smaller */
                cv_text(x + 38, y + 16, &FONT_M, t, lit ? C_WHITE : C_HI);
            else
                cv_text(x + 38, y + 20, &FONT_B, t, lit ? C_WHITE : C_HI);
        }
        cv_blit(0, 34);
        cv_begin(240, 70, C_BG);
        cv_rect(10, 0, 220, 1, C_LINE);
        notes_view(70, 400u, 0);
        if (ui.page == PG_GLOBAL) {          /* the load: so a dropout can be told from a reset */
            load_text(t);
            cv_text(10, 4, &FONT_XS, t, C_DIM);
        }
        cv_blit(0, 142);
    }
    state_line(now);
}

void app_ui_init(void)
{
    theme();
    knobs_init();
    touch_on(1);                             /* velocity: the keys' readings on */
    lcd_fill(0, 0, 240, 240, C_BG);
    ui.page = PG_HOME;
    ui.redraw = 1;
}

void app_frame(void)
{
    static uint32_t last_viz, last_frame;
    uint32_t now = plat_ms();
    input();
    speak();
    lights();
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (viz_w != last_viz || ((viz_w || ui.page == PG_GLOBAL) && now - last_frame >= 50u))   /* the notes move (and
                                                                                                * GLOBAL's load) */
        ui.redraw = 1;
    if (ui.redraw) {
        ui.redraw = 0;
        last_viz = viz_w;
        last_frame = now;
        draw();
    }
}

#ifdef PLAT_HOST
static uint32_t arp_mark;
int app_expect(const char *what, int *got)
{
    uint32_t i;
    if (!strcmp(what, "octave"))
        *got = set.octave;
    else if (!strcmp(what, "preset"))
        *got = set.preset;
    else if (!strcmp(what, "npreset"))
        *got = (int)NPRESET;
    else if (!strcmp(what, "page"))
        *got = (int)ui.page;
    else if (!strcmp(what, "arp"))
        *got = set.arp_on;
    else if (!strcmp(what, "hold"))
        *got = set.hold;
    else if (!strcmp(what, "held"))          /* the notes the arp plays from */
        *got = (int)nheld;
    else if (!strcmp(what, "arp_notes"))     /* the notes the arp has played since the last arp_mark */
        *got = (int)(arp_count - arp_mark);
    else if (!strcmp(what, "arp_mark")) {    /* (always 0: counting starts again) */
        arp_mark = arp_count;
        *got = 0;
    }
    else if (!strcmp(what, "voices"))        /* sounding */
        *got = (int)eng_active();
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "sheds"))         /* the render ran late and a voice went */
        *got = (int)sheds;
    else if (!strcmp(what, "cap"))           /* the voices new notes may take */
        *got = (int)eng_cap(0);
    else if (!strcmp(what, "overload")) {    /* (always 0) the render ran late, as the device's audio interrupt tells */
        app_overload();
        *got = 0;
    }
    else if (!strcmp(what, "dsp_resets"))    /* the render's guard cleared a bad number */
        *got = (int)dsp_resets;
    else if (!strcmp(what, "last_note"))     /* the arp's last note played */
        *got = (int)last_played;
    else if (!strcmp(what, "ext_bpm"))       /* the tempo coming in over MIDI clock */
        *got = (int)((ext_bpm_x10 + 5u) / 10u);
    else if (!strcmp(what, "poison")) {      /* (always 0) a NaN into the reverb, as the out-of-table read made */
        eng_poison();
        *got = 0;
    } else {
        for (i = 0; i < NP && strcmp(what, PAR[i].id); i++)   /* a parameter by its id: its value */
            ;
        if (i == NP)
            return 0;
        *got = set.p[i];
    }
    return 1;
}
#endif
