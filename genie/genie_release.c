#include <stdint.h>

typedef int (*envelope_fn)(uint8_t *, int);
extern int genie_stock_envelope(uint8_t *voice, int sample);

/* Live Sound Generator slots. Export voices retain the stock envelope path. */
static struct {
    int filtered;
    unsigned age;
} release_filter[4] __attribute__((section(".release_state")));

__attribute__((section(".release"),used))
int genie_release_envelope(uint8_t *voice,int sample){
    int output=genie_stock_envelope(voice,sample);
    unsigned type=voice[0x70];
    if(type<15 || type>31)return output;
    uintptr_t address=(uintptr_t)voice;
    unsigned slot;
    if(address==0x80249800u)slot=0;
    else {
        unsigned offset=address-0x8353a310u;
        if(offset>=3*0x1b0u || offset%0x1b0u)return output;
        slot=1+offset/0x1b0u;
    }
    if(!voice[0] || !voice[1]){
        release_filter[slot].age=0;
        release_filter[slot].filtered=output*256;
        return output;
    }
    /* Bring in the low-pass over 32 frames to avoid introducing a step at
     * key-up. The stock envelope still sets the length and level of the tail. */
    int filtered=release_filter[slot].filtered;
    filtered+=(output*256-filtered)/4;
    release_filter[slot].filtered=filtered;
    unsigned age=release_filter[slot].age;
    if(age<32)release_filter[slot].age=++age;
    int blend=output+(((filtered/256)-output)*(int)age)/32;
    if(!*(const uint32_t *)(voice+0x10)){
        /* ENV OFF uses the stock signed 400-frame counter. Saturate the
         * filtered output to zero near its endpoint to avoid residual lag. */
        int remaining=199-*(const int *)(voice+0x4c);
        if(remaining<=0)return 0;
        if(remaining<32)blend=blend*remaining/32;
    }
    return blend;
}

__attribute__((naked,section(".release")))
int genie_stock_envelope(uint8_t *voice,int sample){
    __asm__ volatile(
        "push.w {r4-r9,lr}\n"
        "movw r2,#0x9800\nmovt r2,#0x8024\n"
        "movw ip,#0x048d\nmovt ip,#0x83f8\nbx ip\n");
}
