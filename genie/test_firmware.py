from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'synth'))
from test_sg_subpad import Machine,KEY_SHIFT,SUB_PAD
from unicorn import UC_HOOK_CODE
from unicorn import arm_const as ar
from build_genie import ROOT, COPY, scatter, SLOTS
import struct
p=ROOT/'build/SP404MKII_APP1.bin'
image = p.read_bytes()
slots = {start for start, _ in SLOTS.values()}
for src, dest, size, kind in scatter(image):
    if dest in slots:
        assert kind == COPY and (src | dest | size) % 4 == 0, (hex(dest), size)
m=Machine(image);print('constructors and Sound Generator page OK',flush=True)
def word(a):return int.from_bytes(m.uc.mem_read(a,4),'little')
def observe(fn,cb):return m.uc.hook_add(UC_HOOK_CODE,cb,begin=fn&~1,end=fn&~1)
pages=[];notes=[]
observe(word(0x802296a4),lambda uc,*_:pages.append(uc.reg_read(ar.UC_ARM_REG_R0)))
def voice(uc,*_):
    notes.append((uc.reg_read(ar.UC_ARM_REG_R1),uc.reg_read(ar.UC_ARM_REG_R2)))
    m._ret()
observe(0x83fa3449,voice)
# Record which objects receive the real widget invalidator during entry.
invalidations=[]
invalidator=observe(0x800d0019,lambda uc,*_:invalidations.append(uc.reg_read(ar.UC_ARM_REG_R0)))
m.key(KEY_SHIFT,True);m.pad(SUB_PAD);m.pad(SUB_PAD,0);m.key(KEY_SHIFT,False)
assert pages
page=pages[-1]
assert page not in invalidations, 'widget invalidator was called with the Sound Generator page'
m.uc.hook_del(invalidator)
# Drive the installed draw pointer using a framebuffer stand-in. The stock
# footer and display primitives are boundaries; check the visible pixel runs.
rects=[]
observe(0x83fe7669,lambda uc,*_:m._ret())
observe(0x800d8019,lambda uc,*_:m._ret())
def rectangle(uc,*_):
    rects.append(struct.unpack('<4i',uc.mem_read(uc.reg_read(ar.UC_ARM_REG_R1),16)))
    m._ret()
observe(0x800ce581,rectangle)
bar_draws=[]
def bar_continuation(uc,*_):
    sp=uc.reg_read(ar.UC_ARM_REG_SP)
    # Unwind exactly as the real renderer does, after inspecting its saved LR.
    saved_lr=word(sp+0x78+7*4)
    assert saved_lr & 1
    bar_draws.append(1)
    uc.reg_write(ar.UC_ARM_REG_SP,sp+0x78+32)
    uc.reg_write(ar.UC_ARM_REG_PC,saved_lr)
observe(0x80123abd,bar_continuation)
column_draws=[]
observe(0x83fe7595,lambda uc,*_: (column_draws.append(1),m._ret()))
widget=0x30088000
m.w32(widget,0x80227000);m.w32(widget+0x84,0x30089000)
m.call(word(0x802256fc),widget)
assert rects[0]==(0,0,127,63) and len(rects)>50
assert all(0<=x0<=x1<128 and 0<=y0<=y1<64 for x0,y0,x1,y1 in rects)
intro=rects.copy()
m.call(word(0x8022bbd4),widget)
m.call(word(0x80222130),widget)
m.call(0x80123ab1,widget)
assert not bar_draws, 'stock PRM bar drew over Genie intro'
assert not column_draws, 'stock column drew over Genie intro' 
# Only the widget invalidation is stubbed for this synthetic framebuffer.
observe(0x800d0019,lambda uc,*_:m._ret())
m.pad(15);m.pad(15,0)
closed=[]
def close_popup(uc,*_):
    closed.append(1)
    m.w32(0x805922b0,0)
    m._ret()
