/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Philip Bos */
/* ECHOFIELD FM-1: the granular engine, its six modes, the DRIFT ORACLE and the effects chain. A port of the R36S
 * engine (echofield/src/engine.c) to the FM-1: single precision, fixed-point read positions, grains decoding the
 * IMA ADPCM source straight from flash through a one-block cache each, the delay and the reverb at half rate.
 *
 * Real-time rules: called from the audio interrupt only (ef_render); no allocation, no flash writes, bounded loops.
 * Grain parameters are drawn at a grain's birth, continuous parameters are smoothed, so knob moves never click.
 * Budget (360 MHz, 8160 cycles a sample): a grain costs about 30 cycles a sample plus its share of a block decode
 * (256 decodes per 256 source samples); the decode load is capped (LOAD_MAX) besides the grain count. */
#include "ef_dsp.h"
#define EF_IMA_STEP_TAB ima_step_ram     /* adpcm.h's tables in RAM (.data): the RAM code reads no flash constants */
#define EF_IMA_INDEX_TAB ima_index_ram
static int16_t ima_step_ram[89];
static int8_t ima_index_ram[16];
#include "adpcm.h"
static int16_t ima_step_ram[89] = {EF_IMA_STEP_VALUES};
static int8_t ima_index_ram[16] = {EF_IMA_INDEX_VALUES};
#include "plat.h"
/* fm1-base's fastmath helpers, forced inline here: the RAM code may call nothing */
#pragma push_macro("inline")
#define inline inline __attribute__((always_inline))
#include "fastmath.h"
#pragma pop_macro("inline")

/* the hot loops run from RAM (fm1-base's .ram_text: "an app's audio code, out of the XIP cache's way"): the samples
 * stream through the flash's cache, and code fetched from the flash beside them ran about five times slower than its
 * instruction count (measured on an FM-1). RAM code may call nothing (tools/build.py checks): what it uses is inlined */
#ifdef PLAT_HOST
#define EF_RAM __attribute__((noinline))
#else
#define EF_RAM __attribute__((section(".ram_text"), noinline))
#endif
#define INL static inline __attribute__((always_inline))
#define SUB 64u                          /* control-rate sub-block, output samples */
#define WIN_N 256u
#define Q 12u                            /* read positions: Q20.12 source samples */
#define QONE (1u << Q)
#define STEP_MAX 4.0f                    /* source samples per output sample (+24 st at 44.1 kHz) */
#define LOAD_MAX 56.0f                   /* sum over grains of (1 + step): the decode budget */
#define TWOPI 6.28318531f
#define PI 3.14159265f

#define TAPE_LEN 1024u                   /* tape stage's wow / flutter line, stereo */
#define HSR 22050.0f                     /* the half-rate section (delay, reverb, shimmer) */
#define DLY_LEN 22080u                   /* 1.0 s at 22 050 Hz, stereo int16 */
#define SHIM_LEN 2048u
#define SHIM_W 1024.0f
#define NFDN 8u

INL float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
INL float lerpf(float a, float b, float t) { return a + (b - a) * t; }
INL float smooth_to(float cur, float tgt, float a) { return cur + (tgt - cur) * a; }
/* a soft clip with no division (the FPU's divide is slow): x - 4/27 x^3 on [-1.5, 1.5], 1 at the ends */
INL float tanh_fast(float x)
{
    x = clampf(x, -1.5f, 1.5f);
    return x - 0.148148148f * x * x * x;
}
INL float semis(float st) { return fm_exp2f(st * (1.0f / 12.0f)); }
INL float onepole(float hz, float sr) { return 1.0f - fm_expf(-TWOPI * hz / sr); }

/* ------------------------------------------------------------------ random */
static uint32_t rng_s = 0x1234567u, fx_rng_s = 0x89ABCDEFu;
INL uint32_t xs(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}
static inline float rndf(void) { return (float)(xs(&rng_s) >> 8) * (1.0f / 16777216.0f); }
static inline float rndb(void) { return rndf() * 2.0f - 1.0f; }
INL float fx_rndb(void) { return (float)(xs(&fx_rng_s) >> 8) * (2.0f / 16777216.0f) - 1.0f; }

/* ------------------------------------------------------------------ state */
typedef struct {
    uint8_t on, win, rev, tone;          /* tone: 0 none, 1 low-pass, 2 high-pass */
    int32_t blk;                         /* the block in cache, -1 none */
    uint32_t pos, inc;                   /* Q12 source position and step */
    uint32_t len, age;                   /* output samples */
    uint32_t delay;                      /* start offset inside the sub-block */
    uint32_t wph, wstep;                 /* window phase, Q16 table entries (0 .. WIN_N << 16) */
    int32_t gl, gr;                      /* pan x level, Q14: a sample's int16 scale into the bus's */
    int32_t tone_a, z;                   /* one-pole colour, Q14 */
    float load;                          /* 1 + step */
    int8_t vis_pitch;
    int16_t cache[EF_BLK_SAMPLES + 1u];
} grain_t;

typedef struct {
    float timer, glide_cur, glide_target;
    float bloom, scatter_burst, rev_swell, octave_flick, hush;
    int chord[4], nchord, bass_deg, last_scale, last_drone, on;
} oracle_t;

typedef struct {
    float size_s, density, jitter, center, scatter;
    float pitch_rand, rev_prob, pan_spread, amp, tone, tone_rand;
    int win, clocked, tape;
    float tape_speed;
} ctl_t;

static ef_ctl_t ctl;
static ef_meter_t meter;
static ef_vis_t vis;
static float sm[P_COUNT];
static float win_tab[WIN_COUNT][WIN_N + 2u];
static int16_t win_q15[WIN_COUNT][WIN_N + 2u];   /* the grain loop's: integer (the FPU's operations do not overlap) */
static grain_t gr[EF_MAX_GRAINS] PLAT_POOL;
static float load_now;

/* LISTEN: the sample itself, straight, at its own pitch, from START to END (the region the grains use); the cloud
 * fades out meanwhile and back in after. One voice with its own block cache (a grain_t: the same decoder) */
static grain_t aud;
static volatile uint8_t aud_req;         /* 1 start (once the source is the one asked for), 2 stop */
static volatile uint8_t aud_loop;
static uint8_t aud_on;
static float aud_last;                   /* its last sample: what fades out when it stops */
static float aud_gain, duck;

static const ef_source_t *volatile src_next;
static volatile uint8_t src_pending;
static const ef_source_t *src;
static float fade_gain;

/* modes */
static float spawn_wait, scan, walk_pos, walk_pitch, walk_v1, walk_v2, walk_size;
static float lfo1, lfo2_cur, lfo2_tgt, lfo2_t;
static float tape_head, tape_speed = 1.0f, tape_stop;
static int tape_k, stutter_left, pulse_step, pulse_rot;
static int pulse_last = -1;                /* the PULSE step that fired last */
static uint32_t ext_seq_seen;
static float stutter_pos, pulse_acc, step_pos[16];
static int step_oct[16];
static oracle_t orc;
static float env, play_gain;
static uint32_t note_seq_seen, note_rr;
static float tapestop, sweep_ph, punch_stut_pos;
static int punch_stut_on;

/* effects */
static float tape_buf[TAPE_LEN * 2u] PLAT_POOL;
static int16_t dly[2][DLY_LEN] PLAT_POOL;
static float shim[SHIM_LEN] PLAT_POOL;
static const uint16_t FDN_BASE[NFDN] = {1499, 1889, 2381, 2803, 3191, 3607, 4027, 4513};   /* at 48 kHz */
static const uint16_t AP_BASE[4] = {142, 107, 379, 277};
#define FDN_ROOM 16u                     /* each line's room past its length: the modulation reads up to 12 back */
#define FDN_TOTAL 11120u                 /* the lines' lengths at 22 050 Hz (10 984) + their room, packed */
static float fdn_mem[FDN_TOTAL] PLAT_POOL;
static float ap[4][176] PLAT_POOL;
static struct {
    uint32_t tape_w;
    float wow_ph, flut_ph, flut_noise, wow_noise, tape_lpz[2], age_gain, age_timer, hiss_z[2];
    float svf1[2], svf2[2], cut_smooth, g_coef;
    uint32_t dly_w;
    float dly_time_smooth, dly_lp[2];
    uint32_t fdn_len[NFDN], fdn_cap[NFDN], fdn_off[NFDN], fdn_w[NFDN], ap_len[4], ap_w[4];
    float fdn_lp[NFDN], rev_ph1, rev_ph2;
    uint32_t shim_w;
    float shim_ph, shim_lp, shim_fb, shim_dx, shim_dy;   /* shim_dx / dy: the shimmer loop's DC blocker */
    float dc_x[2], dc_y[2], lim_gain, agc_ms, agc_gain;
    float out_dx[2], out_dy[2];          /* the output's DC blocker */
    uint32_t hr_phase, mod_k;
    float dl, a1, mod1, mod5;
    float hr_in[2], hr_prev[4], hr_cur[4];
} fx;

static float busL[SUB], busR[SUB];

/* RESAMPLE */
static uint8_t *rec_buf;
static uint32_t rec_cap, rec_n, rec_half;
static volatile uint8_t rec_on;
static float rec_acc;
static ef_ima_t rec_ima;

static const char *const EVENT_NAME[6] = {"BLOOM", "SCATTER", "REVERSE SWELL", "OCTAVE FLICKER", "LEAP", "HUSH"};

/* scales of the oracle (the R36S's) */
typedef struct {
    int n;
    int8_t deg[7];
} scale_t;
static const scale_t SCALES[12] = {
    {7, {0, 2, 4, 5, 7, 9, 11}}, {7, {0, 2, 3, 5, 7, 8, 10}}, {7, {0, 2, 3, 5, 7, 9, 10}},
    {7, {0, 1, 3, 5, 7, 8, 10}}, {7, {0, 2, 4, 6, 7, 9, 11}}, {7, {0, 2, 4, 5, 7, 9, 10}},
    {5, {0, 2, 4, 7, 9}},        {5, {0, 3, 5, 7, 10}},       {5, {0, 2, 3, 7, 8}},
    {5, {0, 1, 5, 7, 10}},       {6, {0, 2, 4, 6, 8, 10}},    {6, {0, 2, 4, 7, 9, 11}},
};

