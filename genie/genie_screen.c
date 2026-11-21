/* Monochrome 128x64 presentation for the Sound Generator companion and APP1.
 * Row-major pixels, MSB first. No SDK, heap or device addresses. */
#include <stdint.h>
#include "sequence_perform.h"

static void dot(uint8_t *p,int x,int y) {
    if ((unsigned)x<128 && (unsigned)y<64) p[y*16+x/8]|=(uint8_t)(128u>>(x%8));
}
static void rect(uint8_t *p,int x,int y,int w,int h) {
    for(int j=0;j<h;j++) for(int i=0;i<w;i++) dot(p,x+i,y+j);
}
static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-:/ ";
static const uint16_t glyph[]={
    0x2bed,0x6bae,0x3923,0x6b6e,0x79a7,0x79a4,0x397b,0x5bed,0x7497,
    0x124a,0x5bad,0x4927,0x5fed,0x5ffd,0x2b6a,0x6ba4,0x2b7b,0x6bad,
    0x388e,0x7492,0x5b6f,0x5b6a,0x5bfd,0x5aad,0x5a92,0x72a7,
    0x7b6f,0x3497,0x73e7,0x73cf,0x5bc9,0x79cf,0x79ef,0x7249,
    0x7bef,0x7bcf,0x01c0,0x0410,0x12a4,0
};
static void label(uint8_t *p,int x,int y,const char *s,int scale) {
    for(;*s;s++,x+=4*scale) {
        unsigned i=0;
        while(alphabet[i] && alphabet[i]!=*s)i++;
        uint16_t g=alphabet[i]?glyph[i]:0;
        for(int r=0;r<5;r++) for(int c=0;c<3;c++)
            if(g & (1u<<(14-r*3-c))) rect(p,x+c*scale,y+r*scale,scale,scale);
    }
}
static void number(uint8_t *p,int x,int y,unsigned n) {
    char v[4];unsigned len=n>=100?3:(n>=10?2:1);
    v[len]=0;
    while(len){v[--len]=(char)('0'+n%10);n/=10;}
    label(p,x,y,v,1);
}
static void spark(uint8_t *p,int x,int y,int r) {
    rect(p,x-r,y,2*r+1,1);rect(p,x,y-r,1,2*r+1);
}
void sp_genie_screen(uint8_t *p,const sp_sequence *s,unsigned bpm,int playhead,int intro) {
    for(unsigned i=0;i<1024;i++)p[i]=0;
    if(intro>=0) {
        label(p,5,7,"SOUND GENERATOR",1);
        label(p,5,22,"SEQUENCE",2);label(p,5,36,"GENIE",3);
        /* A wisp rises from the 404 pad matrix. */
        for(int row=0;row<4;row++)for(int col=0;col<4;col++)
            rect(p,83+col*8-row,41+row*4,6,3);
        for(int i=0;i<17;i++){
            int x=100+(i<6?i:(i<12?12-i:i-12));
            rect(p,x,22+i,1+i/6,1);
        }
        int blink=(intro/2)%3;
        spark(p,103,14,3+blink);spark(p,88,26,1+blink);spark(p,117,31,2);
        return;
    }
    label(p,3,3,"SG / SEQUENCE GENIE",1);rect(p,2,11,124,1);
    label(p,3,16,"TEMPO",1);number(p,26,16,bpm);
    label(p,62,16,"STEPS",1);number(p,86,16,s->length);
    unsigned page=playhead>=0?(unsigned)playhead/16:0;
    for(unsigned i=0;i<16;i++){
        unsigned k=16*page+i;
        int x=3+(int)i*7+(int)(i/4)*3;
        rect(p,x,37,5,1);
        if(k<s->length && s->step[k].hit)rect(p,x,27,5,9);
        if(playhead==(int)k)rect(p,x,40,5,2);
    }
    rect(p,2,46,124,1);label(p,3,53,playhead<0?"READY":"PLAY",1);
    label(p,48,53,"BAR",1);number(p,65,53,page+1);label(p,74,53,"/",1);number(p,80,53,(s->length+15)/16);
}
/* Hardware setup screen. Selection stays visible while the phrase is playing. */
static const uint8_t redux_title[]={
#include "genie_logo.inc"
};
void genie_setup(uint8_t *p,unsigned intro,unsigned row,unsigned bpm,unsigned hits,
                 unsigned scale,unsigned root,unsigned playing,unsigned editing,const unsigned *extra) {
    if(intro){
        unsigned i=0,j=0;
        while(i<sizeof redux_title){
            unsigned n=redux_title[i++];
            if(n&128){uint8_t value=redux_title[i++];for(n-=128;n;n--)p[j++]=value;}
            else while(n--)p[j++]=redux_title[i++];
        }
        return;
    }
    for(unsigned i=0;i<1024;i++)p[i]=0;
    static const char *const names[] __attribute__((section(".screen_strings")))={"PHRASE","SCALE","ROOT","HITS","TEMPO","LENGTH","ROTATE","VELOCITY MIN","VELOCITY MAX","GATE PCT","NOTES","RHYTHM","OCTAVES"};
    static const char *const keys[]={"C","DB","D","EB","E","F","GB","G","AB","A","BB","B"};
    static const char modes[][10] __attribute__((section(".screen_strings")))={
#define SCALE(name, mask) name,
#include "genie_scales.inc"
#undef SCALE
    };
    label(p,2,1,"SEQUENCE GENIE",1); label(p,99,1,playing&4?"QUEUED":editing?"EDIT":(playing&2?"LOOP":(playing?"PLAYING":"SELECT")),1);
    rect(p,0,8,128,1);
    for(unsigned i=0;i<5;i++){
        unsigned item=(row/5)*5+i;if(item>=13)break;
        int y=11+(int)i*8;label(p,9,y,names[item],1);
        switch(item){
        case 0:label(p,60,y,"NEW / MUTATE",1);break;
        case 1:label(p,60,y,modes[scale],1);break;
        case 2:label(p,60,y,keys[root%12],1);break;
        case 3:number(p,60,y,hits);break;
        case 12:number(p,60,y,extra[5]);break;
        case 4:number(p,60,y,bpm);break;
        default:if(item<10)number(p,60,y,extra[item-5]);else label(p,60,y,"REGENERATE",1);break;
        }
        if(item==row){rect(p,1,y,2,5);rect(p,3,y+1,1,3);dot(p,4,y+2);}
    }
    rect(p,0,51,128,1);label(p,1,56,"EDIT 15 ROLL 16 PLAY",1);
}

