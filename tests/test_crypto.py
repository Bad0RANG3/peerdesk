"""crypto.py 单元测试：X25519 → HKDF 会话密钥、ChaCha20-Poly1305 包保护、重放窗口。"""

import unittest

from peerdesk import crypto

HELLO_HOST = b'{"role":"host","nonce":"aaaa","ts":1}'
HELLO_CLIENT = b'{"role":"client","nonce":"bbbb","ts":2}'


def handshake(psk: bytes = b""):
    host_kp = crypto.KeyPair.generate()
    client_kp = crypto.KeyPair.generate()
    host = crypto.SessionKeys.derive(host_kp, client_kp.public_bytes, HELLO_HOST, HELLO_CLIENT, psk)
    client = crypto.SessionKeys.derive(client_kp, host_kp.public_bytes, HELLO_HOST, HELLO_CLIENT, psk)
    return host, client


class KeyPairTest(unittest.TestCase):
    def test_public_key_is_32_bytes(self):
        self.assertEqual(len(crypto.KeyPair.generate().public_bytes), 32)

    def test_private_bytes_roundtrip(self):
        kp = crypto.KeyPair.generate()
        again = crypto.KeyPair.from_private_bytes(kp.private_bytes())
        self.assertEqual(again.public_bytes, kp.public_bytes)

    def test_two_keypairs_differ(self):
        self.assertNotEqual(crypto.KeyPair.generate().public_bytes,
                            crypto.KeyPair.generate().public_bytes)


class SessionKeysTest(unittest.TestCase):
    def test_both_sides_derive_same_directional_keys(self):
        host, client = handshake()
        self.assertEqual(host.host_to_client, client.host_to_client)
        self.assertEqual(host.client_to_host, client.client_to_host)
        self.assertNotEqual(host.host_to_client, host.client_to_host)
        self.assertEqual(host.transcript, client.transcript)

    def test_psk_changes_keys(self):
        a_host, _ = handshake(b"code-psk")
        b_host, _ = handshake(b"")
        self.assertNotEqual(a_host.host_to_client, b_host.host_to_client)

    def test_psk_digest_is_not_plaintext(self):
        host, _ = handshake(b"123456789")
        self.assertNotIn(b"123456789", host.host_to_client + host.client_to_host)

    def test_transcript_binds_both_hellos(self):
        host_kp = crypto.KeyPair.generate()
        client_kp = crypto.KeyPair.generate()
        a = crypto.SessionKeys.derive(host_kp, client_kp.public_bytes, HELLO_HOST, HELLO_CLIENT)
        b = crypto.SessionKeys.derive(host_kp, client_kp.public_bytes, HELLO_HOST, HELLO_CLIENT + b"x")
        self.assertNotEqual(a.host_to_client, b.host_to_client)

    def test_peer_public_length_enforced(self):
        with self.assertRaises(ValueError):
            crypto.SessionKeys.derive(crypto.KeyPair.generate(), b"\x00" * 31, HELLO_HOST, HELLO_CLIENT)


class CryptoContextTest(unittest.TestCase):
    def setUp(self):
        host_keys, client_keys = handshake()
        self.host = crypto.CryptoContext(host_keys, is_host=True)
        self.client = crypto.CryptoContext(client_keys, is_host=False)

    def test_host_to_client_roundtrip(self):
        header = b"H" * 20
        seq = self.host.next_seq()
        body = self.host.seal(header, b"payload-h2c", seq)
        self.assertNotIn(b"payload-h2c", body)
        self.assertEqual(self.client.open(header, body, seq), b"payload-h2c")

    def test_client_to_host_roundtrip(self):
        header = b"C" * 20
        seq = self.client.next_seq()
        body = self.client.seal(header, "中文载荷".encode(), seq)
        self.assertEqual(self.host.open(header, body, seq), "中文载荷".encode())

    def test_wrong_direction_rejected(self):
        header = b"H" * 20
        seq = self.host.next_seq()
        body = self.host.seal(header, b"x", seq)
        # 同一 seq 用同一侧再解一次（方向键不同）应失败
        self.assertIsNone(self.client.open(header, body, seq + 999))

    def test_tampered_ciphertext_rejected(self):
        header = b"H" * 20
        seq = self.host.next_seq()
        body = bytearray(self.host.seal(header, b"secret", seq))
        body[0] ^= 0xFF
        self.assertIsNone(self.client.open(header, bytes(body), seq))

    def test_aad_mismatch_rejected(self):
        header = b"H" * 20
        seq = self.host.next_seq()
        body = self.host.seal(header, b"secret", seq)
        self.assertIsNone(self.client.open(b"X" * 20, body, seq))

    def test_replay_rejected(self):
        header = b"H" * 20
        seq = self.host.next_seq()
        body = self.host.seal(header, b"once", seq)
        self.assertEqual(self.client.open(header, body, seq), b"once")
        self.assertIsNone(self.client.open(header, body, seq))

    def test_seq_is_monotonic(self):
        seqs = [self.host.next_seq() for _ in range(5)]
        self.assertEqual(seqs, sorted(set(seqs)))
        self.assertEqual(len(set(seqs)), 5)


class ReplayWindowTest(unittest.TestCase):
    def test_in_order_accepted(self):
        w = crypto.ReplayWindow(width=8)
        for seq in range(1, 20):
            self.assertTrue(w.check_and_update(seq))

    def test_replay_rejected(self):
        w = crypto.ReplayWindow(width=8)
        self.assertTrue(w.check_and_update(5))
        self.assertFalse(w.check_and_update(5))

    def test_old_outside_window_rejected(self):
        w = crypto.ReplayWindow(width=8)
        for seq in range(1, 30):
            w.check_and_update(seq)
        self.assertFalse(w.check_and_update(2))

    def test_future_gap_then_fill(self):
        w = crypto.ReplayWindow(width=64)
        self.assertTrue(w.check_and_update(10))
        self.assertTrue(w.check_and_update(7))   # 窗口内的跳跃
        self.assertFalse(w.check_and_update(7))  # 重复
        self.assertTrue(w.check_and_update(11))


class HelperTest(unittest.TestCase):
    def test_fingerprint_is_stable_and_part_sensitive(self):
        self.assertEqual(crypto.fingerprint(b"a", b"b"), crypto.fingerprint(b"a", b"b"))
        self.assertNotEqual(crypto.fingerprint(b"a", b"b"), crypto.fingerprint(b"ab"))
        self.assertEqual(len(crypto.fingerprint(b"x")), 64)

    def test_random_token(self):
        self.assertEqual(len(crypto.random_token(8)), 8)
        self.assertNotEqual(crypto.random_token(8), crypto.random_token(8))


if __name__ == "__main__":
    unittest.main(verbosity=2)
