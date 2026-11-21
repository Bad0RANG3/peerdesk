"""用一个真·exe 跑端到端：dist/peerdesk.exe server/agent/viewer 三个进程互连。

    python tests/e2e_exe.py [--exe dist/peerdesk.exe] [--seconds 8]

比 e2e_local.py 更"黑盒"：不 import 任何 peerdesk 模块里的运行时对象，
只通过 WebSocket 和 exe 的 stdout 观察，能抓到打包漏文件（web/*.html）之类的问题。

刻意用 ``--relay-only``：本机有一堆虚拟网卡（WireGuard/Hyper-V/GameViewer IDD），
打洞候选经常挑到不通的那张，打包冒烟测试不该被 NAT 行为干扰（打洞本身由
e2e_local.py 覆盖）。
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import json
import os
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests" / "out"
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WS:
    def __init__(self, reader, writer):
        self.r = reader
        self.w = writer

    @classmethod
    async def connect(cls, host, port, path="/ws"):
        r, w = await asyncio.open_connection(host, port)
        key = base64.b64encode(os.urandom(16)).decode()
        w.write(
            (
                f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
                f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n"
            ).encode()
        )
        await w.drain()
        head = await r.readuntil(b"\r\n\r\n")
        assert b"101" in head.split(b"\r\n")[0], head
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        assert accept.encode() in head
        return cls(r, w)

    async def send(self, text):
        data = text.encode()
        mask = os.urandom(4)
        n = len(data)
        if n < 126:
            head = bytes([0x81, 0x80 | n])
        elif n < 65536:
            head = bytes([0x81, 0x80 | 126]) + n.to_bytes(2, "big")
        else:
            head = bytes([0x81, 0x80 | 127]) + n.to_bytes(8, "big")
        self.w.write(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
        await self.w.drain()

    async def recv(self):
        b1, b2 = await self.r.readexactly(2)
        opcode = b1 & 0x0F
        n = b2 & 0x7F
        if n == 126:
            n = int.from_bytes(await self.r.readexactly(2), "big")
        elif n == 127:
            n = int.from_bytes(await self.r.readexactly(8), "big")
        payload = await self.r.readexactly(n)
        return opcode, payload

    def close(self):
        self.w.close()


async def wait_port(host, port, timeout=25.0):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            r, w = await asyncio.open_connection(host, port)
            w.close()
            return True
        except OSError:
            await asyncio.sleep(0.15)
    return False


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(ROOT / "dist" / "peerdesk.exe"))
    ap.add_argument("--seconds", type=float, default=9.0)
    args = ap.parse_args()

    exe = Path(args.exe)
    if not exe.exists():
        print(f"没有 exe: {exe}（先跑 python packaging/build.py）")
        return 1
    code = "481920573"
    base = 7300 + (os.getpid() % 200) * 5  # 避免和上一次残留进程抢端口
    sig_port, udp_port, agent_web, viewer_web = base, base + 1, base + 2, base + 3

    procs = []

    def spawn(name, argv):
        out = open(OUT / f"exe_{name}.log", "wb")
        p = asyncio.create_subprocess_exec(
            str(exe), *argv, stdout=out, stderr=asyncio.subprocess.STDOUT, cwd=str(ROOT)
        )
        procs.append(out)
        return p

    OUT.mkdir(parents=True, exist_ok=True)
    server = await spawn("server", ["server", "--port", str(sig_port), "--udp-port", str(udp_port)])
    if not await wait_port("127.0.0.1", sig_port):
        print("FAIL: 信令服务没起来")
        return 1
    agent = await spawn(
        "agent",
        [
            "agent", "--server", f"127.0.0.1:{sig_port}", "--code", code,
            "--web-port", str(agent_web), "--udp-port", "0", "--audio-test", "--relay-only",
            "--fps", "20", "--width", "1280", "--quality", "55",
        ],
    )
    viewer = await spawn(
        "viewer",
        [
            "viewer", "--server", f"127.0.0.1:{sig_port}", "--code", code,
            "--web-port", str(viewer_web), "--udp-port", "0", "--relay-only",
        ],
    )

    ok = True
    try:
        if not await wait_port("127.0.0.1", viewer_web, timeout=30):
            print("FAIL: viewer 的 web 端口没起来")
            return 1
        ws = await WS.connect("127.0.0.1", viewer_web)
        page = await ws.recv()
        print(f"[OK  ] viewer 握手 {page[1][:60]!r}")
        await ws.send(json.dumps({"t": "connect", "code": code, "server": f"127.0.0.1:{sig_port}"}))

        frames = audio = 0
        state = None
        t0 = time.monotonic()
        while time.monotonic() - t0 < args.seconds:
            try:
                opcode, payload = await asyncio.wait_for(ws.recv(), timeout=args.seconds)
            except TimeoutError:
                break
            if opcode == 0x1:
                msg = json.loads(payload)
                if msg.get("t") == "link":
                    state = msg.get("state")
                if msg.get("t") == "stats" and msg.get("fps"):
                    print(f"       stats fps={msg['fps']} enc={msg.get('enc_ms')}ms kb={msg.get('kb')}")
            elif opcode == 0x2 and payload:
                if payload[:1] == b"J":
                    frames += 1
                elif payload[:1] == b"A":
                    audio += 1
        ws.close()
        print(f"[{'OK  ' if state == 'relay' else 'FAIL'}] 链路状态 {state}")
        ok &= state == "relay"
        print(f"[{'OK  ' if frames >= 5 else 'FAIL'}] 视频帧 {frames}")
        ok &= frames >= 5
        print(f"[{'OK  ' if audio >= 10 else 'FAIL'}] 音频帧 {audio}")
        ok &= audio >= 10
    finally:
        for p in (viewer, agent, server):
            try:
                p.terminate()
            except ProcessLookupError:
                pass
        for p in (viewer, agent, server):
            try:
                await asyncio.wait_for(p.wait(), timeout=10)
            except TimeoutError:
                p.kill()  # 卡住的 exe 会一直占着 UDP 端口，必须清掉
                await p.wait()
        for out in procs:
            out.close()

    for name in ("server", "agent", "viewer"):
        tail = (OUT / f"exe_{name}.log").read_text(encoding="utf-8", errors="replace").strip().splitlines()
        print(f"       {name}.log 末行: {tail[-1][:110] if tail else '(空)'}")
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
