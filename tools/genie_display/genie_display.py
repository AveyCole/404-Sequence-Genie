"""Local USB viewer for the SP-404MKII OLED."""
from __future__ import annotations

from collections import deque
from pathlib import Path
import sys
import time
import tkinter as tk
from tkinter import filedialog, ttk

from serial import Serial, SerialException, SerialTimeoutException
from serial.tools.list_ports import comports


FRAME_BYTES = 1024
TILE_BYTES = 64
TILES = FRAME_BYTES // TILE_BYTES
PREFIX_V1 = b"\xf0\x7dGN\x01"
PREFIX_V2 = b"\xf0\x7dGN\x02"
FLAG_PAGE_MAJOR = 1
FLAG_INPUT_READY = 2

# Host -> 404 input (firmware: screen_link_input). Ten bytes each.
INPUT_PREFIX = b"\xf0\x7dGN\x10"
KEY_DOWN, KEY_UP, PAD_DOWN, PAD_UP, VALUE_TURN = 1, 2, 3, 4, 5
PAD_VELOCITY = 100

# Keyboard grid laid out like the 404's pads: the top row is pads 13-16.
PAD_KEYS = {key: pad for pad, key in enumerate("zxcvasdfqwer1234")}
# Tk keysym -> stock key number (the held-key bitmap index).
BUTTON_KEYS = {
    "Shift_L": 0x2A, "Shift_R": 0x2A,     # SHIFT
    "Escape": 0x22,                       # EXIT
    "Return": 0x31, "KP_Enter": 0x31,     # VALUE push
    "Delete": 0x1A, "BackSpace": 0x1A,    # DEL
    "Tab": 0x13,                          # SUB PAD
    "h": 0x11,                            # HOLD
    "m": 0x19,                            # MARK
    "o": 0x21,                            # ROLL
    "y": 0x24,                            # REMAIN
    "p": 0x15,                            # PATTERN EDIT
    "F1": 0x2B, "F2": 0x2C, "F3": 0x2D,   # FX buttons FILTER+DRIVE .. MFX
    "F4": 0x2E, "F5": 0x2F, "F6": 0x30,
    "F7": 0x1B,                           # REC
    "F8": 0x1C,                           # RESAMPLE
    "F9": 0x16,                           # RECORD SETTING
    "F10": 0x12,                          # EXT SOURCE
}
VALUE_KEYS = {"Up": 1, "Right": 1, "Down": -1, "Left": -1}
SHIFTED_NUMBERS = {
    "!": "1", "@": "2", "#": "3", "$": "4",
    "exclam": "1", "at": "2", "numbersign": "3", "dollar": "4",
}
UI_FONT = "Segoe UI" if sys.platform == "win32" else ("Helvetica Neue" if sys.platform == "darwin" else "TkDefaultFont")
CONTROLS_HELP = ("Pads 1234/qwer/asdf/zxcv · Shift SHIFT · Esc EXIT · Enter VALUE push · "
                 "wheel/arrows VALUE · Del DEL · Tab SUB PAD · H HOLD · M MARK · O ROLL · "
                 "Y REMAIN · P PTN EDIT · F1-F6 FX · F7 REC · F8 RESAMPLE · F9 REC SET · F10 EXT SRC")


def input_packet(command: int, a: int, b: int = 0) -> bytes:
    body = bytes((0x7D, ord("G"), ord("N"), 0x10, command, a, b))
    return b"\xf0" + body + bytes(((-sum(body)) & 127,)) + b"\xf7"
V1_BODY_LEN = 5 + 3 + 74 + 1
V2_BODY_LEN = 5 + 2 + 1171 + 1

_ON, _OFF = b"\xc6\xf8\xf2", b"\x06\x0d\x12"
_ROW_LUT = tuple(
    b"".join(_ON if byte & (128 >> bit) else _OFF for bit in range(8))
    for byte in range(256)
)


def decode_seven_bit(encoded: bytes | bytearray, expected: int) -> bytes | None:
    out = bytearray()
    for i in range(0, len(encoded), 8):
        mask = encoded[i]
        out.extend(
            byte | (((mask >> bit) & 1) << 7)
            for bit, byte in enumerate(encoded[i + 1:i + 8])
        )
    return bytes(out) if len(out) == expected else None


