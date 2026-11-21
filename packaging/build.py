"""打包成单文件 exe：``python packaging/build.py [--clean]``。

产物：``dist/peerdesk.exe``（约 20–25 MB，首次运行解压到临时目录约 1 秒）。

不做的事：签名、UPX 压缩、图标。需要签名请自行接 ``signtool``；
UPX 会拖慢启动（每次解压），所以 spec 里显式关了。
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist" / "peerdesk.exe"
BUILD = ROOT / "build"


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--clean" in argv:
        for path in (BUILD, ROOT / "dist"):
            if path.exists():
                shutil.rmtree(path, ignore_errors=True)
                print(f"删除 {path}")

    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", str(ROOT / "packaging" / "peerdesk.spec")]
    print("执行:", " ".join(cmd))
    proc = subprocess.run(cmd, cwd=ROOT)
    if proc.returncode != 0:
        print("打包失败", file=sys.stderr)
        return proc.returncode

    if not DIST.exists():
        print(f"没找到产物 {DIST}", file=sys.stderr)
        return 1
    size = DIST.stat().st_size / 1024 / 1024
    print(f"\n产物: {DIST}  ({size:.1f} MB)")
    print(f"试运行: {DIST} --help")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
