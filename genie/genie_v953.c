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
#define SYNTH ((void (*)(unsigned,int,int,int))0x83fa3449u)
#define INVALIDATE ((page_handler)0x800d0019u)
#define COLOR ((void (*)(void *,unsigned,unsigned))0x800d8019u)
#define FILL ((void (*)(void *,const int *))0x800ce581u)
extern void genie_setup(uint8_t *,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,const unsigned *);
extern void genie_roll(uint8_t *,const sp_sequence *,unsigned,unsigned,int,unsigned);
extern unsigned genie_bounce_active(void);
extern void genie_bounce_start(unsigned,unsigned);
extern unsigned genie_bounce_poll(unsigned,unsigned,unsigned);
static struct {
    sp_sequence seq;
    uint8_t pixels[1024];
    uint32_t seed, opened, last, started, consumed;
    void *footer;
    uintptr_t footer_type;
    unsigned queued,octaves,loop,length,rotation,lowvel,hivel,gate,view,roll, editing, knobdown, row, bpm, hits, root, scale, next, playing, visible, held;
    int note, head, drawn;
} g;
typedef struct {
    sp_sequence seq;
    unsigned octaves,seed,length,rotation,lowvel,hivel,gate,bpm,hits,root,scale;
} phrase_snapshot;
static phrase_snapshot previous __attribute__((section(".undo")));
static void remember(void){
    previous.octaves=g.octaves;previous.seq=g.seq;previous.seed=g.seed;previous.length=g.length;
    previous.rotation=g.rotation;previous.lowvel=g.lowvel;previous.hivel=g.hivel;
    previous.gate=g.gate;previous.bpm=g.bpm;previous.hits=g.hits;
    previous.root=g.root;previous.scale=g.scale;
}
static void undo(void){
    if(!previous.length)return;
    g.octaves=previous.octaves;g.seq=previous.seq;g.seed=previous.seed;g.length=previous.length;
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
    if(g.playing && g.note>=0) SYNTH(2,g.note,0,1);
    if(g.head>=0)g.view=(unsigned)g.head/16;g.queued=0;g.note=-1;g.playing=0;g.head=-1;
}
static void generate(void) {
    sp_seq_generate(&g.seq,0,g.length,g.hits,g.rotation,g.root,g.octaves,scales[g.scale],g.lowvel,g.hivel,g.seed);
}
static void redraw(void) {
    if(g.footer && SG_PAGE && *(uintptr_t *)g.footer==g.footer_type) INVALIDATE(g.footer);
}
static void clear_page(void) {
    stop();g.visible=0;redraw();g.footer=0;
}
__attribute__((section(".controls"))) int genie_press(void *page,int pad,int vel) {
    if(pad==0x13 && SHIFT) {
        if(genie_bounce_active()){genie_bounce_poll(FRAMES,1,0);g.held=1;return 0;}
        if(g.visible) clear_page();
        else {
            if(!g.octaves){g.octaves=1;g.scale=2;g.length=16;g.lowvel=72;g.hivel=112;g.gate=75;g.seed=404;g.bpm=120;g.root=60;g.hits=5;}
            g.roll=g.editing=0;g.visible=1;g.opened=g.last=FRAMES;g.drawn=-100;g.head=-1;g.note=-1;
            generate();redraw();
        }
        g.held=1;return 0;
    }
    if(!g.visible || (unsigned)pad>=16) return OLD_PRESS(page,pad,vel);
    g.consumed|=1u<<pad;
    if(genie_bounce_active())return 0;
    switch(pad){
    case 0:g.row=(g.row+12)%13;break;
    case 1:g.row=(g.row+1)%13;break;
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
        }
        break;
    }
    case 10:if(!g.playing)genie_bounce_start(FRAMES,(g.length*720000u+g.bpm/2)/g.bpm);break;
    case 11:if(g.playing)g.queued^=1;break;
    case 12:stop();undo();break;
    case 13:g.loop^=1;break;
    case 14:g.roll^=1;g.editing=0;break;
    case 15:
        if(g.playing)stop();
        else {g.playing=1;g.started=FRAMES;g.next=0;g.note=-1;}
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
void genie_poll(void *page) {
    OLD_POLL(page);
    uint32_t now=FRAMES;
    if(genie_bounce_poll(now,!SG_PAGE || !g.visible, !g.playing)) {
        g.playing=1;g.started=now;g.next=0;g.note=-1;
    }
    if(!g.visible)return;
    if(!SG_PAGE || (((volatile uint8_t *)0x80592448u)[4] & 4u) || now-g.last>24000){clear_page();return;}
    g.last=now;
    dismiss_percentage();
    if(g.playing){
        uint32_t elapsed=now-g.started;
        unsigned step=elapsed*g.bpm/720000u;
        if(step>=g.length && (g.loop || g.queued) && !genie_bounce_active()){
            if(g.note>=0){SYNTH(2,g.note,0,1);g.note=-1;}
            if(g.queued){remember();g.seed=g.seed*1664525u+1013904223u;generate();g.queued=0;g.drawn=-100;}
            g.started+=(g.length*720000u+g.bpm/2)/g.bpm;
            elapsed=now-g.started;step=elapsed*g.bpm/720000u;g.next=0;g.head=-1;
        }
        if(step>=g.length)stop();
        else {
            unsigned phase=elapsed*g.bpm-step*720000u;
            if(g.note>=0 && (step!= (unsigned)g.head || phase>=7200u*g.gate)){
                SYNTH(2,g.note,0,1);g.note=-1;
            }
            if(step>=g.next){
                g.head=(int)step;g.next=step+1;
                sp_seq_step e=g.seq.step[step];
                if(e.hit && phase<7200u*g.gate){g.note=e.note;SYNTH(2,e.note,e.velocity,0);}
            }
        }
    }
    int frame=now-g.opened<48000?20:(g.roll?g.head:(int)g.playing);
    if(g.drawn!=frame){g.drawn=frame;redraw();}
}
__attribute__((section(".controls"))) void genie_column(void *widget){
    if(!g.visible || !SG_PAGE)((page_handler)0x83fe7595u)(widget);
}
/* The parameter widget paints its background and content separately. */
__attribute__((section(".controls"))) void genie_parameter_background(void *widget){
    if(!g.visible || !SG_PAGE)((page_handler)0x801239e9u)(widget);
}
/* Entry detour covers callers that bypass the bar's class draw pointer. */
__attribute__((used,noinline)) unsigned genie_owns_display(void){
    return g.visible && SG_PAGE;
}
__attribute__((naked,section(".controls"))) void genie_parameter_bar(void *widget){
    __asm__ volatile(
        "push {r0-r3,r4,lr}\n"
        "bl genie_owns_display\n"
        "cmp r0,#0\n"
        "pop {r0-r3,r4,lr}\n"
        "bne 1f\n"
        ".global genie_bar_stock\n"
        "genie_bar_stock:\n"
        "push.w {r4-r10,lr}\n"
        "sub sp,#0x78\n"
        "mov r4,r0\n"
        "ldr.w r0,[r0,#0x84]\n"
        "movw ip,#0x3abd\n"
        "movt ip,#0x8012\n"
        "bx ip\n"
        "1: bx lr\n");
}
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
    if(g.roll && age>=48000)genie_roll(g.pixels,&g.seq,g.root,g.octaves,g.head,g.head>=0?(unsigned)g.head/16:g.view);
    else {unsigned extra[]={g.length,g.rotation,g.lowvel,g.hivel,g.gate,g.octaves};genie_setup(g.pixels,age<48000,g.row,g.bpm,g.hits,g.scale,g.root,g.playing|(g.loop<<1)|(g.queued<<2),g.editing,extra);}
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
int genie_key(void *page,int key){
    if(g.visible && genie_bounce_active())return 1;
    if(!g.visible || key!=0x31)return ((key_handler)0x8015c551u)(page,key);
    if(!g.knobdown){
        if(g.roll)g.roll=0;
        g.editing^=1;g.drawn=-100;redraw();
    }
    g.knobdown=1;return 1;
}
int genie_keyup(void *page,int key){
    if(key==0x31 && g.knobdown){g.knobdown=0;return 1;}
    return ((key_handler)0x8015bf79u)(page,key);
}
