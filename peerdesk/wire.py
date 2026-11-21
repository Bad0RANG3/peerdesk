"""PeerDesk wire format (PDTP/1).

Packet layout
-------------
::

    byte  size  field
    0     2     magic  b"PD"
    2     1     version
    3     1     type
    4     4     seq        per direction, 1..2^32-1 (wraps)
    8     4     ts_ms      sender monotonic clock (ms) truncated to 32 bit
    12    1     stream     STREAM_*
    13    1     flags      FLAG_*
    14    2     plen       payload length before encryption? -> ciphertext length
    16    4     session    random 32 bit session tag from the handshake

Everything after the header is ChaCha20-Poly1305 ciphertext (payload + tag).

Types
-----
HELLO, HELLO_ACK, HELLO_FIN carry the *unencrypted* handshake material.
PUNCH / PUNCH_ACK are plain 8 byte probes used while hole punching.
DATA / PARITY / AUDIO / ACK / NACK / PING / PONG / BYE are encrypted.
"""

from __future__ import annotations

import json
import struct
from collections.abc import Iterable
from dataclasses import dataclass

MAGIC = b"PD"
VERSION = 1
HEADER = struct.Struct("<2sBBIIBBHI")
HEADER_LEN = HEADER.size  # 20

TYPE_HELLO = 1
TYPE_HELLO_ACK = 2
TYPE_HELLO_FIN = 3
TYPE_PUNCH = 4
TYPE_PUNCH_ACK = 5
TYPE_DATA = 6
TYPE_PARITY = 7
TYPE_ACK = 8
TYPE_NACK = 9
TYPE_PING = 10
TYPE_PONG = 11
TYPE_BYE = 12
TYPE_AUDIO = 13

TYPE_NAMES = {
    TYPE_HELLO: "hello",
    TYPE_HELLO_ACK: "hello_ack",
    TYPE_HELLO_FIN: "hello_fin",
    TYPE_PUNCH: "punch",
    TYPE_PUNCH_ACK: "punch_ack",
    TYPE_DATA: "data",
    TYPE_PARITY: "parity",
    TYPE_ACK: "ack",
    TYPE_NACK: "nack",
    TYPE_PING: "ping",
    TYPE_PONG: "pong",
    TYPE_BYE: "bye",
    TYPE_AUDIO: "audio",
}

STREAM_VIDEO = 0
STREAM_CONTROL = 1
STREAM_FILE = 2
STREAM_STATS = 3
STREAM_AUDIO = 4

FLAG_KEYFRAME = 0x01
FLAG_LAST_FRAG = 0x02
FLAG_RELIABLE = 0x04

UNRELIABLE_TYPES = {
    TYPE_PUNCH,
    TYPE_PUNCH_ACK,
    TYPE_DATA,
    TYPE_PARITY,
    TYPE_AUDIO,
    TYPE_PING,
    TYPE_PONG,
}


@dataclass(slots=True)
class Header:
    version: int
    type: int
    seq: int
    ts_ms: int
    stream: int
    flags: int
    plen: int
    session: int

    def pack(self) -> bytes:
        return HEADER.pack(
            MAGIC,
            self.version,
            self.type,
            self.seq,
            self.ts_ms,
            self.stream,
            self.flags,
            self.plen,
            self.session,
        )


def parse_header(buf: bytes) -> Header | None:
    if len(buf) < HEADER_LEN:
        return None
    magic, ver, typ, seq, ts, stream, flags, plen, session = HEADER.unpack_from(buf, 0)
    if magic != MAGIC or ver != VERSION:
        return None
    return Header(ver, typ, seq, ts, stream, flags, plen, session)


# --------------------------------------------------------------------------- #
# payload helpers
# --------------------------------------------------------------------------- #
VIDEO_FRAG = struct.Struct("<IHH")  # frame_id, frag_idx, frag_cnt
PARITY_HDR = struct.Struct("<IHBB")  # frame_id, parity_index, data_shards, ...
ACK_PAYLOAD = struct.Struct("<III")  # ack_cum, bitmap_lo, bitmap_hi
NACK_PAYLOAD = struct.Struct("<IH")  # frame_id, count
# 音频：seq, ts_ms, samples（解码后样本数）, prev_len（上一帧重传长度）
AUDIO_HDR = struct.Struct("<IIHH")


def pack_video_fragment(frame_id: int, frag_idx: int, frag_cnt: int, data: bytes) -> bytes:
    return VIDEO_FRAG.pack(frame_id, frag_idx, frag_cnt) + data


def unpack_video_fragment(payload: bytes):
    frame_id, frag_idx, frag_cnt = VIDEO_FRAG.unpack_from(payload, 0)
    return frame_id, frag_idx, frag_cnt, payload[VIDEO_FRAG.size :]


