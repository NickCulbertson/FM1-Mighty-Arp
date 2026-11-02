/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal, Nick Culbertson */
/* DRIFT: an FM keys synth with a chance arpeggiator, a ping-pong delay and a reverb, for generative playing: the
 * player picks the notes, the arp picks when. From fm1-base's template.
 *
 * The audio interrupt owns all the note state: the keys reach it through a queue (ev_push), MIDI in is read there,
 * and the arp, the voices and the effects all run there. The main loop owns the settings and the screen; the render
 * reads the settings as they are (single 32-bit values).
 *
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in
 * scope. */
#include "fastmath.h"

#define SR 44100.0f
#define NVOICE 10
#define NHELD 16

/* ------------------------------------------------------------- parameters --- */
/* five pages of four knobs: SEL (the sound), ENV, ARP, FX, GLO */
enum { P_BRIGHT, P_TINE, P_DETUNE, P_VELO,
       P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE,
       P_RATE, P_CHANCE, P_OCTAVES, P_MODE,
       P_DTIME, P_DFDBK, P_DMIX, P_REVERB,
       P_TEMPO, P_SIZE, P_TONE, P_GATE, NP };
enum { U_PCT, U_TIME, U_DIV, U_OCT, U_MODE, U_DDIV, U_BPM };
typedef struct {
    const char *name;
    int16_t lo, hi, def;
    uint8_t unit;
} param_t;
static const param_t PAR[NP] = {
    {"BRIGHT", 0, 100, 50, U_PCT},   {"TINE", 0, 100, 50, U_PCT},     {"DETUNE", 0, 100, 25, U_PCT},
    {"VELOCITY", 0, 100, 60, U_PCT}, {"ATTACK", 0, 100, 0, U_TIME},   {"DECAY", 0, 100, 70, U_TIME},
    {"SUSTAIN", 0, 100, 30, U_PCT},  {"RELEASE", 0, 100, 82, U_TIME}, {"RATE", 0, 6, 2, U_DIV},
    {"CHANCE", 0, 100, 33, U_PCT},   {"OCTAVES", 1, 4, 2, U_OCT},     {"MODE", 0, 3, 3, U_MODE},
    {"DLY TIME", 0, 7, 3, U_DDIV},   {"FEEDBACK", 0, 95, 45, U_PCT},  {"DLY MIX", 0, 100, 30, U_PCT},
    {"REVERB", 0, 100, 40, U_PCT},   {"TEMPO", 40, 200, 90, U_BPM},   {"SIZE", 0, 100, 75, U_PCT},
    {"TONE", 0, 100, 55, U_PCT},     {"GATE", 5, 100, 50, U_PCT},
};
#define NPAGE 5
static const char *const PAGE_NAME[NPAGE] = {"SOUND", "ENVELOPE", "ARP", "EFFECTS", "GLOBAL"};
static const uint8_t PAGE_BTN[NPAGE] = {B_SEL, B_ENV, B_ARP, B_FX, B_GLO};

