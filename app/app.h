/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Philip Bos */
/* ECHOFIELD FM-1: the app's half of the contract with fm1-base (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"
#include "ef_dsp.h"

#define APP_NAME "ECHOFIELD"
#define APP_TAGLINE "granular sampler"
#define APP_SPLASH_RGB RGB(255, 122, 26)
#define APP_PALETTE 4u                   /* MONO: fm1-base's own screens in white */
#define APP_BLOCK 256u                   /* 5.8 ms at 44.1 kHz */
#define APP_CANVAS_PX (240u * 80u)       /* the screen is drawn in bands of 80 rows at most */
#define FM1B_UAC_DEEP 1                  /* the USB audio input keeps a half more in hand: the render takes ~40-60 % */
#define FM1B_UAC_RING_POOL 1             /* (its 4 KB ring in the pool) */
#define APP_TOUCH                        /* velocity from the keys (lib/touch.h) */

/* the store: four objects of the app's, the panel's calibration (the platform's). Flash 0xD8000..0xDFFFF and the
 * settings pair at 0xFC000; the sample bank has the rest of the data windows (bank.c) */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BANK, OBJ_PRESETS, OBJ_SESSION, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDE000u, 0xDC000u, 0xDA000u, 0xD8000u}

/* backup and restore over USB (fm1-base backup.c): settings, presets and the session. The samples are not in it (the
 * companion app exports them itself: proto.c READ) */
#define APP_BACKUP_OBJS {OBJ_SETTINGS, OBJ_PRESETS, OBJ_SESSION}
#define APP_BACKUP_MAGIC 0x31424345u     /* "ECB1" */
#define APP_BACKUP_VERSION 1u
#define APP_BACKUP_WHAT "replaces presets and settings"
int app_backup_playing(void);
void app_backup_flush(void);
void app_backup_hold(int on);
void app_backup_restored(void);

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_OVERLOAD                     /* the render took over 90 % of its time: fewer grains */
void app_overload(void);
#define APP_REDRAW                       /* the mirror lost a part: everything again */
void app_redraw(void);
#define APP_USB_CMD                      /* the companion app's commands (proto.c) */
void app_usb_cmd(const uint8_t *d, uint32_t n);
#define APP_SPLASH
void app_splash(void);

#define APP_WEB                          /* the emulator page's sample editor (web.c) */
#define APP_HOST_EXPECT                  /* the host simulator's checks (the tests directory) */
int app_expect(const char *what, int *got);
#define APP_HOST_CMD
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);
