"""Build the exact 651C + SP-ETC v9.53 + Sequence Genie integration candidate."""

import argparse
import base64
import json
from pathlib import Path
import re
import struct
import sys
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "synth"))
import build
from build_arp import FLASH, assemble, jump
from build_genie import ROOT as GENIE_ROOT, build as build_genie
from elftools.elf.elffile import ELFFile

DOOM_074 = "aaec78ada566ba8aa59602694984ca6fbb229baa1dbcaefac813430da49bbada"
ETC_953 = "faeea5b004273f2dbcfca00387cfcc59078d3ba42fd832d52d363209d8b3b75e"
SETTINGS_START = 0x603FF000
FILTER_SLOTS = (0x80223474, 0x8022CE64)
PADCONF_LOAD = 0x8018AE68
ETC_PADCONF_CONTINUATION = 0x83FD5034


def container(path, name):
    match = re.search(r'window\.' + name + r'\s*=\s*"([A-Za-z0-9+/=]+)"',
                      path.read_text(encoding="utf-8"))
    if not match:
        raise ValueError(f"{name} not found in {path}")
    return base64.b64decode(match[1], validate=True)


def legacy_inputs(stock):
    old_doom = build.apply_container(build.patch_container(), stock)
    etc = build.apply_container(container(ROOT / "kits-data.js", "DOOMOS_KITS_PATCH"), old_doom)
    build.require_hash(etc, ETC_953, "SP-ETC v9.53")
    return old_doom, etc


def legacy_genie(stock):
    old_doom, etc = legacy_inputs(stock)
    patch = json.loads((ROOT / "docs/genie-patch.json").read_text(encoding="utf-8"))
    build.require_hash(etc, patch["input"], "Genie overlay base")
    genie = bytearray(etc)
    genie.extend(bytes(patch["size"] - len(genie)))
    for offset, encoded in patch["segments"]:
        data = bytes.fromhex(encoded)
        if not 0 <= offset <= len(genie) - len(data):
            raise ValueError("Genie overlay segment outside image")
        genie[offset:offset + len(data)] = data
    build.require_hash(genie, patch["output"], "Genie overlay output")
    return old_doom, genie


