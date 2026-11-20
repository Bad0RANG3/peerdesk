"""Signaling client: JSON lines over TCP, plus candidate publishing."""

from __future__ import annotations

import asyncio
import json
import logging
from collections.abc import Callable

from . import util
from .transport import PeerLink

log = logging.getLogger("peerdesk.signal")


class SignalingClient:
    def __init__(self, host: str, port: int, on_message: Callable[[dict], None]) -> None:
        self.host = host
        self.port = port
        self.on_message = on_message
        self.reader: asyncio.StreamReader | None = None
        self.writer: asyncio.StreamWriter | None = None
        self._task: asyncio.Task | None = None
        self.connected = False
        self.last_error = ""

    async def connect(self, timeout: float = 8.0) -> bool:
        try:
            self.reader, self.writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port), timeout
            )
        except (TimeoutError, OSError) as exc:
            self.last_error = f"{type(exc).__name__}: {exc}"
            log.warning("signal connect failed %s", self.last_error)
            return False
        self.connected = True
        self._task = asyncio.get_event_loop().create_task(self._read_loop())
        log.info("signal connected %s:%d", self.host, self.port)
        return True

    async def _read_loop(self) -> None:
        assert self.reader
        try:
            while True:
                line = await self.reader.readline()
                if not line:
                    break
                try:
                    msg = json.loads(line.decode("utf-8"))
                except Exception:
                    continue
                try:
                    self.on_message(msg)
                except Exception:
                    log.exception("signal handler")
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            self.connected = False
            log.info("signal closed")

    async def send(self, obj: dict) -> None:
        if not self.writer or self.writer.is_closing():
            return
        try:
            self.writer.write((json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n").encode())
            await self.writer.drain()
        except (ConnectionError, RuntimeError):
            self.connected = False

    async def close(self) -> None:
        if self._task:
            self._task.cancel()
        if self.writer:
            try:
                self.writer.close()
            except Exception:
                pass
        self.connected = False


def cands_of(link: PeerLink) -> list[dict]:
    return [c.as_dict() for c in link.candidate_list()]


def parse_server(text: str, default_port: int = 7900) -> tuple[str, int]:
    host, port = util.parse_addr(text, default_port)
    return host, port