/* the arp's steps and the delay's times, in beats */
static const char *const DIV_NAME[7] = {"1/2", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const float DIV_BEATS[7] = {2.0f, 1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f};
static const char *const DDIV_NAME[8] = {"1/16", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2"};
static const float DDIV_BEATS[8] = {0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 2.0f / 3.0f, 1.0f, 1.5f, 2.0f};
enum { M_UP, M_DOWN, M_UPDN, M_RANDOM };
static const char *const MODE_NAME[4] = {"UP", "DOWN", "UP-DN", "RANDOM"};

/* ---------------------------------------------------------------- presets --- */
/* two modulator -> carrier pairs a voice, as DX electric pianos are built: pair A the body, pair B the tine. ratio
 * c / m are the carrier's and the modulator's frequency ratios, idx the modulation index at full velocity, dec the
 * index's fall to sus (of idx) in seconds, lvl the pair's level. ENV: the envelope knobs the preset starts from */
typedef struct {
    float c, m, idx, dec, sus, lvl;
} pair_t;
typedef struct {
    const char *name;
    pair_t a, b;
    int8_t attack, decay, sustain, release;
} preset_t;
#define NPRESET 8
static const preset_t PRESET[NPRESET] = {
    {"E.PIANO", {1.0f, 1.0f, 1.6f, 1.2f, 0.15f, 1.0f}, {1.0f, 14.0f, 1.3f, 0.07f, 0.0f, 0.35f}, 0, 74, 25, 82},
    {"BELLS", {1.0f, 3.5f, 2.4f, 2.5f, 0.2f, 1.0f}, {1.0f, 1.41f, 1.0f, 1.5f, 0.1f, 0.5f}, 0, 86, 0, 88},
    {"GLASS", {1.0f, 7.0f, 1.0f, 0.8f, 0.3f, 1.0f}, {2.0f, 1.0f, 0.5f, 1.0f, 0.4f, 0.4f}, 4, 80, 20, 86},
    {"KALIMBA", {1.0f, 5.0f, 1.8f, 0.05f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.4f, 0.3f, 0.0f, 0.5f}, 0, 62, 0, 70},
    {"MARIMBA", {1.0f, 4.0f, 2.0f, 0.03f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.5f, 0.12f, 0.0f, 0.6f}, 0, 58, 0, 66},
    {"PAD", {1.0f, 1.0f, 1.2f, 4.0f, 0.6f, 1.0f}, {1.003f, 2.0f, 0.8f, 3.0f, 0.5f, 0.8f}, 72, 82, 80, 88},
    {"MUSIC BOX", {1.0f, 7.0f, 1.5f, 0.15f, 0.05f, 1.0f}, {3.0f, 1.0f, 0.2f, 0.4f, 0.0f, 0.3f}, 0, 68, 0, 82},
    {"HARP", {1.0f, 2.0f, 1.5f, 0.15f, 0.1f, 1.0f}, {1.0f, 1.0f, 0.25f, 0.5f, 0.0f, 0.5f}, 0, 64, 0, 80},
};

/* --------------------------------------------------------------- settings --- */
#define SET_MAGIC 0x44524631u            /* "DRF1": the settings record; another magic loads defaults */
typedef struct {
    uint32_t magic;
    int16_t p[NP];
    uint8_t preset;
    int8_t octave;                       /* -2 .. +2 */
    uint8_t arp_on, hold;
} settings_t;
static settings_t set;
static uint8_t set_dirty;                /* changed since the last save: SAVE is lit */

/* ------------------------------------------------------------------- dsp --- */
#define SIN_N 2048u
static float sin_tab[SIN_N + 1];
static inline float sin_t(float ph)      /* ph in turns, any value: the table, read between its points */
{
    float x, f;
    uint32_t i;
    ph -= fm_floorf(ph);
    x = ph * (float)SIN_N;
    i = (uint32_t)x;
    f = x - (float)i;
    return sin_tab[i] + (sin_tab[i + 1] - sin_tab[i]) * f;
}

static uint32_t rng = 0x9E3779B9u;
static inline uint32_t rnd(void)         /* xorshift32 */
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static inline uint32_t rnd_n(uint32_t n) { return (uint32_t)(((uint64_t)rnd() * n) >> 32); }

/* a 0..100 time knob: 1 ms .. 10 s, exponential */
static float knob_secs(int32_t v) { return 0.001f * fm_exp2f((float)v * 0.1328771f); }   /* 10000 ^ (v / 100) */
/* the per-sample factor of a fall to 1/1000 in s seconds */
static float fall_coef(float s) { return fm_exp2f(-9.965784f / (fm_maxf(s, 0.0005f) * SR)); }

/* ---------------------------------------------------------------- voices --- */
enum { ENV_OFF, ENV_ATT, ENV_DEC, ENV_REL };
typedef struct {
    int32_t note;                        /* MIDI note, -1 = free */
    uint32_t age;                        /* the trigger's count: the oldest is stolen first */
    int32_t gate;                        /* samples left before the arp releases it; -1: held by a key */
    uint8_t stage;
    float env, att_inc, dec_k, sus, rel_k;
    float ph[4], inc[4];                 /* carrier A, modulator A, carrier B, modulator B: turns a sample */
    float ia, ia_floor, ia_k, ib, ib_floor, ib_k;   /* each pair's index, falling to its floor */
    float la, lb, pan_l, pan_r, gain;
} voice_t;
static voice_t vc[NVOICE];
static uint32_t vc_age;

static void voice_on(int32_t note, uint32_t vel, int32_t gate)
{
    const preset_t *pr = &PRESET[set.preset];
    voice_t *v = 0;
    uint32_t i, oldest = 0xFFFFFFFFu;
    float f, velf = (float)vel / 127.0f, velo = (float)set.p[P_VELO] / 100.0f, bright, tine, det, idx_v, pos;
    for (i = 0; i < NVOICE; i++)         /* the same note again: that voice, so it never stacks */
        if (vc[i].note == note)
            v = &vc[i];
    if (!v)
        for (i = 0; i < NVOICE; i++)     /* else a free one, else the oldest releasing, else the oldest */
            if (vc[i].stage == ENV_OFF) {
                v = &vc[i];
                break;
            }
    if (!v)
        for (i = 0; i < NVOICE; i++)
            if (vc[i].stage == ENV_REL && vc[i].age < oldest)
                oldest = vc[i].age, v = &vc[i];
    if (!v)
        for (i = 0, oldest = 0xFFFFFFFFu; i < NVOICE; i++)
            if (vc[i].age < oldest)
                oldest = vc[i].age, v = &vc[i];
    f = 440.0f * fm_exp2f((float)(note - 69) / 12.0f) / SR;
    det = fm_exp2f((float)set.p[P_DETUNE] * 0.00015f);    /* up to 18 cents between the pairs */
    bright = fm_exp2f(((float)set.p[P_BRIGHT] - 50.0f) / 25.0f);   /* x0.25 .. x4 */
    tine = (float)set.p[P_TINE] / 50.0f;
    idx_v = 1.0f - velo * (1.0f - velf);                  /* velocity opens the index and the level */
    v->note = note;
    v->age = ++vc_age;
    v->gate = gate;
    v->inc[0] = f * pr->a.c;
    v->inc[1] = f * pr->a.m;
    v->inc[2] = f * pr->b.c * det;
    v->inc[3] = f * pr->b.m * det;
    if (v->stage == ENV_OFF) {
        v->ph[0] = v->ph[1] = v->ph[2] = v->ph[3] = 0.0f;
        v->env = 0.0f;
    }
    v->ia = pr->a.idx * bright * idx_v;
    v->ia_floor = v->ia * pr->a.sus;
    v->ia_k = fall_coef(pr->a.dec);
    v->ib = pr->b.idx * bright * idx_v;
    v->ib_floor = v->ib * pr->b.sus;
    v->ib_k = fall_coef(pr->b.dec);
    v->la = pr->a.lvl;
    v->lb = pr->b.lvl * tine;
    v->gain = 0.5f * (1.0f - velo * (1.0f - velf) * 0.85f);
    pos = fm_clampf((float)(note - 60) / 36.0f, -0.5f, 0.5f);   /* low notes a little left, high a little right */
    v->pan_l = 0.5f - pos * 0.6f;
    v->pan_r = 0.5f + pos * 0.6f;
    v->att_inc = 1.0f / (knob_secs(set.p[P_ATTACK]) * SR);
    v->dec_k = fall_coef(knob_secs(set.p[P_DECAY]));
    v->sus = (float)set.p[P_SUSTAIN] / 100.0f;
    v->rel_k = fall_coef(knob_secs(set.p[P_RELEASE]));
    v->stage = ENV_ATT;
}

static void voice_release(voice_t *v)
{
    if (v->stage != ENV_OFF && v->stage != ENV_REL) {
        v->rel_k = fall_coef(knob_secs(set.p[P_RELEASE]));
        v->stage = ENV_REL;
    }
}

static void voice_off(int32_t note)      /* a key's note let go */
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (vc[i].note == note && vc[i].gate < 0)
            voice_release(&vc[i]);
}

static inline float voice_tick(voice_t *v, float *r)   /* one sample: the left side returned, the right in *r */
{
    float ca, cb, s;
    switch (v->stage) {
    case ENV_ATT:
        if ((v->env += v->att_inc) >= 1.0f) {
            v->env = 1.0f;
            v->stage = ENV_DEC;
        }
        break;
    case ENV_DEC:
        v->env = v->sus + (v->env - v->sus) * v->dec_k;
        break;
    case ENV_REL:
        v->env *= v->rel_k;
        if (v->env < 0.0003f) {
            v->stage = ENV_OFF;
            v->note = -1;
            *r = 0.0f;
            return 0.0f;
        }
        break;
    default:
        *r = 0.0f;
        return 0.0f;
    }
    if (v->gate > 0 && --v->gate == 0)
        voice_release(v);
    v->ia = v->ia_floor + (v->ia - v->ia_floor) * v->ia_k;
    v->ib = v->ib_floor + (v->ib - v->ib_floor) * v->ib_k;
    s = v->env * v->gain;                /* the index in radians, the phases in turns */
    ca = v->la * sin_t(v->ph[0] + v->ia * 0.159155f * sin_t(v->ph[1]));
    cb = v->lb * sin_t(v->ph[2] + v->ib * 0.159155f * sin_t(v->ph[3]));
    *r = s * (ca * v->pan_r + cb * v->pan_l);   /* the pairs mirrored across the voice's place: the detune is wide */
    s *= ca * v->pan_l + cb * v->pan_r;
    v->ph[0] += v->inc[0];
    v->ph[1] += v->inc[1];
    v->ph[2] += v->inc[2];
    v->ph[3] += v->inc[3];
    if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
    if (v->ph[1] >= 1.0f) v->ph[1] -= fm_floorf(v->ph[1]);
    if (v->ph[2] >= 1.0f) v->ph[2] -= 1.0f;
    if (v->ph[3] >= 1.0f) v->ph[3] -= fm_floorf(v->ph[3]);
    return s;
}

/* --------------------------------------------------------------- effects --- */
/* ping-pong delay: two lines of int16, up to 0.74 s; the read point slews to a new time like tape */
#define DLY_N 32768u
static PLAT_POOL int16_t dly_l[DLY_N], dly_r[DLY_N];
static uint32_t dly_w;
static float dly_t, dly_lp_l, dly_lp_r;

static inline float dly_read(const int16_t *b, float d)
{
    float p = (float)dly_w - d, f;
    int32_t i;
    if (p < 0.0f)
        p += (float)DLY_N;
    i = (int32_t)p;
    f = p - (float)i;
    return ((float)b[i & (DLY_N - 1)] * (1.0f - f) + (float)b[(i + 1) & (DLY_N - 1)] * f) * (1.0f / 16384.0f);
}
static inline int16_t to16(float x) { return (int16_t)fm_clampf(x * 16384.0f, -32767.0f, 32767.0f); }

/* reverb: four delay lines fed back through a Hadamard matrix, each with a low-pass, after four allpasses that smear
 * the input. SIZE is the feedback (the tail), TONE the low-pass */
static const uint16_t FDN_LEN[4] = {3119, 3739, 4507, 5441};
static const uint16_t AP_LEN[4] = {142, 107, 379, 277};
static PLAT_POOL float fdn_buf0[3119], fdn_buf1[3739], fdn_buf2[4507], fdn_buf3[5441];
static PLAT_POOL float ap_buf0[142], ap_buf1[107], ap_buf2[379], ap_buf3[277];
static float *const FDN_BUF[4] = {fdn_buf0, fdn_buf1, fdn_buf2, fdn_buf3};
static float *const AP_BUF[4] = {ap_buf0, ap_buf1, ap_buf2, ap_buf3};
static uint16_t fdn_i[4], ap_i[4];
static float fdn_lp[4];

static inline void reverb(float in, float g, float damp, float *ol, float *or_)
{
    float x = in, d[4], h[4];
    uint32_t k;
    for (k = 0; k < 4; k++) {            /* the input's allpasses */
        float *b = AP_BUF[k], y = b[ap_i[k]];
        float w = x + y * 0.6f;
        b[ap_i[k]] = w;
        x = y - w * 0.6f;
        if (++ap_i[k] >= AP_LEN[k])
            ap_i[k] = 0;
    }
    for (k = 0; k < 4; k++) {
        d[k] = FDN_BUF[k][fdn_i[k]];
        fdn_lp[k] = d[k] + (fdn_lp[k] - d[k]) * damp;
    }
    h[0] = (fdn_lp[0] + fdn_lp[1] + fdn_lp[2] + fdn_lp[3]) * 0.5f;
    h[1] = (fdn_lp[0] - fdn_lp[1] + fdn_lp[2] - fdn_lp[3]) * 0.5f;
    h[2] = (fdn_lp[0] + fdn_lp[1] - fdn_lp[2] - fdn_lp[3]) * 0.5f;
    h[3] = (fdn_lp[0] - fdn_lp[1] - fdn_lp[2] + fdn_lp[3]) * 0.5f;
    for (k = 0; k < 4; k++) {
        FDN_BUF[k][fdn_i[k]] = fm_flush(x + h[k] * g);
        if (++fdn_i[k] >= FDN_LEN[k])
            fdn_i[k] = 0;
    }
    *ol = d[0] + d[2] * 0.7f - d[3] * 0.3f;
    *or_ = d[1] + d[3] * 0.7f - d[2] * 0.3f;
}

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
static uint32_t nheld;
static uint8_t down[128];                /* physically down (keys and MIDI in): HOLD keeps the rest */
static uint32_t ndown;
static uint8_t arp_on_now, hold_now;     /* what the render last acted on */

/* published to the screen: the held set, and the notes the arp has played */
static volatile uint32_t held_mask[4];
#define VIZ_N 64u
static volatile uint32_t viz[VIZ_N];     /* note | vel << 8 | the time in 1/100 s << 16 */
static volatile uint32_t viz_w, arp_count;

/* the arp's MIDI out: each note it sent and the samples left before its note-off */
static int16_t out_note[NHELD];
static int32_t out_left[NHELD];
static void midi_note(uint32_t note, uint32_t vel)
{
    plat_midi_send(0x09u | 0x90u << 8 | note << 16 | vel << 24, TO_USB);
}
static void midi_off(uint32_t note) { plat_midi_send(0x08u | 0x80u << 8 | note << 16, TO_USB); }
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
    }
    held[i] = note;
    held_vel[i] = (uint8_t)vel;
    nheld++;
    if (!arp_on_now) {
        voice_on(note, vel, -1);
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
    }
    nheld--;
    if (!arp_on_now) {
        voice_off(note);
        midi_off((uint32_t)note);
    }
}

