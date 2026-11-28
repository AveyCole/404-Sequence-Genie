"""Build and inject the 5 native i.MX DSP effects (49: STOP, 50: Dub Delay,
51: Algoverb, 52: Gated Verb, 53: Pitched Echo) into a Genie Suite APP1 image.

Supports both DOOM OS 0.7.4 + Genie Suite (--mode doom) and Stock 5.52 + Genie
Suite (--mode stock). Verifies every stock/SP-ETC byte before patching, performs
byte-level movw/movt table relocation scans, validates every patched instruction
with Capstone, and audits audio-path symbols for pure SDRAM residency.
"""

import argparse
import hashlib
import struct
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

REPO = Path(__file__).resolve().parents[1]
for p in (REPO / "genie" / "build" / "modular-dev" / "deps", REPO / "genie", REPO / "synth"):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from elftools.elf.elffile import ELFFile
from keystone import Ks, KS_ARCH_ARM, KS_MODE_LITTLE_ENDIAN, KS_MODE_THUMB

from build_fx_deep_edit import Runtime
from build_stock import COPY, FLASH, SETTINGS, file_offset, scatter

ZERO = 0x60080100
MD = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
KS = Ks(KS_ARCH_ARM, KS_MODE_THUMB | KS_MODE_LITTLE_ENDIAN)

# SDRAM layout carved from OS heap (0x8354700C -> 0x8361700C, 832 KB carved, 2.16 MB left)
POOL_OLD = 0x8354700C
POOL_NEW = 0x8361700C
FX_CODE_RAM = 0x83547010   # up to 48 KB .text + .rodata
FX_CODE_MAX = 0x0000C000
FX_BSS_RAM  = 0x83553000   # up to 16 KB .fx_bss
FX_BSS_MAX  = 0x00004000
# 0x83557000..0x83617000 = 768 KB (2 x 384 KB BUS 1 & BUS 2 delay/reverb arenas)

# Flash slots (tables live in flash XIP for UI; compiled code is copied to FX_CODE_RAM at boot)
DOOM_FX_FLASH_START = 0x6030CC10
DOOM_FX_FLASH_END   = 0x60318C10  # 49,152 B (reclaimed from unused DOOM boot frames)
STOCK_FX_FLASH_START = 0x603E1D00
STOCK_FX_FLASH_END   = 0x603EF000 # 54,016 B (contiguous zero gap)

# Stock table bases & expected movw/movt counts in stock SDRAM code (0x80000000..0x801A0000)
OLD_TABLES = {
    "effect_names_1":     (0x8020B3D8, 1),
    "effect_names_2":     (0x8020B49C, 2),
    "effect_descriptors": (0x802E5B40, 35),
    "grid_table_T3":      (0x80245D54, 10),
    "grid_table_T3_alt":  (0x80245DFC, 3),
    "param_names":        (0x80245EE4, 2),
    "param_defaults":     (0x801ABE64, 3),
    "param_max":          (0x801AC830, 19),
    "param_min":          (0x801AD1FC, 18),
}

# SP-ETC fx_deep_edit (0x83FE2000) references that must also be repointed on DOOM Suite
FDE_BASE = 0x83FE2000
FDE_COUNT_OLD = 0x83FE2FFE
FDE_TABLES = {
    "effect_descriptors": (0x802E5B40, 3),
    "param_defaults":     (0x801ABE64, 3),
    "param_max":          (0x801AC830, 2),
    "param_min":          (0x801AD1FC, 2),
    "fde_count":          (FDE_COUNT_OLD, 8),
}

