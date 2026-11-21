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
pages=[];notes=[];note_frames=[]
observe(word(0x802296a4),lambda uc,*_:pages.append(uc.reg_read(ar.UC_ARM_REG_R0)))
def voice(uc,*_):
    ptr=uc.reg_read(ar.UC_ARM_REG_R0)
    on=uc.reg_read(ar.UC_ARM_REG_R1)
    note=(word(ptr+0x5c)+48)&0xffffffff
    notes.append((note,word(ptr+0x74)//100 if on else 0))
    if on:note_frames.append(word(0x8059226c)*2)
    uc.mem_write(ptr,bytes([int(bool(on)),0]))
    m._ret()
observe(0x800d0669,voice)
observe(0x80132341,lambda uc,*_:m._ret())
observe(0x83fa2241,lambda uc,*_:m._ret())
def audio_tick():
    m.call(0x83fa21b1,0,0,64,0)
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
fill_observer=observe(0x800ce581,rectangle)
background_draws=[]
observe(0x801239e9,lambda uc,*_: (background_draws.append(1),m._ret()))
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
# Exercise all three stock rectangle entry points with actual detour code.
m.uc.hook_del(fill_observer)
stock_rectangles=[]
def rectangle_return(uc,*_):
    stock_rectangles.append(1)
    sp=uc.reg_read(ar.UC_ARM_REG_SP)
    for index,reg in enumerate((ar.UC_ARM_REG_R4,ar.UC_ARM_REG_R5,ar.UC_ARM_REG_R7)):
        uc.reg_write(reg,word(sp+16+4*index))
    uc.reg_write(ar.UC_ARM_REG_SP,sp+32)
    uc.reg_write(ar.UC_ARM_REG_PC,word(sp+28))
for addr in (0x800ce51d,0x800ce555,0x800ce58d):
    observe(addr,rectangle_return)
box=0x3008b000
for addr in (0x800ce511,0x800ce549,0x800ce581):
    m.uc.mem_write(box,struct.pack('<4i',96,54,127,62))
    m.call(addr,0x30089000,box)
    assert not stock_rectangles
m.uc.mem_write(box,struct.pack('<4i',96,4,127,12))
m.call(0x800ce511,0x30089000,box)
assert stock_rectangles==[1]
fill_observer=observe(0x800ce581,rectangle)

m.call(word(0x8022bbd4),widget)
m.call(word(0x80222130),widget)
m.call(0x80123ab1,widget)
m.call(word(0x8022209c),widget)
assert not background_draws, 'stock bar background drew while Genie was open'
assert not bar_draws, 'stock PRM bar drew over Genie intro'
assert not column_draws, 'stock column drew over Genie intro'
# Only the widget invalidation is stubbed for this synthetic framebuffer.
stub_invalidate=observe(0x800d0019,lambda uc,*_:m._ret())
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
    m.w32(0x8059226c,frame//2);audio_tick();m.call(word(0x80229678),page)
rects.clear();m.call(word(0x802256fc),widget)
assert len(rects)>40 and rects!=intro
# Selection and edits must change what the physical framebuffer receives.
def picture():
    rects.clear();m.call(word(0x802256fc),widget);return tuple(rects)
before=picture()
assert all(x1 < 88 for x0,y0,x1,y1 in before[1:] if y0 >= 55), 'setup footer overlaps stock strip'
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
        m.w32(0x8059226c,frame//2);audio_tick()
        if step==3 and phase==0:m.pad(14);m.pad(14,0)
        if step==127 and phase==0:
            roll_pixels=picture()
            assert (117,7,122,7) in roll_pixels,'playhead did not reach step 128'
            assert all(y1<52 for _,_,_,y1 in roll_pixels[1:]), 'roll overlaps stock strip'
assert len(notes)==20,'changing view interrupted or restarted the eight-bar phrase'
assert all((frame-start)%6000==0 for frame in note_frames[5:]),note_frames
print('Eight-bar playback stayed on the audio grid without running the UI poll')
m.pad(14);m.pad(14,0)
# Queue from the final bar, with looping off. Playback continues for exactly one phrase.
def tap(index):
    m.pad(index);m.pad(index,0)
def tick(frame):
    m.w32(0x8059226c,frame//2);audio_tick();m.call(word(0x80229678),page)
tick(start+6000*127+4500)
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
# EXIT from the piano roll consumes both edges without leaving Sound Generator.
tap(14)
stock_widgets={word(page+offset) for offset in range(0x1a6c,0x1a84,4)}
assert len(stock_widgets)==6 and 0 not in stock_widgets
tile_draws=[]
observe(0x80140291,lambda uc,*_: (tile_draws.append(1),m._ret()))
m.call(word(0x80225790),word(page+0x1a80))
assert not tile_draws
m.key(0x22,True)
m.key(0x22,False)
assert m.uc.mem_read(0x80245880,2)==bytes([0x1e,0]), 'EXIT left Sound Generator'
rects.clear()
restore_order=[]
trace_fill=observe(0x800ce581,lambda uc,*_:restore_order.append('clear'))
m.uc.hook_del(stub_invalidate)
trace_dirty=observe(0x800d0019,lambda uc,*_: (restore_order.append(uc.reg_read(ar.UC_ARM_REG_R0)),m._ret()))
tick(offset+6000*128)
assert rects==[(0,0,127,63)], 'leaving Genie did not clear the display once'
assert restore_order[0]=='clear' and (stock_widgets|{widget})<=set(restore_order),restore_order
m.uc.hook_del(trace_fill);m.uc.hook_del(trace_dirty)
rects.clear()
m.call(word(0x802256fc),widget)
assert not rects, 'stock footer erased the display after Genie closed'
m.call(word(0x80225790),word(page+0x1a80))
assert tile_draws==[1]
assert m.pad(0), 'stock Sound Generator input did not resume'
m.call(word(0x8022bbd4),widget)
m.call(word(0x80222130),widget)
m.call(word(0x8022209c),widget)
assert background_draws == [1], 'stock bar background did not resume'
assert bar_draws == [1], 'stock PRM bar did not resume'
assert column_draws == [1], 'stock column did not resume'
print('Genie closed and stock pad dispatch resumed')

# Compare the actual CTRL event dispatch with exact upstream SP-ETC.
def shifted_ctrl_writes(firmware):
    machine=Machine(firmware)
    writes=[]
    def setting_write(uc,*_):
        setting=uc.reg_read(ar.UC_ARM_REG_R1)
        if 0x7b <= setting <= 0x80:
            writes.append(setting)
    machine.uc.hook_add(UC_HOOK_CODE,setting_write,begin=0x800dd0c0,end=0x800dd0c0)
    page_objects=[]
    machine.uc.hook_add(UC_HOOK_CODE,lambda uc,*_: page_objects.append(uc.reg_read(ar.UC_ARM_REG_R0)),
                        begin=int.from_bytes(machine.uc.mem_read(0x802296a4,4),'little')&~1,
                        end=int.from_bytes(machine.uc.mem_read(0x802296a4,4),'little')&~1)
    machine.pad(0);machine.pad(0,0)
    page=page_objects[-1]
    poll=int.from_bytes(machine.uc.mem_read(0x80229678,4),'little')
    for _ in range(3):
        machine.key(KEY_SHIFT,True)
        machine.call(poll,page)
        for knob in (1,2,3):
            writes.append(('CTRL',knob))
            for value in (12,64,114):
                machine.dispatch(9,knob,value)
        machine.key(KEY_SHIFT,False)
        machine.call(poll,page)
    return writes

if len(sys.argv)>1:
    original=shifted_ctrl_writes(Path(sys.argv[1]).read_bytes())
    actual=shifted_ctrl_writes(image)
    assert actual==original,(actual,original)
    print('SHIFT + CTRL routing matches the unmodified v9.53 image:',actual)

# Run the patched upstream feed tail with a known stereo packet.
with (ROOT/'build/genie.elf').open('rb') as fp:
    from elftools.elf.elffile import ELFFile
    capture_state=ELFFile(fp).get_section_by_name('.capture_state')['sh_addr']
m.w32(capture_state,3)
m.w32(capture_state+24,1024)
m.w32(capture_state+28,0)
m.uc.mem_write(0x82e0e96a,bytes([2]))
observe(0x1ca81,lambda uc,*_:m._ret())
# Resolve the target from the installed movw/movt and call it with the stock ABI.
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
instructions=list(Cs(CS_ARCH_ARM,CS_MODE_THUMB).disasm(bytes(m.uc.mem_read(0x83fcbc8a,8)),0x83fcbc8a))
parts=[int(i.op_str.split('#')[-1],0) for i in instructions]
feed=parts[0]|parts[1]<<16
packet=0x3008a000
raw=struct.pack('<16h',10000,-10000,*(1234,)*14)*64
def capture_block(frame):
    m.w32(capture_state+28,frame)
    m.uc.mem_write(packet,raw)
    m.call(feed,packet,len(raw))
    return list(struct.iter_unpack('<16h',bytes(m.uc.mem_read(packet,len(raw)))))
head=capture_block(0)
middle=capture_block(512)
tail=capture_block(960)
assert head[0][:2]==(0,0) and 0<head[-1][0]<10000
assert middle[0][:2]==(10000,-10000)
assert 0<tail[0][0]<10000 and tail[-1][:2]==(0,0)
assert all(row[2:]==(1234,)*14 for row in head+middle+tail)
m.w32(capture_state,0)
assert capture_block(0)[0][:2]==(10000,-10000)
print('Bounce feed fades both sample edges, leaves other channels and stock captures unchanged')

# Start playback on a render boundary after RX capture starts. The saved
# window skips capture lead and effects latency only.
with (ROOT/'build/genie.elf').open('rb') as fp:
    capture_clock=ELFFile(fp).get_section_by_name('.symtab').get_symbol_by_name('genie_bounce_clock')[0]['st_value']
m.w32(capture_state,2)
m.w32(capture_state+28,0)
assert m.call(capture_clock)==0
capture_block(64)
assert m.call(capture_clock)==1
window=128+75
assert word(capture_state+32)==window
window_head=capture_block(window)
assert window_head[0][:2]==(0,0)
assert 0<window_head[-1][0]<1700
assert capture_block(window+128)[0][:2]==(5000,-5000)
assert capture_block(window+256)[0][:2]==(10000,-10000)
assert m.call(capture_clock)==2
assert capture_block(window+1024-64)[-1][:2]==(0,0)
assert m.call(capture_clock)==3

# Stock finalization can move START and leave extra recorder frames at END.
# Exercise the exact range rewrite with a stereo file whose data begins at frame 16.
with (ROOT/'build/genie.elf').open('rb') as fp:
    trim=ELFFile(fp).get_section_by_name('.symtab').get_symbol_by_name('set_loop_range')[0]['st_value']
m.w32(capture_state+28,window+1024)
m.w32(0x82e0e970,0x3008c000)
m.w32(0x3008c1c0,108)
m.w32(0x82e495e4+8,2)
m.w32(0x82e495e4+12,44)
markers={0x67:32,0x68:window+1120,0x71:32}
recharged=[]
def read_marker(uc,*_):
    m._ret(markers[uc.reg_read(ar.UC_ARM_REG_R1)])
def write_marker(uc,*_):
    markers[uc.reg_read(ar.UC_ARM_REG_R1)]=uc.reg_read(ar.UC_ARM_REG_R2)
    m._ret()
observe(0x800dc6b9,read_marker)
observe(0x800da721,write_marker)
observe(0x800dda39,lambda uc,*_: (uc.reg_write(ar.UC_ARM_REG_R0,0),m._ret()))
for address in (0x800d6391,0x800d7509):
    observe(address,lambda uc,*_: (recharged.append(1),m._ret()))
assert m.call(trim)==1
assert markers=={0x67:16+window,0x68:1040+window,0x71:16+window} and len(recharged)==2
markers[0x68]=1000
assert m.call(trim)==0 and markers[0x68]==1000
print('Verified the first-pass capture window, exact faded loop range, and rejection of a short take')

if len(sys.argv)>1:
    def render_release(firmware, kind, env=0, blocks_to_render=22):
        synth=Machine(firmware)
        voice,bus=0x80249800,0x3008a000
        synth.uc.mem_write(voice,bytes(0x1b0))
        synth.call(0x80132341,voice)
        for offset,value in ((0x10,env),(0x48,50),(0x60,4400),(0x180,3),
                             (0x5c,12),(0x74,10000),(0x198,10000)):
            synth.w32(voice+offset,value)
        synth.uc.mem_write(voice+0x70,bytes([kind,kind]))
        synth.call(0x800d0669,voice,1)
        blocks=[]
        for block in range(blocks_to_render):
            if block==10:synth.call(0x800d0669,voice,0)
            synth.uc.mem_write(bus,bytes(64*14*4))
            synth.call(0x1fd81,0,bus,64,14)
            blocks.extend(struct.unpack('<896i',synth.uc.mem_read(bus,64*14*4))[::14])
        return blocks

    source=Path(sys.argv[1]).read_bytes()
    assert render_release(source,0)==render_release(image,0)
    for kind in range(15,32):
        dry=render_release(source,kind)
        smooth=render_release(image,kind)
        assert dry[384:640]==smooth[384:640],kind
        assert abs(smooth[0])<=abs(dry[0]),kind
        assert max(map(abs,smooth[:128]))<=max(map(abs,dry[:128])),kind
        assert not any(smooth[1088:]),kind
        change=lambda x: sum((b-a)**2 for a,b in zip(x[639:],x[640:]))
        assert change(smooth)<=change(dry),kind
    print('Real audio render: smoothed starts, unchanged sustained and stock tones, lower release edge energy on all 17 added waves')

    # A preset release keeps its audible decay but reaches digital silence earlier.
    dry=render_release(source,17,env=1,blocks_to_render=250)
    smooth=render_release(image,17,env=1,blocks_to_render=250)
    assert dry[384:640]==smooth[384:640]
    assert any(smooth[640:1280])
    assert max(i for i,x in enumerate(smooth) if x)<max(i for i,x in enumerate(dry) if x)
    print('Preset release retains its audible decay and reaches silence sooner')