static void held_clear(void)
{
    while (nheld)
        held_remove(held[nheld - 1]);
}

static float arp_pos, arp_len;           /* samples into the step, and the step's length */
static int32_t arp_i, arp_dir = 1;
static volatile uint32_t sample_clock;   /* samples rendered: the falling notes' clock */

static void note_down(int32_t note, uint32_t vel)
{
    if (note < 0 || note > 127)
        return;
    if (hold_now && ndown == 0)          /* HOLD: a new chord after every key was let go replaces the last */
        held_clear();
    if (!down[note]) {
        down[note] = 1;
        ndown++;
    }
    if (nheld == 0 && arp_on_now)        /* the first note plays at once */
        arp_pos = arp_len;
    held_add(note, vel);
    held_publish();
}

static void note_up(int32_t note)
{
    if (note < 0 || note > 127 || !down[note])
        return;
    down[note] = 0;
    ndown--;
    if (!hold_now)
        held_remove(note);
    held_publish();
}

static void arp_step(void)
{
    uint32_t n = nheld, oct = (uint32_t)set.p[P_OCTAVES], len, i, vel;
    int32_t note, gate;
    if (!n)
        return;
    len = n * oct;
    switch (set.p[P_MODE]) {             /* the pattern moves every step, played or not */
    case M_UP:
        arp_i = (arp_i + 1) % (int32_t)len;
        break;
    case M_DOWN:
        arp_i = arp_i <= 0 || arp_i >= (int32_t)len ? (int32_t)len - 1 : arp_i - 1;
        break;
    case M_UPDN:
        if (len < 2) {
            arp_i = 0;
            break;
        }
        arp_i += arp_dir;
        if (arp_i >= (int32_t)len) {
            arp_dir = -1;
            arp_i = (int32_t)len - 2;
        } else if (arp_i < 0) {
            arp_dir = 1;
            arp_i = 1;
        }
        break;
    default:
        arp_i = (int32_t)rnd_n(len);
        break;
    }
    if (arp_i >= (int32_t)len)
        arp_i = 0;
    if (rnd_n(100) >= (uint32_t)set.p[P_CHANCE])   /* the chance: this step rests */
        return;
    note = held[arp_i % (int32_t)n] + 12 * (arp_i / (int32_t)n);
    if (note > 127)
        note -= 12 * ((note - 116) / 12);
    vel = held_vel[arp_i % (int32_t)n];
    vel = (uint32_t)clamp((int32_t)vel + (int32_t)rnd_n(25) - 12, 1, 127);   /* a little life */
    gate = (int32_t)(arp_len * (float)set.p[P_GATE] / 100.0f);
    if (gate < 64)
        gate = 64;
    voice_on(note, vel, gate);
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
}