/* ------------------------------------------------------------------ windows */
float ef_window(int w, float x)
{
    x = clampf(x, 0.0f, 1.0f);
    switch (w) {
    default:
    case WIN_HANN:
        return 0.5f - 0.5f * fm_cosf(TWOPI * x);
    case WIN_TUKEY: {
        const float a = 0.3f;
        if (x < a * 0.5f)
            return 0.5f - 0.5f * fm_cosf(TWOPI * x / a);
        if (x > 1.0f - a * 0.5f)
            return 0.5f - 0.5f * fm_cosf(TWOPI * (1.0f - x) / a);
        return 1.0f;
    }
    case WIN_PERC: {
        const float att = 0.03f;
        float v = x < att ? fm_sinf(0.5f * PI * x / att) : fm_expf(-5.0f * (x - att) / (1.0f - att));
        if (x > 0.92f)
            v *= 0.5f + 0.5f * fm_cosf(PI * (x - 0.92f) / 0.08f);
        return v;
    }
    case WIN_SWELL:
        return ef_window(WIN_PERC, 1.0f - x);
    case WIN_GAUSS: {
        float d = (x - 0.5f) / 0.16f, g = fm_expf(-0.5f * d * d);
        const float e = 0.00752f;        /* exp(-0.5 (0.5 / 0.16)^2) */
        return clampf((g - e) / (1.0f - e), 0.0f, 1.0f);
    }
    }
}

/* ------------------------------------------------------------------ init */
static void fx_reset(void)
{
    uint32_t i;
    float *z = (float *)&fx;
    for (i = 0; i < sizeof fx / sizeof(float); i++)
        z[i] = 0.0f;
    for (i = 0; i < NFDN; i++) {
        fx.fdn_len[i] = (uint32_t)((float)FDN_BASE[i] * (HSR / 48000.0f));
        fx.fdn_cap[i] = fx.fdn_len[i] + FDN_ROOM;
        fx.fdn_off[i] = i ? fx.fdn_off[i - 1u] + fx.fdn_cap[i - 1u] : 0u;
        fx.fdn_w[i] = 0;
    }
    for (i = 0; i < 4u; i++) {
        fx.ap_len[i] = (uint32_t)((float)AP_BASE[i] * (HSR / 48000.0f)) + 1u;
        fx.ap_w[i] = 0;
    }
    fx.tape_w = fx.dly_w = fx.shim_w = fx.hr_phase = 0;
    fx.lim_gain = fx.agc_gain = fx.age_gain = 1.0f;
    fx.cut_smooth = 1.0f;
    fx.dly_time_smooth = 400.0f;
}

static void clear_lines(void)
{
    uint32_t i, q;
    for (i = 0; i < TAPE_LEN * 2u; i++)
        tape_buf[i] = 0.0f;
    for (i = 0; i < DLY_LEN; i++)
        dly[0][i] = dly[1][i] = 0;
    for (i = 0; i < SHIM_LEN; i++)
        shim[i] = 0.0f;
    for (i = 0; i < FDN_TOTAL; i++)
        fdn_mem[i] = 0.0f;
    for (q = 0; q < 4u; q++)
        for (i = 0; i < 176u; i++)
            ap[q][i] = 0.0f;
}

void ef_reset_time(void)
{
    int i;
    scan = 0.0f;
    tape_head = 0.0f;
    tape_speed = 1.0f;
    tape_stop = 0.0f;
    walk_pos = walk_pitch = walk_v1 = walk_v2 = walk_size = 0.0f;
    for (i = 0; i < 16; i++) {
        step_pos[i] = rndf();
        step_oct[i] = 0;
    }
    orc.timer = 0.0f;
    orc.nchord = 1;
    orc.chord[0] = 0;
}

void ef_init(void)
{
    uint32_t w, i;
    for (w = 0; w < WIN_COUNT; w++) {
        for (i = 0; i <= WIN_N; i++) {
            win_tab[w][i] = ef_window((int)w, (float)i / (float)WIN_N);
            win_q15[w][i] = (int16_t)(win_tab[w][i] * 32767.0f + 0.5f);
        }
        win_tab[w][WIN_N + 1u] = 0.0f;
        win_q15[w][WIN_N + 1u] = 0;
    }
    for (i = 0; i < EF_MAX_GRAINS; i++) {
        gr[i].on = 0;
        gr[i].blk = -1;
    }
    ctl.max_grains = 20;
    ctl.shed = 0;
    fx_reset();
    clear_lines();
    ef_reset_time();
    for (i = 0; i < P_COUNT; i++)
        sm[i] = ctl.p[i];
    fade_gain = 1.0f;
    play_gain = 0.0f;
    env = 0.0f;
}

ef_ctl_t *ef_ctl(void) { return &ctl; }
const ef_meter_t *ef_meter(void) { return &meter; }
void ef_meter_peak_reset(void) { meter.peak = 0.0f; meter.limiting = 0; }
void ef_meter_time_reset(void) { meter.t_render = meter.t_grains = meter.t_fx = meter.t_spawn = 0; }
#define TMAX(f, t0) do { uint32_t d_ = plat_ticks24() - (t0); if (d_ > meter.f) meter.f = d_; } while (0)
const ef_vis_t *ef_vis(void) { return &vis; }

void ef_set_source(const ef_source_t *s)
{
    src_next = s;
    src_pending = 1;
}
int ef_source_idle(void) { return !src_pending; }
const ef_source_t *ef_source_now(void) { return src; }

void ef_panic(void)
{
    uint32_t i;
    for (i = 0; i < EF_MAX_GRAINS; i++)
        gr[i].on = 0;
    clear_lines();
    fx_reset();
    env = 0.0f;
    play_gain = 0.0f;
    aud_on = 0;
    aud_req = 0;
    aud_gain = duck = 0.0f;
}

/* ------------------------------------------------------------------ the source */
INL const uint8_t *block_ptr(const ef_source_t *s, uint32_t b)
{
    uint32_t k = b / EF_SEC_BLOCKS;
    return s->sec[k] + (b - k * EF_SEC_BLOCKS) * EF_BLK_BYTES;
}
INL uint32_t nblocks(const ef_source_t *s) { return (s->frames + EF_BLK_SAMPLES - 1u) / EF_BLK_SAMPLES; }

int16_t ef_sample_at(const ef_source_t *s, uint32_t i)
{
    ef_ima_t st;
    const uint8_t *b;
    uint32_t k, n;
    int32_t v = 0;
    if (!s || i >= s->frames)
        return 0;
    b = block_ptr(s, i / EF_BLK_SAMPLES);
    ef_ima_head(&st, b);
    n = i % EF_BLK_SAMPLES;
    for (k = 0; k <= n; k++)
        v = ef_ima_step(&st, (k & 1u) ? b[4u + k / 2u] >> 4 : b[4u + k / 2u] & 15u);
    return (int16_t)v;
}

INL void grain_load(grain_t *g, uint32_t b)
{
    uint32_t nb = nblocks(src);
    ef_ima_block(block_ptr(src, b), g->cache);
    g->cache[EF_BLK_SAMPLES] = b + 1u < nb ? ef_ima_first(block_ptr(src, b + 1u)) : g->cache[EF_BLK_SAMPLES - 1u];
    g->blk = (int32_t)b;
}

/* ------------------------------------------------------------------ DRIFT ORACLE */
static int scale_semi(const scale_t *sc, int deg)
{
    int oct = deg >= 0 ? deg / sc->n : -((-deg + sc->n - 1) / sc->n);
    return sc->deg[deg - oct * sc->n] + 12 * oct;
}

static void oracle_event(int k)
{
    const char *s = EVENT_NAME[k];
    int i;
    for (i = 0; i < 15 && s[i]; i++)
        meter.oracle_name[i] = s[i];
    meter.oracle_name[i] = 0;
    meter.oracle_event++;
}

static void oracle_update(float dt)
{
    oracle_t *o = &orc;
    float glide = sm[P_OR_GLIDE], tc = 0.05f + glide * glide * 4.0f;
    o->on = sm[P_OR_ON] > 0.5f;
    o->glide_cur = smooth_to(o->glide_cur, o->on ? o->glide_target : 0.0f, 1.0f - fm_expf(-dt / tc));
    o->bloom = fm_maxf(0.0f, o->bloom - dt);
    o->scatter_burst = fm_maxf(0.0f, o->scatter_burst - dt);
    o->rev_swell = fm_maxf(0.0f, o->rev_swell - dt);
    o->octave_flick = fm_maxf(0.0f, o->octave_flick - dt);
    o->hush = fm_maxf(0.0f, o->hush - dt);
    if (!o->on || sm[P_OR_HOLD] > 0.5f || ctl.freeze)
        return;
    {
        int si = (int)clampf(sm[P_OR_SCALE], 0.0f, 11.0f), drone = sm[P_OR_DRONE] > 0.5f;
        int range = (int)sm[P_OR_RANGE], d;
        const scale_t *sc = &SCALES[si];
        float act = sm[P_OR_ACTIVITY], amb = sm[P_OR_AMBIG], p;
        o->timer -= dt;
        if (si != o->last_scale || drone != o->last_drone) {
            o->timer = 0.0f;
            o->last_scale = si;
            o->last_drone = drone;
        }
        if (o->timer <= 0.0f) {
            float w[7], tot = 0.0f, r, mean;
            int pick = 0, oct = 0, b, third;
            for (d = 0; d < sc->n; d++) {
                int semi = sc->deg[d];
                float base = semi == 0 ? 4.0f : semi == 7 ? 3.0f : (semi == 3 || semi == 4) ? 2.2f : semi == 5 ? 1.6f : 1.0f;
                if (drone)
                    base = semi == 0 ? 6.0f : (semi == 7 || semi == 5) ? 2.5f : 0.15f;
                w[d] = fm_powf(base, 1.0f - amb * 0.85f);
                if (d == o->bass_deg)
                    w[d] *= 0.5f;
                tot += w[d];
            }
            r = rndf() * tot;
            for (d = 0; d < sc->n; d++) {
                r -= w[d];
                if (r <= 0.0f) {
                    pick = d;
                    break;
                }
            }
            o->bass_deg = pick;
            if (range > 0 && rndf() < 0.25f * (float)range)
                oct = rndf() < 0.6f ? 0 : 1;
            if (range > 1 && rndf() < 0.15f)
                oct = -1;
            o->glide_target = (float)(scale_semi(sc, pick) + 12 * oct);
            b = scale_semi(sc, pick);
            o->nchord = 0;
            o->chord[o->nchord++] = 0;
            if (drone) {
                o->chord[o->nchord++] = 7;
                o->chord[o->nchord++] = 12;
            } else {
                third = scale_semi(sc, pick + 2) - b;
                if (rndf() < amb * 0.4f)
                    third = scale_semi(sc, pick + (rndf() < 0.5f ? 1 : 3)) - b;
                o->chord[o->nchord++] = third;
                o->chord[o->nchord++] = scale_semi(sc, pick + 4) - b;
                if (amb > 0.5f && rndf() < amb - 0.4f)
                    o->chord[o->nchord++] = scale_semi(sc, pick + 6) - b;
            }
            meter.oracle_note = (int8_t)(scale_semi(sc, pick) + (int)sm[P_OR_ROOT]);
            mean = lerpf(14.0f, 1.2f, act) * (drone ? 3.0f : 1.0f);
            o->timer = mean * (0.5f + rndf());
        }
        p = fm_powf(act, 1.5f) * 0.35f * dt * (drone ? 0.3f : 1.0f);
        if (rndf() < p) {
            int k = (int)(rndf() * 6.0f);
            if (k > 5)
                k = 5;
            switch (k) {
            case 0: o->bloom = 2.0f + 2.0f * rndf(); break;
            case 1: o->scatter_burst = 1.5f; break;
            case 2: o->rev_swell = 2.0f; break;
            case 3: o->octave_flick = 1.5f; break;
            case 4: scan += (rndf() - 0.5f) * 0.5f; break;
            default: o->hush = 2.0f; break;
            }
            oracle_event(k);
        }
    }
}

