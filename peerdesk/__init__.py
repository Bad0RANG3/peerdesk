"""PeerDesk — 开源的远程桌面，等价替代网易UU远程的 P2P 收费玩法。

自研 PDTP/1 传输：UDP + X25519/ChaCha20-Poly1305、STUN 打洞、中继回退、
控制流可靠重传、视频分片 XOR FEC + NACK。被控端抓屏编码为 JPEG，控制端在
浏览器 canvas 内解码；键鼠/剪贴板/文件全部经加密通道回传。
"""

__version__ = "0.1.0"
__all__ = ["__version__"]
