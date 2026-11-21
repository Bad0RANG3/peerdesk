"""Local web UI: static pages + WebSocket bridge to the session objects.

The desktop UI is a browser window talking to a loopback HTTP/WS server. That
keeps the client a single Python process with no Qt/Electron dependency while
still giving us the polished look of a native app.
"""

from __future__ import annotations

import asyncio
import json
import logging
import mimetypes
from pathlib import Path
from urllib.parse import unquote

from . import util
from .websocket import WSConnection

log = logging.getLogger("peerdesk.webui")

WEB_DIR = Path(__file__).resolve().parent / "web"
FRAME_MAGIC = b"J"
UPLOAD_MAGIC = b"U"
AUDIO_MAGIC = b"A"


class _Conn:
    __slots__ = ("ws", "busy", "pending")

    def __init__(self, ws: WSConnection) -> None:
        self.ws = ws
        self.busy = False
        self.pending: bytes | None = None


class Hub:
    """Base class: fan-out of JSON/binary messages to connected browsers."""

    STICKY = ("link", "stats", "info", "settings", "notice")

    def __init__(self) -> None:
        self.conns: list[_Conn] = []
        self._loop: asyncio.AbstractEventLoop | None = None
        self.sticky: dict[str, dict] = {}

    def bind_loop(self) -> None:
        self._loop = asyncio.get_event_loop()

    async def add(self, ws: WSConnection) -> _Conn:
        conn = _Conn(ws)
        self.conns.append(conn)
        return conn

    def drop(self, conn: _Conn) -> None:
        if conn in self.conns:
            self.conns.remove(conn)

    async def broadcast_json(self, obj: dict) -> None:
        text = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
        for conn in list(self.conns):
            try:
                await conn.ws.send_text(text)
            except Exception:
                self.drop(conn)

    def remember(self, obj: dict) -> None:
        """Keep the latest of each sticky message so a fresh tab can catch up."""
        kind = obj.get("t")
        if kind in self.STICKY:
            self.sticky[kind] = obj

    async def replay(self, conn: _Conn) -> None:
        for obj in self.sticky.values():
            try:
                await conn.ws.send_text(json.dumps(obj, ensure_ascii=False))
            except Exception:
                return

    def post_json(self, obj: dict) -> None:
        """Thread-safe fire-and-forget broadcast."""
        self.remember(obj)
        if self._loop and not self._loop.is_closed():
            asyncio.run_coroutine_threadsafe(self.broadcast_json(obj), self._loop)

    def push_event(self, obj: dict) -> None:
        """Alias used by the session/apps for fire-and-forget UI events."""
        self.post_json(obj)

    def post_frame(self, data: bytes) -> None:
        """Thread/task-safe frame push (drops stale frames under backpressure)."""
        if self._loop and not self._loop.is_closed():
            asyncio.ensure_future(self.broadcast_frame(data))

    async def broadcast_frame(self, data: bytes) -> None:
        for conn in list(self.conns):
            if conn.busy:
                conn.pending = data
                continue
            conn.busy = True
            asyncio.ensure_future(self._pump(conn, data))

    async def _pump(self, conn: _Conn, data: bytes) -> None:
        try:
            while data:
                await conn.ws.send_binary(FRAME_MAGIC + data)
                data = conn.pending
                conn.pending = None
        except Exception:
            self.drop(conn)
        finally:
            conn.busy = False

    def post_audio(self, payload: bytes) -> None:
        """音频帧不进帧通道：视频可以丢，声音丢了就是断音。"""
        if self._loop and not self._loop.is_closed():
            asyncio.ensure_future(self.broadcast_audio(payload))

    async def broadcast_audio(self, payload: bytes) -> None:
        data = AUDIO_MAGIC + payload
        for conn in list(self.conns):
            try:
                await conn.ws.send_binary(data)
            except Exception:
                self.drop(conn)

    async def on_connect(self, conn: _Conn) -> None:  # pragma: no cover - overridden
        pass

    async def on_json(self, conn: _Conn, obj: dict) -> None:  # pragma: no cover - overridden
        pass

    async def on_binary(self, conn: _Conn, payload: bytes) -> None:  # pragma: no cover - overridden
        """浏览器发来的二进制帧（上传分块）。默认忽略。"""


