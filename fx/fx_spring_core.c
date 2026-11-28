#include "fx_engine.h"

/* ============================================================================
 * FX 55: SPRING (Dispersive Spring Tank Reverb)
 *   - Input ~80 Hz 1-pole HPF -> DWELL (0..2x drive + Pade tanh soft clip)
 *   - DRIP transient tap (1-pole ~2.2 kHz HPF fed into tank input)
 *   - Stereo tank (L & R): 6 series dispersive allpasses per channel with
 *     decorrelated mutual-prime delay lengths (~29 KB float32 in bus arena)
 *   - TENSION scales allpass delay spread (1.35x boomy -> 0.68x tight/bright)
 *     using zipper-free linear interpolation + subtle LP brightening at top
 *   - Feedback loop with DECAY (0.35..0.955, ~0.3..3.5 s), DAMP (8 kHz -> 800 Hz),
 *     DC blocker, and tank-output soft limiter (crash musically, never explode!)
 *   - Fixed 80% stereo crossfeed (wet1=0.90, wet2=0.10), MIX=0 exact passthrough
 *   - O(1) arena reset via valid counter (zero SDRAM memset spike in audio IRQ!)
 * ============================================================================ */

#define SP_NUM_AP 6u

/* Max allocated buffer sizes (>= 1.4x center primes, 4-aligned) */
#define SP_L0_MAX 160u
#define SP_L1_MAX 296u
#define SP_L2_MAX 472u
#define SP_L3_MAX 640u
#define SP_L4_MAX 844u
#define SP_L5_MAX 1084u

#define SP_R0_MAX 180u
#define SP_R1_MAX 324u
#define SP_R2_MAX 516u
#define SP_R3_MAX 700u
#define SP_R4_MAX 916u
#define SP_R5_MAX 1176u

typedef struct {
    float ap_l0[SP_L0_MAX], ap_l1[SP_L1_MAX], ap_l2[SP_L2_MAX];
    float ap_l3[SP_L3_MAX], ap_l4[SP_L4_MAX], ap_l5[SP_L5_MAX];
    float ap_r0[SP_R0_MAX], ap_r1[SP_R1_MAX], ap_r2[SP_R2_MAX];
    float ap_r3[SP_R3_MAX], ap_r4[SP_R4_MAX], ap_r5[SP_R5_MAX];
} spring_arena_t;

typedef struct {
    uint32_t valid;              /* O(1) reset sample counter */
    uint32_t wr_l[SP_NUM_AP];
    uint32_t wr_r[SP_NUM_AP];
    /* Input 80 Hz HPF states */
    float    in_hp_xl, in_hp_yl;
    float    in_hp_xr, in_hp_yr;
    /* DRIP ~2.2 kHz 1-pole HPF states */
    float    drip_xl, drip_yl;
    float    drip_xr, drip_yr;
    /* Tank feedback states (1-pole LP damping + DC blocker) */
    float    fb_l, fb_r;
    float    lp_l, lp_r;
    float    dc_xl, dc_yl;
    float    dc_xr, dc_yr;
} spring_state_t;

static spring_state_t s_spring[FX_AUDIO_BUSES] __attribute__((section(".fx_bss")));

/* Mutual-prime center delay lengths @ 48 kHz (spec §3.1) */
static const float k_sp_base_l[SP_NUM_AP] = { 113.0f, 211.0f, 337.0f, 457.0f, 601.0f, 773.0f };
static const float k_sp_base_r[SP_NUM_AP] = { 127.0f, 229.0f, 367.0f, 499.0f, 653.0f, 839.0f };

static const uint32_t k_sp_max_l[SP_NUM_AP] = {
    SP_L0_MAX, SP_L1_MAX, SP_L2_MAX, SP_L3_MAX, SP_L4_MAX, SP_L5_MAX
};
static const uint32_t k_sp_max_r[SP_NUM_AP] = {
    SP_R0_MAX, SP_R1_MAX, SP_R2_MAX, SP_R3_MAX, SP_R4_MAX, SP_R5_MAX
};

