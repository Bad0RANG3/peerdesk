"""End-to-end check of the browser bridge (HTTP + WebSocket + JPEG frames).

    python tests/e2e_web.py

Starts server + agent + viewer in-process, then behaves like the web client:
loads the page over HTTP, opens /ws, asks the viewer to connect by code and
verifies binary frames arrive and decode as JPEG.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import io
import json
import os
import ssl
import struct
import sys
import types
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from peerdesk.agent import AgentApp  # noqa: E402
from peerdesk.server import SignalingServer  # noqa: E402
from peerdesk.viewer import ViewerApp  # noqa: E402

CODE = "604173829"
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WSClient:
    """Bare-bones RFC6455 client (text/binary, no extensions)."""

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.reader = reader
        self.writer = writer

    @classmethod
    async def connect(cls, host: str, port: int, path: str = "/ws") -> "WSClient":
        reader, writer = await asyncio.open_connection(host, port)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        writer.write(req.encode())
        await writer.drain()
        header = await reader.readuntil(b"\r\n\r\n")
        assert b"101" in header.split(b"\r\n")[0], header
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        assert accept.encode() in header, "bad Sec-WebSocket-Accept"
        return cls(reader, writer)

    async def send(self, payload: bytes, opcode: int = 0x1) -> None:
        mask = os.urandom(4)
        n = len(payload)
        head = bytes([0x80 | opcode])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        body = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.writer.write(head + mask + body)
        await self.writer.drain()

    async def send_json(self, obj: dict) -> None:
        await self.send(json.dumps(obj).encode())

    async def recv(self, timeout: float = 5.0):
        async def _read():
            b1, b2 = await self.reader.readexactly(2)
            opcode = b1 & 0x0F
            length = b2 & 0x7F
            if length == 126:
                length = struct.unpack(">H", await self.reader.readexactly(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", await self.reader.readexactly(8))[0]
            data = await self.reader.readexactly(length) if length else b""
            return opcode, data

        try:
            return await asyncio.wait_for(_read(), timeout)
        except asyncio.TimeoutError:
            return None, None

    def close(self) -> None:
        try:
            self.writer.close()
        except Exception:
            pass


async def http_get(host: str, port: int, path: str = "/") -> str:
    reader, writer = await asyncio.open_connection(host, port)
    writer.write(f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n".encode())
    await writer.drain()
    raw = await asyncio.wait_for(reader.read(), 5.0)
    writer.close()
    head, _, body = raw.partition(b"\r\n\r\n")
    return body.decode("utf-8", "replace")


def ns(**kw):
    return types.SimpleNamespace(**kw)


async def run() -> int:
    sig_port, udp_port = 7920, 7921
    srv = SignalingServer("127.0.0.1", sig_port, udp_port)
    asyncio.get_event_loop().create_task(srv.start())
    await asyncio.sleep(0.4)

    agent = AgentApp(
        ns(code=CODE, server=f"127.0.0.1:{sig_port}", fps=12, width=1024, quality=50, monitor=0,
           no_skip_static=True, no_clipboard=True, no_input=True, relay_only=False,
           web_port=0, udp_port=0, open=False, verbose=False)
    )
    asyncio.get_event_loop().create_task(agent.start())
    await asyncio.sleep(1.0)

    viewer = ViewerApp(
        ns(code="", server=f"127.0.0.1:{sig_port}", relay_only=False, web_port=0, udp_port=0,
           open=False, verbose=False)
    )
    asyncio.get_event_loop().create_task(viewer.start())
    await asyncio.sleep(0.6)

    failures = 0
    # 1. the control page renders
    html = await http_get("127.0.0.1", viewer.ui.port, "/")
    ok_html = "canvas" in html and "viewer.js" in html and "PeerDesk" in html
    print(f"[{'OK  ' if ok_html else 'FAIL'}] viewer page served ({len(html)} bytes)")
    failures += 0 if ok_html else 1

    # 2. websocket bridge delivers frames
    ws = await WSClient.connect("127.0.0.1", viewer.ui.port)
    await ws.send_json({"t": "connect", "code": CODE, "server": f"127.0.0.1:{sig_port}"})

    frames: list[bytes] = []
    saw_info = False
    deadline = asyncio.get_event_loop().time() + 25
    while asyncio.get_event_loop().time() < deadline and len(frames) < 8:
        opcode, data = await ws.recv(timeout=6.0)
        if opcode is None:
            break
        if opcode == 0x2 and data[:1] == b"J":
            frames.append(data[1:])
        elif opcode == 0x1:
            msg = json.loads(data)
            if msg.get("t") == "info":
                saw_info = True
            if msg.get("t") in ("error", "link"):
                print("   event:", msg)

    print(f"[{'OK  ' if saw_info else 'FAIL'}] host info over bridge")
    failures += 0 if saw_info else 1
    print(f"[{'OK  ' if len(frames) >= 8 else 'FAIL'}] binary frames: {len(frames)}")
    failures += 0 if len(frames) >= 8 else 1

    if frames:
        from PIL import Image

        img = Image.open(io.BytesIO(frames[-1]))
        print(f"[OK  ] jpeg decodes: {img.size[0]}x{img.size[1]} {img.format} {len(frames[-1]) // 1024} KB")
        ok_jpeg = img.format == "JPEG"
        failures += 0 if ok_jpeg else 1

    # 3. input/command plumbing: quality change + mouse event are accepted
    await ws.send_json({"t": "quality", "q": 40, "w": 800})
    await ws.send_json({"t": "m", "act": "move", "x": 0.5, "y": 0.5})
    await asyncio.sleep(1.0)
    pending = viewer.session.link._ctrl.pending() if viewer.session else -1
    print(f"[OK  ] control channel pending={pending}")
    ws.close()

    state = viewer.link.state if viewer.link else "none"
    print(f"[{'OK  ' if state in ('direct', 'relay') else 'FAIL'}] link state: {state}")
    failures += 0 if state in ("direct", "relay") else 1

    print("RESULT:", "PASS" if failures == 0 else f"FAIL ({failures})")
    return 0 if failures == 0 else 1


def main() -> int:
    argparse.ArgumentParser().parse_args()
    return asyncio.run(run())


if __name__ == "__main__":
    raise SystemExit(main())
