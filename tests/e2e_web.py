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
    async def connect(cls, host: str, port: int, path: str = "/ws") -> WSClient:
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
        except TimeoutError:
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
           no_skip_static=True, no_clipboard=True, no_audio=False, audio_test=True,
           no_input=True, relay_only=False,
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

    # 2b. 声音：WS 上带 "A" 前缀的二进制帧，解码后应当有非静音样本
    audio_frames: list[bytes] = []
    audio_deadline = asyncio.get_event_loop().time() + 6
    while asyncio.get_event_loop().time() < audio_deadline and len(audio_frames) < 30:
        try:
            opcode, data = await ws.recv(timeout=2.0)
        except Exception:
            break
        if opcode is None:
            break
        if opcode == 0x2 and data[:1] == b"A":
            audio_frames.append(data[1:])

    audio_ok = False
    if audio_frames:
        from peerdesk import audio as audio_mod
        from peerdesk import wire

        good = 0
        for payload in audio_frames:
            _seq, _ts, samples, _prev, cur = wire.unpack_audio(payload)
            if samples == audio_mod.SAMPLES and len(cur) == audio_mod.MULAW_BYTES and any(audio_mod.ulaw_decode(cur)):
                good += 1
        audio_ok = good >= 20
        print(f"[{'OK  ' if audio_ok else 'FAIL'}] audio frames over bridge: {len(audio_frames)} ({good} 非静音)")
    else:
        print("[FAIL] audio frames over bridge: 0")
    failures += 0 if audio_ok else 1

    # 3. input/command plumbing: quality change + mouse event are accepted
    await ws.send_json({"t": "quality", "q": 40, "w": 800})
    await ws.send_json({"t": "m", "act": "move", "x": 0.5, "y": 0.5})
    await asyncio.sleep(1.0)
    pending = viewer.session.link._ctrl.pending() if viewer.session else -1
    print(f"[OK  ] control channel pending={pending}")

    # 4. 浏览器上传 -> 被控端落盘（顺带验证 >MTU 控制消息的分片/重组）
    import tempfile

    import peerdesk.util as util_mod

    tmp = Path(tempfile.mkdtemp(prefix="peerdesk-e2e-"))
    orig_download = util_mod.download_dir
    util_mod.download_dir = lambda: tmp
    blob = os.urandom(180 * 1024)
    up_name = "e2e-upload.bin"
    try:
        await ws.send_json({"t": "upload_begin", "name": up_name, "size": len(blob)})
        for off in range(0, len(blob), 24576):
            await ws.send(b"U" + blob[off : off + 24576], opcode=0x2)
        await ws.send_json({"t": "upload_end"})
        landed = tmp / "PeerDesk" / up_name
        for _ in range(100):
            if landed.exists() and landed.stat().st_size == len(blob):
                break
            await asyncio.sleep(0.2)
        got = landed.read_bytes() if landed.exists() else b""
        ok_up = got == blob
        print(f"[{'OK  ' if ok_up else 'FAIL'}] browser upload -> host file {len(got)} bytes")
        failures += 0 if ok_up else 1

        # 5. 反方向：被控端推文件 -> 控制端落盘 -> 浏览器经 /dl 下载
        sample = tmp / "handover-sample.bin"
        sample.write_bytes(os.urandom(96 * 1024))
        agent.host.send_file(sample)
        back = tmp / "PeerDesk" / "handover-sample.bin"
        for _ in range(100):
            if back.exists() and back.stat().st_size == sample.stat().st_size:
                break
            await asyncio.sleep(0.2)
        ok_back = back.exists() and back.read_bytes() == sample.read_bytes()
        print(f"[{'OK  ' if ok_back else 'FAIL'}] host push -> viewer file {back.stat().st_size if back.exists() else 0} bytes")
        failures += 0 if ok_back else 1

        reader, writer = await asyncio.open_connection("127.0.0.1", viewer.ui.port)
        writer.write(b"GET /dl/handover-sample.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        await writer.drain()
        raw = await asyncio.wait_for(reader.read(), 10.0)
        writer.close()
        head, _, body = raw.partition(b"\r\n\r\n")
        ok_dl = b"200 OK" in head and body == sample.read_bytes()
        print(f"[{'OK  ' if ok_dl else 'FAIL'}] /dl download {len(body)} bytes")
        failures += 0 if ok_dl else 1
    finally:
        util_mod.download_dir = orig_download
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
