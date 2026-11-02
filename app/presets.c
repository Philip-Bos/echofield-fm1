/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Philip Bos */
/* ECHOFIELD FM-1: settings, the session (the sound as it was left), the user presets, the factory presets and the
 * constrained RANDOM. Every record carries a magic, a version and its parameter count; whatever fails a check is
 * replaced by defaults, never trusted. Parameter ids only ever grow at the end (ef_dsp.h), so an older record loads
 * into a newer firmware with the new parameters at their defaults. Part of app.c's unity build. */

#define SET_MAGIC 0x31544645u            /* "EFT1" */
#define OUT_DEFAULT 8u                   /* OUTPUT at 80 %: full level over USB was too hot for a computer's input */
#define SES_MAGIC 0x31534553u            /* "SES1" */
#define PRE_MAGIC 0x31455250u            /* "PRE1" */
#define NUSER 16u
#define NAME_N 12u

enum { THEME_COLOUR, THEME_MONO, THEME_AMBER, NTHEMES };
static const char *const THEME_NAME[NTHEMES] = {"COLOUR", "MONO", "AMBER"};

typedef struct {
    uint32_t magic;
    uint8_t theme;                       /* THEME_* */
    uint8_t midi_ch;                     /* 0 omni, 1..16 */
    uint8_t voices;                      /* the grain cap: 8 .. 32 */
    uint8_t out_level;                   /* OUTPUT, the most the FM-1 puts out (USB, headphones, speaker): 5..10 = 50..100 %,
                                          * MASTER below it. (rev 0: usb_full, 0 / 1; rev 1: USB only, 0 = at MASTER) */
    uint8_t cc_out;                      /* knob moves out as MIDI CC */
    uint8_t rev;                         /* 2: out_level (an older FM-1's settings have 0 or 1 here) */
    uint8_t rsv[2];
} settings_t;

typedef struct {
    uint32_t magic;
    uint16_t version, n;                 /* n: parameters in q */
    uint16_t q[P_COUNT];
    char slot_name[NAME_N];              /* the sample: by name first, the slot index if the name is gone */
    int8_t slot;
    uint8_t play;
    uint8_t locks[8];                    /* RANDOM leaves these alone: bit per parameter */
    uint8_t octave;                      /* + 2: 0..4 */
    uint8_t rsv;
} session_t;

typedef struct {
    char name[NAME_N];
    char slot_name[NAME_N];
    uint8_t used;
    int8_t slot;
    uint16_t q[P_COUNT];
} upreset_t;

typedef struct {
    uint32_t magic;
    uint16_t version, n;
    upreset_t p[NUSER];
} presets_t;

_Static_assert(sizeof(presets_t) <= PLAT_STORE_MAX, "user presets fit one store object");
_Static_assert(sizeof(session_t) <= PLAT_STORE_MAX, "the session fits one store object");

static settings_t set;
static presets_t pre;
static uint8_t locks[8];

static void settings_defaults(void)
{
    set.magic = SET_MAGIC;
    set.theme = THEME_COLOUR;
    set.midi_ch = 0;
    set.voices = 20;
    set.out_level = OUT_DEFAULT;
    set.cc_out = 0;
    set.rev = 2;
}

static void settings_load(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC ||
        set.theme >= NTHEMES || set.midi_ch > 16u || set.voices < 8u || set.voices > EF_MAX_GRAINS || set.cc_out > 1u)
        settings_defaults();
    if (set.rev < 1u)                    /* 0.2.2 and before: FULL (1) stays at 100 %, MASTER (0, the default) 80 % */
        set.out_level = set.out_level == 1u ? 10u : OUT_DEFAULT;
    else if (set.rev == 1u && set.out_level == 0u)   /* 0.2.3-0.2.4's USB "MASTER": at full, MASTER below it */
        set.out_level = 10u;
    set.rev = 2;
    if (set.out_level < 5u || set.out_level > 10u)
        set.out_level = OUT_DEFAULT;
}