static float oracle_grain_note(void)
{
    oracle_t *o = &orc;
    float n, r;
    int idx, range;
    if (!o->on)
        return 0.0f;
    n = sm[P_OR_ROOT] + o->glide_cur;
    r = rndf();
    idx = r < 0.4f ? 0 : 1 + (int)(rndf() * (float)(o->nchord - 1));
    if (idx > o->nchord - 1)
        idx = o->nchord - 1;
    n += (float)o->chord[idx];
    range = (int)sm[P_OR_RANGE];
    if (range > 0 && rndf() < 0.2f * (float)range)
        n += 12.0f;
    if (sm[P_OR_DRONE] > 0.5f && rndf() < 0.2f)
        n -= 12.0f;
    if (rndf() < sm[P_OR_AMBIG] * 0.12f)
        n += rndf() < 0.5f ? -1.0f : 1.0f;
    if (o->octave_flick > 0.0f && rndf() < 0.4f)
        n += 12.0f;
    return n;
}

/* ------------------------------------------------------------------ mode controls */
INL int punch(int k) { return (ctl.punch >> k) & 1u; }
static inline int frozen(void) { return ctl.freeze || punch(PUNCH_FREEZE); }

static void euclid(int k, int n, int rot, uint16_t *pat)
{
    int i;
    *pat = 0;
    if (k <= 0)
        return;
    for (i = 0; i < n; i++)
        if (((i * k) % n) < k)
            *pat |= (uint16_t)(1u << ((i + rot) % n));
}

static float region_seconds(void)
{
    float a = fm_minf(sm[P_START], sm[P_END]), b = fm_maxf(sm[P_START], sm[P_END]);
    return src ? (b - a) * (float)src->frames / (float)src->rate : 1.0f;
}

static void compute_ctl(ctl_t *c, float dt)
{
    float *s = sm;
    int mode = (int)s[P_MODE];
    float T = s[P_TEXTURE], S = s[P_SCATTER], D = s[P_DRIFT], M = s[P_MOTION], W = s[P_WIDTH];
    float size_mul = fm_exp2f(s[P_SIZE_MUL]), dens_mul = fm_exp2f(s[P_DENS_MUL]);
    float lfo, pos, wander, cc;
    oracle_t *o = &orc;
    int wsel;

    if (!frozen()) {
        float wr;
        lfo1 += dt * (0.03f + 0.3f * M);
        if (lfo1 > 1.0f)
            lfo1 -= 1.0f;
        lfo2_t += dt * (0.1f + 1.5f * M);
        if (lfo2_t >= 1.0f) {
            lfo2_t -= 1.0f;
            lfo2_tgt = rndb();
        }
        lfo2_cur = smooth_to(lfo2_cur, lfo2_tgt, fm_minf(1.0f, dt * (0.5f + 3.0f * M)));
        wr = (mode == MODE_DRIFT ? 0.25f + 1.2f * M : 0.08f + 0.4f * M) * dt;
        walk_v1 += rndb() * wr * 2.0f - walk_v1 * wr * 2.0f;
        walk_pos = clampf(walk_pos + walk_v1 * wr * 4.0f - walk_pos * wr * 0.3f, -1.0f, 1.0f);
        walk_v2 += rndb() * wr * 2.0f - walk_v2 * wr * 2.0f;
        walk_pitch = clampf(walk_pitch + walk_v2 * wr * 4.0f - walk_pitch * wr * 0.3f, -1.0f, 1.0f);
        walk_size = smooth_to(walk_size, rndb(), dt * 0.3f);
        if (src) {
            scan += s[P_SCAN] * dt / fm_maxf(0.05f, fm_fabsf(region_seconds()));
            scan -= fm_floorf(scan);
        }
    }
    lfo = fm_sin_turns(lfo1);
    pos = s[P_POSITION];

    c->jitter = 0.5f;
    c->pitch_rand = s[P_PITCH_RAND] + 0.08f + D * 0.3f;
    c->rev_prob = s[P_REVERSE];
    c->pan_spread = W;
    c->amp = 1.0f;
    c->tone = s[P_GRAIN_TONE];
    c->tone_rand = 0.0f;
    c->win = WIN_HANN;
    c->clocked = 0;
    c->tape = 0;
    c->tape_speed = 1.0f;
    wander = D * 0.3f * walk_pos;

    switch (mode) {
    default:
    case MODE_CLOUD:
        c->size_s = 0.12f * fm_powf(1500.0f / 120.0f, T);
        c->density = (4.0f + 8.0f * T) / c->size_s * (1.0f + 0.4f * M * lfo2_cur);
        c->jitter = 0.4f + 0.6f * S;
        c->center = pos + 0.15f * M * lfo + wander;
        c->scatter = 0.03f + 0.6f * fm_powf(S, 1.3f);
        c->rev_prob += S * 0.1f;
        break;
    case MODE_DUST:
        c->size_s = 0.02f * fm_powf(200.0f / 20.0f, T);
        c->density = (6.0f + 24.0f * (1.0f - T * 0.5f)) * (0.5f + S);
        c->jitter = 0.7f + 0.3f * S;
        c->center = pos + 0.1f * M * lfo + wander;
        c->scatter = 0.1f + 0.85f * S;
        c->rev_prob += 0.2f + 0.3f * S;
        c->win = WIN_GAUSS;
        c->amp = 0.85f;
        c->tone = clampf(s[P_GRAIN_TONE] + 0.25f, -1.0f, 1.0f);
        c->tone_rand = 0.3f;
        c->pan_spread = fm_minf(1.0f, W * 1.3f);
        break;
    case MODE_TAPE: {
        float target = semis(s[P_PITCH]) * lerpf(1.0f, 0.3f, fm_powf(T, 1.2f));
        target *= 1.0f + 0.03f * M * lfo2_cur + 0.02f * D * walk_pitch;
        if (!frozen() && tape_stop <= 0.0f && rndf() < M * 0.02f * dt)
            tape_stop = 1.6f;
        if (tape_stop > 0.0f) {
            float ph;
            tape_stop -= dt;
            ph = 1.6f - tape_stop;
            target *= ph < 0.8f ? (1.0f - ph / 0.8f) : ((ph - 0.8f) / 0.8f);
            target = fm_maxf(target, 0.02f);
        }
        tape_speed = smooth_to(tape_speed, target, fm_minf(1.0f, dt * 6.0f));
        c->tape = 1;
        c->tape_speed = tape_speed;
        c->size_s = 0.18f * (1.0f + T);
        c->density = 4.0f / c->size_s;
        c->jitter = 0.15f;
        c->center = tape_head;
        c->scatter = 0.002f + S * 0.03f;
        c->pitch_rand = s[P_PITCH_RAND];
        c->pan_spread = W * 0.8f;
        break;
    }
    case MODE_BLOOM:
        c->size_s = 0.6f * fm_powf(2.5f / 0.6f, T);
        c->density = (3.0f + 3.0f * T) / c->size_s * (1.0f + 0.5f * M * lfo2_cur);
        c->jitter = 0.9f;
        c->center = pos + 0.2f * M * lfo + wander;
        c->scatter = 0.08f + 0.5f * S;
        c->rev_prob += 0.25f;
        c->tone = clampf(s[P_GRAIN_TONE] + 0.15f, -1.0f, 1.0f);
        c->tone_rand = 0.25f;
        c->win = rndf() < 0.5f ? WIN_HANN : WIN_GAUSS;
        c->amp = 0.95f;
        break;
    case MODE_PULSE:
        c->clocked = 1;
        c->size_s = 0.06f * fm_powf(1200.0f / 60.0f, T);
        c->density = 0.0f;
        c->center = pos + wander;
        c->scatter = S;
        c->win = T > 0.6f ? WIN_HANN : WIN_PERC;
        c->jitter = 0.0f;
        break;
    case MODE_DRIFT:
        c->size_s = 0.1f * fm_powf(12.0f, T) * (1.0f + 0.5f * walk_size);
        c->density = (2.0f + 4.0f * T) * (1.0f + 0.5f * lfo) / c->size_s;
        c->jitter = 0.8f;
        c->center = pos + walk_pos * (0.1f + 0.5f * S);
        c->scatter = 0.03f + 0.3f * S;
        c->pitch_rand = s[P_PITCH_RAND] + D * 0.5f;
        c->rev_prob += 0.15f;
        c->win = WIN_GAUSS;
        break;
    }
    c->size_s *= size_mul;
    c->density *= dens_mul;
    if (o->on) {
        if (o->bloom > 0.0f)
            c->density *= 2.5f;
        if (o->hush > 0.0f)
            c->density *= 0.3f;
        if (o->scatter_burst > 0.0f)
            c->scatter = fm_minf(1.0f, c->scatter + 0.4f);
        if (o->rev_swell > 0.0f)
            c->rev_prob += 0.6f;
        c->jitter = fm_minf(1.0f, c->jitter + s[P_OR_ACTIVITY] * 0.3f);
    }
    /* performance effects */
    if (punch(PUNCH_BLOOM)) {
        c->density *= 2.5f;
        c->size_s *= 1.5f;
    }
    if (punch(PUNCH_HUSH))
        c->density = 0.0f;
    if (punch(PUNCH_REVERSE))
        c->rev_prob = 1.0f;
    if (punch(PUNCH_STUTTER)) {
        if (!punch_stut_on)
            punch_stut_pos = c->center;
        punch_stut_on = 1;
        c->center = punch_stut_pos;
        c->scatter = 0.0f;
        c->size_s = fm_minf(c->size_s, 0.09f);
        c->density = fm_maxf(c->density, 24.0f);
        c->jitter = 0.0f;
    } else
        punch_stut_on = 0;
    wsel = (int)s[P_WINDOW];
    if (wsel > 0)
        c->win = wsel - 1;
    c->rev_prob = clampf(c->rev_prob, 0.0f, 1.0f);
    c->size_s = clampf(c->size_s, 0.004f, 2.5f);
    c->density = clampf(c->density, 0.0f, 400.0f);
    c->scatter = clampf(c->scatter, 0.0f, 1.0f);
    if (!c->tape) {
        cc = c->center + scan;
        c->center = cc - fm_floorf(cc);
    }
}