NEW_EFFECTS = [
    # (id, name, [(param_name, min, max, default), ... x6]) -- all ranges <= 200 for exact Roland KnobScale
    (49, "STOP", [
        ("TIME",     0, 200,  75),   # x5ms -> 0..1000 ms (def 375ms)
        ("FEEDBACK", 0,  95,  40),
        ("MIX",      0, 100,  50),
        ("DROPOUT",  0, 100,  30),
        ("TELE",     0, 100, 100),
        ("CRUSH",    0,   5,   0),
    ]),
    (50, "Dub Delay", [
        ("TIME",     0, 200,  45),   # x10ms -> 0..2000 ms (def 450ms)
        ("FEEDBACK", 0,  98,  55),
        ("MIX",      0, 100,  40),
        ("TONE",     0, 100,  35),
        ("MOD",      0, 100,  15),
        ("SYNC",     0,   5,   0),
    ]),
    (51, "Algoverb", [
        ("SIZE",      0, 100,  60),
        ("DECAY",     0, 100,  50),
        ("DAMP",      0, 100,  40),
        ("MIX",       0, 100,  35),
        ("PRE DELAY", 0, 200,  20),
        ("WIDTH",     0, 100, 100),
    ]),
    (52, "Gated Verb", [
        ("SIZE",     0, 100,  70),
        ("DAMP",     0, 100,  30),
        ("THRESH",   0,  60,  24),
        ("HOLD",     2, 100,  30),   # x5ms -> 10..500 ms (def 150ms)
        ("RELEASE",  5, 200,  30),
        ("MIX",      0, 100,  50),
    ]),
    (53, "Pitched Echo", [
        ("TIME",     0, 200,  70),   # x5ms -> 0..1000 ms (def 350ms)
        ("FEEDBACK", 0,  90,  45),
        ("PITCH",    0,  24,  19),
        ("MIX",      0, 100,  45),
        ("TONE",     0, 100,  30),
        ("GRAIN",   20, 100,  55),
    ]),
    (54, "FREEZE", [
        ("GRAIN",   10, 250,  80),   # 10..250 ms (def 80ms)
        ("PITCH",    0,  24,  12),   # -12..+12 st (def 0st = 12)
        ("DENSITY",  0, 100,  50),   # 0..100% (1..4 overlapping grains)
        ("MIX",      0, 100,  50),   # 0..100% wet (0 = bit-identical passthrough)
        ("FREEZE",   0, 100,   0),   # <50 Live, >=50 Frozen
        ("POSITN",   0, 100,  85),   # Live behind-write / Frozen scrub pos
    ]),
    (55, "SPRING", [
        ("DWELL",    0, 100,  60),   # 0..2x input drive + soft clip
        ("DECAY",    0, 100,  50),   # tank feedback ~0.3..3.5 s
        ("TENSION",  0, 100,  50),   # dispersive allpass delay spread
        ("MIX",      0, 100,  35),   # 0..100% wet (0 = bit-identical passthrough)
        ("DAMP",     0, 100,  40),   # feedback LP ~8 kHz -> 800 Hz
        ("DRIP",     0, 100,  50),   # 1-pole HP transient tap into tank
    ]),
    (56, "SHIMMER", [
        ("SHIMMER",  0, 100,  55),   # pitch-shifted shimmer feedback amount
        ("PITCH",    0,  24,  24),   # -12..+12 st (def +12 st = 24)
        ("SIZE",     0, 100,  70),   # tank room size
        ("DECAY",    0, 100,  65),   # tank tail decay
        ("DAMP",     0, 100,  35),   # high-frequency damping
        ("MIX",      0, 100,  45),   # 0..100% wet (0 = exact passthrough)
    ]),
    (57, "GEN LOSS", [
        ("WOW",      0, 100,  35),   # slow pitch drift
        ("FLUTTER",  0, 100,  25),   # fast motor warble
        ("FAILURE",  0, 100,  30),   # oxide dropouts & L/R azimuth wobble
        ("SATURATE", 0, 100,  40),   # asymmetric magnetic tape drive
        ("BAND",     0, 100,  35),   # bandwidth: Reel -> VHS -> Microcassette
        ("NOISE",    0, 100,  20),   # pink tape hiss & micro-crackle
    ]),
    (58, "CHORD RESO", [
        ("ROOT",     0,  23,   0),   # C2..B3 chromatic root note
        ("CHORD",    0,   7,   0),   # MIN/MAJ/MIN7/MAJ7/MIN9/SUS4/5THS/OCT
        ("DECAY",    0, 100,  65),   # modal comb decay
        ("DAMP",     0, 100,  35),   # Karplus-Strong loop damping
        ("SPREAD",   0, 100,  65),   # stereo voice spread
        ("MIX",      0, 100,  45),   # 0..100% wet (0 = exact passthrough)
    ]),
    (59, "TRANSIENT", [
        ("ATTACK",   0, 200, 125),   # -100..+100% (def +25%)
        ("SUSTAIN",  0, 200, 100),   # -100..+100% (def 0%)
        ("PUNCH",    0, 100,  25),   # ~100 Hz low-end chest thump
        ("DRIVE",    0, 100,  15),   # analog bus glue soft-clip
        ("TONE",     0, 100,  50),   # -50..+50 tilt EQ
        ("MIX",      0, 100, 100),   # 0..100% wet (0 = exact passthrough)
    ]),
    (60, "MICROLOOP", [
        ("LENGTH",   0, 100,  40),   # 15..500 ms free slice length
        ("MODE",     0,   5,   0),   # FWD/REV/PONG/HALF/DBL/R-HALF
        ("GLITCH",   0, 100,  30),   # hold/jump/flip probability
        ("FADE",     0, 100,  25),   # boundary crossfade & swell softness
        ("SYNC",     0,   5,   0),   # OFF/1/4/1/8/1/8T/1/16/1/32
        ("MIX",      0, 100,  50),   # 0..100% wet (0 = exact passthrough)
    ]),
]


def asm(text, addr=0):
    raw, _ = KS.asm(text, addr)
    return bytes(raw)


def encode_movw_movt(is_movt, rd, imm16):
    i = (imm16 >> 11) & 1
    imm4 = (imm16 >> 12) & 0xF
    imm3 = (imm16 >> 8) & 7
    imm8 = imm16 & 0xFF
    hw1 = (0xF2C0 if is_movt else 0xF240) | (i << 10) | imm4
    hw2 = (imm3 << 12) | (rd << 8) | imm8
    return struct.pack("<HH", hw1, hw2)


def scan_movw_movt(mem, base, target_addrs):
    lo_map = {}
    for t in target_addrs:
        lo_map.setdefault(t & 0xFFFF, []).append(t)
    results = {t: [] for t in target_addrs}
    n = len(mem)
    for off in range(0, n - 4, 2):
        hw1, hw2 = struct.unpack_from("<HH", mem, off)
        if (hw1 & 0xFBF0) == 0xF240 and (hw2 & 0x8000) == 0:
            i = (hw1 >> 10) & 1
            imm4 = hw1 & 0xF
            imm3 = (hw2 >> 12) & 7
            rd = (hw2 >> 8) & 0xF
            imm8 = hw2 & 0xFF
            imm16 = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8
            if imm16 in lo_map:
                for off2 in range(off + 4, min(n - 3, off + 256), 2):
                    h1, h2 = struct.unpack_from("<HH", mem, off2)
                    if (h1 & 0xFBF0) == 0xF2C0 and (h2 & 0x8000) == 0:
                        rd2 = (h2 >> 8) & 0xF
                        if rd2 == rd:
                            i2 = (h1 >> 10) & 1
                            im4 = h1 & 0xF
                            im3 = (h2 >> 12) & 7
                            im8 = h2 & 0xFF
                            hi16 = (im4 << 12) | (i2 << 11) | (im3 << 8) | im8
                            full = (hi16 << 16) | imm16
                            if full in results:
                                results[full].append((base + off, base + off2, rd))
                            break
    return results


