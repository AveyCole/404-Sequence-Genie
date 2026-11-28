#include "fx_engine.h"

/* ============================================================================
 * FX 54: FREEZE (Granular Freeze / Live Cloud)
 *   - 2.0 s @ 48 kHz stereo int16 circular capture buffer (384,000 B in bus arena)
 *   - Up to 4 concurrent Hann-windowed stereo grains (8 channels total), preallocated
 *   - Live mode (FREEZE < 50): records continuously, grains read behind write ptr
 *   - Frozen mode (FREEZE >= 50): recording stops, POSITN scrubs frozen 2 s buffer
 *   - Short-buffer safe wrap when freezing before 2.0 s have been recorded
 *   - Smooth 10 ms wet crossfade on freeze/unfreeze crossing, O(1) arena reset
 * ============================================================================ */

#define FREEZE_BUF_LEN    96000u   /* 2.00 s @ 48 kHz stereo int16 = 384,000 B */
#define FREEZE_MAX_GRAINS 4u

typedef struct {
    int16_t cap_l[FREEZE_BUF_LEN];
    int16_t cap_r[FREEZE_BUF_LEN];
} freeze_arena_t;

typedef struct {
    uint8_t  active;
    float    pos_l;        /* fractional buffer read position [0, ring_len) */
    float    pos_r;
    float    rate;         /* 2^(semi/12) in [0.5 .. 2.0] */
    uint32_t age;
    uint32_t len;          /* output duration in samples (480..12000) */
    float    inv_len;
} freeze_grain_t;

typedef struct {
    uint32_t wr;
    uint32_t valid;        /* total valid recorded samples (0..FREEZE_BUF_LEN) */
    uint32_t live_valid;   /* samples written since last unfreeze (prevents reading stale audio) */
    uint32_t rng;
    uint32_t spawn_cnt;
    uint8_t  is_frozen;
    uint8_t  pos_init;
    float    smooth_pos;   /* per-sample smoothed POSITN [0..1] (~20 ms) */
    float    xfade_phase;  /* >0 while 10 ms transition crossfade is active */
    freeze_grain_t grains[FREEZE_MAX_GRAINS];
} freeze_state_t;

static freeze_state_t s_freeze[FX_AUDIO_BUSES] __attribute__((section(".fx_bss")));

/* Exact 2^(semi/12) for semi = -12..+12 (indices 0..24) */
static const float k_frz_pitch[25] = {
    0.500000f, 0.529732f, 0.561231f, 0.594604f, 0.629961f, 0.667420f,
    0.707107f, 0.749154f, 0.793701f, 0.840896f, 0.890899f, 0.943874f,
    1.000000f,
    1.059463f, 1.122462f, 1.189207f, 1.259921f, 1.334840f, 1.414214f,
    1.498307f, 1.587401f, 1.681793f, 1.781797f, 1.887749f, 2.000000f
};

/* Exact symmetric half-Hann LUT: 0.5*(1 - cos(pi*i/128)) for i=0..128 */
static const float k_frz_hann[129] = {
    0.000000f, 0.000151f, 0.000602f, 0.001355f, 0.002408f, 0.003760f, 0.005412f, 0.007361f,
    0.009607f, 0.012149f, 0.014984f, 0.018112f, 0.021530f, 0.025236f, 0.029228f, 0.033504f,
    0.038060f, 0.042895f, 0.048005f, 0.053388f, 0.059039f, 0.064957f, 0.071136f, 0.077573f,
    0.084265f, 0.091208f, 0.098396f, 0.105827f, 0.113495f, 0.121396f, 0.129524f, 0.137876f,
    0.146447f, 0.155230f, 0.164221f, 0.173414f, 0.182803f, 0.192384f, 0.202150f, 0.212096f,
    0.222215f, 0.232501f, 0.242949f, 0.253551f, 0.264302f, 0.275194f, 0.286222f, 0.297379f,
    0.308658f, 0.320053f, 0.331555f, 0.343159f, 0.354858f, 0.366643f, 0.378509f, 0.390447f,
    0.402455f, 0.414519f, 0.426635f, 0.438793f, 0.450991f, 0.463218f, 0.475469f, 0.487734f,
    0.500000f, 0.512266f, 0.524531f, 0.536782f, 0.549009f, 0.561207f, 0.573365f, 0.585481f,
    0.597545f, 0.609553f, 0.621491f, 0.633357f, 0.645142f, 0.656841f, 0.668445f, 0.679947f,
    0.691342f, 0.702621f, 0.713778f, 0.724806f, 0.735698f, 0.746449f, 0.757051f, 0.767499f,
    0.777785f, 0.787904f, 0.797850f, 0.807616f, 0.817197f, 0.826586f, 0.835779f, 0.844770f,
    0.853553f, 0.862124f, 0.870476f, 0.878604f, 0.886505f, 0.894173f, 0.901604f, 0.908792f,
    0.915735f, 0.922427f, 0.928864f, 0.935043f, 0.940961f, 0.946612f, 0.951995f, 0.957105f,
    0.961940f, 0.966496f, 0.970772f, 0.974764f, 0.978470f, 0.981888f, 0.985016f, 0.987851f,
    0.990393f, 0.992639f, 0.994588f, 0.996240f, 0.997592f, 0.998645f, 0.999398f, 0.999849f,
    1.000000f
};

