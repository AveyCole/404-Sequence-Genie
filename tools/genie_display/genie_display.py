"""Local USB OLED mirror, remote control, accessibility & OBS capture studio for the SP-404MKII."""
from __future__ import annotations

from collections import deque
import json
from pathlib import Path
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
import webbrowser

from serial import Serial, SerialException, SerialTimeoutException
from serial.tools.list_ports import comports

try:
    from PIL import Image, ImageTk
except ImportError:
    Image = None
    ImageTk = None

try:
    from obs_server import ObsServer
    from panel_window import VirtualPanelWindow
except ImportError:
    ObsServer = None
    VirtualPanelWindow = None


FRAME_BYTES = 1024
TILE_BYTES = 64
TILES = FRAME_BYTES // TILE_BYTES
PREFIX_V1 = b"\xf0\x7dGN\x01"
PREFIX_V2 = b"\xf0\x7dGN\x02"
FLAG_PAGE_MAJOR = 1
FLAG_INPUT_READY = 2

# Host -> 404 input (firmware: screen_link_input). Ten bytes each.
INPUT_PREFIX = b"\xf0\x7dGN\x10"
KEY_DOWN, KEY_UP, PAD_DOWN, PAD_UP, VALUE_TURN, LINK_STATE, CTRL_KNOB = 1, 2, 3, 4, 5, 6, 7
PAD_VELOCITY = 100

# Default SP-404MKII top-to-bottom pad grid: 1234 = Pads 1-4 ... zxcv = Pads 13-16
PAD_KEYS = {key: pad for pad, key in enumerate("1234qwerasdfzxcv")}
PAD_PRESETS = {
    "SP-404 Layout (1234 Top)": "1234qwerasdfzxcv",
    "MPC Layout (ZXCV Bottom)": "zxcvasdfqwer1234",
}

# Stock key number -> human display name (for HUD, TTS, and Virtual Panel)
KEY_NAMES = {
    0x10: "BUS FX", 0x11: "HOLD", 0x12: "EXT SRC", 0x13: "SUB PAD",
    0x14: "PTN SEL", 0x15: "PTN EDIT", 0x16: "REC SET", 0x17: "START/END",
    0x18: "PITCH/SPD", 0x19: "MARK", 0x1A: "DEL", 0x1B: "REC",
    0x1C: "RESAMPLE", 0x1D: "BPM SYNC", 0x1E: "GATE", 0x1F: "LOOP",
    0x20: "REVERSE", 0x21: "ROLL", 0x22: "EXIT", 0x23: "COPY",
    0x24: "REMAIN", 0x25: "BANK A/F", 0x26: "BANK B/G", 0x27: "BANK C/H",
    0x28: "BANK D/I", 0x29: "BANK E/J", 0x2A: "SHIFT",
    0x2B: "FILTER+DRIVE", 0x2C: "RESONATOR", 0x2D: "DELAY",
    0x2E: "ISOLATOR", 0x2F: "DJFX LOOPER", 0x30: "MFX", 0x31: "VALUE",
}

# Tk keysym -> stock key number (Full Keyboard default)
BUTTON_KEYS = {
    "Shift_L": 0x2A, "Shift_R": 0x2A,     # SHIFT
    "Escape": 0x22,                       # EXIT
    "Return": 0x31, "KP_Enter": 0x31,     # VALUE push
    "Delete": 0x1A, "BackSpace": 0x1A,    # DEL
    "Tab": 0x13,                          # SUB PAD
    "b": 0x10,                            # BUS FX
    "h": 0x11,                            # HOLD
    "m": 0x19,                            # MARK
    "o": 0x21,                            # ROLL
    "y": 0x24,                            # REMAIN
    "p": 0x15,                            # PATTERN EDIT
    "u": 0x14,                            # PATTERN SELECT
    "t": 0x17,                            # START/END
    "g": 0x18,                            # PITCH/SPEED
    "n": 0x1D,                            # BPM SYNC
    "j": 0x1E,                            # GATE
    "l": 0x1F,                            # LOOP
    "k": 0x20,                            # REVERSE
    "i": 0x23,                            # COPY
    "5": 0x25, "6": 0x26, "7": 0x27, "8": 0x28, "9": 0x29,  # BANKS A/F..E/J
    "F1": 0x2B, "F2": 0x2C, "F3": 0x2D,   # FX buttons FILTER+DRIVE .. MFX
    "F4": 0x2E, "F5": 0x2F, "F6": 0x30,
    "F7": 0x1B,                           # REC
    "F8": 0x1C,                           # RESAMPLE
    "F9": 0x16,                           # RECORD SETTING
    "F10": 0x12,                          # EXT SOURCE
}

LAPTOP_60_BUTTONS = {
    **{k: v for k, v in BUTTON_KEYS.items() if not k.startswith("F")},
    "5": 0x2B, "6": 0x2C, "7": 0x2D, "8": 0x2E, "9": 0x2F, "0": 0x30,  # FX on top number row
    "minus": 0x1B, "equal": 0x1C, "bracketleft": 0x16, "bracketright": 0x12,
}

VALUE_KEYS = {"Up": 1, "Right": 1, "Down": -1, "Left": -1}
SHIFTED_NUMBERS = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
    "exclam": "1", "at": "2", "numbersign": "3", "dollar": "4", "percent": "5",
    "asciicircum": "6", "ampersand": "7", "asterisk": "8", "parenleft": "9", "parenright": "0",
}
UI_FONT = "Segoe UI" if sys.platform == "win32" else ("Helvetica Neue" if sys.platform == "darwin" else "TkDefaultFont")
CONTROLS_HELP = (
    "Pads 1234/QWER/ASDF/ZXCV · Shift SHIFT · Esc EXIT · Enter VALUE · Arrows/Wheel VALUE · "
    "B BUS FX · F1–F6 FX · F12 Virtual 404 Panel & Layout Map"
)

