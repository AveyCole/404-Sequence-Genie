"""Verify native instrument selection and note events on the 651C integration."""

import argparse
from pathlib import Path
import struct

from build_doom074 import GENIE_ROOT
from elftools.elf.elffile import ELFFile
from test_sg_subpad import KEY_SHIFT, SUB_PAD, Machine
from unicorn import UC_HOOK_CODE, arm_const as arm


def test_instruments(image):
    with (GENIE_ROOT / "build/genie-doom074.elf").open("rb") as elf:
        symbols = ELFFile(elf).get_section_by_name(".symtab")
        release = symbols.get_symbol_by_name("genie_doom_release")[0]["st_value"]
        start = symbols.get_symbol_by_name("genie_doom_start")[0]["st_value"]

    machine = Machine(image)
    # Unicorn runs this harness in Thumb-A mode. Stub the Cortex-M interrupt
    # mask instructions locally; record writes and barriers still execute.
    lock = bytes(machine.uc.mem_read(0x83FD3DF8, 0x208))
    for original in ("eff3108c", "72b6", "8cf31088"):
        offset = lock.index(bytes.fromhex(original))
        machine.uc.mem_write(0x83FD3DF8 + offset, b"\x00\xbf" * (len(original) // 4))
    state = 0x32800000
    ring = state + 0x4E894
    instrument_pad = 19
    instrument_id = 0x01234567
    machine.w32(0x80202720, state)
    machine.w32(0x805923FC, 2)
    machine.w32(state + 0x61D0, 3)
    machine.w32(state + (0x13CDE + instrument_pad) * 4, instrument_id)
    machine.w32(state + 0x34, instrument_id)
    machine.uc.mem_write(state, b"\x01")

    def tap(pad):
        machine.pad(pad)
        machine.pad(pad, 0)

    def word(address):
        return int.from_bytes(machine.uc.mem_read(address, 4), "little")

    def message(index):
        address = ring + 12 + (index % 64) * 32
        return struct.unpack("<IIIiIII", machine.uc.mem_read(address, 28))

    def tick(frame):
        machine.w32(0x8059226C, frame // 2)
        machine.call(0x83FA21B1, 0, 0, 64, 0)

    # Take the full UI path to choose B4 and run a phrase from the audio tick.
    machine.key(KEY_SHIFT, True)
    tap(SUB_PAD)
    tap(15)
    machine.key(KEY_SHIFT, False)
    tick(96000)
    tap(15)
    tick(96000)
    first = message(0)
    assert first[:2] == (1, instrument_pad), (first, word(ring))
    assert 1 <= first[2] <= 127 and -24 <= first[3] <= 24
    assert first[4:] == (0x14, 0, 0)

    # Let the normal audio callback carry note-off at 75% of a 6000-frame step.
    tick(100480)
    assert word(ring) == 1
    tick(100544)
    assert message(1) == (2, instrument_pad, 0, first[3], 0x14, 0, 1)

    # The installed 651C producer still accepts a normal seven-argument event.
    machine.call(0x60336211, 1, instrument_pad, 93, -4)
    assert message(2)[:4] == (1, instrument_pad, 93, -4)

    # A full queue must never overwrite a note. Retain and retry the release.
    assert machine.call(start, 72, 87) == 1
    before = word(ring)
    assert message(before - 1)[0] == 1
    machine.w32(ring + 4, (before - 64) & 0xFFFFFFFF)
    machine.call(release)
    assert word(ring) == before
    machine.w32(ring + 4, before)
    machine.call(release)
    assert word(ring) == before + 1
    assert message(before)[0] == 2

    # Drive 651C through native voice allocation, then stop at the mixer.
    # The emulated project has a synthetic instrument record and no audio buses.
    dispatched = []
    machine.w32(ring + 4, 0)
    machine.w32(ring, 2)
    def observe(uc, *_):
        record = uc.reg_read(arm.UC_ARM_REG_R1)
        dispatched.append(struct.unpack("<IIIiIII", uc.mem_read(record, 28)))

    machine.uc.hook_add(UC_HOOK_CODE, observe, begin=0x60339948, end=0x60339948)
    machine.uc.hook_add(UC_HOOK_CODE, observe, begin=0x60339C34, end=0x60339C34)

    def allocated(uc, *_):
        assert word(state + 0x3200) == 1
        assert word(state + 0x3204) == instrument_pad
        assert word(state + 0x32AC) == first[2]

    machine.uc.hook_add(UC_HOOK_CODE, allocated, begin=0x6033A48E, end=0x6033A48E)

    def end_mix(uc, *_):
        sp = uc.reg_read(arm.UC_ARM_REG_SP) + 0xD4
        pc = word(sp + 32)
        uc.reg_write(arm.UC_ARM_REG_SP, sp + 36)
        uc.reg_write(arm.UC_ARM_REG_PC, pc)

    machine.uc.hook_add(UC_HOOK_CODE, end_mix, begin=0x6033A490, end=0x6033A490)
    machine.call(0x6033A429, 0, 0)
    assert dispatched == [message(0), message(1)]
    print("Genie controls, audio-clock note timing, native queue dispatch and full-queue release retry OK")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path)
    test_instruments(parser.parse_args().firmware.read_bytes())
