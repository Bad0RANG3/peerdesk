"""Host and viewer session logic: capture loop, input, clipboard, file transfer."""

from __future__ import annotations

import asyncio
import base64
import logging
import os
import platform
import socket
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

from . import util
from .capture import FramePacer, Monitor, ScreenCapture
from .transport import Candidate, LinkStats, PeerLink

log = logging.getLogger("peerdesk.session")

CHUNK_B64 = 24 * 1024
MAX_UPLOAD = 512 * 1024 * 1024

_thread_local = threading.local()


def thread_capture() -> ScreenCapture:
    cap = getattr(_thread_local, "cap", None)
    if cap is None:
        cap = ScreenCapture()
        _thread_local.cap = cap
    return cap


@dataclass
class VideoSettings:
    fps: int = 30
    max_width: int = 1920
    quality: int = 65
    monitor: int = 0
    skip_static: bool = True
    clipboard: bool = True
    pixel_format: str = "jpeg"

    def as_dict(self) -> dict:
        return {
            "fps": self.fps,
            "max_width": self.max_width,
            "quality": self.quality,
            "monitor": self.monitor,
            "skip_static": self.skip_static,
            "clipboard": self.clipboard,
            "pixel_format": self.pixel_format,
        }

    def update(self, obj: dict) -> None:
        for key in ("fps", "max_width", "quality", "monitor"):
            if key in obj:
                try:
                    setattr(self, key, int(obj[key]))
                except (TypeError, ValueError):
                    pass
        for key in ("skip_static", "clipboard"):
            if key in obj:
                setattr(self, key, bool(obj[key]))
        self.fps = max(1, min(120, self.fps))
        self.max_width = max(320, min(3840, self.max_width))
        self.quality = max(20, min(95, self.quality))