def jump10(target_odd):
    if not (target_odd & 1):
        raise ValueError(f"Jump target {target_odd:#x} missing Thumb bit!")
    return (encode_movw_movt(False, 12, target_odd & 0xFFFF)
            + encode_movw_movt(True, 12, target_odd >> 16)
            + bytes.fromhex("6047"))  # bx ip


def compile_fx_module(zig_path, scratch, passthrough_only=False):
    fx_dir = REPO / "fx"
    ld = scratch / "fx.ld"
    elf = scratch / "fx.elf"
    ld.write_text(f"""SECTIONS {{
  .text {FX_CODE_RAM:#x} : {{
    KEEP(*(.text.fx_hooks))
    *(.text .text.*)
    *(.rodata .rodata.*)
    . = ALIGN(4);
  }}
  .fx_bss {FX_BSS_RAM:#x} (NOLOAD) : {{
    *(.fx_bss)
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
  }}
  /DISCARD/ : {{ *(.ARM.exidx* .ARM.extab* .comment .note* .debug*) }}
}}
""", encoding="ascii")

    sources = [fx_dir / "fx_hooks.S", fx_dir / "fx_plumbing.c"]
    defines = []
    if passthrough_only:
        defines.append("-DFX_PASSTHROUGH_ONLY")
    else:
        sources.extend([
            fx_dir / "fx_delay_core.c",
            fx_dir / "fx_reverb_core.c",
            fx_dir / "fx_freeze_core.c",
            fx_dir / "fx_spring_core.c",
            fx_dir / "fx_page4_core.c",
        ])

    cmd = [
        str(zig_path), "cc",
        "-target", "thumb-freestanding-eabihf",
        "-mcpu=cortex_m7+vfp4d16",
        "-Os", "-g", "-nostdlib", "-ffreestanding", "-fno-builtin", "-fno-lto",
        "-Wall", "-Wextra",
        "-I", str(fx_dir),
        *defines,
        "-Wl,-e,fx_audio_hook_doom",
        "-Wl,--gc-sections",
        "-Wl,-T," + ld.as_posix(),
        *(str(s) for s in sources),
        "-o", str(elf),
    ]
    subprocess.run(cmd, check=True)

    with elf.open("rb") as f:
        obj = ELFFile(f)
        text_sec = obj.get_section_by_name(".text")
        bss_sec = obj.get_section_by_name(".fx_bss")
        code_bytes = text_sec.data()
        bss_size = bss_sec["sh_size"] if bss_sec else 0
        if len(code_bytes) > FX_CODE_MAX:
            raise ValueError(f"FX code ({len(code_bytes)} B) exceeds {FX_CODE_MAX} B limit")
        if bss_size > FX_BSS_MAX:
            raise ValueError(f"FX BSS ({bss_size} B) exceeds {FX_BSS_MAX} B limit")

        symtab = obj.get_section_by_name(".symtab")
        syms = {}
        for name in (
            "fx_audio_hook_doom", "fx_audio_hook_stock",
            "fx_dt1_hook_doom", "fx_dt1_hook_stock",
            "fx_set_ctrl_hook", "fx_dirty_hook_1", "fx_dirty_hook_2",
            "fx_get_param_hook", "fx_fmt_param_hook", "fx_find_t3_led",
            "fx_page4_title_hook", "fx_page4_keys_hook",
            "fx_process_block", "fx_on_select", "fx_set_ctrl_ext",
            "fx_get_param_ext", "fx_format_param", "g_fx",
        ):
            found = symtab.get_symbol_by_name(name)
            if not found:
                raise ValueError(f"Missing symbol {name} in fx.elf")
            syms[name] = found[0]["st_value"]

        # Verify pure SDRAM residency for every symbol in the ELF (no flash calls in audio path)
        for s in symtab.iter_symbols():
            val = s["st_value"]
            if s["st_info"]["type"] == "STT_FUNC" and val != 0:
                if not (FX_CODE_RAM <= (val & ~1) < FX_CODE_RAM + FX_CODE_MAX):
                    raise ValueError(f"Function {s.name} at {val:#x} is outside FX SDRAM region!")

    return code_bytes, bss_size, syms, elf.read_bytes()


