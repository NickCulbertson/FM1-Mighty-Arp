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
       P_HUMAN, P_OJUMP, P_VRAND, P_SWING,            /* SEQ */
       P_DTIME, P_DFDBK, P_DMIX, P_CHORUS,            /* DELAY (MIX first on the page; CHORUS: its own page) */
       P_RMIX, P_RLEN, P_RDARK, P_SHIMMER,            /* REVERB */
       P_CLOCK, P_WIDTH, P_MIDICH, P_VCURVE,          /* GLOBAL (WIDTH: on DELAY) */
       P_TEMPO, P_CHANCE,                             /* SELECT and ALGO */
       P_CRATE, P_CDEPTH,                             /* CHORUS (after the rest: a save's slots keep their place) */
       P_VOICES, P_GLIDE,                             /* VOICE (SEL again) */
       P_CUTOFF, P_RES, P_FENV, P_FDEC,               /* FILTER (ENV again) */
       P_PWM, P_LFOCUT,                               /* LFO 2 (LFO again) */
       P_FATT, P_FSUS, P_FREL, P_KEYTRK,              /* FILTER ENVELOPE (ENV a third time; its DECAY is P_FDEC),
                                                       * FILTER's KEY TRACK */
       P_CHORD, P_STRUM, P_KEY, P_SCALE,              /* CHORDS (ARP again) */
       P_LOADS,                                       /* SOUNDS's fourth: what a sound change sets (the player's) */
       NP,
       PX_SPACE = NP,                                 /* HOME's macro: the reverb's mix and length together */
       PX_OPR, PX_OPL, PX_OPW, PX_OPD,                /* OP 1-4: the operator's ratio, level, wave, decay */
       PX_CAT, PX_SND, PX_SLOT,                       /* SOUNDS: the kind, the sound, the USER slot SAVE writes */
       PX_RESET,                                      /* RESETS: PARAMS, KEEP or (a detent right) RESET */
       PX_FACTORY,                                    /* RESETS: FACTORY, KEEP, SURE?, then everything as new */
       PX_NONE = 0xFF };                              /* no knob there */
#define NWAVE 14u                        /* W1..W8 the TX81Z's, W9 SAW, W10 SQUARE, W11-W12 PULSES, W13 TRIANGLE,
                                          * W14 NOISE */
static const char *const WAVE_SHORT[NWAVE] = {"SINE", "", "", "", "", "", "", "", "SAW", "SQR", "25%", "12%", "TRI",
                                              "NOISE"};
enum { U_PCT, U_TIME, U_DIV, U_OCT, U_MODE, U_DDIV, U_BPM, U_HZ, U_CHZ, U_SHAPE, U_NUM1, U_RATIO, U_NUM, U_WAVE,
       U_SECS, U_VOICES, U_GLIDE, U_CUT, U_CHORD, U_STRUM, U_KEY, U_SCALE, U_LOADS,
       U_SYNC, U_ONOFF, U_CH, U_CURVE };
/* CHORDS: what the arp makes of a note (a diatonic chord on it, or its chord's tones as the notes to play from), in
 * a key and a scale */
enum { CH_OFF, CH_TRIAD, CH_7TH, CH_LUSH, CH_OPEN, CH_TRIAD_T, CH_7TH_T, CH_OMNI_T, NCHORD };
#define CH_TONES(k) ((k) >= CH_TRIAD_T)     /* the chords' tones the notes to play from, not a chord a step */
#define NSCALE 5u
/* the arp's modes */
enum { M_RANDOM, M_UP, M_DOWN, M_UPDN, M_PLAYED, M_CONVERGE, M_DIVERGE, M_THUMB, M_THUMB_UD, M_PINKY, M_PINKY_UD,
       M_CHORD, M_WALK, NMODE };
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
    {"FEEDBACK", "fm_fdbk", 0, 7, 0, U_NUM},     {"MOD WAVE", "wave", 0, NWAVE - 1, 0, U_WAVE},
    {"RATE", "rate", 0, 10, 5, U_DIV},            {"OCTAVES", "octaves", 1, 4, 2, U_OCT},
    {"MODE", "mode", 0, NMODE - 1, M_RANDOM, U_MODE},           {"GATE", "gate", 5, 100, 50, U_PCT},
    {"HUMANIZE", "humanize", 0, 100, 0, U_PCT},  {"OCT JUMP", "oct_jump", 0, 100, 0, U_PCT},
    {"VEL RAND", "vel_rand", 0, 100, 20, U_PCT}, {"SWING", "swing", 0, 100, 0, U_PCT},
    {"TIME", "dly_time", 0, 8, 4, U_DDIV},       {"FEEDBACK", "dly_fdbk", 0, 95, 45, U_PCT},
    {"MIX", "dly_mix", 0, 100, 30, U_PCT},       {"MIX", "chorus", 0, 100, 25, U_PCT},
    {"MIX", "rev_mix", 0, 100, 40, U_PCT},       {"LENGTH", "rev_len", 0, 100, 55, U_SECS},
    {"DARK", "rev_dark", 0, 100, 40, U_PCT},     {"SHIMMER", "shimmer", 0, 100, 0, U_PCT},
    {"CLOCK", "clock", 0, 2, 0, U_SYNC},         {"WIDTH", "width", 0, 100, 100, U_PCT},
    {"MIDI CH", "midi_ch", 0, 16, 0, U_CH},      {"VEL CURVE", "vel_curve", 0, 3, 1, U_CURVE},
    {"TEMPO", "tempo", 40, 200, 90, U_BPM},      {"CHANCE", "chance", 0, 100, 33, U_PCT},
    {"RATE", "chorus_rate", 0, 100, 44, U_CHZ},  {"DEPTH", "chorus_depth", 0, 100, 65, U_PCT},
    {"VOICES", "voice_mode", 0, 2, 0, U_VOICES},     {"GLIDE", "glide", 0, 100, 0, U_GLIDE},
    {"CUTOFF", "cutoff", 0, 100, 100, U_CUT},    {"RESONANCE", "res", 0, 100, 0, U_PCT},
    {"ENV AMOUNT", "fenv", 0, 100, 0, U_PCT},    {"DECAY", "fdec", 0, 100, 60, U_TIME},
    {"PWM", "pwm", 0, 100, 0, U_PCT},            {"FILTER", "lfo_cut", 0, 100, 0, U_PCT},
    {"ATTACK", "fatt", 0, 100, 0, U_TIME},       {"SUSTAIN", "fsus", 0, 100, 0, U_PCT},
    {"RELEASE", "frel", 0, 100, 60, U_TIME},     {"KEY TRACK", "keytrack", 0, 100, 50, U_PCT},
    {"CHORD", "chord", 0, NCHORD - 1, 0, U_CHORD}, {"STRUM", "strum", 0, 100, 0, U_STRUM},
    {"KEY", "key", 0, 11, 0, U_KEY},             {"SCALE", "scale", 0, NSCALE - 1, 0, U_SCALE},
    {"PARAMS", "loads", 0, 1, 0, U_LOADS},
};

/* the pages: each button's four knobs */
enum { PG_HOME, PG_SOUND, PG_ENV, PG_LFO, PG_EDIT, PG_ARP, PG_SEQ, PG_DELAY, PG_REVERB, PG_GLOBAL, PG_CHORUS,
       PG_OP1, PG_OP2, PG_OP3, PG_OP4, PG_VOICE, PG_SOUNDS, PG_FILTER, PG_LFO2, PG_FENV, PG_CHORDS, PG_RESETS, NPAGE };   /* EDIT again: OP 1..4; FX: DELAY, REVERB, CHORUS; SEL
                                                             * again: VOICE */
typedef struct {
    const char *name;
    uint8_t btn, p[4];
} page_t;
static const page_t PAGE[NPAGE] = {
    {"HOME", B_HOME, {P_CHANCE, P_RATE, PX_SPACE, P_VIB}},
    {"SOUND", B_SEL, {P_TONE, P_STRIKE, P_DETUNE, P_VELO}},
    {"AMP ENVELOPE", B_ENV, {P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE}},
    {"LFO", B_LFO, {P_LRATE, P_LSHAPE, P_VIB, P_TREM}},
    {"EDIT", B_EDIT, {P_ALGO, P_RATIO, P_FDBK, P_WAVE}},
    {"ARP", B_ARP, {P_RATE, P_OCTAVES, P_MODE, P_GATE}},
    {"SEQ", B_SEQ, {P_HUMAN, P_OJUMP, P_VRAND, P_SWING}},
    {"DELAY", B_FX, {P_DMIX, P_DTIME, P_DFDBK, P_WIDTH}},
    {"REVERB", B_FX, {P_RMIX, P_RLEN, P_RDARK, P_SHIMMER}},
    {"GLOBAL", B_GLO, {P_CLOCK, P_MIDICH, P_VCURVE, PX_NONE}},
    {"CHORUS", B_FX, {P_CHORUS, P_CRATE, P_CDEPTH, PX_NONE}},
    {"OP 1", B_EDIT, {PX_OPR, PX_OPL, PX_OPW, PX_OPD}},
    {"OP 2", B_EDIT, {PX_OPR, PX_OPL, PX_OPW, PX_OPD}},
    {"OP 3", B_EDIT, {PX_OPR, PX_OPL, PX_OPW, PX_OPD}},
    {"OP 4", B_EDIT, {PX_OPR, PX_OPL, PX_OPW, PX_OPD}},
    {"VOICE", B_SEL, {P_VOICES, P_GLIDE, PX_NONE, PX_NONE}},
    {"SOUNDS", B_HOME, {PX_CAT, PX_SND, PX_SLOT, P_LOADS}},
    {"FILTER", B_ENV, {P_CUTOFF, P_RES, P_FENV, P_KEYTRK}},
    {"LFO 2", B_LFO, {P_PWM, P_LFOCUT, PX_NONE, PX_NONE}},
    {"FILTER ENVELOPE", B_ENV, {P_FATT, P_FDEC, P_FSUS, P_FREL}},
    {"CHORDS", B_ARP, {P_CHORD, P_STRUM, P_KEY, P_SCALE}},
    {"RESETS", B_GLO, {PX_RESET, PX_FACTORY, PX_NONE, PX_NONE}},   /* GLO again */
};
static const char *const CHORD_NAME[NCHORD] = {"OFF", "TRIAD", "7TH", "LUSH", "OPEN", "TRIAD TONES", "7TH TONES",
                                               "OMNI TONES"};
/* Nick's voicings, from his Macro MIDI Pad (MacroPicoMIDI): for each degree of the scale, the notes from the key's
 * root (127: none). LUSH: the major key's (with their ninths and the tones they share), the minor's, the harmonic
 * minor's; OPEN: the wide, cinematic ones (a major key's); OMNI: eight notes spread over the chord, to pluck from */
#define NV 127
static const int8_t LUSH_MAJ[7][5] = {{-12, 7, 12, 14, 16}, {-10, 5, 7, 12, 16}, {-8, 7, 11, 12, 16},
                                      {-7, 7, 9, 12, 16}, {-5, 7, 11, 12, 14}, {-3, 7, 11, 12, 16},
                                      {-1, 7, 11, 12, 14}};
static const int8_t LUSH_MIN[7][5] = {{0, 7, 12, 15, NV}, {2, 10, 14, 17, NV}, {3, 7, 10, 15, NV}, {5, 8, 12, 17, NV},
                                      {7, 10, 14, 19, NV}, {3, 8, 12, 15, NV}, {5, 10, 14, 17, NV}};
static const int8_t LUSH_HARM[7][5] = {{0, 3, 7, 11, NV}, {2, 5, 8, 12, NV}, {3, 7, 11, 14, NV}, {5, 8, 12, 15, NV},
                                       {7, 11, 14, 17, NV}, {8, 12, 15, 19, NV}, {11, 14, 17, 20, NV}};
static const int8_t OPEN_MAJ[7][5] = {{-12, 0, 7, 16, 19}, {-10, 2, 9, 17, 21}, {-8, 4, 11, 19, 23},
                                      {-7, 5, 12, 21, 24}, {-5, 7, 14, 23, 26}, {-3, 9, 16, 24, 28},
                                      {-1, 12, 18, 26, 31}};
static const int8_t OMNI_MAJ[7][8] = {{-12, 0, 4, 7, 12, 16, 19, 24}, {-10, 2, 5, 9, 14, 17, 21, 26},
                                      {-8, 4, 7, 11, 16, 19, 23, 28}, {-7, 5, 9, 12, 17, 21, 24, 29},
                                      {-5, 7, 11, 14, 19, 23, 26, 31}, {-3, 9, 12, 16, 21, 24, 28, 33},
                                      {-1, 11, 14, 18, 23, 26, 30, 35}};
static const uint8_t SCALE_STEPS[NSCALE][7] = {{0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10},
                                               {0, 2, 4, 5, 7, 9, 10}, {0, 2, 3, 5, 7, 8, 11}};
static const char *const SCALE_NAME[NSCALE] = {"MAJOR", "MINOR", "DORIAN", "MIXOLYDIAN", "HARM MINOR"};
static const char *const KEY_NAME[12] = {"C", "C#", "D", "EB", "E", "F", "F#", "G", "AB", "A", "BB", "B"};
#define IS_OP_PAGE(pg) ((pg) >= PG_OP1 && (pg) <= PG_OP4)
enum { V_POLY, V_MONO, V_LEGATO };       /* VOICES: MONO starts each note afresh, LEGATO only glides to it */
/* an operator's ratios, as OP's RATIO knob steps through them (every sound's are among them) */
static const float OP_RATIO[] = {0.5f, 1.0f, 1.41f, 1.5f, 2.0f, 2.5f, 2.76f, 3.0f, 3.5f, 4.0f, 5.0f, 5.4f, 6.0f, 7.0f,
                                 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f};
#define N_OPRATIO (sizeof OP_RATIO / sizeof OP_RATIO[0])

/* the arp's steps (in MIDI clocks: 24 a beat) and the delay's times (in beats) */
static const char *const DIV_NAME[11] = {"1/1", "1/2", "1/4D", "1/4", "1/8D", "1/8", "1/16D", "1/8T", "1/16", "1/16T",
                                         "1/32"};
static const uint8_t DIV_TICKS[11] = {96, 48, 36, 24, 18, 12, 9, 8, 6, 4, 3};
#define CLK_WRAP 576u                    /* six bars of clocks: every step, and every pair of steps, divides it */
static const char *const DDIV_NAME[9] = {"1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2"};
static const float DDIV_BEATS[9] = {0.25f, 0.375f, 1.0f / 3.0f, 0.5f, 0.75f, 2.0f / 3.0f, 1.0f, 1.5f, 2.0f};
static const char *const MODE_NAME[NMODE] = {"RANDOM", "UP", "DOWN", "UP-DN", "AS PLAYED", "CONVERGE", "DIVERGE",
                                             "THUMB UP", "THUMB U/D", "PINKY UP", "PINKY U/D", "CHORD", "WALK"};
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
/* the sounds' kinds: HOME again browses them so (SOUNDS); USER, the player's own (user[]) */
enum { CAT_KEYS, CAT_BELLS, CAT_PLUCKS, CAT_BASS, CAT_LEADS, CAT_PADS, CAT_PERC, CAT_WEIRD, CAT_USER, NCAT };
static const char *const CAT_NAME[NCAT] = {"KEYS", "BELLS", "PLUCKS", "BASS", "LEADS", "PADS", "PERC", "WEIRD",
                                           "USER"};
