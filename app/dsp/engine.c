/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Nick Culbertson */
/* Mighty Arp's sound (engine.h). Voices -> tremolo -> chorus -> ping-pong delay -> reverb (with shimmer) -> out. */
#include "engine.h"
#include "plat.h"
#include "fastmath.h"

#define SR ENG_SR

/* ------------------------------------------------------------------ tables --- */
#define SIN_N 2048u
static float sin_tab[SIN_N + 1];
/* ph in turns, -64 .. 64 (a phase plus its modulation: far inside that): the table, read between its points. The
 * offset makes it positive, so a truncation is its floor (no floorf: this runs four times a voice a sample) */
#define PH_OFS 64.0f
static inline float sin_t(float ph)
{
    float x = (ph + PH_OFS) * (float)SIN_N, f;
    uint32_t i = (uint32_t)x;
    f = x - (float)i;
    i &= SIN_N - 1u;
    return sin_tab[i] + (sin_tab[i + 1] - sin_tab[i]) * f;
}

/* the eight waves, as the TX81Z's: W1 the sine, W2 its halves squared, W3 W4 their positive halves, W5..W8 the same
 * at twice the speed in the first half of the turn and silent in the second */
static inline float wave(float ph, uint32_t w)
{
    float s, f;
    switch (w) {
    case 0:
        return sin_t(ph);
    case 1:
        s = sin_t(ph);
        return s * fm_fabsf(s);
    case 2:
        s = sin_t(ph);
        return s > 0.0f ? s : 0.0f;
    case 3:
        s = sin_t(ph);
        return s > 0.0f ? s * s : 0.0f;
    default:
        f = ph + PH_OFS;
        f -= (float)(uint32_t)f;
        if (f >= 0.5f)
            return 0.0f;
        s = sin_t(2.0f * f);
        switch (w) {
        case 4:
            return s;
        case 5:
            return s * fm_fabsf(s);
        case 6:
            return fm_fabsf(s);
        default:
            return s * s;
        }
    }
}

static uint32_t erng = 0x2545F491u;
static inline uint32_t rnd(void)         /* xorshift32 */
{
    erng ^= erng << 13;
    erng ^= erng >> 17;
    erng ^= erng << 5;
    return erng;
}
static inline float rnd_f(void) { return (float)(rnd() >> 8) * (1.0f / 8388608.0f) - 1.0f; }   /* -1 .. 1 */

/* the per-sample factor of a fall to 1/1000 in s seconds */
static float fall_coef(float s) { return fm_exp2f(-9.965784f / (fm_maxf(s, 0.0005f) * SR)); }

/* ------------------------------------------------------------- algorithms --- */
/* MOD[alg][k]: the operators (bit j = operator j + 1) that modulate operator k + 1. They are computed 4, 3, 2, 1, so
 * a modulator always comes first. CAR[alg]: the carriers */
static const uint8_t MOD[8][4] = {
    {0x2, 0x4, 0x8, 0},                  /* 1: 4 > 3 > 2 > 1 */
    {0x2, 0xC, 0, 0},                    /* 2: (3 + 4) > 2 > 1 */
    {0xA, 0x4, 0, 0},                    /* 3: 3 > 2 > 1, 4 > 1 */
    {0x6, 0, 0x8, 0},                    /* 4: 4 > 3 > 1, 2 > 1 */
    {0x2, 0, 0x8, 0},                    /* 5: 2 > 1, 4 > 3 */
    {0x8, 0x8, 0x8, 0},                  /* 6: 4 > 1, 2, 3 */
    {0, 0, 0x8, 0},                      /* 7: 4 > 3, 2, 1 */
    {0, 0, 0, 0},                        /* 8: four carriers */
};
static const uint8_t CAR[8] = {0x1, 0x1, 0x1, 0x1, 0x5, 0x7, 0x7, 0xF};
static const float DSIGN[4] = {-1.0f, 0.5f, 1.0f, -0.5f};   /* DETUNE: each operator's way */

/* ------------------------------------------------------------------ voices --- */
typedef struct {
    int32_t note, gate;                  /* gate: samples before the arp releases it; -1 a key holds it */
    uint32_t age;
    uint8_t on, rel, shed, wv[4], att[4];   /* att[k]: operator k still rising; shed: fading out for the CPU */
    float env[4], pk[4], ai[4], tgt[4], kk[4], sus[4], dk[4], rk[4];
    float lvl[4], lvl0[4], cl[4], cr[4], ic[4];   /* lvl0: before the high-note taper (voice_pitch); ic: 1 a
                                                   * carrier (its envelope ends the voice) */
    uint8_t alg;                         /* its algorithm, from the note on */
    float ph[4], inc[4], rat[4];
    float fb1, fb2, fbk;
    float f, f_to;                       /* the note, turns a sample, gliding to f_to */
    float drift, drift_to;
    uint32_t drift_n;
} voice_t;
static voice_t vc[ENG_NVOICE];
static uint32_t vc_age, vc_cap = ENG_NVOICE;   /* the voices a new note may take: fewer after an overload */
static int32_t shed_note = -1;           /* the note eng_shed last let go (the host's tests) */

/* the second core (eng_cpu2): it renders voices C2_FIRST.. into its own buffers while this one renders the rest
 * and then the effects; new notes go to whichever side has fewer (eng_note_on) */
#define C2_FIRST 4u
static int (*c2_run)(void (*)(uint32_t), uint32_t);
static int (*c2_wait)(void);

static const eng_sound_t *snd;
static eng_params_t prm;
static float lfo_ph, lfo_val, lfo_sh, lfo_dg, lfo_dg_to, glide_k, drift_k;
static float trem_l = 1.0f, trem_r = 1.0f, trem_l_to = 1.0f, trem_r_to = 1.0f;

