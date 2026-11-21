"""Build the Sequence Genie hardware-test image against exact SP-ETC v9.53."""
import argparse
import hashlib
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
UPSTREAM = ROOT.parent / 'synth'
sys.path.insert(0, str(UPSTREAM))
from build_arp import COPY, FLASH, file_offset, scatter, assemble, jump
from build_sg_subpad import Runtime
from build_start_fix import refs_into
from elftools.elf.elffile import ELFFile

PARENT = 'faeea5b004273f2dbcfca00387cfcc59078d3ba42fd832d52d363209d8b3b75e'
SLOTS = {'.capture': (0x83fe5d88,0x83fe6000), '.capture_poll': (0x83feee68,0x83fef000), '.capture_state': (0x83fef0b8,0x83fef100), '.undo': (0x83fa1bc0,0x83fa2000), '.controls': (0x83fa09c0,0x83fa1000), '.text': (0x83fc8480, 0x83fc9000), '.display': (0x83fdd7c0, 0x83fde000), '.roll': (0x83fd6a80,0x83fd7000), '.state': (0x83fd87c0, 0x83fd9000)}
HOOKS = [(0x8022209c, 0x801239e9, "genie_parameter_background"), (0x8022bbd4, 0x83fe7595, "genie_column"), (0x802296a4, 0x83fd4001, 'genie_press'), (0x802296a8, 0x83fd4079, 'genie_release'),
         (0x80229678, 0x83fe76e3, 'genie_poll'), (0x802256fc, 0x83fe7669, 'genie_footer'), (0x80229688,0x8015c531,'genie_encoder'), (0x80229680,0x8015c551,'genie_key'), (0x80229684,0x8015bf79,'genie_keyup')]

def build(input_path, dest, zig):
    parent = input_path.read_bytes()
    if hashlib.sha256(parent).hexdigest() != PARENT:
        raise ValueError('Expected the published v9.53 APP1')
    tmp = ROOT / 'build'
    tmp.mkdir(parents=True, exist_ok=True)
    ld = tmp / 'genie.ld'
    ld.write_text("""ENTRY(genie_press)
SECTIONS {
 .capture 0x83fe5d88 : { *(.capture) }
 .capture_poll 0x83feee68 : { *(.capture_poll) }
 .capture_state 0x83fef0b8 : { *(.capture_state) }
 .controls 0x83fa09c0 : { KEEP(*(.controls)) }
 .undo 0x83fa1bc0 : { KEEP(*(.undo)) *(.screen_strings) }
 .text 0x83fc8480 : { KEEP(*(.text.genie_encoder)) KEEP(*(.text.genie_key)) KEEP(*(.text.genie_keyup)) KEEP(*(.text.genie_press)) KEEP(*(.text.genie_release))
 KEEP(*(.text.genie_footer)) KEEP(*(.text.genie_poll)) *(EXCLUDE_FILE (*genie_screen*.o) .text* .rodata*) }
 .state 0x83fd87c0 : { *(.data*) *(.bss*) *(COMMON) }
 .roll 0x83fd6a80 : { KEEP(*(.roll)) }
 .display 0x83fdd7c0 : { *genie_screen*.o(.text* .rodata*) }
 /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}""")
    elf = tmp / 'genie.elf'
    subprocess.run([zig, 'cc',
                    '-target', 'thumb-freestanding-eabihf', '-mcpu=cortex_m7+vfp4d16',
                    '-DGENIE_ON_DEVICE','-Os','-g','-fno-lto','-Wl,-e,genie_press','-Wl,-u,genie_release','-Wl,-u,genie_poll','-Wl,-u,genie_footer','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections',
                    '-nostdlib','-Wl,--gc-sections','-Wl,--no-undefined','-Wl,-T,'+ld.as_posix(),
                    *[str(ROOT/x) for x in ('genie_v953.c','genie_bounce.c','sequence_perform.c','genie_screen.c')],
                    '-o',str(elf)],check=True)
    rows = list(scatter(parent))
    rt = Runtime(parent)
    img = bytearray(parent)
    blocks = []
    with elf.open('rb') as fp:
        obj = ELFFile(fp)
        for section in obj.iter_sections():
            if not(section['sh_flags'] & 2) or not section['sh_size']: continue
            lo,hi = SLOTS[section.name]
            # The hardware scatter loader copies whole words and drops partial tails.
            data = section.data()
            data += bytes(-len(data) % 4)
            n = len(data)
            if lo % 4:
                raise ValueError(f'Unaligned scatter destination: {lo:#x}')
            if section['sh_addr'] != lo or lo+n > hi: raise ValueError((section.name,n))
            if any(max(lo,d)<min(lo+n,d+l) for _,d,l,_ in rows) or refs_into(parent,lo,lo+n):
                raise ValueError('Used runtime span '+hex(lo))
            img.extend(bytes(-len(img)%16))
            rows.append([FLASH+len(img),lo,n,COPY])
            img.extend(data)
            blocks.append((lo,data))
        for address,original,name in HOOKS:
            if rt.read(address,4)!=struct.pack('<I',original):raise ValueError(name)
            target=obj.get_section_by_name('.symtab').get_symbol_by_name(name)[0]['st_value']
            if not target&1:raise ValueError(name)
            off=file_offset(parent,address,4)
            struct.pack_into('<I',img,off,target)
            blocks.append((address,struct.pack('<I',target)))
        address = 0x80123ab0
        original = bytes.fromhex('2de9f0479eb00446d0f88400')
        if rt.read(address,len(original)) != original:
            raise ValueError('Unexpected PRM renderer prologue')
        target = obj.get_section_by_name('.symtab').get_symbol_by_name('genie_parameter_bar')[0]['st_value']
        patch = assemble(jump(target) + 'nop',address)
        if len(patch) != len(original):
            raise ValueError('Unexpected PRM detour size')
        offset = file_offset(parent,address,len(patch))
        img[offset:offset+len(patch)] = patch
        blocks.append((address,patch))
    img.extend(bytes(-len(img)%16))
    table=len(img)
    for row in rows:img.extend(struct.pack('<4I',*row))
    struct.pack_into('<II',img,0x80,table-0x80,len(img)-0x80)
    after=Runtime(img)
    for address,expected in blocks:
        if after.read(address,len(expected))!=expected:raise ValueError(hex(address))
    dest.parent.mkdir(parents=True,exist_ok=True)
    dest.write_bytes(img)
    print(dest,hashlib.sha256(dest.read_bytes()).hexdigest())
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path, help='Exact SP-ETC v9.53 APP1')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/SP404MKII_APP1.bin')
    parser.add_argument('--zig', default='zig')
    args = parser.parse_args()
    build(args.input, args.output, args.zig)
