/* SP-ETC v9.53 Sound Generator side screen. All callbacks run on the UI task. */
#include "sequence_perform.h"
#include <stdint.h>
typedef int (*pad_handler)(void *, int, int);
typedef void (*page_handler)(void *);
#define SG_PAGE (*(volatile int16_t *)0x80245880u == 0x1e)
#define OLD_PRESS ((pad_handler)0x83fd4001u)
#define OLD_RELEASE ((pad_handler)0x83fd4079u)
#define OLD_POLL ((page_handler)0x83fe76e3u)
#define OLD_FOOT ((page_handler)0x83fe7669u)
#define FRAMES (*(volatile uint32_t *)0x8059226cu * 2u)
#define SHIFT (((volatile uint8_t *)0x80592448u)[5] & 4u)
#define VOICE_START ((void (*)(volatile uint8_t *,int))0x800d0669u)
#define VOICE_RESET ((void (*)(volatile uint8_t *))0x80132341u)
#define ORIGINAL_ARP_TICK ((void (*)(int))0x83fa2241u)
#define INVALIDATE ((page_handler)0x800d0019u)
#define COLOR ((void (*)(void *,unsigned,unsigned))0x800d8019u)
#define FILL ((void (*)(void *,const int *))0x800ce581u)
extern void genie_setup(uint8_t *,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,const unsigned *);
extern void genie_roll(uint8_t *,const sp_sequence *,unsigned,unsigned,int,unsigned,unsigned);
extern unsigned genie_bounce_active(void);
extern void genie_bounce_start(unsigned,unsigned,unsigned,unsigned);
extern void genie_bounce_poll(unsigned,unsigned,unsigned);
extern unsigned genie_bounce_clock(void);
#ifdef GENIE_DOOM_074
extern unsigned genie_doom_start(unsigned, unsigned);
extern void genie_doom_release(void);
extern void genie_doom_select(int);
#endif
static struct {
    sp_sequence seq;
    uint8_t pixels[1024];
    uint32_t seed, opened, consumed;
    volatile uint32_t started;
    void *footer;
    void *host_page;
    uintptr_t footer_type;
    unsigned fixed_velocity,return_hint,capturing,painting,exit_down,clean_exit,octaves,length,rotation,lowvel,hivel,gate,view,roll,editing,knobdown,row,bpm,hits,root,scale,held;
    volatile unsigned queued,loop,next,playing,visible;
    unsigned reverse,skip_edge;
    volatile int head;
    int drawn;
} g;
typedef struct {
    sp_sequence seq;
    unsigned fixed_velocity,octaves,seed,length,rotation,lowvel,hivel,gate,bpm,hits,root,scale;
} phrase_snapshot;
static phrase_snapshot previous __attribute__((section(".undo")));
static void remember(void){
    previous.fixed_velocity=g.fixed_velocity;previous.octaves=g.octaves;previous.seq=g.seq;previous.seed=g.seed;previous.length=g.length;
    previous.rotation=g.rotation;previous.lowvel=g.lowvel;previous.hivel=g.hivel;
    previous.gate=g.gate;previous.bpm=g.bpm;previous.hits=g.hits;
    previous.root=g.root;previous.scale=g.scale;
}
static void undo(void){
    if(!previous.length)return;
    g.fixed_velocity=previous.fixed_velocity;g.octaves=previous.octaves;g.seq=previous.seq;g.seed=previous.seed;g.length=previous.length;
    g.rotation=previous.rotation;g.lowvel=previous.lowvel;g.hivel=previous.hivel;
    g.gate=previous.gate;g.bpm=previous.bpm;g.hits=previous.hits;
    g.root=previous.root;g.scale=previous.scale;g.view=0;previous.length=0;
}
static const uint16_t scales[]={
#define SCALE(name, mask) mask,
#include "genie_scales.inc"
#undef SCALE
};
enum { SCALE_COUNT = sizeof scales / sizeof scales[0] };
static void stop(void) {
    g.playing=0;
    if(g.head>=0)g.view=(unsigned)g.head/16;
    g.queued=0;g.head=-1;
}
static void generate(void) {
    unsigned low=g.fixed_velocity?100:g.lowvel;
    unsigned high=g.fixed_velocity?100:g.hivel;
    sp_seq_generate(&g.seq,0,g.length,g.hits,g.rotation,g.root,g.octaves,scales[g.scale],low,high,g.seed);
}
static void redraw(void) {
    if(g.footer && SG_PAGE && *(uintptr_t *)g.footer==g.footer_type) INVALIDATE(g.footer);
}
static void clear_page(void) {
    stop();g.visible=0;
    g.clean_exit=g.footer!=0;
    g.return_hint=SG_PAGE;
}
#ifdef GENIE_DOOM_074
__attribute__((section(".doom_open"), noinline))
#endif
static void toggle_genie(void) {
    if(g.visible) clear_page();
    else {
        if(!g.octaves){g.octaves=1;g.scale=2;g.length=16;g.lowvel=72;g.hivel=112;g.gate=75;g.seed=404;g.bpm=120;g.root=60;g.hits=5;}
        g.clean_exit=0;g.roll=g.editing=0;g.visible=1;g.opened=FRAMES;g.drawn=-100;g.head=-1;
        generate();redraw();
    }
}
__attribute__((section(".controls"))) int genie_press(void *page,int pad,int vel) {
    if(pad==0x13 && SHIFT) {
        if(genie_bounce_active()){genie_bounce_poll(FRAMES,1,0);g.held=1;return 0;}
        toggle_genie();
        g.held=1;return 0;
    }
    if(!g.visible || (unsigned)pad>=16) return OLD_PRESS(page,pad,vel);
    g.consumed|=1u<<pad;
    if(genie_bounce_active())return 0;
#ifdef GENIE_DOOM_074
    if (SHIFT && (pad == 14 || pad == 15)) {
        stop();
        genie_doom_select(pad == 14 ? -1 : 1);
        return 0;
    }
#endif
    switch(pad){
    case 0:g.row=(g.row+13)%14;break;
    case 1:g.row=(g.row+1)%14;break;
    case 2:case 3:{
        int delta=pad==2?-1:1;remember();
        stop();
        switch(g.row){
        case 0:
            if(delta<0){g.seed=g.seed*1664525u+1013904223u;generate();}
            else sp_seq_mutate(&g.seq,++g.seed);
            break;
        case 1:g.scale=(g.scale+SCALE_COUNT+delta)%SCALE_COUNT;generate();break;
        case 2:g.root=60+(g.root-60+12+delta)%12;generate();break;
        case 3:if((delta<0 && g.hits>1)||(delta>0 && g.hits<g.length))g.hits+=delta;
               generate();break;
        case 4:if((delta<0 && g.bpm>40)||(delta>0 && g.bpm<240))g.bpm+=delta;break;
        case 5:
            if((delta<0 && g.length>16)||(delta>0 && g.length<128)) g.length+=16*delta;
            if(g.hits>g.length)g.hits=g.length;
            g.rotation%=g.length;g.view=0;generate();break;
        case 6:g.rotation=(g.rotation+g.length+delta)%g.length;generate();break;
        case 7:if((delta<0 && g.lowvel>1)||(delta>0 && g.lowvel<g.hivel))g.lowvel+=delta;
               generate();break;
        case 8:if((delta<0 && g.hivel>g.lowvel)||(delta>0 && g.hivel<127))g.hivel+=delta;
               generate();break;
        case 9:if((delta<0 && g.gate>10)||(delta>0 && g.gate<95))g.gate+=5*delta;break;
        case 10:
            g.seed=g.seed*1664525u+1013904223u;
            for(unsigned i=0;i<g.length;i++){
                g.seed=g.seed*1664525u+1013904223u;
                unsigned n=(g.seed>>16)%12;
                while(!(scales[g.scale]&(1u<<n)))n=(n+1)%12;
                g.seq.step[i].note=g.root+n+12*((g.seed>>8)%g.octaves);
            }break;
        case 11:
            g.seed=g.seed*1664525u+1013904223u;g.rotation=g.seed%g.length;
            for(unsigned i=0;i<g.length;i++)
                g.seq.step[i].hit=((i+g.length-g.rotation)*g.hits)%g.length<g.hits;
            break;
        case 12:if((delta<0 && g.octaves>1)||(delta>0 && g.octaves<4))g.octaves+=delta;
            generate();break;
        case 13:g.fixed_velocity=delta<0;generate();break;
        }
        break;
    }
    case 10:
        if(!g.playing){
            unsigned steps=g.loop==3?2*g.length-2:g.length;
            genie_bounce_start(FRAMES,(steps*720000u+g.bpm/2)/g.bpm,g.loop,g.bpm);
        }
        break;
    case 11:if(g.playing)g.queued^=1;break;
    case 12:stop();undo();break;
    case 13:g.loop=(g.loop+1)&3;break;
    case 14:g.roll^=1;g.editing=0;break;
    case 15:
        if(g.playing)stop();
        else {g.started=FRAMES;g.next=0;g.reverse=g.loop==2;g.skip_edge=0;__asm__ volatile("dmb" ::: "memory");g.playing=1;}
        break;
    }
    g.drawn=-100;redraw();return 0;
}
int genie_release(void *page,int pad,int vel) {
    if(pad==0x13 && g.held){g.held=0;return 0;}
    if((unsigned)pad<16 && (g.consumed&(1u<<pad))){g.consumed&=~(1u<<pad);return 0;}
    return OLD_RELEASE(page,pad,vel);
}
/* The parent also uses this shared popup buffer for warnings. Only dismiss a
 * bare numeric percentage while playing, never a named or multiline alert. */