/* ------------------------------------------------------------------ grains */
/* the notes grains are born on: the keys held, or, while their release sounds (PLAY off), the last ones held */
static int8_t held_note[EF_MAX_NOTES];
static uint8_t held_vel[EF_MAX_NOTES];
static uint32_t held_n;

static void notes_track(void)
{
    uint32_t n = ctl.nnotes, k;
    if (!n)
        return;
    if (n > EF_MAX_NOTES)
        n = EF_MAX_NOTES;
    for (k = 0; k < n; k++) {
        held_note[k] = ctl.note[k];
        held_vel[k] = ctl.vel[k];
    }
    held_n = n;
}

static int notes_sound(void) { return ctl.nnotes || (!ctl.play && held_n); }

static float note_pitch(float *amp)
{
    uint32_t k;
    float velo = sm[P_VELO];
    if (!held_n)
        return 0.0f;
    k = note_rr++ % held_n;
    *amp *= (1.0f - velo) + velo * (float)held_vel[k] * (1.0f / 127.0f);
    return (float)held_note[k];
}

static uint32_t grain_cap(void)
{
    uint32_t cap = ctl.max_grains, shed = ctl.shed;
    if (cap > EF_MAX_GRAINS)
        cap = EF_MAX_GRAINS;
    cap = shed < cap ? cap - shed : 0;
    return cap < 4u ? 4u : cap;
}

static void spawn(const ctl_t *c, uint32_t offset, float amp_scale, float extra_pitch, float pos_override)
{
    grain_t *g = 0;
    uint32_t i, active = 0, cap = grain_cap(), len, frames;
    float ra, rb, rs, re, rlen, size, pitch, harm = 0.0f, step, span, center, cpos, start, pan, overlap, amp, tone;
    int head = 0, rev;
    if (!src || src->frames < 512u)
        return;
    for (i = 0; i < EF_MAX_GRAINS; i++) {
        if (gr[i].on)
            active++;
        else if (!g)
            g = &gr[i];
    }
    if (!g || active >= cap)
        return;
    frames = src->frames;
    ra = fm_minf(sm[P_START], sm[P_END]);
    rb = fm_maxf(sm[P_START], sm[P_END]);
    rs = ra * (float)frames;
    re = rb * (float)frames;
    if (re - rs < 512.0f) {
        re = fm_minf((float)frames - 1.0f, rs + 512.0f);
        rs = fm_maxf(0.0f, re - 512.0f);
    }
    re = fm_minf(re, (float)frames - 2.0f);
    rlen = re - rs;
    if (rlen < 64.0f)
        return;

    size = c->size_s * (1.0f + rndb() * sm[P_SIZE_RAND] * 0.7f * (c->tape ? 0.3f : 1.0f));
    len = (uint32_t)(fm_maxf(size, 0.003f) * (float)EF_SR);
    pitch = sm[P_PITCH];
    if (notes_sound())
        pitch += note_pitch(&amp_scale);
    else
        pitch += oracle_grain_note();
    if (punch(PUNCH_OCT_UP))
        pitch += 12.0f;
    if (punch(PUNCH_OCT_DOWN))
        pitch -= 12.0f;
    pitch -= 36.0f * tapestop * tapestop;
    if (rndf() < sm[P_HARMONY] * 0.65f) {
        static const float IV[8] = {12, 12, -12, 7, 19, 24, -5, 7};
        harm = IV[xs(&rng_s) & 7u];
        amp_scale *= 0.7f;
    }
    if (c->tape) {
        static const float HR[3] = {1.0f, 0.5f, 2.0f}, HA[3] = {1.0f, 0.8f, 0.4f};
        head = tape_k;
        tape_k = (tape_k + 1) % 3;
        step = c->tape_speed * HR[head] * semis(harm + (pitch - sm[P_PITCH]));
        amp_scale *= HA[head];
        g->vis_pitch = (int8_t)clampf(12.0f * fm_log2f(fm_maxf(0.01f, step)), -48.0f, 48.0f);
    } else {
        float st = pitch + harm + extra_pitch + rndb() * c->pitch_rand + walk_pitch * sm[P_DRIFT] * 2.0f;
        st = clampf(st, -48.0f, 36.0f);
        g->vis_pitch = (int8_t)st;
        step = semis(st);
    }
    step *= (float)src->rate * (1.0f / (float)EF_SR);
    step = clampf(step, 1.0f / 64.0f, STEP_MAX);
    if (load_now + 1.0f + step > LOAD_MAX)
        return;
    if (len < 64u)
        len = 64u;
    span = (float)len * step;
    if (span > rlen * 0.98f) {
        len = (uint32_t)(rlen * 0.98f / step);
        span = (float)len * step;
    }
    if (len < 32u)
        return;
    rev = rndf() < c->rev_prob;
    center = pos_override >= 0.0f ? pos_override : c->center + rndb() * c->scatter * 0.5f;
    if (c->tape)
        center = c->center + (float)head / 3.0f + rndb() * c->scatter;
    center -= fm_floorf(center);
    cpos = rs + center * rlen;
    if (!rev)
        start = clampf(cpos - span * 0.5f, rs, re - span);
    else
        start = clampf(cpos + span * 0.5f, rs + span, re - 1.0f);
    g->pos = (uint32_t)(fm_maxf(start, 0.0f) * (float)QONE);
    g->inc = (uint32_t)(step * (float)QONE);
    g->rev = (uint8_t)rev;
    g->len = len;
    g->age = 0;
    g->delay = offset;
    g->win = (uint8_t)(c->win < 0 ? 0 : c->win >= WIN_COUNT ? WIN_COUNT - 1 : c->win);
    g->wph = 0;
    g->wstep = (uint32_t)(((float)WIN_N * 65536.0f) / (float)len);
    g->blk = -1;
    g->load = 1.0f + step;
    pan = 0.5f + rndb() * 0.5f * c->pan_spread;
    if (c->tape)
        pan = clampf(0.5f + (head == 1 ? -0.35f : head == 2 ? 0.35f : 0.0f) * c->pan_spread + rndb() * 0.1f, 0.0f, 1.0f);
    overlap = fm_maxf(1.0f, c->density * c->size_s);
    amp = c->amp * amp_scale * 0.75f * fm_rsqrtf(overlap);
    g->gl = (int32_t)(clampf(amp * fm_sqrtf(1.0f - pan) * 1.41421f, 0.0f, 1.9f) * 16384.0f);
    g->gr = (int32_t)(clampf(amp * fm_sqrtf(pan) * 1.41421f, 0.0f, 1.9f) * 16384.0f);
    tone = clampf(c->tone + rndb() * c->tone_rand, -1.0f, 1.0f);
    if (fm_fabsf(tone) < 0.02f) {
        g->tone = 0;
    } else if (tone < 0.0f) {
        g->tone = 1;
        g->tone_a = (int32_t)(onepole(fm_minf(18000.0f * fm_powf(0.02f, -tone), 20000.0f), (float)EF_SR) * 16384.0f);
    } else {
        g->tone = 2;
        g->tone_a = (int32_t)(onepole(150.0f * fm_powf(30.0f, tone), (float)EF_SR) * 16384.0f);
    }
    g->z = 0;
    g->on = 1;
    load_now += g->load;
}

/* the grains first, first + stride, ... into L / R (added); -> their decode load. From RAM, on either core */
/* the grains first, first + stride, ... into L / R (added, int16 scale); -> their decode load. From RAM, on either core.
 * Integer throughout: the interpolation (8-bit fraction), the colour filter, the window, the pan */
static EF_RAM float render_grains_part(uint32_t n, uint32_t first, uint32_t stride, int32_t *busL, int32_t *busR)
{
    uint32_t k, last;
    float load = 0.0f;
    if (!src || src->frames < 2u)
        return 0.0f;
    last = src->frames - 1u;
    for (k = first; k < EF_MAX_GRAINS; k += stride) {
        grain_t *g = &gr[k];
        const int16_t *w, *c = g->cache;
        uint32_t i, pos, inc, left, wph, wstep;
        int32_t gl, gr_, z, ta;
        int rev, tone;
        if (!g->on)
            continue;
        w = win_q15[g->win];
        i = g->delay;
        g->delay = 0;
        pos = g->pos;
        inc = g->inc;
        rev = g->rev;
        tone = g->tone;
        wph = g->wph;
        wstep = g->wstep;
        gl = g->gl;
        gr_ = g->gr;
        z = g->z;
        ta = g->tone_a;
        left = g->len - g->age;
        if (left > n - i)
            left = n - i;
        g->age += left;
        for (; left; left--, i++) {
            uint32_t idx = pos >> Q, b = idx >> 8, j = idx & (EF_BLK_SAMPLES - 1u), wi = wph >> 16, wf = (wph >> 8) & 255u;
            int32_t x, a, wv;
            if (idx >= last) {           /* never past the sample (a position that wrapped below 0 lands here too) */
                g->age = g->len;
                break;
            }
            if ((int32_t)b != g->blk)
                grain_load(g, b);
            x = c[j] + (((int32_t)c[j + 1u] - c[j]) * (int32_t)((pos >> (Q - 8u)) & 255u) >> 8);
            if (tone) {
                z += (x - z) * ta >> 14;
                x = tone == 1 ? z : x - z;
            }
            wv = w[wi] + (((int32_t)w[wi + 1u] - w[wi]) * (int32_t)wf >> 8);
            a = x * wv >> 15;
            busL[i] += a * gl >> 14;
            busR[i] += a * gr_ >> 14;
            pos = rev ? pos - inc : pos + inc;
            wph += wstep;
            if (wph > (WIN_N << 16))
                wph = WIN_N << 16;
        }
        g->pos = pos;
        g->wph = wph;
        g->z = z;
        if (g->age >= g->len)
            g->on = 0;
        else
            load += g->load;
    }
    return load;
}

