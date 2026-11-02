/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Philip Bos */
/* ECHOFIELD FM-1: the app. The panel and the screen, the sample bank, presets and the companion app's commands, around
 * the granular engine (dsp/engine.c). Part of fm1-base's unity build (device/unity.c, host/plat_host.c): gfx.c is in
 * scope (cv_*, fonts, RGB).
 *
 * The panel:
 *   K1..K4         the page's four parameters (blue, green, white, orange)
 *   SELECT         POSITION in the sample          ALGORITHM   the mode          PRESETS   browse and load presets
 *   HOME ENV LFO FX SEL EDIT GLO SAVE   pages (again: the group's next page). Hold HOME: what this page does
 *   PLAY   the drone on / off       REC   RESAMPLE: the output into a new sample      SEQ   FREEZE
 *   ARP    RANDOM (this page; held: everything). ARP held + a knob: lock / unlock it.  OCT- + OCT+: undo RANDOM
 *   OCT- / OCT+    the keys' octave (on PRESETS and STORAGE: back / do it)
 *   keys           play the cloud at their pitch (C4: the sample's own)
 *   FX held + black key: a performance effect while held   EDIT held + white key: sample 1..16
 *   SAVE held + white key: user preset 1..16, + black key: factory preset   SEL held + a key: the oracle's root */
#include "fastmath.h"
#include "adpcm.h"
#ifdef PLAT_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

/* the audio's own timing, seen from here (STATS): frames rendered, the longest wait between two renders (a half the
 * DMA played twice shows as a wait of two halves), how many waits were over one and a half halves */
static volatile uint32_t rt_frames, rt_last, rt_gap_max, rt_gaps_long;

/* the side limiter (app_render): the FM-1's headphone amplifier crackles when, loud, left and right swing far apart (a
 * wide reverb), never in mono, never on the line input; so the difference (L - R) / 2 is held under this ceiling (of
 * full scale), the sum untouched. -17 dBFS: the highest clean on headphones (2026-11-02; -14 crackled). Only near full
 * MASTER does it act. Proto SET 0xF7 sets it (thousandths of full scale; 1000 = off) */
static volatile float side_ceil = 0.141f;

#include "params.c"
#include "presets.c"
#include "bank.c"
#include "proto.c"

/* ================================================================ state */
static ef_source_t srcs[2];
static uint8_t src_i;
static int pending_slot = -100;          /* a selection that waits for the engine to let the last one go */
static uint8_t listen_after;             /* LISTEN once that selection is made */
static int8_t listen_key = -1;           /* EDIT + this white key: LISTEN while it is held */

/* LISTEN: the sample itself, START to END (ef_listen); after a selection still waiting, once it is made */
static void listen_cmd(int how)          /* the companion's (proto.c SET 0xF6): 0 stop, 1 once, 2 looped */
{
    if (!how) {
        listen_after = 0;
        ef_listen(0, 0);
    } else if (pending_slot != -100)
        listen_after = 1;
    else
        ef_listen(1, how == 2);
}
static void listen_start(void)
{
    if (pending_slot != -100)
        listen_after = 1;
    else
        ef_listen(1, 0);
}
static volatile float mute_target = 1.0f;
static float mute_gain = 1.0f;
static int quiet_depth;
static uint8_t seed_ready;

static uint8_t ses_dirty;
static uint32_t ses_changed_at;
static uint8_t backup_hold;
static float undo_pv[P_COUNT];
static uint8_t undo_ok;

static int8_t ov_lo[240], ov_hi[240];    /* the playing sample's outline, -64..63 */

enum { SCR_PRESETS = NPAGES, SCR_SYSTEM, SCR_STORAGE, NSCREENS };

static struct {
    int page;                            /* a knob page (PAGES), or SCR_* */
    int group_page[G_COUNT];             /* the page each group returns to */
    uint32_t btn, keys;                  /* held at the last input */
    uint32_t down_at[NB];                /* when each button went down */
    uint8_t used[NB];                    /* a held button did something with another control: no tap on release */
    uint8_t lock_done[4];                /* ARP held: each knob locks once */
    int octave;                          /* -2..+2 */
    uint8_t vel[NKEYS];
    uint32_t latched;                    /* LATCH: the keys held on */
    uint32_t midi_notes[4];              /* MIDI notes held (bit per note 0..127) */
    uint8_t midi_vel[128];
    uint8_t sustain;
    uint32_t turned_at[4];
    int8_t touched;                      /* the cell turned last (-1 none) */
    uint32_t enc_t[NE];                  /* acceleration */
    char msg[2][24];
    uint32_t msg_until;
    int preset_cursor;                   /* PRESETS: the list's cursor; the PRESETS encoder's choice */
    uint32_t preset_turned_at;
    int preset_pending;
    int confirm;                         /* a question waiting for its second press: CONFIRM_* */
    uint32_t confirm_until;
    int store_cursor;
    uint8_t d_head, d_knobs, d_field;
    uint32_t frame;
    uint8_t help;
    uint8_t settings_dirty;
    uint32_t rec_t0;
    uint32_t shed_at;
    uint32_t say_at;
    uint32_t sound_at;                   /* the last time something was meant to sound (PLAY, keys, RESAMPLE) */
    float peak_q;                        /* the output's peak over the last frame */
} ui;

enum { CONFIRM_NONE, CONFIRM_OVERWRITE, CONFIRM_DELETE_PRESET, CONFIRM_DELETE_SAMPLE };

/* ================================================================ colours */
static uint16_t col_k[4], col_bg, col_line, col_dim, col_gray, col_text, col_mode[MODE_COUNT], col_rec;
static void theme_apply(void)
{
    col_bg = RGB(0, 0, 0);
    col_line = RGB(40, 40, 46);
    col_dim = RGB(78, 78, 88);
    col_gray = RGB(140, 140, 150);
    col_text = RGB(240, 240, 240);
    col_rec = RGB(255, 60, 60);
    if (set.theme == THEME_MONO) {
        col_k[0] = col_k[1] = col_k[2] = col_k[3] = RGB(230, 230, 230);
    } else if (set.theme == THEME_AMBER) {
        col_k[0] = RGB(255, 176, 60);
        col_k[1] = RGB(255, 140, 30);
        col_k[2] = RGB(255, 210, 140);
        col_k[3] = RGB(230, 110, 20);
    } else {
        col_k[0] = RGB(61, 139, 255);     /* OP-1's four: blue, green, white, orange */
        col_k[1] = RGB(48, 208, 122);
        col_k[2] = RGB(230, 230, 230);
        col_k[3] = RGB(255, 122, 26);
    }
    col_mode[MODE_CLOUD] = col_k[0];
    col_mode[MODE_DUST] = col_k[2];
    col_mode[MODE_TAPE] = col_k[3];
    col_mode[MODE_BLOOM] = col_k[1];
    col_mode[MODE_PULSE] = set.theme == THEME_COLOUR ? RGB(255, 77, 94) : col_k[3];
    col_mode[MODE_DRIFT] = set.theme == THEME_COLOUR ? RGB(168, 121, 255) : col_k[0];
}
static uint16_t mix565(uint16_t a, uint16_t b, uint32_t t)   /* a .. b, t 0..256 */
{
    uint32_t r = ((a >> 11) * (256u - t) + (b >> 11) * t) >> 8, g = (((a >> 5) & 63u) * (256u - t) + ((b >> 5) & 63u) * t) >> 8,
             bl = ((a & 31u) * (256u - t) + (b & 31u) * t) >> 8;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

/* ================================================================ sound plumbing */
#ifndef FM1B_CPU2
#define FM1B_CPU2 0
#endif
/* the second core: 0 off (not built, or it did not start), 1 rendering, 2 + the reason it was given up */
static int app_cpu2_state(void)
{
#if FM1B_CPU2
    if (plat_cpu2_on())
        return 1;
    return plat_cpu2_gave_up() ? 2 + plat_cpu2_gave_up() : 0;
#else
    return 0;
#endif
}
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    float t = mute_target;
    {
        uint32_t now = plat_ticks24(), gap = now - rt_last;
        if (rt_last) {
            if (gap > rt_gap_max)
                rt_gap_max = gap;
            if (gap > (uint32_t)(1.5f * (float)APP_BLOCK * 1000000.0f / 44100.0f) * PLAT_TICKS_PER_US)
                rt_gaps_long++;
        }
        rt_last = now;
        rt_frames += n;
    }
    if (ef_ctl()->dbg_bypass & 8u) {     /* (diagnostics: a plain 440 Hz sine, the engine out of the way; at MASTER,
                                          * up to the limiter's ceiling as the music reaches it: -1 dBFS) */
        static uint32_t ph;
        uint32_t i;
        const float amp = 0.89f * 8300000.0f * (float)master_q12 * (1.0f / 4096.0f);
        for (i = 0; i < n; i++, ph += 42852281u) {   /* 440 / 44100 * 2^32 */
            int32_t v = (int32_t)(fm_sin_turns((float)ph * (1.0f / 4294967296.0f)) * amp);
            lr[2u * i] = lr[2u * i + 1u] = v;
        }
        return;
    }
    mute_gain += (t - mute_gain) * 0.45f;
    if (fm_fabsf(t - mute_gain) < 0.002f)
        mute_gain = t;
    master_q12 = master_q12 * set.out_level / 10u;   /* OUTPUT: the ceiling, MASTER below it (USB, headphones and
                                          * speaker alike: fm1-base's separate USB level, plat_usb_audio_full, crackles
                                          * on the headphones, so it stays off) */
    ef_render(lr, n, (float)master_q12 * (1.0f / 4096.0f) * mute_gain);
    if (ef_ctl()->dbg_bypass & 64u) {    /* (diagnostics: the output in mono, L and R summed) */
        uint32_t i;
        for (i = 0; i < n; i++)
            lr[2u * i] = lr[2u * i + 1u] = (lr[2u * i] >> 1) + (lr[2u * i + 1u] >> 1);
    } else if (side_ceil < 1.0f) {       /* the side limiter: instant down, back up over ~100 ms */
        static float g = 1.0f;
        const float thr = side_ceil * 8300000.0f, rel = 1.0f / (0.1f * 44100.0f);
        uint32_t i;
        for (i = 0; i < n; i++) {
            float l = (float)lr[2u * i], r = (float)lr[2u * i + 1u], m = (l + r) * 0.5f, sd = (l - r) * 0.5f;
            float a = fm_fabsf(sd), need = a > thr ? thr / a : 1.0f;
            if (need < g)
                g = need;
            else
                g += (1.0f - g) * rel;
            sd *= g;
            lr[2u * i] = (int32_t)(m + sd);
            lr[2u * i + 1u] = (int32_t)(m - sd);
        }
    }
}

void app_panic(void) { ef_panic(); }

static void settle(uint32_t ms)          /* let the audio interrupt run a while (the device; the host has no time) */
{
#ifndef PLAT_HOST
    uint32_t t0 = plat_ms();
    while (plat_ms() - t0 < ms)
        plat_yield();
#else
    (void)ms;
#endif
}

/* the sound faded out and the bank let go (a bank write, a store write while playing), then back */
static void quiet_begin(void)
{
    uint32_t i;
    if (quiet_depth++)
        return;
    mute_target = 0.0f;
    settle(30);
    ef_set_source(0);
    for (i = 0; i < 20u && !ef_source_idle(); i++)
        settle(5);
}
static void select_now(int s);
static void quiet_end(void)
{
    if (--quiet_depth)
        return;
    select_now(cur_slot);
    mute_target = 1.0f;
}
/* a store save with the sound faded around it (an erase stops the audio for tens of ms: a gap, never a click) */
static int store_quiet(uint32_t obj, const void *p, uint32_t n)
{
    int rc;
    mute_target = 0.0f;
    settle(30);
    rc = plat_store_save(obj, p, n);
    mute_target = 1.0f;
    return rc;
}

