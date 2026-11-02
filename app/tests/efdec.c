/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Philip Bos */
/* the firmware's decoder on the host: payload file + frame count -> raw int16 LE on stdout (tests/codec_test.mjs) */
#include <stdio.h>
#include <stdlib.h>
#include "ef_dsp.h"
#include "adpcm.h"
int main(int argc, char **argv)
{
    FILE *f = argc > 2 ? fopen(argv[1], "rb") : 0;
    uint32_t frames = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 10) : 0, b, nb;
    static uint8_t pay[EF_MAX_SECS * EF_SEC_PAYLOAD];
    int16_t blk[256];
    if (!f)
        return 2;
    fread(pay, 1, sizeof pay, f);
    nb = (frames + 255u) / 256u;
    for (b = 0; b < nb; b++) {
        uint32_t k = frames - b * 256u < 256u ? frames - b * 256u : 256u;
        ef_ima_block(pay + b * EF_BLK_BYTES, blk);
        fwrite(blk, 2, k, stdout);
    }
    return 0;
}