class ViewerHub(Hub):
    """Browser (controller) side bridge."""

    def __init__(self, session=None, on_command=None) -> None:
        super().__init__()
        self.session = session
        self.on_command = on_command

    def attach(self, session) -> None:
        self.session = session

    async def on_connect(self, conn: _Conn) -> None:
        await conn.ws.send_text(json.dumps({"t": "ready", "role": "viewer"}))
        if self.session and self.session.info:
            await conn.ws.send_text(json.dumps({"t": "info", **self.session.info}, ensure_ascii=False))
        await self.replay(conn)

    async def on_json(self, conn: _Conn, obj: dict) -> None:
        session = self.session
        if session is None:
            if self.on_command:
                await self.on_command(conn, obj)
            return
        kind = obj.get("t")
        if kind == "m":
            act = obj.get("act", "move")
            msg = {"t": "mouse", "act": act, "x": obj.get("x", 0), "y": obj.get("y", 0)}
            if act in ("down", "up", "click"):
                msg["btn"] = obj.get("btn", "left")
            if act == "wheel":
                msg["dy"] = obj.get("dy", 0)
            session.send(msg)
        elif kind == "k":
            session.send({"t": "key", "code": obj.get("code", ""), "down": bool(obj.get("down"))})
        elif kind == "text":
            session.send({"t": "text", "s": obj.get("s", "")})
        elif kind == "clip":
            session.send({"t": "clip", "s": obj.get("s", "")})
        elif kind == "quality":
            session.set_quality(obj.get("q"), obj.get("w"), obj.get("fps"))
        elif kind == "monitor":
            session.select_monitor(int(obj.get("i", 0)))
        elif kind == "keyframe":
            session.request_keyframe()
        elif kind == "audio":
            session.set_audio(bool(obj.get("on", True)))
        elif kind == "upload_begin":
            try:
                session.begin_upload(str(obj.get("name") or "file.bin"), int(obj.get("size") or 0))
            except Exception:
                log.exception("upload_begin failed")
        elif kind == "upload_end":
            session.end_upload()
        elif kind == "file":
            path = obj.get("path")
            if path:
                try:
                    session.send_file(Path(path))
                except Exception:
                    log.exception("send_file failed")
        elif self.on_command:
            await self.on_command(conn, obj)

    async def on_binary(self, conn: _Conn, payload: bytes) -> None:
        """浏览器上传：帧首字节 b"U" + 原始字节。"""
        if payload[:1] != UPLOAD_MAGIC or self.session is None:
            return
        try:
            self.session.upload_chunk(payload[1:])
        except Exception:
            log.exception("upload_chunk failed")

    async def on_frame(self, data: bytes) -> None:  # called by ViewerSession
        await self.broadcast_frame(data)


class AgentHub(Hub):
    """Browser (host panel) side bridge."""

    def __init__(self, host=None, code: str = "", server: str = "", on_command=None) -> None:
        super().__init__()
        self.host = host
        self.code = code
        self.server = server
        self.events: list[dict] = []
        self.on_command = on_command

    def attach(self, host) -> None:
        self.host = host

    async def on_connect(self, conn: _Conn) -> None:
        payload = {
            "t": "ready",
            "role": "agent",
            "code": self.code,
            "server": self.server,
            "name": self.host.info().get("name") if self.host else "",
            "monitors": (self.host.info().get("monitors") if self.host else []),
            "settings": (self.host.settings.as_dict() if self.host else {}),
            "audio": (self.host.audio_state() if self.host else {}),
        }
        await conn.ws.send_text(json.dumps(payload, ensure_ascii=False))
        await self.replay(conn)
        for ev in self.events[-40:]:
            await conn.ws.send_text(json.dumps(ev, ensure_ascii=False))

    async def on_json(self, conn: _Conn, obj: dict) -> None:
        if self.host is None:
            return
        kind = obj.get("t")
        if kind == "settings":
            self.host.settings.update(obj.get("settings") or {})
            self.host._force_keyframe = True
            await self.broadcast_json({"t": "settings", "settings": self.host.settings.as_dict()})
        elif kind == "log":
            self.events.append({"t": "log", "text": str(obj.get("text", ""))[:400]})
        elif self.on_command:
            await self.on_command(conn, obj)

    def push_event(self, obj: dict) -> None:
        self.events.append(obj)
        if len(self.events) > 200:
            self.events = self.events[-100:]
        self.post_json(obj)