/* the outline of a source for the screen: 240 columns, a few blocks each */
static void overview(const ef_source_t *s)
{
    uint32_t c;
    int16_t blk[EF_BLK_SAMPLES];
    for (c = 0; c < 240u; c++) {
        ov_lo[c] = ov_hi[c] = 0;
    }
    if (!s || !s->frames)
        return;
    for (c = 0; c < 240u; c++) {
        uint32_t a = s->frames * c / 240u, b = s->frames * (c + 1u) / 240u, b0 = a / EF_BLK_SAMPLES,
                 b1 = (b + EF_BLK_SAMPLES - 1u) / EF_BLK_SAMPLES, step = (b1 - b0 + 1u) / 2u, k;
        int32_t lo = 0, hi = 0;
        if (!step)
            step = 1;
        for (k = b0; k < b1; k += step) {
            uint32_t j, j0 = k == b0 ? a % EF_BLK_SAMPLES : 0, j1 = k == b1 - 1u && b % EF_BLK_SAMPLES ? b % EF_BLK_SAMPLES : EF_BLK_SAMPLES;
            uint32_t sec = k / EF_SEC_BLOCKS;
            if (sec >= s->nsec)
                break;
            ef_ima_block(s->sec[sec] + (k % EF_SEC_BLOCKS) * EF_BLK_BYTES, blk);
            for (j = j0; j < j1; j++) {
                if (blk[j] < lo)
                    lo = blk[j];
                if (blk[j] > hi)
                    hi = blk[j];
            }
        }
        ov_lo[c] = (int8_t)(lo >> 9);
        ov_hi[c] = (int8_t)(hi >> 9);
    }
}

static void select_now(int s)            /* the engine onto a sample (no checks on the engine's readiness) */
{
    ef_source_t *d = &srcs[src_i ^ 1u];
    const ef_source_t *from = 0;
    if (s >= 0 && s < (int)BANK_SLOTS && slot_state[s] == SLOT_OK) {
        bank_source((uint32_t)s, d);
        from = d;
    } else if (s == -2 && rec_src.frames) {
        from = &rec_src;
    } else {
        s = -1;
        if (!seed_ready) {
            seed_make();
            seed_ready = 1;
        }
        from = &seed_src;
    }
    if (from == d)
        src_i ^= 1u;
    cur_slot = (int8_t)s;
    ef_set_source(from);
    overview(from);
    ui.d_head = ui.d_field = ui.d_knobs = 1;
}

static void select_slot(int s)
{
    if (s >= 0 && (s >= (int)BANK_SLOTS || slot_state[s] != SLOT_OK)) {
        app_say("SAMPLE", s < (int)BANK_SLOTS && slot_state[s] == SLOT_DAMAGED ? "DAMAGED" : "EMPTY SLOT");
        return;
    }
    if (!ef_source_idle() || quiet_depth) {
        pending_slot = s;
        return;
    }
    pending_slot = -100;
    select_now(s);
    ses_dirty = 1;
    ses_changed_at = plat_ms();
}

static int first_ok_slot(void)
{
    uint32_t s;
    for (s = 0; s < BANK_SLOTS; s++)
        if (slot_state[s] == SLOT_OK)
            return (int)s;
    return -1;
}

static void dir_changed(void)            /* the bank changed (an upload, an erase): the sample playing still there? */
{
    if ((cur_slot >= 0 && slot_state[cur_slot] != SLOT_OK) || cur_slot == -1)
        cur_slot = (int8_t)first_ok_slot();   /* (the seed gives way to the first real sample) */
    if (!quiet_depth)
        select_now(cur_slot);
    ui.d_head = ui.d_field = ui.d_knobs = 1;
}

static void upload_progress(uint32_t k, uint32_t n)   /* the screen while a sample comes in (the main loop is ours) */
{
    char t[32], a[8];
    int32_t w = n ? (int32_t)(200u * k / n) : 0;
    cv_begin(240, 60, col_bg);
    cv_text(20, 4, &FONT_B, "RECEIVING SAMPLE", col_text);
    cv_rect(20, 30, 200, 8, col_line);
    cv_rect(20, 30, w, 8, col_k[3]);
    int_text((int32_t)k, t);
    text_cat(t, " / ");
    int_text((int32_t)n, a);
    text_cat(t, a);
    text_cat(t, " SECTORS");
    cv_text(20, 42, &FONT_XS, t, col_gray);
    cv_blit(0, 90);
    ui.d_head = ui.d_field = ui.d_knobs = 1;
}

/* ================================================================ parameters */
static uint8_t cc_last[P_COUNT];
static void param_set(int id, float v, int from_host)
{
    v = pclamp(id, v);
    if (v == pv[id])
        return;
    pv[id] = v;
    ef_ctl()->p[id] = v;
    ses_dirty = 1;
    ses_changed_at = plat_ms();
    ui.d_knobs = 1;
    if (id == P_MODE)
        ui.d_head = 1;
    if (!from_host) {
        notify(id, v);
        if (set.cc_out && PI[id].cc) {
            uint32_t q = (uint32_t)(pnorm(id, v) * 127.0f + 0.5f), ch = set.midi_ch ? set.midi_ch - 1u : 0u;
            if (q != cc_last[id]) {
                cc_last[id] = (uint8_t)q;
                plat_midi_out(0x0Bu | (0xB0u | ch) << 8 | (uint32_t)PI[id].cc << 16 | q << 24);
            }
        }
    }
}

static void params_all(const float *v)   /* a whole sound at once (a preset, an undo) */
{
    int i;
    for (i = 0; i < P_COUNT; i++)
        param_set(i, v[i], 0);
}

static void play_set(int on)
{
    ef_ctl()->play = (uint8_t)(on != 0);
    ses_dirty = 1;
    ses_changed_at = plat_ms();
    ui.d_head = 1;
}
static void freeze_set(int on)
{
    ef_ctl()->freeze = (uint8_t)(on != 0);
    ui.d_head = 1;
}

/* ================================================================ session */
static void session_save(void)
{
    session_t s;
    uint32_t i;
    s.magic = SES_MAGIC;
    s.version = 1;
    s.n = P_COUNT;
    for (i = 0; i < P_COUNT; i++)
        s.q[i] = pq((int)i, pv[i]);
    for (i = 0; i < NAME_N; i++)
        s.slot_name[i] = cur_slot >= 0 ? dir.slot[cur_slot].name[i] : 0;
    s.slot = cur_slot >= 0 ? cur_slot : -1;
    s.play = ef_ctl()->play;
    for (i = 0; i < 8u; i++)
        s.locks[i] = locks[i];
    s.octave = (uint8_t)(ui.octave + 2);
    s.rsv = 0;
    if (plat_store_save(OBJ_SESSION, &s, sizeof s) == 0)
        ses_dirty = 0;
    ui.d_head = 1;
}

static void session_load(int *slot)
{
    session_t s;
    uint32_t i;
    *slot = -1;
    pdefaults(pv);
    for (i = 0; i < 8u; i++)
        locks[i] = 0;
    if (plat_store_load(OBJ_SESSION, &s, sizeof s) < 16 || s.magic != SES_MAGIC || s.version != 1u || !s.n ||
        s.n > P_COUNT) {
        *slot = first_ok_slot();
        return;
    }
    for (i = 0; i < s.n; i++)
        pv[i] = pdq((int)i, s.q[i]);
    name_clean(s.slot_name, NAME_N);
    *slot = bank_find(s.slot_name);
    if (*slot < 0 && s.slot >= 0 && s.slot < (int8_t)BANK_SLOTS && slot_state[s.slot] == SLOT_OK)
        *slot = s.slot;
    if (*slot < 0)
        *slot = first_ok_slot();
    for (i = 0; i < 8u; i++)
        locks[i] = s.locks[i];
    ui.octave = s.octave <= 4u ? (int)s.octave - 2 : 0;
    ef_ctl()->play = s.play ? 1u : 0u;
}

/* ================================================================ presets */
static int preset_count(void) { return (int)NUSER + NFACTORY; }   /* the list: U01..U16, then the factory's */

static void preset_label(int i, char *t)
{
    t[0] = 0;
    if (i < (int)NUSER) {
        t[0] = 'U';
        t[1] = (char)('0' + (i + 1) / 10);
        t[2] = (char)('0' + (i + 1) % 10);
        t[3] = ' ';
        t[4] = 0;
        text_cat(t, pre.p[i].used ? pre.p[i].name : "---");
    } else {
        int f = i - (int)NUSER;
        t[0] = 'F';
        t[1] = (char)('0' + (f + 1) / 10);
        t[2] = (char)('0' + (f + 1) % 10);
        t[3] = ' ';
        t[4] = 0;
        text_cat(t, FACTORY[f].name);
    }
}

static void preset_load(int i)
{
    float v[P_COUNT];
    int s = -1, k;
    char t[24];
    if (i < 0 || i >= preset_count())
        return;
    if (i < (int)NUSER) {
        const upreset_t *u = &pre.p[i];
        if (!u->used) {
            app_say("PRESET", "EMPTY");
            return;
        }
        for (k = 0; k < P_COUNT; k++)
            v[k] = pdq(k, u->q[k]);
        s = bank_find(u->slot_name);
        if (s < 0 && u->slot >= 0 && u->slot < (int)BANK_SLOTS && slot_state[u->slot] == SLOT_OK && !u->slot_name[0])
            s = u->slot;
    } else {
        factory_values(i - (int)NUSER, pv, v);
        s = bank_find(FACTORY[i - (int)NUSER].sample);
    }
    params_all(v);
    ef_reset_time();
    if (s >= 0 && s != cur_slot)
        select_slot(s);
    preset_label(i, t);
    if (s >= 0 || i == (int)NUSER + NFACTORY - 1)
        app_say(t + 4, "");
    else {                               /* its sound is not on the FM-1: say which, it plays on the one there is */
        char need[24] = "NEEDS ";
        const char *nm = i < (int)NUSER ? pre.p[i].slot_name : FACTORY[i - (int)NUSER].sample;
        text_cat(need, nm[0] ? nm : "ITS SAMPLE");
        app_say(t + 4, need);
    }
}

static int preset_store(int i, const char *name)
{
    upreset_t *u;
    uint32_t k;
    if (i < 0 || i >= (int)NUSER)
        return BANK_E_ARGS;
    u = &pre.p[i];
    if (name && name[0])
        text_cpy(u->name, name, NAME_N);
    else if (!u->used)
        random_name(u->name);
    for (k = 0; k < NAME_N; k++)
        u->slot_name[k] = cur_slot >= 0 ? dir.slot[cur_slot].name[k] : 0;
    u->slot = cur_slot;
    u->used = 1;
    for (k = 0; k < P_COUNT; k++)
        u->q[k] = pq((int)k, pv[k]);
    return store_quiet(OBJ_PRESETS, &pre, sizeof pre) ? BANK_E_STORE : BANK_OK;
}

