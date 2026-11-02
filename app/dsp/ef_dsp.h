/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Philip Bos */
/* ECHOFIELD FM-1: the sound engine's interface. The engine (engine.c, compiled at -O2) knows nothing of the panel,
 * the screen or the flash: it reads a sample through an ef_source_t (a list of payload pointers, in flash or RAM) and
 * a float parameter array the main loop writes. Single precision only (the FM-1's FPU has no double). */
#pragma once
#include <stdint.h>

#define EF_SR 44100u                     /* the FM-1's output rate */

/* ---- the sample format (EFB1): blocked IMA ADPCM, mono ----
 * A block is 256 samples: a 4-byte header (predictor int16 LE, step index, 0) holding the decoder state BEFORE its
 * first sample, then 128 bytes of nibbles, low nibble first. 29 blocks fill a sector's 3828-byte payload; a sample is
 * a list of such payloads in order (flash sectors anywhere in the bank, or RAM). Any block decodes on its own: random
 * access with no index. */
#define EF_BLK_SAMPLES 256u
#define EF_BLK_BYTES 132u
#define EF_SEC_BLOCKS 29u
#define EF_SEC_PAYLOAD (EF_SEC_BLOCKS * EF_BLK_BYTES)          /* 3828 */
#define EF_SEC_SAMPLES (EF_SEC_BLOCKS * EF_BLK_SAMPLES)        /* 7424 */
#define EF_MAX_SECS 96u

typedef struct {
    const uint8_t *sec[EF_MAX_SECS];     /* each payload's first byte, in order */
    uint32_t nsec;
    uint32_t frames;                     /* samples (<= nsec * EF_SEC_SAMPLES) */
    uint32_t rate;                       /* Hz: 8000 .. 48000 */
} ef_source_t;

/* ---- parameters ---- */
enum {
    /* PERFORM (HOME) */
    P_POSITION, P_TEXTURE, P_SCATTER, P_DRIFT, P_MOTION, P_PITCH, P_SPACE, P_TAPE, P_WIDTH, P_LEVEL, P_HARMONY,
    /* GRAIN */
    P_SIZE_MUL, P_DENS_MUL, P_SCAN, P_REVERSE, P_PITCH_RAND, P_SIZE_RAND, P_WINDOW, P_GRAIN_TONE,
    /* FX */
    P_FILTER_TYPE, P_CUTOFF, P_RESO, P_DRIVE,
    P_DLY_TIME, P_DLY_FB, P_DLY_MIX, P_DLY_PINGPONG,
    P_REV_SIZE, P_REV_DAMP, P_SHIMMER,
    P_TAPE_WOW, P_TAPE_FLUTTER, P_TAPE_AGE, P_TAPE_NOISE,
    /* DRIFT ORACLE */
    P_OR_ON, P_OR_ROOT, P_OR_SCALE, P_OR_ACTIVITY, P_OR_AMBIG, P_OR_RANGE, P_OR_GLIDE, P_OR_DRONE, P_OR_HOLD,
    /* KEYS */
    P_ATTACK, P_RELEASE, P_KEYMODE, P_VELO,
    /* SAMPLE region */
    P_START, P_END,
    /* scene */
    P_MODE,
    P_COUNT
};

enum { MODE_CLOUD, MODE_DUST, MODE_TAPE, MODE_BLOOM, MODE_PULSE, MODE_DRIFT, MODE_COUNT };
enum { WIN_HANN, WIN_TUKEY, WIN_PERC, WIN_SWELL, WIN_GAUSS, WIN_COUNT };
enum { KEYMODE_GATE, KEYMODE_LATCH };

#define EF_MAX_GRAINS 32u
#define EF_MAX_NOTES 8u
#define EF_VIS_N 32u

/* what the screen shows of the engine (written by the render, read by the UI: racy, visual only) */
typedef struct {
    uint8_t n;
    uint8_t x[EF_VIS_N];                 /* position, 0..255 of the sample */
    int8_t pitch[EF_VIS_N];              /* semitones */
    uint8_t amp[EF_VIS_N];               /* 0..255 */
    uint8_t pan[EF_VIS_N];               /* 0..255 */
} ef_vis_t;

