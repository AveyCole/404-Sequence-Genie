"""Separate interactive Virtual SP-404MKII Panel window with visual keymap overlay."""
from __future__ import annotations

import math
from pathlib import Path
import sys
import tkinter as tk
from tkinter import ttk

try:
    from PIL import Image, ImageTk
except ImportError:
    Image = None
    ImageTk = None

UI_FONT = "Segoe UI" if sys.platform == "win32" else ("Helvetica Neue" if sys.platform == "darwin" else "TkDefaultFont")

# Native image size of sp404mkii-panel.jpg: 858 x 1345
IMG_W, IMG_H = 858, 1345

# (kind, id, label, (x1, y1, x2, y2))
# kind: "pad" (0..15), "key" (stock key code 0x10..0x31), "knob" (1..3), "value" (0x31 + wheel)
PANEL_CONTROLS = [
    # Top knobs
    ("knob", 1, "CTRL 1", (290, 100, 386, 196)),
    ("knob", 2, "CTRL 2", (471, 100, 567, 196)),
    ("knob", 3, "CTRL 3", (653, 100, 749, 196)),
    # Six face FX buttons
    ("key", 0x2B, "FILTER+DRIVE", (142, 270, 260, 332)),
    ("key", 0x2C, "RESONATOR", (125, 344, 236, 412)),
    ("key", 0x2D, "DELAY", (140, 424, 260, 492)),
    ("key", 0x2E, "ISOLATOR", (598, 270, 718, 332)),
    ("key", 0x2F, "DJFX LOOPER", (622, 344, 735, 412)),
    ("key", 0x30, "MFX", (598, 424, 718, 492)),
    # VALUE push + encoder
    ("value", 0x31, "VALUE", (682, 542, 756, 616)),
    # Row 1 below OLED
    ("key", 0x14, "PTN SEL", (104, 562, 174, 614)),
    ("key", 0x15, "PTN EDIT", (190, 562, 260, 614)),
    ("key", 0x16, "REC SET", (278, 562, 348, 614)),
    ("key", 0x17, "START/END", (394, 562, 464, 614)),
    ("key", 0x18, "PITCH/SPD", (482, 562, 552, 614)),
    ("key", 0x19, "MARK", (570, 562, 640, 614)),
    # Row 2 below OLED
    ("key", 0x1A, "DEL", (104, 640, 174, 692)),
    ("key", 0x1B, "REC", (190, 640, 260, 692)),
    ("key", 0x1C, "RESAMPLE", (278, 640, 348, 692)),
    ("key", 0x1D, "BPM SYNC", (374, 640, 424, 692)),
    ("key", 0x1E, "GATE", (440, 640, 490, 692)),
    ("key", 0x1F, "LOOP", (507, 640, 576, 692)),
    ("key", 0x20, "REVERSE", (594, 640, 664, 692)),
    ("key", 0x21, "ROLL", (686, 640, 756, 692)),
    # Row 3 below OLED
    ("key", 0x22, "EXIT", (104, 718, 174, 768)),
    ("key", 0x23, "COPY", (190, 718, 260, 768)),
    ("key", 0x24, "REMAIN", (278, 718, 348, 768)),
    ("key", 0x25, "BANK A/F", (374, 718, 424, 768)),
    ("key", 0x26, "BANK B/G", (440, 718, 490, 768)),
    ("key", 0x27, "BANK C/H", (507, 718, 557, 768)),
    ("key", 0x28, "BANK D/I", (573, 718, 623, 768)),
    ("key", 0x29, "BANK E/J", (640, 718, 690, 768)),
    ("key", 0x2A, "SHIFT", (706, 718, 756, 768)),
    # Right column next to pads
    ("key", 0x10, "BUS FX", (646, 796, 756, 892)),
    ("key", 0x11, "HOLD", (646, 914, 756, 1010)),
    ("key", 0x12, "EXT SRC", (646, 1032, 756, 1128)),
    ("key", 0x13, "SUB PAD", (646, 1150, 756, 1246)),
]

# Pads 1..16 (indices 0..15) in exact physical top-to-bottom 4x4 grid
_PAD_COLS = ((102, 212), (238, 348), (374, 484), (510, 620))
_PAD_ROWS = ((796, 892), (914, 1010), (1032, 1128), (1150, 1246))
for _r, (_y1, _y2) in enumerate(_PAD_ROWS):
    for _c, (_x1, _x2) in enumerate(_PAD_COLS):
        _idx = _r * 4 + _c
        PANEL_CONTROLS.append(("pad", _idx, f"PAD {_idx + 1}", (_x1, _y1, _x2, _y2)))

