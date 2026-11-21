"""Screen capture and JPEG encoding.

两套抓屏后端，都用 ctypes + Pillow，不引入需要编译的依赖：

- ``gdi``（默认）：持久 DIB section + ``BitBlt``，每帧只拷像素，2560×1440 约 12 ms；
- ``mss``：``mss`` 每次抓屏新建上下文，同样分辨率约 19 ms。

选择顺序：环境变量 ``PEERDESK_CAPTURE=gdi|mss`` 强制指定，否则先试 gdi，失败回退 mss。
"""

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


SRCCOPY = 0x00CC0020
CAPTUREBLT = 0x40000000
HALFTONE = 4
COLORONCOLOR = 3
SCALE_MODES = {"fast": COLORONCOLOR, "halftone": HALFTONE, "pil": 0}
DIB_RGB_COLORS = 0
BI_RGB = 0
MONITORINFOF_PRIMARY = 0x1


class _RECT(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_long),
        ("top", ctypes.c_long),
        ("right", ctypes.c_long),
        ("bottom", ctypes.c_long),
    ]


class _MONITORINFO(ctypes.Structure):
    _fields_ = [
        ("cbSize", ctypes.c_ulong),
        ("rcMonitor", _RECT),
        ("rcWork", _RECT),
        ("dwFlags", ctypes.c_ulong),
    ]


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", ctypes.c_ulong),
        ("biWidth", ctypes.c_long),
        ("biHeight", ctypes.c_long),
        ("biPlanes", ctypes.c_ushort),
        ("biBitCount", ctypes.c_ushort),
        ("biCompression", ctypes.c_ulong),
        ("biSizeImage", ctypes.c_ulong),
        ("biXPelsPerMeter", ctypes.c_long),
        ("biYPelsPerMeter", ctypes.c_long),
        ("biClrUsed", ctypes.c_ulong),
        ("biClrImportant", ctypes.c_ulong),
    ]


