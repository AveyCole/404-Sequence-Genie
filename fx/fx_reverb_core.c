#include "fx_engine.h"

/* ============================================================================
 * Shared ReverbCore (Freeverb-style: 4 LP combs + 2 series allpasses per ch)
 * Powers FX 51 (Algoverb) and FX 52 (Gated Verb)
 * ============================================================================ */
#define RV_PREDELAY_LEN 9632u   /* > 200 ms @ 48 kHz */

/* Mutual-prime comb & allpass lengths scaled from Freeverb 44.1k -> 48 kHz */
#define C0L 1217u
#define C1L 1301u
#define C2L 1399u
#define C3L 1487u
#define C0R 1237u
#define C1R 1327u
#define C2R 1423u
#define C3R 1511u

#define A0L 245u
#define A1L 607u
#define A0R 269u
#define A1R 631u

typedef struct {
    int16_t pre_l[RV_PREDELAY_LEN];
    int16_t pre_r[RV_PREDELAY_LEN];
    float   comb_l0[C0L], comb_l1[C1L], comb_l2[C2L], comb_l3[C3L];
    float   comb_r0[C0R], comb_r1[C1R], comb_r2[C2R], comb_r3[C3R];
    float   ap_l0[A0L],   ap_l1[A1L];
    float   ap_r0[A0R],   ap_r1[A1R];
} reverb_arena_t;

typedef struct {
    uint32_t pre_wr;
    uint32_t valid;         /* O(1) reset counter: avoids zeroing 84 KB arena in audio IRQ! */
    uint32_t cl_idx[4], cr_idx[4];
    uint32_t al_idx[2], ar_idx[2];
    float    cl_flt[4], cr_flt[4];
    /* Gated Verb detector + gate envelope */
    float    env;
    float    gate_gain;
    int32_t  hold_cnt;
} reverb_state_t;

static reverb_state_t s_reverb[FX_AUDIO_BUSES] __attribute__((section(".fx_bss")));

/* Precomputed dB-to-linear LUT for -60..0 dB (indices 0..60, step 1 dB) */
static const float k_db_lin[61] = {
    0.001000f, 0.001122f, 0.001259f, 0.001413f, 0.001585f, 0.001778f,
    0.001995f, 0.002239f, 0.002512f, 0.002818f, 0.003162f, 0.003548f,
    0.003981f, 0.004467f, 0.005012f, 0.005623f, 0.006310f, 0.007079f,
    0.007943f, 0.008913f, 0.010000f, 0.011220f, 0.012589f, 0.014125f,
    0.015849f, 0.017783f, 0.019953f, 0.022387f, 0.025119f, 0.028184f,
    0.031623f, 0.035481f, 0.039811f, 0.044668f, 0.050119f, 0.056234f,
    0.063096f, 0.070795f, 0.079433f, 0.089125f, 0.100000f, 0.112202f,
    0.125893f, 0.141254f, 0.158489f, 0.177828f, 0.199526f, 0.223872f,
    0.251189f, 0.281838f, 0.316228f, 0.354813f, 0.398107f, 0.446684f,
    0.501187f, 0.562341f, 0.630957f, 0.707946f, 0.794328f, 0.891251f,
    1.000000f
};

static inline int16_t rv_f_to_s16(float x) {
    float s = x * 32767.0f;
    if (s > 32767.0f) return 32767;
    if (s < -32768.0f) return -32768;
    return (int16_t)s;
}

static inline float rv_s16_to_f(int16_t s) {
    return (float)s * (1.0f / 32768.0f);
}

static inline float comb_step(float *buf, uint32_t len, uint32_t *idx, float *flt,
                              uint32_t valid, float in, float fb, float damp1, float damp2) {
    uint32_t i = *idx;
    float out = (valid >= len) ? buf[i] : 0.0f;
    float f = out * damp2 + (*flt) * damp1;
    *flt = f;
    buf[i] = in + f * fb;
    if (++i >= len) i = 0;
    *idx = i;
    return out;
}

