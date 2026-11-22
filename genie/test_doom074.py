"""Smoke-test the 651C integration's shared APP1 hooks in emulation."""

import argparse
from pathlib import Path
import struct

from build_doom074 import FILTER_SLOTS, PADCONF_LOAD
from build_arp import assemble, load
from test_sg_subpad import EVENT, Machine, STACK
from unicorn import UC_HOOK_CODE, arm_const as arm


def test_shared_hooks(image):
    machine = Machine(image)

    # The 651C filter in the non-DOOM mode takes its stock fallback. Verify
    # that both installed SP-ETC filters still reach it with intact arguments.
    page = 0x30088000
    machine.w32(0x202BBBFC, 1)
    machine.w32(0x8020979C, 2)
    machine.uc.mem_write(EVENT, struct.pack("<6h", 0, 0, 0, 0, 0, 0))
    calls = []

    def stock_filter(uc, *_):
        calls.append((uc.reg_read(arm.UC_ARM_REG_R0), uc.reg_read(arm.UC_ARM_REG_R1)))
        machine._ret(37)

    hook = machine.uc.hook_add(UC_HOOK_CODE, stock_filter, begin=0x800A80A8, end=0x800A80A8)
    for slot in FILTER_SLOTS:
        function = struct.unpack("<I", machine.uc.mem_read(slot, 4))[0]
        assert machine.call(function, page, EVENT) == 37
    assert calls == [(page, EVENT), (page, EVENT)]
    machine.uc.hook_del(hook)

    # On the matching project the same filter forwards to DOOM OS's handler.
    machine.w32(0x8020979C, 1)
    calls.clear()
    machine.uc.hook_add(UC_HOOK_CODE, stock_filter, begin=0x60330594, end=0x60330594)
    for slot in FILTER_SLOTS:
        function = struct.unpack("<I", machine.uc.mem_read(slot, 4))[0]
        assert machine.call(function, page, EVENT) == 37
    assert calls == [(page, EVENT), (page, EVENT)]

    # The two loaders share one stock entry. Enter through the installed
    # SP-ETC wrapper and observe the actual 651C XIP loader after its veneer.
    entry = struct.unpack("<I", machine.uc.mem_read(0x802098A4, 4))[0] & ~1
    calls.clear()

    def doom_load(uc, *_):
        calls.append((uc.reg_read(arm.UC_ARM_REG_R0), uc.reg_read(arm.UC_ARM_REG_R1),
                      uc.reg_read(arm.UC_ARM_REG_SP)))
        machine._ret(23)

    machine.uc.hook_add(UC_HOOK_CODE, doom_load, begin=entry, end=entry)
    assert machine.call(PADCONF_LOAD, 0x82E2CD08, 0) == 23
    assert calls == [(0x82E2CD08, 0, STACK - 0x40)]
    print("Both SP-ETC page filters and the PADCONF loader hand off to 651C")


def test_audio_chain(image):
    machine = Machine(image)
    render_entry = 0x1C758
    mixer_copy = 0x32800000
    machine.w32(0x80202728, mixer_copy)
    machine.uc.hook_add(UC_HOOK_CODE, lambda uc, *_: machine._ret(0x32801000),
                        begin=0x60336168, end=0x60336168)
    # Model only the interrupt-mask instructions in the M-profile installer.
    for address, instruction in ((0x6031961E, b"\x72\xb6"), (0x603196AE, b"\x62\xb6")):
        assert bytes(machine.uc.mem_read(address, 2)) == instruction
        machine.uc.mem_write(address, b"\x00\xbf")
    assert machine.call(0x603195D1) == 1
    expected = bytes.fromhex("2846") + assemble(load("r12", mixer_copy + 0x1C9) + "blx r12\nnop\nnop\nnop", render_entry + 2)
    assert bytes(machine.uc.mem_read(render_entry, len(expected))) == expected

    # The native mixer's displaced call must enter the SP-ETC render bridge.
    calls = []
    def render(uc, *_):
        calls.append(tuple(uc.reg_read(reg) for reg in (
            arm.UC_ARM_REG_R0, arm.UC_ARM_REG_R1, arm.UC_ARM_REG_R2, arm.UC_ARM_REG_R3)))
        machine._ret()
    machine.uc.hook_add(UC_HOOK_CODE, render, begin=0x1FD80, end=0x1FD80)
    machine.call(0x6033E38D, 0x80249800, 0x30088000, 64, 14)
    assert calls == [(0x80249800, 0x30088000, 64, 14)]
    print("651C first-entry installer accepts the SP-ETC render hook and chains back through it")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    image = parser.parse_args().image.read_bytes()
    test_shared_hooks(image)
    test_audio_chain(image)