def pack_ack(ack_cum: int, bitmap: int) -> bytes:
    return ACK_PAYLOAD.pack(ack_cum & 0xFFFFFFFF, bitmap & 0xFFFFFFFF, (bitmap >> 32) & 0xFFFFFFFF)


def unpack_ack(payload: bytes):
    cum, lo, hi = ACK_PAYLOAD.unpack_from(payload, 0)
    return cum, (hi << 32) | lo


def pack_nack(frame_id: int, missing: Iterable[int]) -> bytes:
    missing = list(missing)
    return NACK_PAYLOAD.pack(frame_id, len(missing)) + struct.pack(f"<{len(missing)}H", *missing)


def unpack_nack(payload: bytes):
    frame_id, count = NACK_PAYLOAD.unpack_from(payload, 0)
    off = NACK_PAYLOAD.size
    idx = struct.unpack_from(f"<{count}H", payload, off) if count else ()
    return frame_id, idx


def pack_audio(seq: int, ts_ms: int, samples: int, current: bytes, previous: bytes = b"") -> bytes:
    """音频包：定长头 + 可选的上一帧（丢一个包也不会静音）。"""
    return AUDIO_HDR.pack(seq & 0xFFFFFFFF, ts_ms & 0xFFFFFFFF, samples, len(previous)) + previous + current


def unpack_audio(payload: bytes):
    seq, ts_ms, samples, prev_len = AUDIO_HDR.unpack_from(payload, 0)
    off = AUDIO_HDR.size
    previous = payload[off : off + prev_len]
    return seq, ts_ms, samples, previous, payload[off + prev_len :]


def json_payload(obj) -> bytes:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def parse_json(payload: bytes):
    try:
        return json.loads(payload.decode("utf-8"))
    except Exception:
        return None


# --------------------------------------------------------------------------- #
# fragmentation + XOR FEC
# --------------------------------------------------------------------------- #
def fragment(data: bytes, mtu_payload: int) -> list[bytes]:
    if mtu_payload <= 0:
        raise ValueError("mtu_payload must be positive")
    return [data[i : i + mtu_payload] for i in range(0, len(data), mtu_payload)] or [b""]


def xor_bytes(chunks: Iterable[bytes], size: int) -> bytes:
    out = bytearray(size)
    for chunk in chunks:
        for i, b in enumerate(chunk):
            out[i] ^= b
    return bytes(out)


@dataclass
class ParityGroup:
    """Simple systematic XOR FEC: every ``k`` data shards produce 1 parity."""

    shards: list[bytes]
    parity: bytes

    @classmethod
    def build(cls, fragments: list[bytes], k: int) -> list[ParityGroup]:
        groups: list[ParityGroup] = []
        for start in range(0, len(fragments), k):
            group = fragments[start : start + k]
            size = max((len(f) for f in group), default=0)
            groups.append(cls(shards=list(group), parity=xor_bytes(group, size)))
        return groups


class FragmentAssembler:
    """Reassembles one frame from fragments, tolerating duplicates/reorder."""

    def __init__(self, frag_cnt: int, max_bytes: int = 32 * 1024 * 1024) -> None:
        self.frag_cnt = frag_cnt
        self.max_bytes = max_bytes
        self.parts: list[bytes | None] = [None] * frag_cnt
        self.received = 0

    def add(self, idx: int, data: bytes) -> bool:
        if idx >= self.frag_cnt or self.parts[idx] is not None:
            return False
        self.parts[idx] = data
        self.received += 1
        return True

    def fill_parity(self, idx: int, parity: bytes, group_size_idx: list[int]) -> bool:
        """Recover a single missing fragment in a group using XOR parity.

        ``missing = parity ^ (all other shards)`` — the parity block itself has
        to take part in the XOR, otherwise the "recovered" bytes are the XOR of
        the survivors rather than of the lost shard.
        """
        missing = [i for i in group_size_idx if i < len(self.parts) and self.parts[i] is None]
        if len(missing) != 1:
            return False
        target = missing[0]
        known = [self.parts[i] for i in group_size_idx if i != target and i < len(self.parts) and self.parts[i] is not None]
        if len(known) != len(group_size_idx) - 1:
            return False
        recovered = xor_bytes([*known, parity], len(parity))
        self.parts[target] = recovered
        self.received += 1
        return True

    @property
    def complete(self) -> bool:
        return self.received == self.frag_cnt

    def assemble(self) -> bytes:
        total = b"".join(p or b"" for p in self.parts)
        if len(total) > self.max_bytes:
            raise ValueError("frame too large")
        return total

    def missing(self) -> list[int]:
        return [i for i, p in enumerate(self.parts) if p is None]
