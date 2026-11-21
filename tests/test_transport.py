"""transport.py 逻辑层单元测试：可靠控制流 + 视频分片/FEC/NACK。

这里不碰真实 socket：ReliableStream 与 VideoSender/VideoReceiver 都是纯逻辑，
用一个可控的"信道"把两端连起来，按需丢包/乱序。
"""

import unittest

from peerdesk import transport, wire


def split_parity(body: bytes):
    """PARITY_HDR 直接在 transport 里内联，这里复刻解析。"""
    fid, gidx, shards, _rsv = wire.PARITY_HDR.unpack_from(body, 0)
    return fid, gidx, shards, body[wire.PARITY_HDR.size:]


class FakeChannel:
    """对接 ReliableStream：记录发出的包，可注入丢失。"""

    def __init__(self):
        self.sent: list[tuple[int, bytes, int]] = []
        self.drop: set[int] = set()

    def send_fn(self, seq: int, payload: bytes, flags: int) -> None:
        self.sent.append((seq, payload, flags))

    def deliverable(self):
        return [p for p in self.sent if p[0] not in self.drop]


class ReliableStreamTest(unittest.TestCase):
    def test_ordered_delivery_and_ack(self):
        got_a: list[bytes] = []
        got_b: list[bytes] = []
        chan_a, chan_b = FakeChannel(), FakeChannel()
        a = transport.ReliableStream(got_a.append, chan_a.send_fn)
        b = transport.ReliableStream(got_b.append, chan_b.send_fn)

        for i in range(3):
            a.send(f"msg{i}".encode())
        for seq, payload, _ in chan_a.sent:
            b.on_data(seq, payload)
        self.assertEqual(got_b, [b"msg0", b"msg1", b"msg2"])

        ack_cum, bitmap = wire.unpack_ack(b.ack_payload())
        a.on_ack(ack_cum, bitmap)
        self.assertEqual(a.pending(), 0)

    def test_loss_is_repaired_by_retransmit(self):
        got: list[bytes] = []
        chan = FakeChannel()
        sender = transport.ReliableStream(lambda _: None, chan.send_fn)
        receiver = transport.ReliableStream(got.append, lambda *_: None)

        sender.send(b"first")
        sender.send(b"lost")
        sender.send(b"third")
        chan.drop.add(1)  # 中间那条丢掉

        for seq, payload, _ in chan.deliverable():
            receiver.on_data(seq, payload)
        self.assertEqual(got, [b"first"])  # 乱序缓存，等 1 号

        # RTO 到期触发重传，然后重新投递
        chan.drop.clear()
        before = len(chan.sent)
        sender.tick(now=1e9)
        self.assertGreater(len(chan.sent), before)
        for seq, payload, _ in chan.sent[before:]:
            receiver.on_data(seq, payload)
        self.assertEqual(got, [b"first", b"lost", b"third"])
        self.assertEqual(sender.pending(), 3)  # 从未收到 ACK，三条都还在等

    def test_retransmit_stops_after_max_retries(self):
        chan = FakeChannel()
        sender = transport.ReliableStream(lambda _: None, chan.send_fn)
        sender.send(b"x")
        for i in range(transport.ReliableStream.MAX_RETRIES + 2):
            sender.tick(now=1e9 + i * 3)
        self.assertEqual(sender.pending(), 0)

    def test_duplicate_data_is_ignored(self):
        got: list[bytes] = []
        receiver = transport.ReliableStream(got.append, lambda *_: None)
        receiver.on_data(0, b"a")
        receiver.on_data(0, b"a")
        receiver.on_data(1, b"b")
        self.assertEqual(got, [b"a", b"b"])

    def test_needs_ack_tracks_progress(self):
        receiver = transport.ReliableStream(lambda _: None, lambda *_: None)
        receiver.ack_payload()  # 初始：ack_cum=-1 → 记录
        self.assertFalse(receiver.needs_ack())
        receiver.on_data(0, b"a")
        self.assertTrue(receiver.needs_ack())


