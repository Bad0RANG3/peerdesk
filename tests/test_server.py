"""服务端策略单测：握手令牌、连接频次限制、中继令牌桶。"""

from __future__ import annotations

import json
import unittest

from peerdesk.server import CONNECT_MAX, SignalingServer, TokenBucket


class FakeWriter:
    def __init__(self, ip: str = "203.0.113.9") -> None:
        self.ip = ip
        self.lines: list[bytes] = []
        self.closing = False

    def write(self, data: bytes) -> None:
        self.lines.append(data)

    def is_closing(self) -> bool:
        return False

    def close(self) -> None:
        self.closing = True

    def get_extra_info(self, name: str):
        return (self.ip, 51234) if name == "peername" else None

    def messages(self) -> list[dict]:
        return [json.loads(line) for line in self.lines]


def make_peer(ip: str = "203.0.113.9"):
    from peerdesk.server import Peer

    return Peer(writer=FakeWriter(ip), role="unknown")


class TokenAuthTest(unittest.TestCase):
    def test_open_server_allows_anything(self):
        srv = SignalingServer(token="")
        peer = make_peer()
        self.assertTrue(srv._auth(peer, {}))
        self.assertTrue(srv._auth(peer, {"token": "whatever"}))
        self.assertEqual(peer.writer.lines, [])

    def test_token_required(self):
        srv = SignalingServer(token="s3cret")
        peer = make_peer()
        self.assertFalse(srv._auth(peer, {}))
        reasons = [m.get("reason") for m in peer.writer.messages()]
        self.assertEqual(reasons, ["bad_token"])
        self.assertTrue(srv._auth(peer, {"token": "s3cret"}))

    def test_wrong_token_differs_in_length(self):
        srv = SignalingServer(token="s3cret")
        peer = make_peer()
        self.assertFalse(srv._auth(peer, {"token": "s3cre"}))

    def test_non_string_token_rejected(self):
        srv = SignalingServer(token="s3cret")
        peer = make_peer()
        self.assertFalse(srv._auth(peer, {"token": 12345}))


class RateLimitTest(unittest.TestCase):
    def test_default_window(self):
        srv = SignalingServer()
        self.assertEqual(srv.connect_max, CONNECT_MAX)
        for _ in range(CONNECT_MAX):
            self.assertTrue(srv._rate_ok("198.51.100.7", now=1000.0))
        self.assertFalse(srv._rate_ok("198.51.100.7", now=1000.5))

    def test_window_slide(self):
        srv = SignalingServer(connect_max=2, connect_window=10.0)
        self.assertTrue(srv._rate_ok("198.51.100.8", now=0.0))
        self.assertTrue(srv._rate_ok("198.51.100.8", now=5.0))
        self.assertFalse(srv._rate_ok("198.51.100.8", now=6.0))
        # 11 秒后第一条已经出窗，名额回到 1 个
        self.assertTrue(srv._rate_ok("198.51.100.8", now=11.0))
        self.assertFalse(srv._rate_ok("198.51.100.8", now=11.1))

    def test_ips_are_independent(self):
        srv = SignalingServer(connect_max=1)
        self.assertTrue(srv._rate_ok("198.51.100.9", now=0.0))
        self.assertFalse(srv._rate_ok("198.51.100.9", now=0.1))
        self.assertTrue(srv._rate_ok("198.51.100.10", now=0.2))

    def test_attempt_map_is_bounded(self):
        srv = SignalingServer(connect_max=50, connect_window=1.0)
        for i in range(5000):
            srv._rate_ok(f"10.0.{i // 256}.{i % 256}", now=100.0)
        self.assertLessEqual(len(srv.attempts), 8192)


class TokenBucketTest(unittest.TestCase):
    def test_disabled_always_allows(self):
        bucket = TokenBucket(0)
        self.assertFalse(bucket.enabled)
        for _ in range(100):
            self.assertTrue(bucket.allow(1400))
        self.assertEqual(bucket.dropped, 0)
        self.assertEqual(bucket.passed, 100)

    def test_rate_conversion(self):
        bucket = TokenBucket(8)
        self.assertTrue(bucket.enabled)
        self.assertEqual(bucket.rate, 1_000_000.0)  # 8 Mbps = 1 MB/s

    def test_burst_then_drop_then_refill(self):
        bucket = TokenBucket(8, burst_sec=1.0)
        self.assertTrue(bucket.allow(1_000_000, now=0.0))
        self.assertFalse(bucket.allow(1400, now=0.0))
        self.assertEqual(bucket.dropped, 1)
        # 再过 1 秒令牌补满，能继续发
        self.assertTrue(bucket.allow(1_000_000, now=1.0))
        self.assertEqual(bucket.passed, 2)

    def test_partial_refill(self):
        bucket = TokenBucket(8, burst_sec=1.0)
        bucket.allow(bucket.capacity, now=0.0)  # 清空
        self.assertFalse(bucket.allow(200_000, now=0.0))
        self.assertTrue(bucket.allow(200_000, now=0.3))  # 0.3 s * 1 MB/s = 300 KB
        self.assertFalse(bucket.allow(200_000, now=0.35))

    def test_never_exceeds_capacity(self):
        bucket = TokenBucket(8, burst_sec=1.0)
        bucket.allow(bucket.capacity, now=0.0)
        bucket.allow(1, now=1000.0)
        self.assertLessEqual(bucket.tokens, bucket.capacity)


if __name__ == "__main__":
    unittest.main()