static int preset_op(uint32_t op, uint32_t idx, const char *name)
{
    switch (op) {
    case 0:
        if (idx >= NUSER || !pre.p[idx].used)
            return BANK_E_ARGS;
        preset_load((int)idx);
        return BANK_OK;
    case 1:
        return preset_store((int)idx, name);
    case 2:
        if (idx >= NUSER)
            return BANK_E_ARGS;
        pre.p[idx].used = 0;
        return store_quiet(OBJ_PRESETS, &pre, sizeof pre) ? BANK_E_STORE : BANK_OK;
    case 3:
        if (idx >= (uint32_t)NFACTORY)
            return BANK_E_ARGS;
        preset_load((int)(NUSER + idx));
        return BANK_OK;
    case 4:
        if (idx >= NUSER || !pre.p[idx].used || !name[0])
            return BANK_E_ARGS;
        text_cpy(pre.p[idx].name, name, NAME_N);
        return store_quiet(OBJ_PRESETS, &pre, sizeof pre) ? BANK_E_STORE : BANK_OK;
    }
    return BANK_E_ARGS;
}

/* ================================================================ RANDOM */
static void random_do(int all)
{
    uint8_t only[8] = {0};
    int k;
    for (k = 0; k < P_COUNT; k++)
        undo_pv[k] = pv[k];
    undo_ok = 1;
    if (!all) {
        if (ui.page >= NPAGES)
            return;
        for (k = 0; k < 4; k++) {
            int id = PAGES[ui.page].p[k];
            if (id >= 0)
                only[id >> 3] |= (uint8_t)(1u << (id & 7));
        }
        if (ui.page == 12)               /* SAMPLE: the position alone */
            only[P_POSITION >> 3] = (uint8_t)(1u << (P_POSITION & 7));
    }
    {
        float v[P_COUNT];
        for (k = 0; k < P_COUNT; k++)
            v[k] = pv[k];
        randomize(v, all ? 0 : only);
        params_all(v);
    }
    app_say("RANDOM", all ? "EVERYTHING" : "THIS PAGE");
}

static void undo_do(void)
{
    float v[P_COUNT];
    int k;
    if (!undo_ok) {
        app_say("UNDO", "NOTHING TO UNDO");
        return;
    }
    for (k = 0; k < P_COUNT; k++) {
        v[k] = undo_pv[k];
        undo_pv[k] = pv[k];
    }
    params_all(v);
    app_say("UNDO", "");
}

/* ================================================================ RESAMPLE */
static void rec_toggle(void)
{
    if (!ef_rec_on()) {
        uint32_t room = bank_free_secs(-1);
        if (bank_free_slot() < 0 || room == 0) {
            app_say("RESAMPLE", "BANK FULL");
            return;
        }
        ef_rec_start(rec_mem, room < REC_NSEC ? room : REC_NSEC);
        ui.rec_t0 = plat_ms();
        app_say("RESAMPLE", "RECORDING");
    } else {
        uint32_t frames = ef_rec_stop();
        int s = bank_free_slot(), rc;
        char name[NAME_N] = "REC ";
        uint32_t k, nr = 1;
        if (frames < 2048u) {
            app_say("RESAMPLE", "TOO SHORT");
            ui.d_head = 1;
            return;
        }
        for (k = 0; k < BANK_SLOTS; k++)   /* REC 1, REC 2 ... the first number not taken */
            if (dir.slot[k].used && dir.slot[k].name[0] == 'R' && dir.slot[k].name[1] == 'E' && dir.slot[k].name[2] == 'C')
                nr++;
        int_text((int32_t)nr, name + 4);
        if (s < 0)
            rc = BANK_E_ROOM;
        else
            rc = bank_write((uint32_t)s, frames, 22050, name, 0, 0, rec_fill, 0, 0);
        if (rc == BANK_OK) {
            cur_slot = (int8_t)s;
            dir_changed();
            app_say(name, "SAVED");
        } else {
            uint32_t i;
            for (i = 0; i < REC_NSEC; i++)   /* no room: it plays from RAM until the next take */
                rec_src.sec[i] = rec_mem + i * EF_SEC_PAYLOAD;
            rec_src.nsec = (frames + EF_SEC_SAMPLES - 1u) / EF_SEC_SAMPLES;
            rec_src.frames = frames;
            rec_src.rate = 22050;
            cur_slot = -2;
            dir_changed();
            app_say("NOT SAVED", rc == BANK_E_ROOM ? "BANK FULL" : "FLASH ERROR");
        }
    }
    ui.d_head = 1;
}

/* ================================================================ notes */
static void notes_push(void)             /* the keys, latched keys and MIDI notes held -> the engine */
{
    ef_ctl_t *c = ef_ctl();
    uint32_t k, n = 0, keys = ui.keys | ui.latched;
    int8_t note[EF_MAX_NOTES];
    uint8_t vel[EF_MAX_NOTES];
    for (k = 0; k < NKEYS && n < EF_MAX_NOTES; k++)
        if ((keys >> k) & 1u) {
            note[n] = (int8_t)(53 + (int32_t)k + 12 * ui.octave - 60);
            vel[n++] = ui.vel[k] ? ui.vel[k] : 100u;
        }
    for (k = 0; k < 128u && n < EF_MAX_NOTES; k++)
        if ((ui.midi_notes[k >> 5] >> (k & 31u)) & 1u) {
            note[n] = (int8_t)((int32_t)k - 60);
            vel[n++] = ui.midi_vel[k];
        }
    for (k = 0; k < n; k++) {
        c->note[k] = note[k];
        c->vel[k] = vel[k];
    }
    c->nnotes = (uint8_t)n;
}
static void note_new(void) { ef_ctl()->note_seq++; }

static void midi_note_out(int on, uint32_t note, uint32_t vel)
{
    uint32_t ch = set.midi_ch ? set.midi_ch - 1u : 0u;
    if (on)
        plat_midi_out(0x09u | (0x90u | ch) << 8 | note << 16 | vel << 24);
    else
        plat_midi_out(0x08u | (0x80u | ch) << 8 | note << 16);
}

/* ================================================================ MIDI in */
static void midi_in(void)
{
    uint32_t pkt;
    int port;
    int changed = 0;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu, ty = st & 0xF0u, ch = st & 15u;
        if (st < 0x80u || st >= 0xF0u)
            continue;
        if (set.midi_ch && ch != set.midi_ch - 1u)
            continue;
        if (ty == 0x90u && d2) {
            ui.midi_notes[d1 >> 5] |= 1u << (d1 & 31u);
            ui.midi_vel[d1] = (uint8_t)d2;
            note_new();
            changed = 1;
        } else if (ty == 0x80u || ty == 0x90u) {
            if (!ui.sustain)
                ui.midi_notes[d1 >> 5] &= ~(1u << (d1 & 31u));
            changed = 1;
        } else if (ty == 0xB0u) {
            int i;
            if (d1 == 64u) {
                ui.sustain = d2 >= 64u;
                if (!ui.sustain) {
                    ui.midi_notes[0] = ui.midi_notes[1] = ui.midi_notes[2] = ui.midi_notes[3] = 0;
                    changed = 1;
                }
            } else if (d1 == 120u || d1 == 123u) {
                ui.midi_notes[0] = ui.midi_notes[1] = ui.midi_notes[2] = ui.midi_notes[3] = 0;
                changed = 1;
            } else if (d1 == 102u)
                param_set(P_MODE, (float)(d2 * MODE_COUNT / 128u), 1);
            else if (d1 == 103u)
                freeze_set(d2 >= 64u);
            else if (d1 == 104u)
                play_set(d2 >= 64u);
            else
                for (i = 0; i < P_COUNT; i++)
                    if (PI[i].cc == d1) {
                        const pinfo_t *p = &PI[i];
                        param_set(i, p->min + (p->max - p->min) * (float)d2 / 127.0f, 1);
                    }
        } else if (ty == 0xC0u) {
            preset_load((int)d1 < preset_count() ? (int)d1 : preset_count() - 1);
        }
    }
    if (changed)
        notes_push();
}

