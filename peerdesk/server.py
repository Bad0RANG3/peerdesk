"""Self-hosted PeerDesk server: signaling (TCP/JSON lines) + relay + STUN (UDP).

One process is enough for a whole household or a small team: it introduces the
two peers, hands out relay tags, and forwards relayed datagrams when a direct
path cannot be established. No accounts, no metering, no subscription.

    python -m peerdesk.server --host 0.0.0.0 --port 7900 --udp-port 7901
"""

from __future__ import annotations

import argparse
import asyncio
import hmac
import json
import logging
import os
import secrets
import time
from dataclasses import dataclass, field

from . import stun, util
from .transport import RELAY_MAGIC

log = logging.getLogger("peerdesk.server")

MAX_LINE = 1 << 20
RELAY_SILENCE = 180.0
CONNECT_WINDOW = 60.0  # 连接尝试计数窗口（秒）
CONNECT_MAX = 10  # 窗口内每个来源 IP 允许的 connect 次数（9 位码防爆破）


class TokenBucket:
    """按字节计费的令牌桶，给中继做带宽上限（0 = 不限速）。

    按 1 秒的突发容量起步：既能扛住关键帧那种瞬时尖峰，又不会让长时间
    下载把整条线路占满。``allow()`` 接受显式的 ``now``，方便单测。
    """

    def __init__(self, mbps: float, burst_sec: float = 1.0) -> None:
        self.mbps = max(0.0, float(mbps))
        self.rate = self.mbps * 125_000.0  # bytes/s
        self.capacity = max(self.rate * burst_sec, 8192.0)
        self.tokens = self.capacity
        self.last = 0.0  # 首次 allow() 时按需补充，允许调用方注入时钟
        self.passed = 0
        self.dropped = 0
        self.bytes = 0

    @property
    def enabled(self) -> bool:
        return self.rate > 0.0

    def allow(self, size: int, now: float | None = None) -> bool:
        if not self.enabled:
            self.passed += 1
            self.bytes += size
            return True
        now = time.monotonic() if now is None else now
        if now > self.last:
            self.tokens = min(self.capacity, self.tokens + (now - self.last) * self.rate)
            self.last = now
        if self.tokens < size:
            self.dropped += 1
            return False
        self.tokens -= size
        self.passed += 1
        self.bytes += size
        return True


@dataclass
class Peer:
    writer: asyncio.StreamWriter
    role: str
    code_id: str = ""
    name: str = ""
    cands: list[dict] = field(default_factory=list)
    sid: str = ""
    partner: Peer | None = None
    relay_tag: int = 0
    udp_addr: tuple[str, int] | None = None
    last_seen: float = field(default_factory=time.monotonic)

    def send(self, obj: dict) -> None:
        try:
            self.writer.write((json.dumps(obj, separators=(",", ":")) + "\n").encode())
        except Exception:
            pass


@dataclass
class Pair:
    sid: str
    agent: Peer
    viewer: Peer
    agent_tag: int
    viewer_tag: int
    created: float = field(default_factory=time.monotonic)
    bucket: TokenBucket = field(default_factory=lambda: TokenBucket(0.0))


class RelayProtocol(asyncio.DatagramProtocol):
    def __init__(self, server: SignalingServer) -> None:
        self.server = server
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport) -> None:
        self.transport = transport

    def datagram_received(self, data: bytes, addr) -> None:
        if len(data) >= 20 and data[:2] == b"\x00\x01":
            self._handle_stun(data, addr)
            return
        if not data.startswith(RELAY_MAGIC) or len(data) < 8:
            return
        tag = int.from_bytes(data[4:8], "big")
        self.server.on_relay(tag, data[8:], addr)

    def _handle_stun(self, data: bytes, addr) -> None:
        parsed = stun.parse_message(data)
        if not parsed:
            return
        mtype, txid, _attrs = parsed
        if mtype != stun.BINDING_REQUEST:
            return
        reply = stun.build_response(txid, addr[0], addr[1])
        if self.transport:
            self.transport.sendto(reply, addr)


