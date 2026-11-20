"""UDP transport: handshake, hole punching, reliability, FEC video.

Dependency free (asyncio only) so the same code runs on Windows, macOS and
Linux and a self-hosted server stays trivial to deploy.

Lifecycle
---------
``await link.start()``        bind a UDP socket
``link.set_candidates()``     feed candidates learned from signaling / HELLO
``link.begin_handshake()``    client: emit HELLO until HELLO_ACK; host: reply
``link.on_state``             idle -> punching -> direct | relay -> connected
``link.send_video``           fire-and-forget JPEG frame (fragmented + XOR FEC)
``link.send_control``         reliable, ordered JSON messages
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import logging
import os
import struct
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

from . import stun, util, wire
from .crypto import CryptoContext, KeyPair, SessionKeys

log = logging.getLogger("peerdesk.link")

MAX_DATAGRAM = 1400
PAYLOAD_MTU = MAX_DATAGRAM - wire.HEADER_LEN - 16
HELLO_INTERVAL = 0.25
HELLO_TIMEOUT = 15.0
PUNCH_INTERVAL = 0.10
KEEPALIVE = 4.0
RELAY_ESCALATE_AFTER = 6.0
RX_TIMEOUT = 25.0

RELAY_MAGIC = b"RLY1"
RELAY_HDR = struct.Struct(">4sI")


@dataclass
class Candidate:
    host: str
    port: int
    kind: str = "host"  # host | srflx | relay

    def key(self) -> tuple[str, int]:
        return (self.host, self.port)

    def as_dict(self) -> dict:
        return {"host": self.host, "port": int(self.port), "kind": self.kind}


@dataclass
class LinkStats:
    rtt_ms: float = 0.0
    sent_bytes: int = 0
    recv_bytes: int = 0
    sent_packets: int = 0
    recv_packets: int = 0
    fps: float = 0.0
    mbps: float = 0.0
    mode: str = "idle"
    path: str = ""
    using_relay: bool = False
    reflexive: str = ""
    started: float = field(default_factory=time.monotonic)


class ReliableStream:
    """Ordered, retransmitting message stream (cumulative ACK + 64 bit bitmap)."""

    RTO_MIN = 0.15
    RTO_MAX = 1.2
    MAX_RETRIES = 14

    def __init__(self, on_message: Callable[[bytes], None], send_fn: Callable[[int, bytes, int], None]) -> None:
        self._on_message = on_message
        self._send_raw = send_fn
        self._next_seq = 0
        self._pending: dict[int, list] = {}
        self._recv_next = 0
        self._recv_buf: dict[int, bytes] = {}
        self._last_ack_cum = -1

    def send(self, payload: bytes, flags: int = wire.FLAG_RELIABLE) -> int:
        seq = self._next_seq
        self._next_seq += 1
        self._pending[seq] = [payload, time.monotonic() + self.RTO_MIN, 0]
        self._send_raw(seq, payload, flags)
        return seq

    def tick(self, now: float) -> None:
        for seq, entry in list(self._pending.items()):
            payload, deadline, retries = entry
            if deadline > now:
                continue
            if retries >= self.MAX_RETRIES:
                self._pending.pop(seq, None)
                continue
            entry[1] = now + min(self.RTO_MIN * (2 ** (retries + 1)), self.RTO_MAX)
            entry[2] = retries + 1
            self._send_raw(seq, payload, wire.FLAG_RELIABLE)

    def on_ack(self, ack_cum: int, bitmap: int) -> None:
        # 累积 ACK：ack_cum 及之前的所有消息都已被对端接收（控制流严格按序交付）
        for seq in [s for s in self._pending if s <= ack_cum]:
            self._pending.pop(seq, None)
        for bit in range(64):
            if bitmap & (1 << bit):
                self._pending.pop(ack_cum - 1 - bit, None)

    def on_data(self, seq: int, payload: bytes) -> None:
        if seq < self._recv_next or seq in self._recv_buf:
            return
        self._recv_buf[seq] = payload
        while self._recv_next in self._recv_buf:
            msg = self._recv_buf.pop(self._recv_next)
            self._recv_next += 1
            self._on_message(msg)

    def ack_payload(self) -> bytes:
        ack_cum = self._recv_next - 1
        bitmap = 0
        for i in range(64):
            if (ack_cum - i) in self._recv_buf:
                bitmap |= 1 << i
        self._last_ack_cum = ack_cum
        return wire.pack_ack(max(ack_cum, 0), bitmap)

    def needs_ack(self) -> bool:
        return (self._recv_next - 1) != self._last_ack_cum

    def pending(self) -> int:
        return len(self._pending)


class VideoSender:
    """Fragments JPEG frames, adds XOR parity, keeps a retransmit cache."""

    FEC_K = 8

    def __init__(self, mtu: int = PAYLOAD_MTU - wire.VIDEO_FRAG.size, cache_frames: int = 10) -> None:
        self.mtu = max(200, mtu)
        self.cache: dict[int, list[bytes]] = {}
        self.order: list[int] = []
        self.cache_frames = cache_frames
        self.frame_id = 0

    def build(self, data: bytes, keyframe: bool) -> list[tuple[int, int, bytes]]:
        """Return a list of (packet_type, flags, payload)."""
        self.frame_id = (self.frame_id + 1) & 0xFFFFFFFF
        fid = self.frame_id
        frags = wire.fragment(data, self.mtu)
        out: list[tuple[int, int, bytes]] = []
        last = len(frags) - 1
        for idx, chunk in enumerate(frags):
            flags = wire.FLAG_LAST_FRAG if idx == last else 0
            if keyframe:
                flags |= wire.FLAG_KEYFRAME
            out.append((wire.TYPE_DATA, flags, wire.pack_video_fragment(fid, idx, len(frags), chunk)))
        if self.FEC_K > 1:
            for gidx, start in enumerate(range(0, len(frags), self.FEC_K)):
                shards = frags[start : start + self.FEC_K]
                if len(shards) < 3:
                    continue
                parity = wire.xor_bytes(shards, max(len(s) for s in shards))
                out.append((wire.TYPE_PARITY, 0, struct.pack("<IHBB", fid, gidx, len(shards), 0) + parity))
        self.cache[fid] = frags
        self.order.append(fid)
        while len(self.order) > self.cache_frames:
            self.cache.pop(self.order.pop(0), None)
        return out

    def retransmit(self, frame_id: int, missing: list[int]) -> list[tuple[int, int, bytes]]:
        frags = self.cache.get(frame_id)
        if not frags:
            return []
        out = []
        for idx in missing:
            if idx < len(frags):
                flags = wire.FLAG_LAST_FRAG if idx == len(frags) - 1 else 0
                out.append((wire.TYPE_DATA, flags, wire.pack_video_fragment(frame_id, idx, len(frags), frags[idx])))
        return out


@dataclass
class _FrameState:
    frag_cnt: int
    asm: wire.FragmentAssembler
    keyframe: bool
    created: float
    last_nack: float = 0.0


class VideoReceiver:
    def __init__(self, on_frame: Callable[[bytes, bool], None], on_need_keyframe: Callable[[], None], fec_k: int = 8) -> None:
        self._on_frame = on_frame
        self._on_need_keyframe = on_need_keyframe
        self._frames: dict[int, _FrameState] = {}
        self.fec_k = fec_k
        self.last_delivered = 0
        self.lost_frames = 0

    def _deliver(self, fid: int, st: _FrameState) -> None:
        self._frames.pop(fid, None)
        self.last_delivered = fid
        self._on_frame(st.asm.assemble(), st.keyframe)

    def on_fragment(self, fid: int, idx: int, cnt: int, data: bytes, keyframe: bool) -> None:
        st = self._frames.get(fid)
        if st is None:
            if cnt <= 0 or cnt > 65535:
                return
            st = _FrameState(cnt, wire.FragmentAssembler(cnt), keyframe, time.monotonic())
            self._frames[fid] = st
        st.asm.add(idx, data)
        if st.asm.complete:
            self._deliver(fid, st)

    def on_parity(self, fid: int, gidx: int, shards: int, data: bytes) -> None:
        st = self._frames.get(fid)
        if st is None:
            return
        indices = list(range(gidx * self.fec_k, min((gidx + 1) * self.fec_k, st.frag_cnt)))
        if st.asm.fill_parity(gidx, data, indices) and st.asm.complete:
            self._deliver(fid, st)

    def tick(self, now: float) -> list[tuple[int, list[int]]]:
        requests: list[tuple[int, list[int]]] = []
        for fid, st in list(self._frames.items()):
            if now - st.created > 0.45:
                self._frames.pop(fid, None)
                self.lost_frames += 1
                if st.keyframe:
                    self._on_need_keyframe()
                continue
            if now - st.last_nack > 0.05 and len(self._frames) <= 4:
                missing = st.asm.missing()
                if missing:
                    st.last_nack = now
                    requests.append((fid, missing[:32]))
        return requests


class PeerLink(asyncio.DatagramProtocol):
    def __init__(
        self,
        is_host: bool,
        psk: bytes = b"",
        *,
        on_state: Optional[Callable[[str, LinkStats], None]] = None,
        on_control: Optional[Callable[[dict], None]] = None,
        on_video: Optional[Callable[[bytes, bool], None]] = None,
        on_log: Optional[Callable[[str], None]] = None,
        label: str = "",
        device_id: str = "",
        force_relay: bool = False,
    ) -> None:
        self.is_host = is_host
        self.psk = psk
        self.force_relay = force_relay
        self.label = label
        self.device_id = device_id
        self.on_state = on_state
        self.on_control = on_control
        self.on_video = on_video
        self.on_log = on_log or (lambda m: log.debug(m))

        self.loop = asyncio.get_event_loop()
        self.transport: Optional[asyncio.DatagramTransport] = None
        self.local_addr: Optional[tuple[str, int]] = None
        self.public_addr: Optional[tuple[str, int]] = None
        self.candidates: list[Candidate] = []
        self.peer_addr: Optional[tuple[str, int]] = None
        self.relay_addr: Optional[tuple[str, int]] = None
        self.relay_out_tag: int = 0
        self.using_relay = False
        self.srflx: Optional[tuple[str, int]] = None
        self._stun_txid = b""
        self._stun_future: Optional[asyncio.Future] = None

        self.session_id = os.urandom(4)
        self.session_tag = int.from_bytes(os.urandom(4), "big")
        self.state = "idle"
        self.stats = LinkStats()

        self._kp = KeyPair.generate()
        self._crypto: Optional[CryptoContext] = None
        self._hello_client_bytes: bytes = b""
        self._hello_host_bytes: bytes = b""
        self._hello_sent_at = 0.0
        self._handshake_started = 0.0
        self._last_rx = 0.0
        self._last_ping = 0.0
        self._established = False
        self._remote_sid = ""

        self._video_sender = VideoSender()
        self._video_recv = VideoReceiver(self._deliver_video, self._request_keyframe, fec_k=VideoSender.FEC_K)
        self._ctrl = ReliableStream(self._deliver_control, self._send_control_raw)
        self._tasks: list[asyncio.Task] = []
        self._last_ack_at = 0.0
        self._last_state_sent = "idle"
        self._last_state_at = 0.0
        self._rtt = 0.0
        self._ping_out: dict[int, float] = {}
        self._ping_seq = 0
        self._closing = False
        self._bytes_window: list[tuple[float, int]] = []
        self._frames_window: list[float] = []
        self._rx_window: list[tuple[float, int]] = []
        self._rx_frames: list[float] = []
        self._keyframe_requests = 0

    # ------------------------------------------------------------------ set up
    def connection_made(self, transport) -> None:
        self.transport = transport
        self.local_addr = transport.get_extra_info("sockname")[:2]
        self._log(f"bound udp {self.local_addr}")

    def connection_lost(self, exc) -> None:
        if exc:
            self._log(f"udp closed: {exc}")

    def _log(self, msg: str) -> None:
        try:
            self.on_log(f"[{self.label or 'link'}] {msg}")
        except Exception:
            pass

    async def start(self, port: int = 0, host: str = "0.0.0.0") -> None:
        await self.loop.create_datagram_endpoint(lambda: self, local_addr=(host, port))
        self._tasks.append(self.loop.create_task(self._timer_loop()))

    async def close(self) -> None:
        if self._closing:
            return
        self._closing = True
        try:
            self._send_packet(wire.TYPE_BYE, 0, 0, b"")
        except Exception:
            pass
        for task in self._tasks:
            task.cancel()
        self._tasks.clear()
        if self.transport:
            self.transport.close()
        self._set_state("closed")

    # -------------------------------------------------------------- candidates
    def set_candidates(self, cands: list[Candidate]) -> None:
        known = {c.key() for c in self.candidates}
        self.candidates.extend(c for c in cands if c.key() not in known)

    def begin_handshake(self) -> None:
        self._handshake_started = time.monotonic()
        if self.force_relay and self.relay_addr:
            self.using_relay = True
            self.stats.using_relay = True
            self._log("force relay mode: skipping direct paths")
        self._set_state("punching")
        self._send_hello(force=True)

    def offer_relay(self, host: str, port: int) -> None:
        self.relay_addr = (host, port)
        self._log(f"relay offered {host}:{port}")

    # ------------------------------------------------------------------ sending
    def _hello_body(self) -> bytes:
        return wire.json_payload(
            {
                "v": 1,
                "sid": self.session_id.hex(),
                "pub": base64.b64encode(self._kp.public_bytes).decode(),
                "psk": hashlib.sha256(b"peerdesk-psk" + self.psk).hexdigest()[:16],
                "role": "host" if self.is_host else "client",
                "id": self.device_id,
                "cands": [c.as_dict() for c in self.candidates],
                "ts": int(time.time() * 1000),
            }
        )

    def _send_hello(self, force: bool = False) -> None:
        now = time.monotonic()
        if not force and now - self._hello_sent_at < HELLO_INTERVAL:
            return
        # The hello body feeds the key schedule, so it must stay byte-identical
        # across retransmissions: build it once and reuse it.
        if self.is_host:
            body = self._hello_host_bytes or self._hello_body()
            self._hello_host_bytes = body
            typ = wire.TYPE_HELLO_ACK
        else:
            body = self._hello_client_bytes or self._hello_body()
            self._hello_client_bytes = body
            typ = wire.TYPE_HELLO
        self._hello_sent_at = now
        for addr in self._targets():
            self._send_packet(typ, 0, 0, body, addr, plain=True)

    def _targets(self) -> list[tuple[str, int]]:
        out: list[tuple[str, int]] = []
        if self.peer_addr:
            out.append(self.peer_addr)
        for c in self.candidates:
            if c.key() not in out:
                out.append(c.key())
        if self.using_relay and self.relay_addr:
            return [self.relay_addr]
        return out

    def _send_packet(self, typ: int, stream: int, flags: int, payload: bytes, addr=None, plain: bool = False, seq: Optional[int] = None) -> None:
        if not self.transport:
            return
        addr = addr or self.peer_addr or (self.candidates[0].key() if self.candidates else None)
        if not addr and not self.using_relay:
            return
        if plain:
            seq_v = 0
            body = payload
            plen = len(payload)
        else:
            if not self._crypto:
                return
            seq_v = self._crypto.next_seq() if seq is None else seq
            body = b""
            plen = len(payload) + 16
        head = wire.Header(
            wire.VERSION,
            typ,
            seq_v,
            int(time.monotonic() * 1000) & 0xFFFFFFFF,
            stream,
            flags,
            plen,
            self.session_tag,
        ).pack()
        if not plain:
            body = self._crypto.seal(head, payload, seq_v)
        data = head + body
        assert self.transport
        if self.using_relay and self.relay_addr:
            self.transport.sendto(RELAY_HDR.pack(RELAY_MAGIC, self.relay_out_tag or self.session_tag) + data, self.relay_addr)
        else:
            self.transport.sendto(data, addr)
        self.stats.sent_bytes += len(data)
        self.stats.sent_packets += 1
        self._bytes_window.append((time.monotonic(), len(data)))
        if len(self._bytes_window) > 600:
            self._bytes_window.pop(0)

    def send_control(self, obj: dict) -> None:
        if self._crypto:
            self._ctrl.send(wire.json_payload(obj))

    def send_video(self, jpeg: bytes, keyframe: bool = False) -> None:
        if not self._crypto or not self._established:
            return
        for typ, flags, payload in self._video_sender.build(jpeg, keyframe):
            self._send_packet(typ, wire.STREAM_VIDEO, flags, payload)
        self._frames_window.append(time.monotonic())
        if len(self._frames_window) > 300:
            self._frames_window.pop(0)

    def _send_control_raw(self, seq: int, payload: bytes, flags: int) -> None:
        # The reliable-stream sequence travels inside the payload: the packet
        # header seq is the AEAD nonce counter and must stay strictly monotonic
        # per direction, otherwise nonces repeat and the replay window rejects
        # legitimate retransmissions.
        self._send_packet(wire.TYPE_DATA, wire.STREAM_CONTROL, flags, struct.pack("<I", seq) + payload)

    def _request_keyframe(self) -> None:
        self._keyframe_requests += 1
        if self._established:
            self.send_control({"t": "keyframe_req"})

    # ---------------------------------------------------------------- receiving
    def _on_stun(self, data: bytes) -> None:
        parsed = stun.parse_message(data)
        if not parsed:
            return
        mtype, txid, attrs = parsed
        if mtype != stun.BINDING_SUCCESS or txid != self._stun_txid:
            return
        mapped = stun.parse_mapped(attrs, txid)
        if mapped:
            self.srflx = mapped
            self.stats.reflexive = f"{mapped[0]}:{mapped[1]}"
            if self._stun_future and not self._stun_future.done():
                self._stun_future.set_result(mapped)

    async def discover_reflexive(self, server: tuple[str, int], timeout: float = 2.5):
        """Ask the server for our public UDP mapping, using *this* socket."""
        if not self.transport:
            return None
        loop = asyncio.get_event_loop()
        self._stun_future = loop.create_future()
        packet, self._stun_txid = stun.build_request()
        self.transport.sendto(packet, server)
        try:
            return await asyncio.wait_for(self._stun_future, timeout)
        except asyncio.TimeoutError:
            return None

    def candidate_list(self) -> list[Candidate]:
        out: list[Candidate] = []
        if self.local_addr and self.local_addr[0] not in ("0.0.0.0", "::"):
            out.append(Candidate(self.local_addr[0], self.local_addr[1], "host"))
        if self.srflx:
            out.append(Candidate(self.srflx[0], self.srflx[1], "srflx"))
        for ip in util.local_ipv4s():
            if self.local_addr:
                out.append(Candidate(ip, self.local_addr[1], "host"))
        seen = set()
        uniq = []
        for c in out:
            if c.key() in seen:
                continue
            seen.add(c.key())
            uniq.append(c)
        return uniq

    def datagram_received(self, data: bytes, addr) -> None:
        self.stats.recv_bytes += len(data)
        self.stats.recv_packets += 1
        self._rx_window.append((time.monotonic(), len(data)))
        if len(self._rx_window) > 2000:
            del self._rx_window[:1000]
        if len(data) >= 20 and data[:2] in (b"\x00\x01", b"\x01\x01"):
            self._on_stun(data)
            return
        hdr = wire.parse_header(data)
        if not hdr:
            return
        body = data[wire.HEADER_LEN :]

        if hdr.type == wire.TYPE_HELLO:
            if self.is_host:
                self._handle_hello(body, addr)
            return
        if hdr.type == wire.TYPE_HELLO_ACK:
            if not self.is_host:
                self._handle_hello_ack(body, addr)
            return
        if hdr.type == wire.TYPE_PUNCH:
            if not self.peer_addr:
                self.peer_addr = addr
            self._send_packet(wire.TYPE_PUNCH_ACK, 0, 0, b"", addr, plain=True)
            if self.is_host and not self._crypto:
                self._send_hello(force=True)
            return
        if hdr.type == wire.TYPE_PUNCH_ACK:
            if not self.peer_addr:
                self.peer_addr = addr
            return
        if hdr.type == wire.TYPE_BYE and not self._crypto:
            self._set_state("closed")
            return

        if not self._crypto:
            return
        payload = self._crypto.open(data[: wire.HEADER_LEN], body, hdr.seq)
        if payload is None:
            return
        if not self.peer_addr and not self.using_relay:
            self.peer_addr = addr
            self._log(f"path locked {addr}")
        self._last_rx = time.monotonic()
        if not self._established:
            self._established = True
            self._set_state("relay" if self.using_relay else "direct")
            self.send_control(
                {"t": "hello", "role": "host" if self.is_host else "client", "v": 1, "id": self.device_id}
            )

        if hdr.type == wire.TYPE_DATA:
            if hdr.stream == wire.STREAM_VIDEO:
                fid, idx, cnt, chunk = wire.unpack_video_fragment(payload)
                self._video_recv.on_fragment(fid, idx, cnt, chunk, bool(hdr.flags & wire.FLAG_KEYFRAME))
            elif hdr.stream == wire.STREAM_CONTROL and len(payload) >= 4:
                msg_seq = struct.unpack_from("<I", payload, 0)[0]
                self._ctrl.on_data(msg_seq, payload[4:])
                self._maybe_ack()
        elif hdr.type == wire.TYPE_PARITY:
            fid, gidx, shards, _ = struct.unpack_from("<IHBB", payload, 0)
            self._video_recv.on_parity(fid, gidx, shards, payload[8:])
        elif hdr.type == wire.TYPE_ACK:
            cum, bitmap = wire.unpack_ack(payload)
            self._ctrl.on_ack(cum, bitmap)
        elif hdr.type == wire.TYPE_NACK:
            fid, missing = wire.unpack_nack(payload)
            for typ, flags, pay in self._video_sender.retransmit(fid, list(missing)):
                self._send_packet(typ, wire.STREAM_VIDEO, flags, pay)
        elif hdr.type == wire.TYPE_PING:
            self._send_packet(wire.TYPE_PONG, 0, 0, payload)
        elif hdr.type == wire.TYPE_PONG:
            self._on_pong(payload)
        elif hdr.type == wire.TYPE_BYE:
            self._log("peer closed")
            self._set_state("closed")

    def _on_pong(self, payload: bytes) -> None:
        obj = wire.parse_json(payload) or {}
        sent = self._ping_out.pop(int(obj.get("n", -1)), None)
        if sent:
            self._rtt = (time.monotonic() - sent) * 1000.0
            self.stats.rtt_ms = self._rtt

    # -- handshake -----------------------------------------------------------
    def _handle_hello(self, body: bytes, addr) -> None:
        obj = wire.parse_json(body) or {}
        try:
            pub = base64.b64decode(obj.get("pub", ""))
        except Exception:
            return
        if len(pub) != 32:
            return
        self._hello_client_bytes = body
        self._remote_sid = obj.get("sid", "")
        if self.device_id and obj.get("id"):
            self._remote_id = obj["id"]
        if not self.peer_addr:
            self.peer_addr = addr
        self._absorb_candidates(obj)
        if not self._crypto:
            # Reuse the body already sent to this peer (if any): the client may
            # have derived keys from the earlier retransmission.
            ack = self._hello_host_bytes or self._hello_body()
            self._hello_host_bytes = ack
            keys = SessionKeys.derive(self._kp, pub, ack, body, self.psk)
            self._crypto = CryptoContext(keys, is_host=True)
            self._log(f"hello from {addr}; keys derived, sending ack")
            self._hello_sent_at = time.monotonic()
            self._send_packet(wire.TYPE_HELLO_ACK, 0, 0, ack, addr, plain=True)
        self._set_state("punching")

    def _handle_hello_ack(self, body: bytes, addr) -> None:
        obj = wire.parse_json(body) or {}
        try:
            pub = base64.b64decode(obj.get("pub", ""))
        except Exception:
            return
        if len(pub) != 32:
            return
        if not self.peer_addr:
            self.peer_addr = addr
        self._hello_host_bytes = body
        self._remote_sid = obj.get("sid", "")
        if self.device_id and obj.get("id"):
            self._remote_id = obj["id"]
        self._absorb_candidates(obj)
        if not self._crypto:
            keys = SessionKeys.derive(self._kp, pub, body, self._hello_client_bytes, self.psk)
            self._crypto = CryptoContext(keys, is_host=False)
            self._log(f"hello_ack from {addr}; keys derived")
            self._ping_seq = 1
            self._ping_out[1] = time.monotonic()
            self._last_ping = time.monotonic()
            self._send_packet(wire.TYPE_PING, 0, 0, wire.json_payload({"n": 1}))

    _remote_id = ""

    def _absorb_candidates(self, obj: dict) -> None:
        extra = []
        for c in obj.get("cands") or []:
            try:
                extra.append(Candidate(str(c["host"]), int(c["port"]), str(c.get("kind", "host"))))
            except Exception:
                continue
        if extra:
            self.set_candidates(extra)

    def _maybe_ack(self) -> None:
        now = time.monotonic()
        if self._ctrl.needs_ack() and now - self._last_ack_at > 0.004:
            self._last_ack_at = now
            self._send_packet(wire.TYPE_ACK, 0, 0, self._ctrl.ack_payload())

    def _deliver_control(self, payload: bytes) -> None:
        obj = wire.parse_json(payload)
        if obj and self.on_control:
            try:
                self.on_control(obj)
            except Exception:
                log.exception("control handler failed")

    def _deliver_video(self, payload: bytes, keyframe: bool) -> None:
        self._rx_frames.append(time.monotonic())
        if len(self._rx_frames) > 400:
            del self._rx_frames[:200]
        if self.on_video:
            self.on_video(payload, keyframe)

    # ------------------------------------------------------------------- loops
    async def _timer_loop(self) -> None:
        while not self._closing:
            now = time.monotonic()
            try:
                self._ctrl.tick(now)
                for fid, missing in self._video_recv.tick(now):
                    self._send_packet(wire.TYPE_NACK, 0, 0, wire.pack_nack(fid, missing), plain=False)
                if not self._crypto:
                    if self._handshake_started and now - self._handshake_started < HELLO_TIMEOUT:
                        self._send_hello()
                        for c in self.candidates:
                            self._send_packet(wire.TYPE_PUNCH, 0, 0, b"", c.key(), plain=True)
                elif not self._established:
                    self._send_hello()
                    for c in self.candidates:
                        self._send_packet(wire.TYPE_PUNCH, 0, 0, b"", c.key(), plain=True)
                    if (
                        self.relay_addr
                        and not self.using_relay
                        and self._handshake_started
                        and now - self._handshake_started > RELAY_ESCALATE_AFTER
                    ):
                        self.using_relay = True
                        self.stats.using_relay = True
                        self._send_hello(force=True)
                        self._log("escalating to relay")
                else:
                    if now - self._last_ping > KEEPALIVE:
                        self._send_ping()
                    if now - self._last_rx > RX_TIMEOUT:
                        self._log("rx timeout, re-punching")
                        self._established = False
                        self._set_state("punching")
            except Exception:  # pragma: no cover
                log.exception("link timer")
            self._update_stats()
            await asyncio.sleep(0.02)

    def _send_ping(self) -> None:
        self._ping_seq = (self._ping_seq + 1) & 0xFFFF
        self._ping_out[self._ping_seq] = time.monotonic()
        if len(self._ping_out) > 32:
            self._ping_out.pop(min(self._ping_out))
        self._last_ping = time.monotonic()
        self._send_packet(wire.TYPE_PING, 0, 0, wire.json_payload({"n": self._ping_seq}))

    def _update_stats(self) -> None:
        now = time.monotonic()
        cutoff = now - 1.0
        if self._bytes_window and self._bytes_window[0][0] < cutoff:
            self._bytes_window = [x for x in self._bytes_window if x[0] >= cutoff]
        if self._frames_window and self._frames_window[0] < cutoff:
            self._frames_window = [t for t in self._frames_window if t >= cutoff]
        if self._rx_window and self._rx_window[0][0] < cutoff:
            self._rx_window = [x for x in self._rx_window if x[0] >= cutoff]
        if self._rx_frames and self._rx_frames[0] < cutoff:
            self._rx_frames = [t for t in self._rx_frames if t >= cutoff]
        # throughput counts both directions; fps counts whichever side moves frames
        self.stats.mbps = (sum(b for _, b in self._bytes_window) + sum(b for _, b in self._rx_window)) * 8 / 1e6
        self.stats.fps = float(len(self._frames_window) + len(self._rx_frames))
        self.stats.mode = self.state
        self.stats.using_relay = self.using_relay
        self.stats.path = f"{self.peer_addr[0]}:{self.peer_addr[1]}" if self.peer_addr else ""
        changed = self.state != self._last_state_sent
        if self.on_state and (changed or now - self._last_state_at >= 1.0):
            self._last_state_sent = self.state
            self._last_state_at = now
            try:
                self.on_state(self.state, self.stats)
            except Exception:
                pass

    def _set_state(self, state: str) -> None:
        if state != self.state:
            self.state = state
            self.stats.mode = state
            self._log(f"state -> {state}")
            if self.on_state:
                try:
                    self.on_state(state, self.stats)
                except Exception:
                    pass

    @property
    def established(self) -> bool:
        return self._established

    @property
    def rtt_ms(self) -> float:
        return self._rtt