/* flash code reaches RAM code through pointers (a direct call cannot span from the XIP window to RAM); volatile, so
 * the compiler keeps the indirect call */
static float (*volatile grains_part_fn)(uint32_t, uint32_t, uint32_t, int32_t *, int32_t *) = render_grains_part;

/* the second core (app.c gives it: ef_set_cpu2): it renders the odd grains into bus2 while this core does the even */
static int32_t busIL[SUB], busIR[SUB], bus2L[SUB], bus2R[SUB];
static float load2;
static int (*cpu2_run)(void (*)(uint32_t), uint32_t);
static int (*cpu2_wait)(void);
static volatile uint32_t cpu2_jobs, cpu2_fails;
void ef_set_cpu2(int (*run)(void (*)(uint32_t), uint32_t), int (*wait)(void))
{
    cpu2_run = run;
    cpu2_wait = wait;
}
uint32_t ef_cpu2_jobs(void) { return cpu2_jobs; }
static void grains_job(uint32_t n)       /* runs on the second core */
{
    uint32_t i;
    for (i = 0; i < n; i++)
        bus2L[i] = bus2R[i] = 0;
    load2 = grains_part_fn(n, 1, 2, bus2L, bus2R);
}

static void render_grains(uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        busIL[i] = busIR[i] = 0;
    if (cpu2_run && cpu2_run(grains_job, n)) {
        float own = grains_part_fn(n, 0, 2, busIL, busIR);
        if (cpu2_wait() == 0) {
            for (i = 0; i < n; i++) {
                busIL[i] += bus2L[i];
                busIR[i] += bus2R[i];
            }
            load_now = own + load2;
            cpu2_jobs++;
        } else {                         /* given up (the job dropped, never run): this core does them */
            load_now = own + grains_part_fn(n, 1, 2, busIL, busIR);
            cpu2_fails++;
        }
    } else
        load_now = grains_part_fn(n, 0, 1, busIL, busIR);
    for (i = 0; i < n; i++) {
        busL[i] = (float)busIL[i] * (1.0f / 32768.0f);
        busR[i] = (float)busIR[i] * (1.0f / 32768.0f);
    }
}

static void schedule(const ctl_t *c, uint32_t n)
{
    uint32_t i;
    notes_track();
    if (ctl.play && !ctl.nnotes)
        held_n = 0;                      /* the drone again at its own pitch */
    if (!src || (env <= 0.0005f && !ctl.play && !ctl.nnotes))
        return;
    if (ctl.note_seq != note_seq_seen) {   /* a new key: two grains at once, so the key answers */
        note_seq_seen = ctl.note_seq;
        if (!c->clocked && ctl.nnotes) {
            spawn(c, 0, 1.0f, 0.0f, -1.0f);
            spawn(c, 0, 0.8f, 0.0f, -1.0f);
        }
    }
    if (c->clocked) {
        float bpm = 30.0f + 90.0f * sm[P_MOTION], ext = ctl.ext_bpm, step_samples;
        int spb = 4;                     /* steps a quarter note */
        if (ext > 0.0f) {
            /* on a MIDI clock: its 1/16, 1/8 or 1/4, whichever is nearest ECHOFIELD's own pace */
            float own = bpm, best = 1e9f;
            int d;
            for (d = 1; d <= 4; d <<= 1) {
                float r = ext / (float)d, e = r > own ? r / own : own / r;
                if (e < best) {
                    best = e;
                    bpm = r;
                    spb = 4 / d;
                }
            }
        }
        step_samples = (float)EF_SR * 60.0f / bpm / 4.0f;
        if (ext > 0.0f && ctl.ext_beat_seq != ext_seq_seen) {
            /* a quarter note of the clock: back onto its grid (unless that step has just played, a hair early) */
            int target = (int)((ctl.ext_beat * (uint32_t)spb) & 15u);
            ext_seq_seen = ctl.ext_beat_seq;
            if (!(pulse_last == target && pulse_acc > step_samples * 0.5f)) {
                pulse_step = target;
                pulse_acc = 0.0f;
            }
        }
        int k = (int)clampf(5.0f + sm[P_DENS_MUL] * 2.5f + 0.5f, 1.0f, 16.0f);
        uint16_t pat;
        if (orc.on && orc.bloom > 0.0f)
            k = k + 3 > 16 ? 16 : k + 3;
        if (punch(PUNCH_BLOOM))
            k = k + 4 > 16 ? 16 : k + 4;
        euclid(k, 16, pulse_rot, &pat);
        meter.pulse_pat = pat;
        if (punch(PUNCH_HUSH))
            pat = 0;
        for (i = 0; i < n; i++) {
            pulse_acc -= 1.0f;
            if (pulse_acc <= 0.0f) {
                int st = pulse_step;
                pulse_last = st;
                pulse_acc += step_samples * (1.0f + ((pulse_step & 1) ? 0.08f : -0.08f) * sm[P_DRIFT]);
                pulse_step = (pulse_step + 1) & 15;
                meter.pulse_step = (uint8_t)st;
                if (st == 0 && !frozen() && rndf() < sm[P_MOTION] * 0.35f) {
                    int j = (int)(rndf() * 16.0f) & 15;
                    step_pos[j] = rndf();
                    step_oct[j] = rndf() < sm[P_DRIFT] * 0.5f ? (rndf() < 0.5f ? 12 : 7) : 0;
                    if (rndf() < sm[P_MOTION] * 0.3f)
                        pulse_rot = (pulse_rot + 1) & 15;
                }
                if ((pat >> st) & 1u) {
                    float p = c->center + (step_pos[st] - 0.5f) * c->scatter;
                    p -= fm_floorf(p);
                    spawn(c, i, (st % 4 == 0) ? 1.1f : 0.77f, (float)step_oct[st], p);
                }
            }
        }
        return;
    }
    if (c->density <= 0.0f)
        return;
    {
        float mean = (float)EF_SR / c->density;
        int mode = (int)sm[P_MODE];
        for (i = 0; i < n; i++) {
            spawn_wait -= 1.0f;
            if (spawn_wait > 0.0f)
                continue;
            spawn_wait += mean * (1.0f - c->jitter + c->jitter * 2.0f * rndf());
            if (spawn_wait < 1.0f)
                spawn_wait = 1.0f;
            if (mode == MODE_DUST) {
                float sp = 0.0f;
                if (stutter_left > 0) {
                    stutter_left--;
                    spawn(c, i, 1.0f, 0.0f, stutter_pos);
                    continue;
                }
                if (rndf() < sm[P_MOTION] * 0.05f) {
                    stutter_left = 2 + (int)(rndf() * 4.0f);
                    stutter_pos = c->center + rndb() * c->scatter * 0.5f;
                    stutter_pos -= fm_floorf(stutter_pos);
                }
                if (rndf() < sm[P_SCATTER] * 0.35f) {
                    static const float J[5] = {12, 19, 24, 7, -12};
                    sp = J[(uint32_t)(rndf() * 5.0f) % 5u];
                }
                spawn(c, i, 1.0f, sp, -1.0f);
            } else if (mode == MODE_BLOOM) {
                static const float PART[4] = {0, 12, 7, -12};
                float r = rndf();
                int k = r < 0.55f ? 0 : r < 0.75f ? 1 : r < 0.9f ? 2 : 3;
                spawn(c, i, 0.5f + 0.5f * rndf(), PART[k], -1.0f);
            } else
                spawn(c, i, 1.0f, 0.0f, -1.0f);
        }
    }
}