/* ================================================================ messages */
void app_say(const char *a, const char *b)
{
    text_cpy(ui.msg[0], a ? a : "", 24);
    text_cpy(ui.msg[1], b ? b : "", 24);
    ui.msg_until = plat_ms() + 1300u;
    ui.d_field = 1;
    if (ann_on()) {
        char t[52];
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

/* ================================================================ pages */
static int group_of(int page)
{
    return page < NPAGES ? PAGES[page].group : page == SCR_PRESETS ? G_PRESETS : G_SYSTEM;
}
static void page_go(int page)
{
    if (ui.page == SCR_SYSTEM && page != SCR_SYSTEM && ui.settings_dirty) {
        ui.settings_dirty = 0;
        store_quiet(OBJ_SETTINGS, &set, sizeof set);
    }
    ui.page = page;
    ui.group_page[group_of(page)] = page;
    ui.confirm = CONFIRM_NONE;
    ui.touched = -1;
    ui.d_head = ui.d_field = ui.d_knobs = 1;
}
static void group_button(int g)          /* a page button: its group's page, or the next page of it */
{
    int p, first = -1, next = -1;
    if (g == G_PRESETS) {
        page_go(SCR_PRESETS);
        return;
    }
    if (g == G_SYSTEM) {
        page_go(ui.page == SCR_SYSTEM ? SCR_STORAGE : SCR_SYSTEM);
        return;
    }
    if (group_of(ui.page) != g) {
        page_go(ui.group_page[g]);
        return;
    }
    for (p = 0; p < NPAGES; p++)
        if (PAGES[p].group == g) {
            if (first < 0)
                first = p;
            if (p > ui.page && next < 0)
                next = p;
        }
    page_go(next >= 0 ? next : first);
}
static int group_pages(int g, int *index, int page)   /* how many pages the group has, and page's place in it */
{
    int p, n = 0;
    for (p = 0; p < NPAGES; p++)
        if (PAGES[p].group == g) {
            if (p == page)
                *index = n;
            n++;
        }
    return n;
}

/* ================================================================ input */
static int32_t accel(int e, uint32_t k)  /* faster turns go further */
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[k];
    int32_t m = dt < 25u ? 6 : dt < 60u ? 3 : 1;
    ui.enc_t[k] = now;
    return (int32_t)e * m;
}

static void knob_turn(int cell, int e)
{
    int id;
    if (ui.page == SCR_SYSTEM) {
        int d = e > 0 ? 1 : -1;
        if (cell == 0)
            set.theme = (uint8_t)((set.theme + NTHEMES + d) % NTHEMES), theme_apply();
        else if (cell == 1)
            set.midi_ch = (uint8_t)clamp(set.midi_ch + d, 0, 16);
        else if (cell == 2)
            set.voices = (uint8_t)clamp(set.voices + d, 8, EF_MAX_GRAINS), ef_ctl()->max_grains = set.voices;
        else if (cell == 3) {            /* MASTER, 50 % .. 100 % */
            set.out_level = (uint8_t)clamp(set.out_level + d, 5, 10);
        }
        ui.settings_dirty = 1;
        ui.d_knobs = ui.d_field = ui.d_head = 1;
        ui.touched = (int8_t)cell;
        ui.turned_at[cell] = plat_ms();
        return;
    }
    if (ui.page == SCR_PRESETS) {
        if (cell == 0) {
            ui.preset_cursor = clamp(ui.preset_cursor + e, 0, preset_count() - 1);
            ui.confirm = CONFIRM_NONE;
            ui.d_field = 1;
        }
        return;
    }
    if (ui.page == SCR_STORAGE) {
        if (cell == 0) {
            ui.store_cursor = clamp(ui.store_cursor + (e > 0 ? 1 : -1), 0, (int)BANK_SLOTS - 1);
            ui.confirm = CONFIRM_NONE;
            ui.d_field = 1;
        }
        return;
    }
    id = PAGES[ui.page].p[cell];
    ui.touched = (int8_t)cell;
    ui.turned_at[cell] = plat_ms();
    ui.d_knobs = 1;
    if (id == -2) {                      /* SAMPLE: K1 picks the sample */
        int s = cur_slot, k;
        for (k = 0; k < (int)BANK_SLOTS; k++) {
            s = (s + (e > 0 ? 1 : -1) + (int)BANK_SLOTS) % (int)BANK_SLOTS;
            if (slot_state[s] == SLOT_OK)
                break;
        }
        if (k < (int)BANK_SLOTS)
            select_slot(s);
        return;
    }
    if (id < 0)
        return;
    if (PI[id].kind == PK_CONT)
        param_set(id, pv[id] + PI[id].step * (float)accel(e, (uint32_t)(EN_K1 + cell)), 0);
    else
        param_set(id, pv[id] + (float)(e > 0 ? 1 : -1), 0);
}

static void action_ok(void)              /* OCT+ on PRESETS / STORAGE: load, play */
{
    if (ui.page == SCR_PRESETS) {
        preset_load(ui.preset_cursor);
    } else if (ui.page == SCR_STORAGE) {
        if (ui.store_cursor != cur_slot)
            select_slot(ui.store_cursor);
        if (slot_state[ui.store_cursor] == SLOT_OK)
            listen_start();
    }
}
static void action_back(void)            /* OCT- on PRESETS / STORAGE: delete (asked twice) */
{
    uint32_t now = plat_ms();
    if (ui.page == SCR_PRESETS && ui.preset_cursor < (int)NUSER && pre.p[ui.preset_cursor].used) {
        if (ui.confirm == CONFIRM_DELETE_PRESET && now < ui.confirm_until) {
            ui.confirm = CONFIRM_NONE;
            preset_op(2, (uint32_t)ui.preset_cursor, "");
            app_say("PRESET", "DELETED");
        } else {
            ui.confirm = CONFIRM_DELETE_PRESET;
            ui.confirm_until = now + 3000u;
            app_say("DELETE PRESET?", "OCT- AGAIN");
        }
    } else if (ui.page == SCR_STORAGE && dir.slot[ui.store_cursor].used) {
        if (ui.confirm == CONFIRM_DELETE_SAMPLE && now < ui.confirm_until) {
            ui.confirm = CONFIRM_NONE;
            bank_erase((uint32_t)ui.store_cursor);
            dir_changed();
            app_say("SAMPLE", "DELETED");
        } else {
            ui.confirm = CONFIRM_DELETE_SAMPLE;
            ui.confirm_until = now + 3000u;
            app_say("DELETE SAMPLE?", "OCT- AGAIN");
        }
    }
    ui.d_field = 1;
}

static void save_button_tap(void)
{
    uint32_t now = plat_ms();
    int i = ui.preset_cursor;
    if (ui.page != SCR_PRESETS) {
        page_go(SCR_PRESETS);
        return;
    }
    if (i >= (int)NUSER) {
        app_say("FACTORY PRESET", "PICK A U SLOT");
        return;
    }
    if (pre.p[i].used && !(ui.confirm == CONFIRM_OVERWRITE && now < ui.confirm_until)) {
        ui.confirm = CONFIRM_OVERWRITE;
        ui.confirm_until = now + 3000u;
        app_say("OVERWRITE?", "SAVE AGAIN");
        return;
    }
    ui.confirm = CONFIRM_NONE;
    app_say(preset_store(i, "") == BANK_OK ? "SAVED" : "SAVE FAILED", pre.p[i].name);
    ui.d_field = 1;
}

static int held(uint32_t b) { return (int)((ui.btn >> b) & 1u); }

static void input(void)
{
    uint32_t btn = plat_buttons(), keys, k, now = plat_ms(), press, rel;
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    /* modifiers from this very input (a button and a key pressed in one scan count as a combination) */
#define HB(x) ((btn >> (x)) & 1u)
    int mod = HB(B_FX) || HB(B_EDIT) || HB(B_SAVE) || HB(B_SEL), notes_changed = 0;
    ef_ctl_t *c = ef_ctl();
    touch_poll();
    keys = touch_keys();
    for (k = 0; k < NB; k++)             /* buttons just down: fresh, before the keys may use them as modifiers */
        if (((btn & ~ui.btn) >> k) & 1u) {
            ui.down_at[k] = now;
            ui.used[k] = 0;
        }
    press = keys & ~ui.keys;
    rel = ui.keys & ~keys;
    /* ---- keys */
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        int white = -1, black = -1, w;
        for (w = 0; w < 16; w++)
            if (WK[w] == k)
                white = w;
        for (w = 0; w < 11; w++)
            if (BK[w] == k)
                black = w;
        if (press & bit) {
            if (HB(B_FX) && black >= 0) {
                c->punch |= 1u << black;
                ui.used[B_FX] = 1;
                ui.d_head = ui.d_field = 1;
                continue;
            }
            if (HB(B_EDIT) && white >= 0) {   /* a sound, heard as it is while the key is held */
                ui.used[B_EDIT] = 1;
                if (white < (int)BANK_SLOTS && slot_state[white] == SLOT_OK) {
                    if (white != cur_slot)
                        select_slot(white);
                    listen_key = (int8_t)k;
                    listen_start();
                } else
                    select_slot(white);  /* (says EMPTY SLOT / DAMAGED) */
                continue;
            }
            if (HB(B_SAVE)) {
                ui.used[B_SAVE] = 1;
                if (white >= 0)
                    preset_load(white);
                else if (black >= 0 && black < NFACTORY)
                    preset_load((int)NUSER + black);
                continue;
            }
            if (HB(B_SEL)) {
                ui.used[B_SEL] = 1;
                param_set(P_OR_ROOT, (float)((53u + k) % 12u), 0);
                param_set(P_OR_ON, 1.0f, 0);
                app_say("ORACLE ROOT", NOTE_NAME[(53u + k) % 12u]);
                continue;
            }
            if (mod)
                continue;
            ui.vel[k] = touch_vel(k);
            if ((int)pv[P_KEYMODE] == KEYMODE_LATCH) {
                ui.latched ^= bit;
                midi_note_out((ui.latched & bit) != 0, (uint32_t)(53 + (int32_t)k + 12 * ui.octave), ui.vel[k]);
            } else
                midi_note_out(1, (uint32_t)(53 + (int32_t)k + 12 * ui.octave), ui.vel[k]);
            note_new();
            notes_changed = 1;
        }
        if (rel & bit) {
            if ((int)k == listen_key) {
                listen_key = -1;
                listen_after = 0;
                ef_listen(0, 0);
                continue;
            }
            if (black >= 0 && ((c->punch >> black) & 1u)) {
                c->punch &= ~(1u << black);
                ui.d_head = ui.d_field = 1;
            }
            if ((int)pv[P_KEYMODE] != KEYMODE_LATCH)
                midi_note_out(0, (uint32_t)(53 + (int32_t)k + 12 * ui.octave), 0);
            notes_changed = 1;
        }
    }
    ui.keys = keys;
    if ((int)pv[P_KEYMODE] != KEYMODE_LATCH && ui.latched) {
        ui.latched = 0;
        notes_changed = 1;
    }
    if (notes_changed)
        notes_push();
    if (c->punch && !HB(B_FX)) {
        c->punch = 0;                    /* FX let go: every punch-in ends */
        ui.d_head = ui.d_field = 1;
    }

    /* ---- buttons */
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if ((btn & bit) && !(ui.btn & bit)) {   /* down */
            switch (k) {
            case B_PLAY:
                play_set(!c->play);
                break;
            case B_REC:
                rec_toggle();
                break;
            case B_SEQ:
                freeze_set(!c->freeze);
                app_say("FREEZE", c->freeze ? "ON" : "OFF");
                break;
            case B_OCTDN:
            case B_OCTUP:
                if (held(k == B_OCTDN ? B_OCTUP : B_OCTDN)) {
                    ui.used[B_OCTDN] = ui.used[B_OCTUP] = 1;
                    undo_do();
                }
                break;
            case B_ARP:
                ui.lock_done[0] = ui.lock_done[1] = ui.lock_done[2] = ui.lock_done[3] = 0;
                break;
            }
            if (k == B_FX)
                ui.d_field = 1;
        }
        if (!(btn & bit) && (ui.btn & bit)) {   /* up: a tap, unless it was held into a combination */
            uint32_t dur = now - ui.down_at[k];
            int tap = !ui.used[k] && dur < 600u;
            switch (k) {
            case B_HOME:
                if (ui.help) {
                    ui.help = 0;
                    ui.d_field = ui.d_knobs = 1;
                } else if (tap)
                    group_button(G_HOME);
                break;
            case B_ENV: if (tap) group_button(G_KEYS); break;
            case B_LFO: if (tap) group_button(G_GRAIN); break;
            case B_FX:
                if (tap)
                    group_button(G_FX);
                ui.d_field = 1;
                break;
            case B_SEL: if (tap) group_button(G_ORACLE); break;
            case B_EDIT:                 /* on the SAMPLE page: LISTEN on / off */
                if (tap && ui.page == 12) {
                    if (ef_listening())
                        ef_listen(0, 0);
                    else
                        listen_start();
                    ui.d_field = 1;
                } else if (tap)
                    group_button(G_SAMPLE);
                break;
            case B_GLO: if (tap) group_button(G_SYSTEM); break;
            case B_SAVE: if (tap) save_button_tap(); break;
            case B_ARP:
                if (tap)
                    random_do(0);
                break;
            case B_OCTDN:
                if (!ui.used[k]) {
                    if (ui.page == SCR_PRESETS || ui.page == SCR_STORAGE)
                        action_back();
                    else if (ui.octave > -2) {
                        char t[8] = "";
                        ui.octave--;
                        int_text(ui.octave, t);
                        app_say("OCTAVE", t);
                    }
                }
                break;
            case B_OCTUP:
                if (!ui.used[k]) {
                    if (ui.page == SCR_PRESETS || ui.page == SCR_STORAGE)
                        action_ok();
                    else if (ui.octave < 2) {
                        char t[8] = "+";
                        ui.octave++;
                        int_text(ui.octave, t + 1);
                        app_say("OCTAVE", t);
                    }
                }
                break;
            }
            ui.d_head = 1;
        }
    }
    /* holds */
    if ((btn >> B_HOME & 1u) && !ui.used[B_HOME] && now - ui.down_at[B_HOME] >= 500u && !ui.help) {
        ui.help = 1;
        ui.used[B_HOME] = 1;
        ui.d_field = ui.d_knobs = 1;
    }
    if ((btn >> B_ARP & 1u) && !ui.used[B_ARP] && now - ui.down_at[B_ARP] >= 700u) {
        ui.used[B_ARP] = 1;
        random_do(1);
    }
    ui.btn = btn;

    /* ---- encoders */
    for (k = 0; k < NE; k++) {
        int32_t e = plat_enc((int)k);
        if (!e)
            continue;
        if (ui.help) {
            ui.help = 0;
            ui.d_field = ui.d_knobs = 1;
        }
        switch (k) {
        case EN_SELECT:
            param_set(P_POSITION, pv[P_POSITION] + PI[P_POSITION].step * (float)accel((int)e, k), 0);
            ui.d_field = 1;
            break;
        case EN_ALGO:
            param_set(P_MODE, (float)(((int)pv[P_MODE] + (e > 0 ? 1 : MODE_COUNT - 1)) % MODE_COUNT), 0);
            app_say(MODE_NAME[(int)pv[P_MODE]], MODE_BLURB[(int)pv[P_MODE]]);
            break;
        case EN_PRESET: {
            char t[24];
            ui.preset_cursor = (ui.preset_cursor + (e > 0 ? 1 : -1) + preset_count()) % preset_count();
            ui.preset_turned_at = now;
            ui.preset_pending = 1;
            preset_label(ui.preset_cursor, t);
            app_say("PRESET", t);
            break;
        }
        default: {
            int cell = (int)k - EN_K1;
            if (held(B_ARP) && ui.page < NPAGES) {   /* ARP + a knob: its lock */
                int id = PAGES[ui.page].p[cell];
                ui.used[B_ARP] = 1;
                if (id >= 0 && !ui.lock_done[cell]) {
                    ui.lock_done[cell] = 1;
                    lock_toggle(id);
                    app_say(PI[id].label, locked(id) ? "LOCKED" : "UNLOCKED");
                    ui.d_knobs = 1;
                    ses_dirty = 1;
                }
            } else
                knob_turn(cell, (int)e);
        }
        }
    }
    if (ui.preset_pending && now - ui.preset_turned_at > 350u) {
        ui.preset_pending = 0;
        preset_load(ui.preset_cursor);
    }
    midi_in();
}

