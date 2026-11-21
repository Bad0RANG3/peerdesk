"""Host agent: advertise a connection code, stream the desktop, apply input.

    python -m peerdesk.agent --server 192.168.1.10:7900

The agent opens a loopback web panel (http://127.0.0.1:<port>) showing the code,
the live link state and the capture settings.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import socket
import webbrowser

from . import signal as signaling
from . import util
from .session import HostSession, VideoSettings
from .transport import Candidate, PeerLink
from .webui import AgentHub, WebUI

log = logging.getLogger("peerdesk.agent")


class AgentApp:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.code = util.normalize_code(args.code) if args.code else util.generate_code()
        self.server_host, self.server_port = util.parse_addr(args.server, 7900)
        self.token = str(getattr(args, "token", "") or "").strip()
        self.settings = VideoSettings(
            fps=args.fps,
            max_width=args.width,
            quality=args.quality,
            monitor=args.monitor,
            skip_static=not args.no_skip_static,
            clipboard=not args.no_clipboard,
            audio=not getattr(args, "no_audio", False),
        )
        self.link: PeerLink | None = None
        self.host: HostSession | None = None
        self.signal: signaling.SignalingClient | None = None
        self.hub = AgentHub(code=self.code, server=f"{self.server_host}:{self.server_port}")
        self.ui = WebUI("agent.html", self.hub, port=args.web_port, title="PeerDesk · 本机")
        self._queue: asyncio.Queue = asyncio.Queue()
        self._relay: tuple[str, int] | None = None
        self._relay_tag = 0
        self._viewer_cands: list[Candidate] = []
        self.hub.on_command = self.on_command

    # ------------------------------------------------------------------ setup
    async def start(self) -> None:
        await self._make_link()
        await self.ui.start()
        log.info("panel: %s", self.ui.url)
        self.hub.push_event({"t": "notice", "text": f"面板已启动 {self.ui.url}"})
        if self.args.open:
            try:
                webbrowser.open(self.ui.url)
            except Exception:
                pass
        await self._connect_signaling()
        asyncio.get_event_loop().create_task(self._consume_signal())
        await self._idle()

    async def _make_link(self) -> None:
        if self.link:
            await self.link.close()
        self.link = PeerLink(
            is_host=True,
            psk=util.psk_from_code(self.code),
            label="agent",
            device_id=socket.gethostname(),
            force_relay=bool(getattr(self.args, "relay_only", False)),
            on_log=self._link_log,
        )
        await self.link.start(port=self.args.udp_port)
        self.host = HostSession(
            self.link,
            self.code,
            settings=self.settings,
            on_event=self._session_event,
            allow_input=not self.args.no_input,
            audio_test=bool(getattr(self.args, "audio_test", False)),
        )
        self.hub.attach(self.host)
        self.hub.code = self.code
        log.info("udp bound %s", self.link.local_addr)

    async def _connect_signaling(self) -> None:
        self.signal = signaling.SignalingClient(self.server_host, self.server_port, self._enqueue)
        while not await self.signal.connect():
            self.hub.push_event({"t": "link", "state": "offline", "path": ""})
            self.hub.push_event({"t": "log", "text": f"信令服务器不可达：{self.signal.last_error}"})
            log.warning("retrying signaling in 3s")
            await asyncio.sleep(3.0)
        await self._register()

    def _enqueue(self, msg: dict) -> None:
        self._queue.put_nowait(msg)

    async def _register(self) -> None:
        assert self.link and self.signal
        cands = signaling.cands_of(self.link)
        await self.signal.send(
            {
                "t": "register",
                "role": "agent",
                "code": self.code,
                "name": socket.gethostname(),
                "cands": cands,
                **signaling.token_args(self.token),
            }
        )
        self.hub.push_event({"t": "ready", "code": self.code, "server": f"{self.server_host}:{self.server_port}",
                             "name": socket.gethostname(), "monitors": self.host.info().get("monitors") if self.host else [],
                             "settings": self.settings.as_dict()})

    # ------------------------------------------------------------- signal loop
    async def _consume_signal(self) -> None:
        while True:
            msg = await self._queue.get()
            try:
                await self._handle_signal(msg)
            except Exception:
                log.exception("signal message")

    def _apply_rate_limit(self, msg: dict) -> None:
        if "relay_mbps" not in msg or not self.host:
            return
        mbps = float(msg.get("relay_mbps") or 0.0)
        if abs(mbps - self.host.rate_limit) > 1e-9:
            self.host.set_rate_limit(mbps)

    async def _handle_signal(self, msg: dict) -> None:
        kind = msg.get("t")
        assert self.link and self.signal
        if kind == "registered":
            self._apply_rate_limit(msg)
            relay = msg.get("relay") or []
            if len(relay) == 2:
                self._relay = (str(relay[0]), int(relay[1]))
                log.info("relay advertised %s", self._relay)
                mapped = await self.link.discover_reflexive(self._relay)
                if mapped:
                    log.info("reflexive address %s:%d", mapped[0], mapped[1])
            await self.signal.send({"t": "cands", "cands": signaling.cands_of(self.link)})
            self.hub.push_event({"t": "notice", "text": f"连接码 {util.format_code(self.code)} 已就绪"})
        elif kind == "viewer":
            self._apply_rate_limit(msg)
            relay = msg.get("relay") or []
            if len(relay) == 2:
                self._relay = (str(relay[0]), int(relay[1]))
            self._relay_tag = int(msg.get("relay_tag") or 0)
            cands = []
            for c in msg.get("cands") or []:
                try:
                    cands.append(Candidate(str(c["host"]), int(c["port"]), str(c.get("kind", "host"))))
                except Exception:
                    continue
            self._viewer_cands = cands
            self.link.set_candidates(cands)
            if self._relay:
                self.link.offer_relay(*self._relay)
            self.link.relay_out_tag = self._relay_tag
            log.info("viewer %s cands=%s relay_tag=%08x", msg.get("sid"), [c.key() for c in cands], self._relay_tag)
            self.hub.push_event({"t": "log", "text": f"控制端接入 {msg.get('sid')}"})
            self.link.begin_handshake()
        elif kind == "peer_cands":
            for c in msg.get("cands") or []:
                try:
                    self.link.set_candidates([Candidate(str(c["host"]), int(c["port"]), str(c.get("kind", "host")))])
                except Exception:
                    continue
        elif kind in ("peer_bye", "peer_gone"):
            log.info("viewer left")
            self.hub.push_event({"t": "link", "state": "closed", "path": ""})
        elif kind == "error":
            reason = msg.get("reason")
            text = {"bad_code": "连接码格式错误", "offline": "对端离线"}.get(reason, str(reason))
            self.hub.push_event({"t": "error", "text": text})

    # ------------------------------------------------------------------ events
    def _link_log(self, text: str) -> None:
        log.debug(text)
        self.hub.push_event({"t": "log", "text": text})

    def _session_event(self, ev: dict) -> None:
        if ev.get("t") == "stats":
            self.hub.push_event({"t": "stats", **{k: v for k, v in ev.items() if k != "t"}})
        else:
            self.hub.push_event(ev)

    async def on_command(self, conn, obj: dict) -> None:
        kind = obj.get("t")
        if kind == "regen":
            await self._regen()
        elif kind == "allow_input":
            if self.host:
                self.host.allow_input = bool(obj.get("v"))
                self.hub.push_event(
                    {"t": "notice", "text": "已允许键鼠控制" if self.host.allow_input else "已禁止键鼠控制"}
                )
        elif kind == "audio":
            if self.host:
                on = bool(obj.get("v", obj.get("on", True)))
                self.host.set_audio(on)
                self.hub.push_event({"t": "notice", "text": "已开启声音" if on else "已关闭声音"})

    async def _regen(self) -> None:
        self.code = util.generate_code()
        self.hub.push_event({"t": "log", "text": f"新连接码 {util.format_code(self.code)}"})
        await self._make_link()
        await self._register()

    async def _idle(self) -> None:
        try:
            while True:
                await asyncio.sleep(3600)
        except asyncio.CancelledError:
            pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="peerdesk-agent", description="PeerDesk host agent (被控端)")
    parser.add_argument("--server", default=os.environ.get("PEERDESK_SERVER", "127.0.0.1:7900"))
    parser.add_argument("--code", default=os.environ.get("PEERDESK_CODE", ""), help="固定连接码（默认随机生成）")
    parser.add_argument("--web-port", type=int, default=0, help="本地面板端口（0=随机）")
    parser.add_argument("--udp-port", type=int, default=0, help="媒体 UDP 端口（0=随机）")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--quality", type=int, default=62)
    parser.add_argument("--monitor", type=int, default=0)
    parser.add_argument(
        "--token",
        default=os.environ.get("PEERDESK_TOKEN", ""),
        help="信令服务器令牌（服务器用 --token 开启时必填）",
    )
    parser.add_argument("--relay-only", action="store_true", help="强制走中继（调试/联机受限网络）")
    parser.add_argument("--no-input", action="store_true", help="只读模式，忽略远端键鼠")
    parser.add_argument("--no-clipboard", action="store_true")
    parser.add_argument("--no-audio", action="store_true", help="不采集本机播放的声音（默认采集）")
    parser.add_argument("--audio-test", action="store_true", help="用 440 Hz 合成音代替声卡（联调/无声卡机器）")
    parser.add_argument("--no-skip-static", action="store_true")
    parser.add_argument(
        "--scale-mode",
        choices=("fast", "halftone", "pil"),
        default=os.environ.get("PEERDESK_SCALE", "fast"),
        help="抓屏缩放方式：fast=GDI 最近采样（最快）/ halftone=GDI 平滑 / pil=Pillow 双线性（最清晰但最慢）",
    )
    parser.add_argument(
        "--capture",
        choices=("auto", "gdi", "mss", "test"),
        default=os.environ.get("PEERDESK_CAPTURE", "auto"),
        help="抓屏后端（auto：先 GDI 再回退 mss；test：合成画面，不读真实屏幕，给 CI/演示用）",
    )
    parser.add_argument("--open", action="store_true", help="自动打开浏览器面板")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)
    util.setup_logging(args.verbose, str(util.config_dir() / "agent.log"))
    # 抓屏后端/缩放方式通过环境变量传给真正建抓屏器的线程
    if args.capture and args.capture != "auto":
        os.environ["PEERDESK_CAPTURE"] = args.capture
    if args.scale_mode:
        os.environ["PEERDESK_SCALE"] = args.scale_mode
    app = AgentApp(args)
    try:
        asyncio.run(app.start())
    except KeyboardInterrupt:
        log.info("agent stopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