def unpack_page_major(raw: bytes | bytearray) -> bytes:
    """Convert 128x64 SSD1309 page-major bytes (8 pages x 128 cols) to row-major MSB-first."""
    out = bytearray(FRAME_BYTES)
    for page in range(8):
        base = page * 128
        row_base = page * 8 * 16
        for bx in range(16):
            c0, c1, c2, c3, c4, c5, c6, c7 = raw[base + bx * 8:base + bx * 8 + 8]
            for bit in range(8):
                out[row_base + bit * 16 + bx] = (
                    (((c0 >> bit) & 1) << 7)
                    | (((c1 >> bit) & 1) << 6)
                    | (((c2 >> bit) & 1) << 5)
                    | (((c3 >> bit) & 1) << 4)
                    | (((c4 >> bit) & 1) << 3)
                    | (((c5 >> bit) & 1) << 2)
                    | (((c6 >> bit) & 1) << 1)
                    | ((c7 >> bit) & 1)
                )
    return bytes(out)


class ScreenReceiver:
    def __init__(self):
        self.message = bytearray()
        self.frame = bytearray(FRAME_BYTES)
        self.serial = None
        self.tiles = 0
        self.begun = 0.0
        self.flags = 0

    def feed(self, data):
        newest = None
        for value in data:
            if value == 0xF0:
                self.message[:] = [value]
            elif value >= 0xF8:
                continue
            elif self.message and value == 0xF7:
                newest = self.receive(self.message) or newest
                self.message.clear()
            elif self.message and value < 128 and len(self.message) < V2_BODY_LEN + 8:
                self.message.append(value)
            else:
                self.message.clear()
        return newest

    def receive(self, message):
        if len(message) == V2_BODY_LEN and message.startswith(PREFIX_V2):
            if sum(message[1:]) & 127:
                return None
            flags = message[5]
            raw = decode_seven_bit(message[7:-1], FRAME_BYTES)
            if raw is None:
                return None
            self.flags = flags
            return unpack_page_major(raw) if (flags & FLAG_PAGE_MAJOR) else raw

        # Legacy v1 tiled packets (64 bytes per tile x 16 tiles).
        if len(message) != V1_BODY_LEN or not message.startswith(PREFIX_V1):
            return None
        serial, tile, count = message[5:8]
        if count != TILES or tile >= TILES or sum(message[1:]) & 127:
            return None
        if tile == 0:
            self.serial = serial
            self.begun = time.monotonic()
            self.tiles = 0
        if serial != self.serial or time.monotonic() - self.begun > 2:
            return None
        out = decode_seven_bit(message[8:-1], TILE_BYTES)
        if out is None:
            return None
        start = tile * TILE_BYTES
        self.frame[start:start + TILE_BYTES] = out
        self.tiles |= 1 << tile
        if self.tiles == (1 << TILES) - 1:
            self.serial = None
            return bytes(self.frame)
        return None


class UsbInput:
    def __init__(self):
        self.connection = None

    @staticmethod
    def ports():
        found = [port for port in comports() if (port.vid, port.pid) == (0x0582, 0x02E7)]
        if sys.platform == "darwin":
            cu = [port for port in found if port.device.startswith("/dev/cu.")]
            if cu:
                found = cu
        return [(port.device, f"SP-404MKII ({port.device})") for port in found]

    def open(self, port):
        self.close()
        self.connection = Serial(port, 921600, timeout=0, write_timeout=0.25)

    def write(self, data):
        if self.connection:
            self.connection.write(data)

    def read(self):
        if self.connection:
            waiting = self.connection.in_waiting
            if waiting:
                data = self.connection.read(min(65536, waiting))
                if data:
                    yield data

    def close(self):
        if self.connection:
            self.connection.close()
            self.connection = None


