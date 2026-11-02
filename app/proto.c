/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Philip Bos */
/* ECHOFIELD FM-1: the companion app's commands over USB, in the update's SysEx framing (felucca-bsp ota.c:
 * F0 pack7(00 59 cmd len24 body ~sum8(body)) F7), cmd 0x70..0x7F (fm1-base takes 0x11, 0x30, 0x41..0x44, 0x50..0x54).
 * The companion app's side: companion/lib/efproto.js. Every command is answered with a frame of the same cmd whose
 * body starts with a status byte (0 ok, else BANK_E_*), except UPLOAD, which ends the way the resource transfer does:
 * the FM-1 reads 0xC0000000 (done) or 0xCF000000 | code (failed) and the host acknowledges.
 *
 *   70 HELLO        -> magic "EFM1", protocol, firmware version (16), slots, bank sectors, free sectors, samples a
 *                      sector, payload bytes a sector, directory gen, parameter count, current slot, play, user
 *                      presets, factory presets
 *   71 DIR          -> per slot: state (0 empty, 1 ok, 2 damaged), factory, name (12), frames u32, rate u16
 *   72 UPLOAD       slot, factory, rate u16, frames u32, crc u32, name (12): the FM-1 then pulls the payload with read
 *                   requests (address = byte offset in the payload, 512 at most), writes it, reports as above
 *   73 ERASE        slot
 *   74 RENAME       slot, name (12)
 *   75 PARAMS       -> parameter count, then u16 each (params.c pq), slot, play, freeze, locks (8)
 *   76 SET          (id u8, value u16)...: parameters; 0xF0 play, 0xF1 freeze, 0xF2 select slot n, 0xF3 RANDOM (0 this
 *                   page's on the panel, 1 everything), 0xF4 undo RANDOM, 0xF7 the side limiter's ceiling (1/1000 of
 *                   full scale, 1000 off), 0xF6 LISTEN (0 stop, 1 once, 2 looped:
 *                   the playing sample as it is, START to END)
 *   77 PRESETS      -> user presets: used, name (12), sample name (12) each; then the factory presets' names
 *   78 PRESET OP    op, index, name (12): 0 load user, 1 store user (name may be empty), 2 delete user, 3 load factory,
 *                   4 rename user
 *   79 READ         slot, offset u32, length u16 (<= 512) -> the payload's bytes (an export, a backup)
 *   7A WIPE         "WIPE": every sample out of the directory
 *   7B PRESET GET   index -> used, slot, name (12), sample name (12), parameter count, u16 each (a backup)
 *   7D STATS        -> u32 each: render peak, grains peak, fx peak, spawn peak (us, since the last STATS),
 *                   CPU %, xruns since boot, grains now, grains shed, the cap, second core (0 off, 1 on, 2+ gave up),
 *                   frames rendered, the longest wait between renders (us), waits over 1.5 halves, plat_ms
 *   7C PRESET PUT   index, used, slot, name (12), sample name (12), parameter count, u16 each (a restore)
 *   7F (FM-1 -> host) a parameter changed on the panel: (id u8, value u16)..., sent while a host listens (it said
 *                   HELLO or PARAMS within 20 s)
 * Part of app.c's unity build. */

#define EF_PROTO 1u
enum { C_HELLO = 0x70, C_DIR, C_UPLOAD, C_ERASE, C_RENAME, C_PARAMS, C_SET, C_PRESETS, C_PRESET_OP, C_READ, C_WIPE,
       C_PRESET_GET, C_PRESET_PUT, C_STATS, C_NOTIFY = 0x7F };
#define UP_OK_ADDR 0xC0000000u
#define UP_FAIL_ADDR 0xCF000000u

static uint32_t host_until;              /* a host listens until then (ms) */
static uint8_t px_wire[1024];

static uint32_t pk7(const uint8_t *in, uint32_t n, uint8_t *out)   /* 8-to-7, LSB first (ota.c's) */
{
    uint32_t acc = 0, nb = 0, w = 0, i;
    for (i = 0; i < n; i++) {
        acc |= (uint32_t)in[i] << nb;
        nb += 8u;
        while (nb >= 7u) {
            out[w++] = (uint8_t)(acc & 0x7Fu);
            acc >>= 7;
            nb -= 7u;
        }
    }
    if (nb)
        out[w++] = (uint8_t)(acc & 0x7Fu);
    return w;
}

