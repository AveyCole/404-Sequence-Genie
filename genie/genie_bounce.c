#include <stdint.h>

/* SP-ETC v9.53 stock sampler-task recording commands. */
#define U8(a) (*(volatile uint8_t *)(a))
#define U32(a) (*(volatile uint32_t *)(a))
#define CALL3(a) ((unsigned (*)(unsigned,unsigned,unsigned))(a))
#define CALL4(a) ((void (*)(unsigned,unsigned,unsigned,unsigned))(a))
#define REC U8(0x82e0e755u)
#define TARGET U32(0x80246e18u)
#define SLOT __attribute__((section(".capture"),noinline))
static volatile struct {
    unsigned phase, since, pad, length, snap, ext;
    unsigned frames, written, window;
    char message[10];
} take __attribute__((section(".capture_state")));

static SLOT void command(unsigned op,unsigned value){
    CALL3(0x800e25c1u)(op,value,0);
}
static SLOT void setting(unsigned id,unsigned value){
    CALL4(0x800dd0c1u)(0x82e01144u,id,value,0);
}
static SLOT unsigned occupied(unsigned pad){
    return CALL3(0x800dda39u)(0x82e009d0u,0x64,pad);
}
static SLOT void notice(const char *text){
    ((void (*)(const char *, unsigned))0x800d3ff1u)(text,1500);
}
static SLOT void saved_notice(unsigned pad){
    static const char prefix[]="PAD ";
    unsigned i=0;
    while(prefix[i]){take.message[i]=prefix[i];++i;}
    take.message[i++]='A'+pad/16;
    take.message[i++]='-';
    unsigned number=pad%16+1;
    if(number>=10){take.message[i++]='1';number-=10;}
    take.message[i++]='0'+number;
    take.message[i]=0;
    notice((const char *)take.message);
}
SLOT unsigned genie_bounce_active(void){return take.phase;}

SLOT void genie_bounce_start(unsigned now,unsigned frames){
    if(take.phase)return;
    unsigned looper=U8(0x82e0b81du);
    if(REC || TARGET!=UINT32_MAX || U8(0x82e0d705u) || U32(0x82dfff48u) ||
       (looper && looper!=6) || U8(0x82e0b81eu) ||
       !U32(0x80246e20u) || CALL3(0x800da011u)(0x82dfeed0u,0x5c,UINT32_MAX)){
        notice("REC BUSY / CHECK ROUTING");return;
    }
    unsigned pad=0;
    while(pad<160 && (occupied(pad) || CALL3(0x8001aa79u)(0x82e2cd08u,0x79,pad)))++pad;
    if(pad==160){notice("NO EMPTY PAD");return;}
    take.pad=pad;
    take.frames=frames;take.written=0;take.window=0;
    take.length=CALL3(0x800d94d9u)(0x82e01144u,0x24,0);
    take.snap=CALL3(0x800d94d9u)(0x82e01144u,0x59,0);
    take.ext=CALL3(0x800d94d9u)(0x82e01144u,0x25,0);
    setting(0x24,6);setting(0x59,0);setting(0x25,0);setting(0x1a,pad);
    CALL4(0x800d48e9u)(0x82e0ebbcu,7,0,0);
    CALL4(0x800d48e9u)(0x82e0ebbcu,1,pad%16,0);
    CALL4(0x800d48e9u)(0x82e0ebbcu,0,pad/16,0);
    command(5,0);command(0x14,0);
    take.since=now;take.phase=1;
}

/* Stock finalization may trim the onset and retain recorder stop latency.
 * Restore the physical first captured frame and use the same frame count
 * that the RX fade used, so pad playback wraps at the faded boundary. */