/* the arp or HOLD switched: what sounds follows */
static void modes_follow(void)
{
    uint32_t i;
    if (set.arp_on != arp_on_now) {
        arp_on_now = set.arp_on;
        for (i = 0; i < NVOICE; i++)     /* every note let go into its release */
            voice_release(&vc[i]);
        out_all_off();
        if (!arp_on_now)                 /* the arp stopped: the keys still down sound again */
            for (i = 0; i < nheld; i++) {
                voice_on(held[i], held_vel[i], -1);
                midi_note((uint32_t)held[i], held_vel[i]);
            }
        else {
            for (i = 0; i < nheld; i++)
                midi_off((uint32_t)held[i]);
            arp_pos = arp_len;
        }
    }
    if (set.hold != hold_now) {
        hold_now = set.hold;
        if (!hold_now) {                 /* HOLD off: what no key holds goes */
            for (i = nheld; i-- > 0;)
                if (!down[held[i]])
                    held_remove(held[i]);
            held_publish();
        }
    }
}

static volatile uint32_t midi_count;
static void midi_in(void)                /* audio interrupt: the packets that came in */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24;
        if ((st & 0xF0u) == 0x90u && d2) {
            note_down((int32_t)d1, d2);
            midi_count++;
        } else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) {
            note_up((int32_t)d1);
        } else if ((st & 0xF0u) == 0xB0u && (d1 == 123u || d1 == 120u)) {   /* all notes off, all sound off */
            ndown = 0;
            for (d1 = 0; d1 < 128u; d1++)
                down[d1] = 0;
            held_clear();
            held_publish();
        }
    }
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
static float gain_now, dmix_now, rmix_now;

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i, k;
    float gain = (float)master_q12 / 4096.0f * 8300000.0f;   /* MASTER, read once a block */
    float beat = 60.0f / (float)set.p[P_TEMPO] * SR;
    float dtarget = fm_minf(beat * DDIV_BEATS[set.p[P_DTIME]], (float)(DLY_N - 4u));
    float fb = (float)set.p[P_DFDBK] / 100.0f, dmix = (float)set.p[P_DMIX] / 100.0f;
    float rmix = (float)set.p[P_REVERB] / 100.0f * 0.6f;
    float rg = 0.55f + 0.44f * (float)set.p[P_SIZE] / 100.0f;
    float damp = 0.05f + 0.85f * (1.0f - (float)set.p[P_TONE] / 100.0f);
    modes_follow();
    midi_in();
    events_in();
    arp_len = beat * DIV_BEATS[set.p[P_RATE]];
    if (dly_t <= 0.0f)
        dly_t = dtarget;
    for (i = 0; i < n; i++) {
        float l = 0.0f, r = 0.0f, vr, dl, dr, rl, rr, outl, outr;
        if (arp_on_now) {
            arp_pos += 1.0f;
            if (arp_pos >= arp_len) {
                arp_pos -= arp_len;
                if (arp_pos >= arp_len)
                    arp_pos = 0.0f;
                arp_step();
            }
        }
        for (k = 0; k < NVOICE; k++)
            if (vc[k].stage != ENV_OFF) {
                l += voice_tick(&vc[k], &vr);
                r += vr;
            }
        /* the delay: ping-pong, a low-pass in its feedback */
        dly_t += (dtarget - dly_t) * 0.00005f;
        dl = dly_read(dly_l, dly_t);
        dr = dly_read(dly_r, dly_t);
        dly_lp_l += (dl - dly_lp_l) * 0.45f;
        dly_lp_r += (dr - dly_lp_r) * 0.45f;
        dly_l[dly_w] = to16((l + r) * 0.5f + dly_lp_r * fb);
        dly_r[dly_w] = to16(dly_lp_l * fb);
        dly_w = (dly_w + 1u) & (DLY_N - 1u);
        dmix_now += (dmix - dmix_now) * 0.001f;
        rmix_now += (rmix - rmix_now) * 0.001f;
        outl = l + dl * dmix_now;
        outr = r + dr * dmix_now;
        /* the reverb, fed the voices and the delay */
        reverb((outl + outr) * 0.25f, rg, damp, &rl, &rr);
        outl += rl * rmix_now;
        outr += rr * rmix_now;
        gain_now += (gain - gain_now) * 0.004f;   /* ~5 ms: turning MASTER does not click */
        lr[2 * i] = (int32_t)(fm_tanhf(outl * 0.9f) * gain_now);
        lr[2 * i + 1] = (int32_t)(fm_tanhf(outr * 0.9f) * gain_now);
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
    uint32_t i;
    for (i = 0; i < NVOICE; i++) {
        vc[i].stage = ENV_OFF;
        vc[i].note = -1;
    }
    nheld = 0;
}

