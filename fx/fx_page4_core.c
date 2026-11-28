#include "fx_engine.h"

/* ============================================================================
 * Page-4 Creative & Utility DSP Engines (FX 56..60)
 *   56: SHIMMER    — WSOLA pitch-shifted Freeverb-style shimmer reverb
 *   57: GEN LOSS   — Broken VHS/Walkman wow, flutter, oxide dropouts, sat, hiss
 *   58: CHORD RESO — 4-voice Karplus-Strong tuned modal chord resonator bank
 *   59: TRANSIENT  — SPL-style dual-envelope Attack/Sustain shaper + Punch/Drive
 *   60: MICROLOOP  — Beat-synced/free micro-looper (Fwd/Rev/Pong/Half/Dbl/R-Half)
 *
 * Shared zero-libc float32 helpers keep flash footprint tiny (~7 KB total for
 * all 5 engines) and all arena resets O(1) via valid counters (zero IRQ spike).
 * ============================================================================ */

static inline float p4_tanh(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static inline int16_t p4_f_to_s16(float x) {
    float s = x * 32767.0f;
    if (s > 32767.0f) return 32767;
    if (s < -32768.0f) return -32768;
    return (int16_t)s;
}

static inline float p4_s16_to_f(int16_t s) {
    return (float)s * (1.0f / 32768.0f);
}

static inline uint32_t p4_xorshift(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static inline float p4_sin01(float p) {
    p -= (float)(int)p;
    if (p < 0.0f) p += 1.0f;
    float x = p * 2.0f - 1.0f;
    float y = 4.0f * x * (1.0f - (x < 0.0f ? -x : x));
    return -y * (0.775f + 0.225f * (y < 0.0f ? -y : y));
}

static inline float p4_hann_up(float u) {
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    /* 0.5 * (1 - cos(pi*u)) = 0.5 * (1 - sin(2*pi*(0.5*u + 0.25))) */
    return 0.5f * (1.0f - p4_sin01(u * 0.5f + 0.25f));
}

static inline float p4_read_s16_lin(const int16_t *buf, uint32_t mask, uint32_t wr,
                                    uint32_t valid, float delay) {
    if (delay < 1.0f) delay = 1.0f;
    if (delay > (float)(mask - 2u)) delay = (float)(mask - 2u);
    uint32_t di = (uint32_t)delay;
    if (di + 1u >= valid) return 0.0f;
    float frac = delay - (float)di;
    uint32_t i0 = (wr - di) & mask;
    uint32_t i1 = (i0 - 1u) & mask;
    float s0 = p4_s16_to_f(buf[i0]);
    float s1 = p4_s16_to_f(buf[i1]);
    return s0 + frac * (s1 - s0);
}

static const float k_p4_pitch[25] = {
    0.500000f, 0.529732f, 0.561231f, 0.594604f, 0.629961f, 0.667420f,
    0.707107f, 0.749154f, 0.793701f, 0.840896f, 0.890899f, 0.943874f,
    1.000000f,
    1.059463f, 1.122462f, 1.189207f, 1.259921f, 1.334840f, 1.414214f,
    1.498307f, 1.587401f, 1.681793f, 1.781797f, 1.887749f, 2.000000f
};

/* ============================================================================
 * 56: SHIMMER (Pitch-Shifted Reverb)
 *   - 4 LP combs + 2 canonical allpasses per L/R tank
 *   - Phase-locked WSOLA pitch shifter on the tank return feeding back into tank
 *   - 140 Hz shimmer HPF + soft-clip safety so high SHIMMER+DECAY blooms cleanly
 * ============================================================================ */
#define SH_C0L 1217u
#define SH_C1L 1301u
#define SH_C2L 1399u
#define SH_C3L 1487u
#define SH_C0R 1237u
#define SH_C1R 1327u
#define SH_C2R 1423u
#define SH_C3R 1511u
#define SH_A0L 245u
#define SH_A1L 607u
#define SH_A0R 269u
#define SH_A1R 631u
#define SH_GLEN 4096u  /* power-of-2 WSOLA shifter buffer (~85 ms) */

typedef struct {
    float   cl0[SH_C0L], cl1[SH_C1L], cl2[SH_C2L], cl3[SH_C3L];
    float   cr0[SH_C0R], cr1[SH_C1R], cr2[SH_C2R], cr3[SH_C3R];
    float   al0[SH_A0L], al1[SH_A1L];
    float   ar0[SH_A0R], ar1[SH_A1R];
    int16_t gl[SH_GLEN];
    int16_t gr[SH_GLEN];
} shimmer_arena_t;

typedef struct {
    uint32_t valid;
    uint32_t g_wr;
    uint32_t g_valid;
    uint32_t cl_i[4], cr_i[4];
    uint32_t al_i[2], ar_i[2];
    float    cl_f[4], cr_f[4];
    float    d_a, d_b;
    int32_t  xf_pos, xf_len;
    uint8_t  ps_init;
    float    sh_fb_l, sh_fb_r;
    float    aa_l, aa_r;
    float    hp_l, hp_r;
} shimmer_state_t;

/* ============================================================================
 * 57: GEN LOSS (Broken VHS / Walkman Tape Degradation)
 * ============================================================================ */
#define GL_BUF_LEN 2048u  /* ~42.6 ms stereo tape delay (power of 2) */

typedef struct {
    int16_t line_l[GL_BUF_LEN];
    int16_t line_r[GL_BUF_LEN];
} genloss_arena_t;

typedef struct {
    uint32_t wr;
    uint32_t valid;
    uint32_t rng;
    uint32_t tick_cnt;
    float    wow_ph;
    float    flt_ph1, flt_ph2;
    float    drift;
    /* Random tape failure / oxide crinkle envelope */
    int32_t  fail_rem;
    float    fail_gain_l, fail_gain_r;
    float    fail_lp_l, fail_lp_r;
    float    fail_az;
    float    cur_gain_l, cur_gain_r;
    float    cur_az;
    /* Oxide crinkle LP + Bandwidth HP/LP + Pink noise state */
    float    ox_l, ox_r;
    float    hp_xl, hp_yl, hp_xr, hp_yr;
    float    lp1_l, lp2_l, lp1_r, lp2_r;
    float    pink_l, pink_r;
} genloss_state_t;

/* ============================================================================
 * 58: CHORD RESO (4-Voice Karplus-Strong Tuned Comb Chord Bank)
 * ============================================================================ */
#define CR_VOICES  4u
#define CR_BUF_LEN 1024u  /* > 15.3 ms -> supports down to 48 Hz (C2 = 65.41 Hz = 733.8 samp) */

typedef struct {
    float buf[CR_VOICES][CR_BUF_LEN];
} chord_arena_t;

typedef struct {
    uint32_t wr[CR_VOICES];
    uint32_t valid;
    float    lp[CR_VOICES];
    float    dc_x[CR_VOICES];
    float    dc_y[CR_VOICES];
    float    cur_del[CR_VOICES];
    uint8_t  init_done;
} chord_state_t;

/* ============================================================================
 * 59: TRANSIENT (SPL-Style Dual-Envelope Transient Shaper + Punch/Drive/Tone)
 * ============================================================================ */
typedef struct {
    float env_fast;
    float env_med;
    float env_slow;
    float gain_sm;
    float punch_lp_l, punch_lp_r;
    float tilt_lp_l, tilt_lp_r;
} transient_state_t;

/* ============================================================================
 * 60: MICROLOOP (Beat-Synced / Free Micro-Looper)
 * ============================================================================ */
#define ML_BUF_LEN 48000u /* 1.00 s @ 48 kHz stereo int16 = 192,000 B */

typedef struct {
    int16_t cap_l[ML_BUF_LEN];
    int16_t cap_r[ML_BUF_LEN];
} microloop_arena_t;

typedef struct {
    uint32_t wr;
    uint32_t valid;
    uint32_t rng;
    float    rd_pos;      /* current read cursor inside capture ring */
    float    slice_start; /* anchor point of current micro-slice */
    float    slice_age;   /* progress [0..slice_len) within current slice */
    float    slice_len;   /* active slice length in samples */
    int8_t   dir;         /* +1 forward, -1 reverse */
    uint8_t  holding;     /* 1 = repeating captured slice, 0 = refreshing slice */
    uint8_t  init_done;
    /* Smooth crossfade from previous slice on retrigger */
    float    old_pos;
    float    old_rate;
    int32_t  xf_rem;
    int32_t  xf_tot;
} microloop_state_t;

typedef union {
    shimmer_state_t   shimmer;
    genloss_state_t   genloss;
    chord_state_t     chord;
    transient_state_t trans;
    microloop_state_t mloop;
} page4_state_u;

static page4_state_u s_p4[FX_AUDIO_BUSES] __attribute__((section(".fx_bss")));

#include "fx_atmosphere.inc"

void fx_page4_reset(uint32_t bus_idx, uint32_t fx_id, void *arena) {
    if (fx_id >= FX_GHOST_PRINT) {
        atmosphere_t *state = arena;
        /* Clear only metadata. The history is guarded by valid. */
        volatile unsigned char *byte = (volatile unsigned char *)&state->filter;
        for (unsigned i = 0; i < sizeof(*state)-sizeof(state->history); i++)
            byte[i] = 0;
        return;
    }

    (void)arena;
    if (bus_idx >= FX_AUDIO_BUSES) return;
    uint32_t *p = (uint32_t *)&s_p4[bus_idx];
    for (uint32_t i = 0; i < sizeof(page4_state_u) / 4u; ++i) p[i] = 0;
    uint32_t seed = 0xC0FFEE11u ^ (bus_idx * 0x9E3779B9u) ^ (fx_id * 0x85EBCA6Bu) ^ DWT_CYCCNT ^ (g_fx.block_count * 2654435761u);
    if (seed == 0) seed = 0x517CC1B7u + bus_idx;
    if (fx_id == FX_GEN_LOSS) {
        s_p4[bus_idx].genloss.rng = seed;
        s_p4[bus_idx].genloss.fail_gain_l = 1.0f;
        s_p4[bus_idx].genloss.fail_gain_r = 1.0f;
        s_p4[bus_idx].genloss.cur_gain_l = 1.0f;
        s_p4[bus_idx].genloss.cur_gain_r = 1.0f;
    } else if (fx_id == FX_TRANSIENT) {
        s_p4[bus_idx].trans.gain_sm = 1.0f;
    } else if (fx_id == FX_MICROLOOP) {
        s_p4[bus_idx].mloop.rng = seed;
        s_p4[bus_idx].mloop.dir = 1;
    }
}

/* ---- Helpers for FX 56 SHIMMER ---- */
static inline float sh_comb(float *buf, uint32_t len, uint32_t *idx, float *flt,
                            uint32_t valid, float in, float fb, float d1, float d2) {
    uint32_t i = *idx;
    float out = (valid >= len) ? buf[i] : 0.0f;
    float f = out * d2 + (*flt) * d1;
    *flt = f;
    buf[i] = in + f * fb;
    if (++i >= len) i = 0;
    *idx = i;
    return out;
}

static inline float sh_ap(float *buf, uint32_t len, uint32_t *idx, uint32_t valid, float in) {
    uint32_t i = *idx;
    float d = (valid >= len) ? buf[i] : 0.0f;
    float out = d - 0.5f * in;
    buf[i] = in + 0.5f * out; /* canonical unit-gain allpass */
    if (++i >= len) i = 0;
    *idx = i;
    return out;
}

static int32_t sh_find_lag(const shimmer_arena_t *ar, uint32_t g_wr, uint32_t g_valid,
                           float d_cur, float d_nom) {
    const uint32_t mask = SH_GLEN - 1u;
    int32_t max_d = (int32_t)(g_valid > (SH_GLEN - 160u) ? (SH_GLEN - 160u) : (g_valid > 160u ? g_valid - 160u : 0u));
    if (max_d < 64) return 0;
    int32_t di_a = (int32_t)d_cur;
    int32_t tmpl[16];
    int32_t ea = 0;
    for (uint32_t k = 0; k < 16u; ++k) {
        uint32_t idx = (g_wr - (uint32_t)di_a - (k << 3)) & mask;
        int32_t s = ((int32_t)ar->gl[idx] + (int32_t)ar->gr[idx]) >> 5;
        tmpl[k] = s;
        ea += s * s;
    }
    if (ea < 64) return 0;
    int32_t best_lag = 0;
    float best_sc = -1e30f;
    for (int32_t lag = -192; lag <= 192; lag += 12) {
        int32_t cand = (int32_t)d_nom + lag;
        if (cand < 16 || cand > max_d) continue;
        int32_t dot = 0, eb = 1;
        for (uint32_t k = 0; k < 16u; ++k) {
            uint32_t idx = (g_wr - (uint32_t)cand - (k << 3)) & mask;
            int32_t s = ((int32_t)ar->gl[idx] + (int32_t)ar->gr[idx]) >> 5;
            dot += tmpl[k] * s;
            eb += s * s;
        }
        float fd = (float)dot;
        float sc = (fd >= 0.0f) ? ((fd * fd) / (float)eb) : (-(fd * fd) / (float)eb);
        if (sc > best_sc) { best_sc = sc; best_lag = lag; }
    }
    int32_t c0 = best_lag;
    for (int32_t lag = c0 - 5; lag <= c0 + 5; ++lag) {
        int32_t cand = (int32_t)d_nom + lag;
        if (cand < 16 || cand > max_d) continue;
        int32_t dot = 0, eb = 1;
        for (uint32_t k = 0; k < 16u; ++k) {
            uint32_t idx = (g_wr - (uint32_t)cand - (k << 3)) & mask;
            int32_t s = ((int32_t)ar->gl[idx] + (int32_t)ar->gr[idx]) >> 5;
            dot += tmpl[k] * s;
            eb += s * s;
        }
        float fd = (float)dot;
        float sc = (fd >= 0.0f) ? ((fd * fd) / (float)eb) : (-(fd * fd) / (float)eb);
        if (sc > best_sc) { best_sc = sc; best_lag = lag; }
    }
    return best_lag;
}

static void process_shimmer(shimmer_state_t *st, shimmer_arena_t *ar, const fx_bus_ctrl_t *ctrl,
                            float *buf_l, float *buf_r) {
    /* Knobs: 0=SHIMMER, 1=PITCH(0..24), 2=SIZE, 3=DECAY, 4=DAMP, 5=MIX */
    float shim  = ctrl->smooth[0] * 0.01f;
    int   p_idx = (int)ctrl->target[1];
    if (p_idx < 0) p_idx = 0;
    if (p_idx > 24) p_idx = 24;
    float ratio = k_p4_pitch[p_idx];
    float size  = ctrl->smooth[2] * 0.01f;
    float decay = ctrl->smooth[3] * 0.01f;
    float damp  = ctrl->smooth[4] * 0.01f;
    float mix   = ctrl->smooth[5] * 0.01f;
    int   zero_mix = (ctrl->target[5] == 0 && mix < 0.002f);

    float room = 0.35f * size + 0.65f * decay;
    float fb = 0.58f + room * 0.37f;          /* 0.58 .. 0.95 (leaves headroom for shimmer feedback) */
    float d1 = 0.06f + damp * 0.68f;
    float d2 = 1.0f - d1;
    float shim_inj = shim * 0.52f;            /* pitched shimmer return injected into tank */
    float aa_a = (ratio > 1.0f) ? (0.80f / ratio) : 0.95f;
    float inc = 1.0f - ratio;
    const float grain_len = 1920.0f;          /* 40 ms lush cloud grain */
    const int32_t xf_t = 480;                 /* 10 ms WSOLA crossfade */

    if (!st->ps_init) {
        st->d_a = (ratio > 1.0f) ? 1200.0f : 48.0f;
        st->d_b = st->d_a;
        st->xf_pos = -1;
        st->xf_len = xf_t;
        st->ps_init = 1u;
    }

    float dry_gain = 1.0f - mix;
    const uint32_t gmask = SH_GLEN - 1u;

    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; ++i) {
        float in_l = buf_l[i];
        float in_r = buf_r[i];

        /* Feed dry + shimmer feedback into stereo reverb tank */
        float tank_in_l = p4_tanh((in_l * 0.55f + in_r * 0.15f + st->sh_fb_l * shim_inj) * 0.06f);
        float tank_in_r = p4_tanh((in_r * 0.55f + in_l * 0.15f + st->sh_fb_r * shim_inj) * 0.06f);
        uint32_t v = st->valid;

        float ol = 0.0f;
        ol += sh_comb(ar->cl0, SH_C0L, &st->cl_i[0], &st->cl_f[0], v, tank_in_l, fb, d1, d2);
        ol += sh_comb(ar->cl1, SH_C1L, &st->cl_i[1], &st->cl_f[1], v, tank_in_l, fb, d1, d2);
        ol += sh_comb(ar->cl2, SH_C2L, &st->cl_i[2], &st->cl_f[2], v, tank_in_l, fb, d1, d2);
        ol += sh_comb(ar->cl3, SH_C3L, &st->cl_i[3], &st->cl_f[3], v, tank_in_l, fb, d1, d2);

        float or_ = 0.0f;
        or_ += sh_comb(ar->cr0, SH_C0R, &st->cr_i[0], &st->cr_f[0], v, tank_in_r, fb, d1, d2);
        or_ += sh_comb(ar->cr1, SH_C1R, &st->cr_i[1], &st->cr_f[1], v, tank_in_r, fb, d1, d2);
        or_ += sh_comb(ar->cr2, SH_C2R, &st->cr_i[2], &st->cr_f[2], v, tank_in_r, fb, d1, d2);
        or_ += sh_comb(ar->cr3, SH_C3R, &st->cr_i[3], &st->cr_f[3], v, tank_in_r, fb, d1, d2);

        ol = sh_ap(ar->al0, SH_A0L, &st->al_i[0], v, ol);
        ol = sh_ap(ar->al1, SH_A1L, &st->al_i[1], v, ol);
        or_ = sh_ap(ar->ar0, SH_A0R, &st->ar_i[0], v, or_);
        or_ = sh_ap(ar->ar1, SH_A1R, &st->ar_i[1], v, or_);
        if (st->valid < 2048u) st->valid++;

        float wet_l = p4_tanh((ol * 0.85f + or_ * 0.15f) * 2.6f);
        float wet_r = p4_tanh((or_ * 0.85f + ol * 0.15f) * 2.6f);

        /* Pre-AA LP into WSOLA shimmer shifter */
        st->aa_l += aa_a * (wet_l - st->aa_l);
        st->aa_r += aa_a * (wet_r - st->aa_r);
        ar->gl[st->g_wr] = p4_f_to_s16(st->aa_l);
        ar->gr[st->g_wr] = p4_f_to_s16(st->aa_r);

        float pl, pr;
        if (p_idx == 12) {
            pl = wet_l;
            pr = wet_r;
        } else if (st->xf_pos < 0) {
            pl = p4_read_s16_lin(ar->gl, gmask, st->g_wr, st->g_valid, st->d_a);
            pr = p4_read_s16_lin(ar->gr, gmask, st->g_wr, st->g_valid, st->d_a);
            st->d_a += inc;
            if (ratio > 1.0f && st->d_a <= (float)xf_t * (ratio - 1.0f) + 24.0f) {
                float d_nom = st->d_a + grain_len;
                int32_t lag = (st->g_valid > 2400u) ? sh_find_lag(ar, st->g_wr, st->g_valid, st->d_a, d_nom) : 0;
                st->d_b = d_nom + (float)lag;
                if (st->d_b < 24.0f) st->d_b = 24.0f;
                if (st->d_b > (float)(SH_GLEN - 32u)) st->d_b = (float)(SH_GLEN - 32u);
                st->xf_pos = 0;
            } else if (ratio < 1.0f && st->d_a >= grain_len + 48.0f) {
                float d_nom = st->d_a - grain_len;
                if (d_nom < 32.0f) d_nom = 32.0f;
                int32_t lag = (st->g_valid > 2400u) ? sh_find_lag(ar, st->g_wr, st->g_valid, st->d_a, d_nom) : 0;
                st->d_b = d_nom + (float)lag;
                if (st->d_b < 24.0f) st->d_b = 24.0f;
                if (st->d_b > (float)(SH_GLEN - 32u)) st->d_b = (float)(SH_GLEN - 32u);
                st->xf_pos = 0;
            }
        } else {
            float wb = p4_hann_up((float)st->xf_pos / (float)st->xf_len);
            float wa = 1.0f - wb;
            pl = wa * p4_read_s16_lin(ar->gl, gmask, st->g_wr, st->g_valid, st->d_a)
               + wb * p4_read_s16_lin(ar->gl, gmask, st->g_wr, st->g_valid, st->d_b);
            pr = wa * p4_read_s16_lin(ar->gr, gmask, st->g_wr, st->g_valid, st->d_a)
               + wb * p4_read_s16_lin(ar->gr, gmask, st->g_wr, st->g_valid, st->d_b);
            st->d_a += inc;
            st->d_b += inc;
            if (++st->xf_pos >= st->xf_len) {
                st->d_a = st->d_b;
                st->xf_pos = -1;
            }
        }
        st->g_wr = (st->g_wr + 1u) & gmask;
        if (st->g_valid < SH_GLEN) st->g_valid++;

        /* 140 Hz HPF on shimmer feedback so octave-down never turns to mud */
        st->hp_l += 0.018f * (pl - st->hp_l);
        st->hp_r += 0.018f * (pr - st->hp_r);
        st->sh_fb_l = (pl - st->hp_l) * 0.85f + (pr - st->hp_r) * 0.15f;
        st->sh_fb_r = (pr - st->hp_r) * 0.85f + (pl - st->hp_l) * 0.15f;

        if (!zero_mix) {
            /* Blend lush tank + subtle direct shimmer sparkle */
            float out_l = wet_l + st->sh_fb_l * (shim * 0.35f);
            float out_r = wet_r + st->sh_fb_r * (shim * 0.35f);
            buf_l[i] = in_l * dry_gain + out_l * mix;
            buf_r[i] = in_r * dry_gain + out_r * mix;
        }
    }
}

/* ---- FX 57: GEN LOSS (Broken VHS / Walkman) ---- */
static void process_genloss(genloss_state_t *st, genloss_arena_t *ar, const fx_bus_ctrl_t *ctrl,
                            float *buf_l, float *buf_r) {
    /* Knobs: 0=WOW, 1=FLUTTER, 2=FAILURE, 3=SATURATE, 4=BANDWIDTH, 5=NOISE */
    float wow  = ctrl->smooth[0] * 0.01f;
    float flt  = ctrl->smooth[1] * 0.01f;
    float fail = ctrl->smooth[2] * 0.01f;
    float sat  = ctrl->smooth[3] * 0.01f;
    float bw   = ctrl->smooth[4] * 0.01f;
    float noi  = ctrl->smooth[5] * 0.01f;

    float wow_depth = wow * 280.0f;    /* up to +/-5.8 ms deep tape wow */
    float flt_depth = flt * 28.0f;     /* fast capstan flutter */
    float drive = 1.0f + sat * 3.2f;
    float post_gain = 1.0f / (1.0f + sat * 0.55f);

    /* Bandwidth: 0% = 30 Hz..16 kHz (Reel), 50% = 120 Hz..5.5 kHz (VHS), 100% = 380 Hz..1.6 kHz (Micro) */
    float hp_pole = 0.996f - bw * 0.048f;
    float inv_bw = 1.0f - bw;
    float lp_a = 0.12f + (inv_bw * inv_bw) * 0.78f;
    float noise_lvl = noi * noi * 0.045f;

    const uint32_t mask = GL_BUF_LEN - 1u;
    const float wow_inc = 0.42f / 48000.0f;
    const float flt_inc1 = 6.7f / 48000.0f;
    const float flt_inc2 = 11.4f / 48000.0f;

    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; ++i) {
        /* 1. Write input to tape delay ring */
        ar->line_l[st->wr] = p4_f_to_s16(buf_l[i]);
        ar->line_r[st->wr] = p4_f_to_s16(buf_r[i]);

        /* 2. Wow + flutter + slow random drift */
        st->wow_ph += wow_inc; if (st->wow_ph >= 1.0f) st->wow_ph -= 1.0f;
        st->flt_ph1 += flt_inc1; if (st->flt_ph1 >= 1.0f) st->flt_ph1 -= 1.0f;
        st->flt_ph2 += flt_inc2; if (st->flt_ph2 >= 1.0f) st->flt_ph2 -= 1.0f;

        /* 3. Random tape failure / oxide crinkle / azimuth events every 15 ms (720 samp) */
        if (st->fail_rem > 0) {
            st->fail_rem--;
            if (st->fail_rem == 0) {
                st->fail_gain_l = 1.0f;
                st->fail_gain_r = 1.0f;
                st->fail_az = 0.0f;
            }
        } else if (++st->tick_cnt >= 720u) {
            st->tick_cnt = 0;
            uint32_t r = p4_xorshift(&st->rng);
            float drift_step = (((float)(int32_t)(r & 0xFFu) / 128.0f) - 1.0f) * 0.18f;
            st->drift = st->drift * 0.94f + drift_step;
            if (fail > 0.01f) {
                uint32_t thresh = (uint32_t)(fail * fail * 0.30f * 65536.0f);
                if (((r >> 8) & 0xFFFFu) < thresh) {
                    st->fail_rem = (int32_t)(600u + ((r >> 24) * 32u)); /* 12..180 ms event */
                    /* Asymmetric L/R dip + azimuth shift (real cassette crinkle!) */
                    float dip = 1.0f - fail * (0.45f + 0.50f * ((float)(r & 15u) / 15.0f));
                    uint32_t side = (r >> 4) & 3u;
                    st->fail_gain_l = (side == 1u) ? (0.5f + 0.5f * dip) : dip;
                    st->fail_gain_r = (side == 2u) ? (0.5f + 0.5f * dip) : dip;
                    st->fail_az = (((float)(int32_t)((r >> 6) & 31u) / 16.0f) - 1.0f) * fail * 36.0f;
                }
            }
        }

        /* Smooth failure transitions (~4 ms) so dropouts crinkle organically instead of digital clicks */
        st->cur_gain_l += 0.008f * (st->fail_gain_l - st->cur_gain_l);
        st->cur_gain_r += 0.008f * (st->fail_gain_r - st->cur_gain_r);
        st->cur_az     += 0.004f * (st->fail_az - st->cur_az);

        float mod = wow_depth * (p4_sin01(st->wow_ph) + 0.35f * st->drift)
                  + flt_depth * (0.7f * p4_sin01(st->flt_ph1) + 0.3f * p4_sin01(st->flt_ph2));
        float base_d = 400.0f + mod;
        float dl = p4_read_s16_lin(ar->line_l, mask, st->wr, st->valid, base_d + st->cur_az);
        float dr = p4_read_s16_lin(ar->line_r, mask, st->wr, st->valid, base_d - st->cur_az);

        st->wr = (st->wr + 1u) & mask;
        if (st->valid < GL_BUF_LEN) st->valid++;

        /* Oxide crinkle: highs dull further during a dropout dip */
        float ox_a_l = 0.08f + 0.88f * st->cur_gain_l;
        float ox_a_r = 0.08f + 0.88f * st->cur_gain_r;
        st->ox_l += ox_a_l * (dl * st->cur_gain_l - st->ox_l);
        st->ox_r += ox_a_r * (dr * st->cur_gain_r - st->ox_r);

        /* 4. Asymmetric magnetic tape saturation (subtle 2nd-harmonic warmth) */
        float sl = (p4_tanh(st->ox_l * drive + sat * 0.06f) - sat * 0.055f) * post_gain;
        float sr = (p4_tanh(st->ox_r * drive + sat * 0.06f) - sat * 0.055f) * post_gain;

        /* 5. Bandwidth HP + 2-pole LP */
        float hpl = hp_pole * (st->hp_yl + sl - st->hp_xl);
        st->hp_xl = sl; st->hp_yl = hpl;
        float hpr = hp_pole * (st->hp_yr + sr - st->hp_xr);
        st->hp_xr = sr; st->hp_yr = hpr;

        st->lp1_l += lp_a * (hpl - st->lp1_l);
        st->lp2_l += lp_a * (st->lp1_l - st->lp2_l);
        st->lp1_r += lp_a * (hpr - st->lp1_r);
        st->lp2_r += lp_a * (st->lp1_r - st->lp2_r);

        /* 6. Warm pink-filtered tape hiss + occasional micro-pop */
        float nl = 0.0f, nr = 0.0f;
        if (noise_lvl > 1e-6f) {
            uint32_t r = p4_xorshift(&st->rng);
            float wl = ((float)(int32_t)(r & 0xFFFFu) / 32768.0f) - 1.0f;
            float wr_n = ((float)(int32_t)((r >> 16) & 0xFFFFu) / 32768.0f) - 1.0f;
            st->pink_l = st->pink_l * 0.72f + wl * 0.28f;
            st->pink_r = st->pink_r * 0.72f + wr_n * 0.28f;
            float pop = ((r & 0x3FFFu) == 0u) ? (wl * 0.35f * noi) : 0.0f;
            nl = st->pink_l * noise_lvl + pop;
            nr = st->pink_r * noise_lvl + pop * 0.7f;
        }

        buf_l[i] = st->lp2_l + nl;
        buf_r[i] = st->lp2_r + nr;
    }
}

