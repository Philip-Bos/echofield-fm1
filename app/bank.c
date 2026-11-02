/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Philip Bos */
/* ECHOFIELD FM-1: the sample bank. 92 sectors of 4 KiB in the flash windows fm1-base leaves to an app (plat.h): every
 * sector holds one 3828-byte payload of EFB1 (ef_dsp.h) and a trailer; a directory (a store object, torn-write safe)
 * names up to 16 samples and which sectors each has, in order. Any sector may belong to any sample, so samples are
 * replaced and deleted with no compaction and nothing ever moves.
 *
 * Sector layout:
 *   0x000 .. 0xEF3   payload (29 ADPCM blocks)
 *   0xEF4 .. 0xFDF   erased (0xFF): bytes 0xF00..0xF07 stay erased, so the update loader can never take the sector
 *                    for an update record (plat.h, bsp/loader/ldr_core.c)
 *   0xFE0 .. 0xFEF   trailer: "EFS1", slot, index in the sample, 0, 0, CRC-32 of the payload, the directory's gen
 *   0xFF0 .. 0xFFF   erased
 * Checks: at boot every sample's sectors are checked (trailer and CRC) and the whole payload against the directory's
 * CRC; a sample that fails is DAMAGED and never handed to the engine. A directory that fails its own checks is read
 * as empty (the samples' sectors are then free: nothing is erased until something new is written there).
 * Writes: a sample is written into free sectors first, each one read back, and only then put in the directory; the
 * sample it replaces leaves the directory before. Power lost at any point: the directory names the old sample, or
 * none, never a half-written one. Part of app.c's unity build. */

#define BANK_SLOTS 16u
#define BANK_NSEC 92u
#define BANK_MAGIC 0x31424645u           /* "EFB1" */
#define BANK_VERSION 1u
#define SEC_MAGIC 0x31534645u            /* "EFS1" */
#define SEC_TRAILER 0xFE0u
#define BANK_MAPB ((BANK_NSEC + 7u) / 8u)

typedef struct {
    char name[NAME_N];
    uint32_t frames;
    uint16_t rate;
    uint8_t used, factory;
    uint32_t crc;                        /* CRC-32 of the whole payload, sector after sector */
    uint8_t map[BANK_MAPB];              /* bit i: bank sector i is this sample's (in ascending order) */
} bank_slot_t;

typedef struct {
    uint32_t magic;
    uint16_t version, nslots;
    uint32_t gen;                        /* counts every change (the companion app's cache) */
    bank_slot_t slot[BANK_SLOTS];
} bank_dir_t;

typedef struct {
    uint32_t magic;
    uint8_t slot, index, rsv[2];
    uint32_t crc, gen;
} sec_trailer_t;

enum { SLOT_EMPTY, SLOT_OK, SLOT_DAMAGED };
enum { BANK_OK, BANK_E_ARGS = 1, BANK_E_XFER = 2, BANK_E_ROOM = 3, BANK_E_ERASE = 7, BANK_E_WRITE = 8, BANK_E_VERIFY = 9,
       BANK_E_BUSY = 10, BANK_E_CRC = 11, BANK_E_STORE = 12, BANK_E_FLASH = 13 };

_Static_assert(sizeof(bank_dir_t) <= PLAT_STORE_MAX, "the bank's directory fits one store object");
_Static_assert(EF_SEC_PAYLOAD <= 0xEF4u, "the payload stays below the update loader's record bytes");

static bank_dir_t dir;
static uint8_t slot_state[BANK_SLOTS];

static uint32_t sec_addr(uint32_t i)     /* bank sector i's flash offset: XDATA0, the data area, XDATA1 */
{
    if (i < 4u)
        return PLAT_XDATA0_LO + i * 0x1000u;
    if (i < 69u)
        return PLAT_DATA_LO + (i - 4u) * 0x1000u;
    return PLAT_XDATA1_LO + (i - 69u) * 0x1000u;
}
_Static_assert(PLAT_XDATA0_HI - PLAT_XDATA0_LO == 4u * 0x1000u, "4 sectors in XDATA0");
_Static_assert(PLAT_XDATA1_HI - PLAT_XDATA1_LO == 23u * 0x1000u, "23 sectors in XDATA1");
_Static_assert(PLAT_DATA_LO + 65u * 0x1000u <= 0xD8000u, "the bank ends below the store objects");

static int map_bit(const uint8_t *m, uint32_t i) { return (m[i >> 3] >> (i & 7u)) & 1; }
static uint32_t map_count(const uint8_t *m)
{
    uint32_t i, n = 0;
    for (i = 0; i < BANK_NSEC; i++)
        n += (uint32_t)map_bit(m, i);
    return n;
}
static uint32_t nsec_for(uint32_t frames) { return (frames + EF_SEC_SAMPLES - 1u) / EF_SEC_SAMPLES; }