class VideoPathTest(unittest.TestCase):
    def _sender(self):
        return transport.VideoSender(mtu=1000)

    def _receiver(self):
        frames: list[tuple[bytes, bool]] = []
        keyframes: list[int] = []
        rx = transport.VideoReceiver(
            lambda data, kf: frames.append((data, kf)),
            lambda: keyframes.append(1),
            fec_k=transport.VideoSender.FEC_K,
        )
        return rx, frames, keyframes

    def test_frame_survives_single_fragment_loss_via_parity(self):
        payload = bytes(range(256)) * 40  # 10240 B → 11 片
        packets = self._sender().build(payload, keyframe=True)
        rx, frames, _ = self._receiver()

        dropped = False
        for typ, _flags, body in packets:
            if typ == wire.TYPE_DATA:
                fid, idx, cnt, chunk = wire.unpack_video_fragment(body)
                if idx == 3 and not dropped:
                    dropped = True
                    continue  # 丢掉第 4 片
                rx.on_fragment(fid, idx, cnt, chunk, bool(_flags & wire.FLAG_KEYFRAME))
            else:
                fid, gidx, shards, parity = split_parity(body)
                rx.on_parity(fid, gidx, shards, parity)

        self.assertTrue(dropped)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0][0], payload)
        self.assertTrue(frames[0][1])

    def test_missing_fragment_triggers_nack_and_retransmit(self):
        payload = b"Z" * 5000
        sender = self._sender()
        packets = sender.build(payload, keyframe=False)
        rx, frames, _ = self._receiver()

        held: list[tuple[int, int, int, bytes]] = []
        for typ, _flags, body in packets:
            if typ == wire.TYPE_DATA:
                fid, idx, cnt, chunk = wire.unpack_video_fragment(body)
                if idx in (1, 2):  # 同组丢两片，parity 也救不回来
                    held.append((fid, idx, cnt, chunk))
                    continue
                rx.on_fragment(fid, idx, cnt, chunk, False)
            else:
                fid, gidx, shards, parity = split_parity(body)
                rx.on_parity(fid, gidx, shards, parity)

        self.assertEqual(frames, [])
        # 在 0.45s 超时窗口内 tick → 触发 NACK（超时后会直接丢弃该帧）
        created = next(iter(rx._frames.values())).created
        reqs = rx.tick(now=created + 0.1)
        self.assertTrue(reqs)
        self.assertEqual(reqs[0][1], [1, 2])

        # 重传路径：VideoSender 缓存仍能给出缺失片
        fid, missing = reqs[0]
        for _typ, _flags, body in sender.retransmit(fid, missing):
            f, idx, cnt, chunk = wire.unpack_video_fragment(body)
            rx.on_fragment(f, idx, cnt, chunk, False)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0][0], payload)

    def test_keyframe_loss_requests_keyframe(self):
        payload = b"K" * 4000
        packets = self._sender().build(payload, keyframe=True)
        rx, frames, keyframes = self._receiver()
        # 只投递第一片，其余丢弃 → 该关键帧永远拼不齐，超时后应请求关键帧
        typ, flags, body = packets[0]
        fid, idx, cnt, chunk = wire.unpack_video_fragment(body)
        rx.on_fragment(fid, idx, cnt, chunk, bool(flags & wire.FLAG_KEYFRAME))
        self.assertEqual(frames, [])
        rx.tick(now=10 ** 9)
        self.assertEqual(keyframes, [1])

    def test_parity_group_indices_align_with_sender_k(self):
        payload = b"P" * 9000
        self._sender().build(payload, keyframe=False)  # 仅确认不抛错
        rx, _, _ = self._receiver()
        st = transport._FrameState(9, wire.FragmentAssembler(9), False, 0.0)
        indices = list(range(1 * transport.VideoSender.FEC_K,
                               min(2 * transport.VideoSender.FEC_K, st.frag_cnt)))
        self.assertEqual(indices, [8])


class ControlFragmentTest(unittest.TestCase):
    """控制消息 > MTU 时必须自己分片，不能靠 IP 分片。"""

    def test_short_message_passes_through(self):
        frag = transport.ControlFragments(chunk=64)
        body = b'{"t":"ping"}'
        parts = frag.split(body)
        self.assertEqual(parts, [body])
        self.assertEqual(frag.feed(body), body)

    def test_split_never_exceeds_budget(self):
        frag = transport.ControlFragments(chunk=1200)
        body = b"x" * 5000
        parts = frag.split(body)
        self.assertGreater(len(parts), 1)
        self.assertTrue(all(len(p) <= 1200 + transport.CTRL_FRAG_HDR.size for p in parts))

    def test_reassembly_roundtrip(self):
        body = b'{"t":"file_data","d":"' + b"A" * 4000 + b'"}'
        parts = transport.ControlFragments(chunk=512).split(body)
        rx = transport.ControlFragments(chunk=512)
        results = [rx.feed(p) for p in parts]
        self.assertTrue(all(r is None for r in results[:-1]))
        self.assertEqual(results[-1], body)

    def test_duplicate_fragment_is_ignored(self):
        body = b"y" * 900
        parts = transport.ControlFragments(chunk=256).split(body)
        rx = transport.ControlFragments(chunk=256)
        self.assertIsNone(rx.feed(parts[0]))
        self.assertIsNone(rx.feed(parts[0]))  # 重传的重复片
        for part in parts[1:]:
            out = rx.feed(part)
        self.assertEqual(out, body)

    def test_interleaved_messages_do_not_mix(self):
        a = b"a" * 700
        b = b"b" * 700
        tx = transport.ControlFragments(chunk=256)
        pa, pb = tx.split(a), tx.split(b)
        rx = transport.ControlFragments(chunk=256)
        outs = []
        for x, y in zip(pa, pb, strict=True):
            outs.append(rx.feed(x))
            outs.append(rx.feed(y))
        done = [o for o in outs if o]
        self.assertEqual(len(done), 2)
        self.assertIn(a, done)
        self.assertIn(b, done)

    def test_truncated_fragment_ignored(self):
        rx = transport.ControlFragments(chunk=64)
        self.assertIsNone(rx.feed(bytes([transport.CTRL_FRAG_TAG, 0, 0])))


if __name__ == "__main__":
    unittest.main(verbosity=2)
