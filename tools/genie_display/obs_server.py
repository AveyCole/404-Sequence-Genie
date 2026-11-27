"""Built-in local HTTP server for OBS Browser Source live OLED streaming."""
from __future__ import annotations

import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading
import time


OBS_HTML = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Screen Genie · OBS OLED Source</title>
<style>
  html, body {
    margin: 0; padding: 0; width: 100%; height: 100%;
    overflow: hidden; background: transparent;
    display: flex; align-items: center; justify-content: center;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  }
  .wrap { position: relative; width: 100%; height: 100%; display: flex; align-items: center; justify-content: center; }
  canvas {
    image-rendering: pixelated;
    image-rendering: crisp-edges;
    max-width: 100%; max-height: 100%;
    aspect-ratio: 2 / 1;
    width: min(100vw, 200vh);
    height: min(100vh, 50vw);
  }
  .hud {
    position: absolute; bottom: 14px; left: 50%; transform: translateX(-50%);
    padding: 6px 14px; border-radius: 999px;
    background: rgba(10, 16, 24, 0.86); color: #e8f8f5;
    border: 1px solid rgba(198, 248, 242, 0.35);
    font-size: 18px; font-weight: 600; letter-spacing: 0.04em;
    opacity: 0; transition: opacity 0.18s ease;
    pointer-events: none; white-space: nowrap;
  }
  .hud.show { opacity: 1; }
</style>
</head>
<body>
<div class="wrap">
  <canvas id="oled" width="128" height="64"></canvas>
  <div id="hud" class="hud"></div>
</div>
<script>
const canvas = document.getElementById("oled");
const ctx = canvas.getContext("2d");
const hud = document.getElementById("hud");
const img = ctx.createImageData(128, 64);
const params = new URLSearchParams(location.search);
const transparentBg = params.get("bg") === "transparent";
let lastSeq = -1;

function hexToRgb(hex) {
  const v = parseInt(hex.replace("#", ""), 16);
  return [(v >> 16) & 255, (v >> 8) & 255, v & 255];
}

function render(b64, onHex, offHex) {
  const bin = atob(b64);
  const on = hexToRgb(onHex || "#c6f8f2");
  const off = hexToRgb(offHex || "#060d12");
  const d = img.data;
  for (let y = 0; y < 64; y++) {
    for (let bx = 0; bx < 16; bx++) {
      const byte = bin.charCodeAt(y * 16 + bx);
      for (let bit = 0; bit < 8; bit++) {
        const lit = (byte & (128 >> bit)) !== 0;
        const p = (y * 128 + bx * 8 + bit) * 4;
        if (lit) {
          d[p] = on[0]; d[p+1] = on[1]; d[p+2] = on[2]; d[p+3] = 255;
        } else {
          d[p] = off[0]; d[p+1] = off[1]; d[p+2] = off[2]; d[p+3] = transparentBg ? 0 : 255;
        }
      }
    }
  }
  ctx.putImageData(img, 0, 0);
}

async function poll() {
  try {
    const res = await fetch("/state", { cache: "no-store" });
    if (res.ok) {
      const s = await res.json();
      if (s.frame && s.seq !== lastSeq) {
        lastSeq = s.seq;
        render(s.frame, s.on, s.off);
      }
      if (s.show_hud && s.hud) {
        hud.textContent = s.hud;
        hud.classList.add("show");
      } else {
        hud.classList.remove("show");
      }
    }
  } catch {}
  setTimeout(poll, 33);
}
poll();
</script>
</body>
</html>
"""


class ObsServer:
    """Serves the latest 128x64 OLED frame and optional combo HUD on localhost."""

    def __init__(self, host: str = "127.0.0.1", port: int = 4040):
        self.host = host
        self.port = port
        self._lock = threading.Lock()
        self._frame_b64 = ""
        self._seq = 0
        self._on = "#c6f8f2"
        self._off = "#060d12"
        self._hud = ""
        self._hud_until = 0.0
        self._show_hud = True
        self._server: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None

    @property
    def running(self) -> bool:
        return self._server is not None

    @property
    def url(self) -> str:
        return f"http://{self.host}:{self.port}/"

    def update_frame(self, frame: bytes, on_hex: str, off_hex: str):
        encoded = base64.b64encode(frame).decode("ascii")
        with self._lock:
            self._frame_b64 = encoded
            self._on = on_hex
            self._off = off_hex
            self._seq += 1

    def update_theme(self, on_hex: str, off_hex: str, show_hud: bool):
        with self._lock:
            self._on = on_hex
            self._off = off_hex
            self._show_hud = show_hud
            self._seq += 1

    def set_hud(self, text: str, duration: float = 1.8):
        with self._lock:
            self._hud = text
            self._hud_until = time.monotonic() + duration

    def snapshot(self) -> dict:
        with self._lock:
            hud = self._hud if time.monotonic() < self._hud_until else ""
            return {
                "seq": self._seq,
                "frame": self._frame_b64,
                "on": self._on,
                "off": self._off,
                "show_hud": self._show_hud,
                "hud": hud,
            }

    def start(self) -> str:
        if self._server is not None:
            return self.url
        parent = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                path = self.path.split("?", 1)[0]
                if path == "/state":
                    body = json.dumps(parent.snapshot()).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Cache-Control", "no-store")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                else:
                    body = OBS_HTML.encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)

            def log_message(self, format, *args):
                return

        self._server = ThreadingHTTPServer((self.host, self.port), Handler)
        self._server.daemon_threads = True
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        return self.url

    def stop(self):
        server, self._server = self._server, None
        if server is not None:
            server.shutdown()
            server.server_close()
        self._thread = None