void app_input_only(void) { input(); }

/* ================================================================ lights */
static void lights(void)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    static const uint8_t GROUP_BTN[G_COUNT] = {B_HOME, B_ENV, B_LFO, B_FX, B_SEL, B_EDIT, B_GLO, B_SAVE};
    ef_ctl_t *c = ef_ctl();
    uint32_t b = 0, k = ui.keys | ui.latched, db = 0, dk = 0, i;
    b |= 1u << GROUP_BTN[group_of(ui.page)];
    if (c->play)
        b |= LED_PLAY_GREEN;
    if (ef_rec_on())
        b |= 1u << B_REC;
    if (c->freeze)
        b |= 1u << B_SEQ;
    if (held(B_ARP))
        b |= 1u << B_ARP;
    if (held(B_EDIT))
        for (i = 0; i < 16u; i++) {
            if ((int)i == cur_slot)
                k |= 1u << WK[i];
            else if (slot_state[i] == SLOT_OK)
                dk |= 1u << WK[i];
        }
    if (held(B_SAVE)) {
        for (i = 0; i < 16u; i++)
            if (pre.p[i].used)
                dk |= 1u << WK[i];
        for (i = 0; i < 11u && i < (uint32_t)NFACTORY; i++)
            dk |= 1u << BK[i];
    }
    if (held(B_FX))
        for (i = 0; i < 11u; i++) {
            if ((c->punch >> i) & 1u)
                k |= 1u << BK[i];
            else
                dk |= 1u << BK[i];
        }
    if (ef_ctl()->dbg_bypass & 16u) {    /* (diagnostics: every light off, no dim pulses) */
        plat_lights(0, 0, 0, 0, 0);
        return;
    }
    plat_lights(b, k, db, dk, 1);
}

/* ================================================================ drawing */
static void band(int32_t y0, int32_t h)  /* a canvas for rows y0 .. y0+h-1, drawn in screen coordinates */
{
    cv_begin(240, (uint32_t)h, col_bg);
    cv_oy = -y0;
    cv_y0 = 0;
    cv_y1 = h;
}
static void band_end(int32_t y0) { cv_blit(0, (uint32_t)y0); }

static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)   /* centred */
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}
static void text_r(int32_t rx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)   /* right-aligned */
{
    cv_text(rx - text_w(f, s), y, f, s, c);
}

static float dial_c[64], dial_s[64];     /* 270 degrees of a dial, from bottom left, clockwise */
static void dial_init(void)
{
    uint32_t i;
    for (i = 0; i < 64u; i++) {
        float a = (0.625f - 0.75f * (float)i / 63.0f);   /* turns: 225 degrees down to -45 */
        dial_c[i] = fm_sin_turns(a + 0.25f);
        dial_s[i] = -fm_sin_turns(a);
    }
}
static void dial(int32_t cx, int32_t cy, float v0, float v1, uint16_t col, uint16_t bgc)
{
    uint32_t i, a = (uint32_t)(fm_clampf(fm_minf(v0, v1), 0.0f, 1.0f) * 63.0f + 0.5f),
                b = (uint32_t)(fm_clampf(fm_maxf(v0, v1), 0.0f, 1.0f) * 63.0f + 0.5f);
    int32_t r;
    for (i = 0; i < 64u; i++)
        for (r = 12; r <= 14; r++) {
            uint16_t c = i >= a && i <= b ? col : bgc;
            cv_pset(cx + (int32_t)(dial_c[i] * (float)r), cy + (int32_t)(dial_s[i] * (float)r), c);
        }
    i = (uint32_t)(fm_clampf(v1, 0.0f, 1.0f) * 63.0f + 0.5f);   /* the pointer */
    cv_line(cx + (int32_t)(dial_c[i] * 5.0f), cy + (int32_t)(dial_s[i] * 5.0f), cx + (int32_t)(dial_c[i] * 10.0f),
            cy + (int32_t)(dial_s[i] * 10.0f), col);
}

static void lock_icon(int32_t x, int32_t y, uint16_t c)
{
    cv_rect(x, y + 3, 7, 5, c);
    cv_rect(x + 1, y, 1, 3, c);
    cv_rect(x + 5, y, 1, 3, c);
    cv_rect(x + 2, y, 3, 1, c);
}

#define HEAD_H 20
#define FIELD_Y 20
#define FIELD_H 144
#define KNOB_Y 164
#define KNOB_H 76
#define MID_Y (FIELD_Y + 72)

static void draw_head(void)
{
    ef_ctl_t *c = ef_ctl();
    int mode = (int)pv[P_MODE], idx = 0, n = 0, i;
    int32_t x;
    const char *name = ui.page < NPAGES ? PAGES[ui.page].name : ui.page == SCR_PRESETS ? "PRESETS"
                       : ui.page == SCR_SYSTEM ? "SYSTEM" : "STORAGE";
    band(0, HEAD_H);
    x = cv_text(6, 2, &FONT_B, name, col_text);
    if (ui.page < NPAGES)
        n = group_pages(PAGES[ui.page].group, &idx, ui.page);
    else if (ui.page == SCR_SYSTEM || ui.page == SCR_STORAGE) {
        n = 2;
        idx = ui.page == SCR_STORAGE;
    }
    for (i = 0; i < n && n > 1; i++)     /* the group's pages as dots */
        cv_rect(x + 6 + i * 7, 9, 4, 4, i == idx ? col_text : col_dim);
    text_r(234, 2, &FONT_B, MODE_NAME[mode], col_mode[mode]);
    x = 234 - text_w(&FONT_B, MODE_NAME[mode]) - 8;
    if (ef_rec_on()) {
        char t[8];
        int_text((int32_t)((plat_ms() - ui.rec_t0) / 1000u), t);
        text_cat(t, "S");
        cv_rect(x - 8, 7, 6, 6, col_rec);
        text_r(x - 12, 4, &FONT_XS, t, col_rec);
        x -= 12 + text_w(&FONT_XS, t) + 6;
    }
    if (c->freeze) {
        text_r(x, 4, &FONT_XS, "FRZ", col_k[0]);
        x -= text_w(&FONT_XS, "FRZ") + 6;
    }
    if (ses_dirty) {
        cv_rect(x - 4, 9, 3, 3, col_dim);  /* not saved yet */
        x -= 9;
    }
    (void)x;
    cv_rect(0, HEAD_H - 1, 240, 1, col_line);
    band_end(0);
}

/* the grain field: the sample's outline, the region, the playhead and its spread, every grain */
static void draw_field_grains(int big)
{
    const ef_vis_t *v = ef_vis();
    const ef_meter_t *m = ef_meter();
    int mode = (int)pv[P_MODE];
    uint16_t mc = col_mode[mode];
    int32_t mid = big ? FIELD_Y + 78 : MID_Y, amp = big ? 52 : 40, i;
    float ra = fm_minf(pv[P_START], pv[P_END]), rb = fm_maxf(pv[P_START], pv[P_END]);
    int32_t xa = (int32_t)(ra * 239.0f), xb = (int32_t)(rb * 239.0f), cx = (int32_t)m->center * 239 / 255,
            sw = (int32_t)m->spread * 240 / 255;
    /* the spread around the playhead */
    if (sw > 0)
        cv_rect(cx - sw / 2, mid - amp - 4, sw < 2 ? 2 : sw, 2 * amp + 8, mix565(col_bg, mc, 40));
    /* the outline, brighter inside the region */
    for (i = 0; i < 240; i++) {
        int32_t lo = ov_lo[i] * amp / 64, hi = ov_hi[i] * amp / 64;
        uint16_t c = i >= xa && i <= xb ? col_gray : col_line;
        if (hi - lo < 1)
            hi = lo + 1;
        cv_rect(i, mid - hi, 1, hi - lo, c);
    }
    if (big) {                           /* SAMPLE: the region's brackets */
        cv_rect(xa, mid - amp - 8, 2, 2 * amp + 16, col_k[1]);
        cv_rect(xa, mid - amp - 8, 6, 2, col_k[1]);
        cv_rect(xa, mid + amp + 6, 6, 2, col_k[1]);
        cv_rect(xb - 1, mid - amp - 8, 2, 2 * amp + 16, col_k[2]);
        cv_rect(xb - 5, mid - amp - 8, 6, 2, col_k[2]);
        cv_rect(xb - 5, mid + amp + 6, 6, 2, col_k[2]);
    }
    /* the playhead; LISTEN's, white, across it */
    cv_rect(cx, mid - amp - 6, 1, 2 * amp + 12, mc);
    if (m->aud)
        cv_rect((int32_t)(m->aud - 1u) * 239 / 254 - 1, mid - amp - 6, 3, 2 * amp + 12, col_text);
    if (mode == MODE_TAPE && m->head != 255u) {   /* the three heads */
        int32_t k;
        for (k = 0; k < 3; k++) {
            float f = ((float)m->head / 255.0f - ra) / fm_maxf(0.001f, rb - ra) + (float)k / 3.0f;
            int32_t hx;
            f -= fm_floorf(f);
            hx = xa + (int32_t)(f * (float)(xb - xa));
            cv_rect(hx - 2, mid + amp + 2, 5, 3, k == 0 ? col_text : col_dim);
        }
    }
    /* grains */
    for (i = 0; i < (int32_t)v->n && i < (int32_t)EF_VIS_N; i++) {
        int32_t gx = (int32_t)v->x[i] * 239 / 255, gy = mid - (int32_t)v->pitch[i] * (big ? 2 : 3) / 2,
                r = 1 + (int32_t)v->amp[i] / 80;
        uint32_t pan = v->pan[i];
        uint16_t c = pan < 128u ? mix565(col_k[0], col_text, pan * 2u) : mix565(col_text, col_k[3], (pan - 128u) * 2u);
        if (gy < FIELD_Y + 4)
            gy = FIELD_Y + 4;
        if (gy > FIELD_Y + FIELD_H - 6)
            gy = FIELD_Y + FIELD_H - 6;
        cv_rect(gx - r, gy - r, 2 * r + 1, 2 * r + 1, c);
    }
    /* PULSE: the sixteen steps */
    if (mode == MODE_PULSE) {
        for (i = 0; i < 16; i++) {
            int on = (m->pulse_pat >> i) & 1u;
            uint16_t c = i == m->pulse_step ? col_text : on ? mc : col_line;
            cv_rect(8 + i * 14, FIELD_Y + 6, 10, on ? 6 : 2, c);
        }
    }
}

