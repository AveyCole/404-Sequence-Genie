#include "fx_engine.h"

fx_global_t g_fx __attribute__((section(".fx_bss")));

/* Spec defaults for all 12 new effects (72 parameters: IDs 627..698, all within Roland 0..250 range) */
static const int16_t k_defaults[FX_NEW_COUNT][6] = {
    /* 49: STOP        TIME(x5ms=375) FB  MIX DROP TELE CRUSH(0=9bit..5=4bit) */
    { 75, 40, 50, 30, 100, 0 },
    /* 50: Dub Delay   TIME(x10ms=450) FB MIX TONE MOD  SYNC(0=Off..5=1/32) */
    { 45, 55, 40, 35,  15, 0 },
    /* 51: Algoverb    SIZE  DEC  DAMP MIX  PRE  WIDTH */
    { 60, 50, 40, 35,  20, 100 },
    /* 52: Gated Verb  SIZE  DAMP THR(-36dB) HOLD(x5ms=150) REL MIX */
    { 70, 30, 24, 30,  30, 50 },
    /* 53: PitchedEcho TIME(x5ms=350) FB PIT(+7st) MIX TONE GRAIN */
    { 70, 45, 19, 45,  30, 55 },
    /* 54: FREEZE      GRAIN(80ms) PIT(0st=12) DENS(50%) MIX(50%) FRZ(0=Live) POS(85%) */
    { 80, 12, 50, 50,   0, 85 },
    /* 55: SPRING      DWELL(60%) DECAY(50%) TENSION(50%) MIX(35%) DAMP(40%) DRIP(50%) */
    { 60, 50, 50, 35,  40, 50 },
    /* 56: SHIMMER     SHIMMER(55%) PITCH(+12st=24) SIZE(70%) DECAY(65%) DAMP(35%) MIX(45%) */
    { 55, 24, 70, 65,  35, 45 },
    /* 57: GEN LOSS    WOW(35%) FLUTTER(25%) FAILURE(30%) SAT(40%) BW(35%) NOISE(20%) */
    { 35, 25, 30, 40,  35, 20 },
    /* 58: CHORD RESO  ROOT(C2=0) CHORD(MIN=0) DECAY(65%) DAMP(35%) SPREAD(65%) MIX(45%) */
    {  0,  0, 65, 35,  65, 45 },
    /* 59: TRANSIENT   ATTACK(+25%=125) SUSTAIN(0%=100) PUNCH(25%) DRIVE(15%) TONE(0=50) MIX(100%) */
    {125,100, 25, 15,  50,100 },
    /* 60: MICROLOOP   LENGTH(40~210ms) MODE(FWD=0) GLITCH(30%) FADE(25%) SYNC(OFF=0) MIX(50%) */
    { 40,  0, 30, 25,   0, 50 },
    { 70, 65, 25, 50, 15, 55 },
    { 35, 65, 55, 65, 40, 40 },
    { 45, 25, 55, 40, 80, 55 },
    { 60, 45, 50, 40, 35, 65 },
};

static const int16_t k_min[FX_NEW_COUNT][6] = {
    {  0,  0,  0,   0,   0,  0 },
    {  0,  0,  0,   0,   0,  0 },
    {  0,  0,  0,   0,   0,  0 },
    {  0,  0,  0,   2,   5,  0 },
    {  0,  0,  0,   0,   0, 20 },
    { 10,  0,  0,   0,   0,  0 },
    {  0,  0,  0,   0,   0,  0 },
    {  0,  0,  0,   0,   0,  0 },
    {  0,  0,  0,   0,   0,  0 },
    {  0,  0,  0,   0,   0,  0 },
    {  0,  0,  0,   0,   0,  0 },
    {  0,  0,  0,   0,   0,  0 },
    { 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0 },
};