class Viewer:
    def __init__(self, root):
        self.root = root
        self.usb = UsbInput()
        self.receiver = ScreenReceiver()
        self.frame = None
        self.last_frame = None
        self.frame_times = deque(maxlen=30)
        self.ports = []
        self.fullscreen = False
        self.image = None
        self.image_item = None
        self.placeholder_item = None
        self.held = {}
        self.control = tk.BooleanVar(value=True)
        root.title("Screen Genie")
        root.geometry("840x520")
        root.minsize(400, 260)
        root.configure(bg="#0d131b")
        if sys.platform == "win32":
            icon = Path(getattr(sys, "_MEIPASS", Path(__file__).parent)) / "genie-display.ico"
            if icon.is_file():
                root.iconbitmap(str(icon))
        self.toolbar = ttk.Frame(root, padding=10)
        self.toolbar.pack(fill=tk.X)
        ttk.Label(self.toolbar, text="SP-404MKII USB").pack(side=tk.LEFT, padx=(0, 8))
        self.picker = ttk.Combobox(self.toolbar, state="readonly", width=28, takefocus=0)
        self.picker.pack(side=tk.LEFT)
        for label, command in (("Refresh", self.refresh), ("Connect", self.open),
                               ("Load frame", self.load_frame), ("Full screen", self.flip)):
            ttk.Button(self.toolbar, text=label, command=command, takefocus=0).pack(side=tk.LEFT, padx=(5, 0))
        ttk.Checkbutton(self.toolbar, text="Control 404", variable=self.control,
                        command=self.control_changed, takefocus=0).pack(side=tk.LEFT, padx=(12, 0))
        self.status = ttk.Label(root, text="", padding=(12, 7))
        self.status.pack(side=tk.BOTTOM, fill=tk.X)
        self.help = ttk.Label(root, text=CONTROLS_HELP, padding=(12, 0), foreground="#7d8f99",
                              wraplength=820, font=(UI_FONT, 9 if sys.platform == "darwin" else 8))
        self.help.pack(side=tk.BOTTOM, fill=tk.X)
        self.canvas = tk.Canvas(root, bg="#0d131b", highlightthickness=0, takefocus=1)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self.canvas.bind("<Configure>", self.draw)
        self.canvas.bind("<Button-1>", lambda _: self.canvas.focus_set())
        self.canvas.bind("<MouseWheel>", self.wheel)
        root.bind("<F11>", self.flip)
        root.bind("<KeyPress>", self.key_down)
        root.bind("<KeyRelease>", self.key_up)
        root.bind("<FocusOut>", self.focus_out)
        root.protocol("WM_DELETE_WINDOW", self.close)
        if sys.platform == "darwin":
            root.createcommand("tk::mac::Quit", self.close)
        self.refresh()
        self.poll()

    def refresh(self):
        self.ports = self.usb.ports()
        self.picker["values"] = [name for _, name in self.ports]
        if self.ports:
            self.picker.current(0)
            self.status["text"] = "Choose the SP-404MKII and connect."
        else:
            self.status["text"] = "No SP-404MKII detected. Connect its USB data cable and click Refresh."

    def open(self):
        choice = self.picker.current()
        if not 0 <= choice < len(self.ports):
            return
        try:
            self.release_all()
            self.usb.open(self.ports[choice][0])
            self.canvas.focus_set()
            self.receiver = ScreenReceiver()
            self.frame_times.clear()
            self.last_frame = None
            self.frame = None
            self.draw()
            self.status["text"] = "Connected. Waiting for SP-404MKII screen frames."
        except OSError as error:
            self.status["text"] = f"Could not open the 404. Close Roland’s app if it owns the port. {error}"

    def poll(self):
        try:
            latest = None
            for data in self.usb.read():
                latest = self.receiver.feed(data) or latest
            if latest:
                now = time.monotonic()
                self.frame = latest
                self.last_frame = now
                self.frame_times.append(now)
                if len(self.frame_times) >= 2:
                    span = self.frame_times[-1] - self.frame_times[0]
                    fps = (len(self.frame_times) - 1) / span if span > 0 else 0.0
                    text = f"Live · 128 × 64 · {fps:.0f} fps"
                else:
                    text = "Live · 128 × 64"
                self.status["text"] = text + " · " + self.input_state()
                self.draw()
            elif self.last_frame and time.monotonic() - self.last_frame > 2:
                self.status["text"] = "Waiting for the next frame. Showing the last screen."
        except OSError as error:
            self.held.clear()
            self.usb.close()
            self.status["text"] = str(error)
        self.root.after(8, self.poll)

    # ---- input: keyboard and wheel -> 404 key, pad and VALUE events ----

    def input_ready(self):
        return bool(self.usb.connection and self.receiver.flags & FLAG_INPUT_READY)

    def input_state(self):
        if not self.control.get():
            return "view only"
        if self.input_ready():
            return "controls on (click the screen, then type)"
        return "controls unavailable: this firmware has no Screen Genie input"

    def can_control(self):
        return self.control.get() and self.input_ready()

    def send(self, command, a, b=0):
        try:
            self.usb.write(input_packet(command, a, b))
            return True
        except (SerialException, SerialTimeoutException, OSError) as error:
            self.held.clear()
            self.status["text"] = f"The 404 did not take the input: {error}"
            return False

    @staticmethod
    def resolve(event):
        if sys.platform == "win32":
            code = event.keycode
            char = chr(code).lower() if 0x30 <= code <= 0x5A else event.keysym.lower()
        else:
            char = SHIFTED_NUMBERS.get(event.keysym, SHIFTED_NUMBERS.get(event.char, event.keysym.lower()))
        if char in PAD_KEYS:
            return PAD_DOWN, PAD_UP, PAD_KEYS[char]
        number = BUTTON_KEYS.get(event.keysym, BUTTON_KEYS.get(char))
        if number is not None:
            return KEY_DOWN, KEY_UP, number
        return None

    def key_down(self, event):
        if not self.can_control():
            if event.keysym == "Escape" and self.fullscreen:
                self.flip()
            return None
        if event.keysym in VALUE_KEYS:
            self.send(VALUE_TURN, 64 + VALUE_KEYS[event.keysym])
            return "break"
        target = self.resolve(event)
        if target is None:
            return None
        down, up, number = target
        key_id = event.keycode or event.keysym.lower()
        if key_id not in self.held:  # ignore key auto-repeat
            if self.send(down, number, PAD_VELOCITY if down == PAD_DOWN else 0):
                self.held[key_id] = (up, number)
        return "break"

    def key_up(self, event):
        key_id = event.keycode or event.keysym.lower()
        held = self.held.pop(key_id, None)
        if held:
            self.send(*held)
            return "break"
        return None

    def release_all(self):
        held, self.held = self.held, {}
        for up, number in held.values():
            self.send(up, number)

    def focus_out(self, _event=None):
        # A key released while another window has focus never reaches us.
        self.root.after(30, lambda: self.release_all() if self.root.focus_get() is None else None)

    def control_changed(self):
        if not self.control.get():
            self.release_all()
        else:
            self.canvas.focus_set()

    def wheel(self, event):
        if not self.can_control():
            return
        if sys.platform == "darwin":
            steps = max(-63, min(63, int(event.delta)))
        else:
            steps = max(-63, min(63, round(event.delta / 120)))
        if steps:
            self.send(VALUE_TURN, 64 + steps)

    def draw(self, _event=None):
        width, height = self.canvas.winfo_width(), self.canvas.winfo_height()
        if not self.frame:
            if self.image_item is not None:
                self.canvas.delete(self.image_item)
                self.image_item = None
            if self.placeholder_item is None:
                self.placeholder_item = self.canvas.create_text(
                    width // 2, height // 2, text="Waiting for SP-404MKII",
                    fill="#c0d5d8", font=(UI_FONT, 20),
                )
            else:
                self.canvas.coords(self.placeholder_item, width // 2, height // 2)
            return
        if self.placeholder_item is not None:
            self.canvas.delete(self.placeholder_item)
            self.placeholder_item = None
        scale = max(1, min(width // 128, height // 64))
        pixels = b"".join(_ROW_LUT[byte] for byte in self.frame)
        self.image = tk.PhotoImage(data=b"P6\n128 64\n255\n" + pixels, format="PPM").zoom(scale)
        if self.image_item is None:
            self.image_item = self.canvas.create_image(width // 2, height // 2, image=self.image)
        else:
            self.canvas.itemconfig(self.image_item, image=self.image)
            self.canvas.coords(self.image_item, width // 2, height // 2)

    def load_frame(self):
        name = filedialog.askopenfilename(filetypes=[("Raw OLED frame", "*.oled"), ("All files", "*.*")])
        if not name:
            return
        try:
            frame = Path(name).read_bytes()
            if len(frame) != FRAME_BYTES:
                raise ValueError("An OLED frame must contain exactly 1024 bytes.")
        except (OSError, ValueError) as error:
            self.status["text"] = str(error)
            return
        self.frame = frame
        self.last_frame = None
        self.status["text"] = f"Frame preview · {Path(name).name}"
        self.draw()

    def flip(self, _event=None):
        self.fullscreen = not self.fullscreen
        self.root.attributes("-fullscreen", self.fullscreen)

    def close(self):
        self.release_all()
        self.usb.close()
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    Viewer(root)
    root.mainloop()