typedef struct {
    volatile float p[P_COUNT];           /* written by the main loop */
    volatile uint8_t play;               /* the drone runs (PLAY) */
    volatile uint8_t freeze;             /* FREEZE: nothing moves */
    volatile uint8_t max_grains;         /* 4 .. EF_MAX_GRAINS: the player's cap */
    volatile uint8_t shed;               /* grains taken off the cap by the overload guard */
    volatile uint32_t punch;             /* held performance effects, bit PUNCH_* */
    /* held notes (keys and MIDI): semitones from C4 and velocity; set by the main loop / MIDI in */
    volatile int8_t note[EF_MAX_NOTES];
    volatile uint8_t vel[EF_MAX_NOTES];
    volatile uint8_t nnotes;
    volatile uint32_t note_seq;          /* bumped at every new note: a burst of grains */
    volatile uint32_t dbg_bypass;        /* diagnostics: effect sections skipped (bit 0 tape, 1 filter, 2 delay+reverb) */
} ef_ctl_t;

/* performance effects (FX held + a black key), momentary */
enum { PUNCH_FREEZE, PUNCH_REVERSE, PUNCH_OCT_UP, PUNCH_OCT_DOWN, PUNCH_STUTTER, PUNCH_HUSH, PUNCH_BLOOM,
       PUNCH_SHIMMER, PUNCH_TAPESTOP, PUNCH_SWEEP, PUNCH_THROW, PUNCH_COUNT };

typedef struct {
    float peak;                          /* output peak since the UI last read (it resets it) */
    uint8_t limiting;
    uint8_t grains;                      /* grains sounding */
    uint8_t center, spread, head;        /* 0..255 of the sample: the playhead, the scatter band, TAPE's head */
    int8_t oracle_note;                  /* bass note, semitones (0 = root) */
    uint8_t oracle_event;                /* bumped at each chance event; its name below */
    char oracle_name[16];
    uint8_t pulse_step;                  /* PULSE: the step playing */
    uint16_t pulse_pat;                  /* PULSE: the euclidean pattern */
    float env;                           /* the keys' gate envelope, 0..1 */
    /* timing, PLAT_TICKS_PER_US ticks, the most since the last ef_meter_time_reset */
    uint32_t t_render, t_grains, t_fx, t_spawn;
    uint8_t aud;                         /* LISTEN's playhead, 1..255 of the sample; 0 not listening */
} ef_meter_t;

/* the engine: one, static, in engine.c */
void ef_init(void);                      /* tables and state; the main loop calls it once before audio */
ef_ctl_t *ef_ctl(void);
const ef_meter_t *ef_meter(void);
void ef_meter_peak_reset(void);
void ef_meter_time_reset(void);
const ef_vis_t *ef_vis(void);
/* the source: the main loop hands a new one over; the render fades out, swaps, fades in. src may be 0 (silence).
 * The struct must stay unchanged until ef_source_idle() says the old one is let go */
void ef_set_source(const ef_source_t *src);
int ef_source_idle(void);                /* 1: no swap pending (the last source handed over is the one playing) */
const ef_source_t *ef_source_now(void);
void ef_render(int32_t *lr, uint32_t n, float gain);   /* audio interrupt: n stereo frames, 24-bit; gain 0..1 */
void ef_panic(void);
/* LISTEN: the playing sample itself, START to END, its own pitch (on 1 / off 0; loop: again from START at the end);
 * the cloud ducks under it. Starts once a pending source swap is done */
void ef_listen(int on, int loop);
int ef_listening(void);
/* the second core: run(fn, arg) hands fn(arg) over (1) or not (0), wait() 0 = done, -1 = given up (do it here) */
void ef_set_cpu2(int (*run)(void (*)(uint32_t), uint32_t), int (*wait)(void));
uint32_t ef_cpu2_jobs(void);                     /* silence now: every grain off, the tails cleared */
void ef_reset_time(void);                /* scan, tape head, walks back to the start (a preset load) */

/* RESAMPLE: the output recorded as EFB1 into RAM (22 050 Hz mono, the payload layout above) */
void ef_rec_start(uint8_t *buf, uint32_t nsec);   /* nsec payloads of EF_SEC_PAYLOAD bytes, back to back */
uint32_t ef_rec_stop(void);              /* -> samples recorded */
uint32_t ef_rec_frames(void);            /* while recording */
int ef_rec_on(void);

/* the ADPCM codec (adpcm.h) and the window tables are shared with the UI (waveform overview) */
int16_t ef_sample_at(const ef_source_t *s, uint32_t i);   /* one sample, decoded from its block (slow: UI only) */
float ef_window(int win, float x);       /* 0..1 -> 0..1 */