static void dismiss_percentage(void) {
    if(!*(volatile uint32_t *)0x805922b0u || *(volatile int32_t *)0x802458a0u>=0)return;
    volatile const char *s=(volatile const char *)0x802468d8u;
    unsigned i=0,value=0;
    while(i<3 && s[i]>='0' && s[i]<='9'){value=value*10+s[i]-'0';++i;}
    if(i && value<=100 && s[i]=='%' && !s[i+1])((void (*)(void))0x8001f731u)();
}
/* Clear the overlay before the compositor draws the restored stock widgets.
 * Clearing in the footer callback erased stock widgets drawn earlier. */
__attribute__((section(".roll"),noinline))
static void restore_sound_generator(void *page){
    if(g.host_page!=page || !SG_PAGE){
        g.host_page=SG_PAGE?page:0;
        g.footer=0;g.clean_exit=0;g.return_hint=0;
        return;
    }
    if(!g.clean_exit)return;
    if(g.footer && *(uintptr_t *)g.footer==g.footer_type){
        void *fb=*(void **)((uint8_t *)g.footer+0x84);
        if(fb){
            const int bounds[]={0,0,127,63};
            COLOR(fb,0xffffff,0);
            FILL(fb,bounds);
        }
        INVALIDATE(g.footer);
    }
    /* The live page owns six widgets, including the small bottom-right
     * status tile at +0x1a80. Repaint it along with the stock parameter bar. */
    void *const *children=(void *const *)((uint8_t *)page+0x1a6c);
    for(unsigned i=0;i<6;++i)if(children[i])INVALIDATE(children[i]);
    g.clean_exit=0;
}
void genie_poll(void *page) {
    restore_sound_generator(page);
    OLD_POLL(page);
    uint32_t now=FRAMES;
    genie_bounce_poll(now,!SG_PAGE || !g.visible,!g.playing);
    if(!g.visible)return;
    if(!SG_PAGE){clear_page();return;}
    dismiss_percentage();
    int frame=now-g.opened<48000?20:(g.roll?g.head:(int)g.playing);
    if(g.drawn!=frame){g.drawn=frame;redraw();}
}
/* Run on the stock arpeggiator's 64-frame render boundary. The voice start
 * follows the arpeggiator's idle-voice path and never waits on a busy voice. */