THEMES = {
    "Classic Cyan": ("#c6f8f2", "#060d12"),
    "Crisp White (High Contrast)": ("#ffffff", "#000000"),
    "High-Contrast Yellow": ("#ffe84d", "#000000"),
    "Warm Amber": ("#ffb347", "#0d0904"),
    "Green Phosphor": ("#6dff8a", "#041107"),
    "Inverted (Black on White)": ("#0c1118", "#f7fafc"),
}

CONFIG_PATH = Path.home() / ".screen_genie.json"


def input_packet(command: int, a: int, b: int = 0) -> bytes:
    body = bytes((0x7D, ord("G"), ord("N"), 0x10, command, a & 127, b & 127))
    return b"\xf0" + body + bytes(((-sum(body)) & 127,)) + b"\xf7"


V1_BODY_LEN = 5 + 3 + 74 + 1
V2_BODY_LEN = 5 + 2 + 1171 + 1


def _hex_to_bytes(color: str) -> bytes:
    value = int(color.lstrip("#"), 16)
    return bytes(((value >> 16) & 255, (value >> 8) & 255, value & 255))


def _build_lut(on_hex: str, off_hex: str) -> tuple[bytes, ...]:
    on_b, off_b = _hex_to_bytes(on_hex), _hex_to_bytes(off_hex)
    return tuple(
        b"".join(on_b if byte & (128 >> bit) else off_b for bit in range(8))
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


class SpeechEngine:
    """Non-blocking platform TTS for accessibility announcements (zero external deps)."""

    def __init__(self):
        self._lock = threading.Lock()
        self._proc = None

    def speak(self, text: str):
        if not text:
            return
        threading.Thread(target=self._run, args=(text,), daemon=True).start()

    def _run(self, text: str):
        safe = text.replace("'", "").replace('"', "").replace("\n", " ")[:120]
        try:
            with self._lock:
                if self._proc and self._proc.poll() is None:
                    self._proc.terminate()
                if sys.platform == "win32":
                    cmd = [
                        "powershell", "-NoProfile", "-WindowStyle", "Hidden", "-Command",
                        f"Add-Type -AssemblyName System.Speech; "
                        f"$s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
                        f"$s.Rate = 2; $s.Speak('{safe}')",
                    ]
                    self._proc = subprocess.Popen(
                        cmd, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    )
                elif sys.platform == "darwin":
                    self._proc = subprocess.Popen(
                        ["say", "-r", "220", safe],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    )
        except OSError:
            pass


class CleanCaptureWindow:
    """Borderless/chrome-free integer-scaled OLED pop-out window for OBS Window Capture."""

    def __init__(self, viewer, scale: int = 6):
        self.viewer = viewer
        self.scale = scale
        self.win = tk.Toplevel(viewer.root)
        self.win.title(f"Screen Genie · Clean OLED ({128 * scale}×{64 * scale})")
        self.win.configure(bg=viewer.off_color)
        self.win.geometry(f"{128 * scale}x{64 * scale}")
        self.win.resizable(True, True)
        self.win.protocol("WM_DELETE_WINDOW", self.close)
        self.canvas = tk.Canvas(self.win, bg=viewer.off_color, highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self.canvas.bind("<Configure>", lambda _: self.render())
        self.image = None
        self.item = None
        self.render()

    def render(self):
        if not self.win.winfo_exists():
            return
        self.canvas.configure(bg=self.viewer.off_color)
        frame = self.viewer.frame
        if not frame:
            return
        w = max(128, self.canvas.winfo_width())
        h = max(64, self.canvas.winfo_height())
        scale = max(1, min(w // 128, h // 64))
        self.image = self.viewer.frame_to_photo(frame, scale)
        if self.item is None:
            self.item = self.canvas.create_image(w // 2, h // 2, image=self.image)
        else:
            self.canvas.itemconfig(self.item, image=self.image)
            self.canvas.coords(self.item, w // 2, h // 2)

    def close(self):
        self.viewer.capture_window = None
        if self.win.winfo_exists():
            self.win.destroy()


class Viewer:
    def __init__(self, root):
        self.root = root
        self.usb = UsbInput()
        self.receiver = ScreenReceiver()
        self.speech = SpeechEngine()
        self.obs = ObsServer() if ObsServer else None
        self.panel_window = None
        self.capture_window = None

        self.frame: bytes | None = None
        self.last_frame = None
        self.last_hello = 0.0
        self.frame_times = deque(maxlen=30)
        self.ports = []
        self.fullscreen = False
        self.image = None
        self.image_item = None
        self.placeholder_item = None
        self.hud_bg = None
        self.hud_text = None
        self.hud_until = 0.0
        self.hud_message = ""
        self.held = {}

        # Recording state (GIF)
        self.recording = False
        self.rec_frames: list[tuple[float, bytes]] = []

        # User preferences & keymaps
        self.pad_keys = dict(PAD_KEYS)
        self.button_keys = dict(BUTTON_KEYS)
        self.keymap_name = "SP-404 Layout (1234 Top)"
        self.theme_name = tk.StringVar(value="Classic Cyan")
        self.pixel_grid = tk.BooleanVar(value=False)
        self.show_hud = tk.BooleanVar(value=True)
        self.show_chrome = tk.BooleanVar(value=True)
        self.always_on_top = tk.BooleanVar(value=False)
        self.control = tk.BooleanVar(value=True)
        self.speak_actions = tk.BooleanVar(value=False)
        self.click_sound = tk.BooleanVar(value=False)
        self.obs_running = tk.BooleanVar(value=False)
        self.sticky_vars = {
            0x2A: tk.BooleanVar(value=False),  # SHIFT
            0x24: tk.BooleanVar(value=False),  # REMAIN
            0x30: tk.BooleanVar(value=False),  # MFX
        }

        self._load_config()
        self.on_color, self.off_color = THEMES.get(self.theme_name.get(), THEMES["Classic Cyan"])
        self.row_lut = _build_lut(self.on_color, self.off_color)

        root.title("Screen Genie")
        root.geometry("880x560")
        root.minsize(420, 280)
        root.configure(bg=self.off_color)
        if sys.platform == "win32":
            icon = Path(getattr(sys, "_MEIPASS", Path(__file__).parent)) / "genie-display.ico"
            if icon.is_file():
                root.iconbitmap(str(icon))

        self._build_menu()

        self.toolbar = ttk.Frame(root, padding=(10, 8))
        self.toolbar.pack(fill=tk.X)
        ttk.Label(self.toolbar, text="SP-404MKII USB").pack(side=tk.LEFT, padx=(0, 8))
        self.picker = ttk.Combobox(self.toolbar, state="readonly", width=26, takefocus=0)
        self.picker.pack(side=tk.LEFT)
        for label, command in (
            ("Refresh", self.refresh),
            ("Connect", self.open),
            ("404 Panel", self.toggle_panel_window),
            ("Full screen", self.flip),
        ):
            ttk.Button(self.toolbar, text=label, command=command, takefocus=0).pack(side=tk.LEFT, padx=(5, 0))
        ttk.Checkbutton(self.toolbar, text="Control 404", variable=self.control,
                        command=self.control_changed, takefocus=0).pack(side=tk.LEFT, padx=(10, 0))

        self.status = ttk.Label(root, text="", padding=(12, 6))
        self.status.pack(side=tk.BOTTOM, fill=tk.X)
        self.help = ttk.Label(root, text=CONTROLS_HELP, padding=(12, 0, 12, 2), foreground="#7d8f99",
                              wraplength=850, font=(UI_FONT, 9 if sys.platform == "darwin" else 8))
        self.help.pack(side=tk.BOTTOM, fill=tk.X)

        self.canvas = tk.Canvas(root, bg=self.off_color, highlightthickness=0, takefocus=1)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self.canvas.bind("<Configure>", self.draw)
        self.canvas.bind("<Button-1>", lambda _: self.canvas.focus_set())
        self.canvas.bind("<MouseWheel>", self.wheel)
        root.bind("<F11>", self.flip)
        root.bind("<F12>", lambda _e: self.toggle_panel_window())
        root.bind("<Control-s>", lambda _e: self.save_screenshot())
        root.bind("<Control-r>", lambda _e: self.toggle_gif_recording())
        root.bind("<KeyPress>", self.key_down)
        root.bind("<KeyRelease>", self.key_up)
        root.bind("<FocusOut>", self.focus_out)
        root.protocol("WM_DELETE_WINDOW", self.close)
        if sys.platform == "darwin":
            root.createcommand("tk::mac::Quit", self.close)
            root.bind("<Command-s>", lambda _e: self.save_screenshot())
            root.bind("<Command-r>", lambda _e: self.toggle_gif_recording())

        self._apply_theme()
        self._apply_always_on_top()
        self.refresh()
        if len(self.ports) == 1:
            self.open()
        self.poll()

    # ---- Menu bar ----

    def _build_menu(self):
        menubar = tk.Menu(self.root)
        self.root.config(menu=menubar)

        # File
        file_m = tk.Menu(menubar, tearoff=0)
        file_m.add_command(label="Connect / Reconnect", command=self.open)
        file_m.add_command(label="Disconnect", command=self.disconnect)
        file_m.add_command(label="Refresh USB Ports", command=self.refresh)
        file_m.add_separator()
        file_m.add_command(label="Load Raw Frame (.oled)…", command=self.load_frame)
        file_m.add_separator()
        file_m.add_command(label="Exit", command=self.close)
        menubar.add_cascade(label="File", menu=file_m)

        # View
        view_m = tk.Menu(menubar, tearoff=0)
        theme_m = tk.Menu(view_m, tearoff=0)
        for name in THEMES:
            theme_m.add_radiobutton(label=name, variable=self.theme_name, value=name,
                                    command=self._apply_theme)
        view_m.add_cascade(label="OLED Theme", menu=theme_m)
        view_m.add_checkbutton(label="OLED Pixel Grid", variable=self.pixel_grid, command=self.draw)
        view_m.add_checkbutton(label="Show Button Combo HUD", variable=self.show_hud,
                               command=self._sync_obs_theme)
        view_m.add_checkbutton(label="Show Toolbar & Help Bar", variable=self.show_chrome,
                               command=self._toggle_chrome)
        view_m.add_separator()
        obs_pop_m = tk.Menu(view_m, tearoff=0)
        for scale in (2, 4, 6, 8):
            obs_pop_m.add_command(
                label=f"{128 * scale} × {64 * scale} ({scale}×)",
                command=lambda s=scale: self.open_capture_window(s),
            )
        view_m.add_cascade(label="Pop-Out Clean OLED Window", menu=obs_pop_m)
        view_m.add_checkbutton(label="Always on Top", variable=self.always_on_top,
                               command=self._apply_always_on_top)
        view_m.add_command(label="Full Screen (F11)", command=self.flip)
        menubar.add_cascade(label="View", menu=view_m)

        # Controls
        ctrl_m = tk.Menu(menubar, tearoff=0)
        ctrl_m.add_checkbutton(label="Enable 404 Remote Control", variable=self.control,
                               command=self.control_changed)
        ctrl_m.add_command(label="Virtual 404 Panel & Visual Keymap (F12)",
                           command=self.toggle_panel_window)
        ctrl_m.add_separator()
        ctrl_m.add_checkbutton(label="Sticky SHIFT", variable=self.sticky_vars[0x2A],
                               command=lambda: self.toggle_sticky(0x2A))
        ctrl_m.add_checkbutton(label="Sticky REMAIN", variable=self.sticky_vars[0x24],
                               command=lambda: self.toggle_sticky(0x24))
        ctrl_m.add_checkbutton(label="Sticky MFX", variable=self.sticky_vars[0x30],
                               command=lambda: self.toggle_sticky(0x30))
        ctrl_m.add_separator()
        preset_m = tk.Menu(ctrl_m, tearoff=0)
        preset_m.add_command(label="SP-404 Layout (1234 Top)",
                             command=lambda: self.apply_keymap_preset("SP-404 Layout (1234 Top)"))
        preset_m.add_command(label="MPC Layout (ZXCV Bottom)",
                             command=lambda: self.apply_keymap_preset("MPC Layout (ZXCV Bottom)"))
        preset_m.add_command(label="60% / Laptop (No F-Keys)",
                             command=lambda: self.apply_keymap_preset("60% / Laptop (No F-Keys)"))
        ctrl_m.add_cascade(label="Keymap Preset", menu=preset_m)
        ctrl_m.add_command(label="Customize Keymap…", command=self.open_keymap_editor)
        macro_m = tk.Menu(ctrl_m, tearoff=0)
        for label, mid in (
            ("Open Sequence Genie (SHIFT+SUB PAD)", "open_genie"),
            ("Open Modes Page (Pad 15 ×2)", "modes"),
            ("Toggle Screen Genie (SHIFT+Pad 14)", "toggle_link"),
            ("Bounce / Resample (Pad 11)", "bounce"),
            ("BUS FX", "bus_fx"),
            ("Utility Menu (SHIFT+Pad 13)", "utility"),
        ):
            macro_m.add_command(label=label, command=lambda m=mid: self.run_macro(m))
        ctrl_m.add_cascade(label="Quick Macros", menu=macro_m)
        ctrl_m.add_separator()
        ctrl_m.add_checkbutton(label="Speak Actions (Accessibility TTS)", variable=self.speak_actions,
                               command=self._save_config)
        ctrl_m.add_checkbutton(label="Button Click Sound", variable=self.click_sound,
                               command=self._save_config)
        menubar.add_cascade(label="Controls", menu=ctrl_m)

        # Capture (Creator Tools)
        cap_m = tk.Menu(menubar, tearoff=0)
        cap_m.add_command(label="Save Screenshot (PNG)…  Ctrl+S", command=self.save_screenshot)
        self.rec_menu = cap_m
        self.rec_menu_idx = 1
        cap_m.add_command(label="Start GIF Recording  Ctrl+R", command=self.toggle_gif_recording)
        cap_m.add_separator()
        cap_m.add_checkbutton(label="OBS Browser Source Server (localhost:4040)",
                              variable=self.obs_running, command=self.toggle_obs_server)
        cap_m.add_command(label="Copy OBS Browser Source URL", command=self.copy_obs_url)
        cap_m.add_command(label="Open OBS Preview in Browser", command=self.open_obs_preview)
        menubar.add_cascade(label="Capture", menu=cap_m)

        # Help
        help_m = tk.Menu(menubar, tearoff=0)
        help_m.add_command(label="Visual Layout Overlay (Virtual 404 Panel)  F12",
                           command=self.toggle_panel_window)
        help_m.add_command(label="Keyboard Shortcuts Summary…", command=self.show_shortcuts_help)
        menubar.add_cascade(label="Help", menu=help_m)

    # ---- Config persistence ----

    def _load_config(self):
        if not CONFIG_PATH.is_file():
            return
        try:
            data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            if data.get("theme") in THEMES:
                self.theme_name.set(data["theme"])
            self.pixel_grid.set(bool(data.get("pixel_grid", False)))
            self.show_hud.set(bool(data.get("show_hud", True)))
            self.speak_actions.set(bool(data.get("speak_actions", False)))
            self.click_sound.set(bool(data.get("click_sound", False)))
            if isinstance(data.get("pad_keys"), dict):
                self.pad_keys = {str(k): int(v) for k, v in data["pad_keys"].items()}
            if isinstance(data.get("button_keys"), dict):
                self.button_keys = {str(k): int(v) for k, v in data["button_keys"].items()}
            if isinstance(data.get("keymap_name"), str):
                self.keymap_name = data["keymap_name"]
        except (OSError, ValueError, TypeError):
            pass

    def _save_config(self):
        data = {
            "theme": self.theme_name.get(),
            "pixel_grid": self.pixel_grid.get(),
            "show_hud": self.show_hud.get(),
            "speak_actions": self.speak_actions.get(),
            "click_sound": self.click_sound.get(),
            "keymap_name": self.keymap_name,
            "pad_keys": self.pad_keys,
            "button_keys": self.button_keys,
        }
        try:
            CONFIG_PATH.write_text(json.dumps(data, indent=2), encoding="utf-8")
        except OSError:
            pass

    # ---- Theme / View helpers ----

    def _apply_theme(self):
        self.on_color, self.off_color = THEMES.get(self.theme_name.get(), THEMES["Classic Cyan"])
        self.row_lut = _build_lut(self.on_color, self.off_color)
        self.root.configure(bg=self.off_color)
        self.canvas.configure(bg=self.off_color)
        self._sync_obs_theme()
        self._save_config()
        self.draw()
        if self.capture_window:
            self.capture_window.render()
        if self.panel_window:
            self.panel_window.update_mini_oled()

    def _sync_obs_theme(self):
        self._save_config()
        if self.obs:
            self.obs.update_theme(self.on_color, self.off_color, self.show_hud.get())

    def _toggle_chrome(self):
        if self.show_chrome.get():
            self.toolbar.pack(fill=tk.X, before=self.canvas)
            self.status.pack(side=tk.BOTTOM, fill=tk.X)
            self.help.pack(side=tk.BOTTOM, fill=tk.X)
        else:
            self.toolbar.pack_forget()
            self.status.pack_forget()
            self.help.pack_forget()
        self.draw()

    def _apply_always_on_top(self):
        self.root.attributes("-topmost", self.always_on_top.get())

    def toggle_panel_window(self):
        if VirtualPanelWindow is None:
            messagebox.showinfo("Virtual 404 Panel", "Panel module is unavailable.")
            return
        if self.panel_window and self.panel_window.win.winfo_exists():
            self.panel_window.win.lift()
            self.panel_window.win.focus_force()
            return
        self.panel_window = VirtualPanelWindow(self)

    def open_capture_window(self, scale: int = 6):
        if self.capture_window and self.capture_window.win.winfo_exists():
            self.capture_window.close()
        self.capture_window = CleanCaptureWindow(self, scale)

    # ---- USB Connection & Polling ----

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
            self.refresh()
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
            self.last_hello = time.monotonic()
            self.usb.write(input_packet(LINK_STATE, 1))
            self.status["text"] = "Connected. Activating Screen Genie on SP-404MKII…"
            self._announce("Connected to SP-404MKII")
        except OSError as error:
            self.status["text"] = f"Could not open the 404. Close Roland’s app if it owns the port. {error}"

    def disconnect(self):
        self.release_all()
        if self.usb.connection:
            try:
                self.usb.write(input_packet(LINK_STATE, 0))
            except OSError:
                pass
        self.usb.close()
        self.status["text"] = "Disconnected."
        self._announce("Disconnected")

    def poll(self):
        try:
            latest = None
            for data in self.usb.read():
                latest = self.receiver.feed(data) or latest
            now = time.monotonic()
            if latest:
                self.frame = latest
                self.last_frame = now
                self.frame_times.append(now)
                if self.recording:
                    self.rec_frames.append((now, latest))
                if self.obs and self.obs.running:
                    self.obs.update_frame(latest, self.on_color, self.off_color)
                if len(self.frame_times) >= 2:
                    span = self.frame_times[-1] - self.frame_times[0]
                    fps = (len(self.frame_times) - 1) / span if span > 0 else 0.0
                    text = f"Live · 128 × 64 · {fps:.0f} fps"
                else:
                    text = "Live · 128 × 64"
                if self.recording:
                    text += f" · REC ({len(self.rec_frames)}f)"
                self.status["text"] = text + " · " + self.input_state()
                self.draw()
                if self.capture_window:
                    self.capture_window.render()
                if self.panel_window:
                    self.panel_window.update_mini_oled()
            elif self.last_frame and now - self.last_frame > 2:
                self.status["text"] = "Waiting for the next frame. Showing the last screen."
            elif self.usb.connection and not self.frame and now - self.last_hello >= 0.5:
                self.last_hello = now
                self.usb.write(input_packet(LINK_STATE, 1))
            if self.hud_message and now >= self.hud_until:
                self.hud_message = ""
                self._draw_hud()
        except OSError as error:
            self.held.clear()
            self.usb.close()
            self.status["text"] = str(error)
        self.root.after(8, self.poll)

    # ---- Input, Sticky Modifiers, Macros & Accessibility ----

    def input_ready(self):
        return bool(self.usb.connection and self.receiver.flags & FLAG_INPUT_READY)

    def input_state(self):
        if not self.control.get():
            return "view only"
        if self.input_ready():
            sticky = [KEY_NAMES[c] for c, v in self.sticky_vars.items() if v.get()]
            suffix = f" · LATCHED: {'+'.join(sticky)}" if sticky else ""
            return f"controls on (F12: 404 Panel){suffix}"
        return "controls unavailable: this firmware has no Screen Genie input"

    def can_control(self):
        return self.control.get() and self.input_ready()

    def _announce(self, text: str):
        if self.speak_actions.get():
            self.speech.speak(text)

    def _beep(self):
        if self.click_sound.get():
            try:
                self.root.bell()
            except tk.TclError:
                pass

    def _show_combo_hud(self, label: str):
        sticky = [KEY_NAMES[c] for c, v in self.sticky_vars.items() if v.get() and KEY_NAMES[c] != label]
        text = " + ".join([*sticky, label]) if sticky else label
        self.hud_message = text
        self.hud_until = time.monotonic() + 1.8
        if self.obs and self.obs.running:
            self.obs.set_hud(text)
        self._draw_hud()

    def send(self, command, a, b=0):
        try:
            self.usb.write(input_packet(command, a, b))
            return True
        except (SerialException, SerialTimeoutException, OSError) as error:
            self.held.clear()
            self.status["text"] = f"The 404 did not take the input: {error}"
            return False

    def send_ctrl_knob(self, knob_id: int, value: int):
        if not self.can_control():
            return
        value = max(0, min(127, int(value)))
        if self.send(CTRL_KNOB, knob_id, value):
            self._show_combo_hud(f"CTRL {knob_id}: {value}")

    def send_value_turn(self, steps: int):
        if not self.can_control() or not steps:
            return
        steps = max(-63, min(63, int(steps)))
        if self.send(VALUE_TURN, 64 + steps):
            self._show_combo_hud(f"VALUE {'+' if steps > 0 else ''}{steps}")

    def virtual_pad_down(self, pad: int, label: str = ""):
        if not self.can_control():
            return
        self._beep()
        if self.send(PAD_DOWN, pad, PAD_VELOCITY):
            name = label or f"PAD {pad + 1}"
            self._show_combo_hud(name)
            self._announce(name)

    def virtual_pad_up(self, pad: int):
        if self.usb.connection:
            self.send(PAD_UP, pad, 0)

    def virtual_key_down(self, code: int, label: str = ""):
        if not self.can_control():
            return
        self._beep()
        if self.send(KEY_DOWN, code, 0):
            name = label or KEY_NAMES.get(code, f"KEY {code:#x}")
            self._show_combo_hud(name)
            self._announce(name)

    def virtual_key_up(self, code: int):
        if self.usb.connection and not (code in self.sticky_vars and self.sticky_vars[code].get()):
            self.send(KEY_UP, code, 0)

    def toggle_sticky(self, code: int):
        latched = self.sticky_vars[code].get()
        name = KEY_NAMES.get(code, f"KEY {code:#x}")
        self._beep()
        if self.can_control():
            self.send(KEY_DOWN if latched else KEY_UP, code, 0)
        self._show_combo_hud(f"{name} {'LATCHED' if latched else 'RELEASED'}")
        self._announce(f"{name} {'latched' if latched else 'released'}")
        if self.panel_window:
            self.panel_window.redraw_overlays()

    def run_macro(self, macro_id: str):
        if not self.can_control():
            return
        sequences = {
            "open_genie": [(KEY_DOWN, 0x2A, 0), (KEY_DOWN, 0x13, 0), (KEY_UP, 0x13, 0), (KEY_UP, 0x2A, 0)],
            "modes": [(PAD_DOWN, 14, PAD_VELOCITY), (PAD_UP, 14, 0), (PAD_DOWN, 14, PAD_VELOCITY), (PAD_UP, 14, 0)],
            "toggle_link": [(KEY_DOWN, 0x2A, 0), (PAD_DOWN, 13, PAD_VELOCITY), (PAD_UP, 13, 0), (KEY_UP, 0x2A, 0)],
            "bounce": [(PAD_DOWN, 10, PAD_VELOCITY), (PAD_UP, 10, 0)],
            "bus_fx": [(KEY_DOWN, 0x10, 0), (KEY_UP, 0x10, 0)],
            "utility": [(KEY_DOWN, 0x2A, 0), (PAD_DOWN, 12, PAD_VELOCITY), (PAD_UP, 12, 0), (KEY_UP, 0x2A, 0)],
        }
        steps = sequences.get(macro_id)
        if not steps:
            return
        self._beep()
        self._show_combo_hud(f"MACRO: {macro_id.replace('_', ' ').upper()}")
        self._announce(macro_id.replace("_", " "))
        for idx, (cmd, a, b) in enumerate(steps):
            self.root.after(idx * 45, lambda c=cmd, x=a, y=b: self.send(c, x, y))

    def _resolve_for(self, event, pad_keys, button_keys):
        if sys.platform == "win32":
            code = event.keycode
            char = chr(code).lower() if 0x30 <= code <= 0x5A else event.keysym.lower()
        else:
            char = SHIFTED_NUMBERS.get(event.keysym, SHIFTED_NUMBERS.get(event.char, event.keysym.lower()))
        if char in pad_keys:
            return PAD_DOWN, PAD_UP, pad_keys[char]
        number = button_keys.get(event.keysym, button_keys.get(char))
        if number is not None:
            return KEY_DOWN, KEY_UP, number
        return None

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

    def reverse_key_labels(self) -> dict[tuple[str, int], str]:
        short = {
            "Shift_L": "Shift", "Shift_R": "Shift", "Escape": "Esc",
            "Return": "Enter", "KP_Enter": "Enter", "Delete": "Del", "BackSpace": "Del",
            "minus": "-", "equal": "=", "bracketleft": "[", "bracketright": "]",
        }
        out: dict[tuple[str, int], str] = {}
        for k, pad in self.pad_keys.items():
            out.setdefault(("pad", pad), k.upper())
        for k, code in self.button_keys.items():
            kind = "value" if code == 0x31 else "key"
            label = short.get(k, k.upper() if len(k) == 1 else k)
            out.setdefault((kind, code), label)
        return out

    def key_down(self, event):
        if event.state & 0x4:  # Ignore Ctrl+S / Ctrl+R shortcuts
            return None
        if not self.can_control():
            if event.keysym == "Escape" and self.fullscreen:
                self.flip()
            return None
        if event.keysym in VALUE_KEYS:
            self.send_value_turn(VALUE_KEYS[event.keysym])
            return "break"
        target = self._resolve_for(event, self.pad_keys, self.button_keys)
        if target is None:
            return None
        down, up, number = target
        key_id = event.keycode or event.keysym.lower()
        if key_id not in self.held:
            if self.send(down, number, PAD_VELOCITY if down == PAD_DOWN else 0):
                self.held[key_id] = (up, number)
                kind = "pad" if down == PAD_DOWN else ("value" if number == 0x31 else "key")
                name = f"PAD {number + 1}" if down == PAD_DOWN else KEY_NAMES.get(number, f"KEY {number:#x}")
                self._show_combo_hud(name)
                if self.panel_window:
                    self.panel_window.set_highlight(kind, number, True)
        return "break"

    def key_up(self, event):
        key_id = event.keycode or event.keysym.lower()
        held = self.held.pop(key_id, None)
        if held:
            up, number = held
            if not (up == KEY_UP and number in self.sticky_vars and self.sticky_vars[number].get()):
                self.send(up, number)
            kind = "pad" if up == PAD_UP else ("value" if number == 0x31 else "key")
            if self.panel_window:
                self.panel_window.set_highlight(kind, number, False)
            return "break"
        return None

    def release_all(self):
        held, self.held = self.held, {}
        for up, number in held.values():
            self.send(up, number)
        for code, var in self.sticky_vars.items():
            if var.get():
                var.set(False)
                if self.usb.connection:
                    self.send(KEY_UP, code, 0)
        if self.panel_window:
            self.panel_window.active_highlights.clear()
            self.panel_window.redraw_overlays()

    def focus_out(self, _event=None):
        def _check():
            focused = self.root.focus_get()
            if focused is None:
                self.release_all()
        self.root.after(30, _check)

    def control_changed(self):
        if not self.control.get():
            self.release_all()
        else:
            self.canvas.focus_set()

    def wheel(self, event):
        if not self.can_control():
            return
        steps = int(event.delta) if sys.platform == "darwin" else round(event.delta / 120)
        if steps:
            self.send_value_turn(steps)

    # ---- Keymap Presets & Customizer ----

    def apply_keymap_preset(self, preset_name: str):
        self.keymap_name = preset_name
        if preset_name == "60% / Laptop (No F-Keys)":
            self.pad_keys = {k: i for i, k in enumerate("1234qwerasdfzxcv")}
            self.button_keys = dict(LAPTOP_60_BUTTONS)
        else:
            chars = PAD_PRESETS.get(preset_name, "1234qwerasdfzxcv")
            self.pad_keys = {k: i for i, k in enumerate(chars)}
            self.button_keys = dict(BUTTON_KEYS)
        self._save_config()
        if self.panel_window:
            self.panel_window.redraw_overlays()
        self._show_combo_hud(f"KEYMAP: {preset_name}")
        self._announce(f"Keymap set to {preset_name}")

    def open_keymap_editor(self):
        win = tk.Toplevel(self.root)
        win.title("Customize Screen Genie Keymap")
        win.geometry("460x520")
        ttk.Label(win, text="Click any SP-404 control below, then press the key you want to assign.",
                  padding=10, wraplength=430).pack(fill=tk.X)

        container = ttk.Frame(win)
        container.pack(fill=tk.BOTH, expand=True, padx=10, pady=(0, 8))
        tree = ttk.Treeview(container, columns=("control", "key"), show="headings", height=16)
        tree.heading("control", text="SP-404MKII Control")
        tree.heading("key", text="Assigned Key")
        tree.column("control", width=230)
        tree.column("key", width=180)
        sb = ttk.Scrollbar(container, orient=tk.VERTICAL, command=tree.yview)
        tree.configure(yscrollcommand=sb.set)
        tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb.pack(side=tk.RIGHT, fill=tk.Y)

        items = [("pad", i, f"Pad {i + 1}") for i in range(16)] + [
            ("key", code, name) for code, name in sorted(KEY_NAMES.items())
        ]
        row_map = {}

        def refresh_rows():
            tree.delete(*tree.get_children())
            row_map.clear()
            rev = self.reverse_key_labels()
            for kind, ident, name in items:
                lookup_kind = "value" if (kind == "key" and ident == 0x31) else kind
                iid = tree.insert("", tk.END, values=(name, rev.get((lookup_kind, ident), "—")))
                row_map[iid] = (kind, ident, name)

        refresh_rows()
        prompt = ttk.Label(win, text="Select a row and click Rebind Key.", padding=(10, 4))
        prompt.pack(fill=tk.X)
        waiting = {"target": None}

        def start_rebind(_event=None):
            sel = tree.selection()
            if not sel:
                return
            waiting["target"] = row_map[sel[0]]
            prompt["text"] = f"Press any key for {waiting['target'][2]} (Esc to cancel)…"

        def capture_key(event):
            if not waiting["target"]:
                return None
            if event.keysym == "Escape" and waiting["target"][1] != 0x22:
                waiting["target"] = None
                prompt["text"] = "Rebind cancelled."
                return "break"
            kind, ident, name = waiting["target"]
            waiting["target"] = None
            sym = event.keysym
            key_str = sym.lower() if len(sym) == 1 else sym
            # Clear existing binding for this key and for this control
            self.pad_keys = {k: v for k, v in self.pad_keys.items() if k != key_str and not (kind == "pad" and v == ident)}
            self.button_keys = {k: v for k, v in self.button_keys.items() if k != key_str and not (kind == "key" and v == ident)}
            if kind == "pad":
                self.pad_keys[key_str] = ident
            else:
                self.button_keys[key_str] = ident
            self.keymap_name = "Custom"
            self._save_config()
            refresh_rows()
            if self.panel_window:
                self.panel_window.redraw_overlays()
            prompt["text"] = f"Bound {name} → {key_str}"
            return "break"

        tree.bind("<Double-1>", start_rebind)
        win.bind("<KeyPress>", capture_key)
        btns = ttk.Frame(win, padding=8)
        btns.pack(fill=tk.X)
        ttk.Button(btns, text="Rebind Selected", command=start_rebind).pack(side=tk.LEFT)
        ttk.Button(btns, text="Reset to Default",
                   command=lambda: (self.apply_keymap_preset("SP-404 Layout (1234 Top)"), refresh_rows())).pack(side=tk.LEFT, padx=8)
        ttk.Button(btns, text="Done", command=win.destroy).pack(side=tk.RIGHT)

    def show_shortcuts_help(self):
        rev = self.reverse_key_labels()
        lines = [
            f"Active Keymap: {self.keymap_name}",
            "",
            "Pads 1–4:   " + " ".join(rev.get(("pad", i), "—") for i in range(0, 4)),
            "Pads 5–8:   " + " ".join(rev.get(("pad", i), "—") for i in range(4, 8)),
            "Pads 9–12:  " + " ".join(rev.get(("pad", i), "—") for i in range(8, 12)),
            "Pads 13–16: " + " ".join(rev.get(("pad", i), "—") for i in range(12, 16)),
            "",
            "VALUE Turn: Arrow keys or Mouse Wheel",
            "Virtual 404 Panel (Visual Layout Overlay): F12",
            "Save PNG Screenshot: Ctrl+S  ·  Record GIF: Ctrl+R  ·  Full Screen: F11",
        ]
        messagebox.showinfo("Screen Genie · Controls", "\n".join(lines))

    # ---- Capture & OBS Creator Tools ----

    def frame_to_pil(self, frame: bytes):
        pixels = b"".join(self.row_lut[byte] for byte in frame)
        return Image.frombytes("RGB", (128, 64), pixels)

    def frame_to_photo(self, frame: bytes, scale: int):
        if self.pixel_grid.get() and scale >= 3 and Image is not None and ImageTk is not None:
            im = self.frame_to_pil(frame).resize((128 * scale, 64 * scale), Image.NEAREST)
            # Subtle OLED pixel grid gap on every scale-th row/col
            px = im.load()
            off_rgb = tuple(_hex_to_bytes(self.off_color))
            w, h = im.size
            for y in range(scale - 1, h, scale):
                for x in range(w):
                    px[x, y] = off_rgb
            for x in range(scale - 1, w, scale):
                for y in range(h):
                    px[x, y] = off_rgb
            return ImageTk.PhotoImage(im)
        pixels = b"".join(self.row_lut[byte] for byte in frame)
        return tk.PhotoImage(data=b"P6\n128 64\n255\n" + pixels, format="PPM").zoom(scale)

    def save_screenshot(self):
        if not self.frame:
            messagebox.showinfo("Save Screenshot", "No SP-404MKII frame received yet.")
            return
        if Image is None:
            messagebox.showerror("Save Screenshot", "Pillow is required to save PNG screenshots.")
            return
        name = filedialog.asksaveasfilename(
            defaultextension=".png",
            initialfile=f"sp404-screen-{time.strftime('%Y%m%d-%H%M%S')}.png",
            filetypes=[("PNG image", "*.png")],
        )
        if not name:
            return
        im = self.frame_to_pil(self.frame).resize((512, 256), Image.NEAREST)
        im.save(name, format="PNG")
        self.status["text"] = f"Saved screenshot: {Path(name).name}"
        self._announce("Screenshot saved")

    def toggle_gif_recording(self):
        if Image is None:
            messagebox.showerror("GIF Recording", "Pillow is required to record animated GIFs.")
            return
        if not self.recording:
            self.rec_frames.clear()
            self.recording = True
            self.rec_menu.entryconfig(self.rec_menu_idx, label="Stop & Save GIF Recording  Ctrl+R")
            self.status["text"] = "Recording GIF frames…"
            self._show_combo_hud("REC GIF")
            self._announce("Started GIF recording")
            return
        self.recording = False
        self.rec_menu.entryconfig(self.rec_menu_idx, label="Start GIF Recording  Ctrl+R")
        if not self.rec_frames:
            self.status["text"] = "No frames captured during recording."
            return
        name = filedialog.asksaveasfilename(
            defaultextension=".gif",
            initialfile=f"sp404-clip-{time.strftime('%Y%m%d-%H%M%S')}.gif",
            filetypes=[("Animated GIF", "*.gif")],
        )
        if not name:
            self.rec_frames.clear()
            return
        imgs = []
        durations = []
        for idx, (ts, raw) in enumerate(self.rec_frames):
            imgs.append(self.frame_to_pil(raw).resize((512, 256), Image.NEAREST).convert("P", palette=Image.ADAPTIVE, colors=4))
            if idx + 1 < len(self.rec_frames):
                dt = int(max(20, min(1000, (self.rec_frames[idx + 1][0] - ts) * 1000)))
            else:
                dt = 60
            durations.append(dt)
        imgs[0].save(name, save_all=True, append_images=imgs[1:], duration=durations, loop=0, optimize=True)
        count = len(imgs)
        self.rec_frames.clear()
        self.status["text"] = f"Saved GIF ({count} frames): {Path(name).name}"
        self._announce("GIF saved")

    def toggle_obs_server(self):
        if not self.obs:
            self.obs_running.set(False)
            return
        if self.obs_running.get():
            try:
                url = self.obs.start()
                self.obs.update_theme(self.on_color, self.off_color, self.show_hud.get())
                if self.frame:
                    self.obs.update_frame(self.frame, self.on_color, self.off_color)
                self.status["text"] = f"OBS Browser Source live at {url} (add ?bg=transparent for alpha)"
                self._announce("OBS Browser Source started")
            except OSError as err:
                self.obs_running.set(False)
                messagebox.showerror("OBS Server Error", f"Could not start OBS server on port 4040:\n{err}")
        else:
            self.obs.stop()
            self.status["text"] = "OBS Browser Source server stopped."

    def copy_obs_url(self):
        url = self.obs.url if self.obs else "http://127.0.0.1:4040/"
        self.root.clipboard_clear()
        self.root.clipboard_append(url)
        self.status["text"] = f"Copied OBS Browser Source URL: {url}"

    def open_obs_preview(self):
        if not self.obs:
            return
        if not self.obs.running:
            self.obs_running.set(True)
            self.toggle_obs_server()
        if self.obs.running:
            webbrowser.open(self.obs.url)

    # ---- OLED Canvas Rendering ----

    def _draw_hud(self):
        self.canvas.delete("hud")
        if not self.show_hud.get() or not self.hud_message:
            return
        w = self.canvas.winfo_width()
        h = self.canvas.winfo_height()
        if w < 100 or h < 60:
            return
        pad_x = max(14, len(self.hud_message) * 5 + 18)
        y = h - 26
        self.canvas.create_rectangle(
            w // 2 - pad_x, y - 14, w // 2 + pad_x, y + 14,
            fill="#0a121b", outline=self.on_color, width=1, tags=("hud",),
        )
        self.canvas.create_text(
            w // 2, y, text=self.hud_message,
            fill=self.on_color, font=(UI_FONT, 11, "bold"), tags=("hud",),
        )

    def draw(self, _event=None):
        width, height = self.canvas.winfo_width(), self.canvas.winfo_height()
        if not self.frame:
            if self.image_item is not None:
                self.canvas.delete(self.image_item)
                self.image_item = None
            if self.placeholder_item is None:
                self.placeholder_item = self.canvas.create_text(
                    width // 2, height // 2, text="Waiting for SP-404MKII",
                    fill=self.on_color, font=(UI_FONT, 20),
                )
            else:
                self.canvas.itemconfig(self.placeholder_item, fill=self.on_color)
                self.canvas.coords(self.placeholder_item, width // 2, height // 2)
            self._draw_hud()
            return
        if self.placeholder_item is not None:
            self.canvas.delete(self.placeholder_item)
            self.placeholder_item = None
        scale = max(1, min(width // 128, height // 64))
        self.image = self.frame_to_photo(self.frame, scale)
        if self.image_item is None:
            self.image_item = self.canvas.create_image(width // 2, height // 2, image=self.image)
        else:
            self.canvas.itemconfig(self.image_item, image=self.image)
            self.canvas.coords(self.image_item, width // 2, height // 2)
        self._draw_hud()

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
        if self.capture_window:
            self.capture_window.render()
        if self.panel_window:
            self.panel_window.update_mini_oled()

    def flip(self, _event=None):
        self.fullscreen = not self.fullscreen
        self.root.attributes("-fullscreen", self.fullscreen)

    def close(self):
        self.release_all()
        if self.obs and self.obs.running:
            self.obs.stop()
        if self.usb.connection:
            try:
                self.usb.write(input_packet(LINK_STATE, 0))
            except OSError:
                pass
        self.usb.close()
        self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    Viewer(root)
    root.mainloop()