def build_relocated_tables(rt, base_flash, include_fde_count=False):
    """Assemble the extended read-only MFX and parameter tables at base_flash."""
    buf = bytearray()
    addrs = {}

    def align4():
        while len(buf) % 4:
            buf.append(0)

    # 1. Effect name strings (5 new) + 2x54 pointer table (names_1 + names_2)
    stock_n1 = struct.unpack("<49I", rt.read(0x8020B3D8, 49 * 4))
    stock_n2 = struct.unpack("<49I", rt.read(0x8020B49C, 49 * 4))
    align4()
    new_fx_name_ptrs = []
    for _, fx_name, _ in NEW_EFFECTS:
        new_fx_name_ptrs.append(base_flash + len(buf))
        buf.extend(fx_name.encode("ascii") + b"\0")
    align4()
    addrs["effect_names_1"] = base_flash + len(buf)
    for ptr in list(stock_n1) + new_fx_name_ptrs:
        buf.extend(struct.pack("<I", ptr))
    addrs["effect_names_2"] = base_flash + len(buf)
    for ptr in list(stock_n2) + new_fx_name_ptrs:
        buf.extend(struct.pack("<I", ptr))

    # 2. Effect descriptor table (54 x 6 int32s)
    align4()
    addrs["effect_descriptors"] = base_flash + len(buf)
    buf.extend(rt.read(0x802E5B40, 49 * 6 * 4))
    next_pid = 627
    for _ in NEW_EFFECTS:
        for k in range(6):
            buf.extend(struct.pack("<i", next_pid + k))
        next_pid += 6

    # 3. Grid table T3 (42 stock + 5 new = 47 u32s) and T3_alt (0x80245DFC, 42 + 5 = 47 u32s)
    align4()
    addrs["grid_table_T3"] = base_flash + len(buf)
    buf.extend(rt.read(0x80245D54, 42 * 4))
    for fx_id, _, _ in NEW_EFFECTS:
        buf.extend(struct.pack("<I", fx_id))

    align4()
    addrs["grid_table_T3_alt"] = base_flash + len(buf)
    buf.extend(rt.read(0x80245DFC, 42 * 4))
    for fx_id, _, _ in NEW_EFFECTS:
        buf.extend(struct.pack("<I", fx_id))

    # 4. Parameter name strings (30 new) + 657-entry pointer table
    align4()
    new_param_name_ptrs = []
    for _, _, params in NEW_EFFECTS:
        for pname, _, _, _ in params:
            new_param_name_ptrs.append(base_flash + len(buf))
            buf.extend(pname.encode("ascii") + b"\0")
    align4()
    addrs["param_names"] = base_flash + len(buf)
    buf.extend(rt.read(0x80245EE4, 627 * 4))
    for ptr in new_param_name_ptrs:
        buf.extend(struct.pack("<I", ptr))

    # 5. Parameter defaults / max / min (657 int32s each; max[127] = 53)
    new_defs, new_maxs, new_mins = [], [], []
    for _, _, params in NEW_EFFECTS:
        for _, pmin, pmax, pdef in params:
            new_mins.append(pmin)
            new_maxs.append(pmax)
            new_defs.append(pdef)

    align4()
    addrs["param_defaults"] = base_flash + len(buf)
    buf.extend(rt.read(0x801ABE64, 627 * 4))
    for v in new_defs:
        buf.extend(struct.pack("<i", v))

    align4()
    addrs["param_max"] = base_flash + len(buf)
    stock_maxs = list(struct.unpack("<627i", rt.read(0x801AC830, 627 * 4)))
    if stock_maxs[127] != 48:
        raise ValueError(f"Expected stock max_table[127] == 48, got {stock_maxs[127]}")
    stock_maxs[127] = 60
    for v in stock_maxs + new_maxs:
        buf.extend(struct.pack("<i", v))

    align4()
    addrs["param_min"] = base_flash + len(buf)
    buf.extend(rt.read(0x801AD1FC, 627 * 4))
    for v in new_mins:
        buf.extend(struct.pack("<i", v))

    # 6. Extended 55-byte fde_count table for SP-ETC fx_deep_edit (DOOM Suite)
    if include_fde_count:
        align4()
        addrs["fde_count"] = base_flash + len(buf)
        buf.extend(rt.read(FDE_COUNT_OLD, 49))
        buf.extend(bytes(len(NEW_EFFECTS)))  # 0 extra hidden screens for FX 49..54

    align4()
    return bytes(buf), addrs


def patch_exact(image, rows, addr, expected, replacement, label):
    if len(expected) != len(replacement):
        raise ValueError(f"{label}: length mismatch {len(expected)} != {len(replacement)}")
    off = file_offset(rows, addr, len(expected))
    actual = bytes(image[off:off + len(expected)])
    if actual != expected:
        raise ValueError(f"{label} @ {addr:#x}: expected {expected.hex()}, got {actual.hex()}")
    image[off:off + len(replacement)] = replacement


def repoint_movw_movt(image, rows, seg_mem, seg_base, table_spec, new_addrs, scope_name):
    old_map = {old_addr: (name, expected_cnt) for name, (old_addr, expected_cnt) in table_spec.items()}
    hits = scan_movw_movt(seg_mem, seg_base, list(old_map.keys()))
    total_patched = 0
    for old_addr, (name, expected_cnt) in old_map.items():
        sites = hits[old_addr]
        if len(sites) != expected_cnt:
            raise ValueError(
                f"{scope_name} {name} ({old_addr:#x}): expected {expected_cnt} movw/movt sites, found {len(sites)}: "
                f"{[hex(s[0]) for s in sites]}"
            )
        new_addr = new_addrs[name]
        for lo_addr, hi_addr, rd in sites:
            exp_lo = encode_movw_movt(False, rd, old_addr & 0xFFFF)
            exp_hi = encode_movw_movt(True, rd, old_addr >> 16)
            new_lo = encode_movw_movt(False, rd, new_addr & 0xFFFF)
            new_hi = encode_movw_movt(True, rd, new_addr >> 16)
            patch_exact(image, rows, lo_addr, exp_lo, new_lo, f"{scope_name}.{name}.lo")
            patch_exact(image, rows, hi_addr, exp_hi, new_hi, f"{scope_name}.{name}.hi")
            total_patched += 1
    return total_patched


