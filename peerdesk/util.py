"""Small shared helpers: connection codes, address parsing, logging, paths."""

from __future__ import annotations

import hashlib
import logging
import os
import random
import sys
from pathlib import Path

APP_DIR_NAME = "peerdesk"


# --------------------------------------------------------------------------- #
# connection codes
# --------------------------------------------------------------------------- #
def generate_code() -> str:
    """9 decimal digits, grouped 3-3-3, first digit non-zero (UU style)."""
    first = random.SystemRandom().randrange(1, 10)
    rest = "".join(str(random.SystemRandom().randrange(0, 10)) for _ in range(8))
    return f"{first}{rest}"


def normalize_code(text: str) -> str:
    digits = "".join(ch for ch in (text or "") if ch.isdigit())
    return digits


def format_code(code: str) -> str:
    code = normalize_code(code)
    return " ".join(code[i : i + 3] for i in range(0, len(code), 3))


def code_is_valid(code: str) -> bool:
    code = normalize_code(code)
    return len(code) == 9 and code[0] != "0"


def psk_from_code(code: str, salt: bytes = b"") -> bytes:
    """Turn the human code into a pre-shared key bound to the code only.

    Both sides hash the same digits, so a signaling server that tries to insert
    itself in the middle cannot complete the AEAD handshake without the code.
    """
    code = normalize_code(code).encode()
    return hashlib.sha256(b"peerdesk-psk" + salt + b"\x00" + code).digest()


def code_id(code: str) -> str:
    """Public lookup id derived from the code (what the server stores)."""
    code = normalize_code(code).encode()
    return hashlib.sha256(b"peerdesk-id" + b"\x00" + code).hexdigest()[:32]


# --------------------------------------------------------------------------- #
# networking helpers
# --------------------------------------------------------------------------- #
def parse_addr(text: str, default_port: int = 0) -> tuple[str, int]:
    text = (text or "").strip()
    if not text:
        return ("127.0.0.1", default_port)
    if text.startswith("["):  # [v6]:port
        host, _, rest = text[1:].partition("]")
        port = rest.lstrip(":")
        return (host, int(port) if port else default_port)
    if text.count(":") == 1:
        host, _, port = text.partition(":")
        return (host, int(port) if port else default_port)
    return (text, default_port)


def format_addr(host: str, port: int) -> str:
    return f"[{host}]:{port}" if ":" in host else f"{host}:{port}"


def local_ipv4s() -> list[str]:
    """Best-effort list of this host's LAN addresses (for host candidates)."""
    import socket

    ips: set[str] = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except Exception:
        pass
    for target in ("10.255.255.255", "223.5.5.5", "8.8.8.8"):
        try:
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            probe.settimeout(0.2)
            probe.connect((target, 53))
            ips.add(probe.getsockname()[0])
            probe.close()
        except Exception:
            pass
    return sorted(ip for ip in ips if not ip.startswith("127.") and ip != "0.0.0.0")


# --------------------------------------------------------------------------- #
# paths / logging
# --------------------------------------------------------------------------- #
def config_dir() -> Path:
    base = os.environ.get("APPDATA") or os.path.expanduser("~/.config")
    path = Path(base) / APP_DIR_NAME
    path.mkdir(parents=True, exist_ok=True)
    return path


def download_dir() -> Path:
    home = Path(os.path.expanduser("~"))
    for name in ("Downloads", "下载"):
        cand = home / name
        if cand.is_dir():
            return cand
    return home


def setup_logging(verbose: bool = False, logfile: str | None = None) -> None:
    level = logging.DEBUG if verbose else logging.INFO
    handlers: list[logging.Handler] = [logging.StreamHandler(sys.stderr)]
    if logfile:
        handlers.append(logging.FileHandler(logfile, encoding="utf-8"))
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)-7s %(name)-16s %(message)s",
        datefmt="%H:%M:%S",
        handlers=handlers,
        force=True,
    )


def human_bytes(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}GB"