static const int16_t k_max[FX_NEW_COUNT][6] = {
    { 200, 95, 100, 100, 100,   5 },
    { 200, 98, 100, 100, 100,   5 },
    { 100,100, 100, 100, 200, 100 },
    { 100,100,  60, 100, 200, 100 },
    { 200, 90,  24, 100, 100, 100 },
    { 250, 24, 100, 100, 100, 100 },
    { 100,100, 100, 100, 100, 100 },
    { 100, 24, 100, 100, 100, 100 },
    { 100,100, 100, 100, 100, 100 },
    {  23,  7, 100, 100, 100, 100 },
    { 200,200, 100, 100, 100, 100 },
    { 100,  5, 100, 100,   5, 100 },
    { 100, 100, 100, 100, 100, 100 },
    { 100, 100, 100, 100, 100, 100 },
    { 100, 100, 100, 100, 100, 100 },
    { 100, 100, 100, 100, 100, 100 },
};

void fx_ensure_init(void) {
    if (g_fx.initialized == 0x46583534u) return;
    for (uint32_t s = 0; s < FX_SLOTS; ++s) {
        g_fx.slot_fx[s] = 0;
        for (uint32_t e = 0; e < FX_NEW_COUNT; ++e) {
            g_fx.dirty[s][e] = 0;
            for (uint32_t k = 0; k < 6; ++k) {
                g_fx.val[s][e][k] = k_defaults[e][k];
            }
        }
    }
    for (uint32_t b = 0; b < FX_AUDIO_BUSES; ++b) {
        g_fx.bus[b].active_fx = 0;
        g_fx.bus[b].loaded_fx = 0;
        g_fx.bus[b].select_seq = 0;
        g_fx.bus[b].bypassed = 0;
        for (uint32_t k = 0; k < 6; ++k) {
            g_fx.bus[b].target[k] = 0;
            g_fx.bus[b].smooth[k] = 0.0f;
        }
    }
    g_fx.seq_counter = 1;
    g_fx.cached_bpm100 = 12000;
    /* Enable DWT cycle counter for CPU guard (safe on Cortex-M7) */
    COREDEBUG_DEMCR |= (1u << 24);
    DWT_CTRL |= 1u;
    g_fx.initialized = 0x46583534u;
}

static void fx_refresh_bpm(void) {
    int32_t bpm100 = PROJECT_BPM100;
    if (bpm100 >= 4000 && bpm100 <= 24000) {
        g_fx.cached_bpm100 = (uint32_t)bpm100;
    }
}

/* The UI keeps the chosen effect even when the transport sends Bypass. */
static uint32_t fx_selected_on_bus(uint32_t slot) {
    return ((volatile const uint16_t *)(SYSPRM_ADDR + 0xc4u))[slot];
}

/* The stock effect selector reads 0x276 here. The separate BMC bus
 * enable field can remain zero when its effect type is Bypass. */
static uint32_t fx_bus_enabled(unsigned bus) {
    return *(volatile const uint16_t *)(SYSPRM_ADDR + 0x92u + 2u * bus);
}

uint32_t fx_on_select(uint32_t slot, uint32_t effect) {
    fx_ensure_init();
    fx_refresh_bpm();
    if (slot >= FX_SLOTS) return effect;
    g_fx.slot_fx[slot] = (uint8_t)effect;

    if (slot == 1u || slot == 2u) {
        fx_bus_ctrl_t *bc = &g_fx.bus[slot - 1u];
        if (effect >= FX_STOCK_COUNT && effect < FX_TOTAL_COUNT) {
            uint32_t idx = effect - FX_STOCK_COUNT;
            for (uint32_t k = 0; k < 6; ++k) {
                bc->target[k] = g_fx.val[slot][idx][k];
            }
            bc->select_seq = ++g_fx.seq_counter;
            bc->bypassed = 0;
            bc->active_fx = effect;
            return 0u; /* Tell BMC Bypass (0) */
        } else {
            bc->active_fx = 0u;
            bc->bypassed = 0u;
            return effect;
        }
    }
    /* Slots 0 (Input FX), 3 (BUS 3), 4 (BUS 4): remap new FX to Bypass (0) safely */
    if (effect >= FX_STOCK_COUNT && effect < FX_TOTAL_COUNT) {
        return 0u;
    }
    return effect;
}