__attribute__((section(".roll"))) void genie_roll(uint8_t *p,const sp_sequence *seq,unsigned root,unsigned octaves,int head,unsigned page){
    for(unsigned i=0;i<1024;i++)p[i]=0;
    label(p,2,1,"PIANO ROLL",1);number(p,47,1,page+1);label(p,63,1,"15 SETUP 16 PLAY",1);
    unsigned lanes=12*octaves;
    for(unsigned lane=0;lane<lanes;lane++){
        int y=9+(lanes-1-lane)*41/lanes;
        unsigned pitch=(root+lane)%12;
        int black=(0x54au>>pitch)&1;
        rect(p,0,y,black?5:9,octaves==1?3:1);
        if(!pitch)for(int x=12;x<126;x+=3)dot(p,x,y+3);
    }
    for(unsigned step=0;step<16;step++){
        int x=12+7*step;
        if(step%4==0)for(int y=9;y<50;y+=3)dot(p,x,y);
        sp_seq_step ev=seq->step[16*page+step];
        if(ev.hit && ev.note>=root && ev.note-root<lanes)
            rect(p,x+1,9+(lanes-1-(ev.note-root))*41/lanes,5,octaves==1?3:1);
        if((int)(16*page+step)==head){rect(p,x,8,1,42);rect(p,x,7,6,1);}
    }
}