class GdiCapture:
    """持久 DIB + BitBlt。

    ``BitBlt`` 只认桌面左上角为原点的坐标，所以每个显示器直接用它的
    ``rcMonitor``；缓冲区按需重建（分辨率变化时）。
    """

    def __init__(self, scale_mode: int = COLORONCOLOR) -> None:
        self.scale_mode = scale_mode
        self.user32 = ctypes.windll.user32
        self.gdi32 = ctypes.windll.gdi32
        self._hdc_screen = self.user32.GetDC(None)
        if not self._hdc_screen:
            raise OSError("GetDC 失败")
        self._hdc_mem = self.gdi32.CreateCompatibleDC(self._hdc_screen)
        if not self._hdc_mem:
            raise OSError("CreateCompatibleDC 失败")
        self._hbmp = None
        self._old = None
        self._bits = None
        self._size = (0, 0)

    # ------------------------------------------------------------------ layout
    def monitors(self) -> list[Monitor]:
        found: list[tuple[int, int, int, int, bool]] = []

        proto = ctypes.WINFUNCTYPE(
            ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.POINTER(_RECT), ctypes.c_double
        )

        def cb(hmon, hdc, lprc, data):
            mi = _MONITORINFO()
            mi.cbSize = ctypes.sizeof(_MONITORINFO)
            if self.user32.GetMonitorInfoW(hmon, ctypes.byref(mi)):
                r = mi.rcMonitor
                found.append(
                    (r.left, r.top, r.right - r.left, r.bottom - r.top, bool(mi.dwFlags & MONITORINFOF_PRIMARY))
                )
            return 1

        self.user32.EnumDisplayMonitors(None, None, proto(cb), 0)
        found.sort(key=lambda t: (t[0], t[1]))  # 左到右
        return [
            Monitor(index=i, left=left, top=top, width=width, height=height, primary=primary)
            for i, (left, top, width, height, primary) in enumerate(found)
        ]

    # ------------------------------------------------------------------- frames
    def _ensure_buffer(self, width: int, height: int) -> None:
        if self._size == (width, height) and self._bits is not None:
            return
        self._release_bitmap()
        bmi = _BITMAPINFOHEADER()
        bmi.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
        bmi.biWidth = width
        bmi.biHeight = -height  # 负数 = 自上而下
        bmi.biPlanes = 1
        bmi.biBitCount = 32
        bmi.biCompression = BI_RGB
        bits = ctypes.c_void_p()
        self._hbmp = self.gdi32.CreateDIBSection(
            self._hdc_screen, ctypes.byref(bmi), DIB_RGB_COLORS, ctypes.byref(bits), None, 0
        )
        if not self._hbmp or not bits:
            raise OSError("CreateDIBSection 失败")
        self._old = self.gdi32.SelectObject(self._hdc_mem, self._hbmp)
        self._bits = bits
        self._size = (width, height)

    def _release_bitmap(self) -> None:
        if self._hbmp and self._hdc_mem:
            self.gdi32.SelectObject(self._hdc_mem, self._old)
            self.gdi32.DeleteObject(self._hbmp)
        self._hbmp = None
        self._old = None
        self._bits = None
        self._size = (0, 0)

    def grab_rgb(self, mon: Monitor, out_width: int = 0) -> tuple[bytes, int, int]:
        """把显示器画面拷进持久 DIB，返回 BGRA 字节（行宽 = 输出宽度×4）。

        ``out_width`` 非 0 且小于显示器宽度时用 ``StretchBlt`` + HALFTONE 在 GDI 里
        直接缩放，省掉后面 Pillow 的 resize（实测省 10–15 ms/帧）。
        """
        out_w = mon.width if not out_width or out_width >= mon.width else int(out_width)
        out_h = mon.height if out_w == mon.width else max(2, round(mon.height * out_w / mon.width))
        self._ensure_buffer(out_w, out_h)
        if out_w != mon.width:
            self.gdi32.SetStretchBltMode(self._hdc_mem, self.scale_mode)
            self.gdi32.SetBrushOrgEx(self._hdc_mem, 0, 0, None)
            ok = self.gdi32.StretchBlt(
                self._hdc_mem, 0, 0, out_w, out_h,
                self._hdc_screen, mon.left, mon.top, mon.width, mon.height,
                SRCCOPY | CAPTUREBLT,
            )
        else:
            ok = self.gdi32.BitBlt(
                self._hdc_mem, 0, 0, out_w, out_h, self._hdc_screen, mon.left, mon.top, SRCCOPY | CAPTUREBLT
            )
            if not ok:
                ok = self.gdi32.BitBlt(
                    self._hdc_mem, 0, 0, out_w, out_h, self._hdc_screen, mon.left, mon.top, SRCCOPY
                )
        if not ok:
            raise OSError("BitBlt/StretchBlt 失败")
        data = ctypes.string_at(self._bits, out_w * out_h * 4)
        return data, out_w, out_h

    def close(self) -> None:
        self._release_bitmap()
        if self._hdc_mem:
            self.gdi32.DeleteDC(self._hdc_mem)
            self._hdc_mem = None
        if self._hdc_screen:
            self.user32.ReleaseDC(None, self._hdc_screen)
            self._hdc_screen = None


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
    """抓屏器，线程不安全，每个线程一个实例。

    后端选择：``PEERDESK_CAPTURE`` 环境变量可强制 ``gdi`` / ``mss``，默认先 gdi 再回退 mss。
    """

    def __init__(self) -> None:
        _dpi_aware()
        import os

        want = (os.environ.get("PEERDESK_CAPTURE") or "").strip().lower()
        scale = (os.environ.get("PEERDESK_SCALE") or "fast").strip().lower()
        self.scale = scale if scale in SCALE_MODES else "fast"
        self._gdi: GdiCapture | None = None
        self._sct = None
        if want in ("", "gdi"):
            try:
                self._gdi = GdiCapture(SCALE_MODES[self.scale])
            except Exception as exc:  # 没有桌面会话 / 权限不足时退回 mss
                if want == "gdi":
                    raise
                log.info("gdi 后端不可用（%s），回退 mss", exc)
        if self._gdi is None:
            import mss  # 延迟导入，headless CI 上也能 import 本模块

            factory = getattr(mss, "MSS", None) or mss.mss
            self._sct = factory()
        self.backend = "gdi" if self._gdi is not None else "mss"
        self._last_print: tuple[bytes, tuple[int, int, int, int]] | None = None
        self._cursor_visible = True

    # ------------------------------------------------------------------ layout
    def monitors(self) -> list[Monitor]:
        if self._gdi is not None:
            return self._gdi.monitors()
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
        img = self._grab_image(mon, max_width)

        if skip_static:
            key = self.static_key(img, mon)
            if self._last_print == key:
                return None
            self._last_print = key

        width = img.width
        if max_width and width > max_width and (self._gdi is None or self.scale == "pil"):
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

    @staticmethod
    def static_key(img: Image.Image, mon: Monitor) -> tuple[bytes, tuple[int, int, int, int]]:
        """静帧判定用的指纹：压到 48×27 灰度后再比较。"""
        probe = img.convert("L").resize((48, 27))
        return probe.tobytes(), (mon.left, mon.top, mon.width, mon.height)

    def _grab_image(self, mon: Monitor, out_width: int = 0) -> Image.Image:
        if self._gdi is not None:
            # scale=pil 时把缩放交给 Pillow（质量最好、最慢）
            data, width, height = self._gdi.grab_rgb(mon, 0 if self.scale == "pil" else out_width)
            return Image.frombytes("RGB", (width, height), data, "raw", "BGRX")
        region = {"left": mon.left, "top": mon.top, "width": mon.width, "height": mon.height}
        shot = self._sct.grab(region)
        return Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")

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
        if self._gdi is not None:
            try:
                self._gdi.close()
            except Exception:
                pass
            self._gdi = None
        if self._sct is not None:
            try:
                self._sct.close()
            except Exception:
                pass
            self._sct = None


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
            "backend": cap.backend,
            "scale": cap.scale,
            "monitors": [m.as_dict() for m in mons],
            "width": mons[0].width if mons else 0,
            "height": mons[0].height if mons else 0,
        }
    finally:
        cap.close()


if __name__ == "__main__":  # smoke test
    logging.basicConfig(level=logging.INFO)
    cap = ScreenCapture()
    print(f"backend: {cap.backend}  scale: {cap.scale}")
    for m in cap.monitors():
        print(m)
    t0 = time.perf_counter()
    frame = cap.grab(0, 1600, 60)
    dt = (time.perf_counter() - t0) * 1000
    if frame:
        print(f"frame {len(frame[0])} bytes {frame[1]}x{frame[2]} in {dt:.1f} ms")
    cap.close()