/* Called from fx_set_ctrl_hook when CTRL 1-6 is written for effect 49..53 */
void fx_set_ctrl_ext(uint32_t bus, uint32_t effect, uint32_t knob, int32_t value) {
    fx_ensure_init();
    fx_refresh_bpm();
    if (bus >= FX_SLOTS || effect < FX_STOCK_COUNT || effect >= FX_TOTAL_COUNT || knob >= 6u) {
        return;
    }
    uint32_t idx = effect - FX_STOCK_COUNT;
    if (value < k_min[idx][knob]) value = k_min[idx][knob];
    if (value > k_max[idx][knob]) value = k_max[idx][knob];
    g_fx.val[bus][idx][knob] = (int16_t)value;
    g_fx.dirty[bus][idx] = 1u;

    if (bus == 1u || bus == 2u) {
        fx_bus_ctrl_t *bc = &g_fx.bus[bus - 1u];
        if (bc->active_fx == effect) {
            bc->target[knob] = (int16_t)value;
        }
    }
}

/* Called from fx_get_param_hook for fields 0x27F..0x285 when active fx is 49..53 */
int32_t fx_get_param_ext(uint32_t bus, uint32_t effect, uint32_t field) {
    fx_ensure_init();
    if (bus >= FX_SLOTS || effect < FX_STOCK_COUNT || effect >= FX_TOTAL_COUNT) return 0;
    uint32_t idx = effect - FX_STOCK_COUNT;
    if (field == 0x285u) {
        return (int32_t)g_fx.dirty[bus][idx];
    }
    uint32_t knob = field - 0x27fu;
    if (knob >= 6u) return 0;
    return (int32_t)g_fx.val[bus][idx][knob];
}

/* Called from fx_dirty_hook_1/2 for effect 49..53 */
void fx_set_dirty_ext(uint32_t bus, uint32_t effect, uint32_t flag) {
    fx_ensure_init();
    if (bus >= FX_SLOTS || effect < FX_STOCK_COUNT || effect >= FX_TOTAL_COUNT) return;
    g_fx.dirty[bus][effect - FX_STOCK_COUNT] = (uint8_t)(flag ? 1u : 0u);
}

/* Minimal zero-libc integer/unit string formatter for native FX screen OLED */
static char *append_str(char *p, const char *s) {
    while (*s) *p++ = *s++;
    *p = '\0';
    return p;
}

static char *append_int(char *p, int32_t v, int show_plus) {
    char tmp[12];
    int n = 0;
    if (v < 0) {
        *p++ = '-';
        v = -v;
    } else if (show_plus && v > 0) {
        *p++ = '+';
    }
    if (v == 0) {
        *p++ = '0';
        *p = '\0';
        return p;
    }
    while (v > 0 && n < 11) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) *p++ = tmp[--n];
    *p = '\0';
    return p;
}