def reclaim_doom_boot_frames(image):
    """Switch DOOM boot drawer to Cheddar-only and verify 0x6030CC10..0x60318C10 is free."""
    from build_cheddar_boot import ART, BOOT_DRAW, BOOT_TICK, STOCK_DRAW, boot_frame, patch
    rows = scatter(image)
    off = file_offset(rows, BOOT_DRAW, 8)
    draw_ptr, tick_ptr = struct.unpack_from("<2I", image, off)
    if tick_ptr != (BOOT_TICK | 1):
        raise ValueError(f"Unexpected BOOT_TICK pointer {tick_ptr:#x}")
    if draw_ptr == 0x6032FC69:
        # Restore Roland's boot drawer and install direct_cheddar portrait
        image[off:off + 8] = struct.pack("<2I", STOCK_DRAW, BOOT_TICK | 1)
        image = bytearray(patch(image, boot_frame(ART), direct_cheddar=True))
    # Verify 0x6030CC10..0x60318C10 does not overlap any COPY row
    rows = scatter(image)
    for src, _, length, kind in rows:
        if kind == COPY and max(DOOM_FX_FLASH_START, src) < min(DOOM_FX_FLASH_END, src + length):
            raise ValueError(f"Reclaimed DOOM boot slot overlaps COPY row at {src:#x}")
    # Zero the reclaimed 48 KB span (preserving 0x60309C10..0x6030CC10 addons!)
    s_off = DOOM_FX_FLASH_START - FLASH
    e_off = DOOM_FX_FLASH_END - FLASH
    image[s_off:e_off] = bytes(e_off - s_off)
    return image


def bump_os_pool_and_append_copy(image, flash_code_addr, code_len):
    """Carve POOL_OLD..POOL_NEW from OS heap and add scatter COPY row -> FX_CODE_RAM."""
    rows = scatter(image)
    zero_row = next((r for r in rows if r[1] == 0x802E7800), None)
    if zero_row is None or zero_row[3] != ZERO:
        raise ValueError("Could not locate 0x802E7800 OS pool ZERO row")
    if zero_row[1] + zero_row[2] != POOL_OLD:
        raise ValueError(f"Expected OS pool end {POOL_OLD:#x}, got {zero_row[1] + zero_row[2]:#x}")

    # Patch OS allocator base at 0x80002268 / 0x80002270
    patch_exact(image, rows, 0x80002268,
                encode_movw_movt(False, 0, POOL_OLD & 0xFFFF),
                encode_movw_movt(False, 0, POOL_NEW & 0xFFFF), "os_pool_lo")
    patch_exact(image, rows, 0x80002270,
                encode_movw_movt(True, 0, POOL_OLD >> 16),
                encode_movw_movt(True, 0, POOL_NEW >> 16), "os_pool_hi")

    # Rebuild scatter table at the end of APP1
    table_start = struct.unpack_from("<I", image, 0x80)[0] + 0x80
    del image[table_start:]

    new_rows = []
    for s, d, l, h in rows:
        if d == 0x802E7800:
            l = POOL_NEW - d
        new_rows.append((s, d, l, h))
    padded_len = (code_len + 3) & ~3
    new_rows.append((flash_code_addr, FX_CODE_RAM, padded_len, COPY))

    # Sanity-check no RAM overlap among COPY rows
    copy_rows = [(d, d + l) for _, d, l, h in new_rows if h == COPY]
    for i, (a0, a1) in enumerate(copy_rows):
        for b0, b1 in copy_rows[i + 1:]:
            if max(a0, b0) < min(a1, b1):
                raise ValueError(f"SDRAM COPY overlap: {a0:#x}..{a1:#x} vs {b0:#x}..{b1:#x}")

    for row in new_rows:
        image.extend(struct.pack("<4I", *row))
    struct.pack_into("<I", image, 0x84, len(image) - 0x80)
    if FLASH + len(image) > SETTINGS:
        raise ValueError(f"Image exceeds settings sector: {FLASH + len(image):#x} > {SETTINGS:#x}")
    return image


