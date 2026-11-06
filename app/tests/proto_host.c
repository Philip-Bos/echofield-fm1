/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Philip Bos */
/* The FM-1's side of the companion protocol (proto.c, bank.c, presets.c, params.c, the engine) on the host, against
 * a simulated flash (NOR: a write only clears bits) and store, talking SysEx over stdin / stdout as hex lines:
 *   in   "sx HEX"     a frame from the host (F0..F7)
 *   out  "sx HEX"     a frame to the host (replies, read requests)
 * tests/proto_test.mjs runs companion/lib/efproto.js against it. Nothing here is on the FM-1. */
#define PLAT_HOST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "plat.h"
#include "ef_dsp.h"
#include "fastmath.h"
#include "adpcm.h"

#define APP_VERSION "TEST"
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BANK, OBJ_PRESETS, OBJ_SESSION, OBJ_NOBJ };

/* ---- the platform, simulated */
static uint8_t flash[0x100000];
static uint8_t store[OBJ_NOBJ][PLAT_STORE_MAX];
static int store_len[OBJ_NOBJ];
uint32_t plat_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
void plat_yield(void) {}
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    if (obj >= OBJ_NOBJ || store_len[obj] <= 0)
        return -1;
    memcpy(dst, store[obj], (uint32_t)store_len[obj] < max ? (uint32_t)store_len[obj] : max);
    return store_len[obj];
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    if (obj >= OBJ_NOBJ || len > PLAT_STORE_MAX)
        return -1;
    memcpy(store[obj], src, len);
    store_len[obj] = (int)len;
    return 0;
}
const uint8_t *plat_data(uint32_t off) { return PLAT_DATA_OK(off, 1u) ? flash + off : 0; }
int plat_data_erase(uint32_t off)
{
    if ((off & 0xFFFu) || !PLAT_DATA_OK(off, 0x1000u))
        return -1;
    memset(flash + off, 0xFF, 0x1000);
    return 0;
}
int plat_data_write(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (!PLAT_DATA_OK(off, n))
        return -1;
    for (i = 0; i < n; i++)
        flash[off + i] &= ((const uint8_t *)src)[i];   /* NOR: bits only cleared */
    return 0;
}
void plat_midi_out(uint32_t pkt) { (void)pkt; }
void plat_host_patience(uint32_t a, uint32_t b) { (void)a, (void)b; }

static void put_hex(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    printf("sx ");
    for (i = 0; i < n; i++)
        printf("%02X", p[i]);
    printf("\n");
    fflush(stdout);
}
int plat_sysex_send(const uint8_t *frame, uint32_t n) { put_hex(frame, n); return 0; }

static uint32_t unpk7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, w = 0, i;
    for (i = 0; i < n; i++) {
        acc |= (uint32_t)(in[i] & 0x7Fu) << nb;
        nb += 7u;
        while (nb >= 8u) {
            out[w++] = (uint8_t)acc;
            acc >>= 8;
            nb -= 8u;
        }
    }
    return w;
}
static int read_frame(uint8_t *dec, uint32_t *n)   /* the next "sx" line from the host, decoded: 0 ok, -1 end */
{
    static char line[8192];
    static uint8_t raw[4096];
    uint32_t k = 0;
    if (!fgets(line, sizeof line, stdin))
        return -1;
    if (strncmp(line, "sx ", 3))
        return 1;
    for (char *p = line + 3; p[0] && p[1] && p[0] != '\n'; p += 2) {
        char h[3] = {p[0], p[1], 0};
        raw[k++] = (uint8_t)strtoul(h, 0, 16);
    }
    if (k < 2 || raw[0] != 0xF0 || raw[k - 1] != 0xF7)
        return 1;
    *n = unpk7(raw + 1, k - 2, dec);
    return 0;
}