class HostSession:
    """Streams the local desktop and applies remote input."""

    def __init__(
        self,
        link: PeerLink,
        code: str,
        *,
        settings: Optional[VideoSettings] = None,
        on_event: Optional[Callable[[dict], None]] = None,
        allow_input: bool = True,
    ) -> None:
        self.link = link
        self.code = code
        self.settings = settings or VideoSettings()
        self.on_event = on_event or (lambda e: None)
        self.allow_input = allow_input
        self.monitors: list[Monitor] = []
        self.current_monitor: Optional[Monitor] = None
        self.pacer = FramePacer(self.settings.quality)
        self._force_keyframe = True
        self._running = False
        self._upload: dict[str, dict] = {}
        self._injector = None
        self._clip_watcher = None
        self._last_clip = ""
        self._frames = 0
        self._stats_at = time.monotonic()
        self._fps = 0.0
        self._loop = asyncio.get_event_loop()
        link.on_control = self.on_control
        link.on_state = self.on_link_state

    # ------------------------------------------------------------------ setup
    def _lazy_input(self):
        if self._injector is None:
            from .wininput import ClipboardWatcher, InputInjector

            self._injector = InputInjector()
            self._clip_watcher = ClipboardWatcher()
            self._last_clip = self._clip_watcher._last_text
        return self._injector

    def _load_monitors(self) -> None:
        try:
            cap = thread_capture()
            self.monitors = cap.monitors()
            if self.monitors:
                self.current_monitor = self.monitors[
                    max(0, min(self.settings.monitor, len(self.monitors) - 1))
                ]
        except Exception:
            log.exception("monitor enumeration failed")

    def info(self) -> dict:
        self._load_monitors()
        return {
            "t": "info",
            "name": socket.gethostname(),
            "os": f"{platform.system()} {platform.release()}",
            "monitors": [m.as_dict() for m in self.monitors],
            "settings": self.settings.as_dict(),
            "input": self.allow_input,
            "version": "1.0.0",
        }

    # ------------------------------------------------------------------ link
    def on_link_state(self, state: str, stats: LinkStats) -> None:
        self.on_event({"t": "link", "state": state, "path": stats.path, "relay": stats.using_relay})
        if state in ("direct", "relay") and not self._running:
            self._start_streaming()
        elif state == "closed":
            self._running = False

    def _start_streaming(self) -> None:
        self._running = True
        self._load_monitors()
        self.link.send_control(self.info())
        self._loop.create_task(self._capture_loop())

    async def _capture_loop(self) -> None:
        log.info("capture loop start")
        while self._running and self.link.established:
            t0 = time.perf_counter()
            if self._injector is None and self.allow_input:
                self._lazy_input()
            try:
                frame = await self._loop.run_in_executor(None, self._grab_once)
            except Exception:
                log.exception("grab failed")
                frame = None
            if frame:
                payload, w, h, _unchanged = frame
                keyframe = self._force_keyframe
                self._force_keyframe = False
                self.pacer.observe(len(payload), time.perf_counter() - t0)
                self.link.send_video(payload, keyframe=keyframe)
                self._frames += 1
            elif self._force_keyframe:
                self._force_keyframe = False
            self._poll_clipboard()
            now = time.monotonic()
            if now - self._stats_at >= 1.0:
                self._push_stats(now)
            target = self.pacer.frame_budget(self.settings.fps)
            elapsed = time.perf_counter() - t0
            await asyncio.sleep(max(0.0, target - elapsed))
        log.info("capture loop stop")

    def _grab_once(self):
        cap = thread_capture()
        return cap.grab(
            self.settings.monitor,
            self.settings.max_width,
            self.pacer.quality,
            skip_static=self.settings.skip_static,
            pixel_format=self.settings.pixel_format,
        )

    def _push_stats(self, now: float) -> None:
        elapsed = now - self._stats_at
        self._fps = self._frames / elapsed if elapsed > 0 else 0.0
        self._frames = 0
        self._stats_at = now
        stats = self.link.stats
        self.link.send_control(
            {
                "t": "stats",
                "fps": round(self._fps, 1),
                "mbps": round(stats.mbps, 2),
                "rtt": round(stats.rtt_ms, 1),
                "mode": stats.mode,
                "relay": stats.using_relay,
                "quality": self.pacer.quality,
                "kb": round(self.pacer.last_size / 1024, 1),
                "enc_ms": round(self.pacer.last_ms, 1),
            }
        )
        self.on_event({"t": "stats", "fps": round(self._fps, 1), "mbps": round(stats.mbps, 2)})

    def _poll_clipboard(self) -> None:
        if not self.settings.clipboard or self._clip_watcher is None:
            return
        text = self._clip_watcher.poll()
        if text and text != self._last_clip:
            self._last_clip = text
            self.link.send_control({"t": "clip", "s": text[: 256 * 1024]})

    # --------------------------------------------------------------- messages
    def on_control(self, msg: dict) -> None:
        try:
            self._dispatch(msg)
        except Exception:
            log.exception("control dispatch")

    def _dispatch(self, msg: dict) -> None:
        kind = msg.get("t")
        if kind == "hello":
            self.link.send_control(self.info())
        elif kind == "settings":
            self.settings.update(msg)
            if self.monitors:
                self.current_monitor = self.monitors[
                    max(0, min(self.settings.monitor, len(self.monitors) - 1))
                ]
            self._force_keyframe = True
            self.link.send_control({"t": "settings_ack", "settings": self.settings.as_dict()})
        elif kind == "keyframe_req":
            self._force_keyframe = True
        elif kind == "mouse":
            if self.allow_input:
                self._mouse(msg)
        elif kind == "key":
            if self.allow_input:
                self._lazy_input().key(str(msg.get("code", "")), bool(msg.get("down")))
        elif kind == "text":
            if self.allow_input:
                self._lazy_input().type_text(str(msg.get("s", "")))
        elif kind == "clip":
            text = str(msg.get("s", ""))
            if self.settings.clipboard and text != self._last_clip:
                from .wininput import set_clipboard_text

                set_clipboard_text(text)
                self._last_clip = text
                if self._clip_watcher:
                    self._clip_watcher._last_text = text
        elif kind == "file_begin":
            self._file_begin(msg)
        elif kind == "file_data":
            self._file_data(msg)
        elif kind == "file_end":
            self._file_end(msg)
        elif kind == "reboot":
            self.on_event({"t": "notice", "text": "收到重启指令（未执行）"})

    def _mouse(self, msg: dict) -> None:
        inj = self._lazy_input()
        action = msg.get("act", "move")
        if action == "move":
            nx, ny = float(msg.get("x", 0.0)), float(msg.get("y", 0.0))
            inj.move_abs(*self._map_to_virtual(nx, ny))
        elif action in ("down", "up"):
            if action == "down":
                inj.move_abs(*self._map_to_virtual(float(msg.get("x", 0.0)), float(msg.get("y", 0.0))))
            inj.button(str(msg.get("btn", "left")), action == "down")
        elif action == "click":
            inj.move_abs(*self._map_to_virtual(float(msg.get("x", 0.0)), float(msg.get("y", 0.0))))
            inj.click(str(msg.get("btn", "left")))
        elif action == "wheel":
            inj.wheel(int(msg.get("dy", 0)), bool(msg.get("h")))
        elif action == "rel":
            inj.move_rel(int(msg.get("dx", 0)), int(msg.get("dy", 0)))

    def _map_to_virtual(self, nx: float, ny: float) -> tuple[float, float]:
        """Frame-normalized (0..1) -> virtual-desktop-normalized (0..1)."""
        from .wininput import InputInjector

        vx, vy, vw, vh = InputInjector._virtual_screen()
        mon = self.current_monitor
        if not mon:
            return (max(0.0, min(1.0, nx)), max(0.0, min(1.0, ny)))
        x = mon.left + nx * mon.width
        y = mon.top + ny * mon.height
        return ((x - vx) / max(1, vw - 1), (y - vy) / max(1, vh - 1))

    # ----------------------------------------------------------- file transfer
    def _file_begin(self, msg: dict) -> None:
        fid = str(msg.get("id", ""))
        name = Path(str(msg.get("name", "file.bin"))).name
        size = int(msg.get("size", 0))
        if size <= 0 or size > MAX_UPLOAD:
            self.link.send_control({"t": "file_error", "id": fid, "reason": "size"})
            return
        target = util.download_dir() / "PeerDesk"
        target.mkdir(parents=True, exist_ok=True)
        path = target / name
        handle = open(path, "wb")
        self._upload[fid] = {"handle": handle, "path": path, "size": size, "got": 0}
        self.on_event({"t": "notice", "text": f"接收文件 {name}"})

    def _file_data(self, msg: dict) -> None:
        entry = self._upload.get(str(msg.get("id", "")))
        if not entry:
            return
        chunk = base64.b64decode(str(msg.get("d", "")))
        entry["handle"].write(chunk)
        entry["got"] += len(chunk)

    def _file_end(self, msg: dict) -> None:
        fid = str(msg.get("id", ""))
        entry = self._upload.pop(fid, None)
        if not entry:
            return
        entry["handle"].close()
        self.link.send_control({"t": "file_done", "id": fid, "path": str(entry["path"]), "size": entry["got"]})
        self.on_event({"t": "notice", "text": f"文件已保存 {entry['path'].name}"})

    def stop(self) -> None:
        self._running = False


