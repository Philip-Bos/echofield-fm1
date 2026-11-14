/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Philip Bos */
/* ECHOFIELD FM-1: the parameters' table (names, ranges, steps, how they read), shared by the knobs, the presets, the
 * session, MIDI and the companion app (which mirrors this table in companion/lib/efparams.js: same order, same
 * ranges). Part of app.c's unity build. */

enum { PK_CONT, PK_INT, PK_ENUM, PK_BOOL };

typedef struct {
    const char *key;                     /* stable name (the companion app, preset files) */
    const char *label;                   /* on a knob cell: 9 characters at most */
    uint8_t kind;
    uint8_t rnd;                         /* RANDOM may change it */
    uint8_t cc;                          /* MIDI CC (0: none) */
    float min, max, def, step;           /* step: one detent */
    const char *const *names;            /* PK_ENUM / PK_BOOL */
} pinfo_t;

static const char *const MODE_NAME[MODE_COUNT] = {"CLOUD", "DUST", "TAPE", "BLOOM", "PULSE", "DRIFT"};
static const char *const MODE_BLURB[MODE_COUNT] = {
    "long grains melt into a pad", "tiny specks strewn in space", "three tape loops, three speeds",
    "slow swells with overtones", "soft notes on a slow clock", "everything wanders slowly"};
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const SCALE_NAME[12] = {"MAJOR", "MINOR", "DORIAN", "PHRYG", "LYDIAN", "MIXO",
                                           "PENT MAJ", "PENT MIN", "HIRAJOSHI", "IN-SEN", "WHOLE", "GLASS"};
static const char *const WIN_NAME[6] = {"AUTO", "HANN", "TUKEY", "PERC", "SWELL", "GAUSS"};
static const char *const FILTER_NAME[3] = {"LOW", "BAND", "HIGH"};
static const char *const ONOFF[2] = {"OFF", "ON"};
static const char *const KEYMODE_NAME[2] = {"GATE", "LATCH"};

#define C(k, l, mn, mx, d, st, r, cc) {k, l, PK_CONT, r, cc, mn, mx, d, st, 0}
#define E(k, l, mx, d, nm, r, cc) {k, l, PK_ENUM, r, cc, 0, mx, d, 1, nm}
#define B(k, l, d, cc) {k, l, PK_BOOL, 0, cc, 0, 1, d, 1, ONOFF}