/* ------------------------------------------------------------------ effects */
/* the half-rate section: echo and reverb from one stereo input pair -> out[0..1] echo, out[2..3] reverb */
#define SHIM_DC_R (1.0f - 20.0f * TWOPI / HSR)   /* the shimmer loop's DC blocker, ~20 Hz at the half rate */
INL void half_rate(float il, float ir, float *out, float fb, float dmix, int ping, float dlp, float rev_wet,
                      const float *g, float damp_a, float shimmer)
{
    float dsamp, drp, dfr, el, er, fbl, fbr, rin, y[NFDN], sum = 0.0f, wl, wr;
    uint32_t d0, d1, a, q;
    /* DELAY: smoothed time, filtered and saturated feedback, ping-pong */
    fx.dly_time_smooth += (sm[P_DLY_TIME] - fx.dly_time_smooth) * 0.0008f;
    dsamp = clampf(fx.dly_time_smooth * 0.001f * HSR, 1.0f, (float)(DLY_LEN - 4u));
    drp = (float)fx.dly_w - dsamp;
    if (drp < 0.0f)
        drp += (float)DLY_LEN;
    d0 = (uint32_t)drp;
    dfr = drp - (float)d0;
    if (d0 >= DLY_LEN)
        d0 -= DLY_LEN;
    d1 = d0 + 1u >= DLY_LEN ? 0u : d0 + 1u;
    el = ((float)dly[0][d0] + (float)(dly[0][d1] - dly[0][d0]) * dfr) * (1.0f / 16384.0f);
    er = ((float)dly[1][d0] + (float)(dly[1][d1] - dly[1][d0]) * dfr) * (1.0f / 16384.0f);
    fx.dly_lp[0] += dlp * (el - fx.dly_lp[0]);
    fx.dly_lp[1] += dlp * (er - fx.dly_lp[1]);
    fbl = tanh_fast(fx.dly_lp[0] * fb);
    fbr = tanh_fast(fx.dly_lp[1] * fb);
    {
        float wl0 = ping ? (il + ir) * 0.5f + fbr : il + fbl, wr0 = ping ? fbl : ir + fbr;
        dly[0][fx.dly_w] = (int16_t)clampf(wl0 * 16384.0f, -32767.0f, 32767.0f);
        dly[1][fx.dly_w] = (int16_t)clampf(wr0 * 16384.0f, -32767.0f, 32767.0f);
    }
    if (++fx.dly_w >= DLY_LEN)
        fx.dly_w = 0;
    out[0] = el * dmix;
    out[1] = er * dmix;

    /* REVERB: four diffusers, an 8-line FDN (Householder) with damping, an optional +12 st shimmer */
    rin = (il + ir + out[0] + out[1]) * 0.5f + fx.shim_fb * shimmer * 0.16f;
    for (a = 0; a < 4u; a++) {
        float *buf = ap[a], dd = buf[fx.ap_w[a]], v = rin + 0.6f * dd;
        buf[fx.ap_w[a]] = v;
        rin = dd - 0.6f * v;
        if (++fx.ap_w[a] >= fx.ap_len[a])
            fx.ap_w[a] = 0;
    }
    fx.rev_ph1 += 0.31f / HSR;
    if (fx.rev_ph1 > 1.0f)
        fx.rev_ph1 -= 1.0f;
    fx.rev_ph2 += 0.47f / HSR;
    if (fx.rev_ph2 > 1.0f)
        fx.rev_ph2 -= 1.0f;
    if ((fx.mod_k++ & 15u) == 0) {       /* the two modulated lines' offsets, every 16 samples */
        fx.mod1 = 6.0f * (1.0f + fm_sin_turns(fx.rev_ph1));
        fx.mod5 = 6.0f * (1.0f + fm_sin_turns(fx.rev_ph2));
    }
    for (q = 0; q < NFDN; q++) {
        float mod = q == 1u ? fx.mod1 : q == 5u ? fx.mod5 : 0.0f;
        float *line = fdn_mem + fx.fdn_off[q], rpos = (float)fx.fdn_w[q] - (float)fx.fdn_len[q] - mod, rf, v;
        uint32_t r0, r1, cap = fx.fdn_cap[q];
        while (rpos < 0.0f)
            rpos += (float)cap;
        r0 = (uint32_t)rpos;
        rf = rpos - (float)r0;
        if (r0 >= cap)
            r0 -= cap;
        r1 = r0 + 1u >= cap ? 0u : r0 + 1u;
        v = line[r0] + (line[r1] - line[r0]) * rf;
        fx.fdn_lp[q] += damp_a * (v - fx.fdn_lp[q]);
        y[q] = fx.fdn_lp[q] * g[q];
        sum += y[q];
    }
    sum *= 0.25f;
    for (q = 0; q < NFDN; q++) {
        float inj = (q & 1u) ? ((q & 2u) ? -rin : rin) : ((q & 2u) ? rin : -rin);
        fdn_mem[fx.fdn_off[q] + fx.fdn_w[q]] = fm_flush(y[q] - sum + inj * 0.5f);
        if (++fx.fdn_w[q] >= fx.fdn_cap[q])
            fx.fdn_w[q] = 0;
    }
    wl = (y[2] - y[0] + y[6] - y[4]) * 0.6f;
    wr = (y[1] - y[3] + y[5] - y[7]) * 0.6f;
    if (shimmer > 0.001f) {
        float o = 0.0f;
        uint32_t t;
        shim[fx.shim_w] = (wl + wr) * 0.25f;
        fx.shim_ph += 1.0f / SHIM_W;
        if (fx.shim_ph >= 1.0f)
            fx.shim_ph -= 1.0f;
        for (t = 0; t < 2u; t++) {
            float ph = fx.shim_ph + (float)t * 0.5f, dd, rp2, sf;
            uint32_t s0, s1;
            if (ph >= 1.0f)
                ph -= 1.0f;
            dd = (1.0f - ph) * SHIM_W + 2.0f;
            rp2 = (float)fx.shim_w - dd;
            while (rp2 < 0.0f)
                rp2 += (float)SHIM_LEN;
            s0 = (uint32_t)rp2 & (SHIM_LEN - 1u);
            sf = rp2 - fm_floorf(rp2);
            s1 = (s0 + 1u) & (SHIM_LEN - 1u);
            o += (shim[s0] + (shim[s1] - shim[s0]) * sf) * (1.0f - fm_fabsf(2.0f * ph - 1.0f));   /* a triangle crossfade */
        }
        fx.shim_w = (fx.shim_w + 1u) & (SHIM_LEN - 1u);
        fx.shim_lp += 0.4f * (o - fx.shim_lp);
        /* no DC round the loop: an octave up of 0 Hz is 0 Hz, and the reverb's own gain at DC (its long lines add
         * up) made the loop's gain over 1 at long sizes: a DC that grew until the limiter held it (~20 Hz high-pass) */
        o = fx.shim_lp - fx.shim_dx + SHIM_DC_R * fx.shim_dy;
        fx.shim_dx = fx.shim_lp;
        fx.shim_dy = fm_flush(o);
        fx.shim_fb = tanh_fast(fx.shim_dy * 0.9f);
    } else
        fx.shim_fb = 0.0f;
    out[2] = wl * rev_wet;
    out[3] = wr * rev_wet;
}