class ViewerSession:
    """Receives frames and pushes remote input."""

    def __init__(
        self,
        link: PeerLink,
        *,
        on_frame: Optional[Callable[[bytes, dict], None]] = None,
        on_message: Optional[Callable[[dict], None]] = None,
    ) -> None:
        self.link = link
        self.on_frame = on_frame or (lambda data, meta: None)
        self.on_message = on_message or (lambda msg: None)
        self.info: dict = {}
        self.stats: dict = {}
        self._seq = 0
        self._frames = 0
        self._decoded_at = time.monotonic()
        link.on_video = self._video
        link.on_control = self._control
        link.on_state = self._state

    def _state(self, state: str, stats: LinkStats) -> None:
        self.on_message({"t": "link", "state": state, "path": stats.path, "relay": stats.using_relay})
        if state in ("direct", "relay"):
            self.link.send_control({"t": "hello", "role": "viewer"})

    def _video(self, payload: bytes, keyframe: bool) -> None:
        self._seq = (self._seq + 1) & 0xFFFFFFFF
        self._frames += 1
        self.on_frame(payload, {"seq": self._seq, "keyframe": keyframe})

    def _control(self, msg: dict) -> None:
        kind = msg.get("t")
        if kind == "info":
            self.info = msg
        elif kind == "stats":
            self.stats = msg
        self.on_message(msg)

    # --------------------------------------------------------------- commands
    def send(self, obj: dict) -> None:
        self.link.send_control(obj)

    def request_keyframe(self) -> None:
        self.send({"t": "keyframe_req"})

    def set_quality(self, quality: int | None = None, max_width: int | None = None, fps: int | None = None) -> None:
        msg: dict = {"t": "settings"}
        if quality is not None:
            msg["quality"] = quality
        if max_width is not None:
            msg["max_width"] = max_width
        if fps is not None:
            msg["fps"] = fps
        self.send(msg)

    def select_monitor(self, index: int) -> None:
        self.send({"t": "settings", "monitor": index})
        self.request_keyframe()

    def send_file(self, path: Path) -> None:
        path = Path(path)
        size = path.stat().st_size
        fid = os.urandom(4).hex()
        self.send({"t": "file_begin", "id": fid, "name": path.name, "size": size})
        with open(path, "rb") as fh:
            while True:
                chunk = fh.read(CHUNK_B64)
                if not chunk:
                    break
                self.send({"t": "file_data", "id": fid, "d": base64.b64encode(chunk).decode()})
        self.send({"t": "file_end", "id": fid})