/* ---- FX 58: CHORD RESO (4-Voice Karplus-Strong Tuned Modal Chord Bank) ---- */
/* Period in samples @ 48 kHz for C2 (65.4064 Hz) up to B5 (987.767 Hz): 48 semitones */
static const float k_note_period[48] = {
    733.87f, 692.68f, 653.80f, 617.11f, 582.47f, 549.78f, 518.92f, 489.80f, 462.30f, 436.36f, 411.87f, 388.75f,
    366.94f, 346.34f, 326.90f, 308.55f, 291.24f, 274.89f, 259.46f, 244.90f, 231.15f, 218.18f, 205.94f, 194.38f,
    183.47f, 173.17f, 163.45f, 154.28f, 145.62f, 137.45f, 129.73f, 122.45f, 115.58f, 109.09f, 102.97f,  97.19f,
     91.73f,  86.58f,  81.73f,  77.14f,  72.81f,  68.72f,  64.87f,  61.22f,  57.79f,  54.55f,  51.48f,  48.59f
};

static const uint8_t k_chords[8][CR_VOICES] = {
    { 0,  3,  7, 12 }, /* 0: MIN  */
    { 0,  4,  7, 12 }, /* 1: MAJ  */
    { 0,  3,  7, 10 }, /* 2: MIN7 */
    { 0,  4,  7, 11 }, /* 3: MAJ7 */
    { 0,  3,  7, 14 }, /* 4: MIN9 */
    { 0,  5,  7, 12 }, /* 5: SUS4 */
    { 0,  7, 12, 19 }, /* 6: 5THS */
    { 0, 12, 19, 24 }, /* 7: OCT  */
};

