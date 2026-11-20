"""wire.py 单元测试：包头、分片、XOR FEC、ACK/NACK 位图、JSON 载荷。"""

import unittest

from peerdesk import wire


def make_header(**kw):
    base = dict(
        version=wire.VERSION,
        type=wire.TYPE_DATA,
        seq=1,
        ts_ms=1,
        stream=wire.STREAM_VIDEO,
        flags=0,
        plen=0,
        session=0,
    )
    base.update(kw)
    return wire.Header(**base)


class HeaderTest(unittest.TestCase):
    def test_pack_parse_roundtrip(self):
        h = make_header(seq=4321, ts_ms=1700000, flags=wire.FLAG_KEYFRAME | wire.FLAG_LAST_FRAG,
                        plen=1200, session=7)
        raw = h.pack()
        self.assertEqual(len(raw), wire.HEADER_LEN)
        self.assertTrue(raw.startswith(wire.MAGIC))
        got = wire.parse_header(raw)
        self.assertIsNotNone(got)
        self.assertEqual((got.type, got.seq, got.ts_ms, got.stream, got.flags, got.plen, got.session),
                         (wire.TYPE_DATA, 4321, 1700000, wire.STREAM_VIDEO,
                          wire.FLAG_KEYFRAME | wire.FLAG_LAST_FRAG, 1200, 7))

    def test_parse_rejects_short_and_bad_magic(self):
        self.assertIsNone(wire.parse_header(b"PD"))
        good = make_header().pack()
        self.assertIsNone(wire.parse_header(b"XX" + good[2:]))

    def test_parse_rejects_wrong_version(self):
        good = bytearray(make_header().pack())
        good[2] = 99
        self.assertIsNone(wire.parse_header(bytes(good)))

    def test_header_defaults_flags_zero(self):
        self.assertEqual(make_header().flags, 0)


class FragmentTest(unittest.TestCase):
    def test_fragment_respects_mtu_and_rejoins(self):
        data = bytes(range(256)) * 20  # 5120 B
        chunks = wire.fragment(data, 1000)
        self.assertEqual(len(chunks), 6)
        for chunk in chunks:
            self.assertLessEqual(len(chunk), 1000)
        self.assertEqual(b"".join(chunks), data)

    def test_fragment_of_empty_is_one_empty_chunk(self):
        self.assertEqual(wire.fragment(b"", 100), [b""])

    def test_video_fragment_roundtrip(self):
        blob = wire.pack_video_fragment(9, 3, 17, b"abc")
        frame_id, idx, count, body = wire.unpack_video_fragment(blob)
        self.assertEqual((frame_id, idx, count), (9, 3, 17))
        self.assertEqual(body, b"abc")

    def test_xor_bytes_sizes(self):
        self.assertEqual(wire.xor_bytes([b"\x01\x02", b"\x03\x04"], 2), b"\x02\x06")
        # 短块按 0 补齐参与异或
        self.assertEqual(wire.xor_bytes([b"\x01\x02", b"\x03"], 2), b"\x02\x02")


class AssemblerTest(unittest.TestCase):
    def test_complete_and_assemble(self):
        asm = wire.FragmentAssembler(3)
        for idx, payload in [(0, b"aaa"), (1, b"bbb"), (2, b"ccc")]:
            self.assertTrue(asm.add(idx, payload))
        self.assertTrue(asm.complete)
        self.assertEqual(asm.assemble(), b"aaabbbccc")

    def test_duplicate_fragment_ignored(self):
        asm = wire.FragmentAssembler(2)
        self.assertTrue(asm.add(0, b"x"))
        self.assertFalse(asm.add(0, b"y"))
        self.assertEqual(asm.missing(), [1])

    def test_missing_list(self):
        asm = wire.FragmentAssembler(3)
        asm.add(0, b"aaa")
        asm.add(2, b"ccc")
        self.assertEqual(asm.missing(), [1])
        self.assertFalse(asm.complete)

    def test_parity_recovers_single_loss(self):
        chunks = [b"aaaa", b"bbbb", b"cccc", b"dddd"]
        groups = wire.ParityGroup.build(chunks, 4)
        self.assertEqual(len(groups), 1)
        group = groups[0]
        asm = wire.FragmentAssembler(4)
        for idx in (0, 2, 3):
            asm.add(idx, chunks[idx])
        self.assertFalse(asm.complete)
        self.assertTrue(asm.fill_parity(1, group.parity, [0, 1, 2, 3]))
        self.assertTrue(asm.complete)
        self.assertEqual(asm.assemble(), b"".join(chunks))

    def test_parity_refuses_two_losses(self):
        chunks = [b"aa", b"bb", b"cc", b"dd"]
        group = wire.ParityGroup.build(chunks, 4)[0]
        asm = wire.FragmentAssembler(4)
        asm.add(0, chunks[0])
        asm.add(1, chunks[1])
        self.assertFalse(asm.fill_parity(0, group.parity, [0, 1, 2, 3]))


class AckNackTest(unittest.TestCase):
    def test_ack_bitmap_roundtrip(self):
        cum, bitmap = wire.unpack_ack(wire.pack_ack(100, 0b1011))
        self.assertEqual((cum, bitmap), (100, 0b1011))

    def test_ack_wide_bitmap(self):
        big = 2 ** 60 + 5
        cum, bitmap = wire.unpack_ack(wire.pack_ack(2 ** 31, big))
        self.assertEqual(cum, 2 ** 31)
        self.assertEqual(bitmap, big)

    def test_nack_roundtrip(self):
        frame_id, idxs = wire.unpack_nack(wire.pack_nack(77, [1, 3, 5, 9]))
        self.assertEqual(frame_id, 77)
        self.assertEqual(list(idxs), [1, 3, 5, 9])

    def test_nack_accepts_tuple_and_empty(self):
        frame_id, idxs = wire.unpack_nack(wire.pack_nack(5, (2, 4)))
        self.assertEqual(list(idxs), [2, 4])
        frame_id, idxs = wire.unpack_nack(wire.pack_nack(6, []))
        self.assertEqual(frame_id, 6)
        self.assertEqual(list(idxs), [])


class JsonPayloadTest(unittest.TestCase):
    def test_roundtrip_keeps_unicode(self):
        obj = {"t": "notice", "text": "连接已建立 · 直连"}
        self.assertEqual(wire.parse_json(wire.json_payload(obj)), obj)

    def test_payload_is_compact_utf8(self):
        payload = wire.json_payload({"t": "ping"})
        self.assertNotIn(b" ", payload)
        self.assertEqual(payload[:1], b"{")
        self.assertEqual(payload, b'{"t":"ping"}')

    def test_parse_json_rejects_garbage(self):
        self.assertIsNone(wire.parse_json(b"not json"))
        self.assertIsNone(wire.parse_json(b""))


if __name__ == "__main__":
    unittest.main(verbosity=2)
