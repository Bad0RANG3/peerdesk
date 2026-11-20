"""Environment self-check:  python -m peerdesk doctor"""

from __future__ import annotations

import platform
import socket
import sys


def _line(ok: bool, label: str, detail: str = "") -> None:
    mark = "OK  " if ok else "FAIL"
    print(f"[{mark}] {label}" + (f" — {detail}" if detail else ""))


def main(argv: list[str] | None = None) -> int:
    print(f"PeerDesk doctor · Python {platform.python_version()} · {platform.platform()}")
    failures = 0

    _line(sys.version_info >= (3, 11), "Python ≥ 3.11", sys.version.split()[0])
    failures += sys.version_info < (3, 11)

    try:
        import PIL

        _line(True, "Pillow", getattr(PIL, "__version__", "?"))
    except Exception as exc:
        _line(False, "Pillow", str(exc))
        failures += 1

    try:
        import numpy

        _line(True, "numpy", numpy.__version__)
    except Exception as exc:
        _line(False, "numpy", str(exc))
        failures += 1

    try:
        from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305  # noqa: F401

        _line(True, "cryptography (ChaCha20-Poly1305)")
    except Exception as exc:
        _line(False, "cryptography", str(exc))
        failures += 1

    try:
        from .capture import ScreenCapture

        cap = ScreenCapture()
        mons = cap.monitors()
        cap.close()
        detail = ", ".join(f"{m.width}x{m.height}" for m in mons)
        _line(bool(mons), f"显示器 {len(mons)} 块", detail)
        failures += 0 if mons else 1
    except Exception as exc:
        _line(False, "屏幕捕获", str(exc))
        failures += 1

    try:
        from .wininput import InputInjector

        inj = InputInjector()
        _line(True, "输入注入 (SendInput)", "虚拟桌面 " + "x".join(str(v) for v in inj._virtual_screen()))
    except Exception as exc:
        _line(False, "输入注入", str(exc))
        failures += 1

    try:
        from . import wire
        from .crypto import CryptoContext, KeyPair, SessionKeys

        a, b = KeyPair.generate(), KeyPair.generate()
        ha, hb = b"hello-a", b"hello-b"
        ka = SessionKeys.derive(a, b.public_bytes, ha, hb)
        kb = SessionKeys.derive(b, a.public_bytes, ha, hb)
        ca, cb = CryptoContext(ka, True), CryptoContext(kb, False)
        hdr = wire.HEADER.pack(wire.MAGIC, wire.VERSION, wire.TYPE_DATA, 1, 0, wire.STREAM_CONTROL, 0, 5, 7)
        sealed = ca.seal(hdr, b"hello", 1)
        opened = cb.open(hdr, sealed, 1)
        _line(opened == b"hello", "PDTP/1 密钥派生 + AEAD 往返")
        failures += 0 if opened == b"hello" else 1
    except Exception as exc:
        _line(False, "PDTP/1 加密自检", str(exc))
        failures += 1

    try:
        from . import stun

        pkt, txid = stun.build_request()
        mtype, txid2, attrs = stun.parse_message(stun.build_response(txid, "203.0.113.7", 41234))
        mapped = stun.parse_mapped(attrs, txid2)
        _line(mapped == ("203.0.113.7", 41234), "STUN 编解码", str(mapped))
        failures += 0 if mapped == ("203.0.113.7", 41234) else 1
    except Exception as exc:
        _line(False, "STUN 编解码", str(exc))
        failures += 1

    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("0.0.0.0", 0))
        port = s.getsockname()[1]
        s.close()
        _line(True, "UDP 绑定", f"临时端口 {port}")
    except Exception as exc:
        _line(False, "UDP 绑定", str(exc))
        failures += 1

    from . import util

    addrs = util.local_ipv4s()
    _line(bool(addrs), "本机 IPv4", ", ".join(addrs) or "未检测到")

    try:
        from .websocket import WSConnection  # noqa: F401

        _line(True, "WebSocket 实现")
    except Exception as exc:
        _line(False, "WebSocket 实现", str(exc))
        failures += 1

    print()
    if failures:
        print(f"存在 {failures} 项问题，先修掉再联机。")
    else:
        print("全部检查通过，可以直接 `python -m peerdesk server` 起服务。")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