static unsigned active_voice __attribute__((section(".clock_state")));
#define VOICE_WORD(v,offset) (*(volatile uint32_t *)((v)+(offset)))
static volatile uint8_t *voice_at(unsigned slot){
    return (volatile uint8_t *)(slot ? 0x8353a310u+(slot-1)*0x1b0 : 0x80249800u);
}
__attribute__((section(".voice"),noinline))
static void release_voice(void){
#ifdef GENIE_DOOM_074
    genie_doom_release();
#endif
    if(!active_voice)return;
    volatile uint8_t *v=voice_at(active_voice-1);
    if(v[0] && !v[1] && v[0x194]==0xff && VOICE_WORD(v,0x1ac)==UINT32_MAX)
        VOICE_START(v,0);
    active_voice=0;
}
__attribute__((section(".voice"),noinline))
static void start_voice(unsigned note,unsigned velocity){
#ifdef GENIE_DOOM_074
    if (genie_doom_start(note, velocity)) return;
#endif
    if(note>108)note-=24;
    else if(note>96)note-=12;
    for(unsigned slot=0;slot<4;++slot){
        volatile uint8_t *v=voice_at(slot);
        if(v[0] || ((volatile uint8_t *)0x83fa0800u)[slot])continue;
        if(slot){
            volatile uint8_t *src=voice_at(0);
            for(unsigned i=4;i<0x1b0;i+=4)VOICE_WORD(v,i)=VOICE_WORD(src,i);
            VOICE_WORD(v,0)=VOICE_WORD(src,0)&0xffff0000u;
        }
        v[0x70]=v[0x71];
        VOICE_RESET(v);
        VOICE_WORD(v,0x5c)=note-48;
        VOICE_WORD(v,0x74)=VOICE_WORD(v,0x198)=velocity*100;
        VOICE_WORD(v,0x19c)=note-48;
        VOICE_WORD(v,0x1a0)=1000;
        VOICE_WORD(v,0x1ac)=UINT32_MAX;
        v[0x194]=0xff;
        VOICE_START(v,1);
        active_voice=slot+1;
        return;
    }
}
__attribute__((section(".clock"),used))
void genie_audio_tick(int frames){
    ORIGINAL_ARP_TICK(frames);
    if(!g.visible || !SG_PAGE){release_voice();return;}
    unsigned capture=genie_bounce_clock();
    if(capture==1){g.started=FRAMES;g.next=0;g.head=-1;g.reverse=g.loop==2;g.skip_edge=0;g.capturing=1;g.playing=1;}
    if(capture==3 || (g.capturing && !capture)){
        g.capturing=0;stop();release_voice();return;
    }
    if(!g.playing){release_voice();return;}
    uint32_t elapsed=FRAMES-g.started;
    unsigned step=elapsed*g.bpm/720000u;
    unsigned capture_ping=capture && g.loop==3;
    unsigned span=capture_ping?2*g.length-2:g.length-g.skip_edge;
    if(step>=span){
        release_voice();
        if(capture || !(g.loop || g.queued)){stop();return;}
        unsigned queued=g.queued;
        if(queued){remember();g.seed=g.seed*1664525u+1013904223u;generate();g.queued=0;}
        g.started+=(span*720000u+g.bpm/2)/g.bpm;
        g.skip_edge=g.loop==3 && !queued;
        g.reverse=g.skip_edge?!g.reverse:g.loop==2;
        span=g.length-g.skip_edge;
        elapsed=FRAMES-g.started;
        step=elapsed*g.bpm/720000u;
        g.next=0;g.head=-1;
    }
    unsigned phase=elapsed*g.bpm-step*720000u;
    unsigned index;
    if(capture_ping)index=step<g.length?step:span-step;
    else index=g.reverse?span-1-step:step+g.skip_edge;
    if(index!=(unsigned)g.head || phase>=7200u*g.gate)release_voice();
    if(step<span && step>=g.next){
        g.head=(int)index;g.next=step+1;
        sp_seq_step ev=g.seq.step[index];
        if(ev.hit && phase<7200u*g.gate)start_voice(ev.note,ev.velocity);
    }
}