static EF_RAM void fx_process(float *L, float *R, uint32_t n);
static void (*volatile fx_fn)(float *, float *, uint32_t) = fx_process;
static EF_RAM void fx_process(float *L, float *R, uint32_t n)
{
    const float sr = (float)EF_SR;
    float *s = sm;
    int mode = (int)s[P_MODE];
    float tape = mode == MODE_TAPE ? fm_maxf(s[P_TAPE], 0.35f) : s[P_TAPE];
    float wow_d = tape * s[P_TAPE_WOW] * 0.0025f * sr, flut_d = tape * s[P_TAPE_FLUTTER] * 0.00015f * sr;
    float sat = 1.0f + 3.0f * tape, sat_norm = 1.0f / tanh_fast(sat);
    float tone_a = onepole(16000.0f * (1.0f - 0.8f * tape * (0.3f + 0.7f * s[P_TAPE_AGE])), sr);
    float hiss = s[P_TAPE_NOISE] * 0.012f * (0.3f + tape), age = tape * s[P_TAPE_AGE];
    int ftype = (int)s[P_FILTER_TYPE];
    float reso = s[P_RESO], cut_target = s[P_CUTOFF];
    float k_res, drive = 1.0f + 6.0f * s[P_DRIVE], drive_norm = fm_rsqrtf(drive);
    float fb = fm_minf(s[P_DLY_FB], 0.92f) * 0.98f, dmix = s[P_DLY_MIX];
    int ping = s[P_DLY_PINGPONG] > 0.5f;
    float dlp = onepole(5000.0f, HSR);
    float space = s[P_SPACE], rev_wet = fm_powf(space, 1.2f) * 0.9f, dry, t60, g[NFDN], damp_a, shimmer, width, master;
    float dc_r = 1.0f - 20.0f * TWOPI / sr, out_r = 1.0f - 5.0f * TWOPI / sr;
    uint32_t i, q;
    if (punch(PUNCH_THROW)) {
        fb = 0.86f;
        dmix = fm_maxf(dmix, 0.7f);
    }
    if (punch(PUNCH_SWEEP)) {
        sweep_ph += (float)n * (0.35f / sr);
        if (sweep_ph >= 1.0f)
            sweep_ph -= 1.0f;
        cut_target = 0.35f + 0.5f * (0.5f + 0.5f * fm_sin_turns(sweep_ph));
        reso = fm_maxf(reso, 0.6f);
        ftype = 0;
    }
    k_res = 2.0f - 1.94f * reso;
    if (mode == MODE_BLOOM || mode == MODE_DUST)
        rev_wet = fm_minf(1.0f, rev_wet + 0.2f);
    shimmer = s[P_SHIMMER];
    if (punch(PUNCH_SHIMMER)) {
        shimmer = 1.0f;
        rev_wet = fm_maxf(rev_wet, 0.8f);
    }
    dry = 1.0f - 0.35f * space;
    t60 = 0.6f * fm_powf(24.0f, s[P_REV_SIZE]) * (0.7f + 0.6f * space);
    for (q = 0; q < NFDN; q++)
        g[q] = fm_exp2f(-3.0f * 3.3219281f * (float)fx.fdn_len[q] / (t60 * HSR));
    damp_a = onepole(fm_minf(12000.0f * fm_powf(0.12f, s[P_REV_DAMP]), 10000.0f), HSR);
    width = 0.4f + 0.8f * s[P_WIDTH];
    master = 1.6f * s[P_LEVEL] * s[P_LEVEL];

    for (i = 0; i < n; i++) {
        float l = L[i], r = R[i], yl, yr, dl, rp, fr, tl, tr, a1, in2[2], m, sd;
        uint32_t i0, i1, c;
        /* DC blocker */
        yl = l - fx.dc_x[0] + dc_r * fx.dc_y[0];
        fx.dc_x[0] = l;
        fx.dc_y[0] = yl;
        yr = r - fx.dc_x[1] + dc_r * fx.dc_y[1];
        fx.dc_x[1] = r;
        fx.dc_y[1] = yr;
        l = yl;
        r = yr;

        /* TAPE: a modulated short delay (wow, flutter), saturation, tone, wear, hiss */
        if (!(ctl.dbg_bypass & 1u)) {
        fx.wow_ph += (0.55f + 0.1f * fx.wow_noise) / sr;
        if (fx.wow_ph >= 1.0f) {
            fx.wow_ph -= 1.0f;
            fx.wow_noise = fx_rndb();
        }
        fx.flut_ph += 7.3f / sr;
        if (fx.flut_ph >= 1.0f)
            fx.flut_ph -= 1.0f;
        if ((i & 31u) == 0)
            fx.flut_noise = smooth_to(fx.flut_noise, fx_rndb(), 0.3f);
        if ((i & 7u) == 0)               /* the wow and flutter move slowly: their sines every 8 samples */
            fx.dl = 0.006f * sr + wow_d * (1.0f + fm_sin_turns(fx.wow_ph)) +
                    flut_d * (1.0f + 0.6f * fm_sin_turns(fx.flut_ph) + 0.4f * fx.flut_noise);
        dl = fx.dl;
        tape_buf[fx.tape_w * 2u] = l;
        tape_buf[fx.tape_w * 2u + 1u] = r;
        rp = (float)fx.tape_w - dl;
        while (rp < 0.0f)
            rp += (float)TAPE_LEN;
        i0 = (uint32_t)rp & (TAPE_LEN - 1u);
        fr = rp - fm_floorf(rp);
        i1 = (i0 + 1u) & (TAPE_LEN - 1u);
        tl = tape_buf[i0 * 2u] + (tape_buf[i1 * 2u] - tape_buf[i0 * 2u]) * fr;
        tr = tape_buf[i0 * 2u + 1u] + (tape_buf[i1 * 2u + 1u] - tape_buf[i0 * 2u + 1u]) * fr;
        fx.tape_w = (fx.tape_w + 1u) & (TAPE_LEN - 1u);
        if (tape > 0.001f) {
            float mixw = fm_minf(1.0f, tape * 4.0f);
            tl = tanh_fast(tl * sat) * sat_norm;
            tr = tanh_fast(tr * sat) * sat_norm;
            fx.tape_lpz[0] += tone_a * (tl - fx.tape_lpz[0]);
            fx.tape_lpz[1] += tone_a * (tr - fx.tape_lpz[1]);
            tl = fx.tape_lpz[0];
            tr = fx.tape_lpz[1];
            if (age > 0.01f) {
                fx.age_timer -= 1.0f / sr;
                if (fx.age_timer <= 0.0f) {
                    fx.age_timer = 0.05f;
                    if ((fx_rndb() * 0.5f + 0.5f) < age * 0.012f)
                        fx.age_gain = 0.35f;
                }
                fx.age_gain += (1.0f - fx.age_gain) * 0.0006f;
                tl *= fx.age_gain;
                tr *= fx.age_gain;
            }
            l = l + (tl - l) * mixw;
            r = r + (tr - r) * mixw;
        }
        if (hiss > 0.0f) {
            fx.hiss_z[0] += 0.3f * (fx_rndb() - fx.hiss_z[0]);
            fx.hiss_z[1] += 0.3f * (fx_rndb() - fx.hiss_z[1]);
            l += fx.hiss_z[0] * hiss;
            r += fx.hiss_z[1] * hiss;
        }
        }

        /* FILTER: TPT state-variable, the cutoff smoothed per sample, the coefficient every 8 */
        if (!(ctl.dbg_bypass & 2u)) {
        fx.cut_smooth += (cut_target - fx.cut_smooth) * 0.002f;
        if ((i & 7u) == 0 || fx.g_coef == 0.0f) {
            float hz = fm_minf(30.0f * fm_powf(600.0f, fx.cut_smooth), sr * 0.45f);
            fx.g_coef = fm_tanf(PI * hz / sr);
            fx.a1 = 1.0f / (1.0f + fx.g_coef * (fx.g_coef + k_res));   /* (the one divide, every 8 samples) */
        }
        a1 = fx.a1;
        in2[0] = l;
        in2[1] = r;
        for (c = 0; c < 2u; c++) {
            float v3 = in2[c] - fx.svf2[c], v1 = a1 * fx.svf1[c] + fx.g_coef * a1 * v3, v2 = fx.svf2[c] + fx.g_coef * v1;
            fx.svf1[c] = fm_flush(2.0f * v1 - fx.svf1[c]);
            fx.svf2[c] = fm_flush(2.0f * v2 - fx.svf2[c]);
            in2[c] = ftype == 0 ? v2 : ftype == 1 ? v1 * k_res : in2[c] - k_res * v1 - v2;
        }
        l = tanh_fast(in2[0] * drive) * drive_norm;
        r = tanh_fast(in2[1] * drive) * drive_norm;
        }

        /* the half-rate section, one pair of samples at a time, its output interpolated back */
        if (ctl.dbg_bypass & 4u) {
        } else if (!fx.hr_phase) {
            fx.hr_in[0] = l;
            fx.hr_in[1] = r;
            fx.hr_phase = 1;
            l = (l + fx.hr_cur[0]) * dry + fx.hr_cur[2];
            r = (r + fx.hr_cur[1]) * dry + fx.hr_cur[3];
        } else {
            float o[4];
            uint32_t k;
            half_rate((fx.hr_in[0] + l) * 0.5f, (fx.hr_in[1] + r) * 0.5f, o, fb, dmix, ping, dlp, rev_wet, g, damp_a,
                      shimmer);
            for (k = 0; k < 4u; k++) {
                fx.hr_prev[k] = fx.hr_cur[k];
                fx.hr_cur[k] = o[k];
            }
            fx.hr_phase = 0;
            l = (l + 0.5f * (fx.hr_prev[0] + fx.hr_cur[0])) * dry + 0.5f * (fx.hr_prev[2] + fx.hr_cur[2]);
            r = (r + 0.5f * (fx.hr_prev[1] + fx.hr_cur[1])) * dry + 0.5f * (fx.hr_prev[3] + fx.hr_cur[3]);
        }

        /* WIDTH (M/S) and the preset's level, then no DC out (~5 Hz high-pass: whatever a stage may leave, the
         * limiter's headroom and a recording's are the music's) */
        m = (l + r) * 0.5f;
        sd = (l - r) * 0.5f * width;
        l = (m + sd) * master;
        r = (m - sd) * master;
        yl = l - fx.out_dx[0] + out_r * fx.out_dy[0];
        fx.out_dx[0] = l;
        fx.out_dy[0] = fm_flush(yl);
        yr = r - fx.out_dx[1] + out_r * fx.out_dy[1];
        fx.out_dx[1] = r;
        fx.out_dy[1] = fm_flush(yr);
        L[i] = yl;
        R[i] = yr;
    }
}

/* a slow automatic level: sparse and dense sounds at a similar loudness (about -18 dBFS RMS, +12 / -6 dB at most) */
static void auto_level(float *L, float *R, uint32_t n)
{
    float ms = 0.0f, blk = (float)n / (float)EF_SR, target = fx.agc_gain, g0 = fx.agc_gain, tc;
    int sounding = env > 0.5f;           /* a tail that dies away is never pulled back up */
    uint32_t i;
    for (i = 0; i < n; i++)
        ms += 0.5f * (L[i] * L[i] + R[i] * R[i]);
    ms /= (float)n;
    fx.agc_ms += (ms - fx.agc_ms) * (1.0f - fm_expf(-blk / 2.5f));
    if (fx.agc_ms > 1e-6f)
        target = clampf(0.126f * fm_rsqrtf(fx.agc_ms), 0.5f, 4.0f);
    if (!sounding && target > fx.agc_gain)
        target = fx.agc_gain;
    tc = target < fx.agc_gain ? 0.4f : 2.5f;
    fx.agc_gain += (target - fx.agc_gain) * (1.0f - fm_expf(-blk / tc));
    for (i = 0; i < n; i++) {
        float g = g0 + (fx.agc_gain - g0) * (float)i / (float)n;
        L[i] *= g;
        R[i] *= g;
    }
}

static void limiter(float *L, float *R, uint32_t n)
{
    const float thr = 0.89f, rel = 1.0f - fm_expf(-1.0f / (0.15f * (float)EF_SR));
    float pk_out = meter.peak;
    uint32_t i;
    for (i = 0; i < n; i++) {
        float l = L[i], r = R[i], pk, need;
        if (!(l == l) || fm_fabsf(l) > 1e6f)
            l = 0.0f;
        if (!(r == r) || fm_fabsf(r) > 1e6f)
            r = 0.0f;
        pk = fm_maxf(fm_fabsf(l), fm_fabsf(r));
        need = pk > thr ? thr / pk : 1.0f;
        if (need < fx.lim_gain)
            fx.lim_gain = need;
        else
            fx.lim_gain += (1.0f - fx.lim_gain) * rel;
        if (fx.lim_gain < 0.999f)
            meter.limiting = 1;
        l *= fx.lim_gain;
        r *= fx.lim_gain;
        if (fm_fabsf(l) > 0.9f)
            l = (l > 0.0f ? 1.0f : -1.0f) * (0.9f + 0.1f * tanh_fast((fm_fabsf(l) - 0.9f) * 10.0f));
        if (fm_fabsf(r) > 0.9f)
            r = (r > 0.0f ? 1.0f : -1.0f) * (0.9f + 0.1f * tanh_fast((fm_fabsf(r) - 0.9f) * 10.0f));
        L[i] = clampf(l, -1.0f, 1.0f);
        R[i] = clampf(r, -1.0f, 1.0f);
        pk_out = fm_maxf(pk_out, fm_maxf(fm_fabsf(L[i]), fm_fabsf(R[i])));
    }
    meter.peak = pk_out;
}

/* ------------------------------------------------------------------ RESAMPLE */
void ef_rec_start(uint8_t *buf, uint32_t nsec)
{
    rec_on = 0;
    rec_buf = buf;
    rec_cap = nsec * EF_SEC_SAMPLES;
    rec_n = 0;
    rec_half = 0;
    rec_acc = 0.0f;
    rec_ima.pred = 0;
    rec_ima.index = 0;
    rec_on = buf && nsec;
}
uint32_t ef_rec_stop(void)
{
    rec_on = 0;
    return rec_n;
}
uint32_t ef_rec_frames(void) { return rec_n; }
int ef_rec_on(void) { return rec_on; }

static void rec_feed(const float *L, const float *R, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n && rec_on; i++) {
        float m = (L[i] + R[i]) * 0.5f;
        int32_t x;
        uint8_t *blk;
        uint32_t j, nib;
        if (!rec_half) {
            rec_acc = m;
            rec_half = 1;
            continue;
        }
        rec_half = 0;
        x = (int32_t)clampf((rec_acc + m) * 0.5f * 32767.0f, -32767.0f, 32767.0f);
        blk = rec_buf + (rec_n / EF_BLK_SAMPLES) * EF_BLK_BYTES;
        j = rec_n % EF_BLK_SAMPLES;
        if (!j) {                        /* a block's header: the encoder's state before its first sample */
            blk[0] = (uint8_t)rec_ima.pred;
            blk[1] = (uint8_t)((uint32_t)rec_ima.pred >> 8);
            blk[2] = (uint8_t)rec_ima.index;
            blk[3] = 0;
        }
        nib = ef_ima_encode(&rec_ima, x);
        if (j & 1u)
            blk[4u + j / 2u] = (uint8_t)((blk[4u + j / 2u] & 15u) | nib << 4);
        else
            blk[4u + j / 2u] = (uint8_t)nib;
        if (++rec_n >= rec_cap)
            rec_on = 0;
    }
}

