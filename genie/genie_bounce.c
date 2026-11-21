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
    unsigned frames, written;
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
    take.frames=frames;take.written=0;
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

__attribute__((section(".capture_poll"),noinline))
unsigned genie_bounce_poll(unsigned now,unsigned cancel,unsigned finished){
    unsigned phase=take.phase;
    if(!phase)return 0;
    unsigned state=REC;
    unsigned age=now-take.since;
    if(phase==4 || phase==5){
        if(!state && TARGET==UINT32_MAX){
            setting(0x24,take.length);setting(0x59,take.snap);setting(0x25,take.ext);
            take.phase=0;
            if(phase==4 && occupied(take.pad)){command(0xd,1);saved_notice(take.pad);}
            else notice("GENIE BOUNCE ENDED");
        }
        return 0;
    }
    if(cancel || (phase<3 && age>96000) || (phase==3 && state!=2)){
        command(state==2?0x1a:0x18,0);take.phase=5;return 0;
    }
    if(phase==1 && state==1){command(0x17,0);take.phase=2;take.since=now;}
    if(phase==2 && state==2){take.phase=3;return 1;}
    if(phase==3 && finished && (take.written>=take.frames || age>take.frames+24000)){command(0x18,0);take.phase=4;}
    return 0;
}

/* Called after upstream RX processing, immediately before recorder ingestion.
 * Ramp both stereo channels to zero at the capture boundaries. */
__attribute__((section(".capture_fade"),noinline))
unsigned genie_bounce_feed(volatile int16_t *packet,unsigned bytes){
    unsigned phase=take.phase;
    if(bytes==0x800 && U8(0x82e0e96au)==2 && phase>=2 && phase<=4){
        unsigned pos=take.written;
        unsigned total=take.frames;
        for(unsigned i=0;i<64;++i,++pos){
            unsigned end=pos<total?total-1-pos:0;
            unsigned gain=pos<end?pos:end;
            if(gain<256){
                packet[16*i]=(int16_t)(((int32_t)packet[16*i]*(int)gain)/256);
                packet[16*i+1]=(int16_t)(((int32_t)packet[16*i+1]*(int)gain)/256);
            }
        }
        take.written=pos;
    }
    return ((unsigned (*)(volatile int16_t *,unsigned))0x1ca81u)(packet,bytes);
}