static int locked(int id) { return (locks[id >> 3] >> (id & 7)) & 1; }
static void lock_toggle(int id) { locks[id >> 3] ^= (uint8_t)(1u << (id & 7)); }

/* ---------------------------------------------------------------- factory presets (the R36S's) */
typedef struct {
    uint8_t id;
    float v;
} pv_t;
typedef struct {
    const char *name, *sample;
    pv_t v[16];
} factory_t;
#define END_PV {255, 0}
static const factory_t FACTORY[] = {
    {"FELT CLOUDS", "FELT PIANO", {{P_MODE, 0}, {P_TEXTURE, 0.78f}, {P_SCATTER, 0.50f}, {P_DRIFT, 0.35f}, {P_MOTION, 0.30f},
                                   {P_SPACE, 0.72f}, {P_HARMONY, 0.35f}, {P_SHIMMER, 0.25f}, {P_DLY_MIX, 0.20f},
                                   {P_CUTOFF, 0.80f}, {P_WIDTH, 0.90f}, END_PV}},
    {"GLASS CHOIR", "BOWED GLASS", {{P_MODE, 3}, {P_TEXTURE, 0.70f}, {P_SCATTER, 0.40f}, {P_DRIFT, 0.30f}, {P_MOTION, 0.35f},
                                    {P_SPACE, 0.80f}, {P_HARMONY, 0.50f}, {P_SHIMMER, 0.45f}, {P_REV_SIZE, 0.90f},
                                    {P_WIDTH, 1.00f}, {P_CUTOFF, 0.78f}, END_PV}},
    {"DUST LIGHTS", "MUSIC BOX", {{P_MODE, 1}, {P_TEXTURE, 0.45f}, {P_SCATTER, 0.70f}, {P_DRIFT, 0.30f}, {P_MOTION, 0.40f},
                                  {P_SPACE, 0.80f}, {P_HARMONY, 0.30f}, {P_SHIMMER, 0.30f}, {P_DLY_MIX, 0.32f},
                                  {P_DLY_TIME, 640}, {P_WIDTH, 1.00f}, END_PV}},
    {"WARM TIDE", "WARM PAD", {{P_MODE, 5}, {P_TEXTURE, 0.70f}, {P_SCATTER, 0.45f}, {P_DRIFT, 0.55f}, {P_MOTION, 0.40f},
                               {P_SPACE, 0.70f}, {P_TAPE, 0.25f}, {P_CUTOFF, 0.70f}, {P_HARMONY, 0.25f}, END_PV}},
    {"SLOW TAPES", "STRINGS", {{P_MODE, 2}, {P_TEXTURE, 0.60f}, {P_SCATTER, 0.25f}, {P_DRIFT, 0.40f}, {P_MOTION, 0.35f},
                               {P_SPACE, 0.62f}, {P_TAPE, 0.50f}, {P_HARMONY, 0.15f}, {P_TAPE_WOW, 0.6f}, {P_CUTOFF, 0.75f},
                               END_PV}},
    {"DEEP WATER", "SUB BLOOM", {{P_MODE, 0}, {P_TEXTURE, 0.85f}, {P_SCATTER, 0.40f}, {P_DRIFT, 0.40f}, {P_MOTION, 0.25f},
                                 {P_SPACE, 0.80f}, {P_TAPE, 0.30f}, {P_CUTOFF, 0.55f}, {P_HARMONY, 0.40f}, {P_SHIMMER, 0.15f},
                                 END_PV}},
    {"RAIN CATHEDRAL", "RAIN FIELD", {{P_MODE, 0}, {P_TEXTURE, 0.88f}, {P_SCATTER, 0.60f}, {P_DRIFT, 0.40f}, {P_MOTION, 0.30f},
                                      {P_SPACE, 0.90f}, {P_HARMONY, 0.45f}, {P_SHIMMER, 0.50f}, {P_REV_SIZE, 0.95f},
                                      {P_CUTOFF, 0.72f}, END_PV}},
    {"KALIMBA STEPS", "KALIMBA", {{P_MODE, 4}, {P_TEXTURE, 0.40f}, {P_SCATTER, 0.60f}, {P_DRIFT, 0.30f}, {P_MOTION, 0.30f},
                                  {P_SPACE, 0.62f}, {P_DLY_MIX, 0.35f}, {P_DLY_TIME, 750}, {P_DLY_FB, 0.55f},
                                  {P_HARMONY, 0.20f}, END_PV}},
    {"HYMN", "VOX CHOIR", {{P_MODE, 0}, {P_TEXTURE, 0.85f}, {P_SCATTER, 0.45f}, {P_DRIFT, 0.35f}, {P_MOTION, 0.30f},
                           {P_SPACE, 0.78f}, {P_HARMONY, 0.40f}, {P_SHIMMER, 0.32f}, {P_WIDTH, 0.95f}, END_PV}},
    {"BREATH CHOIR", "BREATH", {{P_MODE, 3}, {P_TEXTURE, 0.75f}, {P_SCATTER, 0.50f}, {P_DRIFT, 0.40f}, {P_MOTION, 0.40f},
                                {P_SPACE, 0.75f}, {P_HARMONY, 0.35f}, {P_SHIMMER, 0.25f}, END_PV}},
    {"NIGHT CURRENT", "LOW DRONE", {{P_MODE, 5}, {P_TEXTURE, 0.65f}, {P_SCATTER, 0.40f}, {P_DRIFT, 0.60f}, {P_MOTION, 0.40f},
                                    {P_PITCH, -12}, {P_SPACE, 0.60f}, {P_TAPE, 0.30f}, {P_WIDTH, 0.70f}, {P_OR_ON, 1},
                                    {P_OR_SCALE, 1}, {P_OR_ACTIVITY, 0.30f}, {P_OR_DRONE, 1}, {P_OR_GLIDE, 0.7f}, END_PV}},
    {"GLASS RAIN", "GLASS TINES", {{P_MODE, 1}, {P_TEXTURE, 0.35f}, {P_SCATTER, 0.55f}, {P_DRIFT, 0.20f}, {P_MOTION, 0.50f},
                                   {P_PITCH, 12}, {P_SPACE, 0.70f}, {P_WIDTH, 0.95f}, {P_REVERSE, 0.20f}, {P_SHIMMER, 0.35f},
                                   {P_DLY_MIX, 0.25f}, {P_DLY_FB, 0.45f}, {P_DLY_TIME, 280}, {P_GRAIN_TONE, 0.40f}, END_PV}},
    {"BROKEN STARS", "WIRE PLUCKS", {{P_MODE, 3}, {P_TEXTURE, 0.60f}, {P_SCATTER, 0.70f}, {P_DRIFT, 0.50f}, {P_MOTION, 0.50f},
                                     {P_SPACE, 0.75f}, {P_TAPE, 0.10f}, {P_WIDTH, 1.00f}, {P_SHIMMER, 0.50f}, {P_OR_ON, 1},
                                     {P_OR_SCALE, 4}, {P_OR_ACTIVITY, 0.40f}, {P_OR_AMBIG, 0.35f}, END_PV}},
    {"FAR BROADCAST", "DUST RHYTHM", {{P_MODE, 2}, {P_TEXTURE, 0.50f}, {P_SCATTER, 0.20f}, {P_DRIFT, 0.40f}, {P_MOTION, 0.40f},
                                      {P_SPACE, 0.55f}, {P_TAPE, 0.80f}, {P_WIDTH, 0.60f}, {P_CUTOFF, 0.70f}, {P_RESO, 0.30f},
                                      {P_TAPE_AGE, 0.70f}, {P_TAPE_NOISE, 0.35f}, {P_TAPE_WOW, 0.70f}, END_PV}},
    {"INIT", "", {END_PV}},
};
#define NFACTORY ((int)(sizeof FACTORY / sizeof FACTORY[0]))