__attribute__((section(".capture_range"),noinline))
static unsigned set_loop_range(void){
    unsigned pad=take.pad;
    unsigned info=0x82e495e4u+36*pad;
    unsigned engine=U32(0x82e0e970u);
    if(!engine || U32(info+8)!=2 || take.written<take.window+take.frames)return 0;
    unsigned offset=U32(info+12),data=U32(engine+0x1c0);
    if(data<offset || ((data-offset)&3))return 0;
    unsigned start=(data-offset)/4+take.window;
    unsigned end=start+take.frames;
    int saved_end=(int)CALL3(0x800dc6b9u)(0x82e2cd08u,0x68,pad);
    if(end<start || saved_end<0 || (unsigned)saved_end<end)return 0;
    unsigned zero_cross=CALL3(0x800dda39u)(0x82e009d0u,0xfb,UINT32_MAX);
    if(zero_cross)CALL4(0x800ded19u)(0x82e009d0u,0xfb,0,UINT32_MAX);
    CALL4(0x800da721u)(0x82e2cd08u,0x67,start,pad);
    CALL4(0x800da721u)(0x82e2cd08u,0x68,end,pad);
    CALL4(0x800da721u)(0x82e2cd08u,0x71,start,pad);
    if(zero_cross)CALL4(0x800ded19u)(0x82e009d0u,0xfb,zero_cross,UINT32_MAX);
    ((void (*)(unsigned))0x800d6391u)(pad);
    ((void (*)(unsigned))0x800d7509u)(0x80591800u);
    return CALL3(0x800dc6b9u)(0x82e2cd08u,0x67,pad)==start &&
           CALL3(0x800dc6b9u)(0x82e2cd08u,0x68,pad)==end &&
           CALL3(0x800dc6b9u)(0x82e2cd08u,0x71,pad)==start;
}

__attribute__((section(".capture_poll"),noinline))
void genie_bounce_poll(unsigned now,unsigned cancel,unsigned finished){
    unsigned phase=take.phase;
    if(!phase)return;
    unsigned state=REC;
    unsigned age=now-take.since;
    if(phase==4 || phase==5){
        if(!state && TARGET==UINT32_MAX){
            setting(0x24,take.length);setting(0x25,take.ext);
            take.phase=0;
            if(phase==4 && occupied(take.pad)){
                unsigned exact=set_loop_range();
                command(0xd,1);
                if(exact)saved_notice(take.pad);
                else notice("PAD SAVED: CHECK LOOP");
            }
            else notice("GENIE BOUNCE ENDED");
            setting(0x59,take.snap);
        }
        return;
    }
    if(cancel || (phase<3 && age>96000) || (phase==3 && state!=2)){
        command(state==2?0x1a:0x18,0);take.phase=5;return;
    }
    if(phase==1 && state==1){command(0x17,0);take.phase=2;take.since=now;}
    if(phase==3 && finished && take.written>=take.window+take.frames){command(0x18,0);take.phase=4;}
}

/* Hand off on a render block after the recorder has consumed its first RX
 * packet. Skip recorder lead-in and the estimated 75-frame effects return. */
__attribute__((section(".capture_sync"),noinline))
unsigned genie_bounce_clock(void){
    if(take.phase==2 && take.written && U8(0x82e0e96au)==2){
        take.window=take.written+75;
        take.phase=3;
        return 1;
    }
    if(take.phase==3)return take.written>=take.window+take.frames?3:2;
    return 0;
}

/* Called after upstream RX processing, immediately before recorder ingestion.
 * Ramp both stereo channels to zero at the capture boundaries. */
__attribute__((section(".capture_fade"),noinline))
unsigned genie_bounce_feed(volatile int16_t *packet,unsigned bytes){
    unsigned phase=take.phase;
    if(bytes==0x800 && U8(0x82e0e96au)==2 && phase>=2 && phase<=4){
        unsigned pos=take.written;
        unsigned start=take.window,end=start+take.frames;
        if(phase>=3){
            for(unsigned i=0;i<64;++i){
                unsigned frame=pos+i;
                if(frame<start || frame>=end)continue;
                unsigned head=frame-start,tail=end-1-frame;
                /* Ease in over 256 frames to soften pad starts. Keep
                 * the existing short fade at the far side of the seam. */
                unsigned gain=256;
                if(head<256)gain=(head*head*(768-2*head))>>16;
                if(tail<64 && tail*4<gain)gain=tail*4;
                if(gain<256){
                    packet[16*i]=(int16_t)(((int32_t)packet[16*i]*(int)gain)/256);
                    packet[16*i+1]=(int16_t)(((int32_t)packet[16*i+1]*(int)gain)/256);
                }
            }
        }
        pos+=64;
        take.written=pos;
    }
    return ((unsigned (*)(volatile int16_t *,unsigned))0x1ca81u)(packet,bytes);
}
