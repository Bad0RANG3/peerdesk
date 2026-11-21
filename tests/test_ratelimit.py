"""按带宽上限自适应的单测：FramePacer 目标帧大小与 HostSession 节流。"""

from __future__ import annotations

import asyncio
import unittest
from types import SimpleNamespace

from peerdesk.capture import FramePacer
from peerdesk.session import HostSession, VideoSettings


def setUpModule() -> None:
    asyncio.set_event_loop(asyncio.new_event_loop())


class StubLink:
    """HostSession 只用到这些属性/方法。"""

    established = False
    stats = SimpleNamespace(mbps=0.0, rtt_ms=0.0, mode="direct", using_relay=False)

    def __init__(self) -> None:
        self.on_control = None
        self.on_state = None
        self.on_audio = None
        self.sent: list[dict] = []

    def send_control(self, obj) -> None:
        self.sent.append(obj)


def make_host(fps: int = 15) -> HostSession:
    link = StubLink()
    return HostSession(link, "123456789", settings=VideoSettings(fps=fps), on_event=lambda e: None)


class PacerBudgetTest(unittest.TestCase):
    def test_budget_kb_clamped(self):
        pacer = FramePacer(60)
        pacer.set_budget_kb(0)
        self.assertEqual(pacer.target_kb, 140)
        pacer.set_budget_kb(1)
        self.assertEqual(pacer.target_kb, 4.0)
        pacer.set_budget_kb(5000)
        self.assertEqual(pacer.target_kb, 600.0)
        pacer.set_budget_kb(37.5)
        self.assertEqual(pacer.target_kb, 37.5)

    def test_quality_drops_toward_budget(self):
        pacer = FramePacer(80)
        pacer.set_budget_kb(10)
        for _ in range(20):
            pacer.observe(60 * 1024, 0.01)
        self.assertLess(pacer.quality, 80)
        self.assertGreaterEqual(pacer.quality, pacer.min_q)


class HostRateLimitTest(unittest.TestCase):
    def test_default_off(self):
        host = make_host()
        self.assertEqual(host.rate_limit, 0.0)
        self.assertEqual(host.pacer.target_kb, 140)
        self.assertEqual(host._effective_fps(), 15)

    def test_set_limit_sets_budget(self):
        host = make_host(fps=15)
        host.set_rate_limit(1.5)
        # 1.5 Mbps / 15 fps = 12.5 KB/帧（1.5*125000/15/1024=12.207 KB），再留 25% 余量
        self.assertAlmostEqual(host.pacer.target_kb, 1.5 * 125_000 / 15 / 1024 * 0.75, places=3)
        self.assertEqual(host.rate_limit, 1.5)

    def test_effective_fps_drops_when_frames_too_big(self):
        host = make_host(fps=15)
        host.set_rate_limit(1.5)
        host.pacer.last_size = 44 * 1024  # 44 KB/帧 = 5.3 Mbps @15fps
        fps = host._effective_fps()
        self.assertLess(fps, 15)
        self.assertGreaterEqual(fps, 1)
        self.assertLessEqual(host.pacer.last_size * 8 * fps / 1_000_000.0, 1.5 + 0.4)

    def test_clearing_limit_restores_defaults(self):
        host = make_host()
        host.set_rate_limit(2.0)
        host.set_rate_limit(0.0)
        self.assertEqual(host.pacer.target_kb, 140)
        host.pacer.last_size = 200 * 1024
        self.assertEqual(host._effective_fps(), 15)

    def test_stats_carry_cap(self):
        host = make_host()
        host.set_rate_limit(3.0)
        host._stats_at = 0.0
        host._push_stats(1.0)
        stats = [m for m in host.link.sent if m.get("t") == "stats"]
        self.assertTrue(stats)
        self.assertEqual(stats[-1]["cap"], 3.0)
        self.assertIn("sfps", stats[-1])


if __name__ == "__main__":
    unittest.main()