class WebUI:
    def __init__(self, page: str, hub: Hub, host: str = "127.0.0.1", port: int = 0, title: str = "PeerDesk") -> None:
        self.page = page
        self.hub = hub
        self.host = host
        self.port = port
        self.title = title
        self._server: asyncio.AbstractServer | None = None

    async def start(self) -> int:
        self._server = await asyncio.start_server(self._handle, self.host, self.port)
        sockets = self._server.sockets or []
        if sockets:
            self.port = sockets[0].getsockname()[1]
        self.hub.bind_loop()
        log.info("web ui http://%s:%d/", self.host, self.port)
        return self.port

    @property
    def url(self) -> str:
        return f"http://{self.host}:{self.port}/"

    async def close(self) -> None:
        if self._server:
            self._server.close()

    # ----------------------------------------------------------------- http
    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            request = await reader.readuntil(b"\r\n\r\n")
        except Exception:
            writer.close()
            return
        try:
            line = request.split(b"\r\n", 1)[0].decode("latin-1")
            method, target, _ = line.split(" ", 2)
        except Exception:
            writer.close()
            return
        path = target.split("?", 1)[0]

        if path == "/ws" and method == "GET":
            conn = WSConnection(reader, writer, request)
            if not await conn.handshake():
                return
            slot = await self.hub.add(conn)
            try:
                await self.hub.on_connect(slot)
                while True:
                    msg = await conn.recv()
                    if msg is None:
                        break
                    opcode, payload = msg
                    if opcode == 2:
                        await self.hub.on_binary(slot, payload)
                        continue
                    if opcode != 1:
                        continue
                    try:
                        obj = json.loads(payload.decode("utf-8"))
                    except Exception:
                        continue
                    await self.hub.on_json(slot, obj)
            finally:
                self.hub.drop(slot)
            return

        try:
            if path in ("/", "/index.html"):
                body, ctype = self._render(self.page), "text/html; charset=utf-8"
            elif path.startswith("/dl/"):
                body, ctype = self._download(path[len("/dl/") :])
            elif path == "/favicon.ico":
                body, ctype = b"", "image/x-icon"
                writer.write(b"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n")
                await writer.drain()
                writer.close()
                return
            else:
                body, ctype = self._static(path)
            status = "200 OK" if body is not None else "404 Not Found"
            payload = body or b"not found"
            extra = ""
            if path.startswith("/dl/") and body is not None:
                extra = f'Content-Disposition: attachment; filename*=UTF-8\'\'{Path(unquote(path[4:])).name}\r\n'

            writer.write(
                (
                    f"HTTP/1.1 {status}\r\nContent-Type: {ctype}\r\n"
                    f"Content-Length: {len(payload)}\r\nCache-Control: no-store\r\n"
                    f"{extra}Connection: close\r\n\r\n"
                ).encode()
                + payload
            )
            await writer.drain()
        except Exception:
            log.exception("http handler")
        finally:
            try:
                writer.close()
            except Exception:
                pass

    def _render(self, page: str) -> bytes:
        path = WEB_DIR / page
        if not path.exists():
            return b"<h1>PeerDesk</h1><p>web assets missing</p>"
        html = path.read_text(encoding="utf-8")
        html = html.replace("{{TITLE}}", self.title)
        return html.encode("utf-8")

    def _download(self, name: str) -> tuple[bytes | None, str]:
        """把被控端推过来的文件交给浏览器下载（只允许下载目录内的单层文件名）。"""
        safe = Path(unquote(name)).name
        if not safe or safe in (".", ".."):
            return None, "text/plain"
        target = util.download_dir() / "PeerDesk" / safe
        if not target.is_file():
            return None, "text/plain"
        ctype = mimetypes.guess_type(str(target))[0] or "application/octet-stream"
        return target.read_bytes(), ctype

    def _static(self, path: str) -> tuple[bytes | None, str]:
        name = path.lstrip("/").replace("..", "")
        target = WEB_DIR / name
        if not target.is_file():
            return None, "text/plain"
        ctype = mimetypes.guess_type(str(target))[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript", "application/json"):
            ctype += "; charset=utf-8"
        return target.read_bytes(), ctype