/* the operators' speeds from the note, the glide and the LFO; and their levels, tapered high up: there a modulator's
 * sidebands fold back past half the sample rate as noise. A modulator fades from 8 kHz to nothing at 20, a carrier from
 * 16 to 20, smoothly (no cliff between neighbouring notes), and as the pitch moves (glides, bends, legato) */
static void voice_pitch(voice_t *v)
{
    float x = prm.lfo_shape == LFO_DRIFT ? v->drift : lfo_val;
    float m = v->f * fm_exp2f((x * prm.vib + prm.bend) * (1.0f / 12.0f));
    uint32_t k;
    for (k = 0; k < 4u; k++) {
        float hz = m * v->rat[k] * SR, lo = v->ic[k] > 0.0f ? 16000.0f : 8000.0f;
        v->inc[k] = m * v->rat[k];
        v->lvl[k] = hz <= lo ? v->lvl0[k] : hz >= 20000.0f ? 0.0f : v->lvl0[k] * (20000.0f - hz) / (20000.0f - lo);
    }
}

static void voice_release(voice_t *v)
{
    uint32_t k;
    if (!v->on || v->rel)
        return;
    v->rel = 1;
    for (k = 0; k < 4u; k++) {
        v->att[k] = 0;
        v->tgt[k] = 0.0f;
        v->kk[k] = v->rk[k];
    }
}

void eng_note_on(int32_t note, uint32_t vel, int32_t gate)
{
    const eng_sound_t *s = snd;
    voice_t *v = 0;
    uint32_t i, k, oldest = 0xFFFFFFFFu, car, c;
    float f, velf, amp, bright, kscale, dscale, pos, rk;
    if (!s)
        return;
    f = 440.0f * fm_exp2f((float)(note - 69) * (1.0f / 12.0f)) / SR;
    if (s->mono) {
        v = &vc[0];
        if (v->on && !v->rel) {          /* legato: the envelopes go on, the pitch glides */
            v->note = note;
            v->gate = gate;
            v->age = ++vc_age;
            v->f_to = f;
            if (s->glide <= 0.0f)
                v->f = f;
            voice_pitch(v);
            return;
        }
    } else {
        for (i = 0; i < ENG_NVOICE; i++)   /* the same note again: that voice, so it never stacks */
            if (vc[i].on && vc[i].note == note)
                v = &vc[i];
        if (!v && eng_active() < vc_cap) {   /* else a free one (under the cap) on the side with fewer sounding (the
                                              * two cores' work overlaps only so), else the oldest releasing, else
                                              * the oldest */
            uint32_t lo = 0, hi = 0, first;
            for (i = 0; i < ENG_NVOICE; i++)
                if (vc[i].on)
                    *(i < C2_FIRST ? &lo : &hi) += 1u;
            first = c2_run && hi <= lo ? C2_FIRST : 0u;
            for (i = 0; i < ENG_NVOICE && !v; i++) {
                uint32_t j = (first + i) % ENG_NVOICE;
                if (!vc[j].on)
                    v = &vc[j];
            }
        }
        if (!v)
            for (i = 0; i < ENG_NVOICE; i++)
                if (vc[i].on && vc[i].rel && vc[i].age < oldest)
                    oldest = vc[i].age, v = &vc[i];
        if (!v)
            for (i = 0, oldest = 0xFFFFFFFFu; i < ENG_NVOICE; i++)
                if (vc[i].on && vc[i].age < oldest)
                    oldest = vc[i].age, v = &vc[i];
        if (!v)
            v = &vc[0];
    }
    car = CAR[prm.alg & 7u];
    velf = (float)vel / 127.0f;
    amp = 0.5f * (1.0f - prm.velo * (1.0f - velf) * 0.85f);
    kscale = fm_clampf(fm_exp2f((float)(60 - note) / 48.0f), 0.6f, 1.6f);   /* low notes a little brighter */
    dscale = fm_clampf(fm_exp2f((float)(60 - note) / 36.0f), 0.35f, 2.0f);  /* and longer */
    bright = prm.tone * (1.0f - prm.velo * (1.0f - velf) * 0.7f) * kscale;
    pos = fm_clampf((float)(note - 60) / 36.0f, -0.5f, 0.5f) * 0.6f;         /* low notes left, high right */
    rk = fall_coef(prm.rel);
    if (!v->on) {
        for (k = 0; k < 4u; k++) {
            v->ph[k] = 0.0f;
            v->env[k] = 0.0f;
        }
        v->fb1 = v->fb2 = 0.0f;
    }
    if (!s->mono || s->glide <= 0.0f || v->f <= 0.0f)
        v->f = f;
    v->f_to = f;
    v->note = note;
    v->gate = gate;
    v->age = ++vc_age;
    for (k = 0, c = 0; k < 4u; k++) {
        const eng_op_t *o = &s->op[k];
        uint32_t is_car = car >> k & 1u, own = !is_car || o->strike;
        float a, d, su, pk;
        v->wv[k] = (uint8_t)(is_car || prm.wave > 7u ? o->wave & 7u : prm.wave);
        v->rat[k] = o->ratio * (is_car ? 1.0f : prm.ratio)
                    * fm_exp2f((o->det + DSIGN[k] * prm.detune * 14.0f) * (1.0f / 1200.0f));

        if (own) {
            a = o->att;
            d = o->dec * dscale;
            su = o->sus;
            pk = is_car ? 1.0f : su + (1.0f - su) * prm.strike;   /* STRIKE: the modulators' burst */
        } else {
            a = prm.att;
            d = prm.dec * dscale;
            su = prm.sus;
            pk = 1.0f;
        }
        v->pk[k] = pk;
        v->ai[k] = a < 0.0005f ? 1e9f : pk / (a * SR);
        v->sus[k] = su;
        v->dk[k] = fall_coef(d);
        v->rk[k] = rk;
        v->att[k] = 1;
        v->tgt[k] = su;
        v->kk[k] = v->dk[k];
        if (is_car) {
            float sp = 0.0f, pp;
            if (car != 0x1u && car != 0x0u)   /* two or more carriers: spread across the voice's place */
                sp = (c & 1u ? 0.25f : -0.25f) * (0.4f + prm.detune);
            pp = fm_clampf(pos + sp, -0.9f, 0.9f);
            a = o->lvl * amp * (o->strike ? prm.strike : 1.0f);
            v->lvl0[k] = a;
            v->cl[k] = 0.5f - 0.5f * pp;
            v->cr[k] = 0.5f + 0.5f * pp;
            v->ic[k] = 1.0f;
            c++;
        } else {
            v->lvl0[k] = o->lvl * bright;
            v->cl[k] = v->cr[k] = v->ic[k] = 0.0f;
        }
    }
    v->alg = (uint8_t)(prm.alg & 7u);
    v->fbk = prm.fb ? fm_exp2f((float)prm.fb - 9.0f) : 0.0f;   /* 7: a quarter turn from the last two */
    v->on = 1;
    v->rel = 0;
    v->shed = 0;
    voice_pitch(v);
}