/* --------------------------------------------------------------- settings --- */
static void preset_load(uint32_t p)      /* a sound: its envelope, and the sound page at the middle */
{
    set.preset = (uint8_t)p;
    set.p[P_ATTACK] = PRESET[p].attack;
    set.p[P_DECAY] = PRESET[p].decay;
    set.p[P_SUSTAIN] = PRESET[p].sustain;
    set.p[P_RELEASE] = PRESET[p].release;
    set.p[P_BRIGHT] = 50;
    set.p[P_TINE] = 50;
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
    set.hold = 0;                        /* off: a key let go stops (REC latches the chord) */
}

static int settings_ok(void)
{
    uint32_t i;
    if (set.magic != SET_MAGIC || set.preset >= NPRESET || set.octave < -2 || set.octave > 2 || set.arp_on > 1u
        || set.hold > 1u)
        return 0;
    for (i = 0; i < NP; i++)
        if (set.p[i] < PAR[i].lo || set.p[i] > PAR[i].hi)
            return 0;
    return 1;
}

void app_init(void)
{
    uint32_t i;
    for (i = 0; i <= SIN_N; i++)
        sin_tab[i] = fm_sinf((float)i * FM_TWO_PI / (float)SIN_N);
    for (i = 0; i < NVOICE; i++)
        vc[i].note = -1;
    for (i = 0; i < NHELD; i++)
        out_note[i] = -1;
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || !settings_ok())
        settings_defaults();
