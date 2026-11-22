/* Native instrument preview and voice queue for DOOM OS 0.7.4 (651C). */
#include <stdint.h>

#define CODE __attribute__((section(".doom_track"), noinline))
#define UI __attribute__((section(".doom_source"), noinline))
#define STATE __attribute__((section(".doom_data")))
#define DOOM_CURRENT (*(volatile uintptr_t *)0x80202720u)
#define CURRENT_PROJECT ((unsigned (*)(void))0x800e1191u)
#define GET_INSTRUMENT ((void *(*)(uintptr_t, unsigned))0x603361b9u)
#define POPUP ((void (*)(const char *, unsigned))0x800d3ff1u)

static volatile unsigned selected STATE;
static volatile unsigned selected_project STATE;
static unsigned active_project_id STATE;
static unsigned active_pad STATE;
static int active_pitch STATE;
static uintptr_t active_project STATE;
static char source_name[20] STATE;

static CODE uintptr_t loaded_project(void) {
    uintptr_t state = DOOM_CURRENT;
    if (!state || *(volatile unsigned *)(state + 0x61d0u) != CURRENT_PROJECT() + 1u)
        return 0;
    return state;
}

static CODE unsigned enqueue(uintptr_t state, unsigned kind, unsigned pad, unsigned velocity,
                             int pitch, unsigned voice, unsigned arg, unsigned release) {
    unsigned primask;
    __asm__ volatile("mrs %0, primask\ncpsid i" : "=r"(primask) :: "memory");
    volatile unsigned *ring = (volatile unsigned *)(state + 0x4e894u);
    unsigned write = ring[0];
    unsigned room = write - ring[1] < 64;
    if (room) {
        volatile unsigned *event = ring + 3 + (write & 63u) * 8;
        event[0] = kind;
        event[1] = pad;
        event[2] = velocity;
        event[3] = (unsigned)pitch;
        event[4] = voice;
        event[5] = arg;
        event[6] = release;
        __asm__ volatile("dmb" ::: "memory");
        ring[0] = write + 1;
    }
    __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
    return room;
}

/* Replace the native UI-side producer too, so an audio interrupt cannot
 * race a partially written native record. The layout and consumer stay native. */
CODE void genie_doom_enqueue(unsigned kind, unsigned pad, unsigned velocity,
                            int pitch, unsigned voice, unsigned arg, unsigned release) {
    uintptr_t state = DOOM_CURRENT;
    if (state)
        enqueue(state, kind, pad, velocity, pitch, voice, arg, release);
}

CODE void genie_doom_release(void) {
    if (!active_project)
        return;
    if (loaded_project() != active_project || CURRENT_PROJECT() != active_project_id ||
        enqueue(active_project, 2, active_pad, 0, active_pitch, 0x14, 0, 1))
        active_project = 0;
}

CODE unsigned genie_doom_start(unsigned note, unsigned velocity) {
    unsigned source = selected;
    if (!source)
        return 0;
    uintptr_t state = loaded_project();
    unsigned project = CURRENT_PROJECT();
    if (state && project == selected_project && !active_project) {
        int pitch = (int)note - 60;
        while (pitch > 24)
            pitch -= 12;
        if (enqueue(state, 1, source - 1, velocity, pitch, 0x14, 0, 0)) {
            active_pad = source - 1;
            active_pitch = pitch;
            active_project_id = project;
            active_project = state;
        }
    }
    return 1;
}

UI void genie_doom_select(int direction) {
    uintptr_t state = loaded_project();
    unsigned candidate = selected;
    do {
        candidate = (candidate + 161 + direction) % 161;
    } while (candidate && (!state || !GET_INSTRUMENT(state, candidate - 1)));
    selected_project = CURRENT_PROJECT();
    selected = candidate;
    if (!candidate) {
        POPUP("SOUND GENERATOR", 850);
        return;
    }
    unsigned pad = candidate - 1;
    const char label[] = "DOOM INST A-00";
    for (unsigned i = 0; i < sizeof label; ++i)
        source_name[i] = label[i];
    source_name[10] = 'A' + pad / 16;
    source_name[12] = '0' + (pad % 16 + 1) / 10;
    source_name[13] = '0' + (pad % 16 + 1) % 10;
    POPUP(source_name, 850);
}