void eng_note_off(int32_t note)
{
    uint32_t i;
    for (i = 0; i < ENG_NVOICE; i++)
        if (vc[i].on && vc[i].note == note && vc[i].gate < 0)
            voice_release(&vc[i]);
}

void eng_release_all(void)
{
    uint32_t i;
    for (i = 0; i < ENG_NVOICE; i++)
        voice_release(&vc[i]);
}

uint32_t eng_active(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < ENG_NVOICE; i++)
        n += vc[i].on;
    return n;
}

int32_t eng_sounding(int32_t note)
{
    uint32_t i;
    for (i = 0; i < ENG_NVOICE; i++)
        if (vc[i].on && !vc[i].rel && vc[i].note == note)
            return 1;
    return 0;
}

/* the render ran late: one voice out, quickly (the oldest releasing, else the oldest), and one voice fewer for new
 * notes until eng_cap gives it back. Never all at once */
void eng_shed(void)
{
    voice_t *v = 0;
    uint32_t i, k, oldest = 0xFFFFFFFFu, n = 0;
    for (i = 0; i < ENG_NVOICE; i++)     /* the voices sounding, but those already going */
        n += vc[i].on && !vc[i].shed;
    for (i = 0; i < ENG_NVOICE; i++)
        if (vc[i].on && !vc[i].shed && vc[i].rel && vc[i].age < oldest)
            oldest = vc[i].age, v = &vc[i];
    if (!v)                              /* none releasing: the oldest held */
        for (i = 0; i < ENG_NVOICE; i++)
            if (vc[i].on && !vc[i].shed && vc[i].age < oldest)
                oldest = vc[i].age, v = &vc[i];
    if (n > 4u && n - 1u < vc_cap)
        vc_cap = n - 1u;
    if (!v || n <= 4u)                   /* four always sound */
        return;
    shed_note = v->note;
    voice_release(v);
    v->shed = 1;
    for (k = 0; k < 4u; k++)
        v->kk[k] = v->rk[k] = 0.9985f;   /* ~0.1 s */
}

int32_t eng_shed_note(void) { return shed_note; }

uint32_t eng_cap(int up)                 /* one more voice back (up), or just ask: the cap */
{
    if (up && vc_cap < ENG_NVOICE)
        vc_cap++;
    return vc_cap;
}