static void draw_oracle_strip(void)      /* the oracle: its scale and chord on twelve notes, its last event */
{
    const ef_meter_t *m = ef_meter();
    int sc = (int)pv[P_OR_SCALE], root = (int)pv[P_OR_ROOT], i;
    static const uint8_t DEG[12][7] = {{0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10},
                                       {0, 1, 3, 5, 7, 8, 10}, {0, 2, 4, 6, 7, 9, 11}, {0, 2, 4, 5, 7, 9, 10},
                                       {0, 2, 4, 7, 9, 99, 99}, {0, 3, 5, 7, 10, 99, 99}, {0, 2, 3, 7, 8, 99, 99},
                                       {0, 1, 5, 7, 10, 99, 99}, {0, 2, 4, 6, 8, 10, 99}, {0, 2, 4, 7, 9, 11, 99}};
    int32_t y = FIELD_Y + FIELD_H - 26;
    for (i = 0; i < 12; i++) {
        int in = 0, k, n = (i - root + 12) % 12;
        for (k = 0; k < 7; k++)
            if (DEG[sc][k] == n)
                in = 1;
        cv_rect(6 + i * 15, y, 13, 10, !pv[P_OR_ON] ? col_line : (m->oracle_note % 12 + 12) % 12 == i ? col_k[1]
                                                       : in ? col_dim : col_line);
        if (i == root)
            cv_rect(6 + i * 15, y + 12, 13, 2, col_k[1]);
    }
    text_r(234, y - 2, &FONT_XS, pv[P_OR_ON] ? m->oracle_name : "OFF", col_gray);
}

static void draw_fx(void)                /* an OP-1-ish picture of the effect page */
{
    int32_t i, x0 = 16, w = 208, top = FIELD_Y + 16, bot = FIELD_Y + FIELD_H - 16, h = bot - top;
    const char *nm = PAGES[ui.page].name;
    uint16_t a = col_k[0], b = col_k[3];
    cv_rect(x0, bot, w, 1, col_line);
    if (nm[0] == 'F') {                  /* FILTER: the response, log frequency */
        int ft = (int)pv[P_FILTER_TYPE];
        float fc = 30.0f * fm_powf(600.0f, pv[P_CUTOFF]), q = 1.0f / (2.0f - 1.94f * pv[P_RESO]);
        int32_t px = -1, py = 0;
        for (i = 0; i < w; i += 2) {
            float f = 20.0f * fm_powf(1000.0f, (float)i / (float)w), r = f / fc, re = 1.0f - r * r, im = r / q, mag2,
                  num = ft == 0 ? 1.0f : ft == 1 ? r * r / (q * q) : r * r * r * r;
            int32_t y;
            mag2 = num / (re * re + im * im);
            y = bot - (int32_t)(fm_clampf((10.0f * fm_log2f(mag2 + 1e-9f) * 0.30103f + 36.0f) / 48.0f, 0.0f, 1.0f) * (float)h);
            if (px >= 0)
                cv_line(x0 + px, py, x0 + i, y, a);
            px = i;
            py = y;
        }
        cv_rect(x0 + (int32_t)(fm_log2f(fc / 20.0f) / fm_log2f(1000.0f) * (float)w), top, 1, h, b);
    } else if (nm[0] == 'D') {           /* DELAY: the echoes, fading, left and right */
        float t = pv[P_DLY_TIME], g = 1.0f;
        int k;
        for (k = 0; k < 12; k++) {
            int32_t x = x0 + (int32_t)((float)k * t / 1000.0f * 0.5f * (float)w / 1.5f);
            int32_t hh = (int32_t)(g * (float)h * (k ? pv[P_DLY_MIX] * 0.8f + 0.2f : 1.0f));
            if (x >= x0 + w)
                break;
            cv_rect(x, bot - hh, 5, hh, k == 0 ? col_text : (pv[P_DLY_PINGPONG] > 0.5f && (k & 1)) ? b : a);
            g *= pv[P_DLY_FB];
        }
    } else if (nm[0] == 'R') {           /* REVERB: the tail, the shimmer above it */
        float t60 = 0.6f * fm_powf(24.0f, pv[P_REV_SIZE]), damp = pv[P_REV_DAMP];
        for (i = 0; i < w; i += 2) {
            float tt = (float)i / (float)w * 8.0f, e = fm_exp2f(-tt * 9.97f / t60) * pv[P_SPACE];
            int32_t hh = (int32_t)(e * (float)h);
            cv_rect(x0 + i, bot - hh, 2, hh, mix565(a, col_line, (uint32_t)(fm_clampf(damp * tt * 40.0f, 0.0f, 200.0f))));
            if (pv[P_SHIMMER] > 0.05f && ((i * 37 + (int32_t)ui.frame) % 23) == 0)
                cv_rect(x0 + i, bot - hh - 4 - (i * 13 % 20), 2, 2, col_text);
        }
    } else if (nm[0] == 'T') {           /* TAPE: two reels, the tape between, wobbling */
        float ph = (float)ui.frame * 0.05f, wob = pv[P_TAPE_WOW] * 6.0f;
        int r, k;
        for (k = 0; k < 2; k++) {
            int32_t cx = k ? 172 : 68, cy = FIELD_Y + 60;
            for (r = 0; r < 64; r += 1) {
                float s = fm_sin_turns((float)r / 64.0f), cc = fm_sin_turns((float)r / 64.0f + 0.25f);
                cv_pset(cx + (int32_t)(cc * 34.0f), cy + (int32_t)(s * 34.0f), col_gray);
                cv_pset(cx + (int32_t)(cc * 10.0f), cy + (int32_t)(s * 10.0f), col_dim);
            }
            for (r = 0; r < 3; r++) {
                float an = ph * (k ? 1.0f : 1.1f) + (float)r / 3.0f;
                cv_line(cx, cy, cx + (int32_t)(fm_sin_turns(an + 0.25f) * 30.0f), cy + (int32_t)(fm_sin_turns(an) * 30.0f), b);
            }
        }
        for (i = 68; i < 172; i += 2)
            cv_rect(i, FIELD_Y + 100 + (int32_t)(fm_sin_turns((float)i / 40.0f + ph) * wob), 2, 2, a);
        for (i = 0; i < (int32_t)(pv[P_TAPE_AGE] * 30.0f); i++)
            cv_pset(20 + (i * 73 + (int32_t)ui.frame * 7) % 200, FIELD_Y + 20 + (i * 41) % 110, col_dim);
    } else {                             /* OUTPUT: the stereo image and the level */
        const ef_meter_t *m = ef_meter();
        float wd = pv[P_WIDTH], lv = pv[P_LEVEL];
        int32_t cx = 120, cy = FIELD_Y + 66, rx = 20 + (int32_t)(wd * 80.0f), ry = 36;
        int k;
        for (k = 0; k < 64; k++)
            cv_pset(cx + (int32_t)(fm_sin_turns((float)k / 64.0f + 0.25f) * (float)rx),
                    cy + (int32_t)(fm_sin_turns((float)k / 64.0f) * (float)ry), a);
        cv_rect(x0, bot - 6, (int32_t)(lv * (float)w), 3, b);
        cv_rect(x0, bot - 12, (int32_t)(fm_minf(1.0f, m->peak) * (float)w), 3, m->limiting ? col_rec : col_text);
    }
}

static void draw_presets(void)
{
    int first = ui.preset_cursor - 3, i;
    if (first > preset_count() - 7)
        first = preset_count() - 7;
    if (first < 0)
        first = 0;
    for (i = 0; i < 7; i++) {
        int p = first + i, y = FIELD_Y + 4 + i * 17;
        char t[24];
        uint16_t c;
        if (p >= preset_count())
            break;
        preset_label(p, t);
        c = p < (int)NUSER ? (pre.p[p].used ? col_text : col_dim) : col_gray;
        if (p == ui.preset_cursor) {
            cv_rect(2, y - 1, 236, 17, mix565(col_bg, col_k[0], 70));
            c = col_text;
        }
        cv_text(8, y, &FONT_S, t, c);
    }
    cv_text(8, FIELD_Y + FIELD_H - 4 - 15, &FONT_XS, "K1 PICK  OCT+ LOAD  SAVE STORE  OCT- DEL", col_dim);
}

static void draw_storage(void)           /* the bank's sectors by sample, and the slot under the cursor */
{
    uint32_t i, s;
    int32_t y0 = FIELD_Y + 6;
    uint32_t free_s = bank_free_secs(-1);
    char t[32];
    for (i = 0; i < BANK_NSEC; i++) {
        uint16_t c = col_line;
        for (s = 0; s < BANK_SLOTS; s++)
            if (dir.slot[s].used && map_bit(dir.slot[s].map, i))
                c = slot_state[s] != SLOT_OK ? col_rec : (int)s == ui.store_cursor ? col_text
                                                 : mix565(col_k[s & 3u], col_bg, 40u + 40u * ((s >> 2) & 1u));
        cv_rect(6 + (int32_t)(i % 23u) * 10, y0 + (int32_t)(i / 23u) * 10, 8, 8, c);
    }
    s = (uint32_t)ui.store_cursor;
    t[0] = (char)('0' + (s + 1u) / 10u);
    t[1] = (char)('0' + (s + 1u) % 10u);
    t[2] = ' ';
    t[3] = 0;
    text_cat(t, dir.slot[s].used ? dir.slot[s].name : "EMPTY");
    cv_text(8, y0 + 46, &FONT_B, t, (int)s == cur_slot ? col_k[3] : col_text);
    if (dir.slot[s].used) {
        char a[16];
        t[0] = 0;
        dec1_text((float)dir.slot[s].frames / (float)dir.slot[s].rate, a);
        text_cat(t, a);
        text_cat(t, "S  ");
        int_text((int32_t)(dir.slot[s].rate / 1000u), a);
        text_cat(t, a);
        text_cat(t, "K  ");
        int_text((int32_t)map_count(dir.slot[s].map) * 4, a);
        text_cat(t, a);
        text_cat(t, "KB");
        if (slot_state[s] == SLOT_DAMAGED)
            text_cat(t, "  DAMAGED");
        cv_text(8, y0 + 66, &FONT_XS, t, slot_state[s] == SLOT_DAMAGED ? col_rec : col_gray);
    }
    t[0] = 0;
    int_text((int32_t)(free_s * 4u), t);
    text_cat(t, " KB FREE  ");
    {
        char a[12];
        dec1_text((float)(free_s * EF_SEC_SAMPLES) / 22050.0f, a);
        text_cat(t, a);
        text_cat(t, "S AT 22K");
    }
    cv_text(8, y0 + 84, &FONT_XS, t, col_gray);
    cv_text(8, FIELD_Y + FIELD_H - 19, &FONT_XS, "K1 PICK  OCT+ LISTEN  OCT- DELETE", col_dim);
}

static void draw_system(void)
{
    char t[32];
    uint32_t k = 0, s;
    for (s = 0; s < BANK_SLOTS; s++)
        k += slot_state[s] == SLOT_OK;
    cv_text(8, FIELD_Y + 6, &FONT_M, "ECHOFIELD", col_text);
    cv_text(8, FIELD_Y + 32, &FONT_XS, "FM-1 GRANULAR SAMPLER  " APP_VERSION, col_gray);
    t[0] = 0;
    int_text((int32_t)k, t);
    text_cat(t, " SAMPLES   ");
    {
        char a[8];
        int_text((int32_t)(bank_free_secs(-1) * 4u), a);
        text_cat(t, a);
    }
    text_cat(t, " KB FREE");
    cv_text(8, FIELD_Y + 56, &FONT_XS, t, col_gray);
    t[0] = 'C';
    t[1] = 'P';
    t[2] = 'U';
    t[3] = ' ';
    t[4] = 0;
    int_text((int32_t)plat_cpu_pct(), t + 4);
    text_cat(t, "%   GRAINS ");
    {
        char a[8];
        int_text((int32_t)ef_meter()->grains, a);
        text_cat(t, a);
        text_cat(t, "/");
        int_text((int32_t)(set.voices - ef_ctl()->shed), a);
        text_cat(t, a);
    }
    cv_text(8, FIELD_Y + 74, &FONT_XS, t, col_gray);
    {
        int st = app_cpu2_state();
        cv_text(8, FIELD_Y + 90, &FONT_XS, st == 1 ? "2 CORES" : st ? "1 CORE (2ND GIVEN UP)" : "1 CORE", st == 1 ? col_k[1] : col_gray);
    }
    cv_text(8, FIELD_Y + 112, &FONT_XS, "GLO AGAIN: STORAGE", col_dim);
}