__attribute__((section(".clock"))) void genie_column(void *widget){
    if(!g.visible || !SG_PAGE)((page_handler)0x83fe7595u)(widget);
}
__attribute__((section(".clock"))) void genie_status_tile(void *widget){
    if(!g.visible || !SG_PAGE)((page_handler)0x80140291u)(widget);
}
__attribute__((section(".roll"))) void genie_parameter_background(void *widget){
    if(!g.visible || !SG_PAGE)((page_handler)0x801239e9u)(widget);
}
/* Re-enter the stock PRM renderer after its displaced prologue. */
__attribute__((naked,used)) void genie_bar_stock(void *widget){
    __asm__ volatile(
        "push.w {r4-r10,lr}\n"
        "sub sp,#0x78\n"
        "mov r4,r0\n"
        "ldr.w r0,[r0,#0x84]\n"
        "movw ip,#0x3abd\n"
        "movt ip,#0x8012\n"
        "bx ip\n");
}
__attribute__((section(".roll"),used)) void genie_parameter_bar(void *widget){
    if(SG_PAGE && g.visible)return;
    genie_bar_stock(widget);
    if(SG_PAGE && g.return_hint){
        void *fb=*(void **)((uint8_t *)widget+0x84);
        if(fb){
            const int badge[]={0,43,55,51};
            COLOR(fb,0,0xffffff);
            FILL(fb,badge);
            ((void (*)(void *,unsigned))0x800d80d9u)(fb,0x1d);
            ((void (*)(void *,int,int,const char *))0x800d4fa1u)(fb,28,45,"SHIFT: OTHERS");
            COLOR(fb,0xffffff,0);
        }
    }
}
/* The stock compositor can paint the parameter strip independently of its
 * widget callbacks. Protect the lower-right strip at the rectangle calls. */