/* ------------------------------------------------------------------ the render */
static void write_vis(const ctl_t *c)
{
    uint32_t k, n = 0;
    float fr = src ? 1.0f / (float)src->frames : 0.0f, ra, rb;
    for (k = 0; k < EF_MAX_GRAINS && n < EF_VIS_N; k++) {
        grain_t *g = &gr[k];
        float x, a;
        if (!g->on)
            continue;
        x = (float)(g->pos >> Q) * fr;
        a = win_tab[g->win][g->wph >> 16 > WIN_N ? WIN_N : g->wph >> 16] * (float)(g->gl + g->gr) * (0.7f / 16384.0f);
        vis.x[n] = (uint8_t)clampf(x * 255.0f, 0.0f, 255.0f);
        vis.pitch[n] = g->vis_pitch;
        vis.amp[n] = (uint8_t)clampf(a * 255.0f, 0.0f, 255.0f);
        vis.pan[n] = (uint8_t)clampf((float)g->gr / ((float)(g->gl + g->gr) + 1e-9f) * 255.0f, 0.0f, 255.0f);
        n++;
    }
    vis.n = (uint8_t)n;
    meter.grains = (uint8_t)n;
    ra = fm_minf(sm[P_START], sm[P_END]);
    rb = fm_maxf(sm[P_START], sm[P_END]);
    meter.center = (uint8_t)clampf((ra + (rb - ra) * c->center) * 255.0f, 0.0f, 255.0f);
    meter.spread = (uint8_t)clampf((rb - ra) * c->scatter * 255.0f, 0.0f, 255.0f);
    meter.head = c->tape ? (uint8_t)clampf((ra + (rb - ra) * tape_head) * 255.0f, 0.0f, 255.0f) : 255u;
    meter.env = env;
}

void ef_listen(int on, int loop)
{
    aud_loop = (uint8_t)(loop != 0);
    aud_req = on ? 1u : 2u;
}
int ef_listening(void) { return aud_on || aud_req == 1u; }

/* the LISTEN voice into the bus (after the effects: dry), and the cloud ducked under it */
static void listen(uint32_t n)   /* (the region as set, not smoothed: LISTEN is exact) */
{
    uint32_t i, last, a, b;
    float ra, rb;
    if (aud_req == 2u) {
        aud_req = 0;
        aud_on = 0;                      /* (aud_gain fades it out below) */
    }
    if (aud_req == 1u && !src_pending && src && src->frames > 512u) {
        aud_req = 0;
        ra = fm_minf(ctl.p[P_START], ctl.p[P_END]);
        aud.blk = -1;
        aud.pos = (uint32_t)(ra * (float)(src->frames - 2u)) << Q;
        aud.inc = (uint32_t)((float)src->rate * (float)QONE / (float)EF_SR);
        aud_on = 1;
    }
    if (!aud_on && aud_gain <= 0.0f && duck <= 0.0f)
        return;
    meter.aud = 0;
    ra = fm_minf(ctl.p[P_START], ctl.p[P_END]);
    rb = fm_maxf(ctl.p[P_START], ctl.p[P_END]);
    a = src ? (uint32_t)(ra * (float)(src->frames - 2u)) : 0u;
    b = src ? (uint32_t)(rb * (float)(src->frames - 2u)) : 0u;
    if (b < a + 64u)
        b = a + 64u;
    last = src && src->frames > 2u ? src->frames - 2u : 0u;
    if (b > last)
        b = last;
    for (i = 0; i < n; i++) {
        float x = 0.0f, gt = aud_on ? 1.0f : 0.0f;
        duck += ((aud_on ? 1.0f : 0.0f) - duck) * 0.0015f;   /* ~15 ms */
        aud_gain += (gt - aud_gain) * 0.004f;                /* ~6 ms: no click at either end */
        if (aud_on && src) {
            uint32_t idx = aud.pos >> Q, bk, j;
            if (idx >= b || idx >= last) {  /* the region's end: again from its start, or the end */
                if (aud_loop)
                    aud.pos = a << Q;
                else
                    aud_on = 0;
                idx = aud.pos >> Q;
            }
            bk = idx >> 8;
            j = idx & (EF_BLK_SAMPLES - 1u);
            if (aud_on && bk < nblocks(src)) {   /* (never past the sample: nothing is read once it stops) */
                if ((int32_t)bk != aud.blk)
                    grain_load(&aud, bk);
                aud_last = (float)aud.cache[j] +
                           (float)(aud.cache[j + 1u] - aud.cache[j]) * (float)(aud.pos & (QONE - 1u)) * (1.0f / (float)QONE);
                aud.pos += aud.inc;
            } else
                aud_on = 0;
        }
        x = aud_last * aud_gain * (0.85f / 32768.0f);   /* (stopping: the last sample, faded out) */
        busL[i] = busL[i] * (1.0f - duck) + x;
        busR[i] = busR[i] * (1.0f - duck) + x;
    }
    if (aud_on && src)
        meter.aud = (uint8_t)clampf((float)(aud.pos >> Q) / (float)src->frames * 254.0f + 1.0f, 1.0f, 255.0f);
}

/* the delay time on a MIDI clock: the note value nearest the knob's time, folded into the delay's 20..1000 ms */
static float dly_on_clock(float ms, float bpm)
{
    static const float BEATS[] = {0.25f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.75f, 1.0f, 1.5f, 2.0f};
    float beat = 60000.0f / bpm, best = ms, err = 1e9f;
    uint32_t k;
    for (k = 0; k < sizeof BEATS / sizeof BEATS[0]; k++) {
        float t = beat * BEATS[k], e;
        if (t < 20.0f || t > 1000.0f)
            continue;
        e = t > ms ? t / ms : ms / t;
        if (e < err) {
            err = e;
            best = t;
        }
    }
    return best;
}

void ef_render(int32_t *lr, uint32_t n, float gain)
{
    static const float SMOOTH_A = 0.0352f;   /* 1 - exp(-SUB / (0.04 s * 44100)) */
    const float dt = (float)SUB / (float)EF_SR;
    uint32_t done = 0, i, t0 = plat_ticks24(), t1;
    ctl_t c;
    float atk, rel, env_target, ts_target;
    gain = clampf(gain, 0.0f, 1.0f) * 8300000.0f;
    while (done < n) {
        uint32_t m = n - done < SUB ? n - done : SUB;
        float mdt = dt * (float)m / (float)SUB;
        for (i = 0; i < P_COUNT; i++) {
            float t = ctl.p[i];
            if (!(t == t))
                t = 0.0f;
            sm[i] = (i == P_DLY_TIME || i == P_CUTOFF || i == P_MODE || i == P_WINDOW || i == P_FILTER_TYPE ||
                     i == P_OR_SCALE || i == P_OR_ROOT || i == P_OR_RANGE || i == P_KEYMODE)
                        ? t
                        : smooth_to(sm[i], t, SMOOTH_A);
        }
        if (ctl.ext_bpm > 0.0f)
            sm[P_DLY_TIME] = dly_on_clock(sm[P_DLY_TIME], ctl.ext_bpm);
        /* a new source: fade out, swap, fade in */
        if (src_pending) {
            if (!src || fade_gain <= 0.0f) {
                src = src_next;
                aud_on = 0;              /* (a LISTEN of the old source ends with it) */
                aud_gain = 0.0f;
                for (i = 0; i < EF_MAX_GRAINS; i++)
                    gr[i].on = 0;
                tape_head = 0.0f;
                fade_gain = 0.0f;
                src_pending = 0;
            } else
                fade_gain = fm_maxf(0.0f, fade_gain - (float)m / (0.02f * (float)EF_SR));
        } else if (fade_gain < 1.0f)
            fade_gain = fm_minf(1.0f, fade_gain + (float)m / (0.02f * (float)EF_SR));

        /* the gate: PLAY holds it open, keys open it with their attack, release closes it */
        atk = 0.005f * fm_powf(800.0f, sm[P_ATTACK]);
        rel = 0.01f * fm_powf(800.0f, sm[P_RELEASE]);
        env_target = (ctl.play || ctl.nnotes) ? 1.0f : 0.0f;
        env += (env_target - env) * fm_minf(1.0f, mdt / (env_target > env ? atk : rel));
        ts_target = punch(PUNCH_TAPESTOP) ? 1.0f : 0.0f;
        tapestop += (ts_target - tapestop) * fm_minf(1.0f, mdt * (ts_target > tapestop ? 1.4f : 6.0f));

        oracle_update(mdt);
        compute_ctl(&c, mdt);
        if (c.tape && src && !frozen()) {
            float a = fm_minf(sm[P_START], sm[P_END]), b = fm_maxf(sm[P_START], sm[P_END]);
            float rlen = fm_maxf(512.0f, (b - a) * (float)src->frames);
            tape_head += c.tape_speed * (1.0f - 0.9f * tapestop) * (float)m * ((float)src->rate / (float)EF_SR) / rlen;
            tape_head -= fm_floorf(tape_head);
        }
        for (i = 0; i < m; i++)
            busL[i] = busR[i] = 0.0f;
        t1 = plat_ticks24();
        if (!src_pending)
            schedule(&c, m);
        TMAX(t_spawn, t1);
        t1 = plat_ticks24();
        render_grains(m);
        TMAX(t_grains, t1);
        t1 = plat_ticks24();
        for (i = 0; i < m; i++) {
            float g;
            play_gain += (env - play_gain) * 0.01f;
            g = play_gain * fade_gain * (1.0f - 0.7f * tapestop);
            busL[i] *= g;
            busR[i] *= g;
        }
        fx_fn(busL, busR, m);
        auto_level(busL, busR, m);
        listen(m);
        limiter(busL, busR, m);
        TMAX(t_fx, t1);
        if (rec_on)
            rec_feed(busL, busR, m);
        for (i = 0; i < m; i++) {
            lr[2u * (done + i)] = (int32_t)(busL[i] * gain);
            lr[2u * (done + i) + 1u] = (int32_t)(busR[i] * gain);
        }
        done += m;
    }
    write_vis(&c);
    TMAX(t_render, t0);
}
