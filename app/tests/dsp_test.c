/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Philip Bos */
/* The engine on the host: every mode, the oracle, the keys, the performance effects and the extremes rendered from a
 * real EFB1 source, checked for NaN, silence, runaway levels and reads outside the sample, and timed.
 *   cc -O2 -DPLAT_HOST -Idsp -I../base/include dsp/engine.c tests/dsp_test.c -o build/dsp_test && build/dsp_test [out.wav]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ef_dsp.h"
#include "adpcm.h"

static int fails;
uint32_t plat_ticks24(void) { return 0; }
/* a stand-in second core: the job runs when it is waited for, as on the FM-1 the job runs beside this core's share */
static void (*job_fn)(uint32_t);
static uint32_t job_arg;
static int fake_run(void (*fn)(uint32_t), uint32_t arg) { job_fn = fn; job_arg = arg; return 1; }
static int fake_wait(void) { job_fn(job_arg); return 0; }
static int fake_wait_fail(void) { return -1; }
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* a source in RAM laid out as in flash: payloads with guard pages between them, so a read outside shows */
static uint8_t *arena;
static ef_source_t make_source(const int16_t *x, uint32_t n, uint32_t rate)
{
    ef_source_t s;
    uint32_t nb = (n + 255u) / 256u, b, nsec = (nb + EF_SEC_BLOCKS - 1u) / EF_SEC_BLOCKS, k;
    ef_ima_t st = {0, 0};
    memset(&s, 0, sizeof s);
    arena = calloc(nsec, 8192);
    for (k = 0; k < nsec; k++) {
        s.sec[k] = arena + k * 8192u + 2048u;
        memset(arena + k * 8192u, 0x5A, 2048);              /* guard: nonsense if decoded */
    }
    for (b = 0; b < nb; b++) {
        uint8_t *blk = (uint8_t *)s.sec[b / EF_SEC_BLOCKS] + (b % EF_SEC_BLOCKS) * EF_BLK_BYTES;
        uint32_t j;
        blk[0] = (uint8_t)st.pred;
        blk[1] = (uint8_t)((uint32_t)st.pred >> 8);
        blk[2] = (uint8_t)st.index;
        blk[3] = 0;
        for (j = 0; j < 256u; j++) {
            uint32_t i = b * 256u + j, nib = ef_ima_encode(&st, i < n ? x[i] : 0);
            if (j & 1u)
                blk[4 + j / 2] |= (uint8_t)(nib << 4);
            else
                blk[4 + j / 2] = (uint8_t)nib;
        }
    }
    s.nsec = nsec;
    s.frames = n;
    s.rate = rate;
    return s;
}

static void defaults(ef_ctl_t *c)
{
    static const float D[P_COUNT] = {
        [P_POSITION] = 0.35f, [P_TEXTURE] = 0.72f, [P_SCATTER] = 0.45f, [P_DRIFT] = 0.35f, [P_MOTION] = 0.35f,
        [P_PITCH] = 0, [P_SPACE] = 0.65f, [P_TAPE] = 0.12f, [P_WIDTH] = 0.85f, [P_LEVEL] = 0.75f, [P_HARMONY] = 0.3f,
        [P_SIZE_RAND] = 0.3f, [P_REVERSE] = 0.1f, [P_GRAIN_TONE] = -0.1f, [P_CUTOFF] = 0.82f, [P_RESO] = 0.1f,
        [P_DRIVE] = 0.05f, [P_DLY_TIME] = 520, [P_DLY_FB] = 0.45f, [P_DLY_MIX] = 0.18f, [P_DLY_PINGPONG] = 1,
        [P_REV_SIZE] = 0.78f, [P_REV_DAMP] = 0.5f, [P_SHIMMER] = 0.22f, [P_TAPE_WOW] = 0.4f, [P_TAPE_FLUTTER] = 0.25f,
        [P_TAPE_AGE] = 0.25f, [P_OR_SCALE] = 1, [P_OR_ACTIVITY] = 0.35f, [P_OR_AMBIG] = 0.2f, [P_OR_RANGE] = 1,
        [P_OR_GLIDE] = 0.4f, [P_ATTACK] = 0.2f, [P_RELEASE] = 0.4f, [P_VELO] = 0.5f, [P_START] = 0, [P_END] = 1};
    int i;
    for (i = 0; i < P_COUNT; i++)
        c->p[i] = D[i];
}