observe(0x8001f731,close_popup)
for msg, count in [(b'50%',1),(b'CPU 50%',1),(b'50% WARNING',1),(b'105%',1)]:
    m.uc.mem_write(0x802468d8,msg+b'\x00')
    m.w32(0x805922b0,1);m.w32(0x802458a0,0xffffffff)
    m.call(word(0x80229678),page)
    assert len(closed)==count,(msg,closed)
m.w32(0x805922b0,0)
for frame in range(0,97000,768):
    m.w32(0x8059226c,frame//2);m.call(word(0x80229678),page)
rects.clear();m.call(word(0x802256fc),widget)
assert len(rects)>40 and rects!=intro
# Selection and edits must change what the physical framebuffer receives.
def picture():
    rects.clear();m.call(word(0x802256fc),widget);return tuple(rects)
before=picture()
m.pad(1);m.pad(1,0)
selected=picture()
m.pad(3);m.pad(3,0)
edited=picture()
assert before!=selected and selected!=edited
print('Intro and performance views reached the display API; synth calls:',notes,flush=True)
assert len(notes)==10 and all(60<=n<72 for n,v in notes)
assert all(notes[i][1]>0 and notes[i+1]==(notes[i][0],0) for i in range(0,10,2))
# We are on Scale. Browse with the actual encoder event, push to edit length.
for _ in range(4):m.dispatch(7,0,1)
at_length=picture()
m.key(0x31,True);m.key(0x31,False)
assert picture()!=at_length,'VALUE push did not enter edit'
for _ in range(7):m.dispatch(7,0,1)
m.key(0x31,True);m.key(0x31,False)
assert picture()!=at_length,'encoder did not change length'
start=97000
m.w32(0x8059226c,start//2)
m.pad(15);m.pad(15,0)
for step in range(128):
    for phase in (0,1800,4500):
        frame=start+6000*step+phase
        m.w32(0x8059226c,frame//2);m.call(word(0x80229678),page)
        if step==3 and phase==0:m.pad(14);m.pad(14,0)
        if step==127 and phase==0:
            assert (117,59,122,59) in picture(),'playhead did not reach step 128'
assert len(notes)==20,'changing view interrupted or restarted the eight-bar phrase'
m.pad(14);m.pad(14,0)
# Queue from the final bar, with looping off. Playback continues for exactly one phrase.
def tap(index):
    m.pad(index);m.pad(index,0)
def tick(frame):
    m.w32(0x8059226c,frame//2);m.call(word(0x80229678),page)
unarmed=picture()
tap(11);assert picture()!=unarmed
tap(11);assert picture()==unarmed
tap(11)
for step in range(128,257):
    tick(start+6000*step)
    tick(start+6000*step+4500)
assert len(notes)==30
assert notes[10:20]!=notes[20:30]
# Turn up Octaves to four via the actual encoder. Verify generated synth pitches.
for _ in range(7):m.dispatch(7,0,1)
one_octave=picture()
tap(3);assert picture()!=one_octave
tap(12);assert picture()==one_octave
for _ in range(3):tap(3)
tap(15)
offset=start+6000*256+4500
for step in range(128):
    tick(offset+6000*step)
    tick(offset+6000*step+4500)
pitches=[n for n,v in notes[30:] if v]
assert len(pitches)==5 and max(pitches)>=72 and all(60<=n<108 for n in pitches),pitches
print('Verified queue, cancellation, one-shot boundary, octave editing and undo',pitches)
m.key(KEY_SHIFT,True);m.pad(SUB_PAD);m.pad(SUB_PAD,0);m.key(KEY_SHIFT,False)
assert m.pad(0), 'stock Sound Generator input did not resume'
m.call(word(0x8022bbd4),widget)
m.call(word(0x80222130),widget)
assert bar_draws == [1], 'stock PRM bar did not resume'
assert column_draws == [1], 'stock column did not resume'
print('Genie closed and stock pad dispatch resumed')