static void process_chord_reso(chord_state_t *st, chord_arena_t *ar, const fx_bus_ctrl_t *ctrl,
                               float *buf_l, float *buf_r) {
    /* Knobs: 0=ROOT(0..23: C2..B3), 1=CHORD(0..7), 2=DECAY, 3=DAMP, 4=SPREAD, 5=MIX */
    int root  = (int)ctrl->target[0];
    if (root < 0) root = 0;
    if (root > 23) root = 23;
    int chord = (int)ctrl->target[1];
    if (chord < 0) chord = 0;
    if (chord > 7) chord = 7;

    float decay  = ctrl->smooth[2] * 0.01f;
    float damp   = ctrl->smooth[3] * 0.01f;
    float spread = ctrl->smooth[4] * 0.01f;
    float mix    = ctrl->smooth[5] * 0.01f;
    int   zero_mix = (ctrl->target[5] == 0 && mix < 0.002f);

    /* Per-voice feedback 0.80 .. 0.990 (musical decay without runaway) */
    float fb = 0.80f + decay * 0.190f;
    float lp_pole = 0.08f + damp * 0.72f;
    float lp_in = 1.0f - lp_pole;

    float target_del[CR_VOICES];
    for (uint32_t v = 0; v < CR_VOICES; ++v) {
        int note = root + (int)k_chords[chord][v];
        if (note > 47) note = 47;
        target_del[v] = k_note_period[note];
        if (!st->init_done) st->cur_del[v] = target_del[v];
    }
    st->init_done = 1u;

    /* Stereo pan positions for the 4 chord voices: L, mid-R, mid-L, R */
    static const float k_pan[CR_VOICES] = { -0.85f, 0.45f, -0.45f, 0.85f };
    float pan_l[CR_VOICES], pan_r[CR_VOICES];
    for (uint32_t v = 0; v < CR_VOICES; ++v) {
        float p = k_pan[v] * spread;
        pan_l[v] = 0.5f * (1.0f - p);
        pan_r[v] = 0.5f * (1.0f + p);
    }

    float dry_gain = 1.0f - mix;
    const uint32_t mask = CR_BUF_LEN - 1u;

    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; ++i) {
        float in_l = buf_l[i];
        float in_r = buf_r[i];
        /* Scale excitation inversely with feedback so resonances sit at musical level */
        float exc = (in_l + in_r) * (0.14f * (1.15f - 0.55f * decay));

        float wl = 0.0f, wr = 0.0f;
        uint32_t val = st->valid;

        for (uint32_t v = 0; v < CR_VOICES; ++v) {
            /* Smooth pitch glide when changing ROOT or CHORD live */
            st->cur_del[v] += 0.0025f * (target_del[v] - st->cur_del[v]);
            float d = st->cur_del[v];
            uint32_t di = (uint32_t)d;
            float frac = d - (float)di;
            uint32_t w = st->wr[v];

            float delayed = 0.0f;
            if (val > di + 1u) {
                uint32_t i0 = (w - di) & mask;
                uint32_t i1 = (i0 - 1u) & mask;
                delayed = ar->buf[v][i0] + frac * (ar->buf[v][i1] - ar->buf[v][i0]);
            }

            /* Karplus-Strong LP damping + DC blocker + soft limit inside comb loop */
            st->lp[v] = st->lp[v] * lp_pole + delayed * lp_in;
            float dc = 0.994f * (st->dc_y[v] + st->lp[v] - st->dc_x[v]);
            st->dc_x[v] = st->lp[v];
            st->dc_y[v] = dc;

            float fb_sig = p4_tanh(exc + dc * fb);
            ar->buf[v][w] = fb_sig;
            st->wr[v] = (w + 1u) & mask;

            wl += dc * pan_l[v];
            wr += dc * pan_r[v];
        }
        if (st->valid < CR_BUF_LEN) st->valid++;

        if (!zero_mix) {
            buf_l[i] = in_l * dry_gain + p4_tanh(wl * 0.95f) * mix;
            buf_r[i] = in_r * dry_gain + p4_tanh(wr * 0.95f) * mix;
        }
    }
}