static uint32_t pk7(const uint8_t *in, uint32_t n, uint8_t *out);
int plat_host_read(uint32_t addr, uint8_t *dst, uint32_t n)   /* a read request out, its answer in (as ota.c) */
{
    uint8_t q[16], w[40], dec[1200];
    uint32_t s = 0, i, k, d;
    q[0] = 0; q[1] = 0x59; q[2] = 0x30; q[3] = 8; q[4] = 0; q[5] = 0;
    q[6] = 0;
    q[7] = (uint8_t)addr; q[8] = (uint8_t)(addr >> 8); q[9] = (uint8_t)(addr >> 16); q[10] = (uint8_t)(addr >> 24);
    q[11] = (uint8_t)n; q[12] = (uint8_t)(n >> 8); q[13] = 0;
    for (i = 6; i < 14u; i++)
        s += q[i];
    q[14] = (uint8_t)~s;
    w[0] = 0xF0;
    k = 1u + pk7(q, 15, w + 1);
    w[k++] = 0xF7;
    put_hex(w, k);
    for (;;) {
        int rc = read_frame(dec, &d);
        if (rc < 0)
            return -1;
        if (rc)
            continue;
        if (d == n + 15u && dec[2] == 0x30 && (uint32_t)(dec[7] | dec[8] << 8 | dec[9] << 16 | (uint32_t)dec[10] << 24) == addr) {
            memcpy(dst, dec + 14, n);
            return 0;
        }
        fprintf(stderr, "proto_host: unexpected frame while reading (cmd %02X, %u bytes)\n", dec[2], d);
        return -1;
    }
}

/* ---- the app's pieces proto.c and bank.c lean on */
static int text_w_dummy;
#include "../params.c"
#include "../presets.c"
#include "../bank.c"
static void quiet_begin(void) {}
static void quiet_end(void) {}
static int store_quiet(uint32_t obj, const void *p, uint32_t n) { return plat_store_save(obj, p, n); }
static uint32_t rt_frames, rt_gap_max, rt_gaps_long;   /* (app.c: the audio's timing, for STATS) */
static volatile float side_ceil = 0.141f;                /* (app.c: the side limiter) */
#include "../proto.c"
static void param_set(int id, float v, int from_host) { pv[id] = pclamp(id, v); ef_ctl()->p[id] = pv[id]; (void)from_host; }
static void select_slot(int s) { cur_slot = (int8_t)s; }
static void play_set(int on) { ef_ctl()->play = (uint8_t)on; }
static void freeze_set(int on) { ef_ctl()->freeze = (uint8_t)on; }
static void upload_progress(uint32_t k, uint32_t n) { (void)k, (void)n; }
static void dir_changed(void) {}
static void random_do(int all) { (void)all; }
static void undo_do(void) {}
static void listen_cmd(int how) { (void)how; }
static int app_cpu2_state(void) { return 0; }
uint32_t plat_cpu_pct(void) { return 0; }
uint32_t plat_xruns(void) { return 0; }
uint32_t plat_ticks24(void) { return 0; }
static int preset_op(uint32_t op, uint32_t idx, const char *name)
{
    uint32_t k;
    if (op == 1 && idx < NUSER) {
        upreset_t *u = &pre.p[idx];
        text_cpy(u->name, name[0] ? name : "UNNAMED", NAME_N);
        u->used = 1;
        u->slot = cur_slot;
        for (k = 0; k < P_COUNT; k++)
            u->q[k] = pq((int)k, pv[k]);
        return plat_store_save(OBJ_PRESETS, &pre, sizeof pre) ? BANK_E_STORE : BANK_OK;
    }
    if (op == 2 && idx < NUSER) {
        pre.p[idx].used = 0;
        return plat_store_save(OBJ_PRESETS, &pre, sizeof pre) ? BANK_E_STORE : BANK_OK;
    }
    return BANK_E_ARGS;
}

int main(void)
{
    static uint8_t dec[4096];
    uint32_t n;
    (void)text_w_dummy;
    memset(flash, 0xFF, sizeof flash);
    settings_defaults();
    presets_defaults();
    bank_load();
    pdefaults(pv);
    cur_slot = -1;
    for (;;) {
        int rc = read_frame(dec, &n);
        if (rc < 0)
            break;
        if (!rc)
            app_usb_cmd(dec, n);
    }
    return 0;
}