static inline int16_t frz_f_to_s16(float x) {
    float s = x * 32767.0f;
    if (s > 32767.0f) return 32767;
    if (s < -32768.0f) return -32768;
    return (int16_t)s;
}

static inline float frz_s16_to_f(int16_t s) {
    return (float)s * (1.0f / 32768.0f);
}

static inline uint32_t frz_xorshift(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Exact symmetric Hann window on [0, 1] using 129-entry half LUT */
static inline float frz_hann01(float p) {
    if (p <= 0.0f || p >= 1.0f) return 0.0f;
    float q = (p <= 0.5f) ? (p * 256.0f) : ((1.0f - p) * 256.0f);
    uint32_t idx = (uint32_t)q;
    if (idx >= 128u) return 1.0f;
    float frac = q - (float)idx;
    return k_frz_hann[idx] + frac * (k_frz_hann[idx + 1u] - k_frz_hann[idx]);
}

static inline float frz_wrap_len(float p, float ring_len) {
    while (p < 0.0f) p += ring_len;
    while (p >= ring_len) p -= ring_len;
    return p;
}

static inline float frz_read_lin(const int16_t *buf, float pos, uint32_t ring_len) {
    uint32_t i0 = (uint32_t)pos;
    if (i0 >= ring_len) i0 = 0;
    uint32_t i1 = i0 + 1u;
    if (i1 >= ring_len) i1 = 0;
    float frac = pos - (float)i0;
    float s0 = frz_s16_to_f(buf[i0]);
    float s1 = frz_s16_to_f(buf[i1]);
    return s0 + frac * (s1 - s0);
}

void fx_freeze_reset(uint32_t bus_idx, void *arena) {
    (void)arena; /* O(1) reset via valid counters — zero 384 KB memset spike in audio IRQ! */
    if (bus_idx >= FX_AUDIO_BUSES) return;
    uint32_t *p = (uint32_t *)&s_freeze[bus_idx];
    for (uint32_t i = 0; i < sizeof(freeze_state_t) / 4u; ++i) p[i] = 0;
    uint32_t seed = 0xA5A55A5Au ^ (bus_idx * 0x9E3779B9u) ^ DWT_CYCCNT ^ (g_fx.block_count * 2654435761u);
    if (seed == 0) seed = 0x31415926u + bus_idx;
    s_freeze[bus_idx].rng = seed;
}

static void spawn_grain(freeze_state_t *st, uint32_t grain_len, float rate,
                        float pos01, uint8_t frozen) {
    /* Pick a free grain slot, or steal the oldest grain if all 4 are active */
    int slot = -1;
    uint32_t max_age = 0;
    int oldest = 0;
    for (uint32_t g = 0; g < FREEZE_MAX_GRAINS; ++g) {
        if (!st->grains[g].active) {
            slot = (int)g;
            break;
        }
        if (st->grains[g].age >= max_age) {
            max_age = st->grains[g].age;
            oldest = (int)g;
        }
    }
    if (slot < 0) slot = oldest;

    /* Available history window: in live mode never read stale pre-freeze audio */
    uint32_t avail = frozen ? st->valid : st->live_valid;
    if (avail < 64u) return;

    /* Active ring span: before 2.0 s wrap, only [0, st->wr) contains valid audio */
    float ring_len = (st->valid < FREEZE_BUF_LEN) ? (float)st->wr : (float)FREEZE_BUF_LEN;
    if (ring_len < 64.0f) return;

    /* Required read span in buffer so a live pitched grain does not overtake write pointer */
    float span_in = (float)grain_len * rate + 104.0f; /* +2 ms jitter + safety */
    if (span_in < 64.0f) span_in = 64.0f;

    /* In live mode right after unfreeze, cap grain duration so pitched read stays behind wr */
    uint32_t eff_len = grain_len;
    if (!frozen && rate > 1.0f) {
        float max_live_len = ((float)avail - 104.0f) / (rate - 1.0f);
        if (max_live_len < 64.0f) return;
        if ((float)eff_len > max_live_len) eff_len = (uint32_t)max_live_len;
    }

    /* Random offset (+/-10% of grain_len) and stereo L/R jitter (+/-2 ms = +/-96 samp) */
    uint32_t r = frz_xorshift(&st->rng);
    float rand_off = (((float)(int32_t)(r & 0xFFFFu) / 32768.0f) - 1.0f) * ((float)eff_len * 0.10f);
    float stereo_jit = (((float)(int32_t)((r >> 16) & 0xFFFFu) / 32768.0f) - 1.0f) * 96.0f;

    float min_behind = frozen ? 104.0f : span_in;
    float max_behind = (float)(avail > 64u ? avail - 4u : avail);
    if (min_behind > max_behind) min_behind = max_behind;

    /* POSITN: 0 = oldest (max_behind), 1 = newest (min_behind) */
    float behind = max_behind + pos01 * (min_behind - max_behind) + rand_off;
    if (behind < min_behind) behind = min_behind;
    if (behind > max_behind) behind = max_behind;

    float start_base = (float)st->wr - behind;
    float start_l = frz_wrap_len(start_base + stereo_jit, ring_len);
    float start_r = frz_wrap_len(start_base - stereo_jit, ring_len);

    freeze_grain_t *gr = &st->grains[slot];
    gr->active = 1u;
    gr->pos_l = start_l;
    gr->pos_r = start_r;
    gr->rate = rate;
    gr->age = 0;
    gr->len = eff_len;
    gr->inv_len = 1.0f / (float)eff_len;
}

void fx_freeze_process(uint32_t bus_idx, fx_bus_ctrl_t *ctrl,
                       void *arena, float *buf_l, float *buf_r) {
    if (bus_idx >= FX_AUDIO_BUSES) return;
    freeze_state_t *st = &s_freeze[bus_idx];
    freeze_arena_t *ar = (freeze_arena_t *)arena;

    /* Knobs: 0=GRAIN(10..250ms), 1=PITCH(0..24 => -12..+12st), 2=DENSITY(0..100%),
     *        3=MIX(0..100%), 4=FREEZE(0..100, >=50 frozen), 5=POSITN(0..100%) */
    float grain_ms = ctrl->smooth[0];
    if (grain_ms < 10.0f) grain_ms = 10.0f;
    if (grain_ms > 250.0f) grain_ms = 250.0f;
    uint32_t grain_len = (uint32_t)(grain_ms * 48.0f);

    int p_idx = (int)ctrl->target[1];
    if (p_idx < 0) p_idx = 0;
    if (p_idx > 24) p_idx = 24;
    float rate = k_frz_pitch[p_idx];

    float dens = ctrl->smooth[2] * 0.01f; /* 0..1 */
    if (dens < 0.0f) dens = 0.0f;
    if (dens > 1.0f) dens = 1.0f;

    /* Overlap curve: DENSITY 0% -> 0.65 (sparse stutter with gaps),
     *                50% -> 2.0 (2 overlapping), 100% -> 4.0 (4 overlapping cloud) */
    float overlap;
    if (dens <= 0.5f) {
        overlap = 0.65f + (dens * 2.0f) * (2.0f - 0.65f);
    } else {
        overlap = 2.0f + ((dens - 0.5f) * 2.0f) * 2.0f;
    }
    uint32_t spawn_period = (uint32_t)((float)grain_len / overlap);
    if (spawn_period < 64u) spawn_period = 64u;
    if (st->spawn_cnt > spawn_period) st->spawn_cnt = spawn_period;

    /* Smooth gain normalization across density 0.65..4.0 (~1/sqrt(overlap)) */
    float norm_gain = 1.0f / (0.62f + 0.36f * overlap);

    float mix = ctrl->smooth[3] * 0.01f;
    int zero_mix = (ctrl->target[3] == 0 && mix < 0.002f);

    uint8_t frozen = (ctrl->target[4] >= 50) ? 1u : 0u;
    if (frozen != st->is_frozen) {
        st->is_frozen = frozen;
        st->xfade_phase = 1.0f; /* 10 ms (480 samp) raised-cosine wet transition dip */
        if (!frozen) {
            /* Resuming live capture: prevent reading stale pre-freeze audio ahead of wr */
            st->live_valid = 0u;
        }
    }

    float target_pos = (float)ctrl->target[5] * 0.01f;
    if (target_pos < 0.0f) target_pos = 0.0f;
    if (target_pos > 1.0f) target_pos = 1.0f;
    if (!st->pos_init) {
        st->smooth_pos = target_pos;
        st->pos_init = 1u;
    }

    float dry_gain = 1.0f - mix;

    for (uint32_t i = 0; i < FX_BLOCK_FRAMES; ++i) {
        float in_l = buf_l[i];
        float in_r = buf_r[i];

        /* Capture continuously in live mode (even at MIX 0 so freezing later has audio!) */
        if (!frozen) {
            ar->cap_l[st->wr] = frz_f_to_s16(in_l);
            ar->cap_r[st->wr] = frz_f_to_s16(in_r);
            if (++st->wr >= FREEZE_BUF_LEN) st->wr = 0;
            if (st->valid < FREEZE_BUF_LEN) st->valid++;
            if (st->live_valid < FREEZE_BUF_LEN) st->live_valid++;
        }

        /* Per-sample POSITION smoothing (~20 ms time constant at 48 kHz) */
        st->smooth_pos += 0.001f * (target_pos - st->smooth_pos);

        /* Deterministic grain spawn scheduler */
        if (st->spawn_cnt == 0u) {
            spawn_grain(st, grain_len, rate, st->smooth_pos, frozen);
            st->spawn_cnt = spawn_period;
        } else {
            st->spawn_cnt--;
        }

        /* Active valid ring length: wraps inside [0, st->wr) before buffer has filled 2.0 s */
        uint32_t ring_u = (st->valid < FREEZE_BUF_LEN) ? st->wr : FREEZE_BUF_LEN;
        float ring_f = (float)ring_u;

        /* Render up to 4 concurrent Hann-windowed grains */
        float wet_l = 0.0f;
        float wet_r = 0.0f;
        if (ring_u >= 64u) {
            for (uint32_t g = 0; g < FREEZE_MAX_GRAINS; ++g) {
                freeze_grain_t *gr = &st->grains[g];
                if (!gr->active) continue;

                float w = frz_hann01((float)gr->age * gr->inv_len);
                wet_l += w * frz_read_lin(ar->cap_l, gr->pos_l, ring_u);
                wet_r += w * frz_read_lin(ar->cap_r, gr->pos_r, ring_u);

                gr->pos_l += gr->rate;
                if (gr->pos_l >= ring_f) gr->pos_l -= ring_f;
                gr->pos_r += gr->rate;
                if (gr->pos_r >= ring_f) gr->pos_r -= ring_f;

                if (++gr->age >= gr->len) {
                    gr->active = 0u;
                }
            }
        }

        /* 10 ms click-free wet transition crossfade on freeze/unfreeze crossing */
        float xfade = 1.0f;
        if (st->xfade_phase > 0.0f) {
            /* Raised-cosine dip 1 -> 0.15 -> 1 over 480 samples (10 ms) */
            xfade = 1.0f - 0.85f * frz_hann01(st->xfade_phase);
            st->xfade_phase -= (1.0f / 480.0f);
            if (st->xfade_phase < 0.0f) st->xfade_phase = 0.0f;
        }

        if (!zero_mix) {
            float scale = norm_gain * xfade * mix;
            buf_l[i] = in_l * dry_gain + wet_l * scale;
            buf_r[i] = in_r * dry_gain + wet_r * scale;
        }
    }
}