/* ---- CRC-32 (zlib's), a byte a step: the boot check reads the whole bank */
static uint32_t crc_t[256];
static void crc_init(void)
{
    uint32_t n, k, c;
    for (n = 0; n < 256u; n++) {
        for (c = n, k = 0; k < 8u; k++)
            c = c & 1u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_t[n] = c;
    }
}
static uint32_t crc_upd(uint32_t c, const uint8_t *p, uint32_t n)   /* c: a running CRC (0 to start) */
{
    c = ~c;
    while (n--)
        c = crc_t[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return ~c;
}

/* ---- the directory */
static void dir_empty(void)
{
    uint32_t i, k;
    dir.magic = BANK_MAGIC;
    dir.version = BANK_VERSION;
    dir.nslots = BANK_SLOTS;
    for (i = 0; i < BANK_SLOTS; i++) {
        dir.slot[i].used = 0;
        for (k = 0; k < BANK_MAPB; k++)
            dir.slot[i].map[k] = 0;
    }
}

static int dir_save(void)
{
    dir.gen++;
    return plat_store_save(OBJ_BANK, &dir, sizeof dir) ? BANK_E_STORE : BANK_OK;
}

/* a slot's sectors checked: trailers, each payload's CRC, the whole payload's CRC */
static int slot_verify(uint32_t s)
{
    const bank_slot_t *e = &dir.slot[s];
    uint32_t i, k = 0, c = 0;
    for (i = 0; i < BANK_NSEC; i++) {
        const uint8_t *p;
        sec_trailer_t t;
        uint32_t b;
        if (!map_bit(e->map, i))
            continue;
        p = plat_data(sec_addr(i));
        if (!p)
            return 0;
        for (b = 0; b < sizeof t; b++)
            ((uint8_t *)&t)[b] = p[SEC_TRAILER + b];
        if (t.magic != SEC_MAGIC || t.slot != s || t.index != k || crc_upd(0, p, EF_SEC_PAYLOAD) != t.crc)
            return 0;
        c = crc_upd(c, p, EF_SEC_PAYLOAD);
        k++;
    }
    return c == e->crc;
}

/* boot: the directory loaded and every entry checked; -> samples that play */
static uint32_t bank_load(void)
{
    uint8_t taken[BANK_MAPB] = {0};
    uint32_t i, k, ok = 0;
    crc_init();
    if (plat_store_load(OBJ_BANK, &dir, sizeof dir) != (int)sizeof dir || dir.magic != BANK_MAGIC ||
        dir.version != BANK_VERSION || dir.nslots != BANK_SLOTS)
        dir_empty();
    for (i = 0; i < BANK_SLOTS; i++) {
        bank_slot_t *e = &dir.slot[i];
        uint32_t n;
        int bad = 0;
        slot_state[i] = SLOT_EMPTY;
        if (e->used != 1u) {
            e->used = 0;
            continue;
        }
        name_clean(e->name, NAME_N);
        n = map_count(e->map);
        e->map[BANK_MAPB - 1u] &= (uint8_t)((1u << (BANK_NSEC & 7u)) - 1u);   /* (92: bits past the last sector) */
        if (e->frames < 512u || e->rate < 8000u || e->rate > 48000u || !n || n != nsec_for(e->frames) ||
            n > EF_MAX_SECS)
            bad = 1;
        for (k = 0; k < BANK_MAPB && !bad; k++)
            if (taken[k] & e->map[k])
                bad = 1;                 /* two samples on one sector: the later one is not trusted */
        if (!bad)
            for (k = 0; k < BANK_MAPB; k++)
                taken[k] |= e->map[k];
        slot_state[i] = !bad && slot_verify(i) ? SLOT_OK : SLOT_DAMAGED;
        ok += slot_state[i] == SLOT_OK;
    }
    return ok;
}

static uint32_t bank_free_secs(int except)   /* sectors no sample has (except: a slot counted as free too) */
{
    uint32_t i, s, n = 0;
    for (i = 0; i < BANK_NSEC; i++) {
        int used = 0;
        for (s = 0; s < BANK_SLOTS; s++)
            if (dir.slot[s].used && (int)s != except && map_bit(dir.slot[s].map, i))
                used = 1;
        n += !used;
    }
    return n;
}

static int bank_free_slot(void)
{
    uint32_t s;
    for (s = 0; s < BANK_SLOTS; s++)
        if (!dir.slot[s].used)
            return (int)s;
    return -1;
}

static int bank_find(const char *name)   /* the slot of a sample by name, -1 none */
{
    uint32_t s, i;
    if (!name[0])
        return -1;
    for (s = 0; s < BANK_SLOTS; s++) {
        if (slot_state[s] != SLOT_OK)
            continue;
        for (i = 0; i < NAME_N && name[i] == dir.slot[s].name[i] && name[i]; i++)
            ;
        if (i < NAME_N && name[i] == dir.slot[s].name[i])
            return (int)s;
    }
    return -1;
}

static void bank_source(uint32_t s, ef_source_t *src)   /* a playable slot as the engine's source */
{
    const bank_slot_t *e = &dir.slot[s];
    uint32_t i, k = 0;
    for (i = 0; i < BANK_NSEC && k < EF_MAX_SECS; i++)
        if (map_bit(e->map, i))
            src->sec[k++] = plat_data(sec_addr(i));
    src->nsec = k;
    src->frames = e->frames;
    src->rate = e->rate;
}

/* ---- writing: the sound off first (the engine reads the bank from the audio interrupt, and every erase or write
 * stops it anyway), then back as it was */
static void quiet_begin(void);
static void quiet_end(void);

/* one sector: erased, the payload and its trailer written, read back. pay: EF_SEC_PAYLOAD bytes in RAM */
static int sec_write(uint32_t i, const uint8_t *pay, uint32_t slot, uint32_t index)
{
    sec_trailer_t t;
    uint32_t a = sec_addr(i), b;
    const uint8_t *p;
    t.magic = SEC_MAGIC;
    t.slot = (uint8_t)slot;
    t.index = (uint8_t)index;
    t.rsv[0] = t.rsv[1] = 0;
    t.crc = crc_upd(0, pay, EF_SEC_PAYLOAD);
    t.gen = dir.gen + 1u;
    if (plat_data_erase(a))
        return BANK_E_ERASE;
    if (plat_data_write(a, pay, EF_SEC_PAYLOAD) || plat_data_write(a + SEC_TRAILER, &t, sizeof t))
        return BANK_E_WRITE;
    p = plat_data(a);
    if (!p || crc_upd(0, p, EF_SEC_PAYLOAD) != t.crc)
        return BANK_E_VERIFY;
    for (b = 0; b < 8u; b++)             /* the update loader's record bytes: still erased */
        if (p[0xF00u + b] != 0xFFu)
            return BANK_E_VERIFY;
    return BANK_OK;
}

typedef int (*bank_fill_fn)(uint32_t index, uint8_t *pay, void *ctx);   /* payload index -> pay; 0 = ok */
static uint8_t sec_buf[EF_SEC_PAYLOAD] PLAT_POOL;

/* a sample into slot s: frames, rate, name, its payloads from fill(); crc: the whole payload's expected CRC (0: not
 * checked, the device made it). progress(k, n) after each sector. -> BANK_OK or BANK_E_* */
static int bank_write(uint32_t s, uint32_t frames, uint32_t rate, const char *name, uint32_t crc, int factory,
                      bank_fill_fn fill, void *ctx, void (*progress)(uint32_t, uint32_t))
{
    bank_slot_t e;
    uint32_t n = nsec_for(frames), i, k = 0, c = 0;
    int rc = BANK_OK;
    if (s >= BANK_SLOTS || frames < 512u || rate < 8000u || rate > 48000u || n > EF_MAX_SECS)
        return BANK_E_ARGS;
    if (n > bank_free_secs((int)s))
        return BANK_E_ROOM;
    if (!plat_data(sec_addr(0)))
        return BANK_E_FLASH;
    quiet_begin();
    if (dir.slot[s].used) {              /* the old sample out of the directory first */
        dir.slot[s].used = 0;
        slot_state[s] = SLOT_EMPTY;
        if ((rc = dir_save()) != BANK_OK)
            goto out;
    }
    for (i = 0; i < BANK_MAPB; i++)
        e.map[i] = 0;
    for (i = 0; i < BANK_NSEC && k < n; i++) {
        uint32_t t;
        int busy = 0;
        for (t = 0; t < BANK_SLOTS; t++)
            if (dir.slot[t].used && map_bit(dir.slot[t].map, i))
                busy = 1;
        if (busy)
            continue;
        if (fill(k, sec_buf, ctx)) {
            rc = BANK_E_XFER;
            goto out;
        }
        if ((rc = sec_write(i, sec_buf, s, k)) != BANK_OK)
            goto out;
        c = crc_upd(c, sec_buf, EF_SEC_PAYLOAD);
        e.map[i >> 3] |= (uint8_t)(1u << (i & 7u));
        k++;
        if (progress)
            progress(k, n);
    }
    if (k != n) {
        rc = BANK_E_ROOM;
        goto out;
    }
    if (crc && c != crc) {
        rc = BANK_E_CRC;
        goto out;
    }
    text_cpy(e.name, name, NAME_N);
    name_clean(e.name, NAME_N);
    e.frames = frames;
    e.rate = (uint16_t)rate;
    e.used = 1;
    e.factory = (uint8_t)(factory != 0);
    e.crc = c;
    dir.slot[s] = e;
    if ((rc = dir_save()) != BANK_OK) {
        dir.slot[s].used = 0;
        goto out;
    }
    slot_state[s] = slot_verify(s) ? SLOT_OK : SLOT_DAMAGED;
    if (slot_state[s] != SLOT_OK)
        rc = BANK_E_VERIFY;
out:
    quiet_end();
    return rc;
}

static int bank_erase(uint32_t s)        /* out of the directory (its sectors free; erased when next written) */
{
    int rc;
    if (s >= BANK_SLOTS)
        return BANK_E_ARGS;
    if (!dir.slot[s].used)
        return BANK_OK;
    quiet_begin();
    dir.slot[s].used = 0;
    slot_state[s] = SLOT_EMPTY;
    rc = dir_save();
    quiet_end();
    return rc;
}

static int bank_rename(uint32_t s, const char *name)
{
    if (s >= BANK_SLOTS || !dir.slot[s].used)
        return BANK_E_ARGS;
    text_cpy(dir.slot[s].name, name, NAME_N);
    name_clean(dir.slot[s].name, NAME_N);
    return dir_save();
}

/* ---- the seed: a sound made at boot into RAM when the bank has nothing playable, so ECHOFIELD always plays. A
 * slow FM chord (Cmaj9 voicing, two operators a voice, a soft swell), 8 sectors at 22 050 Hz, in EFB1 */
#define SEED_NSEC 8u
static uint8_t seed_mem[SEED_NSEC * EF_SEC_PAYLOAD] PLAT_POOL;
static ef_source_t seed_src;

static void seed_make(void)
{
    static const float F[5] = {130.81f, 164.81f, 196.0f, 246.94f, 293.66f};
    const uint32_t n = SEED_NSEC * EF_SEC_SAMPLES;
    ef_ima_t st = {0, 0};
    float ph[5] = {0}, mph[5] = {0};
    uint32_t i, v;
    for (i = 0; i < n; i++) {
        float t = (float)i * (1.0f / 22050.0f), env = 1.0f - fm_expf(-t * 1.6f), x = 0.0f;
        uint8_t *blk = seed_mem + (i / EF_BLK_SAMPLES) * EF_BLK_BYTES;
        uint32_t j = i % EF_BLK_SAMPLES, nib;
        env *= 0.6f + 0.4f * fm_expf(-t * 0.35f);
        if (t > 2.45f)
            env *= fm_maxf(0.0f, (2.69f - t) / 0.24f);
        for (v = 0; v < 5u; v++) {
            float idx = 1.4f * fm_expf(-t * (0.6f + 0.15f * (float)v)) + 0.25f;
            mph[v] += F[v] * (2.0f + 0.002f * (float)v) / 22050.0f;
            ph[v] += F[v] * (1.0f + 0.0007f * (float)v) / 22050.0f;
            mph[v] -= fm_floorf(mph[v]);
            ph[v] -= fm_floorf(ph[v]);
            x += fm_sin_turns(ph[v] + idx * fm_sin_turns(mph[v]) * 0.159f) * (v == 0u ? 0.28f : 0.18f);
        }
        x *= env;
        if (!j) {
            blk[0] = (uint8_t)st.pred;
            blk[1] = (uint8_t)((uint32_t)st.pred >> 8);
            blk[2] = (uint8_t)st.index;
            blk[3] = 0;
        }
        nib = ef_ima_encode(&st, (int32_t)fm_clampf(x * 30000.0f, -32767.0f, 32767.0f));
        if (j & 1u)
            blk[4u + j / 2u] = (uint8_t)((blk[4u + j / 2u] & 15u) | nib << 4);
        else
            blk[4u + j / 2u] = (uint8_t)nib;
    }
    for (i = 0; i < SEED_NSEC; i++)
        seed_src.sec[i] = seed_mem + i * EF_SEC_PAYLOAD;
    seed_src.nsec = SEED_NSEC;
    seed_src.frames = n;
    seed_src.rate = 22050;
}

/* ---- RESAMPLE: the output recorded into RAM, then written as a new sample */
#define REC_NSEC 15u                     /* 5 s at 22 050 Hz */
static uint8_t rec_mem[REC_NSEC * EF_SEC_PAYLOAD] PLAT_POOL;
static ef_source_t rec_src;              /* a take that found no room: playable until the next one */

static int rec_fill(uint32_t index, uint8_t *pay, void *ctx)
{
    uint32_t i;
    const uint8_t *p = rec_mem + index * EF_SEC_PAYLOAD;
    (void)ctx;
    for (i = 0; i < EF_SEC_PAYLOAD; i++)
        pay[i] = p[i];
    return 0;
}
