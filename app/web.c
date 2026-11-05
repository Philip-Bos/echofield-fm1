/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Philip Bos */
/* ECHOFIELD FM-1: the browser emulator's sample editor, its half in the firmware (fm1-base web/emu/emu_web.c includes
 * this file for an app with APP_WEB, after app.c: the bank's statics are in scope). The page has the companion's
 * Samples editor under the panel (web/emu/app.js, app_worklet.js), which calls these between two audio quanta.
 * fm1-base's page keeps the store objects, not the bank's sectors, so the page keeps the samples itself and puts them
 * back after boot (ef_web_clear, then ef_web_write each), or, when it has none, calls ef_web_factory: every factory
 * sound in its own slot again. The companion's UPLOAD pulls the payload with read requests the device waits for; a
 * worklet cannot wait, so here the payload is in ef_web_buf first. */
#define WEB_API __attribute__((used, visibility("default")))
static uint8_t web_buf[EF_MAX_SECS * EF_SEC_PAYLOAD];
static char web_name[NAME_N];
WEB_API uint8_t *ef_web_buf(void) { return web_buf; }
WEB_API char *ef_web_name(void) { return web_name; }
WEB_API uint32_t ef_web_gen(void) { return dir.gen; }
WEB_API int ef_web_slot(void) { return cur_slot; }
WEB_API void ef_web_dir(void)            /* proto.c DIR's records into ef_web_buf: 16 x 20 bytes */
{
    uint32_t s, i;
    for (s = 0; s < BANK_SLOTS; s++) {
        uint8_t *o = web_buf + s * 20u;
        o[0] = dir.slot[s].used ? slot_state[s] : SLOT_EMPTY;
        o[1] = dir.slot[s].factory;
        for (i = 0; i < NAME_N; i++)
            o[2u + i] = dir.slot[s].used ? (uint8_t)dir.slot[s].name[i] : 0;
        for (i = 0; i < 4u; i++)
            o[14u + i] = (uint8_t)(dir.slot[s].frames >> (8u * i));
        o[18] = (uint8_t)dir.slot[s].rate;
        o[19] = (uint8_t)(dir.slot[s].rate >> 8);
    }
}
WEB_API int ef_web_read(uint32_t s)      /* a sample's payload into ef_web_buf -> bytes, or -1 */
{
    uint32_t i, k = 0, j;
    if (s >= BANK_SLOTS || slot_state[s] != SLOT_OK)
        return -1;
    for (i = 0; i < BANK_NSEC; i++)
        if (map_bit(dir.slot[s].map, i)) {
            const uint8_t *p = plat_data(sec_addr(i));
            for (j = 0; j < EF_SEC_PAYLOAD; j++)
                web_buf[k * EF_SEC_PAYLOAD + j] = p[j];
            k++;
        }
    return (int)(k * EF_SEC_PAYLOAD);
}
static int web_fill(uint32_t index, uint8_t *pay, void *ctx)
{
    uint32_t i;
    (void)ctx;
    for (i = 0; i < EF_SEC_PAYLOAD; i++)
        pay[i] = web_buf[index * EF_SEC_PAYLOAD + i];
    return 0;
}
WEB_API int ef_web_write(uint32_t s, uint32_t frames, uint32_t rate, uint32_t factory, uint32_t crc)
{                                        /* ef_web_buf's payload, named ef_web_name -> BANK_OK or BANK_E_* */
    int rc = bank_write(s, frames, rate, web_name, crc, (int)factory, web_fill, 0, 0);
    dir_changed();
    return rc;
}
WEB_API int ef_web_erase(uint32_t s)
{
    int rc = bank_erase(s);
    dir_changed();
    return rc;
}
WEB_API int ef_web_rename(uint32_t s)
{
    int rc = bank_rename(s, web_name);
    dir_changed();
    return rc;
}
WEB_API void ef_web_select(int s) { select_slot(s); }
WEB_API void ef_web_listen(int how) { listen_cmd(how); }
WEB_API int ef_web_factory(void)         /* the factory sounds into their slots; what cannot be read is let go */
{
    uint32_t s;
    int n = 0;
    for (s = 0; s < BANK_SLOTS; s++)
        if (dir.slot[s].used && slot_state[s] != SLOT_OK)
            bank_erase(s);
    for (s = 0; s < FAC_N && s < BANK_SLOTS; s++)
        if (!dir.slot[s].used && bank_write(s, FAC_SOUNDS[s].frames, FAC_SOUNDS[s].rate, FAC_SOUNDS[s].name,
                                            FAC_SOUNDS[s].crc, 1, fac_fill, (void *)&FAC_SOUNDS[s], 0) == BANK_OK)
            n++;
    dir_changed();
    return n;
}
WEB_API void ef_web_clear(void)          /* every slot out of the directory (before the page puts its samples back) */
{
    uint32_t s;
    for (s = 0; s < BANK_SLOTS; s++)
        if (dir.slot[s].used)
            bank_erase(s);
}