typedef struct {
    double rms, peak, mean[2];
    int nan, nonzero;
    double ns_per_frame;
} stats_t;

static FILE *wav;
static uint32_t wav_frames;
static stats_t run(float seconds)
{
    stats_t st = {0};
    int32_t buf[512];
    uint32_t total = (uint32_t)(seconds * EF_SR), done = 0;
    double acc = 0, sum[2] = {0, 0};
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (done < total) {
        uint32_t i;
        ef_render(buf, 256, 1.0f);
        for (i = 0; i < 512; i++) {
            double v = buf[i] / 8300000.0;
            if (v != v)
                st.nan++;
            if (fabs(v) > st.peak)
                st.peak = fabs(v);
            if (buf[i])
                st.nonzero++;
            acc += v * v;
            sum[i & 1u] += v;
        }
        if (wav) {
            int16_t s16[512];
            for (i = 0; i < 512; i++)
                s16[i] = (int16_t)(buf[i] >> 8);
            fwrite(s16, 2, 512, wav);
            wav_frames += 256;
        }
        done += 256;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    st.rms = sqrt(acc / (2.0 * done));
    st.mean[0] = sum[0] / done;
    st.mean[1] = sum[1] / done;
    st.ns_per_frame = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / done;
    return st;
}

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t d = frames * 4, v;
    fwrite("RIFF", 1, 4, f); v = 36 + d; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    uint16_t h[2] = {1, 2}; fwrite(h, 2, 2, f);
    v = EF_SR; fwrite(&v, 4, 1, f); v = EF_SR * 4; fwrite(&v, 4, 1, f);
    uint16_t h2[2] = {4, 16}; fwrite(h2, 2, 2, f);
    fwrite("data", 1, 4, f); fwrite(&d, 4, 1, f);
}