def build_new_fx(in_path, out_path, zig_path, mode="doom", passthrough_only=False, elf_out=None):
    raw = in_path.read_bytes()
    in_sha = hashlib.sha256(raw).hexdigest()
    print(f"Input ({mode}): {in_path} ({len(raw):,} B, sha256={in_sha[:16]}...)")

    rt_in = Runtime(raw)
    image = bytearray(raw)

    if mode == "doom":
        image = reclaim_doom_boot_frames(image)
        flash_start, flash_end = DOOM_FX_FLASH_START, DOOM_FX_FLASH_END
    elif mode == "stock":
        flash_start, flash_end = STOCK_FX_FLASH_START, STOCK_FX_FLASH_END
        s_off, e_off = flash_start - FLASH, flash_end - FLASH
        if any(image[s_off:e_off]):
            raise ValueError(f"Stock FX flash slot {flash_start:#x}..{flash_end:#x} is not empty!")
    else:
        raise ValueError(f"Unknown mode {mode!r}")

    with TemporaryDirectory() as tmp:
        code_bytes, bss_size, syms, elf_bytes = compile_fx_module(
            zig_path, Path(tmp), passthrough_only=passthrough_only
        )
    if elf_out:
        elf_out.parent.mkdir(parents=True, exist_ok=True)
        elf_out.write_bytes(elf_bytes)

    tables_bytes, new_addrs = build_relocated_tables(
        rt_in, flash_start, include_fde_count=(mode == "doom")
    )
    flash_code_addr = (flash_start + len(tables_bytes) + 15) & ~15
    total_flash = (flash_code_addr + len(code_bytes)) - flash_start
    avail_flash = flash_end - flash_start
    print(f"Flash layout: tables={len(tables_bytes):,} B @ {flash_start:#x}, "
          f"code={len(code_bytes):,} B @ {flash_code_addr:#x}, "
          f"total={total_flash:,} / {avail_flash:,} B ({avail_flash - total_flash:,} B free), "
          f"BSS={bss_size:,} B @ {FX_BSS_RAM:#x}")
    if flash_code_addr + len(code_bytes) > flash_end:
        raise ValueError(f"FX payload overflows flash slot by {(flash_code_addr + len(code_bytes)) - flash_end} B")

    # Write tables + code into reclaimed flash region
    t_off = flash_start - FLASH
    c_off = flash_code_addr - FLASH
    image[t_off:t_off + len(tables_bytes)] = tables_bytes
    image[c_off:c_off + len(code_bytes)] = code_bytes

    # Bump OS pool + add COPY row to load FX code into SDRAM at boot
    image = bump_os_pool_and_append_copy(image, flash_code_addr, len(code_bytes))
    rows = scatter(image)

    # 1. Byte-level movw/movt repoint across stock SDRAM code
    sdram_code = rt_in.read(0x80000000, 0x1A0000)
    n_stock = repoint_movw_movt(image, rows, sdram_code, 0x80000000, OLD_TABLES, new_addrs, "stock")
    print(f"Repointed {n_stock} stock movw/movt table pairs (expected 93)")

    # 2. Repoint SP-ETC fx_deep_edit (0x83FE2000) references on DOOM Suite
    if mode == "doom":
        fde_mem = rt_in.read(FDE_BASE, 0x2700)
        n_fde = repoint_movw_movt(image, rows, fde_mem, FDE_BASE, FDE_TABLES, new_addrs, "fx_deep_edit")
        print(f"Repointed {n_fde} fx_deep_edit movw/movt pairs (expected 18)")

    # 3. Update stock max_table[127] in place as well (belt-and-suspenders: 48 -> 60)
    patch_exact(image, rows, 0x801ACA2C, struct.pack("<I", 48), struct.pack("<I", 60), "max_table[127]")

    # 4. Update grid/knob/notify/encoder bounds for 54-entry 4-page grid (indices 0..53 = 0x00..0x35)
    for addr in (0x80145E38, 0x80145E4C, 0x80145E60, 0x80145E74, 0x80145E88, 0x80145E9C):
        patch_exact(image, rows, addr, bytes.fromhex("292a"), bytes.fromhex("352a"), f"grid_draw_r2@{addr:#x}")
    patch_exact(image, rows, 0x80145F58, bytes.fromhex("2928"), bytes.fromhex("3528"), "grid_draw_r0")
    patch_exact(image, rows, 0x801472AA, bytes.fromhex("292d"), bytes.fromhex("352d"), "grid_select_r5")
    # CTRL 3 knob multiplier on MFX grid = grid_entries - 1 = 54 - 1 = 53 = 0x35 (Rule 7!)
    patch_exact(image, rows, 0x80147A58, bytes.fromhex("2920"), bytes.fromhex("3520"), "knob_scale_mult")
    # Post-apply notify loop count = 54 = 0x36
    patch_exact(image, rows, 0x801348C2, bytes.fromhex("2a2e"), bytes.fromhex("362e"), "notify_loop_bound")

    # 4a. Extend MFX Grid from 3 pages (48 cells) to 4 pages (64 cells, Pages 1/4..4/4)!
    # (1) Grid draw page->base offset: r7 = (r5 - 1) << 4 (0, 16, 32, 48)
    patch_exact(image, rows, 0x80145D02, bytes.fromhex("4fea0817"), bytes.fromhex("6f1e3f01"), "page4_grid_base")
    patch_exact(image, rows, 0x80145D0E, bytes.fromhex("032d"), bytes.fromhex("00bf"), "page4_grid_nop1")
    patch_exact(image, rows, 0x80145D14, bytes.fromhex("08bf2027"), bytes.fromhex("00bf00bf"), "page4_grid_nop2")
    # (2) Header active-FX base offset: r0 = (r5 - 1) << 4 (keeping movw/movt r1 & mov.w r2)
    patch_exact(image, rows, 0x801460EA, bytes.fromhex("a81e032db0fa80f0"), bytes.fromhex("681e000100bf00bf"), "page4_hdr_base")
    patch_exact(image, rows, 0x801460F6, bytes.fromhex("4fea5010"), bytes.fromhex("00bf00bf"), "page4_hdr_nop1")
    patch_exact(image, rows, 0x801460FE, bytes.fromhex("4fea0010"), bytes.fromhex("00bf00bf"), "page4_hdr_nop2")
    patch_exact(image, rows, 0x80146106, bytes.fromhex("08bf2020"), bytes.fromhex("00bf00bf"), "page4_hdr_nop3")
    # (3) Header page counter "%01d/%01d": 3 -> 4 (shows 1/4, 2/4, 3/4, 4/4)
    patch_exact(image, rows, 0x80146138, bytes.fromhex("0323"), bytes.fromhex("0423"), "page4_hdr_total")
    # (4) Header title string for Page 4 (" MFX LIST 49-64")
    patch_exact(image, rows, 0x8014615A,
                bytes.fromhex("03280cd0022812d00128e6d1"),
                jump10(syms["fx_page4_title_hook"] | 1) + bytes.fromhex("00bf"), "page4_hdr_title")
    # (5) Grid draw & active-page range checks: accept pages 1..4 (subs rX, page, #1; cmp rX, #4)
    patch_exact(image, rows, 0x80146240, bytes.fromhex("032b"), bytes.fromhex("042b"), "page4_draw_check")
    patch_exact(image, rows, 0x8014690C, bytes.fromhex("0328"), bytes.fromhex("0428"), "page4_tick_check")
    # (6) Grid index -> page number (0..15->1, 16..31->2, 32..47->3, 48..63->4)
    idx_to_page = asm("lsrs r0, r1, #4\nadds r0, #1\nb 0x801468b2\nnop\n", 0x80146D08)
    patch_exact(image, rows, 0x80146D08, bytes.fromhex("0f2963dc0120d0e5"), idx_to_page, "page4_idx_to_page")
    # (7) Hold MFX + (DJFX + ISOLATOR) direct Page-4 shortcut
    patch_exact(image, rows, 0x80146920,
                bytes.fromhex("2f2093f7fdfa002800f0de800220e2e0"),
                jump10(syms["fx_page4_keys_hook"] | 1) + bytes.fromhex("00bf00bf00bf"), "page4_keys_hook")

    # 4b. Replace ALL 4 unrolled 42-entry T3/T3_alt linear searches & encoder clamps (now 54 entries: 0..53)!
    # (A) MFX List VALUE encoder (0x80146FC0): search r8=T3'[0..53] for r1 -> r5 (or -1), b 0x801471F6; clamp 0x35
    enc_loop_146ffc = asm(
        "movs r5, #0\n"
        "loop: ldr.w r2, [r8, r5, lsl #2]\n"
        "cmp r2, r1\n"
        "beq done\n"
        "adds r5, #1\n"
        "cmp r5, #54\n"
        "blo loop\n"
        "mov.w r5, #-1\n"
        "done: b.w 0x801471f6\n",
        0x80146FFC,
    )
    patch_exact(image, rows, 0x80146FFC,
                rt_in.read(0x80146FFC, len(enc_loop_146ffc)), enc_loop_146ffc, "mfx_encoder_search")
    patch_exact(image, rows, 0x801471F8, bytes.fromhex("292fa8bf2927"), bytes.fromhex("352fa8bf3527"), "mfx_encoder_clamp")

    # (B) EFX VALUE encoder stepper 1 (0x8006EB00): search r0=T3_alt'[0..53] for r4 -> r1 (or 0), b 0x8006ED18
    step1_loop = asm(
        "movs r1, #0\n"
        "loop: ldr.w r2, [r0, r1, lsl #2]\n"
        "cmp r2, r4\n"
        "beq done\n"
        "adds r1, #1\n"
        "cmp r1, #54\n"
        "blo loop\n"
        "movs r1, #0\n"
        "done: b.w 0x8006ed18\n",
        0x8006EB22,
    )
    patch_exact(image, rows, 0x8006EB22, rt_in.read(0x8006EB22, len(step1_loop)), step1_loop, "efx_step1_search")
    patch_exact(image, rows, 0x8006ED28, bytes.fromhex("2929a8bf2921"), bytes.fromhex("3529a8bf3521"), "efx_step1_clamp")
    patch_exact(image, rows, 0x8006ED34, bytes.fromhex("3029"), bytes.fromhex("3c29"), "efx_step1_maxid")

    # (C) EFX VALUE encoder stepper 2 (0x8006ED40): search r1=T3'[0..53] for r0 -> r2 (or 0), b 0x8006EF4C
    step2_loop = asm(
        "movs r2, #0\n"
        "loop: ldr.w ip, [r1, r2, lsl #2]\n"
        "cmp ip, r0\n"
        "beq done\n"
        "adds r2, #1\n"
        "cmp r2, #54\n"
        "blo loop\n"
        "movs r2, #0\n"
        "done: b.w 0x8006ef4c\n",
        0x8006ED56,
    )
    patch_exact(image, rows, 0x8006ED56, rt_in.read(0x8006ED56, len(step2_loop)), step2_loop, "efx_step2_search")
    patch_exact(image, rows, 0x8006EF5A, bytes.fromhex("292aa8bf2922"), bytes.fromhex("352aa8bf3522"), "efx_step2_clamp")
    patch_exact(image, rows, 0x8006EF66, bytes.fromhex("2f2b"), bytes.fromhex("3b2b"), "efx_step2_maxid")

    # (D) MFX screen current-page highlight lookup (0x80146B4C): search r2=T3'[0..53] for r0 -> r1 (or -1), b 0x80146D02
    page_loop = asm(
        "movs r1, #0\n"
        "loop: ldr.w r3, [r2, r1, lsl #2]\n"
        "cmp r3, r0\n"
        "beq done\n"
        "adds r1, #1\n"
        "cmp r1, #54\n"
        "blo loop\n"
        "mov.w r1, #-1\n"
        "done: b.w 0x80146d02\n",
        0x80146B4C,
    )
    patch_exact(image, rows, 0x80146B4C, rt_in.read(0x80146B4C, len(page_loop)), page_loop, "mfx_page_lookup")

    # (E) MFX grid pad-LED highlight lookup (0x8002649A): jump10 -> fx_find_t3_led
    patch_exact(image, rows, 0x8002649A,
                bytes.fromhex("0a68a24204f06d804a68"),
                jump10(syms["fx_find_t3_led"] | 1), "mfx_led_lookup")

    # 5. Install all 6 hooks (verified stock/SP-ETC bytes -> Thumb-bit jump10)
    if mode == "doom":
        # Chain audio hook inside bnc_tx @ 0x83FE8010 (replaces blx 0x83FE8141)
        patch_exact(image, rows, 0x83FE8010,
                    bytes.fromhex("48f2411cc8f2fe3ce047"),
                    jump10(syms["fx_audio_hook_doom"] | 1), "audio_hook_doom")
        # Chain DT1 hook at 0x800D8118 (currently jumps to fde_dt1 @ 0x83FCA5CB)
        patch_exact(image, rows, 0x800D8118,
                    bytes.fromhex("4af2cb5cc8f2fc3c6047"),
                    jump10(syms["fx_dt1_hook_doom"] | 1), "dt1_hook_doom")
    else:
        # Stock Suite: hook 0x0001D090 directly & stock 0x800D8118 directly
        patch_exact(image, rows, 0x0001D090,
                    bytes.fromhex("4ff480512846fff71bf9"),
                    jump10(syms["fx_audio_hook_stock"] | 1), "audio_hook_stock")
        patch_exact(image, rows, 0x800D8118,
                    bytes.fromhex("2de9f04f83b04ef2c819"),
                    jump10(syms["fx_dt1_hook_stock"] | 1), "dt1_hook_stock")

    # SetParam CTRL hook @ 0x80018A98 (10 B)
    patch_exact(image, rows, 0x80018A98,
                bytes.fromhex("2de9f047a1f27f260d46"),
                jump10(syms["fx_set_ctrl_hook"] | 1), "set_ctrl_hook")

    # Dirty-flag byte write hooks @ 0x800DE800 (10 B) and 0x800DEB40 (12 B = jump10 + nop)
    patch_exact(image, rows, 0x800DE800,
                bytes.fromhex("3120012107fb00a02044"),
                jump10(syms["fx_dirty_hook_1"] | 1), "dirty_hook_1")
    patch_exact(image, rows, 0x800DEB40,
                bytes.fromhex("3120019907fb00a0b8f1000f"),
                jump10(syms["fx_dirty_hook_2"] | 1) + bytes.fromhex("00bf"), "dirty_hook_2")

    # SYS_GET hook @ 0x800DA17C (10 B)
    patch_exact(image, rows, 0x800DA17C,
                bytes.fromhex("a7f27523102b00f2aa81"),
                jump10(syms["fx_get_param_hook"] | 1), "get_param_hook")

    # OLED value formatter hook @ 0x800D595A (10 B)
    patch_exact(image, rows, 0x800D595A,
                bytes.fromhex("a7f27322112a00f2a680"),
                jump10(syms["fx_fmt_param_hook"] | 1), "fmt_param_hook")

    # ========================================================================
    # POST-PATCH VALIDATION (Rule 4 & Rule 6)
    # ========================================================================
    rt_out = Runtime(bytes(image))
    sdram_out = rt_out.read(0x80000000, 0x1A0000)

    # Confirm 0 stale references to any old table base
    stale = scan_movw_movt(sdram_out, 0x80000000, [a for a, _ in OLD_TABLES.values()])
    for old_addr, sites in stale.items():
        if sites:
            raise ValueError(f"VALIDATION FAILED: {len(sites)} stale refs to {old_addr:#x} remain!")

    # Confirm exact new-base site counts
    new_hits = scan_movw_movt(sdram_out, 0x80000000, [new_addrs[k] for k in OLD_TABLES])
    for name, (_, expected_cnt) in OLD_TABLES.items():
        na = new_addrs[name]
        got = len(new_hits[na])
        if got != expected_cnt:
            raise ValueError(f"VALIDATION FAILED: {name} ({na:#x}) count {got} != {expected_cnt}")

    if mode == "doom":
        fde_out = rt_out.read(FDE_BASE, 0x2700)
        stale_fde = scan_movw_movt(fde_out, FDE_BASE, [a for a, _ in FDE_TABLES.values()])
        for old_addr, sites in stale_fde.items():
            if sites:
                raise ValueError(f"VALIDATION FAILED: {len(sites)} stale fde refs to {old_addr:#x}!")

    # Capstone verify every patched bound, Page-4 patch & jump
    for addr, exp_mnem, exp_op in [
        (0x80145E38, "cmp", "r2, #0x35"),
        (0x80145F58, "cmp", "r0, #0x35"),
        (0x801472AA, "cmp", "r5, #0x35"),
        (0x80147A58, "movs", "r0, #0x35"),
        (0x801348C2, "cmp", "r6, #0x36"),
        (0x80146138, "movs", "r3, #4"),
        (0x80146240, "cmp", "r3, #4"),
        (0x8014690C, "cmp", "r0, #4"),
    ]:
        ins = next(MD.disasm(rt_out.read(addr, 2), addr))
        if (ins.mnemonic, ins.op_str) != (exp_mnem, exp_op):
            raise ValueError(f"Capstone mismatch @ {addr:#x}: got '{ins.mnemonic} {ins.op_str}'")

    for addr, sym_name in [
        (0x83FE8010 if mode == "doom" else 0x0001D090, "fx_audio_hook_doom" if mode == "doom" else "fx_audio_hook_stock"),
        (0x800D8118, "fx_dt1_hook_doom" if mode == "doom" else "fx_dt1_hook_stock"),
        (0x80018A98, "fx_set_ctrl_hook"),
        (0x800DE800, "fx_dirty_hook_1"),
        (0x800DEB40, "fx_dirty_hook_2"),
        (0x800DA17C, "fx_get_param_hook"),
        (0x800D595A, "fx_fmt_param_hook"),
        (0x8014615A, "fx_page4_title_hook"),
        (0x80146920, "fx_page4_keys_hook"),
    ]:
        insns = list(MD.disasm(rt_out.read(addr, 10), addr))
        if [i.mnemonic for i in insns] != ["movw", "movt", "bx"] or insns[2].op_str != "ip":
            raise ValueError(f"Capstone jump mismatch @ {addr:#x} ({sym_name}): {[(i.mnemonic, i.op_str) for i in insns]}")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(image)
    out_sha = hashlib.sha256(image).hexdigest()
    print(f"VALIDATION PASSED! Wrote {out_path} ({len(image):,} B, slack={0x37F000 - len(image):,} B, sha256={out_sha[:16]}...)")
    return syms, new_addrs


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("--mode", choices=("doom", "stock"), default="doom")
    p.add_argument("--zig", type=Path, default=Path(r"C:\Users\austi\OneDrive\Documents\ChatGPT\uUHHH\sp404-research\tools\zig-windows-x86_64-0.13.0\zig.exe"))
    p.add_argument("--passthrough-only", action="store_true")
    p.add_argument("--elf-out", type=Path)
    args = p.parse_args()
    build_new_fx(args.input, args.output, args.zig, mode=args.mode,
                 passthrough_only=args.passthrough_only, elf_out=args.elf_out)


if __name__ == "__main__":
    main()