/* a reply: cmd and body (n <= 800) -> one SysEx frame out over USB */
static void reply(uint32_t cmd, const uint8_t *body, uint32_t n)
{
    static uint8_t msg[808];
    uint32_t i, s = 0, w;
    if (n > 800u)
        return;
    msg[0] = 0;
    msg[1] = 0x59;
    msg[2] = (uint8_t)cmd;
    msg[3] = (uint8_t)n;
    msg[4] = (uint8_t)(n >> 8);
    msg[5] = 0;
    for (i = 0; i < n; i++) {
        msg[6u + i] = body[i];
        s += body[i];
    }
    msg[6u + n] = (uint8_t)~s;
    px_wire[0] = 0xF0;
    w = 1u + pk7(msg, 7u + n, px_wire + 1);
    px_wire[w++] = 0xF7;
    plat_sysex_send(px_wire, w);
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

/* the app's side (app.c) */
static int8_t cur_slot;                  /* the sample playing: a slot, -1 the seed, -2 the unsaved take */
static float pv[P_COUNT];                /* the parameters as the panel sees them (the engine's ctl.p follows) */
static void param_set(int id, float v, int from_host);
static void select_slot(int s);
static void play_set(int on);
static void freeze_set(int on);
static int preset_op(uint32_t op, uint32_t idx, const char *name);
static void random_do(int all);
static int app_cpu2_state(void);
static void undo_do(void);
static void listen_cmd(int how);
static void upload_progress(uint32_t k, uint32_t n);
static void dir_changed(void);
static int store_quiet(uint32_t obj, const void *p, uint32_t n);

static void notify(int id, float v)     /* a panel change out to a listening host */
{
    uint8_t b[3];
    if (!host_until || (int32_t)(plat_ms() - host_until) > 0)
        return;
    b[0] = (uint8_t)id;
    put16(b + 1, pq(id, v));
    reply(C_NOTIFY, b, 3);
}

static int up_fill(uint32_t index, uint8_t *pay, void *ctx)   /* one sector's payload pulled from the host */
{
    uint32_t off = 0;
    (void)ctx;
    while (off < EF_SEC_PAYLOAD) {
        uint32_t n = EF_SEC_PAYLOAD - off > 512u ? 512u : EF_SEC_PAYLOAD - off;
        if (plat_host_read(index * EF_SEC_PAYLOAD + off, pay + off, n))
            return -1;
        off += n;
    }
    return 0;
}

static void get_name(char *d, const uint8_t *p)
{
    uint32_t i;
    for (i = 0; i < NAME_N; i++)
        d[i] = (char)p[i];
    name_clean(d, NAME_N);
}

void app_usb_cmd(const uint8_t *d, uint32_t n)
{
    static uint8_t out[800];
    uint32_t cmd = d[2], len = d[3] | (uint32_t)d[4] << 8 | (uint32_t)d[5] << 16, i, s = 0;
    const uint8_t *b = d + 6;
    if (cmd < C_HELLO || cmd > C_STATS || n < 7u + len)
        return;
    for (i = 0; i <= len; i++)
        s += d[6u + i];
    if ((s & 0xFFu) != 0xFFu)
        return;                          /* a damaged frame: no answer, the host asks again */
    host_until = plat_ms() + 20000u;
    switch (cmd) {
    case C_HELLO: {
        const char *v = APP_VERSION;
        out[0] = 0;
        out[1] = 'E'; out[2] = 'F'; out[3] = 'M'; out[4] = '1';
        out[5] = EF_PROTO;
        for (i = 0; i < 16u; i++)
            out[6u + i] = 0;
        for (i = 0; i < 15u && v[i]; i++)
            out[6u + i] = (uint8_t)v[i];
        out[22] = BANK_SLOTS;
        out[23] = BANK_NSEC;
        out[24] = (uint8_t)bank_free_secs(-1);
        put16(out + 25, EF_SEC_SAMPLES);
        put16(out + 27, EF_SEC_PAYLOAD);
        put32(out + 29, dir.gen);
        out[33] = P_COUNT;
        out[34] = (uint8_t)cur_slot;
        out[35] = ef_ctl()->play;
        out[36] = NUSER;
        out[37] = (uint8_t)NFACTORY;
        reply(cmd, out, 38);
        break;
    }
    case C_DIR:
        out[0] = 0;
        for (i = 0; i < BANK_SLOTS; i++) {
            uint8_t *o = out + 1u + i * 20u;
            uint32_t k;
            o[0] = slot_state[i];
            o[1] = dir.slot[i].used ? dir.slot[i].factory : 0;
            for (k = 0; k < NAME_N; k++)
                o[2u + k] = dir.slot[i].used ? (uint8_t)dir.slot[i].name[k] : 0;
            put32(o + 14, dir.slot[i].used ? dir.slot[i].frames : 0);
            put16(o + 18, dir.slot[i].used ? dir.slot[i].rate : 0);
        }
        reply(cmd, out, 1u + BANK_SLOTS * 20u);
        break;
    case C_UPLOAD: {
        char name[NAME_N];
        int rc;
        if (len < 4u + 8u + NAME_N) {
            plat_host_read(UP_FAIL_ADDR | BANK_E_ARGS, out, 8);
            break;
        }
        get_name(name, b + 12);
        if (ef_rec_on())
            rc = BANK_E_BUSY;
        else {
            plat_host_patience(3000u, 0);
            rc = bank_write(b[0], get32(b + 4), get16(b + 2), name, get32(b + 8), b[1], up_fill, 0, upload_progress);
        }
        dir_changed();
        plat_host_read(rc ? UP_FAIL_ADDR | (uint32_t)rc : UP_OK_ADDR, out, 8);   /* (8 bytes: the host answers "success") */
        break;
    }
    case C_ERASE:
        out[0] = (uint8_t)(len >= 1u ? bank_erase(b[0]) : BANK_E_ARGS);
        dir_changed();
        reply(cmd, out, 1);
        break;
    case C_RENAME: {
        char name[NAME_N];
        if (len < 1u + NAME_N)
            out[0] = BANK_E_ARGS;
        else {
            get_name(name, b + 1);
            out[0] = (uint8_t)bank_rename(b[0], name);
        }
        dir_changed();
        reply(cmd, out, 1);
        break;
    }
    case C_PARAMS:
        out[0] = 0;
        out[1] = P_COUNT;
        for (i = 0; i < P_COUNT; i++)
            put16(out + 2u + 2u * i, pq((int)i, pv[i]));
        out[2u + 2u * P_COUNT] = (uint8_t)cur_slot;
        out[3u + 2u * P_COUNT] = ef_ctl()->play;
        out[4u + 2u * P_COUNT] = ef_ctl()->freeze;
        for (i = 0; i < 8u; i++)
            out[5u + 2u * P_COUNT + i] = locks[i];
        reply(cmd, out, 13u + 2u * P_COUNT);
        break;
    case C_SET:
        for (i = 0; i + 3u <= len; i += 3u) {
            uint32_t id = b[i], q = get16(b + i + 1u);
            if (id < P_COUNT)
                param_set((int)id, pdq((int)id, (uint16_t)q), 1);
            else if (id == 0xF0u)
                play_set(q != 0);
            else if (id == 0xF1u)
                freeze_set(q != 0);
            else if (id == 0xF2u)
                select_slot((int)q);
            else if (id == 0xF3u)
                random_do(q != 0);
            else if (id == 0xF4u)
                undo_do();
            else if (id == 0xF6u)
                listen_cmd((int)q);
            else if (id == 0xF5u)
                ef_ctl()->dbg_bypass = q;   /* (diagnostics) */
            else if (id == 0xF7u)
                side_ceil = q >= 1000u ? 1.0f : (float)q * 0.001f;   /* the side limiter's ceiling (app.c) */
        }
        out[0] = 0;
        reply(cmd, out, 1);
        break;
    case C_PRESETS: {
        uint32_t o = 1, f;
        out[0] = 0;
        for (i = 0; i < NUSER; i++) {
            uint32_t k;
            out[o++] = pre.p[i].used;
            for (k = 0; k < NAME_N; k++)
                out[o++] = pre.p[i].used ? (uint8_t)pre.p[i].name[k] : 0;
            for (k = 0; k < NAME_N; k++)
                out[o++] = pre.p[i].used ? (uint8_t)pre.p[i].slot_name[k] : 0;
        }
        for (f = 0; f < (uint32_t)NFACTORY && o + 16u <= sizeof out; f++) {   /* 16 bytes a name, zero-padded */
            uint32_t k;
            const char *nm = FACTORY[f].name;
            int end = 0;
            for (k = 0; k < 16u; k++) {
                if (!end && (k == 15u || !nm[k]))
                    end = 1;             /* (never read past the name's end) */
                out[o++] = end ? 0u : (uint8_t)nm[k];
            }
        }
        reply(cmd, out, o);
        break;
    }
    case C_PRESET_OP: {
        char name[NAME_N];
        name[0] = 0;
        if (len >= 2u + NAME_N)
            get_name(name, b + 2);
        out[0] = (uint8_t)(len >= 2u ? preset_op(b[0], b[1], name) : BANK_E_ARGS);
        reply(cmd, out, 1);
        break;
    }
    case C_READ: {
        uint32_t sl = len >= 7u ? b[0] : BANK_SLOTS, off = get32(b + 1), cnt = get16(b + 5);
        out[0] = BANK_E_ARGS;
        if (sl < BANK_SLOTS && slot_state[sl] == SLOT_OK && cnt <= 512u) {
            ef_source_t src;
            uint32_t total;
            bank_source(sl, &src);
            total = src.nsec * EF_SEC_PAYLOAD;
            if (off <= total && cnt <= total - off) {
                for (i = 0; i < cnt; i++) {
                    uint32_t at = off + i;
                    out[1u + i] = src.sec[at / EF_SEC_PAYLOAD][at % EF_SEC_PAYLOAD];
                }
                out[0] = 0;
                reply(cmd, out, 1u + cnt);
                break;
            }
        }
        reply(cmd, out, 1);
        break;
    }
    case C_PRESET_GET: {
        const upreset_t *u = len >= 1u && b[0] < NUSER ? &pre.p[b[0]] : 0;
        uint32_t k;
        out[0] = u ? 0 : BANK_E_ARGS;
        if (!u) {
            reply(cmd, out, 1);
            break;
        }
        out[1] = u->used;
        out[2] = (uint8_t)u->slot;
        for (k = 0; k < NAME_N; k++) {
            out[3u + k] = (uint8_t)u->name[k];
            out[3u + NAME_N + k] = (uint8_t)u->slot_name[k];
        }
        out[3u + 2u * NAME_N] = P_COUNT;
        for (k = 0; k < P_COUNT; k++)
            put16(out + 4u + 2u * NAME_N + 2u * k, u->q[k]);
        reply(cmd, out, 4u + 2u * NAME_N + 2u * P_COUNT);
        break;
    }
    case C_PRESET_PUT: {
        uint32_t k, np = len >= 4u + 2u * NAME_N ? b[3u + 2u * NAME_N] : 0;
        out[0] = BANK_E_ARGS;
        if (len >= 4u + 2u * NAME_N && b[0] < NUSER && b[1] <= 1u && np <= P_COUNT && len >= 4u + 2u * NAME_N + 2u * np) {
            upreset_t *u = &pre.p[b[0]];
            u->used = b[1];
            u->slot = (int8_t)b[2];
            get_name(u->name, b + 3);
            get_name(u->slot_name, b + 3 + NAME_N);
            for (k = 0; k < P_COUNT; k++)
                u->q[k] = k < np ? (uint16_t)get16(b + 4u + 2u * NAME_N + 2u * k) : pq((int)k, PI[k].def);
            for (k = 0; k < P_COUNT; k++)   /* a value beyond its range comes back inside it */
                u->q[k] = pq((int)k, pdq((int)k, u->q[k]));
            out[0] = (uint8_t)(store_quiet(OBJ_PRESETS, &pre, sizeof pre) ? BANK_E_STORE : BANK_OK);
        }
        reply(cmd, out, 1);
        break;
    }
    case C_STATS: {
        const ef_meter_t *m = ef_meter();
        uint32_t v[14];
        v[0] = m->t_render / PLAT_TICKS_PER_US;
        v[1] = m->t_grains / PLAT_TICKS_PER_US;
        v[2] = m->t_fx / PLAT_TICKS_PER_US;
        v[3] = m->t_spawn / PLAT_TICKS_PER_US;
        v[4] = plat_cpu_pct();
        v[5] = plat_xruns();
        v[6] = m->grains;
        v[7] = ef_ctl()->shed;
        v[8] = ef_ctl()->max_grains;
        v[9] = (uint32_t)app_cpu2_state();
        v[10] = rt_frames;                 /* the audio's timing (app_render): frames, the longest wait (us, since the
                                            * last STATS), waits over 1.5 halves (since boot), the time (ms) */
        v[11] = rt_gap_max / PLAT_TICKS_PER_US;
        v[12] = rt_gaps_long;
        v[13] = plat_ms();
        rt_gap_max = 0;
        ef_meter_time_reset();
        out[0] = 0;
        for (i = 0; i < 14u; i++)
            put32(out + 1u + 4u * i, v[i]);
        reply(cmd, out, 57);
        break;
    }
    case C_WIPE:
        out[0] = BANK_E_ARGS;
        if (len == 4u && b[0] == 'W' && b[1] == 'I' && b[2] == 'P' && b[3] == 'E') {
            quiet_begin();
            for (i = 0; i < BANK_SLOTS; i++) {
                dir.slot[i].used = 0;
                slot_state[i] = SLOT_EMPTY;
            }
            out[0] = (uint8_t)dir_save();
            quiet_end();
            dir_changed();
        }
        reply(cmd, out, 1);
        break;
    }
}