void fx_format_param(char *out, uint32_t bus, uint32_t effect, uint32_t knob) {
    static const char *const k_sync_names[6] = {
        "OFF", "1/4", "1/8", "1/8T", "1/16", "1/32"
    };
    static const char *const k_note_names[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    static const char *const k_chord_names[8] = {
        "MIN", "MAJ", "MIN7", "MAJ7", "MIN9", "SUS4", "5THS", "OCT"
    };
    static const char *const k_ml_modes[6] = {
        "FWD", "REV", "PONG", "HALF", "DBL", "R-HALF"
    };
    fx_ensure_init();
    if (bus >= FX_SLOTS || effect < FX_STOCK_COUNT || effect >= FX_TOTAL_COUNT || knob >= 6u) {
        append_str(out, "---");
        return;
    }
    int32_t v = g_fx.val[bus][effect - FX_STOCK_COUNT][knob];
    char *p = out;
    switch (effect) {
    case FX_STOP:
        if (knob == 0) { p = append_int(p, v * 5, 0); append_str(p, "ms"); }
        else if (knob == 5) { p = append_int(p, 9 - v, 0); append_str(p, "bit"); }
        else { p = append_int(p, v, 0); append_str(p, "%"); }
        break;
    case FX_DUB_DELAY:
        if (knob == 0) { p = append_int(p, v * 10, 0); append_str(p, "ms"); }
        else if (knob == 5) { append_str(p, k_sync_names[(v >= 0 && v < 6) ? v : 0]); }
        else { p = append_int(p, v, 0); append_str(p, "%"); }
        break;
    case FX_ALGOVERB:
        if (knob == 4) { p = append_int(p, v, 0); append_str(p, "ms"); }
        else { p = append_int(p, v, 0); append_str(p, "%"); }
        break;
    case FX_GATED_VERB:
        if (knob == 2) { p = append_int(p, v - 60, 0); append_str(p, "dB"); }
        else if (knob == 3) { p = append_int(p, v * 5, 0); append_str(p, "ms"); }
        else if (knob == 4) { p = append_int(p, v, 0); append_str(p, "ms"); }
        else { p = append_int(p, v, 0); append_str(p, "%"); }
        break;
    case FX_PITCHED_ECHO:
        if (knob == 0) { p = append_int(p, v * 5, 0); append_str(p, "ms"); }
        else if (knob == 5) { p = append_int(p, v, 0); append_str(p, "ms"); }
        else if (knob == 2) { p = append_int(p, v - 12, 1); append_str(p, "st"); }
        else { p = append_int(p, v, 0); append_str(p, "%"); }
        break;
    case FX_FREEZE:
        if (knob == 0) { p = append_int(p, v, 0); append_str(p, "ms"); }
        else if (knob == 1) { p = append_int(p, v - 12, 1); append_str(p, "st"); }
        else if (knob == 4) { append_str(p, v >= 50 ? "FREEZE" : "LIVE"); }
        else { p = append_int(p, v, 0); append_str(p, "%"); }
        break;
    case FX_SPRING:
    case FX_GEN_LOSS:
        p = append_int(p, v, 0);
        append_str(p, "%");
        break;
    case FX_SHIMMER:
        if (knob == 1) { p = append_int(p, v - 12, 1); append_str(p, "st"); }
        else { p = append_int(p, v, 0); append_str(p, "%"); }
        break;
    case FX_CHORD_RESO:
        if (knob == 0) {
            int n = (v >= 0 && v < 24) ? v : 0;
            p = append_str(p, k_note_names[n % 12]);
            append_int(p, 2 + (n / 12), 0);
        } else if (knob == 1) {
            append_str(p, k_chord_names[(v >= 0 && v < 8) ? v : 0]);
        } else {
            p = append_int(p, v, 0); append_str(p, "%");
        }
        break;
    case FX_TRANSIENT:
        if (knob == 0 || knob == 1) { p = append_int(p, v - 100, 1); append_str(p, "%"); }
        else if (knob == 4) { append_int(p, v - 50, 1); }
        else { p = append_int(p, v, 0); append_str(p, "%"); }
        break;
    case FX_MICROLOOP:
        if (knob == 0) { p = append_int(p, 15 + (v * 485) / 100, 0); append_str(p, "ms"); }
        else if (knob == 1) { append_str(p, k_ml_modes[(v >= 0 && v < 6) ? v : 0]); }
        else if (knob == 4) { append_str(p, k_sync_names[(v >= 0 && v < 6) ? v : 0]); }
        else { p = append_int(p, v, 0); append_str(p, "%"); }
        break;
    default:
        append_int(p, v, 0);
        break;
    }
}

/* ============================================================================
 * Audio block entry point (called once per 64-frame block, 750 Hz, FPU saved)
 * tx_buf:    int32_t[64][16] SAI TX line 3 buffer (BUS 1 = words 2,3; BUS 2 = words 4,5)
 * stems_buf: int32_t[64][14] mixer stems at 0x202AD3A0 (BUS 1 = words 2,3; BUS 2 = words 4,5)
 * ============================================================================ */
void fx_process_block(int32_t *tx_buf, int32_t *stems_buf) {
    if (g_fx.initialized != 0x46583534u) return;
    g_fx.block_count++;

    /* Fast path: if neither BUS 1 nor BUS 2 runs a new i.MX FX, bit-identical passthrough */
    const uint32_t selected[2] = {fx_selected_on_bus(1), fx_selected_on_bus(2)};
    uint32_t fx0 = selected[0], fx1 = selected[1];
    if (fx0 < FX_STOCK_COUNT && fx1 < FX_STOCK_COUNT) {
        return;
    }

#ifdef FX_PASSTHROUGH_ONLY
    (void)tx_buf;
    (void)stems_buf;
    return;
#else
    uint32_t cyc_start = DWT_CYCCNT;
    fx_refresh_bpm();

    float buf_l[FX_BLOCK_FRAMES];
    float buf_r[FX_BLOCK_FRAMES];

    /* Type A chains BUS 1 into BUS 2 on the external DSP. If BUS 1
     * has no external effect, we already have its output in TX and can
     * move that feed into the native BUS 2 effect before the handoff. */
    const uint16_t routing = *(volatile const uint16_t *)(SYSPRM_ADDR + 0x70u);
    const int serial_native = routing == 0 &&
        (fx0 == 0 || fx0 >= FX_STOCK_COUNT || !fx_bus_enabled(0));

    for (uint32_t b = 0; b < FX_AUDIO_BUSES; ++b) {
        fx_bus_ctrl_t *bc = &g_fx.bus[b];
        uint32_t fx = selected[b];
        if (!fx_bus_enabled(b)) {
            bc->loaded_fx = 0;
            continue;
        }
        if (fx < FX_STOCK_COUNT || fx >= FX_TOTAL_COUNT || bc->bypassed) {
            continue;
        }
        void *arena = (void *)(FX_ARENA_BASE + b * FX_BUS_ARENA_SIZE);
        for (uint32_t k = 0; k < 6; ++k) {
            bc->target[k] = g_fx.val[b + 1][fx - FX_STOCK_COUNT][k];
        }
        if (bc->loaded_fx != fx) {
            for (uint32_t k = 0; k < 6; ++k) {
                bc->smooth[k] = (float)bc->target[k];
            }
            if (fx >= FX_SHIMMER) {
                fx_page4_reset(b, fx, arena);
            } else if (fx == FX_SPRING) {
                fx_spring_reset(b, arena);
            } else if (fx == FX_FREEZE) {
                fx_freeze_reset(b, arena);
            } else if (fx == FX_ALGOVERB || fx == FX_GATED_VERB) {
                fx_reverb_reset(b, fx, arena);
            } else {
                fx_delay_reset(b, fx, arena);
            }
            bc->loaded_fx = fx;
        }

        /* Extract stereo pair from packed SAI TX buffer (BUS 1 = words 2,3; BUS 2 = words 4,5) */
        uint32_t w_l = (b + 1u) * 2u;
        uint32_t w_r = w_l + 1u;
        const int serial_feed = b == 1 && serial_native;
        const float inv_scale = 1.0f / 32768.0f;
        for (uint32_t i = 0; i < FX_BLOCK_FRAMES; ++i) {
            buf_l[i] = (float)tx_buf[i * 16u + w_l] * inv_scale;
            buf_r[i] = (float)tx_buf[i * 16u + w_r] * inv_scale;
            if (serial_feed) {
                buf_l[i] += (float)tx_buf[i * 16u + 2] * inv_scale;
                buf_r[i] += (float)tx_buf[i * 16u + 3] * inv_scale;
            }
        }

        /* One-pole parameter smoothing (~8 ms time constant: alpha ~= 0.15 per 1.33 ms block) */
        for (uint32_t k = 0; k < 6; ++k) {
            bc->smooth[k] += 0.15f * ((float)bc->target[k] - bc->smooth[k]);
        }

        if (fx >= FX_SHIMMER) {
            fx_page4_process(b, fx, bc, arena, buf_l, buf_r, g_fx.cached_bpm100);
        } else if (fx == FX_SPRING) {
            fx_spring_process(b, bc, arena, buf_l, buf_r);
        } else if (fx == FX_FREEZE) {
            fx_freeze_process(b, bc, arena, buf_l, buf_r);
        } else if (fx == FX_ALGOVERB || fx == FX_GATED_VERB) {
            fx_reverb_process(b, fx, bc, arena, buf_l, buf_r);
        } else {
            fx_delay_process(b, fx, bc, arena, buf_l, buf_r, g_fx.cached_bpm100);
        }

        /* Guarantee bit-identical passthrough when MIX==0 on wet/dry effects */
        if ((fx == FX_FREEZE || fx == FX_SPRING) && bc->target[3] == 0 && bc->smooth[3] < 0.2f) {
            continue;
        }
        if ((fx == FX_SHIMMER || fx == FX_CHORD_RESO || fx == FX_TRANSIENT || fx >= FX_MICROLOOP)
            && bc->target[5] == 0 && bc->smooth[5] < 0.2f) {
            continue;
        }

        /* Write clamped 20-bit samples back to both SAI TX and stems_buf (for Bounce in Place) */
        for (uint32_t i = 0; i < FX_BLOCK_FRAMES; ++i) {
            int32_t sl = fx_clamp_stem(buf_l[i] * 32768.0f);
            int32_t sr = fx_clamp_stem(buf_r[i] * 32768.0f);
            tx_buf[i * 16u + w_l] = sl;
            tx_buf[i * 16u + w_r] = sr;
            if (serial_feed) {
                tx_buf[i * 16u + 2] = 0;
                tx_buf[i * 16u + 3] = 0;
            }
            if (stems_buf) {
                stems_buf[i * 14u + w_l] = sl;
                stems_buf[i * 14u + w_r] = sr;
                if (serial_feed) {
                    stems_buf[i * 14u + 2] = 0;
                    stems_buf[i * 14u + 3] = 0;
                }
            }
        }
    }

    /* CPU guard: bypass newest active slot > 75% (600k cycles), restore < 60% (480k cycles) */
    uint32_t elapsed = DWT_CYCCNT - cyc_start;
    g_fx.last_cycles = elapsed;
    if (elapsed > g_fx.peak_cycles) g_fx.peak_cycles = elapsed;
    if (elapsed > FX_CYC_HI) {
        int newest = -1;
        uint32_t best_seq = 0;
        for (uint32_t b = 0; b < FX_AUDIO_BUSES; ++b) {
            if (fx_bus_enabled(b) && selected[b] >= FX_STOCK_COUNT && selected[b] < FX_TOTAL_COUNT && !g_fx.bus[b].bypassed) {
                if (g_fx.bus[b].select_seq >= best_seq) {
                    best_seq = g_fx.bus[b].select_seq;
                    newest = (int)b;
                }
            }
        }
        if (newest >= 0) {
            g_fx.bus[newest].bypassed = 1u;
            g_fx.guard_trips++;
        }
    } else if (elapsed < FX_CYC_LO) {
        for (uint32_t b = 0; b < FX_AUDIO_BUSES; ++b) {
            if (g_fx.bus[b].bypassed && selected[b] >= FX_STOCK_COUNT) {
                g_fx.bus[b].bypassed = 0u;
                break;
            }
        }
    }
#endif
}