/* ---- FX 59: TRANSIENT (SPL-Style Transient Shaper + Low Punch + Drive + Tilt) ---- */
static void process_transient(transient_state_t *st, const fx_bus_ctrl_t *ctrl,
                              float *buf_l, float *buf_r) {
    /* Knobs: 0=ATTACK(0..200 => -100..+100%), 1=SUSTAIN(0..200 => -100..+100%),
     *        2=PUNCH(0..100%), 3=DRIVE(0..100%), 4=TONE(0..100 => -50..+50), 5=MIX */
    float att   = (ctrl->smooth[0] - 100.0f) * 0.01f;  /* -1.0 .. +1.0 */
    float sus   = (ctrl->smooth[1] - 100.0f) * 0.01f;  /* -1.0 .. +1.0 */
    float punch = ctrl->smooth[2] * 0.01f;             /*  0.0 ..  1.0 */
    float drive = ctrl->smooth[3] * 0.01f;             /*  0.0 ..  1.0 */
    float tilt  = (ctrl->smooth[4] - 50.0f) * 0.02f;   /* -1.0 .. +1.0 */
    float mix   = ctrl->smooth[5] * 0.01f;
    int   zero_mix = (ctrl->target[5] == 0 && mix < 0.002f);

    float drv_gain = 1.0f + drive * 2.4f;
    float drv_comp = 1.0f / (1.0f + drive * 0.45f);
    float dry_gain = 1.0f - mix;

    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; ++i) {
        float in_l = buf_l[i];
        float in_r = buf_r[i];

        float al = in_l < 0.0f ? -in_l : in_l;
        float ar = in_r < 0.0f ? -in_r : in_r;
        float pk = (al > ar) ? al : ar;

        /* Triple envelope follower: fast (~0.8 ms), medium (~18 ms), slow (~160 ms) */
        st->env_fast += (pk > st->env_fast ? 0.055f : 0.0035f) * (pk - st->env_fast);
        st->env_med  += (pk > st->env_med  ? 0.0022f : 0.0012f) * (pk - st->env_med);
        st->env_slow += (pk > st->env_slow ? 0.0003f : 0.00025f) * (pk - st->env_slow);

        /* Normalized transient & sustain ratios (level-independent like SPL Transient Designer!) */
        float norm_f = st->env_med + 0.004f;
        float att_delta = (st->env_fast - st->env_med) / norm_f;
        if (att_delta < 0.0f) att_delta = 0.0f;
        if (att_delta > 2.5f) att_delta = 2.5f;

        float norm_s = st->env_slow + 0.004f;
        float sus_delta = (st->env_slow - st->env_fast) / norm_s;
        if (sus_delta < 0.0f) sus_delta = 0.0f;
        if (sus_delta > 1.5f) sus_delta = 1.5f;

        /* Compute target gain from Attack & Sustain controls */
        float g_att = (att >= 0.0f) ? (1.0f + att * att_delta * 1.65f)
                                    : (1.0f / (1.0f + (-att) * att_delta * 2.2f));
        float g_sus = (sus >= 0.0f) ? (1.0f + sus * sus_delta * 1.45f)
                                    : (1.0f / (1.0f + (-sus) * sus_delta * 3.8f));
        float g_tot = g_att * g_sus;
        if (g_tot > 4.0f) g_tot = 4.0f;
        if (g_tot < 0.06f) g_tot = 0.06f;

        /* Fast smoothing (~0.4 ms) prevents envelope zipper while preserving transient snap */
        st->gain_sm += 0.06f * (g_tot - st->gain_sm);

        float wl = in_l * st->gain_sm;
        float wr = in_r * st->gain_sm;

        /* Low-end Punch (~100 Hz 1-pole shelf boost) */
        st->punch_lp_l += 0.013f * (wl - st->punch_lp_l);
        st->punch_lp_r += 0.013f * (wr - st->punch_lp_r);
        wl += st->punch_lp_l * (punch * 0.95f);
        wr += st->punch_lp_r * (punch * 0.95f);

        /* Subtle 1 kHz Tilt EQ */
        st->tilt_lp_l += 0.125f * (wl - st->tilt_lp_l);
        st->tilt_lp_r += 0.125f * (wr - st->tilt_lp_r);
        float hi_l = wl - st->tilt_lp_l;
        float hi_r = wr - st->tilt_lp_r;
        wl = st->tilt_lp_l * (1.0f - tilt * 0.35f) + hi_l * (1.0f + tilt * 0.45f);
        wr = st->tilt_lp_r * (1.0f - tilt * 0.35f) + hi_r * (1.0f + tilt * 0.45f);

        /* Musical soft-clipper / bus glue */
        if (drive > 0.005f) {
            wl = p4_tanh(wl * drv_gain) * drv_comp;
            wr = p4_tanh(wr * drv_gain) * drv_comp;
        }

        if (!zero_mix) {
            buf_l[i] = in_l * dry_gain + wl * mix;
            buf_r[i] = in_r * dry_gain + wr * mix;
        }
    }
}

