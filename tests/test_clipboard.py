"""剪贴板 ctypes 签名与采集循环容错的回归测试。

真实 bug（0.5.0 现场发现）：`GetClipboardData` / `GlobalLock` 没声明 restype，
ctypes 默认按 `c_int` 返回，64 位进程里句柄被截断成低 32 位，
`GlobalLock(截断的句柄)` 直接 access violation，把整个采集循环带走 ——
现象是"控制端一连上，被控端画面就全黑、HUD 全是 —"。
"""

from __future__ import annotations

import asyncio
import ctypes
import ctypes.wintypes as wt
import unittest
from types import SimpleNamespace

WINDOWS = hasattr(ctypes, "WinDLL")


@unittest.skipUnless(WINDOWS, "Windows only")
class ClipboardSignatureTest(unittest.TestCase):
    def test_handles_are_pointer_sized(self):
        from peerdesk import wininput

        self.assertIs(wininput.kernel32.GlobalLock.restype, ctypes.c_void_p)
        self.assertIs(wininput.kernel32.GlobalAlloc.restype, wt.HANDLE)
        self.assertIs(wininput.user32.GetClipboardData.restype, wt.HANDLE)
        self.assertIs(wininput.user32.SetClipboardData.restype, wt.HANDLE)

    def test_arguments_declared(self):
        from peerdesk import wininput

        self.assertEqual(list(wininput.user32.GetClipboardData.argtypes), [wt.UINT])
        self.assertEqual(list(wininput.kernel32.GlobalUnlock.argtypes), [wt.HANDLE])
        self.assertIs(wininput.user32.OpenClipboard.restype, wt.BOOL)

    def test_text_roundtrip(self):
        from peerdesk import wininput

        # 只读一次、写回同一内容，避免破坏用户剪贴板
        before = wininput.get_clipboard_text()
        self.assertIsInstance(before, str)
        probe = "PeerDesk clipboard probe 中文 ✓"
        if not wininput.set_clipboard_text(probe):
            self.skipTest("剪贴板被别的进程占用")
        self.assertEqual(wininput.get_clipboard_text(), probe)
        wininput.set_clipboard_text(before)


class _BoomWatcher:
    def poll(self):
        raise OSError("clipboard is on fire")


class _StubLink:
    established = False
    stats = SimpleNamespace(mbps=0.0, rtt_ms=0.0, mode="direct", using_relay=False)

    def __init__(self) -> None:
        self.on_control = None
        self.on_state = None
        self.on_audio = None
        self.sent: list[dict] = []

    def send_control(self, obj) -> None:
        self.sent.append(obj)


def setUpModule() -> None:  # HostSession.__init__ 会 get_event_loop()
    asyncio.set_event_loop(asyncio.new_event_loop())


@unittest.skipUnless(WINDOWS, "Windows only")
class CaptureLoopResilienceTest(unittest.TestCase):
    """输入/剪贴板出问题不许把采集循环带走。"""

    def _session(self):
        from peerdesk.session import HostSession, VideoSettings

        try:  # asyncio.run() 会把当前事件循环清掉，每个用例自己补一个
            asyncio.get_event_loop()
        except RuntimeError:
            asyncio.set_event_loop(asyncio.new_event_loop())

        link = _StubLink()
        session = HostSession(link, "123456789", settings=VideoSettings(fps=30), on_event=lambda e: None)
        session.allow_input = True
        return session, link

    def test_lazy_input_failure_sets_flag(self):
        session, _ = self._session()

        def boom():
            raise OSError("access violation reading 0xFFFFFFFF8CFB75D0")

        session._lazy_input = boom
        session._running = True
        session._grab_once = lambda: None
        session._poll_clipboard = lambda: None
        session._push_stats = lambda now: None
        session._effective_fps = lambda: 30

        async def run() -> None:
            session._loop = asyncio.get_running_loop()   # 别继承 setUpModule 里那个
            session.link.established = True
            iterations = 0

            async def stop_after_two(_delay):
                nonlocal iterations
                iterations += 1
                if iterations >= 2:
                    session.link.established = False

            real_sleep = asyncio.sleep
            asyncio.sleep = stop_after_two
            try:
                await asyncio.wait_for(session._capture_loop(), 2.0)
            finally:
                asyncio.sleep = real_sleep

        asyncio.run(run())
        self.assertTrue(session._input_failed)

    def test_clipboard_error_does_not_kill_loop(self):
        session, link = self._session()
        session._injector = object()  # 假装已经初始化过，跳过 _lazy_input
        session._clip_watcher = _BoomWatcher()
        session.settings.clipboard = True
        session._running = True
        session._grab_once = lambda: None
        session._push_stats = lambda now: None
        session._effective_fps = lambda: 30

        async def run() -> None:
            session._loop = asyncio.get_running_loop()   # 别继承 setUpModule 里那个
            session.link.established = True
            iterations = 0

            async def stop_after_two(_delay):
                nonlocal iterations
                iterations += 1
                if iterations >= 2:
                    session.link.established = False

            real_sleep = asyncio.sleep
            asyncio.sleep = stop_after_two
            try:
                await asyncio.wait_for(session._capture_loop(), 2.0)
            finally:
                asyncio.sleep = real_sleep

        asyncio.run(run())
        self.assertEqual(link.sent, [])  # 没有崩、也没有乱发消息