class SignalingServer:
    def __init__(
        self,
        host: str = "0.0.0.0",
        port: int = 7900,
        udp_port: int = 7901,
        *,
        token: str = "",
        relay_mbps: float = 0.0,
        connect_max: int = CONNECT_MAX,
        connect_window: float = CONNECT_WINDOW,
    ) -> None:
        self.host = host
        self.port = port
        self.udp_port = udp_port
        self.token = token or ""
        self.relay_mbps = max(0.0, float(relay_mbps))
        self.connect_max = max(1, int(connect_max))
        self.connect_window = max(1.0, float(connect_window))
        self.attempts: dict[str, list[float]] = {}
        self.registry: dict[str, Peer] = {}
        self.pairs: dict[str, Pair] = {}
        self.tag_map: dict[int, Pair] = {}
        self.relay: RelayProtocol | None = None
        self.public_host = ""
        self.started = time.monotonic()

    # --------------------------------------------------------------- lifecycle
    async def start(self) -> None:
        loop = asyncio.get_event_loop()
        transport, proto = await loop.create_datagram_endpoint(
            lambda: RelayProtocol(self), local_addr=(self.host, self.udp_port)
        )
        self.relay = proto
        self.public_host = self._guess_public_host()
        server = await asyncio.start_server(self._handle_client, self.host, self.port)
        log.info("signaling on %s:%d, udp/relay+stun on %s:%d", self.host, self.port, self.host, self.udp_port)
        log.info("relay address advertised to peers: %s:%d", self.public_host, self.udp_port)
        async with server:
            await server.serve_forever()

    def _guess_public_host(self) -> str:
        import socket

        if self.host not in ("0.0.0.0", "::"):
            return self.host
        try:
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            probe.connect(("223.5.5.5", 53))
            ip = probe.getsockname()[0]
            probe.close()
            return ip
        except Exception:
            return "127.0.0.1"

    # ------------------------------------------------------------ tcp handling
    async def _handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        addr = writer.get_extra_info("peername")
        peer = Peer(writer=writer, role="unknown")
        log.info("tcp connect %s", addr)
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                if len(line) > MAX_LINE:
                    break
                try:
                    msg = json.loads(line.decode("utf-8"))
                except Exception:
                    continue
                peer.last_seen = time.monotonic()
                await self._dispatch(peer, msg)
        except (ConnectionResetError, asyncio.IncompleteReadError):
            pass
        finally:
            self._drop(peer, addr)
            try:
                writer.close()
            except Exception:
                pass

    # ------------------------------------------------------- auth / rate limits
    def _remote_ip(self, peer: Peer) -> str:
        try:
            a = peer.writer.get_extra_info("peername")
            return a[0] if a else "?"
        except Exception:
            return "?"

    def _auth(self, peer: Peer, msg: dict, *, close: bool = False) -> bool:
        """--token 模式下校验握手令牌；未配置 token 时一律放行。"""
        if not self.token:
            return True
        given = str(msg.get("token") or "")
        if hmac.compare_digest(given, self.token):
            return True
        log.warning("bad token from %s (%s)", addr_of(peer), self._remote_ip(peer))
        peer.send({"t": "error", "reason": "bad_token", "text": "服务器令牌不匹配"})
        return False

    def _rate_ok(self, ip: str, now: float | None = None) -> bool:
        """每个来源 IP 在窗口内的 connect 次数上限（挡 9 位码爆破）。"""
        now = time.monotonic() if now is None else now
        stamps = [t for t in self.attempts.get(ip, []) if now - t < self.connect_window]
        if len(stamps) >= self.connect_max:
            self.attempts[ip] = stamps
            return False
        stamps.append(now)
        self.attempts[ip] = stamps
        # 顺手清理，免得长跑之后字典无限膨胀
        if len(self.attempts) > 4096:
            for key in [k for k, v in self.attempts.items() if not v or now - v[-1] > self.connect_window]:
                self.attempts.pop(key, None)
        if len(self.attempts) > 8192:  # 极端情况（大量来源 IP）硬顶，丢最早的
            for key in list(self.attempts)[: len(self.attempts) - 4096]:
                self.attempts.pop(key, None)
        return True

    async def _dispatch(self, peer: Peer, msg: dict) -> None:
        kind = msg.get("t")
        if kind == "register":
            if not self._auth(peer, msg):
                return
            code = util.normalize_code(str(msg.get("code", "")))
            if not util.code_is_valid(code):
                peer.send({"t": "error", "reason": "bad_code"})
                return
            peer.role = "agent"
            peer.code_id = util.code_id(code)
            peer.name = str(msg.get("name", ""))[:64]
            peer.cands = list(msg.get("cands") or [])
            self.registry[peer.code_id] = peer
            log.info("agent registered %s name=%s cands=%d", peer.code_id[:8], peer.name, len(peer.cands))
            peer.send(
                {
                    "t": "registered",
                    "code": util.format_code(code),
                    "relay": [self.public_host, self.udp_port],
                    "server_time": int(time.time()),
                    "token_required": bool(self.token),
                    "relay_mbps": self.relay_mbps,
                }
            )
        elif kind == "connect":
            if not self._auth(peer, msg):
                return
            ip = self._remote_ip(peer)
            if not self._rate_ok(ip):
                log.warning("connect rate limited ip=%s", ip)
                peer.send({"t": "error", "reason": "rate_limited", "text": "连接尝试过于频繁，稍后再试"})
                return
            code = util.normalize_code(str(msg.get("code", "")))
            agent = self.registry.get(util.code_id(code))
            if not agent or not agent.writer:
                peer.send({"t": "error", "reason": "offline"})
                return
            if agent.partner or peer.partner:
                self._teardown_pair(agent.sid)
            sid = secrets.token_hex(8)
            peer.role = "viewer"
            peer.sid = sid
            agent.sid = sid
            peer.partner = agent
            agent.partner = peer
            peer.cands = list(msg.get("cands") or [])
            peer.name = str(msg.get("name", ""))[:64]
            pair = Pair(
                sid,
                agent,
                peer,
                secrets.randbits(32) | 1,
                secrets.randbits(32) | 1,
                bucket=TokenBucket(self.relay_mbps),
            )
            self.pairs[sid] = pair
            self.tag_map[pair.agent_tag] = pair
            self.tag_map[pair.viewer_tag] = pair
            log.info("pair %s agent=%s viewer=%s", sid, addr_of(agent), addr_of(peer))
            agent.send(
                {
                    "t": "viewer",
                    "sid": sid,
                    "cands": peer.cands,
                    "name": peer.name,
                    "relay": [self.public_host, self.udp_port],
                    "relay_tag": pair.agent_tag,
                    "relay_mbps": self.relay_mbps,
                }
            )
            peer.send(
                {
                    "t": "peer",
                    "sid": sid,
                    "cands": agent.cands,
                    "name": agent.name,
                    "relay": [self.public_host, self.udp_port],
                    "relay_tag": pair.viewer_tag,
                    "relay_mbps": self.relay_mbps,
                }
            )
        elif kind == "reconnect":
            code = util.normalize_code(str(msg.get("code", "")))
            if peer.role == "viewer" and peer.partner:
                peer.partner.send({"t": "viewer_gone"})
        elif kind in ("cands", "relay_hello", "bye", "ping"):
            if kind == "ping":
                peer.send({"t": "pong"})
                return
            if peer.partner:
                out = dict(msg)
                out["t"] = "peer_" + kind
                peer.partner.send(out)
        else:
            peer.send({"t": "error", "reason": "unknown_type"})

    def _drop(self, peer: Peer, addr) -> None:
        log.info("tcp disconnect %s role=%s", addr, peer.role)
        if peer.role == "agent" and self.registry.get(peer.code_id) is peer:
            self.registry.pop(peer.code_id, None)
        if peer.partner:
            peer.partner.send({"t": "peer_gone"})
        if peer.sid:
            self._teardown_pair(peer.sid)

    def _teardown_pair(self, sid: str) -> None:
        pair = self.pairs.pop(sid, None)
        if not pair:
            return
        self.tag_map.pop(pair.agent_tag, None)
        self.tag_map.pop(pair.viewer_tag, None)
        if pair.bucket.dropped or pair.bucket.enabled:
            log.info(
                "pair %s relay: %d 包 / %.1f MB, 限速丢包 %d",
                sid[:8],
                pair.bucket.passed,
                pair.bucket.bytes / (1 << 20),
                pair.bucket.dropped,
            )
        for p in (pair.agent, pair.viewer):
            p.partner = None
            p.sid = ""

    # ------------------------------------------------------------- relay logic
    def on_relay(self, tag: int, data: bytes, addr) -> None:
        pair = self.tag_map.get(tag)
        if not pair:
            return
        if tag == pair.agent_tag:
            pair.agent.udp_addr = addr
            dst = pair.viewer.udp_addr
        else:
            pair.viewer.udp_addr = addr
            dst = pair.agent.udp_addr
        if not dst or not self.relay or not self.relay.transport:
            return
        if not pair.bucket.allow(len(data)):
            return  # 超带宽：丢，不排队（视频/声音都是实时的，排队只会更糟）
        try:
            self.relay.transport.sendto(data, dst)
        except Exception:
            log.debug("relay forward failed to %s", dst)


