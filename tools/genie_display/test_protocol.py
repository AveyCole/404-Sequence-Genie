"""Round-trip frames through the firmware encoder and Windows app parser."""
import argparse
import ctypes as c
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from genie_display import (FLAG_PAGE_MAJOR, FRAME_BYTES, KEY_DOWN, KEY_UP, PAD_DOWN, PAD_UP,
                           PAD_VELOCITY, VALUE_TURN, ScreenReceiver, input_packet,
                           unpack_page_major)


def pack_page_major(row_major: bytes) -> bytes:
    out = bytearray(FRAME_BYTES)
    for y in range(64):
        for x in range(128):
            if row_major[y * 16 + (x >> 3)] & (128 >> (x & 7)):
                out[(y >> 3) * 128 + x] |= 1 << (y & 7)
    return bytes(out)


def check(encoder):
    encoder.screen_link_message.argtypes = [c.c_void_p, c.c_void_p, c.c_uint, c.c_uint]
    encoder.screen_link_frame_message.argtypes = [c.c_void_p, c.c_void_p, c.c_uint, c.c_uint]
    receiver = ScreenReceiver()
    # Verify legacy v1 tiled packets.
    for serial in (0, 127, 128):
        expected = bytes((i * 109 + i // 19 + serial) & 255 for i in range(FRAME_BYTES))
        source = c.create_string_buffer(expected)
        for tile in range(16):
            message = c.create_string_buffer(84)
            length = encoder.screen_link_message(message, source, serial, tile)
            assert length == 84
            wire = message.raw
            broken = wire[:20] + bytes((wire[20] ^ 1,)) + wire[21:]
            assert receiver.feed(broken) is None
            assert receiver.feed(wire[:29]) is None
            assert receiver.feed(b"\xf8" + wire[29:]) == (expected if tile == 15 else None)

    # Verify v2 single-transfer full-frame packets (both row-major and SSD1309 page-major).
    for serial in (0, 63, 127, 128):
        expected = bytes((i * 73 + i // 11 + serial) & 255 for i in range(FRAME_BYTES))
        for flags, raw in ((0, expected), (FLAG_PAGE_MAJOR, pack_page_major(expected))):
            source = c.create_string_buffer(raw)
            message = c.create_string_buffer(1180)
            length = encoder.screen_link_frame_message(message, source, serial, flags)
            assert length == 1180
            wire = message.raw
            broken = wire[:400] + bytes((wire[400] ^ 1,)) + wire[401:]
            assert receiver.feed(broken) is None
            assert receiver.feed(wire[:512]) is None
            assert receiver.feed(b"\xf8" + wire[512:]) == expected
        assert unpack_page_major(pack_page_major(expected)) == expected


def check_input(codec):
    """Viewer input packets through the firmware's CDC OUT parser."""
    emit_type = c.CFUNCTYPE(None, c.c_uint, c.c_uint, c.c_uint)
    codec.screen_link_input.argtypes = [c.c_char_p, c.c_uint, emit_type]
    codec.screen_link_input.restype = c.c_uint
    seen = []
    emit = emit_type(lambda command, a, b: seen.append((command, a, b)))

    def parse(data):
        seen.clear()
        return codec.screen_link_input(data, len(data), emit), list(seen)

    events = [(KEY_DOWN, 0x2A, 0), (PAD_DOWN, 12, PAD_VELOCITY), (PAD_UP, 12, 0),
              (KEY_UP, 0x2A, 0), (VALUE_TURN, 65, 0), (VALUE_TURN, 1, 0), (KEY_DOWN, 0x54, 0)]
    for event in events:
        packet = input_packet(*event)
        assert len(packet) == 10 and all(b < 128 for b in packet[1:-1])
        assert parse(packet) == (1, [event])
    # Several packets in one USB transfer.
    batch = b"".join(input_packet(*event) for event in events)
    assert parse(batch) == (len(events), events)
    # Anything that is not purely valid input goes to the stock handler untouched.
    good = input_packet(KEY_DOWN, 0x22)
    rejects = [
        b"", good[:9], good + b"\x00", good[:7] + bytes((good[7] ^ 1,)) + good[8:],
        b"\xf0\x7dGN\x02" + good[5:], good[:9] + b"\xf6",
        input_packet(KEY_DOWN, 0x55), input_packet(KEY_UP, 3, 1), input_packet(PAD_DOWN, 16, 100),
        input_packet(PAD_DOWN, 3, 0), input_packet(PAD_UP, 3, 9), input_packet(VALUE_TURN, 64),
        input_packet(VALUE_TURN, 0), input_packet(9, 1), batch + b"\x01",
        good + good[:5] + b"\x7f" + good[6:],
    ]
    for data in rejects:
        assert parse(data) == (0, []), data


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", default="zig")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2] / "genie" / "screen_link.c"
    with TemporaryDirectory() as temp:
        dll = Path(temp) / "screen-link.dll"
        subprocess.run([args.zig, "cc", "-shared", str(source), "-o", str(dll)], check=True)
        codec = c.CDLL(str(dll))
        check(codec)
        check_input(codec)
        c.windll.kernel32.FreeLibrary(c.c_void_p(codec._handle))
    print("Firmware v1 tiled and v2 full-frame (row-major + SSD1309 page-major) USB serial packets round-trip cleanly.")
    print("Viewer input packets parse in the firmware; malformed, out-of-range and mixed transfers are left to stock.")
