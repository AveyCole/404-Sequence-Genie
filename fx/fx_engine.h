#ifndef FX_ENGINE_H
#define FX_ENGINE_H

#include <stdint.h>

/* Effect indices in the extended 61-entry native MFX list (4-page grid: 54/64 cells) */
#define FX_STOCK_COUNT    49u
#define FX_NEW_COUNT      16u
#define FX_TOTAL_COUNT    65u

#define FX_STOP           49u
#define FX_DUB_DELAY      50u
#define FX_ALGOVERB       51u
#define FX_GATED_VERB     52u
#define FX_PITCHED_ECHO   53u
#define FX_FREEZE         54u
#define FX_SPRING         55u
#define FX_SHIMMER        56u
#define FX_GEN_LOSS       57u
#define FX_CHORD_RESO     58u
#define FX_TRANSIENT      59u
#define FX_MICROLOOP      60u

#define FX_GHOST_PRINT    61u
#define FX_ROOM_IN_ROOM   62u
#define FX_UNDERTOW       63u
#define FX_PAPER_MACHINE  64u

/* 96 new parameter IDs above stock 0..626 (627..722) */
#define FX_PARAM_BASE     627u
#define FX_PARAM_COUNT    96u
#define FX_PARAM_TOTAL    723u

/* 5 effect slots (0 = Input FX, 1..4 = BUS 1..4); i.MX TX stems carry BUS 1 & BUS 2 */
#define FX_SLOTS          5u
#define FX_AUDIO_BUSES    2u   /* index 0 = BUS 1 (TX words 2,3), index 1 = BUS 2 (TX words 4,5) */
#define FX_BLOCK_FRAMES   64u
#define FX_SAMPLE_RATE    48000u

/* 20-bit signed stem clamp (matches stock mixer 0x0001C8B2..0x0001CA70) */
#define FX_STEM_MAX       524287
#define FX_STEM_MIN       (-524288)

/* CPU guard thresholds on 600 MHz Cortex-M7:
 * 1 block = 64 / 48000 s = 1.3333 ms = 800,000 cycles.
 * Bypass newest active slot > 75% (600,000 cyc), restore < 60% (480,000 cyc). */
#define FX_CYC_BUDGET     800000u
#define FX_CYC_HI         600000u
#define FX_CYC_LO         480000u

/* Fixed SDRAM workspace arena for BUS 1 (384 KB) and BUS 2 (384 KB) */
#define FX_ARENA_BASE     0x83557000u
#define FX_BUS_ARENA_SIZE (384u * 1024u)

/* Hardware registers & stock firmware addresses */
#define DWT_CTRL          (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT        (*(volatile uint32_t *)0xE0001004u)
#define COREDEBUG_DEMCR   (*(volatile uint32_t *)0xE000EDFCu)
#define SYSPRM_ADDR       0x82DFEED0u
#define PROJECT_BPM100    (*(volatile int32_t *)0x82E00A60u)  /* PROJECT + 0x90 (BPM * 100) */
#define TX_STEMS_ADDR     0x202AD3A0u

/* Per-bus live shadow state read by the audio DSP */
typedef struct {
    volatile uint32_t active_fx;      /* 0..53 (49..53 = i.MX DSP active) */
    volatile uint32_t loaded_fx;      /* last FX initialized in this bus's arena */
    volatile uint32_t select_seq;     /* incremented on effect select (orders newest slot) */
    volatile uint32_t bypassed;       /* set by CPU guard if overloaded */
    volatile int16_t  target[6];      /* discrete knob values from UI/DT1 */
    float             smooth[6];      /* one-pole smoothed normalized/unit values */
} fx_bus_ctrl_t;

typedef struct {
    uint32_t initialized;
    uint32_t seq_counter;
    uint32_t block_count;
    uint32_t last_cycles;
    uint32_t peak_cycles;
    uint32_t guard_trips;
    uint32_t cached_bpm100;           /* 4000..24000 (40.00..240.00 BPM), default 12000 */
    uint8_t  slot_fx[FX_SLOTS];       /* current active effect per slot 0..4 */
    uint8_t  dirty[FX_SLOTS][FX_NEW_COUNT];
    int16_t  val[FX_SLOTS][FX_NEW_COUNT][6];
    fx_bus_ctrl_t bus[FX_AUDIO_BUSES];
} fx_global_t;

extern fx_global_t g_fx;

/* Plumbing API (fx_plumbing.c) */
void fx_ensure_init(void);
uint32_t fx_on_select(uint32_t slot, uint32_t effect);
void fx_set_ctrl_ext(uint32_t bus, uint32_t effect, uint32_t knob, int32_t value);
int32_t fx_get_param_ext(uint32_t bus, uint32_t effect, uint32_t field);
void fx_set_dirty_ext(uint32_t bus, uint32_t effect, uint32_t flag);
void fx_format_param(char *out, uint32_t bus, uint32_t effect, uint32_t knob);
void fx_process_block(int32_t *tx_buf, int32_t *stems_buf);

/* DSP engines (fx_delay_core.c & fx_reverb_core.c) */
void fx_delay_reset(uint32_t bus_idx, uint32_t fx_id, void *arena);
void fx_delay_process(uint32_t bus_idx, uint32_t fx_id, fx_bus_ctrl_t *ctrl,
                      void *arena, float *buf_l, float *buf_r, uint32_t bpm100);

void fx_reverb_reset(uint32_t bus_idx, uint32_t fx_id, void *arena);
void fx_reverb_process(uint32_t bus_idx, uint32_t fx_id, fx_bus_ctrl_t *ctrl,
                       void *arena, float *buf_l, float *buf_r);

void fx_freeze_reset(uint32_t bus_idx, void *arena);
void fx_freeze_process(uint32_t bus_idx, fx_bus_ctrl_t *ctrl,
                       void *arena, float *buf_l, float *buf_r);

void fx_spring_reset(uint32_t bus_idx, void *arena);
void fx_spring_process(uint32_t bus_idx, fx_bus_ctrl_t *ctrl,
                       void *arena, float *buf_l, float *buf_r);

void fx_page4_reset(uint32_t bus_idx, uint32_t fx_id, void *arena);
void fx_page4_process(uint32_t bus_idx, uint32_t fx_id, fx_bus_ctrl_t *ctrl,
                      void *arena, float *buf_l, float *buf_r, uint32_t bpm100);

static inline int32_t fx_clamp_stem(float x) {
    if (x > (float)FX_STEM_MAX) return FX_STEM_MAX;
    if (x < (float)FX_STEM_MIN) return FX_STEM_MIN;
    return (int32_t)x;
}

#endif
