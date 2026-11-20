"""stun.py 单元测试：STUN Binding 请求/响应编解码与 XOR-MAPPED-ADDRESS。"""

import struct
import unittest

from peerdesk import stun


class BuildTest(unittest.TestCase):
    def test_request_shape(self):
        pkt, txid = stun.build_request()
        self.assertEqual(len(txid), 12)
        mtype, length, magic, got_txid = stun.HEADER.unpack_from(pkt, 0)
        self.assertEqual(mtype, stun.BINDING_REQUEST)
        self.assertEqual(magic, stun.MAGIC)
        self.assertEqual(got_txid, txid)
        self.assertEqual(len(pkt), 20 + length)

    def test_request_txid_is_random(self):
        _, a = stun.build_request()
        _, b = stun.build_request()
        self.assertNotEqual(a, b)

    def test_request_accepts_explicit_txid(self):
        txid = bytes(range(12))
        pkt, got = stun.build_request(txid)
        self.assertEqual(got, txid)
        self.assertEqual(stun.parse_message(pkt)[1], txid)


class ParseTest(unittest.TestCase):
    def test_malformed_rejected(self):
        self.assertIsNone(stun.parse_message(b""))
        self.assertIsNone(stun.parse_message(b"\x00" * 12))
        bad = bytearray(stun.build_request()[0])
        bad[4:8] = b"\x00\x00\x00\x00"  # 打坏 magic cookie
        self.assertIsNone(stun.parse_message(bytes(bad)))

    def test_request_parses_without_attrs(self):
        pkt, txid = stun.build_request()
        mtype, got_txid, attrs = stun.parse_message(pkt)
        self.assertEqual(mtype, stun.BINDING_REQUEST)
        self.assertEqual(got_txid, txid)
        self.assertEqual(attrs, {})


class MappedAddressTest(unittest.TestCase):
    def test_ipv4_roundtrip(self):
        txid = stun.build_request()[1]
        resp = stun.build_response(txid, "203.0.113.9", 45678)
        mtype, got_txid, attrs = stun.parse_message(resp)
        self.assertEqual(mtype, stun.BINDING_SUCCESS)
        self.assertEqual(got_txid, txid)
        self.assertIn(stun.ATTR_XOR_MAPPED, attrs)
        self.assertEqual(stun.parse_mapped(attrs, txid), ("203.0.113.9", 45678))

    def test_xor_mapped_is_not_plaintext(self):
        txid = stun.build_request()[1]
        resp = stun.build_response(txid, "203.0.113.9", 45678)
        self.assertNotIn(bytes([203, 0, 113, 9]), resp)

    def test_parse_mapped_without_attr(self):
        self.assertIsNone(stun.parse_mapped({}, bytes(12)))

    def test_parse_mapped_rejects_truncated(self):
        txid = bytes(12)
        self.assertIsNone(stun.parse_mapped({stun.ATTR_XOR_MAPPED: b"\x00\x01\x00"}, txid))

    def test_parse_mapped_rejects_bad_family(self):
        txid = bytes(12)
        attr = struct.pack(">BBH", 0, 0x03, 1234) + b"\x01\x02\x03\x04"
        self.assertIsNone(stun.parse_mapped({stun.ATTR_XOR_MAPPED: attr}, txid))

    def test_parse_mapped_rejects_short_ipv6(self):
        txid = bytes(12)
        attr = struct.pack(">BBH", 0, 0x02, 1234) + b"\x01\x02\x03\x04"
        self.assertIsNone(stun.parse_mapped({stun.ATTR_XOR_MAPPED: attr}, txid))

    def test_parse_mapped_ipv4_ignores_trailing_bytes(self):
        txid = bytes(12)
        attr = struct.pack(">BBH", 0, 0x01, 1234) + b"\x01\x02\x03\x04" + b"\xff\xff"
        host, _port = stun.parse_mapped({stun.ATTR_MAPPED: attr}, txid)
        self.assertEqual(host, "1.2.3.4")


if __name__ == "__main__":
    unittest.main(verbosity=2)