static inline float allpass_step(float *buf, uint32_t len, uint32_t *idx, uint32_t valid, float in) {
    uint32_t i = *idx;
    float bufout = (valid >= len) ? buf[i] : 0.0f;
    float out = bufout - in;
    buf[i] = in + bufout * 0.5f;
    if (++i >= len) i = 0;
    *idx = i;
    return out;
}

void fx_reverb_reset(uint32_t bus_idx, uint32_t fx_id, void *arena) {
    (void)fx_id;
    (void)arena; /* O(1) reset via valid counter — zero SDRAM memset spike in audio IRQ! */
    if (bus_idx >= FX_AUDIO_BUSES) return;
    uint32_t *sp = (uint32_t *)&s_reverb[bus_idx];
    for (uint32_t i = 0; i < sizeof(reverb_state_t) / 4u; ++i) sp[i] = 0;
}

void fx_reverb_process(uint32_t bus_idx, uint32_t fx_id, fx_bus_ctrl_t *ctrl,
                       void *arena, float *buf_l, float *buf_r) {
    if (bus_idx >= FX_AUDIO_BUSES) return;
    reverb_state_t *st = &s_reverb[bus_idx];
    reverb_arena_t *ar = (reverb_arena_t *)arena;

    int is_gated = (fx_id == FX_GATED_VERB);
    float size, fb, damp1, damp2, mix, wet1, wet2;
    uint32_t pre_samp;

    /* Gated Verb specific params */
    float thresh = 0.015f;
    int32_t hold_samp = 7200;
    float rel_step = 0.0007f;

    if (!is_gated) {
        /* 51: Algoverb: SIZE, DECAY, DAMP, MIX, PRE DELAY, WIDTH */
        size = ctrl->smooth[0] * 0.01f;
        float decay = ctrl->smooth[1] * 0.01f;
        float damp = ctrl->smooth[2] * 0.01f;
        mix = ctrl->smooth[3] * 0.01f;
        pre_samp = (uint32_t)(ctrl->smooth[4] * 48.0f);
        if (pre_samp >= RV_PREDELAY_LEN) pre_samp = RV_PREDELAY_LEN - 1u;
        float width = ctrl->smooth[5] * 0.01f;

        /* SIZE + DECAY jointly shape comb feedback in [0.50 .. 0.975] */
        float room = 0.35f * size + 0.65f * decay;
        fb = 0.50f + room * 0.475f;
        damp1 = 0.05f + damp * 0.72f * (1.0f - 0.15f * size);
        damp2 = 1.0f - damp1;
        wet1 = 0.5f * (1.0f + width);
        wet2 = 0.5f * (1.0f - width);
    } else {
        /* 52: Gated Verb: SIZE, DAMP, THRESH, HOLD, RELEASE, MIX */
        size = ctrl->smooth[0] * 0.01f;
        float damp = ctrl->smooth[1] * 0.01f;
        int t_idx = (int)ctrl->target[2];
        if (t_idx < 0) t_idx = 0;
        if (t_idx > 60) t_idx = 60;
        thresh = k_db_lin[t_idx];
        hold_samp = (int32_t)((ctrl->smooth[3] * 5.0f) * 48.0f); /* 2..100 (x5ms = 10..500 ms) */
        float rel_ms = ctrl->smooth[4];                 /* 5..200 ms */
        if (rel_ms < 5.0f) rel_ms = 5.0f;
        rel_step = 1.0f / (rel_ms * 48.0f);
        mix = ctrl->smooth[5] * 0.01f;

        pre_samp = 480u; /* tight 10 ms predelay for punchy gated snare attack */
        fb = 0.78f + size * 0.17f; /* big dense bloom */
        damp1 = 0.05f + damp * 0.65f;
        damp2 = 1.0f - damp1;
        wet1 = 0.92f;
        wet2 = 0.08f;
    }

    float dry_gain = 1.0f - mix;
    const float input_gain = 0.022f; /* prevents comb summation overload */

    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; ++i) {
        float in_l = buf_l[i];
        float in_r = buf_r[i];

        /* Write to predelay ring buffer */
        ar->pre_l[st->pre_wr] = rv_f_to_s16(in_l);
        ar->pre_r[st->pre_wr] = rv_f_to_s16(in_r);

        int32_t rd = (int32_t)st->pre_wr - (int32_t)pre_samp;
        if (rd < 0) rd += (int32_t)RV_PREDELAY_LEN;
        if (++st->pre_wr >= RV_PREDELAY_LEN) st->pre_wr = 0;

        float pl = (pre_samp == 0u) ? in_l : ((st->valid >= pre_samp) ? rv_s16_to_f(ar->pre_l[rd]) : 0.0f);
        float pr = (pre_samp == 0u) ? in_r : ((st->valid >= pre_samp) ? rv_s16_to_f(ar->pre_r[rd]) : 0.0f);
        float mono_in = (pl + pr) * input_gain;
        uint32_t v = st->valid;

        /* 4 parallel lowpass-feedback combs per channel */
        float out_l = 0.0f;
        out_l += comb_step(ar->comb_l0, C0L, &st->cl_idx[0], &st->cl_flt[0], v, mono_in, fb, damp1, damp2);
        out_l += comb_step(ar->comb_l1, C1L, &st->cl_idx[1], &st->cl_flt[1], v, mono_in, fb, damp1, damp2);
        out_l += comb_step(ar->comb_l2, C2L, &st->cl_idx[2], &st->cl_flt[2], v, mono_in, fb, damp1, damp2);
        out_l += comb_step(ar->comb_l3, C3L, &st->cl_idx[3], &st->cl_flt[3], v, mono_in, fb, damp1, damp2);

        float out_r = 0.0f;
        out_r += comb_step(ar->comb_r0, C0R, &st->cr_idx[0], &st->cr_flt[0], v, mono_in, fb, damp1, damp2);
        out_r += comb_step(ar->comb_r1, C1R, &st->cr_idx[1], &st->cr_flt[1], v, mono_in, fb, damp1, damp2);
        out_r += comb_step(ar->comb_r2, C2R, &st->cr_idx[2], &st->cr_flt[2], v, mono_in, fb, damp1, damp2);
        out_r += comb_step(ar->comb_r3, C3R, &st->cr_idx[3], &st->cr_flt[3], v, mono_in, fb, damp1, damp2);

        /* 2 series allpass diffusers per channel */
        out_l = allpass_step(ar->ap_l0, A0L, &st->al_idx[0], v, out_l);
        out_l = allpass_step(ar->ap_l1, A1L, &st->al_idx[1], v, out_l);
        out_r = allpass_step(ar->ap_r0, A0R, &st->ar_idx[0], v, out_r);
        out_r = allpass_step(ar->ap_r1, A1R, &st->ar_idx[1], v, out_r);
        if (st->valid < RV_PREDELAY_LEN) st->valid++;

        /* Make-up gain + stereo width */
        float wl = (out_l * wet1 + out_r * wet2) * 3.2f;
        float wr = (out_r * wet1 + out_l * wet2) * 3.2f;

        if (is_gated) {
            /* Fast-attack peak envelope on input + wet tail */
            float abs_l = in_l < 0.0f ? -in_l : in_l;
            float abs_r = in_r < 0.0f ? -in_r : in_r;
            float peak = (abs_l > abs_r) ? abs_l : abs_r;
            if (peak > st->env) {
                st->env += 0.12f * (peak - st->env);
            } else {
                st->env += 0.0015f * (peak - st->env);
            }
            if (st->env >= thresh) {
                st->hold_cnt = hold_samp;
                st->gate_gain += 0.15f * (1.0f - st->gate_gain); /* ~0.2 ms click-free open */
            } else if (st->hold_cnt > 0) {
                st->hold_cnt--;
                st->gate_gain += 0.15f * (1.0f - st->gate_gain);
            } else {
                st->gate_gain -= rel_step;
                if (st->gate_gain < 0.0f) st->gate_gain = 0.0f;
            }
            wl *= st->gate_gain;
            wr *= st->gate_gain;
        }

        buf_l[i] = in_l * dry_gain + wl * mix;
        buf_r[i] = in_r * dry_gain + wr * mix;
    }
}