/* every parameter of a factory preset (the rest at their defaults; START, END and POSITION kept: they belong to the
 * sample more than to the sound) */
static void factory_values(int i, const float *now, float *v)
{
    const pv_t *p;
    pdefaults(v);
    v[P_START] = now[P_START];
    v[P_END] = now[P_END];
    for (p = FACTORY[i].v; p->id != 255u; p++)
        v[p->id] = p->v;
}

/* ---------------------------------------------------------------- user presets */
static void presets_defaults(void)
{
    uint32_t i;
    pre.magic = PRE_MAGIC;
    pre.version = 1;
    pre.n = P_COUNT;
    for (i = 0; i < NUSER; i++)
        pre.p[i].used = 0;
}

static void name_clean(char *s, uint32_t n)   /* zero-terminated, only what the font draws */
{
    uint32_t i;
    s[n - 1u] = 0;
    for (i = 0; i < n && s[i]; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == ' ' || s[i] == '-' || s[i] == '.' ||
              s[i] == '_'))
            s[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : '?';
    for (; i < n; i++)
        s[i] = 0;
}

static void presets_load(void)
{
    uint32_t i, k;
    if (plat_store_load(OBJ_PRESETS, &pre, sizeof pre) < (int)(sizeof pre - sizeof pre.p) || pre.magic != PRE_MAGIC ||
        pre.version != 1u || pre.n == 0 || pre.n > P_COUNT) {
        presets_defaults();
        return;
    }
    for (i = 0; i < NUSER; i++) {
        upreset_t *u = &pre.p[i];
        if (u->used > 1u)
            u->used = 0;
        name_clean(u->name, NAME_N);
        name_clean(u->slot_name, NAME_N);
        for (k = pre.n; k < P_COUNT; k++)   /* parameters newer than the record: their defaults */
            u->q[k] = pq((int)k, PI[k].def);
    }
    pre.n = P_COUNT;
}