/* Smooth Pade tanh soft-clipper: saturates musically into [-1, +1] */
static inline float sp_tanh(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Fractional dispersive allpass step with O(1) valid-counter safety:
 *   y[n] = -g * x[n] + d[n - D]
 *   d[n] = x[n] + g * y[n]   (canonical Schroeder/dispersive AP: |H(e^jw)| == 1)
 */
static inline float sp_allpass_step(float *buf, uint32_t buf_len, uint32_t *wr_ptr,
                                    uint32_t valid, float delay, float g, float in) {
    uint32_t w = *wr_ptr;
    float delayed = 0.0f;
    uint32_t d_int = (uint32_t)delay;
    if (valid > d_int + 1u) {
        float frac = delay - (float)d_int;
        int32_t i0 = (int32_t)w - (int32_t)d_int;
        if (i0 < 0) i0 += (int32_t)buf_len;
        int32_t i1 = i0 - 1;
        if (i1 < 0) i1 += (int32_t)buf_len;
        float s0 = buf[i0];
        float s1 = buf[i1];
        delayed = s0 + frac * (s1 - s0);
    }
    float out = delayed - g * in;
    buf[w] = in + g * out;
    if (++w >= buf_len) w = 0;
    *wr_ptr = w;
    return out;
}

void fx_spring_reset(uint32_t bus_idx, void *arena) {
    (void)arena; /* O(1) reset via valid counter — zero SDRAM memset spike in audio IRQ! */
    if (bus_idx >= FX_AUDIO_BUSES) return;
    uint32_t *p = (uint32_t *)&s_spring[bus_idx];
    for (uint32_t i = 0; i < sizeof(spring_state_t) / 4u; ++i) p[i] = 0;
}

void fx_spring_process(uint32_t bus_idx, fx_bus_ctrl_t *ctrl,
                       void *arena, float *buf_l, float *buf_r) {
    if (bus_idx >= FX_AUDIO_BUSES) return;
    spring_state_t *st = &s_spring[bus_idx];
    spring_arena_t *ar = (spring_arena_t *)arena;

    /* Knobs: 0=DWELL(0..100%), 1=DECAY(0..100%), 2=TENSION(0..100%),
     *        3=MIX(0..100%),   4=DAMP(0..100%),  5=DRIP(0..100%) */
    float dwell   = ctrl->smooth[0] * 0.01f;
    float decay   = ctrl->smooth[1] * 0.01f;
    float tension = ctrl->smooth[2] * 0.01f;
    float mix     = ctrl->smooth[3] * 0.01f;
    float damp    = ctrl->smooth[4] * 0.01f;
    float drip    = ctrl->smooth[5] * 0.01f;

    int zero_mix = (ctrl->target[3] == 0 && mix < 0.002f);

    /* DWELL: 0.15x .. 2.0x input drive into soft clipper */
    float drive = 0.15f + dwell * 1.85f;

    /* DECAY: feedback gain 0.35 .. 0.955 (~0.3 s to ~3.5 s tail) */
    float fb_gain = 0.35f + decay * 0.605f;

    /* TENSION: 0% -> 1.35x (slower/boomy), 50% -> 1.00x, 100% -> 0.68x (tight/fast) */
    float t_scale = 1.35f - tension * 0.67f;

    /* DAMP: feedback 1-pole LP (~8 kHz -> ~800 Hz), slightly brighter at high TENSION */
    float eff_damp = damp * (1.0f - 0.18f * tension);
    if (eff_damp < 0.0f) eff_damp = 0.0f;
    if (eff_damp > 1.0f) eff_damp = 1.0f;
    /* lp_a = 1 - alpha: low eff_damp -> ~0.32 (~8 kHz), high eff_damp -> ~0.90 (~800 Hz) */
    float lp_pole = 0.32f + eff_damp * 0.58f;
    float lp_in = 1.0f - lp_pole;

    /* DRIP transient tap gain */
    float drip_gain = drip * 1.35f;

    /* Allpass dispersion coefficient: slightly tighter chirp at high TENSION */
    float ap_g = 0.60f + tension * 0.08f;

    float dry_gain = 1.0f - mix;
    /* Fixed 80% stereo crossfeed (spec §3.4: wet1=0.90, wet2=0.10) */
    const float wet1 = 0.90f;
    const float wet2 = 0.10f;

    /* Precompute per-block scaled allpass delays (strictly inside allocated buffers) */
    float dl[SP_NUM_AP], dr[SP_NUM_AP];
    for (uint32_t k = 0; k < SP_NUM_AP; ++k) {
        float l = k_sp_base_l[k] * t_scale;
        float r = k_sp_base_r[k] * t_scale;
        if (l < 16.0f) l = 16.0f;
        if (l > (float)(k_sp_max_l[k] - 4u)) l = (float)(k_sp_max_l[k] - 4u);
        if (r < 16.0f) r = 16.0f;
        if (r > (float)(k_sp_max_r[k] - 4u)) r = (float)(k_sp_max_r[k] - 4u);
        dl[k] = l;
        dr[k] = r;
    }

    float *bufs_l[SP_NUM_AP] = { ar->ap_l0, ar->ap_l1, ar->ap_l2, ar->ap_l3, ar->ap_l4, ar->ap_l5 };
    float *bufs_r[SP_NUM_AP] = { ar->ap_r0, ar->ap_r1, ar->ap_r2, ar->ap_r3, ar->ap_r4, ar->ap_r5 };

    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; ++i) {
        float in_l = buf_l[i];
        float in_r = buf_r[i];

        /* 1. Input ~80 Hz 1-pole HPF (clears sub rumble from spring tank) */
        float hp_l = 0.9895f * (st->in_hp_yl + in_l - st->in_hp_xl);
        st->in_hp_xl = in_l;
        st->in_hp_yl = hp_l;

        float hp_r = 0.9895f * (st->in_hp_yr + in_r - st->in_hp_xr);
        st->in_hp_xr = in_r;
        st->in_hp_yr = hp_r;

        /* 2. DRIP 1-pole ~2.2 kHz highpass transient tap */
        float tr_l = 0.75f * (st->drip_yl + hp_l - st->drip_xl);
        st->drip_xl = hp_l;
        st->drip_yl = tr_l;

        float tr_r = 0.75f * (st->drip_yr + hp_r - st->drip_xr);
        st->drip_xr = hp_r;
        st->drip_yr = tr_r;

        /* 3. DWELL drive + DRIP transient ping -> soft clip into tank */
        float drv_l = sp_tanh((hp_l * drive + tr_l * drip_gain) * 0.70f);
        float drv_r = sp_tanh((hp_r * drive + tr_r * drip_gain) * 0.70f);

        /* 4. Tank inputs = driven excitation + damped cross-coupled feedback */
        float xl = drv_l + fb_gain * (st->fb_l * 0.94f + st->fb_r * 0.06f);
        float xr = drv_r + fb_gain * (st->fb_r * 0.94f + st->fb_l * 0.06f);

        uint32_t v = st->valid;

        /* 5. Six series dispersive allpasses per tank */
        for (uint32_t k = 0; k < SP_NUM_AP; ++k) {
            xl = sp_allpass_step(bufs_l[k], k_sp_max_l[k], &st->wr_l[k], v, dl[k], ap_g, xl);
            xr = sp_allpass_step(bufs_r[k], k_sp_max_r[k], &st->wr_r[k], v, dr[k], ap_g, xr);
        }
        if (st->valid < 2048u) st->valid++;

        /* 6. Feedback damping LP + gentle DC blocker + tank-output soft limiter */
        st->lp_l = st->lp_l * lp_pole + xl * lp_in;
        st->lp_r = st->lp_r * lp_pole + xr * lp_in;

        float dc_l = 0.994f * (st->dc_yl + st->lp_l - st->dc_xl);
        st->dc_xl = st->lp_l;
        st->dc_yl = dc_l;

        float dc_r = 0.994f * (st->dc_yr + st->lp_r - st->dc_xr);
        st->dc_xr = st->lp_r;
        st->dc_yr = dc_r;

        /* Soft limiter guarantees bounded crash even at DWELL=100% + DECAY=100% */
        float lim_l = sp_tanh(dc_l * 0.85f) * (1.0f / 0.85f);
        float lim_r = sp_tanh(dc_r * 0.85f) * (1.0f / 0.85f);
        st->fb_l = lim_l;
        st->fb_r = lim_r;

        /* 7. Fixed 80% stereo crossfeed + wet/dry mix */
        if (!zero_mix) {
            float wl = (lim_l * wet1 + lim_r * wet2) * 0.85f;
            float wr = (lim_r * wet1 + lim_l * wet2) * 0.85f;
            buf_l[i] = in_l * dry_gain + wl * mix;
            buf_r[i] = in_r * dry_gain + wr * mix;
        }
    }
}
