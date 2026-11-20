"""Tiny RFC 6455 WebSocket server (no extensions, no compression).

Only what PeerDesk needs: text/binary messages, ping/pong, close. Keeping this
in-tree avoids a dependency on ``websockets`` and keeps the install footprint at
"python + pillow + mss + cryptography".
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import logging
import struct

log = logging.getLogger("peerdesk.ws")

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_CONT = 0x0
OP_TEXT = 0x1
OP_BINARY = 0x2
OP_CLOSE = 0x8
OP_PING = 0x9
OP_PONG = 0xA

MAX_MESSAGE = 8 * 1024 * 1024


class WSConnection:
    def __init__(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, request: bytes = b""
    ) -> None:
        self.reader = reader
        self.writer = writer
        self.closed = False
        self._request = request
        self._lock = asyncio.Lock()

    async def handshake(self) -> bool:
        request = self._request
        if not request:
            try:
                request = await self.reader.readuntil(b"\r\n\r\n")
            except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
                return False
        headers = {}
        for line in request.decode("latin-1").split("\r\n")[1:]:
            if ":" in line:
                k, _, v = line.partition(":")
                headers[k.strip().lower()] = v.strip()
        key = headers.get("sec-websocket-key")
        if not key or "websocket" not in headers.get("upgrade", "").lower():
            await self._reject(400, "bad handshake")
            return False
        accept = base64.b64encode(hashlib.sha1(key.encode() + GUID).digest()).decode()
        self.writer.write(
            (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n"
                "\r\n"
            ).encode()
        )
        await self.writer.drain()
        return True

    async def _reject(self, code: int, reason: str) -> None:
        body = reason.encode()
        self.writer.write(
            (
                f"HTTP/1.1 {code} {reason}\r\nContent-Type: text/plain\r\n"
                f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n"
            ).encode()
            + body
        )
        try:
            await self.writer.drain()
            self.writer.close()
        except Exception:
            pass

    # ------------------------------------------------------------------ frames
    async def _send_frame(self, opcode: int, payload: bytes) -> None:
        if self.closed:
            return
        header = bytearray()
        header.append(0x80 | opcode)
        size = len(payload)
        if size < 126:
            header.append(size)
        elif size < 65536:
            header.append(126)
            header.extend(struct.pack(">H", size))
        else:
            header.append(127)
            header.extend(struct.pack(">Q", size))
        async with self._lock:
            try:
                self.writer.write(bytes(header) + payload)
                await self.writer.drain()
            except (ConnectionError, RuntimeError):
                self.closed = True

    async def send_text(self, text: str) -> None:
        await self._send_frame(OP_TEXT, text.encode("utf-8"))

    async def send_binary(self, data: bytes) -> None:
        await self._send_frame(OP_BINARY, data)

    async def close(self, code: int = 1000) -> None:
        if self.closed:
            return
        try:
            await self._send_frame(OP_CLOSE, struct.pack(">H", code))
        finally:
            self.closed = True
            try:
                self.writer.close()
            except Exception:
                pass

    async def recv(self) -> tuple[int, bytes] | None:
        """Return ``(opcode, payload)`` for complete messages, or None on close."""
        buffer = b""
        opcode_first = None
        while True:
            try:
                head = await self.reader.readexactly(2)
            except (asyncio.IncompleteReadError, ConnectionError):
                return None
            b0, b1 = head[0], head[1]
            fin = bool(b0 & 0x80)
            opcode = b0 & 0x0F
            masked = bool(b1 & 0x80)
            length = b1 & 0x7F
            if length == 126:
                length = struct.unpack(">H", await self.reader.readexactly(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", await self.reader.readexactly(8))[0]
            if length > MAX_MESSAGE:
                await self.close(1009)
                return None
            mask = await self.reader.readexactly(4) if masked else b""
            data = await self.reader.readexactly(length) if length else b""
            if masked:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))

            if opcode == OP_PING:
                await self._send_frame(OP_PONG, data)
                continue
            if opcode == OP_PONG:
                continue
            if opcode == OP_CLOSE:
                await self.close()
                return None
            if opcode in (OP_TEXT, OP_BINARY):
                opcode_first = opcode
                buffer = data
            elif opcode == OP_CONT:
                buffer += data
            else:
                continue
            if fin:
                return (opcode_first or OP_TEXT), buffer
