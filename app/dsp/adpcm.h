/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Philip Bos */
/* IMA ADPCM (the classic DVI tables) in ECHOFIELD's block layout (ef_dsp.h). The companion app's encoder
 * (companion/lib/efcodec.js) writes it; this decodes it, bit for bit as efcodec.js's decoder does. */
#pragma once
#include <stdint.h>

#define EF_IMA_STEP_VALUES 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
#define EF_IMA_INDEX_VALUES -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
static const int16_t EF_IMA_STEP[89] = {EF_IMA_STEP_VALUES};
static const int8_t EF_IMA_INDEX[16] = {EF_IMA_INDEX_VALUES};
/* the tables the decoder reads: an includer may give RAM copies (the engine's RAM code keeps off the flash's cache) */
#ifndef EF_IMA_STEP_TAB
#define EF_IMA_STEP_TAB EF_IMA_STEP
#define EF_IMA_INDEX_TAB EF_IMA_INDEX
#endif

typedef struct {
    int32_t pred;
    int32_t index;
} ef_ima_t;

static inline __attribute__((always_inline)) int32_t ef_ima_step(ef_ima_t *s, uint32_t nib)
{
    int32_t step = EF_IMA_STEP_TAB[s->index], d = step >> 3;
    if (nib & 4u)
        d += step;
    if (nib & 2u)
        d += step >> 1;
    if (nib & 1u)
        d += step >> 2;
    s->pred += (nib & 8u) ? -d : d;
    if (s->pred > 32767)
        s->pred = 32767;
    else if (s->pred < -32768)
        s->pred = -32768;
    s->index += EF_IMA_INDEX_TAB[nib & 15u];
    if (s->index < 0)
        s->index = 0;
    else if (s->index > 88)
        s->index = 88;
    return s->pred;
}

/* a block's header -> the decoder's state; a damaged index is clamped, never trusted */
static inline __attribute__((always_inline)) void ef_ima_head(ef_ima_t *s, const uint8_t *blk)
{
    s->pred = (int16_t)(uint16_t)((uint32_t)blk[0] | (uint32_t)blk[1] << 8);
    s->index = blk[2] > 88u ? 88 : blk[2];
}

/* the whole block (256 samples) into out[0..255] */
static inline __attribute__((always_inline)) void ef_ima_block(const uint8_t *blk, int16_t *out)
{
    ef_ima_t s;
    uint32_t i;
    const uint8_t *d = blk + 4;
    ef_ima_head(&s, blk);
    for (i = 0; i < 128u; i++) {
        uint32_t b = d[i];
        out[2u * i] = (int16_t)ef_ima_step(&s, b & 15u);
        out[2u * i + 1u] = (int16_t)ef_ima_step(&s, b >> 4);
    }
}

/* a block's first sample alone (the interpolation's neighbour across a block edge) */
static inline __attribute__((always_inline)) int16_t ef_ima_first(const uint8_t *blk)
{
    ef_ima_t s;
    ef_ima_head(&s, blk);
    return (int16_t)ef_ima_step(&s, blk[4] & 15u);
}

/* the encoder's one sample (RESAMPLE): the nibble that brings the predictor closest, the state advanced */
static inline uint32_t ef_ima_encode(ef_ima_t *s, int32_t x)
{
    int32_t step = EF_IMA_STEP[s->index], diff = x - s->pred;
    uint32_t nib = 0;
    ef_ima_t t;
    if (diff < 0) {
        nib = 8u;
        diff = -diff;
    }
    if (diff >= step) {
        nib |= 4u;
        diff -= step;
    }
    if (diff >= step >> 1) {
        nib |= 2u;
        diff -= step >> 1;
    }
    if (diff >= step >> 2)
        nib |= 1u;
    t = *s;
    ef_ima_step(&t, nib);
    *s = t;
    return nib;
}