/* one operator's envelope, a sample: rising, then falling to its target (the sustain, or 0 once released) */
#define ENV_STEP(k)                                          \
    do {                                                     \
        if (at##k) {                                         \
            if ((e##k += ai##k) >= pk##k) {                  \
                e##k = pk##k;                                \
                at##k = 0;                                   \
            }                                                \
        } else                                               \
            e##k = tg##k + (e##k - tg##k) * kk##k;           \
    } while (0)

/* a voice over samples [a, b), one version for each algorithm (alg a constant: what does not modulate or sound is
 * not computed), with every operator a plain sine (sine 1: no wave to choose) or each its own wave */
#define AI __attribute__((always_inline)) static inline
AI void voice_loop(voice_t *v, float *l, float *r, uint32_t a, uint32_t b, const uint32_t alg, const int sine)
{
    float e0 = v->env[0], e1 = v->env[1], e2 = v->env[2], e3 = v->env[3];
    float ai0 = v->ai[0], ai1 = v->ai[1], ai2 = v->ai[2], ai3 = v->ai[3];
    float pk0 = v->pk[0], pk1 = v->pk[1], pk2 = v->pk[2], pk3 = v->pk[3];
    float tg0 = v->tgt[0], tg1 = v->tgt[1], tg2 = v->tgt[2], tg3 = v->tgt[3];
    float kk0 = v->kk[0], kk1 = v->kk[1], kk2 = v->kk[2], kk3 = v->kk[3];
    uint32_t at0 = v->att[0], at1 = v->att[1], at2 = v->att[2], at3 = v->att[3];
    float p0 = v->ph[0], p1 = v->ph[1], p2 = v->ph[2], p3 = v->ph[3];
    float n0 = v->inc[0], n1 = v->inc[1], n2 = v->inc[2], n3 = v->inc[3];
    float lv0 = v->lvl[0], lv1 = v->lvl[1], lv2 = v->lvl[2], lv3 = v->lvl[3];
    float cl0 = v->cl[0], cl1 = v->cl[1], cl2 = v->cl[2], cl3 = v->cl[3];
    float cr0 = v->cr[0], cr1 = v->cr[1], cr2 = v->cr[2], cr3 = v->cr[3];
    float fb1 = v->fb1, fb2 = v->fb2, fbk = v->fbk;
    uint32_t w0 = v->wv[0], w1 = v->wv[1], w2 = v->wv[2], w3 = v->wv[3], i, rel = v->rel;
    int32_t gate = v->gate;
#define W(x, w) (sine || !(w) ? sin_t(x) : wave((x), (w)))   /* a plain sine here too, without the call */
#define MODS(k, j) (MOD[alg][k] >> (j) & 1u)
#define IS_CAR(k) (CAR[alg] >> (k) & 1u)
    for (i = a; i < b; i++) {
        float o0, o1 = 0.0f, o2, o3, raw, m;
        ENV_STEP(3);
        ENV_STEP(2);
        ENV_STEP(1);
        ENV_STEP(0);
        raw = e3 * W(p3 + fbk * (fb1 + fb2), w3);
        fb2 = fb1;
        fb1 = raw;
        o3 = raw * lv3;
        m = MODS(2, 3) ? o3 : 0.0f;
        o2 = e2 * lv2 * W(p2 + m, w2);
        m = (MODS(1, 2) ? o2 : 0.0f) + (MODS(1, 3) ? o3 : 0.0f);
        o1 = e1 * lv1 * W(p1 + m, w1);
        m = (MODS(0, 1) ? o1 : 0.0f) + (MODS(0, 2) ? o2 : 0.0f) + (MODS(0, 3) ? o3 : 0.0f);
        o0 = e0 * lv0 * W(p0 + m, w0);
        l[i] += cl0 * o0 + (IS_CAR(1) ? cl1 * o1 : 0.0f) + (IS_CAR(2) ? cl2 * o2 : 0.0f) + (IS_CAR(3) ? cl3 * o3 : 0.0f);
        r[i] += cr0 * o0 + (IS_CAR(1) ? cr1 * o1 : 0.0f) + (IS_CAR(2) ? cr2 * o2 : 0.0f) + (IS_CAR(3) ? cr3 * o3 : 0.0f);
        p0 += n0;
        p1 += n1;
        p2 += n2;
        p3 += n3;
        if (p0 >= 1.0f) p0 -= (float)(int32_t)p0;
        if (p1 >= 1.0f) p1 -= (float)(int32_t)p1;
        if (p2 >= 1.0f) p2 -= (float)(int32_t)p2;
        if (p3 >= 1.0f) p3 -= (float)(int32_t)p3;
        if (gate > 0 && --gate == 0 && !rel) {   /* the arp's gate closed: the release, in place */
            rel = 1;
            at0 = at1 = at2 = at3 = 0;
            tg0 = tg1 = tg2 = tg3 = 0.0f;
            kk0 = v->rk[0];
            kk1 = v->rk[1];
            kk2 = v->rk[2];
            kk3 = v->rk[3];
        }
        if (rel && !(i & 15u)                    /* -60 dB: done (looked at every 16 samples) */
            && e0 + (IS_CAR(1) ? e1 : 0.0f) + (IS_CAR(2) ? e2 : 0.0f) + (IS_CAR(3) ? e3 : 0.0f) < 0.001f) {
            v->on = 0;
            v->note = -1;
            break;
        }
    }
#undef W
#undef MODS
#undef IS_CAR
    v->env[0] = e0, v->env[1] = e1, v->env[2] = e2, v->env[3] = e3;
    v->tgt[0] = tg0, v->tgt[1] = tg1, v->tgt[2] = tg2, v->tgt[3] = tg3;
    v->kk[0] = kk0, v->kk[1] = kk1, v->kk[2] = kk2, v->kk[3] = kk3;
    v->att[0] = (uint8_t)at0, v->att[1] = (uint8_t)at1, v->att[2] = (uint8_t)at2, v->att[3] = (uint8_t)at3;
    v->ph[0] = p0, v->ph[1] = p1, v->ph[2] = p2, v->ph[3] = p3;
    v->fb1 = fb1, v->fb2 = fb2;
    v->gate = gate;
    v->rel = (uint8_t)rel;
}

#define VL(alg) \
    case alg * 2u: voice_loop(v, l, r, a, b, alg, 0); break; \
    case alg * 2u + 1u: voice_loop(v, l, r, a, b, alg, 1); break;
static void voice_run(voice_t *v, float *l, float *r, uint32_t a, uint32_t b)
{
    switch ((uint32_t)v->alg * 2u + !(v->wv[0] | v->wv[1] | v->wv[2] | v->wv[3])) {
    VL(0u) VL(1u) VL(2u) VL(3u) VL(4u) VL(5u) VL(6u) VL(7u)
    default: break;
    }
}
#undef VL

static float c2_l[ENG_BLOCK], c2_r[ENG_BLOCK];
static uint32_t c2_a, c2_b;

static void c2_job(uint32_t arg)
{
    uint32_t i, k;
    (void)arg;
    for (i = c2_a; i < c2_b; i++)
        c2_l[i] = c2_r[i] = 0.0f;
    for (k = C2_FIRST; k < ENG_NVOICE; k++)
        if (vc[k].on)
            voice_run(&vc[k], c2_l, c2_r, c2_a, c2_b);
}

void eng_cpu2(int (*run)(void (*)(uint32_t), uint32_t), int (*wait)(void))
{
    c2_run = run;
    c2_wait = wait;
}

static uint8_t eng_clearing;             /* the buffers being cleared after a reset (eng_out): the voices wait,
                                          * so a note played meanwhile keeps its attack */

void eng_voices(float *l, float *r, uint32_t a, uint32_t b)
{
    uint32_t i, k, split = 0;
    if (a >= b || eng_clearing)
        return;
    for (k = C2_FIRST; k < ENG_NVOICE && !split; k++)
        split = vc[k].on;
    if (split && c2_run) {
        c2_a = a;
        c2_b = b;
        split = (uint32_t)c2_run(c2_job, 0);
    } else
        split = 0;
    for (k = 0; k < (split ? C2_FIRST : ENG_NVOICE); k++)
        if (vc[k].on)
            voice_run(&vc[k], l, r, a, b);
    if (!split)
        return;
    if (c2_wait() != 0) {                /* it was given up mid-job (and stopped): its voices' state may be half
                                          * written, so they end here, never run twice or resumed. One core from now */
        for (k = C2_FIRST; k < ENG_NVOICE; k++) {
            vc[k].on = 0;
            vc[k].note = -1;
        }
        return;
    }
    for (i = a; i < b; i++) {
        l[i] += c2_l[i];
        r[i] += c2_r[i];
    }
}

/* ------------------------------------------------------------------ the LFO --- */
void eng_set(const eng_sound_t *s, const eng_params_t *p, uint32_t n)
{
    float d = p->lfo_hz * (float)n / SR, x;
    uint32_t i, k, wrapped;
    snd = s;
    prm = *p;
    lfo_ph += d;
    wrapped = lfo_ph >= 1.0f;
    if (wrapped)
        lfo_ph -= fm_floorf(lfo_ph);
    drift_k = 1.0f - fm_exp2f(-9.0f * d);  /* the drift's smoothing: a cycle's worth */
    if (wrapped) {
        lfo_sh = rnd_f();
        lfo_dg_to = rnd_f();
    }
    lfo_dg += (lfo_dg_to - lfo_dg) * drift_k;
    switch (p->lfo_shape) {
    case LFO_SINE:
        x = sin_t(lfo_ph);
        break;
    case LFO_TRI:
        x = 4.0f * fm_fabsf(lfo_ph - 0.5f) - 1.0f;
        break;
    case LFO_SQUARE:
        x = lfo_ph < 0.5f ? 1.0f : -1.0f;
        break;
    case LFO_SH:
        x = lfo_sh;
        break;
    default:
        x = lfo_dg;
        break;
    }
    lfo_val = x;
    trem_l_to = 1.0f - p->trem * (0.5f + 0.5f * x);   /* the tremolo across the stereo: a Rhodes' */
    trem_r_to = 1.0f - p->trem * (0.5f - 0.5f * x);
    glide_k = s && s->glide > 0.0f ? fm_exp2f(-1.442695f * (float)n / (s->glide * SR)) : 0.0f;
    for (i = 0; i < ENG_NVOICE; i++) {
        voice_t *v = &vc[i];
        if (!v->on)
            continue;
        v->f = v->f_to + (v->f - v->f_to) * glide_k;
        if (v->drift_n == 0u) {          /* each voice its own wander, a new place once a cycle */
            v->drift_to = rnd_f();
            v->drift_n = (uint32_t)(SR / (fm_maxf(p->lfo_hz, 0.05f) * (float)n)) + (rnd() & 7u);
        } else
            v->drift_n--;
        v->drift += (v->drift_to - v->drift) * drift_k;
        for (k = 0; k < 4u; k++)         /* an envelope fallen to nothing stays nothing (no denormals) */
            if (!v->att[k] && v->env[k] < 1e-7f)
                v->env[k] = 0.0f;
        voice_pitch(v);
    }
}

/* ------------------------------------------------------------------ effects --- */
/* the chorus: a mono line, two taps swinging in opposite directions */
#define CH_N 1024u
static PLAT_POOL float ch_buf[CH_N];
static uint32_t ch_w;
static float ch_ph, ch_now;

static inline float ch_read(float d)
{
    float p = (float)ch_w - d, f;
    int32_t i;
    if (p < 0.0f)
        p += (float)CH_N;
    i = (int32_t)p;
    f = p - (float)i;
    return ch_buf[i & (CH_N - 1)] + (ch_buf[(i + 1) & (CH_N - 1)] - ch_buf[i & (CH_N - 1)]) * f;
}

/* the ping-pong delay: two lines of int16 at half the rate (each slot two samples, averaged), so up to 1.49 s; the
 * read point slews to a new time like tape */
#define DLY_N 32768u
static PLAT_POOL int16_t dly_l[DLY_N], dly_r[DLY_N];
static uint32_t dly_w, dly_odd;          /* the slot being filled, and which of its two samples this is */
static float dly_t, dly_lp_l, dly_lp_r, dly_pre_l, dly_pre_r, dly_acc_l, dly_acc_r, dmix_now, rmix_now;

static inline float dly_read(const int16_t *b, float d)   /* d samples back */
{
    float p = (float)dly_w + (dly_odd ? 0.5f : 0.0f) - d * 0.5f, f;
    int32_t i;
    if (p < 0.0f)
        p += (float)DLY_N;
    i = (int32_t)p;
    f = p - (float)i;
    return ((float)b[i & (DLY_N - 1)] * (1.0f - f) + (float)b[(i + 1) & (DLY_N - 1)] * f) * (1.0f / 16384.0f);
}
static inline int16_t to16(float x) { return (int16_t)fm_clampf(x * 16384.0f, -32767.0f, 32767.0f); }

/* the reverb: four lines fed back through a Hadamard matrix, each with a low-pass, after four allpasses that smear
 * the input. The lines are int16 (x 8192: room to +-4), two of them slowly moving so a long tail never rings; their
 * gains set the decay time. SHIMMER: line 1 partly read through an octave-up pitch shifter, so the tail climbs */
#define RV_PAD 16u
static const uint16_t RV_LEN[4] = {5623, 6733, 8111, 9791};
static PLAT_POOL int16_t rv_b0[5623 + RV_PAD], rv_b1[6733 + RV_PAD], rv_b2[8111 + RV_PAD], rv_b3[9791 + RV_PAD];
static int16_t *const RV_BUF[4] = {rv_b0, rv_b1, rv_b2, rv_b3};
static uint16_t rv_w[4];
static float rv_lp[4], rv_g[4], rv_secs_now, rv_mph1, rv_mph2;
static const uint16_t AP_LEN[4] = {241, 167, 557, 431};
static PLAT_POOL float ap_b0[241], ap_b1[167], ap_b2[557], ap_b3[431];
static float *const AP_BUF[4] = {ap_b0, ap_b1, ap_b2, ap_b3};
static uint16_t ap_i[4];
#define SH_N 4096u
#define SH_D 2048.0f
static PLAT_POOL float sh_buf[SH_N];
static uint32_t sh_w, sh_n;              /* sh_n: the window's place, 0 .. SH_D - 1 */

/* a number that is not one (infinite or NaN): in a feedback loop it would sound forever, so the output checks */
static inline int finite_f(float x)
{
    union { float f; uint32_t u; } b;
    b.f = x;
    return (b.u & 0x7F800000u) != 0x7F800000u;
}

static inline float rv_read(uint32_t k, float d)   /* line k, d samples back (fractional) */
{
    uint32_t size = RV_LEN[k] + RV_PAD;
    float p = (float)rv_w[k] - d, f;
    int32_t i, j;
    if (p < 0.0f)
        p += (float)size;
    i = (int32_t)p;
    f = p - (float)i;
    j = i + 1 >= (int32_t)size ? 0 : i + 1;
    return ((float)RV_BUF[k][i] + ((float)RV_BUF[k][j] - (float)RV_BUF[k][i]) * f) * (1.0f / 8192.0f);
}

static inline float rv_at(uint32_t k, uint32_t d)   /* line k, d whole samples back */
{
    int32_t i = (int32_t)rv_w[k] - (int32_t)d;
    if (i < 0)
        i += (int32_t)(RV_LEN[k] + RV_PAD);
    return (float)RV_BUF[k][i] * (1.0f / 8192.0f);
}

static inline float sh_at(uint32_t d) { return sh_buf[(sh_w - d) & (SH_N - 1u)]; }   /* d whole samples back */

/* m1, m3: lines 2 and 4's movement now, in samples (eng_out steps them across the block) */
static inline int reverb(float in, float damp, float shim, float m1, float m3, float *ol, float *or_)   /* 0: its
                                                                                                         * input went
                                                                                                         * bad */
{
    float x = in, d[4], h[4];
    uint32_t k;
    int ok;
    for (k = 0; k < 4u; k++) {           /* the input's allpasses */
        float *b = AP_BUF[k], y = b[ap_i[k]];
        float w = fm_flush(x + y * 0.62f);
        b[ap_i[k]] = w;
        x = y - w * 0.62f;
        if (++ap_i[k] >= AP_LEN[k])
            ap_i[k] = 0;
    }
    d[0] = rv_at(0, RV_LEN[0]);
    d[1] = rv_read(1, (float)RV_LEN[1] - 7.0f + m1);
    d[2] = rv_at(2, RV_LEN[2]);
    d[3] = rv_read(3, (float)RV_LEN[3] - 7.0f + m3);
    for (k = 0; k < 4u; k++)
        rv_lp[k] = fm_flush(d[k] + (rv_lp[k] - d[k]) * damp);
    /* the shimmer: two taps sliding through a window of SH_D, half a window apart, crossfaded (sin^2 and cos^2):
     * an octave up. It always runs (so turned up again it holds the tail now, not one from before); every delay is
     * a whole number of samples */
    sh_buf[sh_w] = rv_lp[0];
    sh_w = (sh_w + 1u) & (SH_N - 1u);
    sh_n = (sh_n + 1u) & ((uint32_t)SH_D - 1u);
    if (shim > 0.0f) {
        uint32_t n2 = (sh_n + (uint32_t)SH_D / 2u) & ((uint32_t)SH_D - 1u);
        float s = sin_tab[sh_n >> 1], g = s * s;   /* sin(pi p), p = sh_n / SH_D: the table's 2 pi over SH_D / 2 */
        float y = g * sh_at(2u + (uint32_t)SH_D - sh_n) + (1.0f - g) * sh_at(2u + (uint32_t)SH_D - n2);
        rv_lp[0] = rv_lp[0] * (1.0f - shim) + y * shim;
    }
    for (k = 0; k < 4u; k++)
        h[k] = rv_lp[k] * rv_g[k];
    {
        float a = (h[0] + h[1] + h[2] + h[3]) * 0.5f, b = (h[0] - h[1] + h[2] - h[3]) * 0.5f;
        float c = (h[0] + h[1] - h[2] - h[3]) * 0.5f, e = (h[0] - h[1] - h[2] + h[3]) * 0.5f;
        h[0] = a, h[1] = b, h[2] = c, h[3] = e;
    }
    for (k = 0; k < 4u; k++)
        h[k] = (x + h[k]) * 8192.0f;
    ok = finite_f(h[0] + h[1] + h[2] + h[3]);
    if (!ok)                             /* a bad number: caught here, before int16 (a NaN there is undefined); the
                                          * lines get silence, and the output guard clears the rest */
        h[0] = h[1] = h[2] = h[3] = 0.0f;
    for (k = 0; k < 4u; k++) {
        uint32_t size = RV_LEN[k] + RV_PAD;
        float q = fm_clampf(h[k], -32767.0f, 32767.0f), r = q >= 0.0f ? 0.5f : -0.5f;
        if (q < 4.0f && q > -4.0f)       /* rounded (truncation would eat quiet tails), but the last few steps
                                          * truncated, so a tail never rests at +-1 for ever */
            r = 0.0f;
        RV_BUF[k][rv_w[k]] = (int16_t)(q + r);
        if (++rv_w[k] >= size)
            rv_w[k] = 0;
    }
    *ol = rv_lp[0] + rv_lp[2] * 0.7f - rv_lp[3] * 0.3f;
    *or_ = rv_lp[1] + rv_lp[3] * 0.7f - rv_lp[2] * 0.3f;
    return ok;
}

/* after a reset the buffers are cleared a part a block (16 KB: about 15 blocks, 90 ms of silence), never all at once
 * in the audio interrupt */
typedef struct {
    int16_t *w;                          /* 16-bit lines, or */
    float *f;                            /* float ones */
    uint32_t n;
} clr_t;
static const clr_t CLR[] = {
    {dly_l, 0, DLY_N}, {dly_r, 0, DLY_N}, {rv_b0, 0, 5623 + RV_PAD}, {rv_b1, 0, 6733 + RV_PAD},
    {rv_b2, 0, 8111 + RV_PAD}, {rv_b3, 0, 9791 + RV_PAD}, {0, ap_b0, 241}, {0, ap_b1, 167}, {0, ap_b2, 557},
    {0, ap_b3, 431}, {0, ch_buf, CH_N}, {0, sh_buf, SH_N},
};
#define NCLR (sizeof CLR / sizeof CLR[0])
#define CLR_BYTES 16384u
static uint32_t clr_i = NCLR, clr_at;    /* the buffer being cleared, and how far: done at NCLR */

static int clearing(void)                /* a part more cleared: 1 while there is more */
{
    uint32_t bytes = 0;
    while (clr_i < NCLR && bytes < CLR_BYTES) {
        const clr_t *c = &CLR[clr_i];
        uint32_t i, e = clr_at + (CLR_BYTES - bytes) / (c->w ? 2u : 4u);
        if (e == clr_at)                 /* less than an element of room left: the next block */
            break;
        if (e > c->n)
            e = c->n;
        if (c->w)
            for (i = clr_at; i < e; i++)
                c->w[i] = 0;
        else
            for (i = clr_at; i < e; i++)
                c->f[i] = 0.0f;
        bytes += (e - clr_at) * (c->w ? 2u : 4u);
        clr_at = e;
        if (clr_at >= c->n) {
            clr_i++;
            clr_at = 0;
        }
    }
    return clr_i < NCLR;
}

static float dc_xl, dc_yl, dc_xr, dc_yr, gain_now;

/* the output's soft limit: a cubic, flat from 1.5 (cheaper than tanh, which it follows to a few percent) */
static inline float soft_clip(float x)
{
    x = fm_clampf(x, -1.5f, 1.5f);
    return x - 0.148148f * x * x * x;
}

int eng_out(float *l, float *r, int32_t *lr, uint32_t n, float gain)
{
    const eng_params_t *p = &prm;
    uint32_t i, k;
    int bad = 0;
    float dtarget = fm_clampf(p->dly_samples, 8.0f, ENG_DLY_MAX), fb = p->dly_fb;
    float damp = 0.05f + 0.85f * p->rev_dark, shim = p->shimmer * 0.7f;
    float tl = trem_l, tr = trem_r, dtl = (trem_l_to - trem_l) / (float)n, dtr = (trem_r_to - trem_r) / (float)n;
    float ch_s, ch_ds, rm1, rm1_d, rm3, rm3_d;
    /* the slow movements (the chorus's sweep, the reverb lines'), at the block's two ends and stepped between */
    ch_s = sin_t(ch_ph) * 170.0f;
    rm1 = 6.0f * sin_t(rv_mph1);
    rm3 = 6.0f * sin_t(rv_mph2);
    ch_ph += 0.55f * (float)n / SR;
    rv_mph1 += 0.31f * (float)n / SR;
    rv_mph2 += 0.43f * (float)n / SR;
    if (ch_ph >= 1.0f) ch_ph -= 1.0f;
    if (rv_mph1 >= 1.0f) rv_mph1 -= 1.0f;
    if (rv_mph2 >= 1.0f) rv_mph2 -= 1.0f;
    ch_ds = (sin_t(ch_ph) * 170.0f - ch_s) / (float)n;
    rm1_d = (6.0f * sin_t(rv_mph1) - rm1) / (float)n;
    rm3_d = (6.0f * sin_t(rv_mph2) - rm3) / (float)n;
    if (fm_fabsf(p->rev_secs - rv_secs_now) > 0.001f) {   /* the decay time: each line's gain for 60 dB in it */
        rv_secs_now = p->rev_secs;
        for (k = 0; k < 4u; k++)
            rv_g[k] = fm_exp2f(-9.965784f * (float)RV_LEN[k] / (rv_secs_now * SR));
    }
    if (dly_t <= 0.0f)
        dly_t = dtarget;
    dly_lp_l = fm_flush(dly_lp_l);       /* the delay's filters: no denormals once it has gone quiet */
    dly_lp_r = fm_flush(dly_lp_r);
    dly_pre_l = fm_flush(dly_pre_l);
    dly_pre_r = fm_flush(dly_pre_r);
    if (clr_i < NCLR) {                  /* still clearing after a reset: silence meanwhile */
        eng_clearing = (uint8_t)clearing();
        for (i = 0; i < 2u * n; i++)
            lr[i] = 0;
        return 0;
    }
    for (i = 0; i < n; i++) {
        float xl = l[i] * tl, xr = r[i] * tr, dl, dr, rl, rr, outl, outr;
        tl += dtl;
        tr += dtr;
        if (!finite_f(xl + xr)) {        /* a voice gone bad: never into the delay and the reverb */
            xl = xr = 0.0f;
            bad = 1;
        }
        /* the chorus */
        ch_buf[ch_w] = (xl + xr) * 0.5f;
        ch_w = (ch_w + 1u) & (CH_N - 1u);
        ch_now += (p->chorus - ch_now) * 0.001f;
        if (ch_now > 0.001f) {
            float c = ch_now, s = ch_s;
            xl = xl * (1.0f - 0.4f * c) + ch_read(530.0f + s) * 0.8f * c;
            xr = xr * (1.0f - 0.4f * c) + ch_read(530.0f - s) * 0.8f * c;
        }
        /* the delay: ping-pong, a low-pass in its feedback */
        dly_t += (dtarget - dly_t) * 0.00005f;
        dl = dly_read(dly_l, dly_t);
        dr = dly_read(dly_r, dly_t);
        dly_lp_l += (dl - dly_lp_l) * 0.45f;
        dly_lp_r += (dr - dly_lp_r) * 0.45f;
        /* into the half-rate line: a low-pass first (~7 kHz), so little folds back below its 11 kHz */
        dly_pre_l += ((xl + xr) * 0.5f + dly_lp_r * fb - dly_pre_l) * 0.63f;
        dly_pre_r += (dly_lp_l * fb - dly_pre_r) * 0.63f;
        dly_acc_l += dly_pre_l;
        dly_acc_r += dly_pre_r;
        if (dly_odd) {
            dly_l[dly_w] = to16(dly_acc_l * 0.5f);
            dly_r[dly_w] = to16(dly_acc_r * 0.5f);
            dly_acc_l = dly_acc_r = 0.0f;
            dly_w = (dly_w + 1u) & (DLY_N - 1u);
        }
        dly_odd ^= 1u;
        dmix_now += (p->dly_mix - dmix_now) * 0.001f;
        rmix_now += (p->rev_mix - rmix_now) * 0.001f;
        {                                /* WIDTH: the echoes from the middle out to either side */
            float m = (dl + dr) * 0.5f, sd = (dl - dr) * 0.5f * p->dly_width;
            outl = xl + (m + sd) * dmix_now;
            outr = xr + (m - sd) * dmix_now;
        }
        /* the reverb, fed the voices and the delay */
        if (!reverb((outl + outr) * 0.25f, damp, shim, rm1, rm3, &rl, &rr))
            bad = 1;
        outl += rl * rmix_now;
        outr += rr * rmix_now;
        if (!finite_f(outl + outr)) {
            outl = outr = 0.0f;
            bad = 1;
        }
        dc_yl = fm_flush(outl - dc_xl + 0.9995f * dc_yl);   /* no DC (the half-wave waves make some) */
        dc_xl = outl;
        dc_yr = fm_flush(outr - dc_xr + 0.9995f * dc_yr);
        dc_xr = outr;
        gain_now += (gain - gain_now) * 0.004f;   /* ~5 ms: turning MASTER does not click */
        lr[2 * i] = (int32_t)(soft_clip(dc_yl * 0.9f) * gain_now);
        lr[2 * i + 1] = (int32_t)(soft_clip(dc_yr * 0.9f) * gain_now);
        ch_s += ch_ds;
        rm1 += rm1_d;
        rm3 += rm3_d;
    }
    trem_l = trem_l_to;
    trem_r = trem_r_to;
    if (bad)                             /* silence, then everything that feeds back starts clean */
        eng_reset();
    return bad;
}

/* ------------------------------------------------------------------- setup --- */
void eng_reset(void)
{
    uint32_t i, k;
    for (i = 0; i < ENG_NVOICE; i++) {
        vc[i].on = 0;
        vc[i].note = -1;
    }
    for (k = 0; k < 4u; k++)
        rv_lp[k] = 0.0f;
    dly_lp_l = dly_lp_r = dly_pre_l = dly_pre_r = dly_acc_l = dly_acc_r = 0.0f;
    dc_xl = dc_yl = dc_xr = dc_yr = 0.0f;
    clr_i = 0;
    clr_at = 0;
    eng_clearing = 1;
}

void eng_init(void)
{
    uint32_t i;
    for (i = 0; i <= SIN_N; i++)
        sin_tab[i] = fm_sinf((float)i * FM_TWO_PI / (float)SIN_N);
    for (i = 0; i < ENG_NVOICE; i++)
        vc[i].note = -1;
#ifndef PLAT_HOST
    erng ^= plat_ticks24() | 1u;         /* a different wander every power-on (the host's tests repeat) */
#endif
}

void eng_silence(void)
{
    uint32_t i;
    for (i = 0; i < ENG_NVOICE; i++) {
        vc[i].on = 0;
        vc[i].note = -1;
    }
}

void eng_poison(void)
{
    union { uint32_t u; float f; } nan = {0x7FC00000u};
    rv_lp[0] = rv_lp[1] = nan.f;
}
