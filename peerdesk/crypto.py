"""PeerDesk session crypto.

Design
------
* X25519 ECDH for key agreement (ephemeral, per session).
* HKDF-SHA256 to expand the shared secret into two directional AEAD keys.
* ChaCha20-Poly1305 for every packet after the handshake.
* Optional pre-shared secret (the 9-digit connection code) is folded into the
  HKDF salt, so an attacker that only controls the signaling channel cannot
  mount a MITM without guessing the code.

Nonce layout (12 bytes)::

    [ direction: 4 bytes ][ packet seq: 8 bytes big endian ]

``direction`` is ``b'\\x00'*4`` for host->client and ``b'\\x01'*4`` for
client->host, so both directions may use the same sequence space safely.

The 20 byte packet header is used as AAD, which binds the header fields
(type / stream / flags) to the ciphertext.
"""

from __future__ import annotations

import hashlib
import os
import struct
from dataclasses import dataclass, field
from typing import Optional

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey,
    X25519PublicKey,
)
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

PROTOCOL_LABEL = b"peerdesk/1"
KEY_LEN = 32
NONCE_LEN = 12
TAG_LEN = 16


# --------------------------------------------------------------------------- #
# key agreement
# --------------------------------------------------------------------------- #
@dataclass
class KeyPair:
    private: X25519PrivateKey
    public_bytes: bytes

    @classmethod
    def generate(cls) -> "KeyPair":
        sk = X25519PrivateKey.generate()
        pk = sk.public_key().public_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PublicFormat.Raw,
        )
        return cls(private=sk, public_bytes=pk)

    @classmethod
    def from_private_bytes(cls, raw: bytes) -> "KeyPair":
        sk = X25519PrivateKey.from_private_bytes(raw)
        pk = sk.public_key().public_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PublicFormat.Raw,
        )
        return cls(private=sk, public_bytes=pk)

    def private_bytes(self) -> bytes:
        return self.private.private_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PrivateFormat.Raw,
            encryption_algorithm=serialization.NoEncryption(),
        )


def _hkdf(ikm: bytes, salt: bytes, info: bytes, length: int = KEY_LEN) -> bytes:
    return HKDF(
        algorithm=hashes.SHA256(), length=length, salt=salt, info=info
    ).derive(ikm)


@dataclass
class SessionKeys:
    """Directional AEAD keys derived from one ECDH handshake."""

    host_to_client: bytes
    client_to_host: bytes
    transcript: bytes
    _h2c: ChaCha20Poly1305 = field(init=False, repr=False)
    _c2h: ChaCha20Poly1305 = field(init=False, repr=False)

    def __post_init__(self) -> None:
        self._h2c = ChaCha20Poly1305(self.host_to_client)
        self._c2h = ChaCha20Poly1305(self.client_to_host)

    @classmethod
    def derive(
        cls,
        private: KeyPair,
        peer_public: bytes,
        hello_host: bytes,
        hello_client: bytes,
        psk: bytes = b"",
    ) -> "SessionKeys":
        if len(peer_public) != 32:
            raise ValueError("peer public key must be 32 bytes")
        peer = X25519PublicKey.from_public_bytes(peer_public)
        shared = private.private.exchange(peer)

        # Canonical transcript: role order is fixed so both sides agree even
        # when they compute it from different moments in the handshake.
        transcript = hashlib.sha256(
            PROTOCOL_LABEL + b"\x00" + hello_host + b"\x00" + hello_client
        ).digest()

        psk_digest = hashlib.sha256(PROTOCOL_LABEL + b"\x00psk\x00" + (psk or b"")).digest()
        salt = hashlib.sha256(transcript + psk_digest).digest()

        h2c = _hkdf(shared, salt, PROTOCOL_LABEL + b" host->client")
        c2h = _hkdf(shared, salt, PROTOCOL_LABEL + b" client->host")
        return cls(host_to_client=h2c, client_to_host=c2h, transcript=transcript)


# --------------------------------------------------------------------------- #
# packet protection
# --------------------------------------------------------------------------- #
def _nonce(direction: int, seq: int) -> bytes:
    return struct.pack(">IQ", direction & 0xFFFFFFFF, seq & 0xFFFFFFFFFFFFFFFF)


def _aad(header: bytes) -> bytes:
    return header


class ReplayWindow:
    """Sliding window replay filter, 2048 packets wide."""

    def __init__(self, width: int = 2048) -> None:
        self.width = width
        self.highest = -1
        self.bitmap = 0

    def check_and_update(self, seq: int) -> bool:
        if seq > self.highest:
            shift = seq - self.highest
            if self.highest >= 0 and shift < self.width:
                self.bitmap = ((self.bitmap << shift) | 1) & ((1 << self.width) - 1)
            else:
                self.bitmap = 1
            self.highest = seq
            return True
        offset = self.highest - seq
        if offset >= self.width:
            return False
        bit = 1 << offset
        if self.bitmap & bit:
            return False
        self.bitmap |= bit
        return True


@dataclass
class CryptoContext:
    """Encrypt/decrypt with one session key set."""

    keys: SessionKeys
    is_host: bool
    _send_seq: int = 0
    _recv: ReplayWindow = field(default_factory=ReplayWindow)

    def next_seq(self) -> int:
        self._send_seq += 1
        return self._send_seq

    def _out_direction(self) -> int:
        return 0 if self.is_host else 1

    def _in_direction(self) -> int:
        return 1 if self.is_host else 0

    def _aead_out(self) -> ChaCha20Poly1305:
        return self.keys._h2c if self.is_host else self.keys._c2h

    def _aead_in(self) -> ChaCha20Poly1305:
        return self.keys._c2h if self.is_host else self.keys._h2c

    def seal(self, header: bytes, payload: bytes, seq: int) -> bytes:
        return self._aead_out().encrypt(
            _nonce(self._out_direction(), seq), payload, _aad(header)
        )

    def open(self, header: bytes, body: bytes, seq: int) -> Optional[bytes]:
        if not self._recv.check_and_update(seq):
            return None
        try:
            return self._aead_in().decrypt(
                _nonce(self._in_direction(), seq), body, _aad(header)
            )
        except Exception:
            return None


def random_token(n: int = 8) -> bytes:
    return os.urandom(n)


def fingerprint(*parts: bytes) -> str:
    h = hashlib.sha256()
    for p in parts:
        h.update(len(p).to_bytes(4, "big"))
        h.update(p)
    return h.hexdigest()