/* ---------------------------------------------------------------- RANDOM */
static uint32_t rnd_s = 0xC0FFEE11u;
static float rnd01(void)
{
    rnd_s ^= rnd_s << 13;
    rnd_s ^= rnd_s >> 17;
    rnd_s ^= rnd_s << 5;
    return (float)(rnd_s >> 8) * (1.0f / 16777216.0f);
}
static float rr(float a, float b) { return a + (b - a) * rnd01(); }
static float skew_low(float a, float b)
{
    float u = rnd01();
    return a + (b - a) * u * u;
}
static float tri(float mean, float width, float lo, float hi)
{
    float v = mean + (rnd01() - rnd01()) * width;
    return v < lo ? lo : v > hi ? hi : v;
}

/* a new sound within what sounds good: the R36S's constraints. only: the parameters it may touch (bit per id), 0 all
 * the randomisable ones; locked ones are always left */
static void randomize(float *p, const uint8_t *only)
{
    static const float PITCHES[15] = {0, 0, 0, 0, -12, -12, 12, 12, -5, 7, 7, -7, 5, 19, -24};
    float v[P_COUNT];
    int i;
    for (i = 0; i < P_COUNT; i++)
        v[i] = p[i];
    v[P_POSITION] = rr(0.05f, 0.95f);
    v[P_TEXTURE] = tri(0.65f, 0.4f, 0.15f, 1.0f);
    v[P_SCATTER] = tri(0.35f, 0.4f, 0.03f, 0.85f);
    v[P_DRIFT] = skew_low(0.0f, 0.75f);
    v[P_MOTION] = skew_low(0.0f, 0.85f);
    v[P_PITCH] = PITCHES[(uint32_t)(rnd01() * 15.0f) % 15u];
    v[P_SPACE] = rr(0.45f, 0.9f);
    v[P_TAPE] = skew_low(0.0f, 0.75f);
    v[P_WIDTH] = rr(0.6f, 1.0f);
    v[P_HARMONY] = skew_low(0.0f, 0.7f);
    v[P_SIZE_MUL] = tri(0.0f, 1.0f, -1.5f, 1.5f);
    v[P_DENS_MUL] = tri(0.0f, 1.0f, -1.5f, 1.5f);
    v[P_SCAN] = rnd01() < 0.7f ? 0.0f : tri(0.0f, 0.35f, -0.5f, 0.5f);
    v[P_REVERSE] = skew_low(0.0f, 0.45f);
    v[P_PITCH_RAND] = rnd01() < 0.7f ? 0.0f : skew_low(0.1f, 3.0f);
    v[P_SIZE_RAND] = rr(0.0f, 0.6f);
    v[P_WINDOW] = rnd01() < 0.6f ? 0.0f : (float)(1u + (uint32_t)(rnd01() * 5.0f) % 5u);
    v[P_GRAIN_TONE] = tri(0.0f, 0.5f, -0.7f, 0.7f);
    v[P_CUTOFF] = 0.95f - skew_low(0.0f, 0.4f);
    v[P_RESO] = skew_low(0.0f, 0.6f);
    v[P_DRIVE] = skew_low(0.0f, 0.5f);
    v[P_DLY_TIME] = rr(120.0f, 900.0f);
    v[P_DLY_FB] = rr(0.1f, 0.6f);
    v[P_DLY_MIX] = skew_low(0.0f, 0.4f);
    v[P_REV_SIZE] = rr(0.3f, 1.0f);
    v[P_REV_DAMP] = rr(0.2f, 0.8f);
    v[P_SHIMMER] = rnd01() < 0.4f ? 0.0f : rr(0.1f, 0.55f);
    v[P_TAPE_WOW] = rr(0.1f, 0.9f);
    v[P_TAPE_FLUTTER] = rr(0.0f, 0.7f);
    v[P_TAPE_AGE] = rr(0.0f, 0.8f);
    v[P_OR_ACTIVITY] = rr(0.1f, 0.7f);
    v[P_OR_AMBIG] = skew_low(0.0f, 0.6f);
    v[P_OR_GLIDE] = rr(0.1f, 0.8f);
    for (i = 0; i < P_COUNT; i++)
        if (PI[i].rnd && !locked(i) && (!only || ((only[i >> 3] >> (i & 7)) & 1u)))
            p[i] = pclamp(i, v[i]);
    /* musical constraints, on the result: never near-silence or mud */
    if ((int)p[P_FILTER_TYPE] == 0 && p[P_CUTOFF] < 0.45f && !locked(P_CUTOFF))
        p[P_CUTOFF] = 0.45f;
    if (p[P_SIZE_MUL] + p[P_DENS_MUL] < -1.8f && !locked(P_DENS_MUL))
        p[P_DENS_MUL] = pclamp(P_DENS_MUL, -1.8f - p[P_SIZE_MUL]);
    if (p[P_DLY_FB] > 0.5f && p[P_DLY_MIX] > 0.3f && !locked(P_DLY_FB))
        p[P_DLY_FB] = 0.5f;
}

static void random_name(char *out)       /* a user preset's name when nobody gave it one */
{
    static const char *const A[12] = {"AMBER", "HOLLOW", "QUIET", "VELVET", "PAPER", "SALT", "SILVER", "ASH", "LUNAR",
                                      "OPAL", "COLD", "SLOW"};
    static const char *const N[12] = {"TIDE", "CHOIR", "WIRE", "FIELD", "SHORE", "RELAY", "COMET", "LANTERN", "GARDEN",
                                      "ARCHIVE", "SIGNAL", "HARBOR"};
    const char *a = A[(uint32_t)(rnd01() * 12.0f) % 12u], *n = N[(uint32_t)(rnd01() * 12.0f) % 12u];
    uint32_t l = 0;
    text_cpy(out, a, NAME_N);
    while (out[l])
        l++;
    if (l + 2u < NAME_N) {               /* "AMBER TIDE", cut to the name's room */
        out[l] = ' ';
        text_cpy(out + l + 1u, n, NAME_N - l - 1u);
    }
}
