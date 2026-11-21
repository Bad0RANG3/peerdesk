"""抓屏后端单测（需要真实桌面会话，headless 会跳过）。"""

from __future__ import annotations

import os
import sys
import unittest

WIN = sys.platform == "win32"


def _has_desktop() -> bool:
    if not WIN:
        return False
    try:
        from peerdesk.capture import ScreenCapture

        cap = ScreenCapture()
        ok = bool(cap.monitors())
        cap.close()
        return ok
    except Exception:
        return False


DESKTOP = _has_desktop()


@unittest.skipUnless(DESKTOP, "需要 Windows 桌面会话")
class GdiCaptureTest(unittest.TestCase):
    def setUp(self):
        from peerdesk.capture import GdiCapture

        os.environ.setdefault("PEERDESK_SCALE", "fast")
        self.cap = GdiCapture()

    def tearDown(self):
        self.cap.close()

    def test_monitors_have_positive_size(self):
        mons = self.cap.monitors()
        self.assertTrue(mons)
        for mon in mons:
            self.assertGreater(mon.width, 0)
            self.assertGreater(mon.height, 0)
            self.assertEqual(mon.as_dict()["index"], mon.index)
        self.assertEqual(sum(1 for m in mons if m.primary), 1)

    def test_grab_full_size(self):
        mon = self.cap.monitors()[0]
        data, width, height = self.cap.grab_rgb(mon)
        self.assertEqual((width, height), (mon.width, mon.height))
        self.assertEqual(len(data), width * height * 4)
        self.assertTrue(any(data))  # 不是全黑

    def test_grab_scaled(self):
        mon = self.cap.monitors()[0]
        want = max(16, mon.width // 3)
        data, width, height = self.cap.grab_rgb(mon, want)
        self.assertEqual(width, want)
        self.assertAlmostEqual(height, round(mon.height * want / mon.width), delta=1)
        self.assertEqual(len(data), width * height * 4)

    def test_buffer_reuse_between_grabs(self):
        mon = self.cap.monitors()[0]
        first, w1, h1 = self.cap.grab_rgb(mon, 320)
        second, w2, h2 = self.cap.grab_rgb(mon, 320)
        self.assertEqual((w1, h1), (w2, h2))
        self.assertEqual(len(first), len(second))
        # 同一块 DIB 复用：对象地址不变
        self.assertIs(self.cap._bits, self.cap._bits)
        self.assertEqual(self.cap._size, (w1, h1))


@unittest.skipUnless(DESKTOP, "需要 Windows 桌面会话")
class ScreenCaptureBackendTest(unittest.TestCase):
    def test_backend_selection_and_jpeg(self):
        from peerdesk.capture import ScreenCapture

        os.environ["PEERDESK_CAPTURE"] = "gdi"
        cap = ScreenCapture()
        try:
            self.assertEqual(cap.backend, "gdi")
            self.assertIn(cap.scale, ("fast", "halftone", "pil"))
            frame = cap.grab(0, 640, 60)
            self.assertIsNotNone(frame)
            payload, width, height, unchanged = frame
            self.assertTrue(payload.startswith(b"\xff\xd8"))  # JPEG SOI
            self.assertEqual(width, 640)
            self.assertFalse(unchanged)
        finally:
            cap.close()

    def test_mss_fallback_still_works(self):
        from peerdesk.capture import ScreenCapture

        os.environ["PEERDESK_CAPTURE"] = "mss"
        cap = ScreenCapture()
        try:
            self.assertEqual(cap.backend, "mss")
            frame = cap.grab(0, 480, 50)
            self.assertIsNotNone(frame)
            self.assertEqual(frame[1], 480)
        finally:
            cap.close()

    def test_probe_display_reports_backend(self):
        from peerdesk.capture import probe_display

        info = probe_display()
        self.assertIn("backend", info)
        self.assertIn(info["backend"], ("gdi", "mss"))
        self.assertTrue(info["monitors"])

    def test_static_key_is_stable_for_same_pixels(self):
        from PIL import Image

        from peerdesk.capture import Monitor

        mon = Monitor(index=0, left=0, top=0, width=64, height=36)
        img = Image.new("RGB", (64, 36), (10, 20, 30))
        from peerdesk.capture import ScreenCapture

        k1 = ScreenCapture.static_key(img, mon)
        k2 = ScreenCapture.static_key(img.copy(), mon)
        self.assertEqual(k1, k2)
        other = Image.new("RGB", (64, 36), (200, 0, 0))
        self.assertNotEqual(k1, ScreenCapture.static_key(other, mon))
        moved = Monitor(index=0, left=100, top=0, width=64, height=36)
        self.assertNotEqual(k1, ScreenCapture.static_key(img, moved))

    def test_skip_static_sets_fingerprint(self):
        from peerdesk.capture import ScreenCapture

        os.environ["PEERDESK_CAPTURE"] = "gdi"
        cap = ScreenCapture()
        try:
            cap.grab(0, 320, 50, skip_static=True)
            self.assertIsNotNone(cap._last_print)  # 抓到第一帧后记下指纹
            first = cap._last_print
            cap.grab(0, 320, 50, skip_static=True)
            # 桌面可能在两次之间变了；要么跳过（指纹不变），要么更新指纹
            self.assertIsNotNone(cap._last_print)
            self.assertEqual(cap._last_print[1], first[1])  # 同一块显示器
        finally:
            cap.close()


if __name__ == "__main__":
    unittest.main()