typedef struct {
    const char *name;
    uint8_t cat;
    uint8_t alg, fb;
    eng_sound_t s;
    int8_t a, d, su, r;                  /* the ENVELOPE page */
    int8_t lrate, lshape, vib, trem;     /* the LFO page */
    int8_t f_on, fcut, fres, fenv, fdec; /* the FILTER page (f_on 0: open, the FM sounds) */
    int8_t lfocut;                       /* LFO 2's FILTER (most leave it out: 0) */
} preset_t;
static const preset_t PRESET[] = {
    /* electric pianos: two stacks (ALGO 5), the body and the tine, which STRIKE rides */
    {"TINE EP", CAT_KEYS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.25f, 0, 0, 0, 1.2f, 0.15f),
                        OP(1, 3, 0.35f, 0, 1, 0, 2.0f, 0.08f), OP(14, 0, 0.21f, 0, 0, 0, 0.07f, 0)}, 0, 0},
     0, 74, 25, 82, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    /* a jazz organ: three drawbars (16', 8', 4') and the third harmonic's percussion, a strike carrier that falls
     * away (STRIKE: how much) */
    {"PERC ORGAN", CAT_KEYS, 7, 0, {{CARRIER(0.5f, 0, 0.28f), CARRIER(1, 0, 0.45f), CARRIER(2, 1, 0.22f),
                           OP(3, 0, 0.42f, 0, 1, 0, 0.3f, 0)}, 0, 0},
     0, 50, 100, 35, 80, LFO_SINE, 8, 10, 0, 0, 0, 0, 0, 0},
    {"DYNO EP", CAT_KEYS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.32f, 0, 0, 0, 0.7f, 0.15f),
                        OP(1, 3, 0.55f, 0, 1, 0, 1.2f, 0.05f), OP(14, 0, 0.36f, 0, 0, 0, 0.1f, 0.02f)}, 0, 0},
     0, 74, 20, 80, 72, LFO_TRI, 0, 40, 0, 0, 0, 0, 0, 0},
    {"BELL EP", CAT_KEYS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.2f, 0, 0, 0, 1.4f, 0.15f),
                        OP(2, 2, 0.4f, 0, 1, 0, 2.6f, 0.02f), OP(7, 0, 0.18f, 0, 0, 0, 0.5f, 0.05f)}, 0, 0},
     0, 80, 20, 86, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"WURLY", CAT_KEYS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.32f, 1, 0, 0, 0.6f, 0.3f),
                      OP(1, 0, 0.3f, 0, 1, 0, 1.0f, 0.2f), OP(3, 0, 0.12f, 0, 0, 0, 0.25f, 0.05f)}, 0, 0},
     0, 72, 35, 74, 74, LFO_TRI, 0, 35, 0, 0, 0, 0, 0, 0},
    /* a worn tape of an EP: dull (the filter low), wobbling (deep, slow drift: wow), dusty (W14 NOISE stirring the
     * tine) */
    {"LOFI EP", CAT_KEYS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.2f, 0, 0, 0, 1.2f, 0.15f),
                        OP(1, 5, 0.3f, 0, 1, 0, 2.0f, 0.08f), OP(1, 0, 0.05f, 13, 0, 0, 0.5f, 1.0f)}, 0, 0},
     2, 78, 25, 84, 14, LFO_DRIFT, 48, 10, 1, 40, 15, 20, 50, 0},
    {"DREAM EP", CAT_KEYS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.18f, 0, 0, 0, 2.5f, 0.2f),
                         OP(2, 5, 0.25f, 0, 1, 0, 3.5f, 0.15f), OP(14, 0, 0.08f, 0, 0, 0, 0.1f, 0)}, 0, 0},
     8, 86, 35, 92, 25, LFO_DRIFT, 14, 15, 0, 0, 0, 0, 0, 0},
    {"GLASS EP", CAT_KEYS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(3, 0, 0.12f, 0, 0, 0, 1.5f, 0.2f),
                         OP(1, 0, 0.45f, 0, 1, 0, 2.0f, 0.05f), OP(7, 0, 0.2f, 0, 0, 0, 0.4f, 0.05f)}, 0, 0},
     0, 78, 22, 84, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"FM PIANO", CAT_KEYS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.3f, 0, 0, 0, 0.8f, 0.1f),
                         OP(2, 3, 0.3f, 0, 1, 0, 1.0f, 0.05f), OP(5, 0, 0.08f, 0, 0, 0, 0.2f, 0)}, 0, 0},
     0, 78, 10, 70, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    /* keys */
    {"CLAV", CAT_KEYS, 4, 3, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.55f, 1, 0, 0, 0.25f, 0.25f),
                     OP(3, 0, 0.3f, 0, 1, 0, 0.3f, 0.1f), OP(1, 0, 0.4f, 0, 0, 0, 0.15f, 0.1f)}, 0, 0},
     0, 60, 30, 42, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"FM ORGAN", CAT_KEYS, 7, 2, {{CARRIER(0.5f, 0, 0.35f), CARRIER(1, 0, 0.45f), CARRIER(2, 0, 0.3f), CARRIER(4, 0, 0.18f)},
                        0, 0},
     1, 50, 100, 40, 82, LFO_SINE, 30, 15, 0, 0, 0, 0, 0, 0},
    {"VIBES", CAT_KEYS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(4, 0, 0.12f, 0, 0, 0, 0.4f, 0),
                      OP(4, 0, 0.14f, 0, 1, 0, 0.5f, 0), OP(1, 0, 0.05f, 0, 0, 0, 0.3f, 0)}, 0, 0},
     0, 84, 0, 82, 76, LFO_SINE, 0, 45, 0, 0, 0, 0, 0, 0},
    /* bells and mallets */
    {"GLASS BELL", CAT_BELLS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(3.5f, 0, 0.38f, 0, 0, 0, 2.5f, 0.2f),
                           OP(1, 0, 0.5f, 0, 1, 0, 3.0f, 0), OP(1.41f, 0, 0.16f, 0, 0, 0, 1.5f, 0.1f)}, 0, 0},
     0, 86, 0, 88, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    /* the WEIRD ones: noise and inharmonic FM for texture. SONAR: a metallic ping that rings and shimmers away */
    {"SONAR", CAT_WEIRD, 4, 0, {{CARRIER(1, 0, 1.0f), OP(2.76f, 0, 0.35f, 0, 0, 0, 2.0f, 0.1f),
                      OP(5.4f, 0, 0.3f, 0, 1, 0, 0.6f, 0), OP(1.41f, 0, 0.15f, 0, 0, 0, 1.0f, 0)}, 0, 0},
     0, 88, 0, 92, 25, LFO_SINE, 8, 0, 0, 0, 0, 0, 0, 0},
    {"MARIMBA", CAT_BELLS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(4, 0, 0.32f, 0, 0, 0, 0.03f, 0),
                        OP(1, 0, 0.6f, 0, 1, 0, 0.5f, 0), OP(1, 0, 0.08f, 0, 0, 0, 0.12f, 0)}, 0, 0},
     0, 58, 0, 66, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"MUSIC BOX", CAT_BELLS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(7, 0, 0.24f, 0, 0, 0, 0.15f, 0.05f),
                          OP(3, 0, 0.3f, 0, 1, 0, 0.8f, 0), OP(1, 0, 0.03f, 0, 0, 0, 0.4f, 0)}, 0, 0},
     0, 68, 0, 82, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"CELESTA", CAT_BELLS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(6, 0, 0.27f, 0, 0, 0, 0.35f, 0.02f),
                        OP(4, 0, 0.42f, 0, 1, 0, 0.55f, 0), OP(3, 0, 0.09f, 0, 0, 0, 0.4f, 0)}, 0, 0},
     0, 76, 0, 82, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"CRYSTAL", CAT_BELLS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(7, 0, 0.2f, 0, 0, 0, 0.08f, 0.02f),
                        OP(3, 0, 0.3f, 0, 1, 0, 1.2f, 0), OP(9, 0, 0.15f, 0, 0, 0, 0.3f, 0)}, 0, 0},
     0, 76, 0, 90, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    /* plucks */
    {"HARP", CAT_PLUCKS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(2, 0, 0.24f, 0, 0, 0, 0.15f, 0.1f),
                     OP(1, 0, 0.5f, 0, 1, 0, 1.2f, 0), OP(1, 0, 0.04f, 0, 0, 0, 0.5f, 0)}, 0, 0},
     0, 64, 0, 80, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"KOTO", CAT_PLUCKS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(3, 0, 0.45f, 0, 0, 0, 0.12f, 0.05f),
                     OP(1, 0, 0.35f, 0, 1, 0, 0.5f, 0), OP(5, 0, 0.2f, 0, 0, 0, 0.05f, 0)}, 0, 0},
     0, 66, 0, 72, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"NYLON", CAT_PLUCKS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.25f, 0, 0, 0, 0.3f, 0.05f),
                      OP(3, 0, 0.15f, 0, 1, 0, 0.15f, 0), OP(1, 0, 0.1f, 0, 0, 0, 0.1f, 0)}, 0, 0},
     0, 70, 0, 70, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"PLUCK", CAT_PLUCKS, 0, 6, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.4f, 0, 0, 0, 0.25f, 0.05f),
                      OP(1, 0, 0.3f, 0, 0, 0, 0.2f, 0), OP(1, 0, 0.3f, 0, 0, 0, 0.2f, 0)}, 0, 0},
     0, 60, 0, 70, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    /* basses: an octave down (ratio 1/2) */
    /* the 80s DX bass (Danger Zone's): a bright snap falling quickly to a round, barking sustain */
    {"FM BASS", CAT_BASS, 2, 6, {{CARRIER(0.5f, 0, 0.8f), OP(0.5f, 0, 0.55f, 0, 0, 0, 0.18f, 0.25f),
                        OP(1, 0, 0.25f, 0, 0, 0, 0.12f, 0), OP(0.5f, 0, 0.3f, 0, 0, 0, 0.08f, 0.1f)}, 1, 0},
     0, 62, 70, 35, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"SLAP BASS", CAT_BASS, 4, 0, {{CARRIER(0.5f, 0, 1.0f), OP(0.5f, 0, 0.65f, 0, 0, 0, 0.15f, 0.2f),
                          OP(1.5f, 0, 0.5f, 0, 1, 0, 0.12f, 0), OP(3.5f, 0, 0.45f, 0, 0, 0, 0.06f, 0)}, 1, 0},
     0, 64, 45, 32, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    /* the 16-bit consoles' bass: a four-operator stack with full feedback, a growl that falls to a buzz */
    {"16BIT BASS", CAT_BASS, 0, 6, {{CARRIER(0.5f, 0, 0.85f), OP(1, 0, 0.5f, 0, 0, 0, 0.2f, 0.35f),
                           OP(1, 0, 0.35f, 0, 0, 0, 0.15f, 0.2f), OP(2, 0, 0.3f, 0, 0, 0, 0.1f, 0.1f)}, 1, 0},
     0, 65, 60, 32, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"WOOD BASS", CAT_BASS, 4, 0, {{CARRIER(0.5f, 0, 1.0f), OP(0.5f, 0, 0.2f, 0, 0, 0, 0.15f, 0.05f),
                          OP(1, 0, 0.2f, 0, 1, 0, 0.08f, 0), OP(2, 0, 0.15f, 0, 0, 0, 0.04f, 0)}, 1, 0},
     0, 70, 0, 50, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    /* synths */
    {"FM BRASS", CAT_LEADS, 0, 4, {{CARRIER(1, 0, 1.0f), OP(1, 0, 0.3f, 0, 0, 0.06f, 0.4f, 0.6f),
                         OP(1, 0, 0.12f, 0, 0, 0.05f, 0.5f, 0.5f), OP(1, 0, 0.08f, 0, 0, 0, 0.4f, 0.5f)}, 0, 0},
     12, 70, 80, 45, 76, LFO_SINE, 25, 0, 0, 0, 0, 0, 0, 0},
    {"SYNC LEAD", CAT_LEADS, 0, 6, {{CARRIER(1, 0, 1.0f), OP(2, 0, 0.35f, 0, 0, 0, 0.6f, 0.6f),
                          OP(1, 0, 0.2f, 0, 0, 0, 0.5f, 0.5f), OP(1, 0, 0.2f, 0, 0, 0, 0.5f, 0.5f)}, 2, 0.05f},
     0, 60, 70, 40, 76, LFO_SINE, 30, 0, 0, 0, 0, 0, 0, 0},
    {"FM SAW", CAT_LEADS, 4, 5, {{CARRIER(1, -8, 0.7f), OP(1, 0, 0.32f, 0, 0, 0, 1.0f, 0.8f),
                       CARRIER(1, 8, 0.7f), OP(1, 0, 0.3f, 0, 0, 0, 1.0f, 0.8f)}, 0, 0},
     0, 60, 85, 50, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    /* saws: W9 carriers (band-limited), a little FM on top */
#define SAWC(r, c, l) OP(r, c, l, 8, 0, 0.0f, 0.0f, 0.0f)
    {"SAW PAD", CAT_PADS, 4, 0, {{SAWC(1, -7, 0.75f), OP(1, 0, 0.05f, 0, 0, 0.5f, 3.0f, 0.8f),
                        SAWC(1, 7, 0.75f), OP(2, 0, 0.03f, 0, 0, 0.8f, 3.0f, 0.6f)}, 0, 0},
     70, 82, 85, 90, 30, LFO_DRIFT, 15, 0, 1, 60, 10, 15, 85, 0},
    {"SAW LEAD", CAT_LEADS, 4, 0, {{SAWC(1, -5, 0.85f), OP(1, 0, 0.12f, 0, 0, 0, 0.4f, 0.3f),
                         SAWC(1, 5, 0.65f), OP(2, 0, 0.08f, 0, 0, 0, 0.5f, 0.3f)}, 2, 0.04f},
     0, 60, 80, 40, 76, LFO_SINE, 25, 0, 1, 65, 20, 20, 65, 0},
    {"SAW BASS", CAT_BASS, 4, 0, {{SAWC(0.5f, 0, 0.8f), OP(0.5f, 0, 0.15f, 0, 0, 0, 0.15f, 0.05f),
                         CARRIER(0.5f, 0, 0.4f), OP(1, 0, 0, 0, 0, 0, 0.1f, 0)}, 1, 0},
     0, 62, 40, 30, 45, LFO_SINE, 0, 0, 1, 35, 30, 45, 55, 0},
    /* pads */
    {"GLASS PAD", CAT_PADS, 4, 0, {{CARRIER(1, 0, 0.7f), OP(1, 0, 0.19f, 0, 0, 0, 4.0f, 0.6f),
                          CARRIER(1, 5, 0.55f), OP(2, 0, 0.13f, 0, 0, 0, 3.0f, 0.5f)}, 0, 0},
     72, 82, 80, 88, 30, LFO_DRIFT, 22, 0, 0, 0, 0, 0, 0, 0},
    {"STRINGS", CAT_PADS, 4, 3, {{CARRIER(1, -6, 0.7f), OP(1, 0, 0.18f, 0, 0, 0.3f, 1.0f, 0.9f),
                        CARRIER(1, 6, 0.7f), OP(1, 0, 0.15f, 0, 0, 0.3f, 1.0f, 0.9f)}, 0, 0},
     70, 80, 90, 85, 76, LFO_SINE, 25, 0, 0, 0, 0, 0, 0, 0},
    {"CHOIR", CAT_PADS, 5, 0, {{CARRIER(1, 0, 0.8f), CARRIER(3, 0, 0.25f), CARRIER(5, 0, 0.1f),
                      OP(1, 0, 0.1f, 0, 0, 0.4f, 2.0f, 0.9f)}, 0, 0},
     75, 80, 90, 86, 30, LFO_DRIFT, 25, 0, 0, 0, 0, 0, 0, 0},
    {"DRIFT PAD", CAT_PADS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(0.5f, 0, 0.1f, 0, 0, 1.0f, 4.0f, 1.0f),
                          CARRIER(2, 5, 0.5f), OP(3, 0, 0.08f, 0, 0, 0, 3.0f, 0.7f)}, 0, 0},
     80, 85, 85, 92, 25, LFO_DRIFT, 40, 25, 0, 0, 0, 0, 0, 0},
    {"HALO", CAT_PADS, 4, 0, {{CARRIER(1, 0, 1.0f), OP(4, 0, 0.1f, 0, 0, 0.5f, 3.0f, 0.8f),
                     CARRIER(2, 0, 0.5f), OP(7, 0, 0.05f, 0, 0, 1.0f, 3.0f, 0.5f)}, 0, 0},
     78, 86, 70, 94, 20, LFO_DRIFT, 20, 20, 0, 0, 0, 0, 0, 0},
    /* the analog waves (W10-W13): square and pulses (two saws), triangle; band-limited like the saw */
#define WAVC(r, c, l, w) OP(r, c, l, w, 0, 0.0f, 0.0f, 0.0f)
    {"SUPER SAW", CAT_PADS, 7, 0, {{SAWC(1, -18, 0.42f), SAWC(1, -6, 0.42f), SAWC(1, 6, 0.42f), SAWC(1, 18, 0.42f)},
                                  0, 0},
     10, 70, 85, 80, 30, LFO_DRIFT, 8, 0, 1, 75, 10, 10, 75, 0},
    {"ANALOG BASS", CAT_BASS, 7, 0, {{SAWC(0.5f, 0, 0.7f), WAVC(1, 3, 0.35f, 9), CARRIER(1, 0, 0), CARRIER(1, 0, 0)},
                                    1, 0},
     0, 60, 50, 25, 45, LFO_SINE, 0, 0, 1, 32, 35, 50, 52, 0},
    {"SQUARE LEAD", CAT_LEADS, 7, 0, {{WAVC(1, -4, 0.6f, 9), WAVC(1, 4, 0.5f, 9), CARRIER(1, 0, 0), CARRIER(1, 0, 0)},
                                     2, 0.03f},
     0, 60, 80, 40, 76, LFO_SINE, 25, 0, 1, 60, 20, 25, 60, 0},
    {"CHIP ARP", CAT_LEADS, 7, 0, {{WAVC(1, 0, 0.9f, 11), WAVC(2, 0, 0.2f, 9), CARRIER(1, 0, 0), CARRIER(1, 0, 0)},
                                  0, 0},
     0, 55, 30, 30, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"PULSE PLUCK", CAT_PLUCKS, 4, 0, {{WAVC(1, 0, 1.0f, 10), OP(2, 0, 0.15f, 0, 0, 0, 0.1f, 0),
                                       CARRIER(1, 0, 0), OP(1, 0, 0, 0, 0, 0, 0.1f, 0)}, 0, 0},
     0, 66, 0, 75, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"TRI PAD", CAT_PADS, 7, 0, {{WAVC(1, -5, 0.7f, 12), WAVC(2, 5, 0.35f, 12), WAVC(0.5f, 0, 0.3f, 12),
                                 CARRIER(1, 0, 0)}, 0, 0},
     70, 82, 85, 90, 25, LFO_DRIFT, 15, 0, 0, 0, 0, 0, 0, 0},
    {"TRI FLUTE", CAT_LEADS, 4, 0, {{WAVC(1, 0, 0.9f, 12), OP(1, 0, 0.03f, 0, 0, 0.08f, 1.0f, 0.6f),
                                    CARRIER(1, 0, 0), OP(1, 0, 0, 0, 0, 0, 0.1f, 0)}, 0, 0},
     6, 70, 80, 40, 76, LFO_SINE, 30, 0, 0, 0, 0, 0, 0, 0},
    /* VA: the analog-synth classics. No filter: the bite and the sweep are FM, a modulator with its own envelope over
     * the saw, square and pulse carriers (a quick fall for a pluck's or a bass's attack, a slow rise for brass) */
#define Z CARRIER(1, 0, 0)
#define NOISEC(l) OP(1, 0, l, 13, 0, 0.0f, 0.0f, 0.0f)   /* a carrier of W14 NOISE */
    {"POLY SYNTH", CAT_KEYS, 6, 4, {{SAWC(1, -6, 0.45f), SAWC(1, 6, 0.45f), CARRIER(1, 0, 0.5f),
                                    OP(1, 0, 0.25f, 0, 0, 0, 0.3f, 0.1f)}, 0, 0},
     0, 72, 40, 60, 45, LFO_SINE, 0, 0, 1, 45, 25, 45, 60, 0},
    {"80S KEYS", CAT_KEYS, 7, 0, {{WAVC(1, 0, 0.9f, 9), SAWC(2, 0, 0.45f), Z, Z}, 0, 0},
     0, 70, 30, 62, 45, LFO_SINE, 0, 0, 1, 58, 15, 35, 58, 0},
    {"SAW PLUCK", CAT_PLUCKS, 4, 0, {{SAWC(1, 0, 0.95f), OP(1, 0, 0.25f, 0, 0, 0, 0.12f, 0),
                                     SAWC(1, 8, 0.55f), OP(2, 0, 0.15f, 0, 0, 0, 0.1f, 0)}, 0, 0},
     0, 66, 0, 72, 45, LFO_SINE, 0, 0, 1, 30, 30, 60, 55, 0},
    {"SQR PLUCK", CAT_PLUCKS, 7, 0, {{WAVC(1, 0, 1.0f, 9), WAVC(2, 4, 0.4f, 10), Z, Z}, 0, 0},
     0, 64, 0, 70, 45, LFO_SINE, 0, 0, 1, 38, 35, 55, 52, 0},
    {"SQUARE BASS", CAT_BASS, 7, 0, {{WAVC(0.5f, 0, 0.6f, 9), CARRIER(0.5f, 0, 0.4f), Z, Z}, 1, 0},
     0, 60, 60, 25, 45, LFO_SINE, 0, 0, 1, 35, 20, 40, 55, 0},
    {"PULSE BASS", CAT_BASS, 4, 0, {{WAVC(0.5f, 0, 1.0f, 10), OP(0.5f, 0, 0.2f, 0, 0, 0, 0.15f, 0.05f), Z,
                                    OP(1, 0, 0, 0, 0, 0, 0.1f, 0)}, 1, 0},
     0, 62, 45, 28, 45, LFO_SINE, 0, 0, 1, 30, 35, 50, 50, 0},
    {"SYNTH BASS", CAT_BASS, 6, 0, {{SAWC(0.5f, 0, 0.6f), SAWC(0.5f, 10, 0.4f), CARRIER(0.5f, 0, 0.35f),
                                    OP(1, 0, 0.2f, 0, 0, 0, 0.1f, 0)}, 1, 0},
     0, 62, 55, 25, 45, LFO_SINE, 0, 0, 1, 28, 45, 55, 52, 0},
    /* the West Coast lead (G-funk's): two saws a little apart, a long legato glide between notes, vibrato */
    {"WEST COAST", CAT_LEADS, 7, 0, {{SAWC(1, -4, 0.5f), SAWC(1, 4, 0.5f), Z, Z}, 2, 0.09f},
     4, 60, 90, 45, 62, LFO_SINE, 28, 0, 1, 70, 22, 15, 65, 0},
    {"PULSE LEAD", CAT_LEADS, 7, 0, {{WAVC(1, 0, 0.6f, 10), WAVC(1, 7, 0.35f, 11), Z, Z}, 2, 0.04f},
     0, 60, 80, 40, 76, LFO_SINE, 30, 0, 1, 60, 25, 25, 62, 0},
    {"80S BRASS", CAT_LEADS, 6, 0, {{SAWC(1, -5, 0.4f), SAWC(1, 5, 0.4f), CARRIER(1, 0, 0.4f),
                                   OP(1, 0, 0.3f, 0, 0, 0.08f, 0.5f, 0.4f)}, 0, 0},
     10, 70, 80, 45, 76, LFO_SINE, 15, 0, 1, 40, 15, 45, 70, 0},
    {"SAW STRINGS", CAT_PADS, 7, 0, {{SAWC(1, -12, 0.35f), SAWC(1, 0, 0.35f), SAWC(1, 12, 0.35f),
                                     WAVC(2, 0, 0.15f, 10)}, 0, 0},
     72, 82, 90, 88, 70, LFO_SINE, 10, 0, 1, 65, 10, 10, 80, 0},
    {"SQUARE PAD", CAT_PADS, 7, 0, {{WAVC(1, -6, 0.45f, 9), WAVC(1, 6, 0.45f, 9), WAVC(2, 0, 0.25f, 12), Z}, 0, 0},
     75, 82, 85, 92, 25, LFO_DRIFT, 12, 15, 1, 55, 20, 15, 85, 0},
    {"SWEEP PAD", CAT_PADS, 4, 0, {{SAWC(1, 0, 0.55f), OP(1, 0, 0.1f, 0, 0, 2.0f, 4.0f, 0.6f),
                                   SAWC(1, 7, 0.55f), OP(1, 0, 0.08f, 0, 0, 3.0f, 4.0f, 0.5f)}, 0, 0},
     70, 85, 85, 92, 20, LFO_DRIFT, 12, 0, 1, 35, 40, 50, 92, 0},
    /* gnarly FM, the 16-bit consoles' way: feedback and inharmonic ratios */
    {"METAL LEAD", CAT_LEADS, 0, 6, {{CARRIER(1, 0, 0.9f), OP(2, 0, 0.4f, 0, 0, 0, 0.6f, 0.7f),
                           OP(3, 0, 0.25f, 0, 0, 0, 0.5f, 0.6f), OP(1, 0, 0.2f, 0, 0, 0, 0.4f, 0.5f)}, 2, 0.03f},
     2, 60, 85, 50, 76, LFO_SINE, 20, 0, 0, 0, 0, 0, 0, 0},
    {"DEEP SEA", CAT_WEIRD, 5, 3, {{CARRIER(0.5f, 0, 0.45f), CARRIER(1, -6, 0.4f), CARRIER(1, 6, 0.4f),
                         OP(1.41f, 0, 0.2f, 0, 0, 1.0f, 4.0f, 0.7f)}, 0, 0},
     70, 85, 85, 90, 20, LFO_DRIFT, 25, 15, 0, 0, 0, 0, 0, 0},
    /* percussion: the noise from full feedback on operator 4 (no noise wave), short and falling */
    /* a snare's two parts: the knock (operator 1, its own quick envelope, STRIKE its level) and the rattle, W14
     * NOISE on the ENVELOPE's short decay */
    {"FM SNARE", CAT_PERC, 4, 0, {{OP(1, 0, 0.75f, 0, 1, 0, 0.06f, 0), OP(1.41f, 0, 0.35f, 0, 0, 0, 0.03f, 0),
                         NOISEC(0.75f), OP(1, 0, 0, 0, 0, 0, 0.1f, 0)}, 0, 0},
     0, 58, 0, 46, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    {"NOISE HAT", CAT_PERC, 4, 0, {{NOISEC(0.7f), OP(1, 0, 0, 0, 0, 0, 0.1f, 0),
                          CARRIER(9, 0, 0.25f), OP(14, 0, 0.6f, 0, 0, 0, 0.05f, 0)}, 0, 0},
     0, 36, 0, 30, 45, LFO_SINE, 0, 0, 0, 0, 0, 0, 0, 0},
    /* STATIC: a wash of noise swelling in, crackling (random tremolo, S&H) over a low hum */
    {"STATIC", CAT_WEIRD, 6, 0, {{CARRIER(1, 0, 0.2f), CARRIER(0.5f, 0, 0.15f), NOISEC(0.35f),
                       OP(1, 0, 0, 0, 0, 0, 0.1f, 0)}, 0, 0},
     60, 80, 80, 85, 70, LFO_SH, 0, 45, 1, 70, 10, 0, 60, 0},
    /* WIND: noise through a resonant filter the slow LFO sweeps, gusting */
    {"WIND", CAT_WEIRD, 6, 0, {{CARRIER(1, 0, 0.05f), CARRIER(0.5f, 0, 0.03f), NOISEC(0.8f),
                     OP(1, 0, 0, 0, 0, 0, 0.1f, 0)}, 0, 0},
     75, 85, 90, 88, 30, LFO_DRIFT, 0, 20, 1, 45, 60, 0, 60, 60},
    /* ALIEN: two inharmonic stacks, the pitch jumping at random (S&H vibrato) */
    {"ALIEN", CAT_WEIRD, 4, 3, {{CARRIER(1, 0, 0.7f), OP(1.41f, 0, 0.4f, 0, 0, 0, 1.0f, 0.8f),
                      CARRIER(2.76f, 12, 0.4f), OP(5.4f, 0, 0.2f, 0, 0, 0, 1.0f, 0.6f)}, 0, 0},
     20, 80, 70, 70, 55, LFO_SH, 60, 0, 0, 0, 0, 0, 0, 0},
};
#define NPRESET (sizeof PRESET / sizeof PRESET[0])
/* the order PRESET and SOUNDS go through them: kind by kind, each kind's like sounds together (the numbers stay
 * as saves know them; a sound added goes into its kind here). tests/make_sounds_test.py reads it */
static const uint8_t ORDER[] = {
    0, 2, 3, 4, 5, 6, 7, 8, 45, 44, 9, 10, 1, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 41, 46, 47, 22, 23, 24, 25,
    31, 38, 48, 49, 50, 26, 53, 27, 28, 57, 30, 51, 39, 52, 40, 43, 29, 37, 54, 33, 32, 34, 35, 36, 42, 55, 56, 59,
    60, 13, 58, 61, 62, 63};
#define NORDER (sizeof ORDER)
#define NBROWSE (NORDER + NUSER)         /* what PRESET goes through: the sounds in ORDER, then the USER slots */
typedef char order_is_every_sound[NORDER == NPRESET ? 1 : -1];
static uint32_t browse_at(uint32_t k) { return k < NORDER ? ORDER[k] : NPRESET + (k - NORDER); }   /* the k-th */
static uint32_t browse_pos(uint32_t n)   /* where sound n is browsed */
{
    uint32_t k;
    if (n >= NPRESET)
        return NORDER + (n - NPRESET);
    for (k = 0; k < NORDER; k++)
        if (ORDER[k] == n)
            return k;
    return 0;
}
/* a saved record names sounds by number: a sound added or taken out, or a knob, changes what the numbers mean, so
 * bump SET_MAGIC and USER_MAGIC (an older record then loads as new: there are no saves before 0.1 to bring forward) */
typedef char npreset_is_mare[NPRESET == 64u ? 1 : -1];

/* the wave EDIT shows for a sound: its first modulator's (the knob, turned, gives every modulator one) */
static uint32_t preset_wave(const preset_t *pr)
{
    uint32_t k;
    for (k = 0; k < 4u; k++)
        if (!(ENG_CAR[pr->alg & 7u] >> k & 1u))
            return pr->s.op[k].wave;
    return 0;
}

/* --------------------------------------------------------------- settings --- */
/* "MARE": the settings record (another magic, or size: the settings as new) */
#define SET_MAGIC 0x4552414Du
typedef struct {
    uint32_t magic;
    int16_t p[NP];
    uint8_t preset;
    int8_t octave;                       /* -2 .. +2 */
    uint8_t arp_on, hold;                /* hold: H_OFF, H_HOLD, H_LATCH */
    /* the sound's four operators as OP 1-4 left them (the sound sets them): the ratio (OP_RATIO's index), the level
     * (0..100: a carrier's amplitude, a modulator's depth, x 100), the wave (0..8), the decay (0..100, knob_secs) */
    uint8_t op_ratio[4], op_level[4], op_wave[4], op_decay[4];
} settings_t;
static settings_t set;
static uint8_t set_dirty;                /* changed since the last save: SAVE is lit */

/* the player's own presets, USER 1..16: every knob (the arp, CHORDS, the effects and TEMPO too, not the player's own:
 * CLOCK, MIDI CH, VEL CURVE, LOADS) and the four operators, on the factory sound it began from (base: the operators'
 * details no knob shows). Their own store object, OBJ_USER: SAVE on SOUNDS writes one there. A sound number past the
 * factory's (set.preset >= NPRESET) is user slot n - NPRESET. LOADS decides what a sound change sets: SOUND, the
 * sound's knobs (SOUND_P) only; ALL, everything a USER preset kept, and a factory sound the arp and effects as new */
static const uint8_t SOUND_P[] = {P_TONE, P_STRIKE, P_DETUNE, P_VELO, P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE,
                                  P_LRATE, P_LSHAPE, P_VIB, P_TREM, P_ALGO, P_RATIO, P_FDBK, P_WAVE, P_VOICES,
                                  P_GLIDE, P_CUTOFF, P_RES, P_FENV, P_FDEC, P_PWM, P_LFOCUT, P_FATT, P_FSUS,
                                  P_FREL, P_KEYTRK};
#define NSOUNDP (sizeof SOUND_P)
#define NUSER 16u
#define USER_MAGIC 0x3555414Du           /* "MAU5": every knob (user_t); another magic, or size: an empty bank */
typedef char np_is_mau5s[NP == 57u ? 1 : -1];   /* a knob added: bump USER_MAGIC (and SET_MAGIC) */
typedef struct {
    uint8_t used, base;
    uint8_t whole;                       /* it kept everything; one from an older bank its sound only */
    uint8_t name;                        /* its name, 1..255 (NAME_A x NAME_B, user_name); 0: none, the factory
                                          * sound's it began from (an older save's) */
    int16_t p[NP];                       /* every knob, by its number (the player's own kept, never loaded) */
    uint8_t op_ratio[4], op_level[4], op_wave[4], op_decay[4];
} user_t;
static struct {
    uint32_t magic;
    user_t u[NUSER];
} users;


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
static float glide_secs(int32_t v) { return v ? 0.002f * fm_exp2f((float)v * 0.0996578f) : 0.0f; }   /* 0, 2 ms .. 2 s */
static float cut_hz(int32_t v) { return 30.0f * fm_exp2f((float)v * 0.0923f); }            /* 30 Hz .. 18 kHz */
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
static uint8_t held_by;                  /* who the note being added comes from (DN_KEY, DN_MIDI) */
static uint32_t nheld;
/* what holds each note down: a key (down), MIDI on which channels (midi_down, a bit a channel). HOLD and LATCH keep
 * the rest; held_src remembers who a held note came from, so MIDI's can go when MIDI says so */
static uint8_t down[128];
static uint16_t midi_down[128];
#define DN_KEY 1u
#define DN_MIDI 2u
static uint8_t held_src[NHELD];
static uint16_t held_ch[NHELD];          /* the MIDI channels a held note came in on (a bit a channel) */
static uint16_t held_by_ch;              /* the note being added's channel bit (MIDI), 0 a key's */
static uint16_t midi_played[128];        /* the channels that played each note (held or not, maybe still releasing):
                                          * All Sound Off silences theirs */
static int is_down(int32_t note) { return (down[note] & DN_KEY) || midi_down[note]; }
static uint32_t ndown;
static uint8_t arp_on_now, hold_now, clk_out_now, sync_now, voices_now;   /* what the render last acted on */

/* published to the screen: the held set, and the steps the arp has taken (played or skipped) */
static volatile uint32_t held_mask[4];
#define VIZ_N 64u
static volatile uint32_t viz[VIZ_N];     /* note | vel << 8 (0: the chance skipped it) | the time in 1/100 s << 16 */
static volatile uint32_t viz_w, arp_count;
static volatile uint32_t sample_clock;   /* samples rendered: the falling notes' clock */

static uint8_t midi_ch_now;              /* MIDI CH as the render last acted on: the channel in and out (0 ALL) */
static uint32_t out_ch(void) { return midi_ch_now ? (uint32_t)midi_ch_now - 1u : 0u; }
static void midi_note(uint32_t note, uint32_t vel)
{
    plat_midi_send(0x09u | (0x90u | out_ch()) << 8 | note << 16 | vel << 24, TO_USB);
}
static void midi_off(uint32_t note) { plat_midi_send(0x08u | (0x80u | out_ch()) << 8 | note << 16, TO_USB); }
static void midi_rt(uint32_t st) { plat_midi_send(0x0Fu | st << 8, TO_USB); }   /* clock, start, stop */

/* the arp's MIDI out: each note it sent and the sample clock its note-off is due at */
static int16_t out_note[NHELD];
static uint32_t out_end[NHELD];
static uint16_t arp_note_chs[128];       /* each pitch the arp last played: the MIDI channels its root came in on (its
                                          * voice's, through its release: All Sound Off stops that channel's only) */
static int16_t out_root[NHELD];          /* the held note it came from (a chord's root, a tone's), and the MIDI */
static uint16_t out_chs[NHELD];           /* channels holding that then: a panic stops only its channels' */
static uint32_t rend_i;                  /* the render's place in the block: where an event falls */
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

/* the factory sound playing, or the one a user sound began from */
static const preset_t *cur_preset(void)
{
    if (set.preset < NPRESET)
        return &PRESET[set.preset];
    return &PRESET[set.preset - NPRESET < NUSER && users.u[set.preset - NPRESET].base < NPRESET
                   ? users.u[set.preset - NPRESET].base : 0];
}

/* a USER preset's name, given when it is saved: an animal and a hairdo, "FROG MULLET" (at most 4 + 1 + 6 letters:
 * with "U03 " it fits the screen) */
static const char *const NAME_A[16] = {"LION", "FROG", "CRAB", "OWL", "YAK", "EMU", "BEAR", "WOLF",
                                       "MOLE", "NEWT", "TOAD", "DUCK", "GOAT", "SEAL", "MOTH", "HARE"};
static const char *const NAME_B[16] = {"AFRO", "MULLET", "MOHAWK", "BOB", "BUN", "PERM", "FADE", "BANGS",
                                       "MOPTOP", "BOWL", "BRAIDS", "BUZZ", "SHAG", "CURLS", "DREADS", "SPIKES"};
static void text_cat(char *d, const char *s);
static void user_name(uint32_t name, char *t)   /* (appended to t) */
{
    text_cat(t, NAME_A[name >> 4 & 15u]);
    text_cat(t, " ");
    text_cat(t, NAME_B[name & 15u]);
}

static void sound_name(uint32_t n, char *t)   /* sound n's name: a factory sound's, or "U03 FROG MULLET" */
{
    const char *b;
    uint32_t i = 0;
    if (n < NPRESET) {
        for (b = PRESET[n].name; *b && i < 15u; b++)
            t[i++] = *b;
    } else {
        uint32_t u = n - NPRESET + 1u;
        t[i++] = 'U';
        t[i++] = (char)('0' + u / 10u);
        t[i++] = (char)('0' + u % 10u);
        t[i++] = ' ';
        t[i] = 0;
        if (n - NPRESET < NUSER && users.u[n - NPRESET].used && users.u[n - NPRESET].name) {
            user_name(users.u[n - NPRESET].name, t);
            return;
        }
        for (b = n - NPRESET < NUSER && users.u[n - NPRESET].used ? PRESET[users.u[n - NPRESET].base].name : "EMPTY";
             *b && i < 15u; b++)
            t[i++] = *b;
    }
    t[i] = 0;
}

/* where room is short (the header, the SOUND knob): a named USER preset's name alone, "FROG MULLET" (the list and
 * SAVE TO show its slot) */
static void sound_title(uint32_t n, char *t)
{
    uint32_t i;
    sound_name(n, t);
    if (n >= NPRESET && n - NPRESET < NUSER && users.u[n - NPRESET].used && users.u[n - NPRESET].name)
        for (i = 0; (t[i] = t[i + 4u]) != 0; i++)
            ;
}

static uint32_t sound_cat(uint32_t n) { return n < NPRESET ? PRESET[n].cat : CAT_USER; }
static int sound_ok(uint32_t n) { return n < NPRESET || (n - NPRESET < NUSER && users.u[n - NPRESET].used); }

static void held_add(int32_t note, uint32_t vel)
{
    uint32_t i, j;
    for (i = 0; i < nheld; i++)
        if (held[i] == note) {
            held_vel[i] = (uint8_t)vel;
            held_src[i] |= held_by;
            held_ch[i] |= held_by_ch;
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
        held_src[j] = held_src[j - 1];
        held_ch[j] = held_ch[j - 1];
    }
    held[i] = note;
    held_vel[i] = (uint8_t)vel;
    held_seq[i] = ++held_seq_n;
    held_src[i] = held_by;
    held_ch[i] = held_by_ch;
    nheld++;
    if (!arp_on_now) {
        eng_note_on(note, vel, -1);
        midi_note((uint32_t)note, vel);
    }
}

static void arp_cancel(void);
static int held_has(int32_t note)         /* held now: in the arp's notes */
{
    uint32_t i;
    for (i = 0; i < nheld; i++)
        if (held[i] == note)
            return 1;
    return 0;
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
        held_src[i] = held_src[i + 1];
        held_ch[i] = held_ch[i + 1];
    }
    nheld--;
    if (!nheld)                          /* nothing held: what the arp had queued (a strum's notes, a late step) goes */
        arp_cancel();
    if (!arp_on_now) {
        if (set.p[P_VOICES] != V_POLY && nheld && eng_sounding(note))   /* mono: back to the top note still held */
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
static int32_t swing_wait, hum_wait;     /* samples to a swung step, and to a step HUMANIZE made late */
static int32_t hum_gate;
static int32_t arp_s = -1, arp_k;        /* steps since the pattern began; WALK's place */
static int32_t st_note[NHELD];           /* the step's notes (one, or CHORD's), for HUMANIZE to play late */
static int32_t st_root[NHELD];           /* and the held note each came from */
static uint8_t st_vel[NHELD];
static uint32_t st_n;
static volatile int32_t last_played = -1;
static uint32_t arp_mark_at;             /* arp_count at the host tests' arp_mark */
static uint8_t played_seq[16];
static uint8_t ext_run = 1;              /* EXT: Stop holds the clock, Start and Continue run it */
static uint32_t ext_last, ext_sc0, ext_cnt;   /* the last tick's sample clock; the period's measure */
static float ext_per = 44100.0f * 60.0f / (120.0f * 24.0f);   /* samples a tick, measured */
static volatile uint32_t ext_bpm_x10;    /* to the screen: the tempo coming in, 0 = none */

static float tick_samples(void)          /* how long a tick is now */
{
    return sync_now ? ext_per : SR * 60.0f / ((float)set.p[P_TEMPO] * 24.0f);
}
static float step_samples(void) { return tick_samples() * (float)DIV_TICKS[set.p[P_RATE]]; }

static void play(int32_t note, uint32_t vel, int32_t gate, int32_t root)   /* one of the arp's notes: the engine,
                                                                          * MIDI, the screen (root: the held note) */
{
    uint32_t i, now = sample_clock + rend_i, old = 0, h;
    eng_note_on(note, vel, gate);
    for (i = 0; i < NHELD; i++)          /* its MIDI: the same note sounding, and those whose time is up, ended first */
        if (out_note[i] >= 0 && (out_note[i] == note || (int32_t)(now - out_end[i]) >= 0)) {
            midi_off((uint32_t)out_note[i]);
            out_note[i] = -1;
        }
    for (i = 0; i < NHELD && out_note[i] >= 0; i++)
        ;
    if (i == NHELD) {                    /* all 16 sounding: the one due soonest ends now, so MIDI plays what we do */
        for (i = 1; i < NHELD; i++)
            if ((int32_t)(out_end[i] - out_end[old]) < 0)
                old = i;
        midi_off((uint32_t)out_note[old]);
        i = old;
    }
    out_note[i] = (int16_t)note;
    out_end[i] = now + (uint32_t)gate;
    out_root[i] = (int16_t)root;
    for (h = 0; h < nheld && held[h] != root; h++)
        ;
    out_chs[i] = h < nheld && held_src[h] & DN_MIDI ? held_ch[h] : 0u;
    arp_note_chs[note & 127] = out_chs[i];
    midi_note((uint32_t)note, vel);
    viz[viz_w % VIZ_N] = (uint32_t)note | vel << 8 | (sample_clock / 441u) << 16;
    viz_w = viz_w + 1u;
    if (arp_count - arp_mark_at < 16u)   /* (the host's tests: the notes since arp_mark, in order) */
        played_seq[arp_count - arp_mark_at] = (uint8_t)note;
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

/* 0 .. m-1 and back, no repeats at the ends: step j */
static uint32_t updn(uint32_t j, uint32_t m)
{
    uint32_t P, t;
    if (m < 2u)
        return 0;
    P = 2u * m - 2u;
    t = j % P;
    return t < m ? t : P - t;
}

/* the place in the run (the held notes low to high, then an octave up, ...) that step s of the pattern plays */
static uint32_t pattern_place(uint32_t s, uint32_t L)
{
    uint32_t t;
    switch (set.p[P_MODE]) {
    case M_DOWN:
        return L - 1u - s % L;
    case M_UPDN:                         /* no repeats at the ends */
        return updn(s, L);
    case M_CONVERGE:                     /* the outside in: low, high, next low, next high */
        t = s % L;
        return t & 1u ? L - 1u - t / 2u : t / 2u;
    case M_DIVERGE:                      /* the inside out */
        t = L - 1u - s % L;
        return t & 1u ? L - 1u - t / 2u : t / 2u;
    case M_THUMB:                        /* the lowest between each of the others, they going up */
        if (L < 2u)
            return 0;
        t = s % (2u * L - 2u);
        return t & 1u ? 1u + t / 2u : 0;
    case M_THUMB_UD:                     /* the same, they going up and down */
        if (L < 2u)
            return 0;
        t = s % (2u * (L > 2u ? 2u * L - 4u : 1u));
        return t & 1u ? 1u + updn(t / 2u, L - 1u) : 0;
    case M_PINKY:                        /* the highest between each of the others, they going up */
        if (L < 2u)
            return 0;
        t = s % (2u * L - 2u);
        return t & 1u ? t / 2u : L - 1u;
    case M_PINKY_UD:                     /* the same, they going up and down */
        if (L < 2u)
            return 0;
        t = s % (2u * (L > 2u ? 2u * L - 4u : 1u));
        return t & 1u ? updn(t / 2u, L - 1u) : L - 1u;
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

/* the diatonic chord on root, snapped down into the key's scale (so major, minor or diminished as the scale makes it):
 * low to high in out, its count returned. TRIAD 1 3 5; 7TH 1 3 5 7; LUSH, OPEN, OMNI Nick's voicings where his pad
 * had them for the scale (LUSH: MAJOR, MINOR, HARM MINOR; OPEN, OMNI: MAJOR), else stacked thirds spread out */
/* a chord kept in MIDI's range: down (or up) by octaves as a whole, its intervals kept; then any notes still
 * outside clamped, and repeats dropped (low to high) */
static uint32_t chord_fit(int32_t *out, uint32_t n)
{
    uint32_t i, j, m = 0;
    int32_t lo = 127, hi = 0;
    for (i = 0; i < n; i++) {
        lo = out[i] < lo ? out[i] : lo;
        hi = out[i] > hi ? out[i] : hi;
    }
    for (; hi > 127 && lo >= 12; hi -= 12, lo -= 12)
        for (i = 0; i < n; i++)
            out[i] -= 12;
    for (; lo < 0 && hi <= 115; hi += 12, lo += 12)
        for (i = 0; i < n; i++)
            out[i] += 12;
    for (i = 0; i < n; i++) {
        int32_t x = clamp(out[i], 0, 127);
        for (j = 0; j < m && out[j] != x; j++)
            ;
        if (j == m)
            out[m++] = x;
    }
    return m;
}

static uint32_t chord_of(int32_t root, uint32_t kind, int32_t *out)
{
    const int8_t *nv = 0;
    uint32_t nn = 5;
    const uint8_t *sc = SCALE_STEPS[set.p[P_SCALE] % NSCALE];
    int32_t rel = ((root - set.p[P_KEY]) % 12 + 12) % 12, deg = 0, base, d;
    uint32_t n = 0, i;
    for (d = 6; d >= 0; d--)
        if (sc[d] <= rel) {
            deg = d;
            break;
        }
    base = root - (rel - sc[deg]);
    {                                    /* Nick's, where there are some: from the key's root at this octave */
        uint32_t scale = (uint32_t)set.p[P_SCALE] % NSCALE;
        if (kind == CH_LUSH)
            nv = scale == 0 ? LUSH_MAJ[deg] : scale == 1 ? LUSH_MIN[deg] : scale == 4 ? LUSH_HARM[deg] : 0;
        else if (kind == CH_OPEN && scale == 0)
            nv = OPEN_MAJ[deg];
        else if (kind == CH_OMNI_T && scale == 0) {
            nv = OMNI_MAJ[deg];
            nn = 8;
        }
    }
    if (nv) {
        int32_t key_root = base - sc[deg];
        for (i = 0; i < nn; i++)
            if (nv[i] != NV)
                out[n++] = key_root + nv[i];
        return chord_fit(out, n);
    }
#define STEP(k) (base - sc[deg] + sc[(deg + (k)) % 7] + 12 * ((deg + (k)) / 7))
    if (kind == CH_OMNI_T) {             /* eight, spread: the bass, then 1 3 5 over two octaves, the top root */
        out[n++] = base - 12;
        out[n++] = base;
        out[n++] = STEP(2);
        out[n++] = STEP(4);
        out[n++] = base + 12;
        out[n++] = STEP(9);
        out[n++] = STEP(11);
        out[n++] = base + 24;
    } else if (kind == CH_LUSH || kind == CH_OPEN) {
        out[n++] = base - 12;
        out[n++] = STEP(4);
        out[n++] = base + 12;
        out[n++] = STEP(8);
        out[n++] = STEP(9);
    } else {
        out[n++] = base;
        out[n++] = STEP(2);
        out[n++] = STEP(4);
        if (kind == CH_7TH || kind == CH_7TH_T)
            out[n++] = STEP(6);
    }
#undef STEP
    return chord_fit(out, n);
}

/* the strum: a chord's notes after its first, STRUM apart (st_i the next, strum_wait the samples to it) */
static uint32_t st_i;
static int32_t strum_wait;
static uint32_t step_at;                 /* the sample the step fell on the grid (its swing and HUMANIZE after) */
static float st_room;                    /* from the chord's first note to the next step: its notes fit in this */
/* STRUM's time between notes: 0 off, else 2 ms .. 250 ms (exponential), but never so long the chord would not finish
 * before the next step (all its notes within 90 % of the time left to it, a swung or late step's less) */
static float strum_ms(int32_t v) { return v ? 2.0f * fm_exp2f((float)v * 0.0696578f) : 0.0f; }   /* 125^(v/100) */
static int32_t strum_samples(void)
{
    float t = strum_ms(set.p[P_STRUM]) * 0.001f * SR;
    if (st_n > 1u)
        t = fm_minf(t, st_room * 0.9f / (float)(st_n - 1u));
    return (int32_t)t;
}

static void chord_play(void)             /* the step's notes: all at once, or the first now and the rest strummed */
{
    uint32_t i;
    st_room = step_samples() - (float)(int32_t)(sample_clock + rend_i - step_at);
    if (st_n > 1u && strum_samples() > 0) {
        play(st_note[0], st_vel[0], hum_gate, st_root[0]);
        st_i = 1;
        strum_wait = strum_samples();
        return;
    }
    for (i = 0; i < st_n; i++)
        play(st_note[i], st_vel[i], hum_gate, st_root[i]);
}

static void arp_cancel(void) { swing_wait = hum_wait = strum_wait = 0; }   /* what the arp had queued: gone */

static void strummed(void)               /* the strum's next note (STRUM turned off meanwhile: the rest now) */
{
    if (st_i < st_n)
        play(st_note[st_i], st_vel[st_i], hum_gate, st_root[st_i]);
    if (++st_i < st_n) {
        strum_wait = strum_samples();
        if (strum_wait <= 0)
            for (; st_i < st_n; st_i++)
                play(st_note[st_i], st_vel[st_i], hum_gate, st_root[st_i]);
    }
}

static void arp_step(void)
{
    static int32_t pool[NHELD * 8];      /* CHORDS ..TONES: the held notes' chords' tones, low to high */
    static uint8_t pool_vel[NHELD * 8];
    static int32_t pool_root[NHELD * 8];  /* the held note whose chord it is */
    const int32_t *hn = held;
    const uint8_t *hv = held_vel;
    uint32_t n = nheld, oct = (uint32_t)set.p[P_OCTAVES], s, i, k, idx, kind = (uint32_t)set.p[P_CHORD];
    int32_t gate, vr, j;
    float step = step_samples();
    if (!n)
        return;
    strum_wait = 0;
    if (CH_TONES(kind)) {                /* the chords' tones the notes to play from */
        uint32_t m = 0, h, c;
        int32_t t[8];
        for (h = 0; h < nheld; h++) {
            c = chord_of(held[h], kind, t);
            for (i = 0; i < c; i++) {
                uint32_t at;
                for (at = 0; at < m && pool[at] < t[i]; at++)
                    ;
                if (at < m && pool[at] == t[i])
                    continue;
                for (k = m; k > at; k--) {
                    pool[k] = pool[k - 1];
                    pool_vel[k] = pool_vel[k - 1];
                    pool_root[k] = pool_root[k - 1];
                }
                pool[at] = t[i];
                pool_vel[at] = held_vel[h];
                pool_root[at] = held[h];
                m++;
            }
        }
        hn = pool;
        hv = pool_vel;
        n = m;
    }
    s = (uint32_t)++arp_s;               /* the pattern moves every step, played or not */
    if (set.p[P_MODE] == M_CHORD) {      /* CHORD: every held note at once, an octave higher each step */
        for (i = 0; i < n && i < NHELD; i++) {
            st_note[i] = hn[i] + 12 * (int32_t)(s % oct);
            st_vel[i] = hv[i];
            st_root[i] = hn == pool ? pool_root[i] : hn[i];
        }
        st_n = i;
    } else {
        k = pattern_place(s, n * oct);
        idx = set.p[P_MODE] == M_PLAYED && hn == held ? played_index(k % n, n) : k % n;
        st_note[0] = hn[idx] + 12 * (int32_t)(k / n);
        st_vel[0] = hv[idx];
        st_root[0] = hn == pool ? pool_root[idx] : hn[idx];
        st_n = 1;
        if (kind != CH_OFF && !CH_TONES(kind)) {   /* the note a chord's root */
            int32_t t[8];
            uint32_t c = chord_of(st_note[0], kind, t);   /* (an octave above 127: chord_fit brings it down) */
            for (i = 0; i < c; i++) {
                st_note[i] = t[i];
                st_vel[i] = st_vel[0];
                st_root[i] = st_root[0];
            }
            st_n = c;
        }
    }
    for (i = 0; i < st_n; i++)
        while (st_note[i] > 127)
            st_note[i] -= 12;
    hum_wait = 0;
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
    hum_gate = gate < 64 ? 64 : gate;
    if (set.p[P_HUMAN]) {                /* HUMANIZE: the step a little late, at random: up to 40 ms (half a step) */
        uint32_t most = (uint32_t)fm_minf((float)set.p[P_HUMAN] * 17.64f, step * 0.5f);
        hum_wait = (int32_t)rnd_n(most + 1u);
        if (hum_wait > 0)
            return;
    }
    chord_play();
}

static void humanized(void)              /* the step HUMANIZE held back, now */
{
    chord_play();
}

static void on_tick(void)                /* a clock tick: MIDI out, and a step where one falls */
{
    uint32_t L = DIV_TICKS[set.p[P_RATE]];
    tick_n = (tick_n + 1u) % CLK_WRAP;
    if (clk_out_now && !sync_now && plat_midi_out_room(TO_USB))
        midi_rt(0xF8u);
    if (!arp_on_now || !nheld || tick_n % L)
        return;
    step_at = sample_clock + rend_i;
    if ((tick_n / L) & 1u && set.p[P_SWING]) {   /* SWING: every second step late, up to half a step */
        int32_t w = (int32_t)(step_samples() * (float)set.p[P_SWING] * 0.005f);
        swing_wait = w < 1 ? 1 : w;
    } else
        arp_step();
}

static void arp_restart(void)            /* a new chord: the pattern from its top, its first note on the grid's next
                                         * step (the clock never restarts: every note in time, as Mighty Synth plays) */
{
    swing_wait = hum_wait = strum_wait = 0;
    arp_s = -1;
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

static void note_down(int32_t note, uint32_t vel, uint32_t by, uint32_t ch)   /* by: DN_KEY, or DN_MIDI on ch */
{
    uint32_t i, was;
    if (note < 0 || note > 127)
        return;
    vel = vel_curve(vel);
    if (hold_now == H_HOLD && ndown == 0)   /* HOLD: a new chord after every key was let go replaces the last */
        held_clear();
    was = nheld;
    if (!is_down(note))
        ndown++;
    if (by == DN_MIDI) {                 /* the note's sound now this channel's (a channel already holding it shares
                                          * it; one that let go no longer owns its new tail) */
        midi_played[note] = (uint16_t)((midi_down[note] ? midi_played[note] : 0u) | 1u << (ch & 15u));
        midi_down[note] |= (uint16_t)(1u << (ch & 15u));
    } else {
        midi_played[note] = 0;           /* a key's now */
        down[note] |= DN_KEY;
    }
    held_by = (uint8_t)by;
    held_by_ch = (uint16_t)(by == DN_MIDI ? 1u << (ch & 15u) : 0u);
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
    if (was == 0 && nheld && arp_on_now)   /* a new chord: the pattern from its top, on the grid */
        arp_restart();
}

static void note_up(int32_t note, uint32_t by, uint32_t ch)   /* let go by one of what held it; another may still */
{
    if (note < 0 || note > 127)
        return;
    if (by == DN_MIDI) {
        if (!(midi_down[note] >> (ch & 15u) & 1u))
            return;
        midi_down[note] &= (uint16_t)~(1u << (ch & 15u));
        if (hold_now == H_OFF) {         /* no hold: a channel owns a note only while it holds it down */
            uint32_t i;
            for (i = 0; i < nheld; i++)
                if (held[i] == note)
                    held_ch[i] &= (uint16_t)~(1u << (ch & 15u));
        }
    } else {
        if (!(down[note] & DN_KEY))
            return;
        down[note] = 0;
    }
    if (is_down(note))
        return;
    ndown--;
    if (hold_now == H_OFF)
        held_remove(note);
    held_publish();
}

/* MIDI's notes on chans let go, those HOLD or LATCH kept too (All Notes Off and All Sound Off: their own channel;
 * MIDI CH changed: every one); a key's notes, and other channels', stay. sound_off: and quickly silent */
static void midi_let_go(uint32_t chans, int sound_off)   /* chans: the channels (a bit each; 0xFFFF all) */
{
    uint32_t i;
    for (i = 0; i < 128u; i++)
        if (midi_down[i] & chans) {
            midi_down[i] &= (uint16_t)~chans;
            if (!is_down((int32_t)i))
                ndown--;
        }
    for (i = nheld; i-- > 0;) {          /* MIDI's held notes from those channels, that nothing else holds now */
        int32_t note = held[i];
        if (!(held_src[i] & DN_MIDI) || !(held_ch[i] & chans))
            continue;
        held_ch[i] &= (uint16_t)~chans;  /* those channels' hold on it gone; another's, or a key's, keeps it (with
                                          * no hold, only one holding it down) */
        if ((hold_now == H_OFF || !held_ch[i]) && !is_down(note)) {
            held_remove(note);
            if (sound_off)               /* All Sound Off: out fast, not into the release */
                eng_note_kill(note);
        }
    }
    held_publish();
    if (arp_on_now) {                    /* the arp's notes from what these channels let go (each note knows the held
                                          * note it came from): those queued never play, those sounding stop. Another
                                          * channel's, or a key's, play on */
        uint32_t from = hum_wait > 0 ? 0u : st_i, k = from;   /* (a late step: all its notes still to come) */
        if (hum_wait > 0 || strum_wait > 0) {
            for (i = from; i < st_n; i++)
                if (held_has(st_root[i])) {
                    st_note[k] = st_note[i];
                    st_vel[k] = st_vel[i];
                    st_root[k] = st_root[i];
                    k++;
                }
            st_n = k;
            if (st_n <= st_i)
                strum_wait = 0;
            if (!st_n)
                hum_wait = 0;
        }
        for (i = 0; i < NHELD; i++)
            if (out_note[i] >= 0 && out_chs[i] & chans && !held_has(out_root[i])) {
                if (sound_off)
                    eng_note_kill(out_note[i]);
                else
                    eng_note_release(out_note[i]);   /* (its gate aside: the arp's notes have one) */
                midi_off((uint32_t)out_note[i]);
                out_note[i] = -1;
            }
        if (sound_off)                   /* All Sound Off: the voices of the pitches this channel's notes last made
                                          * the arp play, the earlier steps' releases too (another channel's own play
                                          * on: the pitch is theirs once they play it) */
            for (i = 0; i < 128u; i++)
                if (arp_note_chs[i] & chans) {   /* (this channel's claim gone; another still holding it: on) */
                    arp_note_chs[i] &= (uint16_t)~chans;
                    if (!arp_note_chs[i])
                        eng_note_kill((int32_t)i);
                }
    }
    for (i = 0; i < 128u; i++)           /* their notes no longer held, a release still ringing too */
        if (midi_played[i] & chans) {
            midi_played[i] &= (uint16_t)~chans;
            if (sound_off && !is_down((int32_t)i)) {
                uint32_t h;
                for (h = 0; h < nheld && held[h] != (int32_t)i; h++)
                    ;
                if (h == nheld)
                    eng_note_kill((int32_t)i);
            }
        }
}

/* the arp, hold, CLOCK, MIDI CH or VOICES switched: what sounds follows */
static void modes_follow(void)
{
    uint32_t i;
    if ((uint8_t)set.p[P_VOICES] != voices_now) {   /* VOICES: what sounds now goes into its release, and the keys
                                                     * held sound again as the new mode plays them: all (POLY), the
                                                     * top one (MONO, LEGATO) */
        voices_now = (uint8_t)set.p[P_VOICES];
        eng_release_all();
        if (!arp_on_now && nheld) {
            if (voices_now == V_POLY)
                for (i = 0; i < nheld; i++)
                    eng_note_on(held[i], held_vel[i], -1);
            else
                eng_note_on(held[nheld - 1], held_vel[nheld - 1], -1);
        }
    }
    if ((uint8_t)set.p[P_MIDICH] != midi_ch_now) {   /* MIDI CH: what went out on the old channel ends there; what
                                                      * came in on it is let go (its offs would no longer be heard) */
        midi_let_go(0xFFFFu, 0);
        if (!arp_on_now)
            for (i = 0; i < nheld; i++)
                midi_off((uint32_t)held[i]);
        out_all_off();
        midi_ch_now = (uint8_t)set.p[P_MIDICH];
        if (!arp_on_now)
            for (i = 0; i < nheld; i++)
                midi_note((uint32_t)held[i], held_vel[i]);
    }
    if (set.arp_on != arp_on_now) {
        arp_on_now = set.arp_on;
        eng_release_all();               /* every note let go into its release */
        out_all_off();
        swing_wait = hum_wait = strum_wait = 0;
        if (!arp_on_now)                 /* the arp stopped: the notes held sound again */
            for (i = 0; i < nheld; i++) {
                eng_note_on(held[i], held_vel[i], -1);
                midi_note((uint32_t)held[i], held_vel[i]);
            }
        else {
            for (i = 0; i < nheld; i++)
                midi_off((uint32_t)held[i]);
            if (nheld)
                arp_restart();
        }
    }
    if (set.hold != hold_now) {
        hold_now = set.hold;
        if (hold_now == H_OFF) {         /* hold off: what no key holds goes */
            for (i = nheld; i-- > 0;)
                if (!is_down(held[i]))
                    held_remove(held[i]);
            held_publish();
        }
    }
    if ((uint8_t)(set.p[P_CLOCK] == 2) != sync_now) {
        sync_now = (uint8_t)(set.p[P_CLOCK] == 2);
        ext_run = 1;
        swing_wait = hum_wait = strum_wait = 0;
    }
    if ((uint8_t)(set.p[P_CLOCK] == 1) != clk_out_now) {
        clk_out_now = (uint8_t)(set.p[P_CLOCK] == 1);
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
    uint32_t pkt, ch_want = midi_ch_now ? (uint32_t)midi_ch_now - 1u : 0xFFu;
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
                swing_wait = hum_wait = strum_wait = 0;
            }
            continue;
        }
        if (st < 0x80u || st >= 0xF0u || (ch_want != 0xFFu && (st & 0x0Fu) != ch_want))
            continue;
        if (ty == 0x90u && d2) {
            note_down((int32_t)d1, d2, DN_MIDI, st & 0x0Fu);
            midi_count++;
        } else if (ty == 0x80u || ty == 0x90u) {
            note_up((int32_t)d1, DN_MIDI, st & 0x0Fu);
        } else if (ty == 0xB0u && d1 == 1u) {
            modwheel = (float)d2 / 127.0f;
        } else if (ty == 0xE0u) {
            bend = (float)((int32_t)(d2 << 7 | d1) - 8192) * (2.0f / 8192.0f);
        } else if (ty == 0xB0u && (d1 == 123u || d1 == 120u)) {   /* All Notes Off, All Sound Off (that silent now):
                                                                 * MIDI's notes, kept or not */
            midi_let_go(1u << (st & 0x0Fu), d1 == 120u);
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
            note_down((int32_t)note, e >> 16 & 0xFFu, DN_KEY, 0);
            break;
        case KEV_OFF:
            note_up((int32_t)note, DN_KEY, 0);
            break;
        case KEV_ALLOFF:                  /* the octave changed: the keys' notes would not find their offs (MIDI's
                                           * stay) */
            for (note = 0; note < 128u; note++)
                note_up((int32_t)note, DN_KEY, 0);
            break;
        }
    }
}

/* ----------------------------------------------------------------- render --- */
static float buf_l[ENG_BLOCK], buf_r[ENG_BLOCK];
/* the output's last samples, for HOME's oscilloscope with the arp off (one store a sample here; the screen reads it) */
#define SCOPE_N 1024u
static int16_t scope[SCOPE_N];
static volatile uint32_t scope_w;
static volatile uint32_t dsp_resets;     /* how often the guard had to clear (GLOBAL shows it; the tests read it) */
static volatile uint32_t sheds;          /* how often the render ran late and a voice went (GLOBAL shows it) */
static uint32_t cap_back_at;             /* the sample clock at which a voice shed comes back */

static void engine_params(uint32_t n)    /* the knobs, as the engine takes them */
{
    static eng_sound_t snd;              /* the sound with OP 1-4's settings on it */
    const preset_t *pr = cur_preset();
    eng_params_t ep;
    uint32_t k;
    snd = pr->s;
    for (k = 0; k < 4u; k++) {
        snd.op[k].ratio = OP_RATIO[set.op_ratio[k] < N_OPRATIO ? set.op_ratio[k] : 1u];
        snd.op[k].lvl = (float)set.op_level[k] / 100.0f;
        snd.op[k].wave = set.op_wave[k];
        snd.op[k].dec = knob_secs(set.op_decay[k]);
    }
    snd.mono = (uint8_t)set.p[P_VOICES];   /* (engine.h: 0 poly, 1 mono, 2 legato) */
    snd.glide = glide_secs(set.p[P_GLIDE]);
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
    ep.wave = 0xFFu;                     /* each operator its own (MOD WAVE sets the modulators', op_wave) */
    ep.ratio = RATIO_MUL[set.p[P_RATIO]];
    ep.lfo_hz = lfo_hz(set.p[P_LRATE]);
    ep.lfo_shape = (uint8_t)set.p[P_LSHAPE];
    ep.vib = v * v + modwheel * 0.5f;    /* up to a semitone; the mod wheel adds half */
    ep.trem = (float)set.p[P_TREM] / 100.0f;
    ep.bend = bend;
    ep.chorus = (float)set.p[P_CHORUS] / 100.0f;
    ep.chorus_hz = 0.1f * fm_exp2f((float)set.p[P_CRATE] * 0.0564386f);   /* 0.1 .. 5 Hz */
    ep.chorus_depth = 20.0f + (float)set.p[P_CDEPTH] * 2.3f;             /* samples: 0.5 .. 5.7 ms */
    ep.dly_samples = beat * DDIV_BEATS[set.p[P_DTIME]];
    while (ep.dly_samples > ENG_DLY_MAX)    /* longer than the line at a slow tempo: half, still in time */
        ep.dly_samples *= 0.5f;
    ep.dly_fb = (float)set.p[P_DFDBK] / 100.0f;
    ep.dly_mix = (float)set.p[P_DMIX] / 100.0f;
    ep.dly_width = (float)set.p[P_WIDTH] / 100.0f;
    ep.rev_mix = (float)set.p[P_RMIX] / 100.0f * 0.6f;
    ep.rev_secs = rev_secs(set.p[P_RLEN]);
    ep.rev_dark = (float)set.p[P_RDARK] / 100.0f;
    ep.shimmer = (float)set.p[P_SHIMMER] / 100.0f;
    ep.filter = (uint8_t)(set.p[P_CUTOFF] < 100 || set.p[P_FENV] || set.p[P_LFOCUT]);   /* open and still: off */
    ep.cut = 4.9069f + (float)set.p[P_CUTOFF] * 0.0923f;                 /* log2 Hz */
    ep.res = (float)set.p[P_RES] / 100.0f * 0.95f;
    ep.env = (float)set.p[P_FENV] / 100.0f * 8.0f;                       /* up to 8 octaves */
    ep.fatt = knob_secs(set.p[P_FATT]) * (set.p[P_FATT] > 0);
    ep.fdec = knob_secs(set.p[P_FDEC]);
    ep.fsus = (float)set.p[P_FSUS] / 100.0f;
    ep.frel = knob_secs(set.p[P_FREL]);
    ep.keytrack = (float)set.p[P_KEYTRK] / 100.0f;
    ep.lfo_cut = (float)set.p[P_LFOCUT] / 100.0f * 3.0f;                 /* up to 3 octaves */
    ep.pwm = (float)set.p[P_PWM] / 100.0f;
    eng_set(&snd, &ep, n);
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i, k, seg = 0;
    float gain = (float)master_q12 / 4096.0f * 8300000.0f, tps;   /* MASTER, read once a block */
    if (n > ENG_BLOCK)
        n = ENG_BLOCK;
    engine_params(n);
    rend_i = 0;                          /* (what comes in comes at the block's start) */
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
        if (hum_wait > 0 && --hum_wait == 0)
            ev |= 4u;
        if (strum_wait > 0 && --strum_wait == 0)
            ev |= 8u;
        if (!ev)
            continue;
        eng_voices(buf_l, buf_r, seg, i);
        seg = i;
        rend_i = i;
        /* what an earlier step queued first (its strum's next note, its late start, its swing), then the clock: a new
         * step may replace the step's notes, and an old timer must never play the new ones */
        if (ev & 8u && arp_on_now)
            strummed();
        if (ev & 4u && arp_on_now && nheld)
            humanized();
        if (ev & 2u && arp_on_now)
            arp_step();
        if (ev & 1u)
            on_tick();
    }
    eng_voices(buf_l, buf_r, seg, n);
    if (eng_out(buf_l, buf_r, lr, n, gain))   /* a bad number: silence, then everything starts clean */
        dsp_resets = dsp_resets + 1u;
    for (i = 0, k = scope_w; i < n; i++, k++)   /* the scope: left and right summed, 16 bits */
        scope[k & (SCOPE_N - 1u)] = (int16_t)((lr[2 * i] + lr[2 * i + 1]) >> 9);
    scope_w = k;
    if (eng_cap(0) < ENG_NVOICE && sample_clock - cap_back_at < 0x80000000u) {   /* 2 s without running late: a
                                                                                  * voice back */
        eng_cap(1);
        cap_back_at = sample_clock + (uint32_t)(2.0f * SR);
    }
    for (k = 0; k < NHELD; k++)          /* the arp's note-offs, to the block */
        if (out_note[k] >= 0 && (int32_t)(sample_clock + n - out_end[k]) >= 0) {
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
static void ops_load(const preset_t *pr) /* OP 1-4 as the sound has them */
{
    uint32_t k, i, best;
    for (k = 0; k < 4u; k++) {
        const eng_op_t *o = &pr->s.op[k];
        for (i = 1, best = 0; i < N_OPRATIO; i++)
            if (fm_fabsf(OP_RATIO[i] - o->ratio) < fm_fabsf(OP_RATIO[best] - o->ratio))
                best = i;
        set.op_ratio[k] = (uint8_t)best;
        set.op_level[k] = (uint8_t)clamp((int32_t)(o->lvl * 100.0f + 0.5f), 0, 100);
        set.op_wave[k] = (uint8_t)(o->wave < NWAVE ? o->wave : 0u);
        set.op_decay[k] = (uint8_t)clamp((int32_t)(7.5257f * fm_log2f(fm_maxf(o->dec, 0.001f) * 1000.0f) + 0.5f),
                                         0, 100);   /* knob_secs backwards: 25 log10(ms) */
    }
}

/* the filter's own envelope as it was before it had one: the amp's attack, no sustain, a release as long as its decay,
 * half the key (the sounds, and the saves from before, so: they sound as they did) */
static void fenv_fill(int16_t *p)
{
    p[P_FATT] = p[P_ATTACK];
    p[P_FSUS] = 0;
    p[P_FREL] = p[P_FDEC];
    p[P_KEYTRK] = 50;
}

static void filter_load(const preset_t *pr)   /* FILTER as the sound has it (open, unless it says), LFO 2 still */
{
    set.p[P_CUTOFF] = pr->f_on ? pr->fcut : 100;
    set.p[P_RES] = pr->f_on ? pr->fres : 0;
    set.p[P_FENV] = pr->f_on ? pr->fenv : 0;
    set.p[P_FDEC] = pr->f_on ? pr->fdec : PAR[P_FDEC].def;
    set.p[P_PWM] = 0;
    set.p[P_LFOCUT] = pr->f_on ? pr->lfocut : 0;
    fenv_fill(set.p);
}

static void voice_load(const preset_t *pr)   /* VOICE as the sound has it (MONO, LEGATO or POLY); and FILTER */
{
    filter_load(pr);
    set.p[P_VOICES] = pr->s.mono == 2u ? V_LEGATO : pr->s.mono ? V_MONO : V_POLY;   /* (basses MONO, leads LEGATO) */
    set.p[P_GLIDE] = (int16_t)(pr->s.glide > 0.0f
                               ? clamp((int32_t)(10.0343f * fm_log2f(pr->s.glide / 0.002f) + 0.5f), 1, 100) : 0);
}

static int sound_par(uint32_t p)         /* one of the sound's knobs (SOUND_P) */
{
    uint32_t i;
    for (i = 0; i < NSOUNDP; i++)
        if (SOUND_P[i] == p)
            return 1;
    return 0;
}
static int patch_par(uint32_t p)         /* one a USER preset loads: not the player's own */
{
    return p != P_CLOCK && p != P_MIDICH && p != P_VCURVE && p != P_LOADS;
}
static void patch_defaults(void)         /* the arp, SEQ, CHORDS, CHANCE and the effects as new (not the sound, TEMPO) */
{
    uint32_t i;
    arp_cancel();                        /* a queued strum or late step: the old settings', never played under the new */
    for (i = 0; i < NP; i++)
        if (patch_par(i) && !sound_par(i) && i != P_TEMPO)
            set.p[i] = PAR[i].def;
}

static void preset_load(uint32_t p)      /* a sound: its envelope, LFO, EDIT, OP and VOICE pages, and SOUND's two
                                          * at the middle; a user preset: its sound, or with LOADS on ALL everything
                                          * it kept (a factory sound then the arp and effects as new) */
{
    const preset_t *pr;
    int all = set.p[P_LOADS] == 1;
    if (all)                             /* the arp's settings replaced: what it had queued under the old goes */
        arp_cancel();
    if (p >= NPRESET) {
        const user_t *u = &users.u[(p - NPRESET) % NUSER];
        uint32_t i;
        set.preset = (uint8_t)p;
        if (all && !u->whole)            /* an older bank's: only its sound kept */
            patch_defaults();
        for (i = 0; i < NP; i++)
            if (sound_par(i) || (all && u->whole && patch_par(i)))
                set.p[i] = (int16_t)clamp(u->p[i], PAR[i].lo, PAR[i].hi);
        for (i = 0; i < 4u; i++) {
            set.op_ratio[i] = u->op_ratio[i];
            set.op_level[i] = u->op_level[i];
            set.op_wave[i] = u->op_wave[i];
            set.op_decay[i] = u->op_decay[i];
        }
        return;
    }
    pr = &PRESET[p];
    set.preset = (uint8_t)p;
    ops_load(pr);
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
    voice_load(pr);                      /* last: the filter's envelope follows this sound's amp envelope */
    if (all)
        patch_defaults();
}

/* the sound now, kept as user sound slot, and that then the one playing; 0 = written. The slot changes only once
 * the flash has it: a failed write leaves the bank, and the sound playing, as they were */
static int user_store(uint32_t slot)
{
    static user_t was;
    user_t *u = &users.u[slot % NUSER];
    uint32_t i;
    was = *u;
    u->used = 1;
    u->base = (uint8_t)(cur_preset() - PRESET);
    u->whole = 1;
    if (!was.used || !was.name) {        /* a new name (over one already named: it keeps its own), none another
                                          * slot has */
        static uint32_t name_rng = 0x6D2B79F5u;   /* its own: SAVE never moves the arp's chance (drift_rng) */
        uint32_t tries, k, nm = 0;
        name_rng ^= plat_ms() | 1u;
        for (tries = 0; tries < 32u; tries++) {
            name_rng ^= name_rng << 13;
            name_rng ^= name_rng >> 17;
            name_rng ^= name_rng << 5;
            nm = 1u + (name_rng >> 8) % 255u;
            for (k = 0; k < NUSER && !(users.u[k].used && users.u[k].name == nm && &users.u[k] != u); k++)
                ;
            if (k == NUSER)
                break;
        }
        u->name = (uint8_t)nm;
    }
    for (i = 0; i < NP; i++)
        u->p[i] = set.p[i];
    for (i = 0; i < 4u; i++) {
        u->op_ratio[i] = set.op_ratio[i];
        u->op_level[i] = set.op_level[i];
        u->op_wave[i] = set.op_wave[i];
        u->op_decay[i] = set.op_decay[i];
    }
    users.magic = USER_MAGIC;
    if (plat_store_save(OBJ_USER, &users, sizeof users) != 0) {
        *u = was;
        return -1;
    }
    set.preset = (uint8_t)(NPRESET + slot % NUSER);
    return 0;
}

/* the next sound from n, d steps on (factory then user, the empty user slots skipped); within one kind (cat) or all
 * (cat NCAT) */
static uint32_t sound_step(uint32_t n, int32_t d, uint32_t cat)
{
    uint32_t total = NBROWSE, i, k = browse_pos(n);   /* (in the browsing order) */
    while (d) {
        for (i = 0; i < total; i++) {
            k = (k + (d > 0 ? 1u : total - 1u)) % total;
            if (sound_ok(browse_at(k)) && (cat == NCAT || sound_cat(browse_at(k)) == cat))
                break;
        }
        d += d > 0 ? -1 : 1;
    }
    return browse_at(k);
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
    if (set.magic != SET_MAGIC || !sound_ok(set.preset) || set.octave < -2 || set.octave > 2 || set.arp_on > 1u
        || set.hold > H_LATCH)
        return 0;
    for (i = 0; i < NP; i++)
        if (set.p[i] < PAR[i].lo || set.p[i] > PAR[i].hi)
            return 0;
    for (i = 0; i < 4u; i++)
        if (set.op_ratio[i] >= N_OPRATIO || set.op_level[i] > 100u || set.op_wave[i] >= NWAVE || set.op_decay[i] > 100u)
            return 0;
    return 1;
}

void app_init(void)
{
    uint32_t i;
    eng_init();
#if defined(FM1B_CPU2) && FM1B_CPU2
    if (plat_cpu2_set(1) == 0)           /* the second core takes part of the voices (fm1.toml cpu2) */
        eng_cpu2(plat_cpu2_run, plat_cpu2_wait);
#endif
    for (i = 0; i < NHELD; i++)
        out_note[i] = -1;
    {                                    /* the user presets (before the settings: they may name one); a bank
                                          * not this firmware's: empty */
        static uint8_t raw[sizeof users];
        int got = plat_store_load(OBJ_USER, raw, sizeof raw);
        uint32_t magic = got >= 4 ? (uint32_t)raw[0] | (uint32_t)raw[1] << 8 | (uint32_t)raw[2] << 16 | (uint32_t)raw[3] << 24
                                  : 0u, k;
        if (magic == USER_MAGIC && got == (int)sizeof users) {
            uint8_t *d = (uint8_t *)&users;
            for (k = 0; k < sizeof users; k++)
                d[k] = raw[k];
            for (k = 0; k < NUSER; k++) {
                users.u[k].used = (uint8_t)(users.u[k].used == 1u && users.u[k].base < NPRESET);
                users.u[k].whole = (uint8_t)(users.u[k].whole == 1u);
            }
        }
        users.magic = USER_MAGIC;
    }
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set)   /* the settings (else as new, below) */
        set.magic = 0;
    if (set.magic != SET_MAGIC || !settings_ok())
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
#define C_NOW RGB(255, 226, 0)              /* yellow: what is happening this moment (the note just played, the knob being
                                             * turned, the operator OP edits): cyan, magenta, yellow, CMYK */
static void theme(void)
{
    pal[0] = RGB(50, 32, 82);            /* C_LINE: rules, the knobs' tracks */
    pal[1] = RGB(104, 78, 156);          /* C_DIM: inactive, units */
    pal[2] = RGB(110, 225, 255);         /* C_GRAY: labels and the held notes, cyan */
    pal[3] = RGB(178, 96, 255);          /* C_AMB: the sound, the tempo, notes as they age: electric purple */
    pal[4] = RGB(255, 46, 151);          /* C_HI: values, the knobs, new notes: hot pink */
}

#define RESET_TURNS 1u                   /* GLOBAL's PARAMS: KEEP, then RESET (a detent right) */
static struct {
    char msg[2][24];                     /* a short message over the header */
    uint32_t msg_until;
    uint32_t keys, btn;                  /* held at the last input */
    uint32_t page;
    uint32_t turned_at[4];               /* when each knob was last turned: its value lit for a moment */
    uint32_t tempo_at, chance_at;        /* the same, SELECT and ALGO */
    uint32_t save_slot;                  /* SOUNDS: the USER slot SAVE writes, 0..15 */
    uint32_t reset_arm;                  /* GLOBAL's PARAMS: KEEP, or turned right RESET (done) */
    uint8_t user_peek;                   /* SOUNDS on USER with none saved: its empty slots, the sound kept */
    uint8_t factory_arm;                 /* RESETS's FACTORY: 0 KEEP, 1 SURE? (SAVE: done) */
    uint8_t rescan;                      /* the keys as up after this scan: one still pressed sounds again (FACTORY) */
    uint8_t redraw;
} ui;

/* the kind SOUNDS shows: the sound's, or USER while KIND looks at it with none saved */
static uint32_t shown_cat(void)
{
    return ui.user_peek && sound_cat(set.preset) != CAT_USER ? CAT_USER : sound_cat(set.preset);
}
static int user_peeking(void) { return shown_cat() != sound_cat(set.preset); }

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

static const char *par_name(uint32_t p)
{
    static const char *const OPN[4] = {"RATIO", "LEVEL", "WAVE", "DECAY"};
    if (p == PX_SPACE)
        return "SPACE";
    if (p == PX_CAT)
        return "KIND";
    if (p == PX_SND)
        return "SOUND";
    if (p == PX_SLOT)
        return "SAVE TO";
    if (p == PX_RESET)
        return "PARAMS";
    if (p == PX_FACTORY)
        return "FACTORY";
    if (p >= PX_OPR && p <= PX_OPD)
        return OPN[p - PX_OPR];
    return p == P_VIB && ui.page == PG_HOME ? "DRIFT" : PAR[p].name;
}

static int op_own_env(uint32_t k);
/* MOD WAVE: the modulators' wave when they share one, -1 when they differ (MIXED), -2 with none (ALGO 8) */
static int32_t mod_wave(void)
{
    int32_t w = -2;
    uint32_t k;
    for (k = 0; k < 4u; k++)
        if (!(ENG_CAR[set.p[P_ALGO] & 7] >> k & 1u)) {
            if (w == -2)
                w = set.op_wave[k];
            else if (w != (int32_t)set.op_wave[k])
                return -1;
        }
    return w;
}
/* the page's knob i, or PX_NONE where it would mean nothing now: DECAY on a carrier the ENVELOPE page shapes, GLIDE
 * with VOICES on POLY */
static uint32_t page_knob(uint32_t i)
{
    uint32_t p = PAGE[ui.page].p[i];
    if ((p == PX_OPD && !op_own_env(IS_OP_PAGE(ui.page) ? ui.page - PG_OP1 : 0u))
        || (p == P_GLIDE && set.p[P_VOICES] == V_POLY) || (p == P_WAVE && mod_wave() == -2))
        return PX_NONE;
    return p;
}

/* OP 1-4: which operator the page edits, and whether it runs its own envelope (a modulator, or a strike carrier:
 * DECAY is its) or the ENVELOPE page's */
static uint32_t op_now(void) { return IS_OP_PAGE(ui.page) ? ui.page - PG_OP1 : 0u; }
static int op_is_carrier(uint32_t k) { return ENG_CAR[set.p[P_ALGO] & 7] >> k & 1u; }
static int op_own_env(uint32_t k) { return !op_is_carrier(k) || cur_preset()->s.op[k].strike; }

static void param_text(uint32_t p, char *t)  /* a parameter's value as the screen shows it */
{
    int32_t v;
    t[0] = 0;
    if (p == PX_CAT) {                       /* SOUNDS's */
        text_cat(t, CAT_NAME[shown_cat()]);
        return;
    }
    if (p == PX_SND) {
        if (user_peeking())
            text_cat(t, "EMPTY");
        else
            sound_title(set.preset, t);
        return;
    }
    if (p == PX_SLOT) {
        char u[4] = {'U', (char)('0' + (ui.save_slot + 1u) / 10u), (char)('0' + (ui.save_slot + 1u) % 10u), 0};
        text_cat(t, u);
        text_cat(t, users.u[ui.save_slot].used ? " USED" : " FREE");
        return;
    }
    if (p == PX_RESET) {                     /* GLOBAL's RESET: turn right until it goes */
        text_cat(t, ui.reset_arm ? "RESET" : "KEEP");
        return;
    }
    if (p == PX_FACTORY) {
        text_cat(t, ui.factory_arm ? "SURE?" : "KEEP");
        return;
    }
    if (p >= PX_OPR && p <= PX_OPD) {        /* OP 1-4's */
        uint32_t k = op_now();
        if (p == PX_OPR) {
            float r = OP_RATIO[set.op_ratio[k]];
            text_cat(t, "X");
            if (r == (float)(int32_t)r)
                int_text((int32_t)r, t + 1);
            else
                dec_text(r, r * 10.0f == (float)(int32_t)(r * 10.0f) ? 1 : 2, t + 1);
        } else if (p == PX_OPL) {
            int_text(set.op_level[k], t);
            text_cat(t, "%");
        } else if (p == PX_OPW) {
            text_cat(t, "W");
            int_text(set.op_wave[k] + 1, t + 1);
            text_cat(t, " ");
            text_cat(t, WAVE_SHORT[set.op_wave[k]]);
        } else {
            float sec = knob_secs(set.op_decay[k]);
            if (sec < 1.0f) {
                int_text((int32_t)(sec * 1000.0f + 0.5f), t);
                text_cat(t, " MS");
            } else {
                dec_text(sec, 1, t);
                text_cat(t, " S");
            }
        }
        return;
    }
    if (p == P_WAVE && mod_wave() < 0) {     /* MOD WAVE over modulators of different waves */
        text_cat(t, "MIXED");
        return;
    }
    if (p == PX_SPACE)
        p = P_RMIX;
    v = p == P_WAVE ? mod_wave() : set.p[p];
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
    case U_CHZ:
    case U_HZ: {
        float h = PAR[p].unit == U_CHZ ? 0.1f * fm_exp2f((float)v * 0.0564386f) : lfo_hz(v);
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
    case U_SYNC:                         /* CLOCK: its own, its own sent out, MIDI clock in */
        text_cat(t, v == 2 ? "EXT" : v ? "OUT" : "INT");
        break;
    case U_CUT: {                        /* CUTOFF: 30 Hz .. 18 kHz */
        float hz = cut_hz(v);
        if (v >= 100)
            text_cat(t, "OPEN");
        else if (hz >= 1000.0f) {
            dec_text(hz / 1000.0f, 1, t);
            text_cat(t, " KHZ");
        } else {
            int_text((int32_t)(hz + 0.5f), t);
            text_cat(t, " HZ");
        }
        break;
    }
    case U_CHORD:
        text_cat(t, CHORD_NAME[v]);
        break;
    case U_KEY:
        text_cat(t, KEY_NAME[v]);
        break;
    case U_SCALE:
        text_cat(t, SCALE_NAME[v]);
        break;
    case U_STRUM:
        if (!v)
            text_cat(t, "OFF");
        else {
            int_text((int32_t)(strum_ms(v) + 0.5f), t);
            text_cat(t, " MS");
        }
        break;
    case U_VOICES:
        text_cat(t, v == V_LEGATO ? "LEGATO" : v == V_MONO ? "MONO" : "POLY");
        break;
    case U_GLIDE:
        if (!v)
            text_cat(t, "OFF");
        else {
            float g = glide_secs(v);
            if (g < 1.0f) {
                int_text((int32_t)(g * 1000.0f + 0.5f), t);
                text_cat(t, " MS");
            } else {
                dec_text(g, 1, t);
                text_cat(t, " S");
            }
        }
        break;
    case U_ONOFF:
        text_cat(t, v ? "ON" : "OFF");
        break;
    case U_LOADS:                        /* PARAMS on a sound change: KEEP the arp and effects, or CHANGE them */
        text_cat(t, v ? "CHANGE" : "KEEP");
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
    uint32_t k = op_now();
    switch (p) {
    case PX_CAT:
        return (int32_t)shown_cat() * w / (int32_t)(NCAT - 1u);
    case PX_SND: {
        uint32_t n = 0, at = 0, i, k;        /* where in its kind */
        for (k = 0; k < NBROWSE; k++)
            if (i = browse_at(k), sound_ok(i) && sound_cat(i) == sound_cat(set.preset)) {
                if (i == set.preset)
                    at = n;
                n++;
            }
        return n > 1u ? (int32_t)at * w / (int32_t)(n - 1u) : 0;
    }
    case PX_SLOT:
        return (int32_t)ui.save_slot * w / (int32_t)(NUSER - 1u);
    case PX_RESET:
        return (int32_t)ui.reset_arm * w / (int32_t)RESET_TURNS;
    case PX_FACTORY:
        return (int32_t)ui.factory_arm * w / 2;
    case PX_OPR:
        return (int32_t)set.op_ratio[k] * w / (int32_t)(N_OPRATIO - 1u);
    case PX_OPL:
        return (int32_t)set.op_level[k] * w / 100;
    case PX_OPW:
        return (int32_t)set.op_wave[k] * w / (int32_t)(NWAVE - 1u);
    case PX_OPD:
        return op_own_env(k) ? (int32_t)set.op_decay[k] * w / 100 : 0;
    default:
        break;
    }
    if (p == P_WAVE)
        return mod_wave() > 0 ? mod_wave() * w / (int32_t)(NWAVE - 1u) : 0;
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
#if defined(FM1B_CPU2) && FM1B_CPU2
    text_cat(t, plat_cpu2_on() ? "% 2 CORES  DROPS " : "% 1 CORE  DROPS ");
#else
    text_cat(t, "%  DROPS ");
#endif
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
        {
            char nm[16];
            sound_name(set.preset, nm);
            text_cat(t, nm);
        }
        text_cat(t, set.arp_on ? ". ARP ON. " : ". ARP OFF. ");
        text_cat(t, HOLD_NAME[set.hold]);
        text_cat(t, ". ");
        text_cat(t, PAGE[ui.page].name);
        for (i = 0; i < 4u; i++) {
            if (page_knob(i) == PX_NONE)
                continue;
            text_cat(t, ". ");
            text_cat(t, par_name(page_knob(i)));
            text_cat(t, " ");
            param_text(page_knob(i), v);
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

static void sound_go(uint32_t n);
static int set_par(uint32_t p, int32_t v)    /* 1 = changed */
{
    v = clamp(v, PAR[p].lo, PAR[p].hi);
    if (v == set.p[p])
        return 0;
    set.p[p] = (int16_t)v;
    return 1;
}

static void page_go(uint32_t pg);
/* RESETS's FACTORY: as a new FM-1 (for testing): every USER preset gone and the settings as new, each written to the
 * flash, then HOME. The empty bank is written before the one in memory changes: a write the flash refuses leaves the
 * presets as they were (a later SAVE never writes a bank emptied only in memory) */
static void factory_reset(void)
{
    static uint8_t blank[sizeof users];
    uint32_t f, k;
    for (k = 0; k < sizeof blank; k++)
        blank[k] = 0;
    blank[0] = USER_MAGIC & 0xFFu;
    blank[1] = USER_MAGIC >> 8 & 0xFFu;
    blank[2] = USER_MAGIC >> 16 & 0xFFu;
    blank[3] = USER_MAGIC >> 24;
    if (plat_store_save(OBJ_USER, blank, sizeof blank) != 0) {
        page_go(PG_HOME);
        app_say("FACTORY RESET", "NOT SAVED");
        return;
    }
    f = plat_lock();
    held_clear();
    for (k = 0; k < 128u; k++)           /* the keys and MIDI notes down: none (a key still pressed comes back, at the
                                          * octave as new, on the next scan) */
        down[k] = 0, midi_down[k] = 0, midi_played[k] = 0;
    ndown = 0;
    ui.rescan = 1;
    arp_cancel();
    eng_release_all();
    {
        uint8_t *b = (uint8_t *)&users;
        for (k = 0; k < sizeof users; k++)
            b[k] = blank[k];
    }
    settings_defaults();
    plat_unlock(f);
    ui.save_slot = 0;
    page_go(PG_HOME);
    if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) != 0) {
        set_dirty = 1;
        app_say("PRESETS CLEARED", "SETTINGS NOT SAVED");
        return;
    }
    set_dirty = 0;
    app_say("FACTORY", "RESET");
}

static void param_turn(uint32_t p, int32_t d)
{
    char t[16];
    int changed;
    if (p == PX_NONE)
        return;
    if (p == PX_CAT || p == PX_SND) {        /* SOUNDS: another kind (its first sound), another sound in this one;
                                              * both stop at their lists' ends, as every knob does */
        uint32_t c = shown_cat(), n = set.preset, i, list[NPRESET + NUSER], cnt = 0, at = 0;
        if (p == PX_SND && user_peeking())   /* USER with none saved: nothing to choose */
            return;
        if (p == PX_SND) {
            uint32_t k;
            for (k = 0; k < NBROWSE; k++)
                if (i = browse_at(k), sound_ok(i) && sound_cat(i) == c) {
                    if (i == set.preset)
                        at = cnt;
                    list[cnt++] = i;
                }
            n = list[clamp((int32_t)at + d, 0, (int32_t)cnt - 1)];
        } else {
            int32_t step = d > 0 ? 1 : -1, cc = (int32_t)c;
            while (d) {                      /* a kind further each detent, an empty one (USER) passed over */
                int32_t nx = cc + step;
                uint32_t j, first = NPRESET + NUSER;
                for (; nx >= 0 && nx < (int32_t)NCAT; nx += step) {
                    for (j = 0; j < NBROWSE && first == NPRESET + NUSER; j++)   /* (the first browsed) */
                        if (sound_ok(browse_at(j)) && sound_cat(browse_at(j)) == (uint32_t)nx)
                            first = browse_at(j);
                    if (first < NPRESET + NUSER)
                        break;
                }
                if (first == NPRESET + NUSER) {  /* the end of the kinds: USER shown, empty, if none is saved */
                    if (step > 0 && cc < (int32_t)CAT_USER && !ui.user_peek) {
                        ui.user_peek = 1;
                        ui.redraw = 1;
                    }
                    break;
                }
                if (ui.user_peek) {          /* back from the empty USER */
                    ui.user_peek = 0;
                    ui.redraw = 1;
                }
                cc = nx;
                n = first;
                d -= step;
            }
        }
        if (n != set.preset) {           /* (passing through the kinds to the empty USER: still shown) */
            uint8_t pk = ui.user_peek;
            sound_go(n);
            ui.user_peek = pk;
        }
        return;
    }
    if (p == PX_SLOT) {
        ui.save_slot = (uint32_t)clamp((int32_t)ui.save_slot + d, 0, (int32_t)NUSER - 1);
        ui.redraw = 1;
        return;
    }
    if (p == PX_RESET) {                     /* RESET: the arp, SEQ, CHORDS, CHANCE and the effects as new. Once
                                              * done it stays DONE (turned on, nothing more); back left, it starts over */
        uint32_t was = ui.reset_arm;
        if (was >= RESET_TURNS && d < 0)
            ui.reset_arm = 0;
        else
            ui.reset_arm = (uint32_t)clamp((int32_t)ui.reset_arm + d, 0, (int32_t)RESET_TURNS);
        ui.redraw = 1;
        if (ui.reset_arm == RESET_TURNS && was < RESET_TURNS) {
            uint32_t f = plat_lock();
            patch_defaults();
            plat_unlock(f);
            set_dirty = 1;
            app_say("RESET", "ARP AND FX");
        }
        return;
    }
    if (p == PX_FACTORY) {                   /* FACTORY: a turn right SURE?; SAVE then erases (no turn of the knob, however
                                              * quick, ever does); left, KEEP */
        if (d < 0)
            ui.factory_arm = 0;
        else if (!ui.factory_arm) {
            ui.factory_arm = 1;
            app_say("FACTORY", "SAVE TO ERASE ALL");
        }
        ui.redraw = 1;
        return;
    }
    if (p >= PX_OPR && p <= PX_OPD) {        /* OP 1-4: the operator's own */
        uint32_t k = op_now();
        uint8_t *f = p == PX_OPR ? &set.op_ratio[k] : p == PX_OPL ? &set.op_level[k] : p == PX_OPW ? &set.op_wave[k]
                                                                                                    : &set.op_decay[k];
        int32_t hi = p == PX_OPR ? (int32_t)N_OPRATIO - 1 : p == PX_OPW ? (int32_t)NWAVE - 1 : 100, v = clamp((int32_t)*f + d, 0, hi);
        changed = v != *f;
        *f = (uint8_t)v;
    } else if (p == P_WAVE) {                /* MOD WAVE: from the modulators' own (the first's, when MIXED) */
        int32_t m = mod_wave();
        uint32_t k;
        if (m == -1)
            for (k = 0; k < 4u && m < 0; k++)
                if (!op_is_carrier(k))
                    m = set.op_wave[k];
        set.p[P_WAVE] = (int16_t)(m < 0 ? 0 : m);
        changed = set_par(P_WAVE, set.p[P_WAVE] + d) || mod_wave() == -1;
    } else if (p == PX_SPACE) {              /* SPACE: the reverb's mix and its length together */
        changed = set_par(P_RMIX, set.p[P_RMIX] + d);
        changed |= set_par(P_RLEN, set.p[P_RLEN] + d);
    } else
        changed = set_par(p, set.p[p] + d);
    if (changed && p == P_WAVE) {            /* MOD WAVE: every modulator's wave (OP 1-4 show each) */
        uint32_t k;
        for (k = 0; k < 4u; k++)
            if (!op_is_carrier(k))
                set.op_wave[k] = (uint8_t)set.p[P_WAVE];
    }
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

static void sound_go(uint32_t n)          /* to sound n, whole (no note between its fields), and say so */
{
    char nm[16];
    uint32_t f = plat_lock();
    preset_load(n);
    plat_unlock(f);
    ui.user_peek = 0;
    set_dirty = 1;
    ui.redraw = 1;
    sound_name(n, nm);
    app_say("SOUND", nm);
}

static void page_go(uint32_t pg)
{
    ui.page = pg;
    ui.reset_arm = 0;
    ui.factory_arm = 0;
    ui.user_peek = 0;
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
        if (k == B_FX) {                     /* FX: DELAY, again REVERB, again CHORUS, and round */
            page_go(ui.page == PG_DELAY ? PG_REVERB : ui.page == PG_REVERB ? PG_CHORUS : PG_DELAY);
            continue;
        }
        if (k == B_HOME) {                   /* HOME: the arp, again SOUNDS, and back */
            page_go(ui.page == PG_HOME ? PG_SOUNDS : PG_HOME);
            continue;
        }
        if (k == B_ENV) {                    /* ENV: the amp's envelope, again FILTER, again its envelope, round */
            page_go(ui.page == PG_ENV ? PG_FILTER : ui.page == PG_FILTER ? PG_FENV : PG_ENV);
            continue;
        }
        if (k == B_GLO) {                    /* GLO: GLOBAL, again RESETS, and back */
            page_go(ui.page == PG_GLOBAL ? PG_RESETS : PG_GLOBAL);
            continue;
        }
        if (k == B_ARP) {                    /* ARP: the arp, again CHORDS, and back */
            page_go(ui.page == PG_ARP ? PG_CHORDS : PG_ARP);
            continue;
        }
        if (k == B_LFO) {                    /* LFO: LFO, again LFO 2, and back */
            page_go(ui.page == PG_LFO ? PG_LFO2 : PG_LFO);
            continue;
        }
        if (k == B_SEL) {                    /* SEL: SOUND, again VOICE, and back */
            page_go(ui.page == PG_SOUND ? PG_VOICE : PG_SOUND);
            continue;
        }
        if (k == B_EDIT) {                   /* EDIT: the algorithm, again OP 1, OP 2, OP 3, OP 4, and round */
            page_go(ui.page == PG_EDIT ? PG_OP1 : IS_OP_PAGE(ui.page) && ui.page < PG_OP4 ? ui.page + 1u : PG_EDIT);
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
        } else if (k == B_SAVE && ui.page == PG_RESETS && ui.factory_arm) {   /* RESETS, FACTORY on SURE?: done */
            ui.factory_arm = 0;
            factory_reset();
        } else if (k == B_SAVE && ui.page == PG_SOUNDS) {   /* on SOUNDS: the sound into the USER slot chosen */
            char nm[16];
            if (user_store(ui.save_slot) != 0)
                app_say("NOT SAVED", "FLASH FAILED");
            else if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) != 0) {
                sound_name(set.preset, nm);   /* the sound is kept; only the settings (what plays at power-on) not */
                app_say(nm, "SETTINGS NOT SAVED");
            } else {
                set_dirty = 0;
                sound_name(set.preset, nm);
                app_say("SAVED", nm);
            }
            ui.redraw = 1;
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
        if (k == EN_PRESET) {                /* every sound, factory then user */
            sound_go(sound_step(set.preset, e, NCAT));
        } else if (k == EN_SELECT) {         /* the two left encoders: tempo and chance, on every page */
            param_turn(P_TEMPO, e);
            ui.tempo_at = now;
        } else if (k == EN_ALGO) {
            param_turn(P_CHANCE, e * 2);
            ui.chance_at = now;
        } else {
            param_turn(page_knob(k - EN_K1), e);
            ui.turned_at[k - EN_K1] = now;
        }
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = ui.rescan ? 0u : keys;
    ui.rescan = 0;
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
    uint8_t pos[KN_MAX * KN_MAX], dist[KN_MAX * KN_MAX], cen[KN_MAX * KN_MAX];   /* cen: from the centre (x 8) */
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
            m->cen[i] = (uint8_t)fm_minf(fm_sqrtf(dx * dx + dy * dy) * 8.0f, 255.0f);
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

/* frac: how far it is turned, 0..240. Inside the ring (with_cap: the pages; HOME's small ones are rings alone), the
 * knob itself: a dark cap with a rim, and its pointer from the centre out to the edge, at the value's angle */
#define KN_CAP RGB(36, 26, 62)
#define KN_RIM RGB(76, 60, 118)
static void knob_draw(const knob_map_t *m, int32_t cx, int32_t cy, int32_t frac, uint16_t c, int with_cap)
{
    int32_t x, y, h = m->n / 2;
    uint32_t cap = (uint32_t)(m->r - 6) * 8u;   /* the cap's radius, x 8 */
    float a = FM_PI * (0.75f + 1.5f * (float)frac / 240.0f), ux = fm_cosf(a), uy = fm_sinf(a);
    float len = (float)(m->r - 6);             /* the pointer: from the centre to the rim */
    for (y = 0; y < m->n; y++)
        for (x = 0; x < m->n; x++) {
            uint32_t i = (uint32_t)(y * m->n + x), d = m->dist[i], pos = m->pos[i], ce = m->cen[i];
            uint16_t col = C_BG;
            if (d < 40u && pos <= 240u) {    /* the ring: about 4 px wide, its edges soft */
                uint16_t ring = (int32_t)pos <= frac && frac > 0 ? c : C_LINE;
                uint32_t cov = d <= 24u ? 256u : (40u - d) * 16u;
                col = mix565(col, ring, cov);
            } else if (with_cap && ce < cap + 8u) {   /* the cap, its rim the last pixel, its edge soft */
                float dx = (float)(x - h), dy = (float)(y - h), t = dx * ux + dy * uy;
                float perp = fm_fabsf(dx * uy - dy * ux);
                col = ce + 12u >= cap ? KN_RIM : KN_CAP;
                if (ce > cap)
                    col = mix565(C_BG, col, (cap + 8u - ce) * 32u);
                if (t > -0.5f && t < len && perp < 1.9f)   /* the pointer, about 3 px wide */
                    col = mix565(col, c, perp < 0.9f ? 256u : (uint32_t)((1.9f - perp) * 256.0f));
            }
            if (col != C_BG)
                cv_pset(cx - h + x, cy - h + y, col);
        }
}

static void header(uint32_t now)
{
    cv_begin(240, 32, C_BG);
    if (now < ui.msg_until) {
        int32_t x = cv_text(10, 6, &FONT_B, ui.msg[0], C_NOW);   /* what just changed: yellow, as the knob turned */
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], C_HI);
    } else {
        cv_text(10, 6, &FONT_B, "MIGHTY", C_GRAY);
        cv_text(10 + text_w(&FONT_B, "MIGHTY "), 6, &FONT_B, "ARP", C_HI);
        char nm[16];
        sound_title(set.preset, nm);
        cv_text(230 - text_w(&FONT_B, nm), 6, &FONT_B, nm, C_AMB);
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
        cv_rect(x - s, y - s, 2 * s + 1, 2 * s + 1, age < 25u ? C_NOW : age < secs_cs / 4u ? C_HI : age < secs_cs / 2u ? C_AMB : C_DIM);
    }
}

/* HOME with the arp off: an oscilloscope of the sound. It waits for a rising zero crossing, so a held note stands
 * still, and scales itself to the sound (quickly up, slowly down: a fading note still fills it) */
#define SCOPE_SPAN 440u                  /* samples across the 220 pixels: 10 ms */
static void scope_view(int32_t h)
{
    static float gain_px;
    uint32_t w = scope_w, s = w - SCOPE_SPAN, i, peak = 0;
    int32_t x, py = 0, mid = h / 2;
    float g;
    for (i = 0; i < 300u; i++) {         /* the latest rising zero crossing that leaves a whole span */
        uint32_t a = w - SCOPE_SPAN - i;
        if (scope[(a - 1u) & (SCOPE_N - 1u)] < 0 && scope[a & (SCOPE_N - 1u)] >= 0) {
            s = a;
            break;
        }
    }
    for (i = 0; i < SCOPE_SPAN; i++) {
        int32_t v = scope[(s + i) & (SCOPE_N - 1u)];
        uint32_t m = (uint32_t)(v < 0 ? -v : v);
        peak = m > peak ? m : peak;
    }
    g = (float)(mid - 6) / (float)(peak > 400u ? peak : 400u);   /* (silence stays a flat line, not noise) */
    gain_px = g < gain_px || gain_px <= 0.0f ? g : gain_px + (g - gain_px) * 0.05f;
    cv_rect(10, mid, 220, 1, C_LINE);
    for (x = 0; x < 220; x++) {
        int32_t v = scope[(s + (uint32_t)x * 2u) & (SCOPE_N - 1u)];
        int32_t y = clamp(mid - (int32_t)((float)v * gain_px), 2, h - 3);
        if (x)
            cv_line(9 + x, py, 10 + x, y, C_HI);
        cv_rect(10 + x, y, 1, 2, C_HI);
        py = y;
    }
}

/* EDIT's WAVE, drawn: two turns of the modulators' wave (dsp/engine.c wave(), the TX81Z's eight), and its name */
static const char *const WAVE_NAME[NWAVE] = {"SINE", "PEAKED SINE", "HALF SINE", "HALF PEAKED", "FAST HALF",
                                             "FAST PEAKED", "FAST BUMPS", "FAST SOFT BUMPS", "SAW", "SQUARE",
                                             "PULSE 25%", "PULSE 12.5%", "TRIANGLE"};
static float wave_shape(float ph, uint32_t w)   /* ph 0..1 */
{
    float s, f = ph;
    if (w == 8u)                         /* W9: the saw, falling (the sum of sin(h x) / h) */
        return 1.0f - 2.0f * ph;
    if (w >= 9u && w <= 11u)             /* W10..W12: the square and the pulses, high for all but their last part */
        return ph < (w == 9u ? 0.5f : w == 10u ? 0.75f : 0.875f) ? 0.8f : -0.8f;
    if (w == 12u)                        /* W13: the triangle */
        return ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f;
    if (w >= 4u) {                       /* W5..W8: twice as fast in the first half, silent in the second */
        if (f >= 0.5f)
            return 0.0f;
        f *= 2.0f;
    }
    s = fm_sinf(f * FM_TWO_PI);
    switch (w & 3u) {
    case 0:
        return s;
    case 1:
        return s * fm_fabsf(s);
    case 2:
        return w >= 4u ? (w == 6u ? fm_fabsf(s) : s) : (s > 0.0f ? s : 0.0f);
    default:
        return w >= 4u ? s * s : (s > 0.0f ? s * s : 0.0f);
    }
}

/* SOUNDS: the kind's sounds, the one playing in pink, a few either side of it (USER: its 16 slots, the empty ones
 * dim) */
static void sounds_view(int32_t h)
{
    uint32_t c = shown_cat(), list[NPRESET + NUSER], n = 0, at = 0, i;
    int32_t first, row;
    char nm[16];
    uint32_t k;
    for (k = 0; k < NBROWSE; k++)
        if (i = browse_at(k), c == CAT_USER ? i >= NPRESET : sound_cat(i) == c) {
            if (i == set.preset)
                at = n;
            list[n++] = i;
        }
    first = clamp((int32_t)at - 2, 0, n > 5u ? (int32_t)n - 5 : 0);
    for (row = 0; row < 5 && first + row < (int32_t)n; row++) {
        uint32_t s2 = list[first + row];
        int32_t y = 4 + row * 13;
        sound_name(s2, nm);
        if (s2 == set.preset)
            cv_rect(8, y - 1, 224, 13, C_LINE);
        cv_text(12, y, &FONT_XS, nm, s2 == set.preset ? C_HI : sound_ok(s2) ? C_GRAY : C_DIM);
    }
    (void)h;
}

/* EDIT's ALGO, drawn: the four operators where the algorithm puts them (carriers along the bottom, pink; modulators
 * above what they modulate, outlined), an arrow each modulation (from engine.h's ENG_MOD, the sound's own), and
 * operator 4's loop when FEEDBACK is up. Beside it the modulators' wave (WAVE), a turn of it */
static const uint8_t ALG_X[8][4] = {     /* each operator's place (its centre), per algorithm */
    {44, 44, 44, 44}, {50, 50, 30, 70}, {50, 30, 30, 70}, {50, 70, 30, 30},
    {30, 30, 80, 80}, {25, 60, 95, 60}, {25, 60, 95, 95}, {20, 52, 84, 116},
};
static const uint8_t ALG_ROW[8][4] = {   /* and its row, 0 the top, 3 the carriers' */
    {3, 2, 1, 0}, {3, 2, 1, 1}, {3, 2, 1, 2}, {3, 2, 2, 1},
    {3, 2, 3, 2}, {3, 3, 3, 2}, {3, 3, 3, 2}, {3, 3, 3, 3},
};

static void algo_view(int32_t h, int32_t hl)   /* hl: the operator OP is editing, outlined white (-1 none) */
{
    uint32_t a = (uint32_t)set.p[P_ALGO] & 7u, fb = (uint32_t)set.p[P_FDBK], k, j;
    uint32_t w = hl >= 0 ? set.op_wave[hl] : mod_wave() >= 0 ? (uint32_t)mod_wave() : set.op_wave[0];   /* the wave
                                                     * beside: this operator's; the modulators' (or operator 1's); or MOD
                                                                         * WAVE's */
    int32_t x, py = 0, lo = 240, hi = 0, y0 = 44, amp = 16;
    char t[24] = "ALGO ";
#define OPX(k) ((int32_t)ALG_X[a][k] + 4)
#define OPY(k) (7 + (int32_t)ALG_ROW[a][k] * 17)
    for (k = 0; k < 4u; k++)             /* the arrows first, under the boxes */
        for (j = k + 1u; j < 4u; j++)
            if (ENG_MOD[a][k] >> j & 1u) {
                int32_t x0 = OPX(j), y0a = OPY(j) + 5, x1 = OPX(k), y1 = OPY(k) - 6;
                cv_line(x0, y0a, x1, y1, C_GRAY);
                cv_rect(x1 - 1, y1 - 1, 3, 2, C_GRAY);
            }
    if (fb) {                            /* operator 4 into itself */
        int32_t fx = OPX(3), fy = OPY(3);
        uint16_t c = fb > 4u ? C_HI : C_AMB;
        cv_rect(fx + 9, fy, 5, 1, c);
        cv_rect(fx + 13, fy - 8, 1, 9, c);
        cv_rect(fx, fy - 8, 14, 1, c);
        cv_rect(fx, fy - 8, 1, 3, c);
    }
    for (k = 0; k < 4u; k++) {           /* the boxes: the carriers filled */
        int32_t bx = OPX(k) - 8, by = OPY(k) - 5;
        char n[2] = {(char)('1' + k), 0};
        if ((int32_t)k == hl) {          /* the one being edited */
            cv_rect(bx - 2, by - 2, 21, 1, C_NOW);
            cv_rect(bx - 2, by + 12, 21, 1, C_NOW);
            cv_rect(bx - 2, by - 2, 1, 15, C_NOW);
            cv_rect(bx + 18, by - 2, 1, 15, C_NOW);
        }
        if (ENG_CAR[a] >> k & 1u) {
            cv_rect(bx, by, 17, 11, C_HI);
            cv_text(bx + 6, by - 1, &FONT_T, n, C_BLACK);
            lo = OPX(k) < lo ? OPX(k) : lo;
            hi = OPX(k) > hi ? OPX(k) : hi;
        } else {
            cv_rect(bx, by, 17, 1, C_GRAY);
            cv_rect(bx, by + 10, 17, 1, C_GRAY);
            cv_rect(bx, by, 1, 11, C_GRAY);
            cv_rect(bx + 16, by, 1, 11, C_GRAY);
            cv_text(bx + 6, by - 1, &FONT_T, n, C_GRAY);
        }
    }
    cv_rect(lo, h - 3, hi - lo + 1, 1, C_HI);   /* the carriers summed: the sound out */
    for (k = 0; k < 4u; k++)
        if (ENG_CAR[a] >> k & 1u)
            cv_rect(OPX(k), OPY(k) + 6, 1, h - 3 - OPY(k) - 6, C_HI);
#undef OPX
#undef OPY
    int_text((int32_t)a + 1, t + 5);     /* the caption and the wave, at the right */
    cv_text(150, 4, &FONT_XS, t, C_GRAY);
    t[0] = 'W';
    int_text((int32_t)w + 1, t + 1);
    text_cat(t, " ");
    text_cat(t, WAVE_NAME[w]);
    cv_text(150, 20, &FONT_T, t, C_DIM);
    cv_rect(150, y0, 80, 1, C_LINE);
    for (x = 0; x <= 80; x++) {
        int32_t y = y0 - (int32_t)(wave_shape((float)x / 80.0f, w) * (float)amp);
        if (x)
            cv_line(149 + x, py, 150 + x, y, C_HI);
        py = y;
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
    if (set.p[P_CLOCK] == 2) {
        t[0] = 0;
        if (ext_bpm_x10) {
            int_text((int32_t)((ext_bpm_x10 + 5u) / 10u), t);
            text_cat(t, " EXT");
        } else
            text_cat(t, "EXT WAIT");
    } else
        param_text(P_TEMPO, t);
    cv_text(120, 12, &FONT_B, t, ui.tempo_at && now - ui.tempo_at < 400u ? C_NOW : C_AMB);
    cv_text(184, 0, &FONT_XS, "CHANCE", C_DIM);
    int_text(set.p[P_CHANCE], t);
    text_cat(t, "%");
    cv_text(184, 12, &FONT_B, t, ui.chance_at && now - ui.chance_at < 400u ? C_NOW : C_AMB);
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
        if (set.arp_on)
            notes_view(110, 600u, 1);
        else
            scope_view(110);             /* the keys alone: the sound itself */
        cv_blit(0, 34);
        cv_begin(240, 68, C_BG);
        cv_rect(10, 0, 220, 1, C_LINE);
        for (i = 0; i < 4u; i++) {
            uint32_t p = pg->p[i];
            int32_t cx = 37 + (int32_t)i * 55;
            int lit = ui.turned_at[i] && now - ui.turned_at[i] < 400u;
            const char *nm = par_name(p);
            knob_draw(&kn_small, cx, 19, par_frac(p, 240), lit ? C_NOW : C_HI, 0);
            param_text(p, t);
            cv_text(cx - text_w(&FONT_B, t) / 2, 34, &FONT_B, t, lit ? C_NOW : C_HI);
            cv_text(cx - text_w(&FONT_XS, nm) / 2, 52, &FONT_XS, nm, C_GRAY);
        }
        cv_blit(0, 144);
    } else {
        /* a page: its four knobs, two by two */
        cv_begin(240, 108, C_BG);
        {
            static const char *const NEXT[NPAGE] = {[PG_DELAY] = "FX AGAIN: REVERB", [PG_REVERB] = "FX AGAIN: CHORUS",
                [PG_CHORUS] = "FX AGAIN: DELAY", [PG_EDIT] = "EDIT AGAIN: OP 1", [PG_OP1] = "EDIT AGAIN: OP 2",
                [PG_OP2] = "EDIT AGAIN: OP 3", [PG_OP3] = "EDIT AGAIN: OP 4", [PG_OP4] = "EDIT AGAIN: ALGO",
                [PG_SOUND] = "SEL AGAIN: VOICE", [PG_VOICE] = "SEL AGAIN: SOUND",
                [PG_SOUNDS] = "SAVE: INTO THE SLOT", [PG_ENV] = "ENV AGAIN: FILTER",
                [PG_FILTER] = "ENV AGAIN: ITS ENV", [PG_FENV] = "ENV AGAIN: AMP", [PG_LFO] = "LFO AGAIN: LFO 2", [PG_LFO2] = "LFO AGAIN: LFO 1",
                [PG_ARP] = "ARP AGAIN: CHORDS", [PG_CHORDS] = "ARP AGAIN: ARP",
                [PG_GLOBAL] = "GLO AGAIN: RESETS", [PG_RESETS] = "FACTORY: SURE?, THEN SAVE"};
            int32_t x = cv_text(10, 0, &FONT_B, pg->name, C_GRAY);
            if (IS_OP_PAGE(ui.page))         /* the operator's part in the algorithm */
                cv_text(x + 6, 0, &FONT_B, op_is_carrier(op_now()) ? "CARRIER" : "MODULATOR", C_HI);
            if (NEXT[ui.page])
                cv_text(230 - text_w(&FONT_XS, NEXT[ui.page]), 1, &FONT_XS, NEXT[ui.page], C_DIM);
        }
        cv_rect(10, 18, 220, 1, C_LINE);
        for (i = 0; i < 4u; i++) {
            uint32_t p = page_knob(i);
            int32_t x = 10 + (int32_t)(i % 2u) * 116, y = 23 + (int32_t)(i / 2u) * 43;
            if (p == PX_NONE)                /* a page with fewer knobs */
                continue;
            int lit = ui.turned_at[i] && now - ui.turned_at[i] < 400u;
            knob_draw(&kn_big, x + 16, y + 20, par_frac(p, 240), lit ? C_NOW : C_HI, 1);
            cv_text(x + 38, y + 1, &FONT_S, par_name(p), C_GRAY);
            param_text(p, t);
            {                                /* a long one (AS PLAYED, LEGATO, a sound's name) smaller */
                int32_t room = i % 2u ? 66 : 68;
                if (text_w(&FONT_M, t) <= room)
                    cv_text(x + 38, y + 16, &FONT_M, t, lit ? C_NOW : C_HI);
                else if (text_w(&FONT_B, t) <= room + 8)
                    cv_text(x + 38, y + 20, &FONT_B, t, lit ? C_NOW : C_HI);
                else
                    cv_text(x + 38, y + 21, &FONT_XS, t, lit ? C_NOW : C_HI);
            }
        }
        cv_blit(0, 34);
        cv_begin(240, 70, C_BG);
        cv_rect(10, 0, 220, 1, C_LINE);
        if (ui.page == PG_SOUNDS)
            sounds_view(70);
        else if (ui.page == PG_EDIT || IS_OP_PAGE(ui.page))
            algo_view(70, IS_OP_PAGE(ui.page) ? (int32_t)op_now() : -1);
        else
            notes_view(70, 400u, 0);
        if (ui.page == PG_GLOBAL) {          /* the load: so a dropout can be told from a reset */
            load_text(t);
            cv_text(10, 4, &FONT_T, t, C_DIM);
        }
        cv_blit(0, 142);
    }
    state_line(now);
}

/* the boot screen (APP_SPLASH): "Mighty" brushed over a chrome "ARP", a picture made by tools/make_splash.py
 * (splash.h, run-length RGB565), unpacked into the canvas in two bands (it holds 240 x 144) */
#include "splash.h"
void app_splash(void)
{
    uint32_t r = 0, left = SPLASH_RLE[1], y0, y, x;
    uint16_t c = SPLASH_RLE[0];
    theme();
    for (y0 = 0; y0 < 240u; y0 += 144u) {
        uint32_t h = y0 ? 96u : 144u;
        cv_begin(240, h, C_BG);
        for (y = 0; y < h; y++)
            for (x = 0; x < 240u; x++) {
                while (!left && ++r < SPLASH_RUNS) {
                    c = SPLASH_RLE[2u * r];
                    left = SPLASH_RLE[2u * r + 1u];
                }
                if (left) {
                    left--;
                    cv_pset((int32_t)x, (int32_t)y, c);
                }
            }
        cv_blit(0, y0);
    }
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
    if (viz_w != last_viz || ((viz_w || ui.page == PG_GLOBAL || (ui.page == PG_HOME && !set.arp_on))
                               && now - last_frame >= 50u))   /* the notes move (GLOBAL's load, HOME's scope) */
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
    else if (!strncmp(what, "note", 4) && what[4] >= '1' && what[4] <= '9') {   /* noteN: the Nth since arp_mark */
        uint32_t k = (uint32_t)atoi(what + 4) - 1u;
        *got = k < 16u && k < arp_count - arp_mark_at ? played_seq[k] : -1;
    }
    else if (!strcmp(what, "arp_mark")) {    /* (always 0: counting starts again) */
        arp_mark = arp_count;
        arp_mark_at = arp_count;
        *got = 0;
    }
    else if (!strcmp(what, "queued"))        /* the arp has a note waiting: a strum's next, a late or swung step */
        *got = strum_wait > 0 || hum_wait > 0 || swing_wait > 0;
    else if (!strcmp(what, "voices"))        /* sounding */
        *got = (int)eng_active();
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "cpu2"))          /* the second core takes voices */
#if defined(FM1B_CPU2) && FM1B_CPU2
        *got = plat_cpu2_on();
#else
        *got = 0;
#endif
    else if (!strncmp(what, "op", 2) && what[2] >= '1' && what[2] <= '4' && what[3] == '_') {   /* opN_ratio, _level,
                                                                                              * _wave, _decay */
        uint32_t k = (uint32_t)(what[2] - '1');
        const char *f = what + 4;
        *got = !strcmp(f, "ratio") ? set.op_ratio[k] : !strcmp(f, "level") ? set.op_level[k]
             : !strcmp(f, "wave") ? set.op_wave[k] : set.op_decay[k];
    }
    else if (!strcmp(what, "splash")) {      /* (always 0) the boot screen drawn again, for a shot (the host shows none) */
        app_splash();
        *got = 0;
    }
    else if (!strncmp(what, "user", 4) && !strcmp(what + strlen(what) - 5, "_used"))   /* userN_used: slot N */
        *got = users.u[(uint32_t)(atoi(what + 4) - 1) % NUSER].used;
    else if (!strncmp(what, "user", 4) && !strcmp(what + strlen(what) - 5, "_name"))   /* userN_name: its name */
        *got = users.u[(uint32_t)(atoi(what + 4) - 1) % NUSER].name;
    else if (!strcmp(what, "shed_note"))     /* the note the last shed let go */
        *got = (int)eng_shed_note();
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