# OLED screen inset inside the faceplate circle
OLED_RECT = (302, 314, 558, 442)


class VirtualPanelWindow:
    """Pop-out interactive SP-404MKII faceplate and visual keyboard map."""

    def __init__(self, viewer):
        self.viewer = viewer
        self.win = tk.Toplevel(viewer.root)
        self.win.title("Virtual SP-404MKII Panel · Visual Layout & Controls")
        self.win.configure(bg="#11161f")
        self.win.geometry("560x860")
        self.win.minsize(420, 640)
        self.win.protocol("WM_DELETE_WINDOW", self.close)
        self.win.bind("<KeyPress>", viewer.key_down)
        self.win.bind("<KeyRelease>", viewer.key_up)

        self.show_labels = tk.BooleanVar(value=True)
        self.show_mini_oled = tk.BooleanVar(value=True)
        self.scale = 0.55
        self.ox = 0
        self.oy = 0
        self.bg_photo = None
        self.oled_photo = None
        self.source_image = self._load_panel_image()
        self.pressed_control = None
        self.drag_knob = None
        self.drag_start_y = 0
        self.drag_start_val = 64
        self.value_drag_y = None
        self.knob_values = {1: 64, 2: 64, 3: 64}
        self.active_highlights: set[tuple[str, int]] = set()

        # Top control strip (sticky modifiers + overlay toggles)
        top = ttk.Frame(self.win, padding=(8, 6))
        top.pack(fill=tk.X)
        ttk.Checkbutton(top, text="Show Key Labels", variable=self.show_labels,
                        command=self.redraw_overlays, takefocus=0).pack(side=tk.LEFT)
        ttk.Checkbutton(top, text="Live OLED", variable=self.show_mini_oled,
                        command=self.redraw_overlays, takefocus=0).pack(side=tk.LEFT, padx=(10, 0))
        ttk.Separator(top, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=8)
        for label, code in (("Sticky SHIFT", 0x2A), ("Sticky REMAIN", 0x24), ("Sticky MFX", 0x30)):
            var = viewer.sticky_vars[code]
            ttk.Checkbutton(top, text=label, variable=var,
                            command=lambda c=code: viewer.toggle_sticky(c),
                            takefocus=0).pack(side=tk.LEFT, padx=(4, 0))

        # Quick Macro bar
        macros = ttk.Frame(self.win, padding=(8, 0, 8, 6))
        macros.pack(fill=tk.X)
        ttk.Label(macros, text="Macros:").pack(side=tk.LEFT, padx=(0, 6))
        for name, macro_id in (
            ("Open Genie", "open_genie"),
            ("Modes", "modes"),
            ("Bounce", "bounce"),
            ("BUS FX", "bus_fx"),
            ("Utility", "utility"),
        ):
            ttk.Button(macros, text=name, command=lambda m=macro_id: viewer.run_macro(m),
                       takefocus=0).pack(side=tk.LEFT, padx=(0, 4))

        # Interactive faceplate canvas
        self.canvas = tk.Canvas(self.win, bg="#11161f", highlightthickness=0, takefocus=1)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self.canvas.bind("<Configure>", self._on_resize)
        self.canvas.bind("<ButtonPress-1>", self._on_press)
        self.canvas.bind("<B1-Motion>", self._on_drag)
        self.canvas.bind("<ButtonRelease-1>", self._on_release)
        self.canvas.bind("<MouseWheel>", self._on_wheel)

        # Bottom tip bar
        self.info = ttk.Label(
            self.win,
            text="Click pads/buttons directly · Drag CTRL 1–3 up/down · Scroll over VALUE or CTRL knobs",
            padding=(8, 5), font=(UI_FONT, 8), foreground="#9bb0bd",
        )
        self.info.pack(side=tk.BOTTOM, fill=tk.X)

    def _load_panel_image(self):
        if Image is None:
            return None
        base = Path(getattr(sys, "_MEIPASS", Path(__file__).parent))
        for candidate in (
            base / "sp404mkii-panel.jpg",
            Path(__file__).parent / "sp404mkii-panel.jpg",
            Path(__file__).resolve().parents[2] / "docs" / "sp404mkii-panel.jpg",
        ):
            if candidate.is_file():
                return Image.open(candidate).convert("RGB")
        return None

    def _img_to_canvas(self, x: float, y: float) -> tuple[float, float]:
        return self.ox + x * self.scale, self.oy + y * self.scale

    def _canvas_to_img(self, cx: float, cy: float) -> tuple[float, float]:
        return (cx - self.ox) / self.scale, (cy - self.oy) / self.scale

    def _hit_test(self, cx: float, cy: float):
        ix, iy = self._canvas_to_img(cx, cy)
        for ctrl in PANEL_CONTROLS:
            _, _, _, (x1, y1, x2, y2) = ctrl
            if x1 <= ix <= x2 and y1 <= iy <= y2:
                return ctrl
        return None

    def _on_resize(self, _event=None):
        cw = max(100, self.canvas.winfo_width())
        ch = max(100, self.canvas.winfo_height())
        self.scale = min(cw / IMG_W, ch / IMG_H)
        dw = max(1, int(IMG_W * self.scale))
        dh = max(1, int(IMG_H * self.scale))
        self.ox = (cw - dw) // 2
        self.oy = (ch - dh) // 2
        self.canvas.delete("all")
        if self.source_image is not None and ImageTk is not None:
            resized = self.source_image.resize((dw, dh), Image.LANCZOS)
            self.bg_photo = ImageTk.PhotoImage(resized)
            self.canvas.create_image(self.ox, self.oy, anchor=tk.NW, image=self.bg_photo, tags=("bg",))
        else:
            # Clean vector fallback if Pillow isn't available
            self.canvas.create_rectangle(self.ox, self.oy, self.ox + dw, self.oy + dh,
                                         fill="#262c36", outline="#5a6a7a", width=2, tags=("bg",))
        self.redraw_overlays()

    def set_highlight(self, kind: str, ident: int, active: bool):
        key = (kind, ident)
        if active:
            self.active_highlights.add(key)
        else:
            self.active_highlights.discard(key)
        if self.win.winfo_exists():
            self.redraw_overlays()

    def set_knob_value(self, knob_id: int, value: int):
        self.knob_values[knob_id] = max(0, min(127, int(value)))
        if self.win.winfo_exists():
            self.redraw_overlays()

    def update_mini_oled(self):
        if not self.win.winfo_exists() or not self.show_mini_oled.get():
            return
        self.canvas.delete("mini_oled")
        frame = self.viewer.frame
        if not frame:
            return
        x1, y1 = self._img_to_canvas(OLED_RECT[0], OLED_RECT[1])
        x2, y2 = self._img_to_canvas(OLED_RECT[2], OLED_RECT[3])
        w = max(16, int(x2 - x1))
        h = max(8, int(y2 - y1))
        if Image is not None and ImageTk is not None:
            im = self.viewer.frame_to_pil(frame).resize((w, h), Image.NEAREST)
            self.oled_photo = ImageTk.PhotoImage(im)
            self.canvas.create_image((x1 + x2) / 2, (y1 + y2) / 2, image=self.oled_photo, tags=("mini_oled",))
        self.canvas.tag_raise("overlay")

    def redraw_overlays(self):
        if not self.win.winfo_exists():
            return
        self.canvas.delete("overlay")
        self.update_mini_oled()
        show_labels = self.show_labels.get()
        key_labels = self.viewer.reverse_key_labels()
        sticky_codes = {c for c, v in self.viewer.sticky_vars.items() if v.get()}
        font_sz = max(7, min(11, int(13 * self.scale)))
        small_sz = max(6, min(9, int(10 * self.scale)))

        for kind, ident, name, (x1, y1, x2, y2) in PANEL_CONTROLS:
            cx1, cy1 = self._img_to_canvas(x1, y1)
            cx2, cy2 = self._img_to_canvas(x2, y2)
            mx, my = (cx1 + cx2) / 2, (cy1 + cy2) / 2
            is_active = (kind, ident) in self.active_highlights or (
                kind in ("key", "value") and ident in sticky_codes
            )

            if kind == "knob":
                val = self.knob_values.get(ident, 64)
                # Draw subtle rotary arc + pointer + numeric badge
                r = (cx2 - cx1) * 0.44
                angle_deg = 225 - (val / 127.0) * 270.0
                rad = math.radians(angle_deg)
                px = mx + r * 0.72 * math.cos(rad)
                py = my - r * 0.72 * math.sin(rad)
                outline = "#5cf2e2" if is_active else "#8fb8c9"
                self.canvas.create_oval(mx - r, my - r, mx + r, my + r,
                                        outline=outline, width=2 if is_active else 1, tags=("overlay",))
                self.canvas.create_line(mx, my, px, py, fill="#5cf2e2", width=3, capstyle=tk.ROUND, tags=("overlay",))
                if show_labels:
                    badge = f"{val}"
                    self.canvas.create_rectangle(mx - 15, cy2 - 14, mx + 15, cy2 + 2,
                                                 fill="#0b131c", outline="#4cd9cb", tags=("overlay",))
                    self.canvas.create_text(mx, cy2 - 6, text=badge, fill="#d9fff9",
                                            font=(UI_FONT, small_sz, "bold"), tags=("overlay",))
                continue

            if is_active:
                self.canvas.create_rectangle(
                    cx1, cy1, cx2, cy2,
                    outline="#5cf2e2", width=3,
                    fill="#34d8c8", stipple="gray25",
                    tags=("overlay",),
                )
            elif show_labels:
                self.canvas.create_rectangle(
                    cx1, cy1, cx2, cy2,
                    outline="#48687a" if self.source_image else "#7b93a8",
                    width=1, tags=("overlay",),
                )

            if show_labels:
                shortcut = key_labels.get((kind, ident), "")
                if kind == "value":
                    shortcut = shortcut or "Ent/Whl"
                if shortcut:
                    bw = max(20, len(shortcut) * (font_sz * 0.62) + 8)
                    bh = font_sz + 6
                    by = cy2 - bh / 2 - 2 if kind == "pad" else my
                    self.canvas.create_rectangle(
                        mx - bw / 2, by - bh / 2, mx + bw / 2, by + bh / 2,
                        fill="#0a121b" if not is_active else "#115e59",
                        outline="#5cf2e2" if is_active else "#8bb8c8",
                        tags=("overlay",),
                    )
                    self.canvas.create_text(
                        mx, by, text=shortcut,
                        fill="#ffffff" if is_active else "#d8f6f2",
                        font=(UI_FONT, font_sz, "bold"),
                        tags=("overlay",),
                    )
                elif self.source_image is None:
                    self.canvas.create_text(
                        mx, my, text=name, fill="#c4d6e0",
                        font=(UI_FONT, small_sz), tags=("overlay",),
                    )

    def _on_press(self, event):
        self.canvas.focus_set()
        ctrl = self._hit_test(event.x, event.y)
        if not ctrl:
            return
        kind, ident, name, _ = ctrl
        if kind == "knob":
            self.drag_knob = ident
            self.drag_start_y = event.y
            self.drag_start_val = self.knob_values.get(ident, 64)
            self.set_highlight("knob", ident, True)
            return
        if kind == "value":
            self.pressed_control = ctrl
            self.value_drag_y = event.y
            self.viewer.virtual_key_down(ident, name)
            self.set_highlight("value", ident, True)
            return
        self.pressed_control = ctrl
        if kind == "pad":
            self.viewer.virtual_pad_down(ident, name)
        else:
            self.viewer.virtual_key_down(ident, name)
        self.set_highlight(kind, ident, True)

    def _on_drag(self, event):
        if self.drag_knob is not None:
            delta = int((self.drag_start_y - event.y) * 0.85)
            val = max(0, min(127, self.drag_start_val + delta))
            if val != self.knob_values.get(self.drag_knob):
                self.knob_values[self.drag_knob] = val
                self.viewer.send_ctrl_knob(self.drag_knob, val)
                self.redraw_overlays()

    def _on_release(self, _event):
        if self.drag_knob is not None:
            knob = self.drag_knob
            self.drag_knob = None
            self.set_highlight("knob", knob, False)
            return
        if self.pressed_control is not None:
            kind, ident, name, _ = self.pressed_control
            self.pressed_control = None
            if kind == "pad":
                self.viewer.virtual_pad_up(ident)
            else:
                self.viewer.virtual_key_up(ident)
            self.set_highlight(kind, ident, False)

    def _on_wheel(self, event):
        steps = int(event.delta) if sys.platform == "darwin" else round(event.delta / 120)
        if not steps:
            return
        ctrl = self._hit_test(event.x, event.y)
        if ctrl and ctrl[0] == "knob":
            knob = ctrl[1]
            val = max(0, min(127, self.knob_values.get(knob, 64) + steps * 4))
            self.knob_values[knob] = val
            self.viewer.send_ctrl_knob(knob, val)
            self.redraw_overlays()
        else:
            self.viewer.send_value_turn(steps)

    def close(self):
        if self.pressed_control is not None:
            self._on_release(None)
        self.viewer.panel_window = None
        if self.win.winfo_exists():
            self.win.destroy()
