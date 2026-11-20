"""Minimal STUN (RFC 5389) client and server, just enough for NAT discovery.

Only BINDING requests and XOR-MAPPED-ADDRESS are implemented: PeerDesk uses
STUN to learn its own server-reflexive address so the two peers can attempt a
direct UDP path before falling back to the relay.
"""

from __future__ import annotations

import asyncio
import os
import struct

MAGIC = 0x2112A442
BINDING_REQUEST = 0x0001
BINDING_SUCCESS = 0x0101
ATTR_XOR_MAPPED = 0x0020
ATTR_MAPPED = 0x0001
HEADER = struct.Struct(">HHI12s")


def build_request(txid: bytes | None = None) -> tuple[bytes, bytes]:
    txid = txid or os.urandom(12)
    return HEADER.pack(BINDING_REQUEST, 0, MAGIC, txid), txid


def _xor_addr(host: str, port: int, txid: bytes) -> bytes:
    cookie = struct.pack(">I", MAGIC)
    pack_ip = cookie + txid
    raw = bytearray()
    if ":" in host:
        import ipaddress

        raw.extend(ipaddress.IPv6Address(host).packed)
        family = 0x02
    else:
        import ipaddress

        raw.extend(ipaddress.IPv4Address(host).packed)
        family = 0x01
    xport = port ^ (MAGIC >> 16)
    for i in range(len(raw)):
        raw[i] ^= pack_ip[i]
    return struct.pack(">BBH", 0, family, xport) + bytes(raw)


def build_response(txid: bytes, host: str, port: int) -> bytes:
    attr = _xor_addr(host, port, txid)
    body = struct.pack(">HH", ATTR_XOR_MAPPED, len(attr)) + attr
    return HEADER.pack(BINDING_SUCCESS, len(body), MAGIC, txid) + body


def parse_message(data: bytes) -> tuple[int, bytes, dict] | None:
    if len(data) < 20:
        return None
    mtype, length, magic, txid = HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        return None
    attrs: dict[int, bytes] = {}
    off = 20
    end = min(len(data), 20 + length)
    while off + 4 <= end:
        atype, alen = struct.unpack_from(">HH", data, off)
        off += 4
        attrs[atype] = data[off : off + alen]
        off += alen + ((4 - alen % 4) % 4)
    return mtype, txid, attrs


def parse_mapped(attrs: dict, txid: bytes) -> tuple[str, int] | None:
    import ipaddress

    raw = attrs.get(ATTR_XOR_MAPPED)
    xor = True
    if raw is None:
        raw = attrs.get(ATTR_MAPPED)
        xor = False
    if not raw or len(raw) < 8:
        return None
    # 地址族必须显式校验：坏包只回 family=0x02 或塞了尾巴字节时，
    # 直接按 raw[4:] 切会被拼成"合法但错误"的地址。
    family = raw[1]
    if family == 0x01:
        addr_len = 4
    elif family == 0x02:
        addr_len = 16
    else:
        return None
    if len(raw) < 4 + addr_len:
        return None
    port = struct.unpack_from(">H", raw, 2)[0]
    addr = bytearray(raw[4 : 4 + addr_len])
    if xor:
        cookie = struct.pack(">I", MAGIC) + txid
        port ^= MAGIC >> 16
        for i in range(addr_len):
            addr[i] ^= cookie[i]
    ip = ipaddress.ip_address(bytes(addr))
    return str(ip), port


class StunClient(asyncio.DatagramProtocol):
    """Ask one STUN server for our reflexive address."""

    def __init__(self, future: asyncio.Future) -> None:
        self.future = future
        self.transport = None
        self.txid = b""

    def connection_made(self, transport) -> None:
        self.transport = transport

    def datagram_received(self, data: bytes, addr) -> None:
        parsed = parse_message(data)
        if not parsed:
            return
        mtype, txid, attrs = parsed
        if mtype != BINDING_SUCCESS or txid != self.txid:
            return
        mapped = parse_mapped(attrs, txid)
        if mapped and not self.future.done():
            self.future.set_result(mapped)
        if self.transport:
            self.transport.close()


async def query_stun(server: tuple[str, int], timeout: float = 2.0) -> tuple[str, int] | None:
    loop = asyncio.get_event_loop()
    fut: asyncio.Future = loop.create_future()
    transport = None
    try:
        transport, proto = await loop.create_datagram_endpoint(
            lambda: StunClient(fut), remote_addr=server
        )
        proto.txid = os.urandom(12)
        packet, _ = build_request(proto.txid)
        transport.sendto(packet)
        return await asyncio.wait_for(fut, timeout)
    except (TimeoutError, OSError):
        return None
    finally:
        if transport:
            transport.close()
