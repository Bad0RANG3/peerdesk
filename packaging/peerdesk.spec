# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller 打包描述（单文件 exe）。

用 ``python packaging/build.py`` 调用；直接跑
``pyinstaller packaging/peerdesk.spec`` 也行，但产物路径不会自动打印。

要点：
- 静态前端（peerdesk/web/*）必须按 ``peerdesk/web`` 的相对路径塞进包里，
  因为 ``webui.WEB_DIR = Path(__file__).parent / "web"``。
- mss 的 DLL 和 cryptography 的 Rust 扩展都由 PyInstaller 自带的 hook 处理，
  hiddenimports 只补那些"运行时按名字导入"的模块（PIL 的图像插件、windows 后端）。
"""

from pathlib import Path

from PyInstaller.utils.hooks import collect_submodules

ROOT = Path(SPECPATH).resolve().parent
PKG = ROOT / "peerdesk"

datas = [
    (str(PKG / "web" / "*.html"), "peerdesk/web"),
    (str(PKG / "web" / "*.js"), "peerdesk/web"),
    (str(PKG / "web" / "*.css"), "peerdesk/web"),
]

hiddenimports = [
    "mss.windows",
    "PIL._tkinter_finder",
    "PIL.Image",
    "PIL.ImageFile",
    "PIL.ImageQt",
    "peerdesk.agent",
    "peerdesk.viewer",
    "peerdesk.server",
    "peerdesk.doctor",
] + collect_submodules("PIL")

a = Analysis(
    [str(ROOT / "packaging" / "entry.py")],
    pathex=[str(ROOT)],
    binaries=[],
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["tkinter", "unittest", "pydoc_data", "test", "email.test"],
    noarchive=False,
    optimize=1,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="peerdesk",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=None,
)
