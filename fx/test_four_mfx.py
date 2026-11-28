"""Check the four experimental effects against a matching APP1 and ELF."""
import argparse
import math
import struct
from pathlib import Path

from test_new_fx import (
    BMC, SYSPRM, FxEmu, STACK_TOP, UC_ARM_REG_R3, UC_ARM_REG_R5, UC_ARM_REG_R6,
    UC_ARM_REG_SP, UC_ARM_REG_PC, run_audio_blocks,
)


def test(image, elf, mode):
    emu = FxEmu(image, elf)
    # Stop at the hardware transport after the firmware's DT1 hooks.
    def dt1_transport(uc):
        sp = uc.reg_read(UC_ARM_REG_SP) + 12
        lr = struct.unpack("<I", uc.mem_read(sp + 32, 4))[0]
        uc.reg_write(UC_ARM_REG_SP, sp + 36)
        uc.reg_write(UC_ARM_REG_PC, lr | 1)

    emu.hooks[0x800D8122] = dt1_transport
    def bus_switch(bus, value):
        # Stock parameter 0x276 is indexed by bus at SYSPRM + 0x90.
        emu.w16(SYSPRM + 0x90 + 2 * bus, value)

    bus_switch(1, 1)
    bus_switch(2, 1)
    for pad in range(10):
        emu.uc.reg_write(UC_ARM_REG_R5, 4)
        emu.uc.reg_write(UC_ARM_REG_R6, pad)
        emu.uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
        emu.uc.emu_start(0x801460EB, 0x80146112, count=100)
        assert emu.uc.reg_read(UC_ARM_REG_R3) == 55 + pad

    signal = [int(15000 * math.sin(i * .117)) if i < 4096 else 0
              for i in range(12288)]
    for effect in range(61, 65):
        # Load a different effect on the bus before entering this one.
        emu.w16(SYSPRM + 0xC6, 59)
        emu.call(emu.syms["fx_on_select"], 1, 59)
        run_audio_blocks(emu, [0]*64, [0]*64, mode=mode)
        emu.call(emu.syms["fx_set_ctrl_ext"], 1, effect, 5, 0)
        emu.w16(SYSPRM + 0xC6, effect)
        assert emu.call(emu.syms["fx_on_select"], 1, effect) == 0
        dry, *_ = run_audio_blocks(emu, signal[:128], signal[:128], mode=mode)
        assert dry == signal[:128], effect
        emu.call(emu.syms["fx_set_ctrl_ext"], 1, effect, 5, 100)
        wet, *_ = run_audio_blocks(emu, signal, signal, mode=mode)
        assert wet != signal and max(map(abs, wet)) > 20, effect
        if effect in (62, 63):
            assert max(map(abs, wet[6000:])) > 20, effect
        print(f"{mode}: FX {effect} grid, wet audio and dry mix passed", flush=True)

    for effect in range(49, 65):
        for bus in (1, 2):
            other = 3 - bus
            emu.w16(SYSPRM + 0xC4 + 2 * other, 0)
            emu.call(emu.syms["fx_on_select"], other, 0)
            selected = SYSPRM + 0xC4 + 2 * bus
            emu.w16(selected, effect)
            fx_type = 0x7E + bus
            bus_switch(bus, 1)
            emu.call(0x800D8118, BMC, fx_type, 0, effect)
            src = signal[:256]
            run_audio_blocks(emu, src, src, src, src, mode=mode)
            bus_switch(bus, 0)
            emu.call(0x800D8118, BMC, fx_type, 0, 0)
            out = run_audio_blocks(emu, src, src, src, src, mode=mode)
            assert all(channel == src for channel in out[:4]), (effect, bus, "off")
            bus_switch(bus, 1)
            emu.call(0x800D8118, BMC, fx_type, 0, 0)
            src = signal[:1024]
            out = run_audio_blocks(emu, src, src, src, src, mode=mode)
            assert out[2 * (bus - 1)] != src, (effect, bus, "on")
            emu.w16(selected, 10)
            emu.call(0x800D8118, BMC, fx_type, 0, 10)
            out = run_audio_blocks(emu, src[:64], src[:64], src[:64], src[:64], mode=mode)
            assert all(channel == src[:64] for channel in out[:4]), (effect, bus, "deselect")
        print(f"{mode}: FX {effect} bypass, resume and deselect passed on both buses", flush=True)


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("image", type=Path)
    cli.add_argument("elf", type=Path)
    cli.add_argument("--mode", choices=("doom", "stock"), default="doom")
    args = cli.parse_args()
    test(args.image, args.elf, args.mode)