static void rng_reset_for_test(void) {}
int main(int argc, char **argv)
{
    static const char *const MODE[MODE_COUNT] = {"CLOUD", "DUST", "TAPE", "BLOOM", "PULSE", "DRIFT"};
    uint32_t n = 22050u * 3u, i;
    int16_t *x = malloc(n * 2);
    ef_source_t s;
    ef_ctl_t *c;
    int m;
    double worst = 0;
    /* a test tone: a chord with an attack every half second, plus a little noise */
    for (i = 0; i < n; i++) {
        double t = i / 22050.0, e = exp(-fmod(t, 0.5) * 4.0);
        x[i] = (int16_t)(9000 * e * (sin(2 * M_PI * 220 * t) + 0.6 * sin(2 * M_PI * 330 * t) + 0.4 * sin(2 * M_PI * 440 * t)) +
                         300 * ((rand() & 255) / 128.0 - 1.0));
    }
    s = make_source(x, n, 22050);
    /* the decoder against the encoder's own reconstruction: sample by sample */
    {
        ef_ima_t st = {0, 0};
        int bad = 0;
        for (i = 0; i < n; i++) {
            int16_t want;
            if (i % 256u == 0) {           /* the encoder restarts each block from its header */
                const uint8_t *blk = s.sec[(i / 256u) / EF_SEC_BLOCKS] + ((i / 256u) % EF_SEC_BLOCKS) * EF_BLK_BYTES;
                ef_ima_head(&st, blk);
            }
            want = (int16_t)ef_ima_step(&st, (i & 1u) ? s.sec[(i / 256u) / EF_SEC_BLOCKS][((i / 256u) % EF_SEC_BLOCKS) * EF_BLK_BYTES + 4 + (i % 256u) / 2] >> 4
                                                      : s.sec[(i / 256u) / EF_SEC_BLOCKS][((i / 256u) % EF_SEC_BLOCKS) * EF_BLK_BYTES + 4 + (i % 256u) / 2] & 15);
            if (ef_sample_at(&s, i) != want)
                bad++;
        }
        CHECK(!bad, "ef_sample_at disagrees with the block decoder in %d samples", bad);
        double err = 0, sig = 0;
        for (i = 0; i < n; i++) {
            double d = ef_sample_at(&s, i) - x[i];
            err += d * d;
            sig += (double)x[i] * x[i];
        }
        printf("ADPCM SNR (greedy encoder): %.1f dB\n", 10 * log10(sig / err));
        CHECK(10 * log10(sig / err) > 20, "ADPCM SNR too low");
    }
    if (argc > 1) {
        wav = fopen(argv[1], "wb");
        wav_header(wav, 0);
    }
    c = ef_ctl();
    defaults(c);
    c->max_grains = 32;
    ef_init();
    ef_set_source(&s);
    c->play = 1;
    for (m = 0; m < MODE_COUNT; m++) {
        stats_t st;
        c->p[P_MODE] = (float)m;
        st = run(4.0f);
        printf("%-6s rms %.3f peak %.3f  %.0f ns/frame  grains %u\n", MODE[m], st.rms, st.peak, st.ns_per_frame, ef_meter()->grains);
        CHECK(!st.nan, "%s: NaN", MODE[m]);
        CHECK(st.rms > 0.01, "%s: silent", MODE[m]);
        CHECK(st.peak <= 1.0001, "%s: over full scale", MODE[m]);
        if (st.ns_per_frame > worst)
            worst = st.ns_per_frame;
    }
    /* extremes: everything at max, the oracle on, every punch effect in turn */
    printf("extremes\n");
    for (i = 0; i < P_COUNT; i++)
        if (i != P_MODE && i != P_START && i != P_END && i != P_LEVEL)
            c->p[i] = i == P_DLY_TIME ? 1000.0f : i == P_PITCH ? 24.0f : i == P_FILTER_TYPE ? 2.0f
                     : i == P_WINDOW ? 5.0f : i == P_OR_SCALE ? 11.0f : i == P_OR_ROOT ? 11.0f : i == P_OR_RANGE ? 2.0f
                     : i == P_SIZE_MUL || i == P_DENS_MUL ? 2.0f : i == P_SCAN ? 1.0f : i == P_PITCH_RAND ? 12.0f : 1.0f;
    c->p[P_DLY_FB] = 0.92f;
    for (m = 0; m < MODE_COUNT; m++) {
        stats_t st;
        c->p[P_MODE] = (float)m;
        st = run(3.0f);
        printf("  %-6s max: rms %.3f peak %.3f %.0f ns/frame\n", MODE[m], st.rms, st.peak, st.ns_per_frame);
        CHECK(!st.nan && st.peak <= 1.0001, "%s extremes: nan %d peak %f", MODE[m], st.nan, st.peak);
        if (st.ns_per_frame > worst)
            worst = st.ns_per_frame;
    }
    defaults(c);
    c->p[P_MODE] = MODE_CLOUD;
    for (i = 0; i < PUNCH_COUNT; i++) {
        stats_t st;
        c->punch = 1u << i;
        st = run(1.5f);
        CHECK(!st.nan && st.peak <= 1.0001, "punch %u: nan %d peak %f", i, st.nan, st.peak);
    }
    c->punch = 0;
    /* tiny region at the very end, reverse, max pitch: reads stay inside */
    c->p[P_START] = 0.995f;
    c->p[P_END] = 1.0f;
    c->p[P_REVERSE] = 1.0f;
    c->p[P_PITCH] = 24.0f;
    for (m = 0; m < MODE_COUNT; m++) {
        stats_t st;
        c->p[P_MODE] = (float)m;
        st = run(1.0f);
        CHECK(!st.nan && st.peak <= 1.0001, "edge region %s", MODE[m]);
    }
    c->p[P_START] = 0;
    c->p[P_END] = 1;
    c->p[P_REVERSE] = 0.1f;
    c->p[P_PITCH] = 0;
    /* keys: play off, a chord held then released: sound, then the release tail, then silence */
    {
        stats_t st;
        c->play = 0;
        c->p[P_MODE] = MODE_CLOUD;
        c->p[P_RELEASE] = 0.1f;
        run(3.0f);
        c->note[0] = 0; c->note[1] = 4; c->note[2] = 7;
        c->vel[0] = c->vel[1] = c->vel[2] = 100;
        c->nnotes = 3;
        c->note_seq++;
        st = run(1.0f);
        CHECK(st.rms > 0.01, "keys: no sound");
        c->nnotes = 0;
        run(6.0f);
        st = run(2.0f);
        CHECK(st.peak < 0.01, "keys: still sounding after the release (%f)", st.peak);
    }
    /* RESAMPLE into RAM, then that recording played as a source */
    {
        static uint8_t rb[6 * EF_SEC_PAYLOAD];
        ef_source_t r;
        uint32_t got, k;
        stats_t st;
        c->play = 1;
        ef_rec_start(rb, 6);
        run(1.0f);
        got = ef_rec_stop();
        CHECK(got > 21000 && got < 23000, "resample: %u samples in a second", got);
        memset(&r, 0, sizeof r);
        for (k = 0; k < 6; k++)
            r.sec[k] = rb + k * EF_SEC_PAYLOAD;
        r.nsec = 6;
        r.frames = got;
        r.rate = 22050;
        ef_set_source(&r);
        st = run(2.0f);
        CHECK(st.rms > 0.01 && !st.nan, "resampled source plays");
        ef_set_source(&s);
        run(0.2f);
    }
    /* two cores: the same sound as one (summing order aside), every job done; a core that gives up: still sound */
    {
        stats_t one, two, given;
        c->p[P_MODE] = MODE_CLOUD;
        c->p[P_DENS_MUL] = 2.0f;
        c->max_grains = 32;
        rng_reset_for_test();
        one = run(3.0f);
        ef_set_cpu2(fake_run, fake_wait);
        two = run(3.0f);
        CHECK(ef_cpu2_jobs() > 1000, "two cores: %u jobs", ef_cpu2_jobs());
        CHECK(!two.nan && two.rms > 0.01 && fabs(20 * log10(two.rms / one.rms)) < 3.0, "two cores: %.3f vs one %.3f rms", two.rms, one.rms);
        ef_set_cpu2(fake_run, fake_wait_fail);
        given = run(1.0f);
        CHECK(!given.nan && given.rms > 0.01, "a second core that gives up: the sound goes on");
        ef_set_cpu2(0, 0);
        c->p[P_DENS_MUL] = 0.0f;
        printf("two cores: rms %.3f (one core %.3f), %u jobs\n", two.rms, one.rms, ef_cpu2_jobs());
    }
    /* LISTEN: the sample itself, then the cloud back; once through a region it stops by itself */
    {
        stats_t st;
        c->play = 0;
        run(8.0f);                       /* (silence first) */
        c->p[P_START] = 0.25f;
        c->p[P_END] = 0.5f;
        ef_listen(1, 0);
        st = run(0.3f);
        CHECK(st.rms > 0.02 && !st.nan && ef_listening(), "listen: the sample sounds (rms %.3f)", st.rms);
        CHECK(ef_meter()->aud > 60 && ef_meter()->aud < 135, "listen: the playhead inside the region (%u)", ef_meter()->aud);
        run(1.0f);                       /* a quarter of 3 s: done after 0.75 s */
        CHECK(!ef_listening() && ef_meter()->aud == 0, "listen: stops at the region's end");
        st = run(0.5f);
        CHECK(st.peak < 0.01, "listen: silence after it (%.4f)", st.peak);
        ef_listen(1, 1);
        run(2.0f);
        CHECK(ef_listening(), "listen with loop: still going after the region's length");
        ef_listen(0, 0);
        run(0.1f);
        CHECK(!ef_listening(), "listen: stopped");
        c->p[P_START] = 0.6f;            /* to the very end of the sample (an FM-1 crashed here: 0.2.1) */
        c->p[P_END] = 1.0f;
        ef_listen(1, 0);
        st = run(1.6f);
        CHECK(!ef_listening() && !st.nan, "listen to the sample's very end: stops, reads nothing past it");
        ef_listen(1, 1);
        run(3.0f);
        CHECK(ef_listening(), "listen to the end, looped: goes round");
        ef_listen(0, 0);
        run(0.2f);
        c->play = 1;                     /* the cloud under it: ducked while listening, back after */
        run(3.0f);
        ef_listen(1, 1);
        run(0.5f);
        ef_listen(0, 0);
        st = run(1.5f);
        CHECK(st.rms > 0.01, "the cloud comes back after listening");
        c->p[P_START] = 0.0f;
        c->p[P_END] = 1.0f;
    }
    /* no DC: RAIN CATHEDRAL's long reverb with shimmer once held a DC offset that grew to the limiter (the shimmer
     * loop: an octave up of 0 Hz is 0 Hz, and the reverb's DC gain made the loop's gain above 1) */
    {
        stats_t st;
        defaults(c);
        c->p[P_MODE] = MODE_CLOUD;
        c->p[P_TEXTURE] = 0.88f, c->p[P_SCATTER] = 0.60f, c->p[P_DRIFT] = 0.40f, c->p[P_MOTION] = 0.30f;
        c->p[P_SPACE] = 0.90f, c->p[P_HARMONY] = 0.45f, c->p[P_SHIMMER] = 0.50f, c->p[P_REV_SIZE] = 0.95f;
        c->p[P_CUTOFF] = 0.72f;
        c->play = 1;
        run(40.0f);
        st = run(8.0f);
        printf("RAIN CATHEDRAL 40 s: DC %+.4f %+.4f, rms %.3f\n", st.mean[0], st.mean[1], st.rms);
        CHECK(fabs(st.mean[0]) < 0.01 && fabs(st.mean[1]) < 0.01, "no DC builds up (long reverb, shimmer)");
        c->play = 0;
        run(15.0f);
        st = run(3.0f);
        printf("  then silence: DC %+.4f %+.4f, rms %.4f\n", st.mean[0], st.mean[1], st.rms);
        CHECK(fabs(st.mean[0]) < 0.002 && fabs(st.mean[1]) < 0.002 && st.rms < 0.01, "silence after it is silence");
        defaults(c);
        c->play = 1;
    }
    /* a source swap to nothing and back, and a source of one block */
    {
        stats_t st;
        ef_set_source(0);
        st = run(0.5f);
        CHECK(!st.nan, "no source");
        ef_set_source(&s);
        run(0.2f);
    }
    if (wav) {
        fseek(wav, 0, SEEK_SET);
        wav_header(wav, wav_frames);
        fclose(wav);
    }
    printf("worst %.0f ns/frame on this host (%.1f%% of real time)\n", worst, worst / (1e9 / EF_SR) * 100);
    /* MIDI clock: PULSE steps on the clock's grid, re-aligned every quarter note */
    {
        int b, off = 0;
        defaults(c);
        c->play = 1;
        c->p[P_MODE] = (float)MODE_PULSE;
        c->ext_bpm = 120.0f;             /* own pace at MOTION 0.35 is ~61 BPM: the clock's 1/8 (2 steps a beat) */
        for (b = 0; b < 8; b++) {
            c->ext_beat = (uint32_t)b;
            c->ext_beat_seq++;
            run(0.004f);
            if (ef_meter()->pulse_step != (uint8_t)((b * 2) & 15))
                off++;
            run(0.496f);
        }
        CHECK(!off, "PULSE off the clock's grid on %d of 8 beats", off);
        c->ext_bpm = 0.0f;
    }
    printf(fails ? "dsp_test: %d FAILED\n" : "dsp_test: all passed\n", fails);
    return fails != 0;
}
