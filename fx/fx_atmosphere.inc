/* Shared arena layout. Valid history starts empty on each effect switch. */
typedef struct {
    float history[2][32768];
    float filter[2][6], band_smooth[2][6], imprint[6], envelope, gap;
    unsigned write, valid, reverse, remaining, span;
} atmosphere_t;

_Static_assert(sizeof(atmosphere_t) <= FX_BUS_ARENA_SIZE, "MFX arena overflow");

static float atmosphere_abs(float x) { return x < 0 ? -x : x; }

static float atmosphere_read(const atmosphere_t *s, unsigned ch, unsigned delay) {
    return delay <= s->valid ? s->history[ch][(s->write-delay)&32767u] : 0;
}

static void atmosphere_process(unsigned fx, const fx_bus_ctrl_t *ctrl,
                               atmosphere_t *s, float *left, float *right) {
    float p[6];
    for (unsigned k=0;k<6;k++) p[k]=ctrl->smooth[k]*0.01f;
    unsigned inner=181+(unsigned)(p[0]*1100), outer=1327+(unsigned)(p[1]*4400);
    for (unsigned i=0;i<FX_BLOCK_FRAMES;i++) {
        float in[2]={left[i],right[i]}, wet[2]={0,0}, feed[2]={in[0],in[1]};
        float level=(atmosphere_abs(in[0])+atmosphere_abs(in[1]))*0.5f;
        s->envelope+=(level-s->envelope)*(level>s->envelope?0.08f:0.001f);
        if (fx==FX_GHOST_PRINT) {
            float mono=(atmosphere_abs(in[0])+atmosphere_abs(in[1]))*0.5f;
            for (unsigned b=0;b<6;b++) {
                float base=0.006f*(1u<<b);
                float coeff=base*(0.5f+p[3]*1.5f), energy=0;
                for (unsigned ch=0;ch<2;ch++) {
                    float band=in[ch]-s->filter[ch][b];
                    s->filter[ch][b]+=coeff*band;
                    s->band_smooth[ch][b]+=coeff*(band-s->band_smooth[ch][b]);
                    band=s->band_smooth[ch][b];
                    wet[ch]+=p4_tanh(band*2)*s->imprint[b];
                    energy+=atmosphere_abs(band);
                }
                float memory=s->imprint[b];
                float neighbor=s->imprint[b==5?4:b+1];
                s->imprint[b]=memory*(0.99975f+p[1]*0.00024f)
                    +p[2]*0.0001f*(neighbor-memory);
                if (mono>0.004f+p[4]*0.24f)
                    s->imprint[b]+=p[0]*0.0008f*(p4_tanh(energy*3)-memory);
            }
        } else if (fx==FX_ROOM_IN_ROOM) {
            for (unsigned ch=0;ch<2;ch++) {
                float a=atmosphere_read(s,ch,inner+ch*53);
                float b=atmosphere_read(s,ch^1u,outer+ch*193);
                float reflections=a*0.5f+b*0.32f-atmosphere_read(s,ch,outer/2)*0.18f;
                s->filter[ch][0]+=(0.65f-p[4]*0.60f)*(reflections-s->filter[ch][0]);
                wet[ch]=s->filter[ch][0];
                feed[ch]=p4_tanh(in[ch]*0.55f+(0.2f+0.72f*p[3])*
                                 (a*(1-p[2])+wet[ch]*p[2]));
            }
        } else if (fx==FX_UNDERTOW) {
            float threshold=0.008f+0.13f*(1-p[0]);
            if (s->envelope>threshold) {
                s->remaining=0;
                s->gap*=1.0f-(0.003f+0.15f*p[4]);
            } else {
                s->gap+=(1-s->gap)*(0.0015f-0.00145f*p[1]);
                if (s->span==0 && s->valid>1024) {
                    s->span=1024+(unsigned)(p[2]*13000);
                    if (s->span>s->valid-512) s->span=s->valid-512;
                    s->remaining=s->span;
                    s->reverse=(s->write-512)&32767u;
                }
                if (s->remaining) {
                    float fade=(float)s->remaining/(float)s->span;
                    for (unsigned ch=0;ch<2;ch++) {
                        float x=s->history[ch][s->reverse];
                        s->filter[ch][0]+=(0.48f-0.44f*p[3])*(x-s->filter[ch][0]);
                        wet[ch]=s->filter[ch][0]*s->gap*fade;
                    }
                    s->reverse=(s->reverse-1)&32767u;
                    --s->remaining;
                }
            }
            if (s->envelope>threshold) s->span=0;
        } else { /* Paper Machine: pressure excites a loose, damped mechanism. */
            unsigned size=19+(unsigned)(p[1]*168);
            for (unsigned ch=0;ch<2;ch++) {
                float tap=atmosphere_read(s,ch,size+ch*7);
                float rattle=atmosphere_read(s,ch,size*3+11);
                s->filter[ch][0]+=(0.12f-0.10f*p[0])*(in[ch]-s->filter[ch][0]);
                float edge=in[ch]-s->filter[ch][0];
                float pressure=p4_tanh(edge*(1+7*p[3]));
                float contact=p4_tanh((pressure-tap)*4)*p[2]*p4_tanh(s->envelope*9);
                feed[ch]=p4_tanh(pressure*0.5f+tap*(0.72f-0.5f*p[4])+contact*0.3f);
                wet[ch]=p4_tanh(pressure*0.35f+tap*0.45f-rattle*0.3f+contact*0.4f);
            }
        }
        for (unsigned ch=0;ch<2;ch++) s->history[ch][s->write]=feed[ch];
        s->write=(s->write+1)&32767u;
        if (s->valid<32767u) ++s->valid;
        left[i]=in[0]*(1-p[5])+wet[0]*p[5];
        right[i]=in[1]*(1-p[5])+wet[1]*p[5];
    }
}
