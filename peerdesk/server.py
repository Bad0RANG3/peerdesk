"""Self-hosted PeerDesk server: signaling (TCP/JSON lines) + relay + STUN (UDP).

One process is enough for a whole household or a small team: it introduces the
two peers, hands out relay tags, and forwards relayed datagrams when a direct
path cannot be established. No accounts, no metering, no subscription.

    python -m peerdesk.server --host 0.0.0.0 --port 7900 --udp-port 7901
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import secrets
import time
from dataclasses import dataclass, field
from typing import Optional

from . import stun, util
from .transport import RELAY_MAGIC

log = logging.getLogger("peerdesk.server")

MAX_LINE = 1 << 20
RELAY_SILENCE = 180.0


@dataclass
class Peer:
    writer: asyncio.StreamWriter
    role: str
    code_id: str = ""
    name: str = ""
    cands: list[dict] = field(default_factory=list)
    sid: str = ""
    partner: Optional["Peer"] = None
    relay_tag: int = 0
    udp_addr: Optional[tuple[str, int]] = None
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


class RelayProtocol(asyncio.DatagramProtocol):
    def __init__(self, server: "SignalingServer") -> None:
        self.server = server
        self.transport: Optional[asyncio.DatagramTransport] = None

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
    def __init__(self, host: str = "0.0.0.0", port: int = 7900, udp_port: int = 7901) -> None:
        self.host = host
        self.port = port
        self.udp_port = udp_port
        self.registry: dict[str, Peer] = {}
        self.pairs: dict[str, Pair] = {}
        self.tag_map: dict[int, Pair] = {}
        self.relay: Optional[RelayProtocol] = None
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

    async def _dispatch(self, peer: Peer, msg: dict) -> None:
        kind = msg.get("t")
        if kind == "register":
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
                }
            )
        elif kind == "connect":
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
            pair = Pair(sid, agent, peer, secrets.randbits(32) | 1, secrets.randbits(32) | 1)
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
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)
    util.setup_logging(args.verbose)
    server = SignalingServer(args.host, args.port, args.udp_port)
    try:
        asyncio.run(server.start())
    except KeyboardInterrupt:
        log.info("shutting down")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