/* ---- FX 60: MICROLOOP (Beat-Synced / Free Micro-Looper) ---- */
static inline float ml_wrap(float p, float ring_f) {
    while (p < 0.0f) p += ring_f;
    while (p >= ring_f) p -= ring_f;
    return p;
}

static inline float ml_read(const int16_t *buf, float pos, uint32_t ring_u) {
    uint32_t i0 = (uint32_t)pos;
    if (i0 >= ring_u) i0 = 0;
    uint32_t i1 = i0 + 1u;
    if (i1 >= ring_u) i1 = 0;
    float frac = pos - (float)i0;
    float s0 = p4_s16_to_f(buf[i0]);
    float s1 = p4_s16_to_f(buf[i1]);
    return s0 + frac * (s1 - s0);
}

static void process_microloop(microloop_state_t *st, microloop_arena_t *ar, const fx_bus_ctrl_t *ctrl,
                              float *buf_l, float *buf_r, uint32_t bpm100) {
    /* Knobs: 0=LENGTH(0..100 => 15..500ms), 1=MODE(0..5), 2=GLITCH(0..100%),
     *        3=FADE(0..100%), 4=SYNC(0=OFF, 1=1/4, 2=1/8, 3=1/8T, 4=1/16, 5=1/32), 5=MIX */
    static const float k_ml_sync[6] = { 0.0f, 1.0f, 0.5f, 0.3333333f, 0.25f, 0.125f };
    int sync = (int)ctrl->target[4];
    float target_len;
    if (sync >= 1 && sync <= 5) {
        float bpm = (bpm100 >= 4000u && bpm100 <= 24000u) ? ((float)bpm100 * 0.01f) : 120.0f;
        target_len = (48000.0f * 60.0f / bpm) * k_ml_sync[sync];
    } else {
        target_len = (15.0f + ctrl->smooth[0] * 4.85f) * 48.0f; /* 15..500 ms */
    }
    if (target_len < 720.0f) target_len = 720.0f;
    if (target_len > 24000.0f) target_len = 24000.0f;

    int mode = (int)ctrl->target[1];
    if (mode < 0) mode = 0;
    if (mode > 5) mode = 5;

    float glitch = ctrl->smooth[2] * 0.01f;
    float fade   = ctrl->smooth[3] * 0.01f;
    float mix    = ctrl->smooth[5] * 0.01f;
    int   zero_mix = (ctrl->target[5] == 0 && mix < 0.002f);

    /* Base playback speed & direction by MODE:
     * 0:FWD(+1), 1:REV(-1), 2:PONG(flip), 3:HALF(+0.5), 4:DBL(+2.0), 5:R-HALF(-0.5) */
    float speed = 1.0f;
    if (mode == 3 || mode == 5) speed = 0.5f;
    else if (mode == 4) speed = 2.0f;

    if (!st->init_done) {
        st->slice_len = target_len;
        st->slice_age = 0.0f;
        st->dir = (mode == 1 || mode == 5) ? -1 : 1;
        st->init_done = 1u;
    }

    float dry_gain = 1.0f - mix;

    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; ++i) {
        float in_l = buf_l[i];
        float in_r = buf_r[i];

        /* Continuously capture live audio into 1.0 s ring */
        ar->cap_l[st->wr] = p4_f_to_s16(in_l);
        ar->cap_r[st->wr] = p4_f_to_s16(in_r);
        if (++st->wr >= ML_BUF_LEN) st->wr = 0;
        if (st->valid < ML_BUF_LEN) st->valid++;

        uint32_t ring_u = (st->valid < ML_BUF_LEN) ? st->wr : ML_BUF_LEN;
        float ring_f = (float)ring_u;

        /* Advance current micro-slice & trigger new slice at boundary */
        if (st->slice_age >= st->slice_len || ring_u < 128u) {
            if (ring_u >= 128u) {
                /* Save outgoing cursor for smooth boundary crossfade */
                st->old_pos = st->rd_pos;
                st->old_rate = (float)st->dir * speed;
                int32_t xf = (int32_t)(96.0f + fade * 0.25f * target_len); /* 2 ms .. 25% slice */
                if (xf > 1920) xf = 1920;
                st->xf_tot = xf;
                st->xf_rem = xf;

                st->slice_len = target_len;
                st->slice_age = 0.0f;

                /* Decide whether to refresh slice from live wr, or hold/repeat + glitch */
                uint32_t r = p4_xorshift(&st->rng);
                float roll = (float)(r & 0xFFFFu) * (1.0f / 65536.0f);
                /* Higher GLITCH increases chance of holding/re-stuttering or jumping back */
                int hold_slice = (st->holding && roll < (0.35f + glitch * 0.45f));
                if (!hold_slice) {
                    float back = st->slice_len * (speed > 1.0f ? speed : 1.0f) + 32.0f;
                    if (glitch > 0.1f && ((r >> 16) & 3u) == 0u) {
                        /* Generative jump back into recent 0.5 s history */
                        back += ((float)((r >> 20) & 0xFFu) / 255.0f) * glitch * 18000.0f;
                    }
                    if (back >= ring_f - 16.0f) back = ring_f - 16.0f;
                    if (back < 32.0f) back = 32.0f;
                    st->slice_start = ml_wrap((float)st->wr - back, ring_f);
                    st->holding = (roll < (0.25f + glitch * 0.55f)) ? 1u : 0u;
                }

                /* Direction update */
                if (mode == 1 || mode == 5) {
                    st->dir = -1;
                } else if (mode == 2) {
                    st->dir = (st->dir > 0) ? -1 : 1;
                } else {
                    st->dir = 1;
                    if (glitch > 0.35f && ((r >> 24) & 7u) == 0u) st->dir = -1;
                }

                if (st->dir > 0) {
                    st->rd_pos = st->slice_start;
                } else {
                    float span = st->slice_len * speed;
                    if (span >= ring_f - 8.0f) span = ring_f - 8.0f;
                    st->rd_pos = ml_wrap(st->slice_start + span, ring_f);
                }
            }
        }

        float wl = 0.0f, wr = 0.0f;
        if (ring_u >= 128u) {
            float rate = (float)st->dir * speed;
            wl = ml_read(ar->cap_l, st->rd_pos, ring_u);
            wr = ml_read(ar->cap_r, st->rd_pos, ring_u);
            st->rd_pos = ml_wrap(st->rd_pos + rate, ring_f);
            st->slice_age += 1.0f;

            /* Crossfade from previous slice on boundary for click-free chops */
            if (st->xf_rem > 0 && st->xf_tot > 0) {
                float u = 1.0f - (float)st->xf_rem / (float)st->xf_tot;
                float w_new = p4_hann_up(u);
                float w_old = 1.0f - w_new;
                float ol = ml_read(ar->cap_l, st->old_pos, ring_u);
                float or_ = ml_read(ar->cap_r, st->old_pos, ring_u);
                st->old_pos = ml_wrap(st->old_pos + st->old_rate, ring_f);
                wl = w_new * wl + w_old * ol;
                wr = w_new * wr + w_old * or_;
                st->xf_rem--;
            }

            /* Optional soft swell shaping at high FADE */
            if (fade > 0.25f) {
                float p = st->slice_age / st->slice_len;
                float env = 0.5f * (1.0f - p4_sin01(p + 0.25f)); /* Hann [0..1..0] */
                float amt = (fade - 0.25f) * 1.15f;
                if (amt > 1.0f) amt = 1.0f;
                float g = (1.0f - amt) + amt * env;
                wl *= g;
                wr *= g;
            }
        }

        if (!zero_mix) {
            buf_l[i] = in_l * dry_gain + wl * mix;
            buf_r[i] = in_r * dry_gain + wr * mix;
        }
    }
}

