"""util.py 单元测试：连接码、PSK 派生、地址解析、格式化。"""

import unittest

from peerdesk import util


class CodeTest(unittest.TestCase):
    def test_generate_code_shape(self):
        for _ in range(200):
            code = util.generate_code()
            self.assertEqual(len(code), 9)
            self.assertTrue(code.isdigit())
            self.assertNotEqual(code[0], "0")

    def test_generate_code_varies(self):
        self.assertGreater(len({util.generate_code() for _ in range(50)}), 40)

    def test_normalize_strips_formatting(self):
        self.assertEqual(util.normalize_code("604 173 829"), "604173829")
        self.assertEqual(util.normalize_code(" 604-173-829 "), "604173829")
        self.assertEqual(util.normalize_code(None), "")

    def test_format_groups_of_three(self):
        self.assertEqual(util.format_code("604173829"), "604 173 829")
        self.assertEqual(util.format_code("604173"), "604 173")

    def test_validity(self):
        self.assertTrue(util.code_is_valid("604 173 829"))
        self.assertFalse(util.code_is_valid("12345678"))
        self.assertFalse(util.code_is_valid("1234567890"))
        self.assertFalse(util.code_is_valid("004 173 829"))  # 首位不能是 0

    def test_psk_is_deterministic_and_code_bound(self):
        a = util.psk_from_code("604 173 829")
        b = util.psk_from_code("604173829")
        self.assertEqual(a, b)
        self.assertEqual(len(a), 32)
        self.assertNotEqual(a, util.psk_from_code("604173820"))

    def test_psk_salt_changes_key(self):
        self.assertNotEqual(util.psk_from_code("604173829"),
                            util.psk_from_code("604173829", b"salt"))

    def test_psk_does_not_leak_code(self):
        self.assertNotIn(b"604173829", util.psk_from_code("604173829"))

    def test_code_id_is_short_public_hash(self):
        cid = util.code_id("604 173 829")
        self.assertEqual(len(cid), 32)
        self.assertEqual(cid, util.code_id("604173829"))
        self.assertNotEqual(cid, util.code_id("604173820"))


class AddrTest(unittest.TestCase):
    def test_parse_hostport(self):
        self.assertEqual(util.parse_addr("127.0.0.1:7900"), ("127.0.0.1", 7900))
        self.assertEqual(util.parse_addr("example.com:1"), ("example.com", 1))

    def test_parse_defaults(self):
        self.assertEqual(util.parse_addr("", 7900), ("127.0.0.1", 7900))
        self.assertEqual(util.parse_addr("10.0.0.5", 7), ("10.0.0.5", 7))
        self.assertEqual(util.parse_addr("10.0.0.5:", 7), ("10.0.0.5", 7))

    def test_parse_ipv6_bracket(self):
        self.assertEqual(util.parse_addr("[::1]:7900"), ("::1", 7900))
        self.assertEqual(util.parse_addr("[fe80::1]"), ("fe80::1", 0))

    def test_bare_ipv6_kept_whole(self):
        self.assertEqual(util.parse_addr("fe80::1"), ("fe80::1", 0))

    def test_format_addr(self):
        self.assertEqual(util.format_addr("1.2.3.4", 80), "1.2.3.4:80")
        self.assertEqual(util.format_addr("::1", 80), "[::1]:80")

    def test_roundtrip(self):
        for text in ("127.0.0.1:7900", "[::1]:7900"):
            host, port = util.parse_addr(text)
            self.assertEqual(util.format_addr(host, port), text)


class HumanBytesTest(unittest.TestCase):
    def test_units(self):
        self.assertEqual(util.human_bytes(0), "0.0B")
        self.assertEqual(util.human_bytes(512), "512.0B")
        self.assertEqual(util.human_bytes(1536), "1.5KB")
        self.assertEqual(util.human_bytes(5 * 1024 * 1024), "5.0MB")
        self.assertTrue(util.human_bytes(3 * 1024 ** 3).endswith("GB"))


class LocalIpsTest(unittest.TestCase):
    def test_returns_plain_ipv4_without_loopback(self):
        ips = util.local_ipv4s()
        self.assertIsInstance(ips, list)
        for ip in ips:
            self.assertNotIn(":", ip)
            self.assertFalse(ip.startswith("127."))
            self.assertNotEqual(ip, "0.0.0.0")


if __name__ == "__main__":
    unittest.main(verbosity=2)