#ifndef PLAT_HOST
    rng ^= plat_ticks24() | 1u;          /* a different piece every power-on (the host's tests repeat) */
#endif
}

/* --------------------------------------------------------------------- UI --- */
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

static void param_text(uint32_t p, char *t)  /* a parameter's value as the screen shows it */
{
    int32_t v = set.p[p];
    t[0] = 0;
    switch (PAR[p].unit) {
    case U_TIME: {
        float s = knob_secs(v);
        if (s < 1.0f) {
            int_text((int32_t)(s * 1000.0f + 0.5f), t);
            text_cat(t, " MS");
        } else {
            int32_t d = (int32_t)(s * 10.0f + 0.5f);
            char f[4] = {'.', (char)('0' + d % 10), 0, 0};
            int_text(d / 10, t);
            text_cat(t, f);
            text_cat(t, " S");
        }
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
    char t[160], v[16];
    uint32_t i;
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, APP_NAME ". ");
        text_cat(t, PRESET[set.preset].name);
        text_cat(t, set.arp_on ? ". ARP ON" : ". ARP OFF");
        text_cat(t, set.hold ? ", HOLD. " : ". ");
        text_cat(t, PAGE_NAME[ui.page]);
        for (i = 0; i < 4u; i++) {
            text_cat(t, ". ");
            text_cat(t, PAR[ui.page * 4u + i].name);
            text_cat(t, " ");
            param_text(ui.page * 4u + i, v);
            text_cat(t, v);
        }
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

static void param_turn(uint32_t p, int32_t d)
{
    int32_t v = clamp(set.p[p] + d, PAR[p].lo, PAR[p].hi);
    char t[16];
    if (v == set.p[p])
        return;
    set.p[p] = (int16_t)v;
    set_dirty = 1;
    ui.redraw = 1;
    if (ann_on()) {
        param_text(p, t);
        app_say(PAR[p].name, t);
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
        for (pg = 0; pg < NPAGE && PAGE_BTN[pg] != k; pg++)
            ;
        if (pg < NPAGE) {
            ui.page = pg;
            ui.turned_at[0] = ui.turned_at[1] = ui.turned_at[2] = ui.turned_at[3] = 0;
            app_say(PAGE_NAME[pg], "");
        } else if (k == B_OCTDN) {
            octave_step(-1);
        } else if (k == B_OCTUP) {
            octave_step(1);
        } else if (k == B_PLAY) {
            set.arp_on = (uint8_t)!set.arp_on;
            set_dirty = 1;
            app_say("ARP", set.arp_on ? "ON" : "OFF");
        } else if (k == B_REC) {
            set.hold = (uint8_t)!set.hold;
            set_dirty = 1;
            app_say("HOLD", set.hold ? "ON" : "OFF");
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
            int32_t p = ((int32_t)set.preset + e) % NPRESET;
            preset_load((uint32_t)(p < 0 ? p + NPRESET : p));
            set_dirty = 1;
            app_say("SOUND", PRESET[set.preset].name);
        } else if (k == EN_SELECT) {         /* the two left encoders: tempo and chance, on every page */
            param_turn(P_TEMPO, e);
            ui.tempo_at = now;
        } else if (k == EN_ALGO) {
            param_turn(P_CHANCE, e * 2);
            ui.chance_at = now;
        } else {
            param_turn(ui.page * 4u + (k - EN_K1), e);
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
    uint32_t btn = 1u << PAGE_BTN[ui.page], keys = 0, dim = 0, k, now_cs = sample_clock / 441u, w = viz_w, i;
    if (set.arp_on)
        btn |= LED_PLAY_GREEN;
    if (set.hold)
        btn |= 1u << B_REC;
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
        if (((now_cs - t) & 0xFFFFu) < 12u && note >= 0 && note < (int32_t)NKEYS)
            keys |= 1u << note;
    }
    keys |= ui.keys;
    plat_lights(btn, keys, 0, dim & ~keys, 1);
}

void app_input_only(void) { input(); }

static void bar(int32_t x, int32_t y, int32_t w, uint32_t p, int lit)
{
    int32_t f = (set.p[p] - PAR[p].lo) * w / (PAR[p].hi - PAR[p].lo);
    cv_rect(x, y, w, 3, C_LINE);
    cv_rect(x, y, f, 3, lit ? C_WHITE : C_HI);
}

static void draw(void)
{
    char t[24];
    uint32_t i, now = plat_ms(), now_cs = sample_clock / 441u, w = viz_w;
    /* the header: the name and the sound, or a message for a moment */
    cv_begin(240, 32, C_BLACK);
    if (now < ui.msg_until) {
        int32_t x = cv_text(10, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], C_HI);
    } else {
        cv_text(10, 6, &FONT_B, APP_NAME, C_WHITE);
        cv_text(230 - text_w(&FONT_B, PRESET[set.preset].name), 6, &FONT_B, PRESET[set.preset].name, C_HI);
    }
    cv_blit(0, 0);
    /* the page: its four knobs, two by two */
    cv_begin(240, 108, C_BLACK);
    cv_text(10, 0, &FONT_XS, PAGE_NAME[ui.page], C_GRAY);
    cv_rect(10, 15, 220, 1, C_LINE);
    for (i = 0; i < 4u; i++) {
        uint32_t p = ui.page * 4u + i;
        int32_t x = 10 + (int32_t)(i % 2u) * 116, y = 22 + (int32_t)(i / 2u) * 44;
        int lit = ui.turned_at[i] && now - ui.turned_at[i] < 400u;
        char k[4] = {'K', (char)('1' + i), 0, 0};
        cv_text(x, y, &FONT_T, k, C_DIM);
        cv_text(x + 16, y, &FONT_XS, PAR[p].name, C_GRAY);
        param_text(p, t);
        cv_text(x, y + 14, &FONT_M, t, lit ? C_WHITE : C_HI);
        bar(x, y + 39, 104, p, lit);
    }
    cv_blit(0, 34);
    /* the notes the arp plays, as they fall: time across (newest at the right, 4 s), pitch up */
    cv_begin(240, 72, C_BLACK);
    cv_rect(10, 0, 220, 1, C_LINE);
    for (i = 0; i < VIZ_N && i < w; i++) {
        uint32_t e = viz[(w - 1u - i) % VIZ_N], age = (now_cs - (e >> 16)) & 0xFFFFu;
        int32_t x, y, s;
        if (age > 400u)
            break;
        x = 228 - (int32_t)age * 218 / 400;
        y = 66 - clamp((int32_t)(e & 0xFFu) - 36, 0, 60);
        s = 1 + (int32_t)((e >> 8 & 0xFFu) / 43u);
        cv_rect(x - s, y - s, 2 * s + 1, 2 * s + 1, age < 25u ? C_WHITE : age < 120u ? C_HI : C_DIM);
    }
    cv_blit(0, 142);
    /* the state line: the arp and HOLD under the buttons that switch them, then tempo and chance (SELECT, ALGO) */
    cv_begin(240, 24, C_BLACK);
    cv_text(10, 0, &FONT_T, "PLAY", C_DIM);
    cv_text(10, 10, &FONT_XS, set.arp_on ? "ARP" : "KEYS", set.arp_on ? C_HI : C_GRAY);
    cv_text(48, 0, &FONT_T, "REC", C_DIM);
    cv_text(48, 10, &FONT_XS, set.hold ? "HOLD" : "NO HOLD", set.hold ? C_HI : C_GRAY);
    cv_text(110, 0, &FONT_T, "SELECT", C_DIM);
    param_text(P_TEMPO, t);
    cv_text(110, 10, &FONT_XS, t, ui.tempo_at && now - ui.tempo_at < 400u ? C_WHITE : C_AMB);
    cv_text(172, 0, &FONT_T, "ALGO  CHANCE", C_DIM);
    int_text(set.p[P_CHANCE], t);
    text_cat(t, "%");
    cv_text(172, 10, &FONT_XS, t, ui.chance_at && now - ui.chance_at < 400u ? C_WHITE : C_AMB);
    cv_blit(0, 216);
}

void app_ui_init(void)
{
    touch_on(1);                             /* velocity: the keys' readings on */
    lcd_fill(0, 0, 240, 240, C_BLACK);
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
    if (viz_w != last_viz || (viz_w && now - last_frame >= 50u))   /* the falling notes move: ~20 frames a second */
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
    uint32_t i, n = 0;
    if (!strcmp(what, "octave"))
        *got = set.octave;
    else if (!strcmp(what, "preset"))
        *got = set.preset;
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
    else if (!strcmp(what, "voices")) {      /* sounding */
        for (i = 0; i < NVOICE; i++)
            n += vc[i].stage != ENV_OFF;
        *got = (int)n;
    } else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strncmp(what, "p", 1) && what[1] >= '0' && what[1] <= '9')   /* pN: parameter N's value */
        *got = set.p[atoi(what + 1) % NP];
    else
        return 0;
    return 1;
}
#endif
