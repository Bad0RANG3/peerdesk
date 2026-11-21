"""PyInstaller 入口：把 ``python -m peerdesk`` 变成单个 exe。

打包后 ``peerdesk.exe agent --server ...`` 与 ``python -m peerdesk agent --server ...``
行为一致；不传子命令时打印用法（退出码 0）。
"""

from __future__ import annotations

import sys

from peerdesk.__main__ import main

if __name__ == "__main__":
    sys.exit(main())
