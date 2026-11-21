"""Screen capture and JPEG encoding (mss + Pillow, no native build step)."""

from __future__ import annotations

import ctypes
import io
import logging
import time
from dataclasses import dataclass

from PIL import Image

log = logging.getLogger("peerdesk.capture")


@dataclass
class Monitor:
    index: int
    left: int
    top: int
    width: int
    height: int
    primary: bool = False

    def as_dict(self) -> dict:
        return {
            "index": self.index,
            "w": self.width,
            "h": self.height,
            "primary": self.primary,
            "name": f"显示器 {self.index + 1}",
        }


def _dpi_aware() -> None:
    """Make the process per-monitor DPI aware so mss sees real pixels."""
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass


class ScreenCapture:
    """Owns one mss instance; not thread safe, create one per thread."""

    def __init__(self) -> None:
        _dpi_aware()
        import mss  # imported lazily so the module stays importable on headless CI

        factory = getattr(mss, "MSS", None) or mss.mss
        self._sct = factory()
        self._last_print: tuple[bytes, tuple[int, int, int, int]] | None = None
        self._cursor_visible = True

    # ------------------------------------------------------------------ layout
    def monitors(self) -> list[Monitor]:
        out: list[Monitor] = []
        raw = list(self._sct.monitors)
        primary = raw[0] if raw else {"left": 0, "top": 0, "width": 0, "height": 0}
        for idx, mon in enumerate(raw[1:], start=0):
            out.append(
                Monitor(
                    index=idx,
                    left=int(mon["left"]),
                    top=int(mon["top"]),
                    width=int(mon["width"]),
                    height=int(mon["height"]),
                    primary=(mon["left"] == primary["left"] and mon["top"] == primary["top"]),
                )
            )
        return out

    def monitor(self, index: int) -> Monitor:
        mons = self.monitors()
        if not mons:
            raise RuntimeError("no display found")
        return mons[max(0, min(index, len(mons) - 1))]

    # ------------------------------------------------------------------- frames
    def grab(
        self,
        monitor: int = 0,
        max_width: int = 1920,
        quality: int = 65,
        *,
        skip_static: bool = False,
        pixel_format: str = "jpeg",
    ) -> tuple[bytes, int, int, bool] | None:
        """Capture and encode one frame.

        Returns ``(payload, width, height, unchanged)`` or ``None`` when the
        frame was skipped because nothing on screen changed.
        """
        mons = self.monitors()
        if not mons:
            return None
        mon = mons[max(0, min(monitor, len(mons) - 1))]
        region = {"left": mon.left, "top": mon.top, "width": mon.width, "height": mon.height}
        shot = self._sct.grab(region)
        img = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")

        if skip_static:
            probe = img.convert("L").resize((48, 27))
            data = probe.tobytes()
            key = (data, (mon.left, mon.top, mon.width, mon.height))
            if self._last_print == key:
                return None
            self._last_print = key

        width = img.width
        if max_width and width > max_width:
            height = max(2, int(round(img.height * max_width / width)))
            img = img.resize((max_width, height), Image.BILINEAR)

        buf = io.BytesIO()
        if pixel_format == "png":
            img.save(buf, format="PNG", compress_level=1)
        elif pixel_format == "webp":
            img.save(buf, format="WEBP", quality=quality, method=2)
        else:
            img.save(buf, format="JPEG", quality=quality, optimize=False, subsampling=2)
        return buf.getvalue(), img.width, img.height, False

    def cursor(self) -> tuple[int, int]:
        pt = ctypes.wintypes.POINT() if hasattr(ctypes, "wintypes") else None
        try:
            import ctypes.wintypes as wt

            pt = wt.POINT()
            ctypes.windll.user32.GetCursorPos(ctypes.byref(pt))
            return int(pt.x), int(pt.y)
        except Exception:
            return (0, 0)

    def close(self) -> None:
        try:
            self._sct.close()
        except Exception:
            pass


class FramePacer:
    """Adaptive bitrate: nudges JPEG quality to hold a target frame size."""

    def __init__(self, quality: int = 65, target_kb: int = 140, min_q: int = 32, max_q: int = 92) -> None:
        self.quality = quality
        self.target_kb = target_kb
        self.min_q = min_q
        self.max_q = max_q
        self.last_size = 0
        self.last_ms = 0.0

    def observe(self, size: int, elapsed: float) -> None:
        self.last_size = size
        self.last_ms = elapsed * 1000.0
        target = self.target_kb * 1024
        if size > target * 1.25:
            self.quality = max(self.min_q, self.quality - 3)
        elif size < target * 0.6:
            self.quality = min(self.max_q, self.quality + 1)

    def set_budget_kb(self, kb: float) -> None:
        """按外部带宽上限设置单帧目标；0 表示恢复默认。"""
        self.target_kb = 140 if kb <= 0 else max(4.0, min(600.0, kb))

    def frame_budget(self, fps: int) -> float:
        return 1.0 / max(1, fps)


def probe_display() -> dict:
    cap = ScreenCapture()
    try:
        mons = cap.monitors()
        return {
            "monitors": [m.as_dict() for m in mons],
            "width": mons[0].width if mons else 0,
            "height": mons[0].height if mons else 0,
        }
    finally:
        cap.close()


if __name__ == "__main__":  # smoke test
    logging.basicConfig(level=logging.INFO)
    cap = ScreenCapture()
    for m in cap.monitors():
        print(m)
    t0 = time.perf_counter()
    frame = cap.grab(0, 1600, 60)
    dt = (time.perf_counter() - t0) * 1000
    if frame:
        print(f"frame {len(frame[0])} bytes {frame[1]}x{frame[2]} in {dt:.1f} ms")
    cap.close()