def addr_of(peer: Peer) -> str:
    try:
        a = peer.writer.get_extra_info("peername")
        return f"{a[0]}:{a[1]}" if a else "?"
    except Exception:
        return "?"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="peerdesk-server", description="PeerDesk signaling + relay server")
    parser.add_argument("--host", default=os.environ.get("PEERDESK_BIND", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("PEERDESK_PORT", "7900")))
    parser.add_argument("--udp-port", type=int, default=int(os.environ.get("PEERDESK_UDP_PORT", "7901")))
    parser.add_argument(
        "--token",
        default=os.environ.get("PEERDESK_TOKEN", ""),
        help="握手令牌（设了之后 agent/viewer 必须带同一个值）",
    )
    parser.add_argument(
        "--relay-mbps",
        type=float,
        default=float(os.environ.get("PEERDESK_RELAY_MBPS", "0")),
        help="每个会话的中继带宽上限（Mbps，0=不限）",
    )
    parser.add_argument("--connect-max", type=int, default=CONNECT_MAX, help="窗口内每 IP 允许的连接尝试次数")
    parser.add_argument("--connect-window", type=float, default=CONNECT_WINDOW, help="连接尝试计数窗口（秒）")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)
    util.setup_logging(args.verbose)
    server = SignalingServer(
        args.host,
        args.port,
        args.udp_port,
        token=args.token,
        relay_mbps=args.relay_mbps,
        connect_max=args.connect_max,
        connect_window=args.connect_window,
    )
    if args.token:
        log.info("handshake token required (%d chars)", len(args.token))
    if args.relay_mbps:
        log.info("relay limited to %.1f Mbps per session", args.relay_mbps)
    try:
        asyncio.run(server.start())
    except KeyboardInterrupt:
        log.info("shutting down")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