static const pinfo_t PI[P_COUNT] = {
    [P_POSITION] = C("position", "POSITION", 0, 1, 0.35f, 0.005f, 1, 14),
    [P_TEXTURE] = C("texture", "TEXTURE", 0, 1, 0.72f, 0.01f, 1, 15),
    [P_SCATTER] = C("scatter", "SCATTER", 0, 1, 0.45f, 0.01f, 1, 16),
    [P_DRIFT] = C("drift", "DRIFT", 0, 1, 0.35f, 0.01f, 1, 17),
    [P_MOTION] = C("motion", "EVOLVE", 0, 1, 0.35f, 0.01f, 1, 18),
    [P_PITCH] = {"pitch", "PITCH", PK_INT, 1, 19, -24, 24, 0, 1, 0},
    [P_SPACE] = C("space", "REVERB", 0, 1, 0.65f, 0.01f, 1, 91),
    [P_TAPE] = C("tape", "TAPE", 0, 1, 0.12f, 0.01f, 1, 21),
    [P_WIDTH] = C("width", "WIDTH", 0, 1, 0.85f, 0.01f, 1, 23),
    [P_LEVEL] = C("level", "LEVEL", 0, 1, 0.75f, 0.01f, 0, 7),
    [P_HARMONY] = C("harmony", "HARMONY", 0, 1, 0.30f, 0.01f, 1, 22),
    [P_SIZE_MUL] = C("size_mul", "SIZE", -2, 2, 0, 0.05f, 1, 24),
    [P_DENS_MUL] = C("dens_mul", "DENSITY", -2, 2, 0, 0.05f, 1, 25),
    [P_SCAN] = C("scan", "SCAN", -1, 1, 0, 0.02f, 1, 26),
    [P_REVERSE] = C("reverse", "REVERSE", 0, 1, 0.10f, 0.01f, 1, 27),
    [P_PITCH_RAND] = C("pitch_rand", "PITCH RND", 0, 12, 0, 0.1f, 1, 30),
    [P_SIZE_RAND] = C("size_rand", "SIZE RND", 0, 1, 0.3f, 0.01f, 1, 31),
    [P_WINDOW] = E("window", "ENVELOPE", 5, 0, WIN_NAME, 1, 0),
    [P_GRAIN_TONE] = C("grain_tone", "TONE", -1, 1, -0.1f, 0.02f, 1, 28),
    [P_FILTER_TYPE] = E("filter_type", "FILTER", 2, 0, FILTER_NAME, 0, 0),
    [P_CUTOFF] = C("cutoff", "CUTOFF", 0, 1, 0.82f, 0.01f, 1, 74),
    [P_RESO] = C("reso", "RESO", 0, 1, 0.10f, 0.01f, 1, 71),
    [P_DRIVE] = C("drive", "DRIVE", 0, 1, 0.05f, 0.01f, 1, 29),
    [P_DLY_TIME] = C("dly_time", "TIME", 20, 1000, 520, 10, 1, 85),
    [P_DLY_FB] = C("dly_fb", "FEEDBACK", 0, 0.92f, 0.45f, 0.01f, 1, 86),
    [P_DLY_MIX] = C("dly_mix", "ECHO", 0, 1, 0.18f, 0.01f, 1, 87),
    [P_DLY_PINGPONG] = B("dly_pingpong", "PINGPONG", 1, 0),
    [P_REV_SIZE] = C("rev_size", "SIZE", 0, 1, 0.78f, 0.01f, 1, 92),
    [P_REV_DAMP] = C("rev_damp", "DAMP", 0, 1, 0.50f, 0.01f, 1, 94),
    [P_SHIMMER] = C("shimmer", "SHIMMER", 0, 1, 0.22f, 0.01f, 1, 93),
    [P_TAPE_WOW] = C("tape_wow", "WOW", 0, 1, 0.4f, 0.01f, 1, 0),
    [P_TAPE_FLUTTER] = C("tape_flutter", "FLUTTER", 0, 1, 0.25f, 0.01f, 1, 0),
    [P_TAPE_AGE] = C("tape_age", "AGE", 0, 1, 0.25f, 0.01f, 1, 0),
    [P_TAPE_NOISE] = C("tape_noise", "HISS", 0, 1, 0, 0.01f, 0, 0),
    [P_OR_ON] = B("oracle_on", "ORACLE", 0, 0),
    [P_OR_ROOT] = E("oracle_root", "ROOT", 11, 0, NOTE_NAME, 0, 0),
    [P_OR_SCALE] = E("oracle_scale", "SCALE", 11, 1, SCALE_NAME, 0, 0),
    [P_OR_ACTIVITY] = C("oracle_activity", "ACTIVITY", 0, 1, 0.35f, 0.01f, 1, 0),
    [P_OR_AMBIG] = C("oracle_ambig", "AMBIGUITY", 0, 1, 0.2f, 0.01f, 1, 0),
    [P_OR_RANGE] = {"oracle_range", "RANGE", PK_INT, 0, 0, 0, 2, 1, 1, 0},
    [P_OR_GLIDE] = C("oracle_glide", "GLIDE", 0, 1, 0.4f, 0.01f, 1, 0),
    [P_OR_DRONE] = B("oracle_drone", "DRONE", 0, 0),
    [P_OR_HOLD] = B("oracle_hold", "HOLD", 0, 0),
    [P_ATTACK] = C("attack", "ATTACK", 0, 1, 0.15f, 0.01f, 0, 73),
    [P_RELEASE] = C("release", "RELEASE", 0, 1, 0.45f, 0.01f, 0, 72),
    [P_KEYMODE] = E("keymode", "KEYS", 1, 0, KEYMODE_NAME, 0, 0),
    [P_VELO] = C("velo", "VELOCITY", 0, 1, 0.5f, 0.01f, 0, 0),
    [P_START] = C("start", "START", 0, 1, 0, 0.002f, 0, 0),
    [P_END] = C("end", "END", 0, 1, 1, 0.002f, 0, 0),
    [P_MODE] = E("mode", "MODE", MODE_COUNT - 1, 0, MODE_NAME, 0, 0),
};