void fx_page4_process(uint32_t bus_idx, uint32_t fx_id, fx_bus_ctrl_t *ctrl,
                      void *arena, float *buf_l, float *buf_r, uint32_t bpm100) {
    if (bus_idx >= FX_AUDIO_BUSES) return;
    if (fx_id >= FX_GHOST_PRINT) {
        atmosphere_process(fx_id, ctrl, arena, buf_l, buf_r);
        return;
    }
    switch (fx_id) {
    case FX_SHIMMER:
        process_shimmer(&s_p4[bus_idx].shimmer, (shimmer_arena_t *)arena, ctrl, buf_l, buf_r);
        break;
    case FX_GEN_LOSS:
        process_genloss(&s_p4[bus_idx].genloss, (genloss_arena_t *)arena, ctrl, buf_l, buf_r);
        break;
    case FX_CHORD_RESO:
        process_chord_reso(&s_p4[bus_idx].chord, (chord_arena_t *)arena, ctrl, buf_l, buf_r);
        break;
    case FX_TRANSIENT:
        process_transient(&s_p4[bus_idx].trans, ctrl, buf_l, buf_r);
        break;
    case FX_MICROLOOP:
        process_microloop(&s_p4[bus_idx].mloop, (microloop_arena_t *)arena, ctrl, buf_l, buf_r, bpm100);
        break;
    default:
        break;
    }
}