def merge(stock, doom, prior_doom, genie, instrument_enqueue=None):
    build.require_hash(doom, DOOM_074, "DOOM OS 0.7.4 (651C)")
    old_rows = build.scatter(prior_doom)
    new_rows = build.scatter(doom)
    genie_rows = build.scatter(genie)
    if new_rows != old_rows or len(old_rows) != 12:
        raise ValueError("Unexpected base scatter layout")
    expected_core = [row[:] for row in old_rows]
    # SP-ETC extends the stock zero range for its voice state.
    expected_core[10][2] = 0x325602C
    if genie_rows[:12] != expected_core:
        raise ValueError("Unexpected SP-ETC initialization rows")

    padconf = build.offset(genie, PADCONF_LOAD, 12)
    filters = [build.offset(genie, addr, 4) for addr in FILTER_SLOTS]
    skip = set(range(0x80, 0x88)) | set(range(padconf, padconf + 12))
    skip.update(pos for off in filters for pos in range(off, off + 4))

    out = bytearray(doom)
    for pos, (before, custom) in enumerate(zip(prior_doom, genie)):
        if pos in skip or custom == before:
            continue
        if pos >= len(stock) or doom[pos] not in (before, custom):
            raise ValueError(f"Unresolved shared patch site at file offset {pos:#x}")
        out[pos] = custom

    # Keep 651C's flash-resident code at its linked addresses. Only the
    # file sources change for the copied SP-ETC/Genie RAM modules.
    table_start = struct.unpack_from("<I", genie, 0x80)[0] + 0x80
    if table_start < len(prior_doom):
        raise ValueError("Unexpected SP-ETC payload location")
    out.extend(bytes(-len(out) % 16))
    shift = len(out) - len(prior_doom)
    out.extend(genie[len(prior_doom):table_start])
    rows = expected_core
    for source, dest, size, handler in genie_rows[12:]:
        if handler == build.COPY:
            if not len(prior_doom) <= source - FLASH <= table_start - size:
                raise ValueError(f"Unexpected copy source for {dest:#x}")
            source += shift
        elif handler != build.ZERO or source != 0:
            raise ValueError(f"Unexpected initializer for {dest:#x}")
        rows.append([source, dest, size, handler])

    # The ETC filters run first, then pass unhandled events to the 651C
    # filter (whose normal fallback is the stock shared page handler).
    for pos in filters:
        if struct.unpack_from("<I", doom, pos)[0] != 0x802097CD:
            raise ValueError("Unexpected 651C page filter")
        out[pos:pos + 4] = genie[pos:pos + 4]

    # SP-ETC calls these three ED5E addresses from its copied code.
    for low, high, reg, old, new in (
        (0x2D975C, 0x2D9760, "r1", 0x6031C9A9, 0x800DA599),
        (0x2F47B6, 0x2F47BC, "r2", 0x6031C8F9, 0x802097CD),
        (0x2F5CDE, 0x2F5CE4, "r2", 0x6031C8F9, 0x802097CD),
    ):
        for pos, op, bits in ((low, "movw", 0), (high, "movt", 16)):
            def instruction(target):
                return assemble(f"{op} {reg}, #{(target >> bits) & 0xffff}", 0)
            if genie[pos:pos + 4] != instruction(old):
                raise ValueError(f"Unexpected ED5E reference at {pos:#x}")
            out[pos + shift:pos + shift + 4] = instruction(new)

    # SP-ETC's PADCONF wrapper loads kit state then replays the stock
    # prologue. Tail-call the 651C loader before that prologue instead.
    entry = build.offset(genie, ETC_PADCONF_CONTINUATION, 22)
    if genie[entry:entry + 22] != bytes.fromhex(
        "2de9f04fd5b009914cf231424af6756cc8f2180c6047"
    ):
        raise ValueError("Unexpected SP-ETC PADCONF continuation")
    target = 0x802098A1
    if doom[padconf:padconf + 12] != assemble(jump(target) + "nop", PADCONF_LOAD):
        raise ValueError("Unexpected 651C PADCONF hook")
    out[padconf:padconf + 12] = genie[padconf:padconf + 12]
    entry += shift
    detour = assemble(jump(target) + "nop\n" * 6, ETC_PADCONF_CONTINUATION)
    if len(detour) != 22:
        raise ValueError("Unexpected PADCONF tail branch size")
    out[entry:entry + 22] = detour

    # 651C installs its instrument mixer into ITCM on first entry. Its
    # guard includes the stock render BL replaced by SP-ETC. Accept that
    # exact replacement and route 651C's displaced render call through it.
    render_site = 0x0001C758
    render_size = 18
    guard = 0x60386CC4 - FLASH
    render_veneer = 0x6033E38C - FLASH
    call_offset = build.offset(out, render_site, render_size)
    expected_guard = build.runtime_bytes(doom, render_site, render_size)
    etc_call = bytes.fromhex("03f00bfb")
    if (out[guard:guard + render_size] != expected_guard
            or out[call_offset:call_offset + render_size] != expected_guard[:14] + etc_call
            or out[render_veneer:render_veneer + 8] != bytes.fromhex("dff800f085cb0100")):
        raise ValueError("Unexpected 651C/SP-ETC instrument mixer handoff")
    out[guard:guard + render_size] = out[call_offset:call_offset + render_size]
    struct.pack_into("<I", out, render_veneer + 4, 0x0001FD81)

    if instrument_enqueue is not None:
        producer = 0x60339D98 - FLASH
        if out[producer:producer + 8] != bytes.fromhex("2de9f84f1c46224b"):
            raise ValueError("Unexpected 651C instrument queue producer")
        # Both producers use the same IRQ-safe implementation.
        out[producer:producer + 8] = bytes.fromhex("dff800f0") + struct.pack("<I", instrument_enqueue)

    out.extend(bytes(-len(out) % 16))
    table = len(out)
    for row in rows:
        out.extend(struct.pack("<4I", *row))
    struct.pack_into("<II", out, 0x80, table - 0x80, len(out) - 0x80)
    if FLASH + len(out) > SETTINGS_START or build.scatter(out) != rows:
        raise ValueError("Combined scatter image exceeds APP1 flash space or failed validation")
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock", type=Path, help="Roland 5.52 APP1")
    parser.add_argument("doom", type=Path, help="APP1 produced by the official 0.7.4 patcher")
    parser.add_argument("output", type=Path, help="New combined APP1 file")
    parser.add_argument("--web-patch", type=Path, help="Optional stock 5.52 browser patch")
    parser.add_argument("--doom-instruments", action="store_true", help="Enable the native DOOM OS instrument source in Genie")
    parser.add_argument("--zig", default="zig")
    args = parser.parse_args()
    stock = args.stock.read_bytes()
    doom = args.doom.read_bytes()
    enqueue = None
    if args.doom_instruments:
        build.require_hash(doom, DOOM_074, "DOOM OS 0.7.4 (651C)")
        prior_doom, etc = legacy_inputs(stock)
        with TemporaryDirectory() as scratch:
            parent = Path(scratch) / "etc.app1"
            candidate = Path(scratch) / "genie.app1"
            parent.write_bytes(etc)
            build_genie(parent, candidate, args.zig, doom_instruments=True)
            genie = candidate.read_bytes()
        with (GENIE_ROOT / "build/genie-doom074.elf").open("rb") as file:
            symbols = ELFFile(file).get_section_by_name(".symtab")
            enqueue = symbols.get_symbol_by_name("genie_doom_enqueue")[0]["st_value"]
    else:
        prior_doom, genie = legacy_genie(stock)
    image = merge(stock, doom, prior_doom, genie, enqueue)
    with args.output.open("xb") as file:
        file.write(image)
    if args.web_patch:
        args.web_patch.write_bytes(build.make_overlay(stock, image, "genie-651c"))
    print(f"{args.output}  {build.sha(image)}")


if __name__ == "__main__":
    main()