__attribute__((used,noinline,section(".roll")))
unsigned genie_strip_guard(void *context,const int *box){
    if(!g.visible || !SG_PAGE || g.painting || !g.footer || !context)return 0;
    const uint8_t *ours=*(const uint8_t **)((uint8_t *)g.footer+0x84);
    if(!ours || *(const void *const *)ours!=*(const void *const *)context)return 0;
    const int *origin=(const int *)((const uint8_t *)context+0x14);
    int left=box[0]+origin[0],top=box[1]+origin[1];
    int right=box[2]+origin[0],bottom=box[3]+origin[1];
    return left>=48 && right>=left && right<128 && top>=45 && bottom>=top && bottom<64;
}
#define STRIP_WRAPPER(name, resume) \
__attribute__((naked,used,section(".roll"))) void name(void){ \
    __asm__ volatile( \
        "push {r0-r3,r4,lr}\nbl genie_strip_guard\ncmp r0,#0\n" \
        "pop {r0-r3,r4,lr}\nbne 1f\n" \
        "push {r4,r5,r7,lr}\nsub sp,#16\nmov r4,r0\nmov r5,sp\n" \
        "ldr r0,[r1,#12]\nstr r0,[sp,#12]\n" \
        "movw ip,#" resume "\nmovt ip,#0x800c\nbx ip\n1: bx lr\n"); \
}
STRIP_WRAPPER(genie_rect_outline,"0xe51d")
STRIP_WRAPPER(genie_rect_pattern,"0xe555")
STRIP_WRAPPER(genie_rect_fill,"0xe58d")

void genie_footer(void *widget){
    if(!SG_PAGE || !g.visible){
        OLD_FOOT(widget);
        if(!SG_PAGE)return;
    }
    g.footer=widget;g.footer_type=*(uintptr_t *)widget;
    if(!g.visible)return;
    void *fb=*(void **)((uint8_t *)widget+0x84);
    if(!fb)return;
    uint32_t age=FRAMES-g.opened;
    if(g.roll && age>=48000)genie_roll(g.pixels,&g.seq,g.root,g.octaves,g.head,g.head>=0?(unsigned)g.head/16:g.view,g.loop);
    else {unsigned extra[]={g.length,g.rotation,g.lowvel,g.hivel,g.gate,g.octaves,g.fixed_velocity};genie_setup(g.pixels,age<48000,g.row,g.bpm,g.hits,g.scale,g.root,g.playing|(g.loop<<1)|(g.queued<<3),g.editing,extra);}
    g.painting=1;
    int box[4]={0,0,127,63};
    COLOR(fb,0xffffff,0);FILL(fb,box);
    COLOR(fb,0,0xffffff);
    for(int y=0;y<64;y++)for(int x=0;x<128;){
        if(!(g.pixels[y*16+x/8]&(128u>>(x%8)))){++x;continue;}
        int begin=x++;
        while(x<128 && (g.pixels[y*16+x/8]&(128u>>(x%8))))++x;
        box[0]=begin;box[1]=box[3]=y;box[2]=x-1;FILL(fb,box);
    }
    COLOR(fb,0xffffff,0);
    g.painting=0;
}

int genie_encoder(void *page,int id,int delta){
    if(!g.visible || id!=0)return ((pad_handler)0x8015c531u)(page,id,delta);
    if(delta && g.roll && !g.playing){g.view=(g.view+g.length/16+(delta>0?1:-1))%(g.length/16);redraw();}
    if(delta && !g.roll){
        unsigned consumed=g.consumed;
        genie_press(page,(g.editing?2:0)+(delta>0),0);
        g.consumed=consumed;
    }
    return 0;
}
typedef int (*key_handler)(void *,int);
__attribute__((section(".roll"))) int genie_key(void *page,int key){
    if(key==0x22 && (g.visible || g.exit_down)){
        if(!g.exit_down){
            if(genie_bounce_active())genie_bounce_poll(FRAMES,1,0);
            else clear_page();
        }
        g.exit_down=1;
        return 1;
    }
    if(g.visible && genie_bounce_active())return 1;
    if(!g.visible || key!=0x31)return ((key_handler)0x8015c551u)(page,key);
    if(!g.knobdown){
        if(g.roll)g.roll=0;
        g.editing^=1;g.drawn=-100;redraw();
    }
    g.knobdown=1;return 1;
}
int genie_keyup(void *page,int key){
    if(key==0x22 && g.exit_down){g.exit_down=0;return 1;}
    if(key==0x31 && g.knobdown){g.knobdown=0;return 1;}
    return ((key_handler)0x8015bf79u)(page,key);
}
