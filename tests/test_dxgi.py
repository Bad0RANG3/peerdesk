"""DXGI 后端单测：ABI、行宽整理、子进程探测与回退路径。"""

from __future__ import annotations

import ctypes
import json
import os
import subprocess
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

from peerdesk import capture, dxgi


class DxgiAbiTest(unittest.TestCase):
    def test_mode_desc_layout(self):
        self.assertEqual(ctypes.sizeof(dxgi._DXGI_MODE_DESC), 28)
        self.assertEqual(dxgi._DXGI_MODE_DESC.Scaling.offset, 24)
        self.assertEqual(ctypes.sizeof(dxgi._DXGI_RATIONAL), 8)

    def test_duplication_desc_layout(self):
        self.assertEqual(ctypes.sizeof(dxgi._DXGI_OUTDUPL_DESC), 36)
        self.assertEqual(dxgi._DXGI_OUTDUPL_DESC.Rotation.offset, 28)
        self.assertEqual(dxgi._DXGI_OUTDUPL_DESC.DesktopImageInSystemMemory.offset, 32)

    def test_frame_info_and_output_desc_layout(self):
        self.assertEqual(ctypes.sizeof(dxgi._DXGI_OUTDUPL_FRAME_INFO), 48)
        self.assertEqual(ctypes.sizeof(dxgi._DXGI_OUTPUT_DESC), 96)
        self.assertEqual(ctypes.sizeof(dxgi._D3D11_TEXTURE2D_DESC), 44)
        self.assertEqual(ctypes.sizeof(dxgi._D3D11_MAPPED_SUBRESOURCE), 16)


class PackRowsTest(unittest.TestCase):
    def test_padding_is_removed_per_row(self):
        width, height = 2, 3
        stride = width * 4
        row_pitch = stride + 4
        rows = [bytes([index]) * stride for index in range(height)]
        raw = b"".join(row + b"\xee" * (row_pitch - stride) for row in rows)

        packed = dxgi._pack_rows(raw, row_pitch, width, height)

        self.assertEqual(packed, b"".join(rows))
        self.assertEqual(len(packed), stride * height)

    def test_contiguous_rows_are_returned_unchanged(self):
        raw = bytes(range(24))
        self.assertIs(dxgi._pack_rows(raw, 8, 2, 3), raw)


class ProbeSubprocessTest(unittest.TestCase):
    @staticmethod
    def _completed(stdout: str = "", stderr: str = "", returncode: int = 0):
        return SimpleNamespace(stdout=stdout, stderr=stderr, returncode=returncode)

    def test_success_parses_last_json_line(self):
        payload = {"ok": True, "width": 2560, "height": 1440, "monitors": 1}
        completed = self._completed(
            stdout="noise\n" + json.dumps(payload) + "\n",
            returncode=0,
        )
        with mock.patch.object(dxgi.subprocess, "run", return_value=completed) as run:
            result = dxgi._run_probe_subprocess()

        self.assertTrue(result["ok"])
        self.assertEqual(result["width"], 2560)
        self.assertIsInstance(result["elapsed_ms"], float)
        self.assertEqual(run.call_args.kwargs["timeout"], dxgi._PROBE_TIMEOUT)

    def test_nonzero_exit_invalidates_json_result(self):
        completed = self._completed(stdout=json.dumps({"ok": True}), returncode=3)
        with mock.patch.object(dxgi.subprocess, "run", return_value=completed):
            result = dxgi._run_probe_subprocess()

        self.assertFalse(result["ok"])
        self.assertEqual(result["returncode"], 3)
        self.assertIn("code 3", result["error"])

    def test_timeout_and_start_error_are_reported(self):
        with mock.patch.object(
            dxgi.subprocess,
            "run",
            side_effect=subprocess.TimeoutExpired("probe", dxgi._PROBE_TIMEOUT),
        ):
            timeout = dxgi._run_probe_subprocess()
        with mock.patch.object(
            dxgi.subprocess,
            "run",
            side_effect=OSError("denied"),
        ):
            failed = dxgi._run_probe_subprocess()

        self.assertFalse(timeout["ok"])
        self.assertIn("timed out", timeout["error"])
        self.assertFalse(failed["ok"])
        self.assertIn("denied", failed["error"])

    def test_missing_json_reports_stderr_tail(self):
        completed = self._completed(stdout="not json\n", stderr="first\nlast line\n", returncode=1)
        with mock.patch.object(dxgi.subprocess, "run", return_value=completed):
            result = dxgi._run_probe_subprocess()

        self.assertFalse(result["ok"])
        self.assertIn("last line", result["error"])
        self.assertEqual(result["returncode"], 1)


