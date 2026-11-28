#include "fx_engine.h"

/* ============================================================================
 * Shared math helpers (zero libm, scalar float32, FTZ+DN active)
 * ============================================================================ */
static inline float soft_clip(float x) {
    /* Smooth Pade tanh approximation, saturates cleanly to +/-1.0 */
    if (x > 2.5f) return 1.0f;
    if (x < -2.5f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static inline int16_t f_to_s16(float x) {
    float s = x * 32767.0f;
    if (s > 32767.0f) return 32767;
    if (s < -32768.0f) return -32768;
    return (int16_t)s;
}

static inline float s16_to_f(int16_t s) {
    return (float)s * (1.0f / 32768.0f);
}

/* Fast sine approximation on phase in [0, 1), max error < 0.001 */
static inline float fast_sin01(float p) {
    p -= (float)(int)p;
    if (p < 0.0f) p += 1.0f;
    float x = p * 2.0f - 1.0f; /* [-1, 1) */
    float y = 4.0f * x * (1.0f - (x < 0.0f ? -x : x));
    return -y * (0.775f + 0.225f * (y < 0.0f ? -y : y));
}

/* Fast Hann window 0.5*(1 - cos(2*pi*p)) on p in [0, 1) */
static inline float fast_hann01(float p) {
    /* cos(2*pi*p) = sin(2*pi*(p + 0.25)) */
    return 0.5f * (1.0f - fast_sin01(p + 0.25f));
}

/* One-pole lowpass coefficient from normalized tone knob t in [0..1]
 * t=0 -> 20 kHz (nearly open, a=0.92), t=1 -> 500 Hz (dark, a=0.063) */
static inline float tone_to_lp_alpha(float t, float min_a, float max_a) {
    /* Exponential-feel curve using cubic approximation */
    float inv = 1.0f - t;
    float curve = inv * inv;
    return min_a + (max_a - min_a) * curve;
}

/* ============================================================================
 * FX 49: STOP (24 kHz / 12 kHz Nyquist, 9->4 bit, telephone bandpass, dropouts)
 * ============================================================================ */
#define STOP_LINE_LEN   24096u  /* > 1.0 s @ 24 kHz */
#define STOP_FIR_TAPS   15u

typedef struct {
    int16_t line_l[STOP_LINE_LEN];
    int16_t line_r[STOP_LINE_LEN];
} stop_arena_t;

typedef struct {
    uint32_t wr;
    uint32_t valid;         /* O(1) reset: samples written so far, avoids zeroing 96 KB in audio IRQ */
    float    cur_delay;     /* per-sample smoothed delay in samples (zero 750 Hz zipper!) */
    uint8_t  delay_init;
    uint32_t rng;
    uint32_t tick_cnt;
    int32_t  drop_rem;      /* samples remaining in active dropout @ 24 kHz */
    int32_t  drop_len;      /* total length of current dropout @ 24 kHz */
    int32_t  drop_pos;      /* current position inside dropout */
    uint8_t  fir_pos;
    float    fir_l[16];
    float    fir_r[16];
    /* Telephone bandpass biquads (Direct Form II transposed) @ 24 kHz */
    float    hp_l1, hp_l2, hp_r1, hp_r2;
    float    lp_l1, lp_l2, lp_r1, lp_r2;
    /* Upsample previous output sample for 2x linear interpolation */
    float    prev_wet_l, prev_wet_r;
    float    prev_gate;
} stop_state_t;

/* 15-tap symmetric Hamming-windowed LP FIR, fc ~ 10.6 kHz @ 48 kHz (sum = 1.0) */
static const float k_stop_fir[STOP_FIR_TAPS] = {
    -0.0042f,  0.0000f,  0.0178f,  0.0000f, -0.0615f,
     0.0000f,  0.2979f,  0.5000f,  0.2979f,  0.0000f,
    -0.0615f,  0.0000f,  0.0178f,  0.0000f, -0.0042f
};

/* RBJ biquad coeffs @ 24 kHz:
 * HP 300 Hz, Q = 0.7071: w0 = 2*pi*300/24000 = 0.078540
 * LP 3400 Hz, Q = 0.7071: w0 = 2*pi*3400/24000 = 0.890118 */
#define TELE_HP_B0  ( 0.9460f)
#define TELE_HP_B1  (-1.8920f)
#define TELE_HP_B2  ( 0.9460f)
#define TELE_HP_A1  (-1.8890f)
#define TELE_HP_A2  ( 0.8949f)

#define TELE_LP_B0  ( 0.1204f)
#define TELE_LP_B1  ( 0.2408f)
#define TELE_LP_B2  ( 0.1204f)
#define TELE_LP_A1  (-0.8065f)
#define TELE_LP_A2  ( 0.2881f)

/* ============================================================================
 * FX 50: Dub Delay (48 kHz stereo up to 2.0 s, LP in feedback, wow/flutter, sync)
 * ============================================================================ */
#define DUB_LINE_LEN    96000u  /* 2.00 s @ 48 kHz stereo int16 = 384,000 bytes */

typedef struct {
    int16_t line_l[DUB_LINE_LEN];
    int16_t line_r[DUB_LINE_LEN];
} dub_arena_t;

typedef struct {
    uint32_t wr;
    uint32_t valid;         /* O(1) reset: avoids zeroing 384 KB in audio IRQ */
    float    cur_delay;     /* per-sample smoothed delay for buttery tape-style repitch */
    uint8_t  delay_init;
    float    lp1_l, lp2_l;
    float    lp1_r, lp2_r;
    float    lfo_phase1;
    float    lfo_phase2;
} dub_state_t;

/* ============================================================================
 * FX 53: Pitched Echo (48 kHz stereo delay up to 1.0 s + Eventide/WSOLA
 *        correlation-locked stereo pitch shifter in feedback loop)
 *   - Single cubic-interpolated read head 75% of the time (pure vari-speed tape,
 *     zero comb flanging, zero metallic doubling, zero resonant ringing!)
 *   - 25% raised-cosine crossfade to a WSOLA correlation-locked splice head
 *     (matches waveform phase & slope down to ~75 Hz -> zero tremolo/warble!)
 *   - Stereo L/R locked to the exact same splice point (rock-solid stereo image)
 *   - Pre-shifter 2-pole anti-alias LP (clean +7/+12 st shimmer, zero aliasing)
 *   - 100 Hz HPF + user TONE LP in feedback loop, clean phase-free bypass at 0 st
 * ============================================================================ */
#define PITCH_LINE_LEN  48128u  /* 1.0 s @ 48 kHz */
#define PITCH_GRAIN_LEN 8192u   /* 170 ms circular pitch-shifter buffer */

typedef struct {
    int16_t line_l[PITCH_LINE_LEN];
    int16_t line_r[PITCH_LINE_LEN];
    int16_t grain_l[PITCH_GRAIN_LEN];
    int16_t grain_r[PITCH_GRAIN_LEN];
} pitch_arena_t;

typedef struct {
    uint32_t wr;
    uint32_t valid;
    uint32_t g_wr;
    uint32_t g_valid;
    float    cur_delay;     /* per-sample smoothed delay */
    uint8_t  delay_init;
    uint8_t  ps_init;
    float    d_a;           /* active read-head delay [samples] */
    float    d_b;           /* incoming crossfade-head delay [samples] */
    int32_t  xf_pos;        /* <0 when single-head, 0..xf_len during crossfade */
    int32_t  xf_len;        /* crossfade duration in samples */
    float    aa_l1, aa_l2;  /* pre-shifter 2-pole anti-alias LP */
    float    aa_r1, aa_r2;
    float    lp_l, lp_r;    /* user TONE lowpass */
    float    hp_l, hp_r;    /* 100 Hz DC/sub-bass blocker in loop */
} pitch_state_t;

/* Exact half-Hann LUT: 0.5*(1 - cos(2*pi*i/256)) for i=0..128 (symmetric around 128) */
static const float k_hann_half[129] = {
    0.000000f,0.000151f,0.000602f,0.001355f,0.002408f,0.003760f,0.005412f,0.007361f,
    0.009607f,0.012149f,0.014984f,0.018112f,0.021530f,0.025236f,0.029228f,0.033504f,
    0.038060f,0.042895f,0.048005f,0.053388f,0.059039f,0.064957f,0.071136f,0.077573f,
    0.084265f,0.091208f,0.098396f,0.105827f,0.113495f,0.121396f,0.129524f,0.137876f,
    0.146447f,0.155230f,0.164221f,0.173415f,0.182803f,0.192382f,0.202145f,0.212085f,
    0.222197f,0.232474f,0.242910f,0.253498f,0.264232f,0.275104f,0.286109f,0.297238f,
    0.308486f,0.319844f,0.331306f,0.342865f,0.354513f,0.366242f,0.378046f,0.389917f,
    0.401846f,0.413827f,0.425851f,0.437911f,0.450000f,0.462109f,0.474231f,0.486357f,
    0.500000f,0.513643f,0.525769f,0.537891f,0.550000f,0.562089f,0.574149f,0.586173f,
    0.598154f,0.610083f,0.621954f,0.633758f,0.645487f,0.657135f,0.668694f,0.680156f,
    0.691514f,0.702762f,0.713891f,0.724896f,0.735768f,0.746502f,0.757090f,0.767526f,
    0.777803f,0.787915f,0.797855f,0.807618f,0.817197f,0.826585f,0.835779f,0.844770f,
    0.853553f,0.862124f,0.870476f,0.878604f,0.886505f,0.894173f,0.901604f,0.908792f,
    0.915735f,0.922427f,0.928864f,0.935043f,0.940961f,0.946612f,0.951995f,0.957105f,
    0.961940f,0.966496f,0.970772f,0.974764f,0.978470f,0.981888f,0.985016f,0.987851f,
    0.990393f,0.992639f,0.994588f,0.996240f,0.997592f,0.998645f,0.999398f,0.999849f,
    1.000000f
};

/* 4-point Catmull-Rom / Hermite cubic ring read for crystal-clear pitch shifting */
static inline float read_ring_cubic(const int16_t *buf, uint32_t len, uint32_t wr,
                                    uint32_t valid, float delay_samp) {
    if (delay_samp < 2.0f) delay_samp = 2.0f;
    float max_d = (float)(len - 4u);
    if (delay_samp > max_d) delay_samp = max_d;
    uint32_t d_int = (uint32_t)delay_samp;
    if (d_int + 2u >= valid) return 0.0f;
    float t = delay_samp - (float)d_int;
    int32_t i0 = (int32_t)wr - (int32_t)d_int;
    if (i0 < 0) i0 += (int32_t)len;
    int32_t im1 = i0 + 1; if (im1 >= (int32_t)len) im1 -= (int32_t)len;
    int32_t i1  = i0 - 1; if (i1 < 0) i1 += (int32_t)len;
    int32_t i2  = i1 - 1; if (i2 < 0) i2 += (int32_t)len;
    float ym1 = s16_to_f(buf[im1]);
    float y0  = s16_to_f(buf[i0]);
    float y1  = s16_to_f(buf[i1]);
    float y2  = s16_to_f(buf[i2]);
    float c0 = y0;
    float c1 = 0.5f * (y1 - ym1);
    float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

/* Exact 2^(semi/12) for semi = -12..+12 (indices 0..24) */
static const float k_pitch_ratio[25] = {
    0.500000f, 0.529732f, 0.561231f, 0.594604f, 0.629961f, 0.667420f,
    0.707107f, 0.749154f, 0.793701f, 0.840896f, 0.890899f, 0.943874f,
    1.000000f,
    1.059463f, 1.122462f, 1.189207f, 1.259921f, 1.334840f, 1.414214f,
    1.498307f, 1.587401f, 1.681793f, 1.781797f, 1.887749f, 2.000000f
};

typedef union {
    stop_state_t  stop;
    dub_state_t   dub;
    pitch_state_t pitch;
} delay_state_u;

static delay_state_u s_delay[FX_AUDIO_BUSES] __attribute__((section(".fx_bss")));

static void zero_words(void *ptr, uint32_t bytes) {
    uint32_t *p = (uint32_t *)ptr;
    uint32_t n = bytes >> 2;
    for (uint32_t i = 0; i < n; ++i) p[i] = 0;
}

void fx_delay_reset(uint32_t bus_idx, uint32_t fx_id, void *arena) {
    (void)arena; /* O(1) reset via valid sample counters — zero SDRAM memset spike in audio IRQ! */
    if (bus_idx >= FX_AUDIO_BUSES) return;
    zero_words(&s_delay[bus_idx], sizeof(delay_state_u));
    if (fx_id == FX_STOP) {
        uint32_t seed = 0x9E3779B9u ^ (bus_idx * 0x85EBCA6Bu) ^ DWT_CYCCNT ^ (g_fx.block_count * 2654435761u);
        if (seed == 0) seed = 0x12345678u + bus_idx;
        s_delay[bus_idx].stop.rng = seed;
        s_delay[bus_idx].stop.prev_gate = 1.0f;
    } else if (fx_id == FX_DUB_DELAY) {
        s_delay[bus_idx].dub.lfo_phase1 = bus_idx * 0.37f;
        s_delay[bus_idx].dub.lfo_phase2 = bus_idx * 0.61f;
    }
}

static inline uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static inline float read_ring_lin(const int16_t *buf, uint32_t len, uint32_t wr,
                                  uint32_t valid, float delay_samp) {
    if (delay_samp < 1.0f) delay_samp = 1.0f;
    float max_d = (float)(len - 2u);
    if (delay_samp > max_d) delay_samp = max_d;
    uint32_t d_int = (uint32_t)delay_samp;
    if (d_int >= valid) return 0.0f;
    float frac = delay_samp - (float)d_int;
    int32_t idx0 = (int32_t)wr - (int32_t)d_int;
    if (idx0 < 0) idx0 += (int32_t)len;
    int32_t idx1 = idx0 - 1;
    if (idx1 < 0) idx1 += (int32_t)len;
    float s0 = s16_to_f(buf[idx0]);
    float s1 = (d_int + 1u < valid) ? s16_to_f(buf[idx1]) : 0.0f;
    return s0 + frac * (s1 - s0);
}

/* ---- FX 49: STOP ---- */
static void process_stop(stop_state_t *st, stop_arena_t *ar, const fx_bus_ctrl_t *ctrl,
                         float *buf_l, float *buf_r) {
    float target_delay = ((float)ctrl->target[0] * 5.0f) * 24.0f; /* 0..200 (x5ms = 0..1000 ms) @ 24 kHz */
    if (target_delay < 24.0f) target_delay = 24.0f;
    if (target_delay > 24000.0f) target_delay = 24000.0f;
    if (!st->delay_init) { st->cur_delay = target_delay; st->delay_init = 1u; }
    float fb = ctrl->smooth[1] * 0.01f;                  /* 0..0.95 */
    float mix = ctrl->smooth[2] * 0.01f;                 /* 0..1.0 */
    float drop_amt = ctrl->smooth[3] * 0.01f;            /* 0..1.0 */
    int   gate_dry = (ctrl->target[3] >= 100);           /* 100% override: breakdown gate hits dry too */
    float tele = ctrl->smooth[4] * 0.01f;                /* 0..1.0 */
    int   bits = 9 - (int)(ctrl->target[5]);             /* 9..4 bits */
    if (bits < 4) bits = 4;
    if (bits > 9) bits = 9;
    float q_steps = (float)(1u << (uint32_t)(bits - 1));
    float q_inv = 1.0f / q_steps;

    /* Process 64 frames @ 48 kHz = 32 decimated frames @ 24 kHz */
    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; i += 2u) {
        /* Per-sample smooth delay time glide (~45 ms @ 24 kHz) */
        st->cur_delay += 0.0009f * (target_delay - st->cur_delay);
        float delay_samp = st->cur_delay;
        /* Push 2 input samples into 16-sample circular FIR history */
        uint8_t p0 = st->fir_pos;
        st->fir_l[p0] = buf_l[i];
        st->fir_r[p0] = buf_r[i];
        uint8_t p1 = (uint8_t)((p0 + 1u) & 15u);
        st->fir_l[p1] = buf_l[i + 1u];
        st->fir_r[p1] = buf_r[i + 1u];
        st->fir_pos = (uint8_t)((p0 + 2u) & 15u);

        /* 15-tap anti-alias FIR (only non-zero taps = 9 MACs per channel!) */
        float in_l = 0.0f, in_r = 0.0f;
        for (uint32_t t = 0; t < STOP_FIR_TAPS; ++t) {
            float c = k_stop_fir[t];
            if (c == 0.0f) continue;
            uint32_t idx = (p1 - t) & 15u;
            in_l += c * st->fir_l[idx];
            in_r += c * st->fir_r[idx];
        }

        /* Read delayed stereo @ 24 kHz */
        float dl = read_ring_lin(ar->line_l, STOP_LINE_LEN, st->wr, st->valid, delay_samp);
        float dr = read_ring_lin(ar->line_r, STOP_LINE_LEN, st->wr, st->valid, delay_samp);

        /* 9->4 bit quantizer in the feedback loop */
        float ql = (float)(int32_t)(dl * q_steps + (dl >= 0.0f ? 0.5f : -0.5f)) * q_inv;
        float qr = (float)(int32_t)(dr * q_steps + (dr >= 0.0f ? 0.5f : -0.5f)) * q_inv;

        /* Telephone bandpass (HP 300 Hz -> LP 3.4 kHz) inside feedback loop */
        float hpl = TELE_HP_B0 * ql + st->hp_l1;
        st->hp_l1 = TELE_HP_B1 * ql - TELE_HP_A1 * hpl + st->hp_l2;
        st->hp_l2 = TELE_HP_B2 * ql - TELE_HP_A2 * hpl;
        float bpl = TELE_LP_B0 * hpl + st->lp_l1;
        st->lp_l1 = TELE_LP_B1 * hpl - TELE_LP_A1 * bpl + st->lp_l2;
        st->lp_l2 = TELE_LP_B2 * hpl - TELE_LP_A2 * bpl;

        float hpr = TELE_HP_B0 * qr + st->hp_r1;
        st->hp_r1 = TELE_HP_B1 * qr - TELE_HP_A1 * hpr + st->hp_r2;
        st->hp_r2 = TELE_HP_B2 * qr - TELE_HP_A2 * hpr;
        float bpr = TELE_LP_B0 * hpr + st->lp_r1;
        st->lp_r1 = TELE_LP_B1 * hpr - TELE_LP_A1 * bpr + st->lp_r2;
        st->lp_r2 = TELE_LP_B2 * hpr - TELE_LP_A2 * bpr;

        float fl = ql + tele * (bpl - ql);
        float fr = qr + tele * (bpr - qr);

        /* Write input + feedback into 24 kHz delay line with musical soft-clip */
        ar->line_l[st->wr] = f_to_s16(soft_clip(in_l + fl * fb));
        ar->line_r[st->wr] = f_to_s16(soft_clip(in_r + fr * fb));
        if (++st->wr >= STOP_LINE_LEN) st->wr = 0;
        if (st->valid < STOP_LINE_LEN) st->valid++;

        /* Dropout gate: evaluated every 10 ms (240 samples @ 24 kHz) */
        if (st->drop_rem <= 0) {
            if (++st->tick_cnt >= 240u) {
                st->tick_cnt = 0;
                if (drop_amt > 0.005f) {
                    uint32_t r = xorshift32(&st->rng);
                    /* Probability scales smoothly up to ~22% per 10 ms tick at max */
                    float prob = drop_amt * drop_amt * 0.22f;
                    uint32_t thresh = (uint32_t)(prob * 65536.0f);
                    if ((r & 0xFFFFu) < thresh) {
                        /* Duration: 20..200 ms (480..4800 samples @ 24 kHz) */
                        uint32_t min_d = 480u;
                        uint32_t max_d = 480u + (uint32_t)(drop_amt * 4320.0f);
                        uint32_t span = (max_d > min_d) ? (max_d - min_d) : 1u;
                        st->drop_len = (int32_t)(min_d + ((r >> 16) % span));
                        st->drop_rem = st->drop_len;
                        st->drop_pos = 0;
                    }
                }
            }
        }

        float gate = 1.0f;
        if (st->drop_rem > 0) {
            /* 5 ms (120-sample) raised-cosine fade out and fade in */
            const int32_t fade = 120;
            if (st->drop_pos < fade) {
                float u = (float)st->drop_pos * (1.0f / (float)fade);
                gate = 1.0f - fast_hann01(u * 0.5f);
            } else if (st->drop_rem <= fade) {
                float u = (float)st->drop_rem * (1.0f / (float)fade);
                gate = 1.0f - fast_hann01(u * 0.5f);
            } else {
                gate = 0.0f;
            }
            st->drop_pos++;
            st->drop_rem--;
        }

        /* Linear 2x upsample wet signal back to 48 kHz and mix */
        float wet_l = fl * gate;
        float wet_r = fr * gate;
        float mid_l = 0.5f * (st->prev_wet_l + wet_l);
        float mid_r = 0.5f * (st->prev_wet_r + wet_r);
        float mid_g = 0.5f * (st->prev_gate + gate);
        st->prev_wet_l = wet_l;
        st->prev_wet_r = wet_r;
        st->prev_gate = gate;

        float dg0 = gate_dry ? mid_g : 1.0f;
        float dg1 = gate_dry ? gate : 1.0f;
        float dry_gain = 1.0f - mix;
        buf_l[i]      = buf_l[i]      * dry_gain * dg0 + mid_l * mix;
        buf_r[i]      = buf_r[i]      * dry_gain * dg0 + mid_r * mix;
        buf_l[i + 1u] = buf_l[i + 1u] * dry_gain * dg1 + wet_l * mix;
        buf_r[i + 1u] = buf_r[i + 1u] * dry_gain * dg1 + wet_r * mix;
    }
}

/* ---- FX 50: Dub Delay ---- */
static void process_dub(dub_state_t *st, dub_arena_t *ar, const fx_bus_ctrl_t *ctrl,
                        float *buf_l, float *buf_r, uint32_t bpm100) {
    static const float k_sync_mult[6] = { 0.0f, 1.0f, 0.5f, 0.3333333f, 0.25f, 0.125f };
    int sync = (int)ctrl->target[5];
    float target_delay;
    if (sync >= 1 && sync <= 5) {
        float bpm = (bpm100 >= 4000u && bpm100 <= 24000u) ? ((float)bpm100 * 0.01f) : 120.0f;
        target_delay = (48000.0f * 60.0f / bpm) * k_sync_mult[sync];
    } else {
        target_delay = ((float)ctrl->target[0] * 10.0f) * 48.0f; /* 0..200 (x10ms = 0..2000 ms) @ 48 kHz */
    }
    if (target_delay < 48.0f) target_delay = 48.0f;
    if (target_delay > 95500.0f) target_delay = 95500.0f;
    if (!st->delay_init) { st->cur_delay = target_delay; st->delay_init = 1u; }

    float fb = ctrl->smooth[1] * 0.01f;       /* 0..0.98 */
    float mix = ctrl->smooth[2] * 0.01f;      /* 0..1.0 */
    float lp_a = tone_to_lp_alpha(ctrl->smooth[3] * 0.01f, 0.063f, 0.92f);
    float mod_depth = ctrl->smooth[4] * 0.01f * 144.0f; /* up to +/-3.0 ms wow/flutter */

    const float inc1 = 0.30f / 48000.0f;      /* 0.30 Hz slow tape wow */
    const float inc2 = 1.73f / 48000.0f;      /* 1.73 Hz subtle flutter */
    float dry_gain = 1.0f - mix;

    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; ++i) {
        /* Per-sample smooth exponential tape glide (~50 ms time constant, zero 750 Hz zipper!) */
        st->cur_delay += 0.00042f * (target_delay - st->cur_delay);
        float delay_samp = st->cur_delay;
        st->lfo_phase1 += inc1;
        if (st->lfo_phase1 >= 1.0f) st->lfo_phase1 -= 1.0f;
        st->lfo_phase2 += inc2;
        if (st->lfo_phase2 >= 1.0f) st->lfo_phase2 -= 1.0f;

        float mod = mod_depth * (0.80f * fast_sin01(st->lfo_phase1)
                               + 0.20f * fast_sin01(st->lfo_phase2));
        float dl = read_ring_lin(ar->line_l, DUB_LINE_LEN, st->wr, st->valid, delay_samp + mod);
        float dr = read_ring_lin(ar->line_r, DUB_LINE_LEN, st->wr, st->valid, delay_samp - mod * 0.85f);

        /* Cascaded 2-pole lowpass in feedback loop for classic dub darkening */
        st->lp1_l += lp_a * (dl - st->lp1_l);
        st->lp2_l += lp_a * (st->lp1_l - st->lp2_l);
        st->lp1_r += lp_a * (dr - st->lp1_r);
        st->lp2_r += lp_a * (st->lp1_r - st->lp2_r);

        /* Subtle 12% stereo crossfeed + warm tape saturation in loop */
        float fl = st->lp2_l * 0.88f + st->lp2_r * 0.12f;
        float fr = st->lp2_r * 0.88f + st->lp2_l * 0.12f;

        float in_l = buf_l[i];
        float in_r = buf_r[i];
        ar->line_l[st->wr] = f_to_s16(soft_clip(in_l + fl * fb));
        ar->line_r[st->wr] = f_to_s16(soft_clip(in_r + fr * fb));
        if (++st->wr >= DUB_LINE_LEN) st->wr = 0;
        if (st->valid < DUB_LINE_LEN) st->valid++;

        buf_l[i] = in_l * dry_gain + st->lp2_l * mix;
        buf_r[i] = in_r * dry_gain + st->lp2_r * mix;
    }
}

/* Fast integer-MAC WSOLA correlation splice finder: locates the lag in [-240..+240]
 * samples around d_nom that best phase-locks incoming Head B to outgoing Head A.
 * Uses 16-tap mono template (stride 8 = 128 samples), 41 coarse lags (step 12) +
 * 11 fine lags (step 1) -> ~830 integer MACs (< 3% of one block on splice trigger!). */
static int32_t pitch_find_best_lag(const pitch_arena_t *ar, uint32_t g_wr, uint32_t g_valid,
                                   float d_cur, float d_nom) {
    const uint32_t mask = PITCH_GRAIN_LEN - 1u; /* 8191 (power of 2!) */
    int32_t max_d = (int32_t)(g_valid > (PITCH_GRAIN_LEN - 200u) ? (PITCH_GRAIN_LEN - 200u) : (g_valid > 200u ? g_valid - 200u : 0u));
    if (max_d < 64) return 0;

    int32_t di_a = (int32_t)d_cur;
    int32_t tmpl[16];
    int32_t ea = 0;
    for (uint32_t k = 0; k < 16u; ++k) {
        uint32_t idx = (g_wr - (uint32_t)di_a - (k << 3)) & mask;
        int32_t s = ((int32_t)ar->grain_l[idx] + (int32_t)ar->grain_r[idx]) >> 5; /* [-2048..2047] */
        tmpl[k] = s;
        ea += s * s;
    }
    /* Skip search on near-silence */
    if (ea < 64) return 0;

    int32_t best_lag = 0;
    float best_score = -1e30f;

    /* Coarse pass: [-240..+240] step 12 (41 candidates) */
    for (int32_t lag = -240; lag <= 240; lag += 12) {
        int32_t cand = (int32_t)d_nom + lag;
        if (cand < 16 || cand > max_d) continue;
        int32_t dot = 0;
        int32_t eb = 1;
        for (uint32_t k = 0; k < 16u; ++k) {
            uint32_t idx = (g_wr - (uint32_t)cand - (k << 3)) & mask;
            int32_t s = ((int32_t)ar->grain_l[idx] + (int32_t)ar->grain_r[idx]) >> 5;
            dot += tmpl[k] * s;
            eb += s * s;
        }
        float fd = (float)dot;
        float sc = (fd >= 0.0f) ? ((fd * fd) / (float)eb) : (-(fd * fd) / (float)eb);
        if (sc > best_score) {
            best_score = sc;
            best_lag = lag;
        }
    }

    /* Fine pass: [best_lag - 6 .. best_lag + 6] step 1 (13 candidates) */
    int32_t center = best_lag;
    for (int32_t lag = center - 6; lag <= center + 6; ++lag) {
        int32_t cand = (int32_t)d_nom + lag;
        if (cand < 16 || cand > max_d) continue;
        int32_t dot = 0;
        int32_t eb = 1;
        for (uint32_t k = 0; k < 16u; ++k) {
            uint32_t idx = (g_wr - (uint32_t)cand - (k << 3)) & mask;
            int32_t s = ((int32_t)ar->grain_l[idx] + (int32_t)ar->grain_r[idx]) >> 5;
            dot += tmpl[k] * s;
            eb += s * s;
        }
        float fd = (float)dot;
        float sc = (fd >= 0.0f) ? ((fd * fd) / (float)eb) : (-(fd * fd) / (float)eb);
        if (sc > best_score) {
            best_score = sc;
            best_lag = lag;
        }
    }
    return best_lag;
}

/* Rising half-Hann crossfade 0.5*(1 - cos(pi*u)) on u in [0..1] via k_hann_half */
static inline float hann_xfade_up(float u) {
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    float q = u * 128.0f;
    int idx = (int)q;
    if (idx >= 128) return 1.0f;
    float frac = q - (float)idx;
    return k_hann_half[idx] + frac * (k_hann_half[idx + 1] - k_hann_half[idx]);
}

/* ---- FX 53: Pitched Echo ---- */
static void process_pitched(pitch_state_t *st, pitch_arena_t *ar, const fx_bus_ctrl_t *ctrl,
                            float *buf_l, float *buf_r) {
    float target_delay = ((float)ctrl->target[0] * 5.0f) * 48.0f; /* 0..200 (x5ms = 0..1000 ms) @ 48 kHz */
    if (target_delay < 48.0f) target_delay = 48.0f;
    if (target_delay > 48000.0f) target_delay = 48000.0f;
    if (!st->delay_init) { st->cur_delay = target_delay; st->delay_init = 1u; }
    float fb = ctrl->smooth[1] * 0.01f;         /* 0..0.90 */
    int   p_idx = (int)ctrl->target[2];         /* 0..24 -> -12..+12 st */
    if (p_idx < 0) p_idx = 0;
    if (p_idx > 24) p_idx = 24;
    float ratio = k_pitch_ratio[p_idx];
    float mix = ctrl->smooth[3] * 0.01f;        /* 0..1.0 */
    float lp_a = tone_to_lp_alpha(ctrl->smooth[4] * 0.01f, 0.10f, 0.90f);
    float grain_len = ctrl->smooth[5] * 48.0f;  /* 20..100 ms = 960..4800 samples */
    if (grain_len < 960.0f) grain_len = 960.0f;
    if (grain_len > 4800.0f) grain_len = 4800.0f;

    /* Pre-shifter 2-pole anti-alias LP only when pitching UP (prevents foldover aliasing) */
    float aa_a = (ratio > 1.0f) ? (0.82f / ratio) : 0.96f;
    float dry_gain = 1.0f - mix;
    int   unshifted = (p_idx == 12);            /* 0 st: pure phase-free delay */

    int32_t xf_target = (int32_t)(grain_len * 0.25f);
    if (xf_target < 240) xf_target = 240;
    if (xf_target > 720) xf_target = 720;

    if (!st->ps_init) {
        st->d_a = (ratio > 1.0f) ? (grain_len * 0.6f) : 48.0f;
        st->d_b = st->d_a;
        st->xf_pos = -1;
        st->xf_len = xf_target;
        st->ps_init = 1u;
    }

    float inc = 1.0f - ratio;

    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; ++i) {
        st->cur_delay += 0.00042f * (target_delay - st->cur_delay);
        float delay_samp = st->cur_delay;

        /* Read delayed stereo from main delay line */
        float dl = read_ring_lin(ar->line_l, PITCH_LINE_LEN, st->wr, st->valid, delay_samp);
        float dr = read_ring_lin(ar->line_r, PITCH_LINE_LEN, st->wr, st->valid, delay_samp);

        /* Pre-shifter 2-pole anti-aliasing LP */
        st->aa_l1 += aa_a * (dl - st->aa_l1);
        st->aa_l2 += aa_a * (st->aa_l1 - st->aa_l2);
        st->aa_r1 += aa_a * (dr - st->aa_r1);
        st->aa_r2 += aa_a * (st->aa_r1 - st->aa_r2);

        /* Push band-limited delayed signal into circular WSOLA buffer */
        ar->grain_l[st->g_wr] = f_to_s16(st->aa_l2);
        ar->grain_r[st->g_wr] = f_to_s16(st->aa_r2);

        float pl, pr;
        if (unshifted) {
            pl = dl;
            pr = dr;
        } else if (st->xf_pos < 0) {
            /* Single pure cubic-interpolated vari-speed head (75% of time: zero comb flanging!) */
            pl = read_ring_cubic(ar->grain_l, PITCH_GRAIN_LEN, st->g_wr, st->g_valid, st->d_a);
            pr = read_ring_cubic(ar->grain_r, PITCH_GRAIN_LEN, st->g_wr, st->g_valid, st->d_a);
            st->d_a += inc;

            /* Trigger correlation-locked splice before hitting buffer edge */
            if (ratio > 1.0f && st->d_a <= (float)xf_target * (ratio - 1.0f) + 24.0f) {
                float d_nom = st->d_a + grain_len;
                int32_t lag = (st->g_valid > (uint32_t)grain_len + 400u)
                    ? pitch_find_best_lag(ar, st->g_wr, st->g_valid, st->d_a, d_nom) : 0;
                st->d_b = d_nom + (float)lag;
                if (st->d_b < 24.0f) st->d_b = 24.0f;
                if (st->d_b > (float)(PITCH_GRAIN_LEN - 32u)) st->d_b = (float)(PITCH_GRAIN_LEN - 32u);
                st->xf_len = xf_target;
                st->xf_pos = 0;
            } else if (ratio < 1.0f && st->d_a >= grain_len + 48.0f) {
                float d_nom = st->d_a - grain_len;
                if (d_nom < 32.0f) d_nom = 32.0f;
                int32_t lag = (st->g_valid > (uint32_t)grain_len + 400u)
                    ? pitch_find_best_lag(ar, st->g_wr, st->g_valid, st->d_a, d_nom) : 0;
                st->d_b = d_nom + (float)lag;
                if (st->d_b < 24.0f) st->d_b = 24.0f;
                if (st->d_b > (float)(PITCH_GRAIN_LEN - 32u)) st->d_b = (float)(PITCH_GRAIN_LEN - 32u);
                st->xf_len = xf_target;
                st->xf_pos = 0;
            }
        } else {
            /* Phase-locked raised-cosine crossfade between Head A and Head B (25% of time) */
            float u = (float)st->xf_pos / (float)st->xf_len;
            float wb = hann_xfade_up(u);
            float wa = 1.0f - wb;
            float pal = read_ring_cubic(ar->grain_l, PITCH_GRAIN_LEN, st->g_wr, st->g_valid, st->d_a);
            float par = read_ring_cubic(ar->grain_r, PITCH_GRAIN_LEN, st->g_wr, st->g_valid, st->d_a);
            float pbl = read_ring_cubic(ar->grain_l, PITCH_GRAIN_LEN, st->g_wr, st->g_valid, st->d_b);
            float pbr = read_ring_cubic(ar->grain_r, PITCH_GRAIN_LEN, st->g_wr, st->g_valid, st->d_b);
            pl = wa * pal + wb * pbl;
            pr = wa * par + wb * pbr;
            st->d_a += inc;
            st->d_b += inc;
            if (++st->xf_pos >= st->xf_len) {
                st->d_a = st->d_b;
                st->xf_pos = -1;
            }
        }

        st->g_wr = (st->g_wr + 1u) & (PITCH_GRAIN_LEN - 1u);
        if (st->g_valid < PITCH_GRAIN_LEN) st->g_valid++;

        /* User TONE lowpass + 100 Hz HPF in loop (clean shimmer highs, zero -12st mud) */
        st->lp_l += lp_a * (pl - st->lp_l);
        st->lp_r += lp_a * (pr - st->lp_r);
        st->hp_l += 0.013f * (st->lp_l - st->hp_l);
        st->hp_r += 0.013f * (st->lp_r - st->hp_r);
        float fl = st->lp_l - st->hp_l;
        float fr = st->lp_r - st->hp_r;

        /* Gentle 10% stereo crossfeed for lush cascading shimmer tails */
        float xl = fl * 0.90f + fr * 0.10f;
        float xr = fr * 0.90f + fl * 0.10f;

        float in_l = buf_l[i];
        float in_r = buf_r[i];
        ar->line_l[st->wr] = f_to_s16(soft_clip(in_l + xl * fb));
        ar->line_r[st->wr] = f_to_s16(soft_clip(in_r + xr * fb));
        if (++st->wr >= PITCH_LINE_LEN) st->wr = 0;
        if (st->valid < PITCH_LINE_LEN) st->valid++;

        buf_l[i] = in_l * dry_gain + fl * mix;
        buf_r[i] = in_r * dry_gain + fr * mix;
    }
}

void fx_delay_process(uint32_t bus_idx, uint32_t fx_id, fx_bus_ctrl_t *ctrl,
                      void *arena, float *buf_l, float *buf_r, uint32_t bpm100) {
    if (bus_idx >= FX_AUDIO_BUSES) return;
    if (fx_id == FX_STOP) {
        process_stop(&s_delay[bus_idx].stop, (stop_arena_t *)arena, ctrl, buf_l, buf_r);
    } else if (fx_id == FX_DUB_DELAY) {
        process_dub(&s_delay[bus_idx].dub, (dub_arena_t *)arena, ctrl, buf_l, buf_r, bpm100);
    } else if (fx_id == FX_PITCHED_ECHO) {
        process_pitched(&s_delay[bus_idx].pitch, (pitch_arena_t *)arena, ctrl, buf_l, buf_r);
    }
}
