"""Viewer (controller): type a code, watch the remote desktop, drive it.

    python -m peerdesk.viewer --server 192.168.1.10:7900

The control surface is a loopback web page: the browser decodes frames into a
canvas and forwards mouse/keyboard events back over the WebSocket.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import webbrowser

from . import signal as signaling
from . import util
from .session import ViewerSession
from .transport import Candidate, PeerLink
from .webui import ViewerHub, WebUI

log = logging.getLogger("peerdesk.viewer")


class ViewerApp:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.server_host, self.server_port = util.parse_addr(args.server, 7900)
        self.link: PeerLink | None = None
        self.session: ViewerSession | None = None
        self.signal: signaling.SignalingClient | None = None
        self.hub = ViewerHub(on_command=self.on_command)
        self.ui = WebUI("viewer.html", self.hub, port=args.web_port, title="PeerDesk · 远程控制")
        self.code = ""
        self.token = str(getattr(args, "token", "") or "").strip()
        self._queue: asyncio.Queue = asyncio.Queue()
        self._connecting = False

    # ------------------------------------------------------------------ startup
    async def start(self) -> None:
        await self.ui.start()
        log.info("panel: %s", self.ui.url)
        self.hub.push_event({"t": "notice", "text": "输入对方连接码开始控制"})
        if self.args.open:
            try:
                webbrowser.open(self.ui.url)
            except Exception:
                pass
        asyncio.get_event_loop().create_task(self._consume_signal())
        if self.args.code:
            await self.connect(self.args.code, self.args.server)
        await self._idle()

    # ------------------------------------------------------------------ connect
    async def connect(self, code_text: str, server_text: str = "", token_text: str = "") -> None:
        code = util.normalize_code(code_text)
        if not util.code_is_valid(code):
            self.hub.push_event({"t": "error", "text": "连接码需要 9 位数字"})
            return
        if server_text:
            host, port = util.parse_addr(server_text, 7900)
            if (host, port) != (self.server_host, self.server_port):
                self.server_host, self.server_port = host, port
                if self.signal:
                    await self.signal.close()
                    self.signal = None
        self.code = code
        if token_text.strip():
            self.token = token_text.strip()
        self.hub.push_event({"t": "connecting", "code": code, "server": f"{self.server_host}:{self.server_port}"})
        await self._make_link()
        if not self.signal or not self.signal.connected:
            self.signal = signaling.SignalingClient(self.server_host, self.server_port, self._enqueue)
            if not await self.signal.connect():
                self.hub.push_event({"t": "error", "text": f"无法连接信令服务器：{self.signal.last_error}"})
                return
        await self.signal.send(
            {"t": "connect", "code": code, "cands": signaling.cands_of(self.link), **signaling.token_args(self.token)}
        )

    async def _make_link(self) -> None:
        if self.link:
            await self.link.close()
        self.link = PeerLink(
            is_host=False,
            psk=util.psk_from_code(self.code),
            label="viewer",
            force_relay=bool(getattr(self.args, "relay_only", False)),
            on_log=lambda text: log.debug(text),
        )
        await self.link.start(port=self.args.udp_port)
        self.session = ViewerSession(
            self.link,
            on_frame=self._on_frame,
            on_audio=self._on_audio,
            on_message=self._on_message,
        )
        self.hub.attach(self.session)
        log.info("udp bound %s", self.link.local_addr)

    def _on_frame(self, data: bytes, meta: dict) -> None:
        self.hub.post_frame(data)

    def _on_audio(self, payload: bytes) -> None:
        self.hub.post_audio(payload)

    def _on_message(self, msg: dict) -> None:
        kind = msg.get("t")
        if kind == "stats":
            self.hub.push_event({"t": "stats", **{k: v for k, v in msg.items() if k != "t"}, "jpeg_kb": round(msg.get("kb", 0))})
        elif kind in ("info", "link", "error", "notice", "clip", "audio"):
            self.hub.push_event(msg)

    # ------------------------------------------------------------- signal loop
    def _enqueue(self, msg: dict) -> None:
        self._queue.put_nowait(msg)

    async def _consume_signal(self) -> None:
        while True:
            msg = await self._queue.get()
            try:
                await self._handle_signal(msg)
            except Exception:
                log.exception("signal message")

    async def _handle_signal(self, msg: dict) -> None:
        kind = msg.get("t")
        assert self.link
        if kind == "peer":
            relay = msg.get("relay") or []
            cands = []
            for c in msg.get("cands") or []:
                try:
                    cands.append(Candidate(str(c["host"]), int(c["port"]), str(c.get("kind", "host"))))
                except Exception:
                    continue
            self.link.set_candidates(cands)
            if len(relay) == 2:
                self.link.offer_relay(str(relay[0]), int(relay[1]))
            self.link.relay_out_tag = int(msg.get("relay_tag") or 0)
            log.info("peer cands=%s relay=%s", [c.key() for c in cands], relay)
            self.link.begin_handshake()
        elif kind == "peer_cands":
            for c in msg.get("cands") or []:
                try:
                    self.link.set_candidates([Candidate(str(c["host"]), int(c["port"]), str(c.get("kind", "host")))])
                except Exception:
                    continue
        elif kind in ("peer_bye", "peer_gone"):
            self.hub.push_event({"t": "link", "state": "closed", "path": ""})
        elif kind == "error":
            reason = msg.get("reason")
            text = {
                "bad_code": "连接码格式错误",
                "offline": "对方不在线或码已失效",
                "bad_token": "服务器令牌不匹配",
                "rate_limited": "连接尝试过于频繁，稍后再试",
            }.get(reason, str(reason))
            self.hub.push_event({"t": "error", "text": text})

    # ------------------------------------------------------------------ commands
    async def on_command(self, conn, obj: dict) -> None:
        kind = obj.get("t")
        if kind == "connect":
            await self.connect(
                str(obj.get("code", "")), str(obj.get("server", "")), str(obj.get("token", ""))
            )
        elif kind == "hangup":
            if self.link:
                await self.link.close()
                self.link = None
                self.session = None
                self.hub.attach(None)
            self.hub.push_event({"t": "link", "state": "closed", "path": ""})

    async def _idle(self) -> None:
        while True:
            await asyncio.sleep(3600)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="peerdesk-viewer", description="PeerDesk viewer (控制端)")
    parser.add_argument("--server", default=os.environ.get("PEERDESK_SERVER", "127.0.0.1:7900"))
    parser.add_argument("--code", default=os.environ.get("PEERDESK_CODE", ""), help="直接连接的连接码")
    parser.add_argument("--web-port", type=int, default=0)
    parser.add_argument("--udp-port", type=int, default=0)
    parser.add_argument(
        "--token",
        default=os.environ.get("PEERDESK_TOKEN", ""),
        help="信令服务器令牌（页面里也可以临时填）",
    )
    parser.add_argument("--relay-only", action="store_true", help="强制走中继（调试）")
    parser.add_argument("--open", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)
    util.setup_logging(args.verbose, str(util.config_dir() / "viewer.log"))
    try:
        asyncio.run(ViewerApp(args).start())
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