static float pclamp(int id, float v)
{
    const pinfo_t *p = &PI[id];
    if (!(v == v))
        v = p->def;
    v = v < p->min ? p->min : v > p->max ? p->max : v;
    if (p->kind != PK_CONT)
        v = (float)(int32_t)(v + (v >= 0.0f ? 0.5f : -0.5f));
    return v;
}

/* a parameter as 16 bits (presets, the session, the companion app): enum / int / bool as themselves (offset by
 * min), a continuous one over its range */
static uint16_t pq(int id, float v)
{
    const pinfo_t *p = &PI[id];
    v = pclamp(id, v);
    if (p->kind != PK_CONT)
        return (uint16_t)(int32_t)(v - p->min);
    return (uint16_t)((v - p->min) / (p->max - p->min) * 65535.0f + 0.5f);
}
static float pdq(int id, uint16_t q)
{
    const pinfo_t *p = &PI[id];
    if (p->kind != PK_CONT)
        return pclamp(id, p->min + (float)q);
    return pclamp(id, p->min + (p->max - p->min) * (float)q * (1.0f / 65535.0f));
}
/* 0..1 of its range (the dials) */
static float pnorm(int id, float v)
{
    const pinfo_t *p = &PI[id];
    return (pclamp(id, v) - p->min) / (p->max - p->min);
}

