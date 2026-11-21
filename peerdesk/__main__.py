"""Unified entry point:  python -m peerdesk {agent,viewer,server,doctor}"""

from __future__ import annotations

import sys


def _force_utf8_console() -> None:
    """Windows 控制台默认 GBK，中文帮助会变成乱码；能改就改成 UTF-8。"""
    for stream in (sys.stdout, sys.stderr):
        enc = (getattr(stream, "encoding", "") or "").lower()
        if enc.replace("-", "").startswith("utf8"):
            continue
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[union-attr]
        except Exception:
            pass


def main(argv: list[str] | None = None) -> int:
    _force_utf8_console()
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(
            "PeerDesk — 开源远程桌面（PDTP/1）\n\n"
            "用法:\n"
            "  python -m peerdesk agent  --server HOST:PORT [--code 123456789]\n"
            "  python -m peerdesk viewer --server HOST:PORT [--code 123456789]\n"
            "  python -m peerdesk server [--port 7900] [--udp-port 7901]\n"
            "  python -m peerdesk doctor\n"
        )
        return 0
    cmd, rest = argv[0], argv[1:]
    if cmd == "agent":
        from .agent import main as run

        return run(rest)
    if cmd == "viewer":
        from .viewer import main as run

        return run(rest)
    if cmd == "server":
        from .server import main as run

        return run(rest)
    if cmd == "doctor":
        from .doctor import main as run

        return run(rest)
    print(f"未知子命令: {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