static const char *const HELP[NPAGES] = {
    "TEXTURE grain size  SCATTER spread|DRIFT wander  EVOLVE self-motion",
    "PITCH semitones  HARMONY octaves|REVERB amount  TAPE wear",
    "ATTACK / RELEASE of the keys|KEYS gate or latch  VELOCITY",
    "SIZE / DENSITY multipliers|ENVELOPE grain shape  TONE dark-airy",
    "SCAN auto-move  REVERSE chance|PITCH RND / SIZE RND per grain",
    "FILTER type  CUTOFF  RESO|DRIVE saturation after it",
    "TIME  FEEDBACK  ECHO amount|PINGPONG left-right",
    "REVERB amount  SIZE decay|DAMP highs  SHIMMER octave up",
    "WOW slow  FLUTTER fast|AGE wear  HISS noise",
    "WIDTH stereo  LEVEL output|",
    "ORACLE on  ROOT  SCALE|ACTIVITY how often it moves",
    "AMBIGUITY colour notes  RANGE|GLIDE  DRONE root+fifth bed",
    "K1 sample  START  END|POSITION   EDIT: LISTEN (as it is)",
};

static void draw_help(void)
{
    static const char *const L[] = {"SELECT position  ALGO mode  PRESETS",
                                    "PLAY drone  REC resample  SEQ freeze",
                                    "ARP random (hold: all)  ARP+knob lock",
                                    "OCT- + OCT+ undo   keys play notes",
                                    "FX+black key: effect  EDIT+white: sample",
                                    "SAVE+white: preset  SEL+key: root"};
    uint32_t i;
    cv_rect(0, FIELD_Y, 240, FIELD_H, col_bg);
    if (ui.page < NPAGES) {
        char a[48], b[48];
        const char *h = HELP[ui.page];
        uint32_t k = 0, j = 0;
        while (h[k] && h[k] != '|' && k < 47u) {
            a[k] = h[k];
            k++;
        }
        a[k] = 0;
        if (h[k] == '|')
            k++;
        while (h[k] && j < 47u)
            b[j++] = h[k++];
        b[j] = 0;
        cv_text(6, FIELD_Y + 4, &FONT_XS, a, col_k[0]);
        cv_text(6, FIELD_Y + 20, &FONT_XS, b, col_k[1]);
    }
    for (i = 0; i < sizeof L / sizeof L[0]; i++)
        cv_text(6, FIELD_Y + 44 + (int32_t)i * 16, &FONT_XS, L[i], col_gray);
}

static void draw_msg(void)
{
    int32_t w = text_w(&FONT_B, ui.msg[0]), w2 = text_w(&FONT_XS, ui.msg[1]);
    if (w2 > w)
        w = w2;
    w += 20;
    if (w > 236)
        w = 236;
    cv_rect(120 - w / 2, MID_Y - 22, w, ui.msg[1][0] ? 42 : 26, col_bg);
    cv_rect(120 - w / 2, MID_Y - 22, w, 1, col_dim);
    cv_rect(120 - w / 2, MID_Y + (ui.msg[1][0] ? 19 : 3), w, 1, col_dim);
    text_c(120, MID_Y - 18, &FONT_B, ui.msg[0], col_text);
    if (ui.msg[1][0])
        text_c(120, MID_Y + 1, &FONT_XS, ui.msg[1], col_k[3]);
}

static void draw_punch_map(void)         /* FX held: the black keys' effects */
{
    static const char *const P[PUNCH_COUNT] = {"FRZ", "REV", "OCT+", "OCT-", "STUT", "HUSH", "BLOOM", "SHIM", "STOP",
                                                "SWEEP", "THROW"};
    uint32_t i, punch = ef_ctl()->punch;
    cv_rect(0, FIELD_Y + FIELD_H - 40, 240, 40, col_bg);
    for (i = 0; i < PUNCH_COUNT; i++) {
        int32_t x = 4 + (int32_t)(i % 6u) * 39, y = FIELD_Y + FIELD_H - 38 + (int32_t)(i / 6u) * 18;
        int on = (punch >> i) & 1u;
        cv_rect(x, y, 37, 16, on ? col_k[3] : col_line);
        text_c(x + 18, y + 1, &FONT_T, P[i], on ? col_bg : col_gray);
    }
}

static void draw_field_content(void)
{
    if (ui.help) {
        draw_help();
        return;
    }
    if (ui.page == SCR_PRESETS)
        draw_presets();
    else if (ui.page == SCR_STORAGE)
        draw_storage();
    else if (ui.page == SCR_SYSTEM)
        draw_system();
    else if (PAGES[ui.page].group == G_FX)
        draw_fx();
    else {
        draw_field_grains(ui.page == 12);
        if (PAGES[ui.page].group == G_ORACLE)
            draw_oracle_strip();
        if (ui.page == 12) {
            char t[40], a[12];
            t[0] = 0;
            if (cur_slot >= 0) {
                t[0] = (char)('0' + (cur_slot + 1) / 10);
                t[1] = (char)('0' + (cur_slot + 1) % 10);
                t[2] = ' ';
                t[3] = 0;
                text_cat(t, dir.slot[cur_slot].name);
            } else
                text_cat(t, cur_slot == -1 ? "SEED (BANK EMPTY)" : "UNSAVED TAKE");
            cv_text(6, FIELD_Y + 4, &FONT_XS, t, col_text);
            if (cur_slot >= 0) {
                dec1_text((float)dir.slot[cur_slot].frames / (float)dir.slot[cur_slot].rate, a);
                text_cat(a, "S");
                text_r(234, FIELD_Y + 4, &FONT_XS, a, col_gray);
            }
            if (ef_meter()->aud)
                text_c(120, FIELD_Y + FIELD_H - 16, &FONT_XS, "LISTENING  -  EDIT: STOP", col_text);
            else
                text_c(120, FIELD_Y + FIELD_H - 16, &FONT_XS, "EDIT: LISTEN  -  EDIT + KEY: ANOTHER", col_dim);
        } else if (ui.page < NPAGES && PAGES[ui.page].group == G_HOME) {
            char t[24];
            t[0] = 0;
            if (cur_slot >= 0)
                text_cat(t, dir.slot[cur_slot].name);
            else
                text_cat(t, cur_slot == -1 ? "SEED" : "TAKE");
            cv_text(6, FIELD_Y + FIELD_H - 18, &FONT_XS, t, col_dim);
            if (!ef_ctl()->play && !ef_ctl()->nnotes && ef_meter()->env < 0.01f)
                text_r(234, FIELD_Y + FIELD_H - 18, &FONT_XS, "PLAY OR KEYS", col_dim);
        }
    }
    if (held(B_FX))
        draw_punch_map();
    if (ui.msg_until && plat_ms() < ui.msg_until)
        draw_msg();
}

static void draw_field(void)
{
    band(FIELD_Y, 72);
    draw_field_content();
    band_end(FIELD_Y);
    band(FIELD_Y + 72, 72);
    draw_field_content();
    band_end(FIELD_Y + 72);
}

static void draw_knobs(void)
{
    int k;
    band(KNOB_Y, KNOB_H);
    cv_rect(0, KNOB_Y, 240, 1, col_line);
    for (k = 0; k < 4; k++) {
        int32_t x0 = k * 60, cx = x0 + 30, cy = KNOB_Y + 20;
        int lit = ui.touched == k && plat_ms() - ui.turned_at[k] < 1500u;
        char val[16], lab[16];
        float v0 = 0.0f, v1 = 0.0f;
        int id = -1, have = 1;
        val[0] = lab[0] = 0;
        if (ui.page == SCR_SYSTEM) {
            static const char *const L[4] = {"THEME", "MIDI CH", "GRAINS", "OUTPUT"};
            text_cpy(lab, L[k], 16);
            if (k == 0)
                text_cpy(val, THEME_NAME[set.theme], 16), v1 = (float)set.theme / (float)(NTHEMES - 1);
            else if (k == 1) {
                if (set.midi_ch)
                    int_text(set.midi_ch, val);
                else
                    text_cpy(val, "OMNI", 16);
                v1 = (float)set.midi_ch / 16.0f;
            } else if (k == 2)
                int_text(set.voices, val), v1 = (float)(set.voices - 8u) / (float)(EF_MAX_GRAINS - 8u);
            else
            {
                int e = 0;
                int_text((int32_t)set.out_level * 10, val);
                while (val[e])                   /* (no strlen on the FM-1) */
                    e++;
                val[e] = '%', val[e + 1] = 0;
                v1 = (float)(set.out_level - 5u) / 5.0f;
            }
        } else if (ui.page == SCR_PRESETS || ui.page == SCR_STORAGE) {
            have = k == 0;
            text_cpy(lab, k == 0 ? (ui.page == SCR_PRESETS ? "PRESET" : "SLOT") : "", 16);
            if (k == 0) {
                int n = ui.page == SCR_PRESETS ? preset_count() : (int)BANK_SLOTS,
                    i = ui.page == SCR_PRESETS ? ui.preset_cursor : ui.store_cursor;
                int_text(i + 1, val);
                v1 = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
            }
        } else {
            id = PAGES[ui.page].p[k];
            if (id == -2) {
                text_cpy(lab, "SAMPLE", 16);
                if (cur_slot >= 0)
                    int_text(cur_slot + 1, val);
                else
                    text_cpy(val, cur_slot == -1 ? "SEED" : "TAKE", 16);
                v1 = cur_slot >= 0 ? (float)cur_slot / 15.0f : 0.0f;
            } else if (id >= 0) {
                text_cpy(lab, PI[id].label, 16);
                pformat(id, pv[id], val);
                v1 = pnorm(id, pv[id]);
                if (PI[id].min < 0.0f && PI[id].max > 0.0f)
                    v0 = pnorm(id, 0.0f);   /* bipolar: from the middle */
            } else
                have = 0;
        }
        if (!have) {
            cv_rect(cx - 6, cy, 12, 1, col_line);
            continue;
        }
        dial(cx, cy, v0, v1, col_k[k], col_line);
        if (id >= 0 && locked(id))
            lock_icon(x0 + 48, KNOB_Y + 4, col_gray);
        text_c(cx, KNOB_Y + 38, &FONT_T, lab, lit ? col_k[k] : col_gray);
        text_c(cx, KNOB_Y + 52, &FONT_S, val, lit ? col_text : mix565(col_text, col_gray, 120));
    }
    band_end(KNOB_Y);
}

void app_redraw(void) { ui.d_head = ui.d_field = ui.d_knobs = 1; }

void app_splash(void)
{
    band(0, 80);
    band_end(0);
    band(80, 80);
    {
        int k;
        theme_apply();
        for (k = 0; k < 4; k++)
            cv_rect(72 + k * 26, 84, 18, 18, col_k[k]);
        text_c(120, 110, &FONT_L, "ECHOFIELD", col_text);
    }
    band_end(80);
    band(160, 80);
    text_c(120, 166, &FONT_XS, "GRANULAR SAMPLER FOR FM-1", col_gray);
    band_end(160);
}