class ProbeCacheTest(unittest.TestCase):
    def setUp(self):
        cached = mock.patch.object(dxgi, "_probe_result", None)
        cached.start()
        self.addCleanup(cached.stop)

    def test_result_is_cached_and_copied(self):
        with (
            mock.patch.dict(os.environ, {"PEERDESK_DXGI_PROBE": "1"}),
            mock.patch.object(
                dxgi,
                "_run_probe_subprocess",
                return_value={"ok": True, "elapsed_ms": 1.0},
            ) as run,
        ):
            first = dxgi.probe_dxgi()
            first["ok"] = False
            second = dxgi.probe_dxgi()

        self.assertEqual(run.call_count, 1)
        self.assertTrue(second["ok"])

    def test_environment_switch_disables_probe(self):
        with (
            mock.patch.dict(os.environ, {"PEERDESK_DXGI_PROBE": "0"}),
            mock.patch.object(dxgi, "_run_probe_subprocess") as run,
        ):
            result = dxgi.probe_dxgi()

        run.assert_not_called()
        self.assertFalse(result["ok"])
        self.assertIn("disabled", result["error"])


class _FakeGdi:
    instances: list[_FakeGdi] = []

    def __init__(self, *args):
        self.args = args
        self.closed = False
        self.instances.append(self)

    def grab_rgb(self, mon, out_width=0):
        width = min(4, out_width) if out_width else 4
        height = max(2, round(3 * width / 4))
        return b"\x00\x00\xff\x00" * (width * height), width, height

    def monitors(self):
        return [capture.Monitor(index=0, left=0, top=0, width=640, height=480, primary=True)]

    def close(self):
        self.closed = True


class _FakeDxgi:
    def __init__(self, *, fail=False):
        self.fail = fail
        self.closed = False

    def grab_rgb(self, mon):
        if self.fail:
            raise dxgi.DxgiError("simulated access loss")
        return b"\x00\x00\xff\x00" * 12, 4, 3

    def close(self):
        self.closed = True


class ScreenCaptureFallbackTest(unittest.TestCase):
    MON = capture.Monitor(index=0, left=0, top=0, width=640, height=480, primary=True)

    def setUp(self):
        _FakeGdi.instances.clear()
        dpi = mock.patch.object(capture, "_dpi_aware")
        dpi.start()
        self.addCleanup(dpi.stop)

    def _capture(
        self,
        *,
        backend=None,
        want: str = "auto",
        scale: str = "fast",
    ) -> capture.ScreenCapture:
        cap = capture.ScreenCapture.__new__(capture.ScreenCapture)
        cap.scale = scale
        cap._want = want
        cap._dxgi = backend
        cap._gdi = None
        cap._sct = None
        cap._test = None
        cap.backend = "dxgi" if backend is not None else ""
        cap._last_print = None
        cap._cursor_visible = True
        return cap

    def test_auto_probe_failure_falls_back_to_gdi(self):
        with (
            mock.patch.dict(os.environ, {"PEERDESK_CAPTURE": "auto"}),
            mock.patch.object(
                dxgi,
                "probe_dxgi",
                return_value={"ok": False, "error": "simulated failure"},
            ),
            mock.patch.object(dxgi, "DxgiCapture") as dxgi_cls,
            mock.patch.object(capture, "GdiCapture", _FakeGdi),
        ):
            cap = capture.ScreenCapture()
            try:
                self.assertEqual(cap.backend, "gdi")
            finally:
                cap.close()

        dxgi_cls.assert_not_called()
        self.assertEqual(len(_FakeGdi.instances), 1)
        self.assertTrue(_FakeGdi.instances[0].closed)

    def test_explicit_dxgi_failure_raises_without_gdi_fallback(self):
        with (
            mock.patch.dict(os.environ, {"PEERDESK_CAPTURE": "dxgi"}),
            mock.patch.object(
                dxgi,
                "probe_dxgi",
                return_value={"ok": False, "error": "simulated failure"},
            ),
            mock.patch.object(capture, "GdiCapture", _FakeGdi),
            self.assertRaisesRegex(RuntimeError, "DXGI 不可用"),
        ):
            capture.ScreenCapture()

        self.assertEqual(_FakeGdi.instances, [])

    def test_runtime_dxgi_error_falls_back_to_gdi(self):
        backend = _FakeDxgi(fail=True)
        cap = self._capture(backend=backend)
        with mock.patch.object(capture, "GdiCapture", _FakeGdi):
            image = cap._grab_image(self.MON, 2)

        self.assertEqual(image.size, (2, 2))
        self.assertEqual(cap.backend, "gdi")
        self.assertIsNone(cap._dxgi)
        self.assertTrue(backend.closed)

    def test_scale_pil_keeps_dxgi_frame_full_size(self):
        backend = _FakeDxgi()
        cap = self._capture(backend=backend, scale="pil")

        image = cap._grab_image(self.MON, 2)

        self.assertEqual(image.size, (4, 3))


@unittest.skipUnless(sys.platform == "win32", "DXGI 仅 Windows")
class DxgiLifecycleTest(unittest.TestCase):
    def test_repeated_create_grab_close(self):
        try:
            backend = dxgi.DxgiCapture()
        except Exception as exc:
            self.skipTest(f"DXGI unavailable: {exc}")
        backend.close()

        for cycle in range(3):
            with self.subTest(cycle=cycle):
                cap = dxgi.DxgiCapture()
                try:
                    monitor = cap.monitors()[0]
                    data, width, height = cap.grab_rgb(monitor)
                    self.assertGreater(width, 0)
                    self.assertGreater(height, 0)
                    self.assertEqual(len(data), width * height * 4)
                finally:
                    cap.close()


if __name__ == "__main__":
    unittest.main()