static void int_text(int32_t v, char *b)
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
static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}
static void text_cpy(char *d, const char *s, uint32_t max)
{
    uint32_t i;
    for (i = 0; i + 1u < max && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}
/* v with one decimal: "12.5" */
static void dec1_text(float v, char *b)
{
    int32_t t = (int32_t)(v * 10.0f + (v >= 0.0f ? 0.5f : -0.5f));
    char f[4];
    if (t < 0) {
        *b++ = '-';
        t = -t;
    }
    int_text(t / 10, b);
    f[0] = '.';
    f[1] = (char)('0' + t % 10);
    f[2] = 0;
    text_cat(b, f);
}

/* how a value reads on the screen */
static void pformat(int id, float v, char *out)
{
    const pinfo_t *p = &PI[id];
    out[0] = 0;
    v = pclamp(id, v);
    if (p->names) {
        int32_t i = (int32_t)v;
        text_cpy(out, p->names[i < 0 ? 0 : i > (int32_t)p->max ? (int32_t)p->max : i], 12);
        return;
    }
    switch (id) {
    case P_PITCH:
        if (v > 0.0f)
            text_cat(out, "+");
        int_text((int32_t)v, out + (v > 0.0f));
        text_cat(out, " ST");
        break;
    case P_PITCH_RAND:
        dec1_text(v, out);
        text_cat(out, " ST");
        break;
    case P_SIZE_MUL:
    case P_DENS_MUL: {
        float m = fm_exp2f(v);
        out[0] = 'X';
        dec1_text(m, out + 1);
        if (m < 0.95f) {                 /* x0.3 reads better with two decimals */
            int32_t h = (int32_t)(m * 100.0f + 0.5f);
            out[1] = '0';
            out[2] = '.';
            out[3] = (char)('0' + h / 10);
            out[4] = (char)('0' + h % 10);
            out[5] = 0;
        }
        break;
    }
    case P_SCAN:
        if (v > 0.0f)
            text_cat(out, "+");
        dec1_text(v, out + (v > 0.0f));
        text_cat(out, "X");
        break;
    case P_GRAIN_TONE:
        if (v < -0.02f) {
            text_cat(out, "DARK ");
            int_text((int32_t)(-v * 100.0f + 0.5f), out + 5);
        } else if (v > 0.02f) {
            text_cat(out, "AIRY ");
            int_text((int32_t)(v * 100.0f + 0.5f), out + 5);
        } else
            text_cat(out, "PURE");
        break;
    case P_CUTOFF: {
        float hz = 30.0f * fm_powf(600.0f, v);
        if (hz >= 1000.0f) {
            dec1_text(hz / 1000.0f, out);
            text_cat(out, "K");
        } else {
            int_text((int32_t)hz, out);
            text_cat(out, "HZ");
        }
        break;
    }
    case P_DLY_TIME:
        int_text((int32_t)v, out);
        text_cat(out, "MS");
        break;
    case P_OR_RANGE:
        int_text((int32_t)v, out);
        text_cat(out, " OCT");
        break;
    case P_ATTACK:
    case P_RELEASE: {
        float s = (id == P_ATTACK ? 0.005f : 0.01f) * fm_powf(800.0f, v);
        if (s < 1.0f) {
            int_text((int32_t)(s * 1000.0f), out);
            text_cat(out, "MS");
        } else {
            dec1_text(s, out);
            text_cat(out, "S");
        }
        break;
    }
    case P_START:
    case P_END:
    case P_POSITION:
        dec1_text(v * 100.0f, out);
        text_cat(out, "%");
        break;
    default:
        int_text((int32_t)(v * 100.0f + 0.5f), out);
        text_cat(out, "%");
        break;
    }
}

/* the parameters' defaults into an array */
static void pdefaults(float *v)
{
    int i;
    for (i = 0; i < P_COUNT; i++)
        v[i] = PI[i].def;
}

/* the knob pages: four parameters each (-1: an empty cell), a name, and the page group (the button) */
enum { G_HOME, G_KEYS, G_GRAIN, G_FX, G_ORACLE, G_SAMPLE, G_SYSTEM, G_PRESETS, G_COUNT };
#define NPAGES 13
typedef struct {
    const char *name;
    uint8_t group;
    int8_t p[4];
} page_t;
#define SYS_THEME (-10)                  /* the settings' cells (not engine parameters) */
#define SYS_MIDI (-11)
#define SYS_VOICES (-12)
#define SYS_USB (-13)
static const page_t PAGES[NPAGES] = {
    {"PLAY", G_HOME, {P_TEXTURE, P_SCATTER, P_DRIFT, P_MOTION}},
    {"COLOUR", G_HOME, {P_PITCH, P_HARMONY, P_SPACE, P_TAPE}},
    {"KEYS", G_KEYS, {P_ATTACK, P_RELEASE, P_KEYMODE, P_VELO}},
    {"GRAIN", G_GRAIN, {P_SIZE_MUL, P_DENS_MUL, P_WINDOW, P_GRAIN_TONE}},
    {"MOTION", G_GRAIN, {P_SCAN, P_REVERSE, P_PITCH_RAND, P_SIZE_RAND}},
    {"FILTER", G_FX, {P_FILTER_TYPE, P_CUTOFF, P_RESO, P_DRIVE}},
    {"DELAY", G_FX, {P_DLY_TIME, P_DLY_FB, P_DLY_MIX, P_DLY_PINGPONG}},
    {"REVERB", G_FX, {P_SPACE, P_REV_SIZE, P_REV_DAMP, P_SHIMMER}},
    {"TAPE", G_FX, {P_TAPE_WOW, P_TAPE_FLUTTER, P_TAPE_AGE, P_TAPE_NOISE}},
    {"MIX", G_FX, {P_WIDTH, P_LEVEL, -1, -1}},
    {"ORACLE", G_ORACLE, {P_OR_ON, P_OR_ROOT, P_OR_SCALE, P_OR_ACTIVITY}},
    {"ORACLE 2", G_ORACLE, {P_OR_AMBIG, P_OR_RANGE, P_OR_GLIDE, P_OR_DRONE}},
    {"SAMPLE", G_SAMPLE, {-2, P_START, P_END, P_POSITION}},   /* -2: the slot */
};