/* ================================================================ the frame */
void app_ui_init(void)
{
    touch_on(1);
    theme_apply();
    dial_init();
    lcd_fill(0, 0, 240, 240, col_bg);
    ui.d_head = ui.d_field = ui.d_knobs = 1;
}

void app_overload(void)                  /* audio interrupt: the render took over 90 % of its time */
{
    ef_ctl_t *c = ef_ctl();
    if (c->shed + 6u < c->max_grains)
        c->shed += 2u;
}

static void housekeeping(void)
{
    uint32_t now = plat_ms();
    ef_ctl_t *c = ef_ctl();
    {                                    /* (diagnostics: the keys' velocity sensing off with bit 32) */
        static uint8_t sense = 1;
        uint8_t want = !(c->dbg_bypass & 32u);
        if (want != sense) {
            sense = want;
            touch_on(want);
        }
    }
    /* the overload guard gives grains back slowly; and sheds more while the load stays high */
    if (now - ui.shed_at > 1000u) {
        uint32_t pct = plat_cpu_pct();
        ui.shed_at = now;
        if (pct > 85u && c->shed + 6u < c->max_grains)
            c->shed++;
        else if (pct < 65u && c->shed)
            c->shed--;
    }
    if (pending_slot != -100 && ef_source_idle() && !quiet_depth) {
        int s = pending_slot;
        pending_slot = -100;
        select_now(s);
        if (listen_after) {
            listen_after = 0;
            ef_listen(1, 0);
        }
    }
    /* the session: saved while nothing sounds (a flash write stops the audio a moment), 3 s after the last change */
    if (c->play || c->nnotes || ef_rec_on())
        ui.sound_at = now;
    if (ses_dirty && !backup_hold && now - ses_changed_at > 3000u && now - ui.sound_at > 1500u &&
        ef_meter()->env < 0.001f && (ui.peak_q < 0.001f || now - ui.sound_at > 10000u))
        session_save();
    if (ef_rec_on() && (ui.frame & 15u) == 0)
        ui.d_head = 1;
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.d_field = 1;
    }
    if (ui.touched >= 0 && now - ui.turned_at[ui.touched] > 1500u) {
        ui.touched = -1;
        ui.d_knobs = 1;
    }
    if (ui.confirm && now >= ui.confirm_until)
        ui.confirm = CONFIRM_NONE;
}

void app_frame(void)
{
    input();
    housekeeping();
    lights();
    ui.frame++;
    if (ui.d_head) {
        ui.d_head = 0;
        draw_head();
    }
    if (ui.d_field || (ui.frame & 1u)) { /* the field moves: about 30 a second */
        int live = ui.page < NPAGES || ui.page == SCR_SYSTEM;
        if (ui.d_field || live) {
            ui.d_field = 0;
            draw_field();
        }
    }
    if (ui.d_knobs) {
        ui.d_knobs = 0;
        draw_knobs();
    }
    ui.peak_q = ef_meter()->peak;
    ef_meter_peak_reset();
    ann_tick(plat_ms());
}

/* ================================================================ the browser emulator */
#if defined(__EMSCRIPTEN__) || defined(PLAT_HOST)
/* an emulated FM-1 starts as a real one does after the flasher: the factory sounds in its bank (the host simulator
 * loads them on its "factory" command: the video's clips, app/../video) */
#include "factory_data.h"
static int fac_fill(uint32_t index, uint8_t *pay, void *ctx)
{
    const fac_sound_t *f = ctx;
    uint32_t i, o = index * EF_SEC_PAYLOAD;
    for (i = 0; i < EF_SEC_PAYLOAD; i++)
        pay[i] = f->pay[o + i];
    return 0;
}
static void emu_factory(void)
{
    uint32_t s;
    for (s = 0; s < BANK_SLOTS; s++)
        if (dir.slot[s].used)
            return;
    for (s = 0; s < FAC_N; s++)
        bank_write(s, FAC_SOUNDS[s].frames, FAC_SOUNDS[s].rate, FAC_SOUNDS[s].name, FAC_SOUNDS[s].crc, 1, fac_fill,
                   (void *)&FAC_SOUNDS[s], 0);
}
#endif

/* ================================================================ boot */
void app_init(void)
{
    int s, i;
    ef_ctl_t *c = ef_ctl();
    settings_load();
    presets_load();
    bank_load();
#ifdef __EMSCRIPTEN__
    emu_factory();
#endif
    for (i = 0; i < G_COUNT; i++)
        ui.group_page[i] = 0;
    for (i = NPAGES - 1; i >= 0; i--)
        ui.group_page[PAGES[i].group] = i;
    ui.group_page[G_PRESETS] = SCR_PRESETS;
    ui.group_page[G_SYSTEM] = SCR_SYSTEM;
    ui.page = 0;
    ui.touched = -1;
    session_load(&s);
    for (i = 0; i < P_COUNT; i++)
        c->p[i] = pv[i];
    c->max_grains = set.voices;
    ef_init();
#if FM1B_CPU2
    if (plat_cpu2_set(1) == 0)           /* at power-on only (plat.h); with cpu2_fallback, one core if it fails */
        ef_set_cpu2(plat_cpu2_run, plat_cpu2_wait);
#endif
    plat_usb_audio_full(0);              /* USB carries what the headphones get (see OUTPUT, app_render) */
    select_now(s);
    ses_dirty = 0;
}

/* ================================================================ backup (fm1-base backup.c) */
int app_backup_playing(void) { return ef_ctl()->play || ef_ctl()->nnotes || ef_rec_on(); }
void app_backup_flush(void)
{
    if (ses_dirty)
        session_save();
}
void app_backup_hold(int on) { backup_hold = (uint8_t)(on != 0); }
void app_backup_restored(void)
{
    int s, i;
    settings_load();
    presets_load();
    session_load(&s);
    for (i = 0; i < P_COUNT; i++)
        ef_ctl()->p[i] = pv[i];
    ef_ctl()->max_grains = set.voices;
    theme_apply();
    select_slot(s);
    ses_dirty = 0;
    app_redraw();
}

/* ================================================================ host simulator */
#ifdef PLAT_HOST
static int host_fill(uint32_t index, uint8_t *pay, void *ctx)   /* a test tone for "bankwrite" */
{
    uint32_t i, seconds = *(uint32_t *)ctx;
    static ef_ima_t st;
    if (!index) {
        st.pred = 0;
        st.index = 0;
    }
    for (i = 0; i < EF_SEC_SAMPLES; i++) {
        uint32_t n = index * EF_SEC_SAMPLES + i, j = i % EF_BLK_SAMPLES, nib;
        uint8_t *blk = pay + (i / EF_BLK_SAMPLES) * EF_BLK_BYTES;
        float t = (float)n / 22050.0f;
        int32_t x = (int32_t)(12000.0f * fm_sin_turns(220.0f * t) * fm_expf(-(t - fm_floorf(t)) * 2.0f) +
                              6000.0f * fm_sin_turns(331.0f * t + (float)seconds));
        if (!j) {
            blk[0] = (uint8_t)st.pred;
            blk[1] = (uint8_t)((uint32_t)st.pred >> 8);
            blk[2] = (uint8_t)st.index;
            blk[3] = 0;
        }
        nib = ef_ima_encode(&st, x);
        if (j & 1u)
            blk[4u + j / 2u] = (uint8_t)((blk[4u + j / 2u] & 15u) | nib << 4);
        else
            blk[4u + j / 2u] = (uint8_t)nib;
    }
    return 0;
}

int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    (void)c;
    (void)d;
    if (!strcmp(cmd, "bankwrite")) {     /* bankwrite SLOT SECONDS [NAME]: a test sample into the bank */
        uint32_t secs = (uint32_t)atoi(b), frames = secs * 22050u;
        int rc = bank_write((uint32_t)atoi(a), frames, 22050, c && c[0] ? c : "TEST TONE", 0, 0, host_fill, &secs, 0);
        dir_changed();
        printf("  bankwrite %s: %d\n", a, rc);
        return 1;
    }
    if (!strcmp(cmd, "factory")) {      /* factory: the 14 factory sounds into an empty bank, as the flasher does */
        emu_factory();
        dir_changed();
        return 1;
    }
    if (!strcmp(cmd, "bankerase")) {
        printf("  bankerase %s: %d\n", a, bank_erase((uint32_t)atoi(a)));
        dir_changed();
        return 1;
    }
    if (!strcmp(cmd, "bankbreak")) {     /* bankbreak SLOT: its first sector erased behind the directory's back */
        uint32_t s = (uint32_t)atoi(a), i;
        for (i = 0; i < BANK_NSEC; i++)
            if (map_bit(dir.slot[s].map, i)) {
                plat_data_erase(sec_addr(i));
                break;
            }
        return 1;
    }
    if (!strcmp(cmd, "usbcmd")) {        /* usbcmd HEXBYTES: a decoded frame to app_usb_cmd (body as hex, checksum made) */
        static uint8_t f[600];
        uint32_t n = 0, s = 0, i;
        const char *p = a;
        f[0] = 0;
        f[1] = 0x59;
        f[2] = (uint8_t)strtoul(b, 0, 16);
        while (p && p[0] && p[1] && n < 500u) {
            char h[3] = {p[0], p[1], 0};
            f[6u + n++] = (uint8_t)strtoul(h, 0, 16);
            p += 2;
        }
        f[3] = (uint8_t)n;
        f[4] = (uint8_t)(n >> 8);
        f[5] = 0;
        for (i = 0; i < n; i++)
            s += f[6u + i];
        f[6u + n] = (uint8_t)~s;
        app_usb_cmd(f, 7u + n);
        return 1;
    }
    return 0;
}

int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "mode"))
        *got = (int)pv[P_MODE];
    else if (!strcmp(what, "play"))
        *got = ef_ctl()->play;
    else if (!strcmp(what, "freeze"))
        *got = ef_ctl()->freeze;
    else if (!strcmp(what, "slot"))
        *got = cur_slot;
    else if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "grains"))
        *got = ef_meter()->grains;
    else if (!strcmp(what, "nnotes"))
        *got = ef_ctl()->nnotes;
    else if (!strcmp(what, "octave"))
        *got = ui.octave;
    else if (!strcmp(what, "dirty"))
        *got = ses_dirty;
    else if (!strcmp(what, "rec"))
        *got = ef_rec_on();
    else if (!strcmp(what, "punch"))
        *got = (int)ef_ctl()->punch;
    else if (!strcmp(what, "slots_ok")) {
        uint32_t s;
        *got = 0;
        for (s = 0; s < BANK_SLOTS; s++)
            *got += slot_state[s] == SLOT_OK;
    } else if (!strncmp(what, "state", 5))
        *got = slot_state[atoi(what + 5) % BANK_SLOTS];
    else if (!strcmp(what, "free"))
        *got = (int)bank_free_secs(-1);
    else if (!strncmp(what, "pq", 2))    /* pqN: parameter N, as 16 bits */
        *got = pq(atoi(what + 2) % P_COUNT, pv[atoi(what + 2) % P_COUNT]);
    else if (!strncmp(what, "locked", 6))
        *got = locked(atoi(what + 6) % P_COUNT);
    else if (!strcmp(what, "user_used")) {
        uint32_t i;
        *got = 0;
        for (i = 0; i < NUSER; i++)
            *got += pre.p[i].used;
    } else if (!strcmp(what, "shed"))
        *got = ef_ctl()->shed;
    else if (!strcmp(what, "listen"))
        *got = ef_listening();
    else
        return 0;
    return 1;
}
#endif
