"""Windows input injection + clipboard bridge (ctypes only, no pywin32)."""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import logging
import time

log = logging.getLogger("peerdesk.input")

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

ULONG_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong

INPUT_MOUSE = 0
INPUT_KEYBOARD = 1

MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_MIDDLEDOWN = 0x0020
MOUSEEVENTF_MIDDLEUP = 0x0040
MOUSEEVENTF_XDOWN = 0x0080
MOUSEEVENTF_XUP = 0x0100
MOUSEEVENTF_WHEEL = 0x0800
MOUSEEVENTF_HWHEEL = 0x1000
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_VIRTUALDESK = 0x4000

KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004
KEYEVENTF_SCANCODE = 0x0008

XBUT1 = 0x0001
XBUT2 = 0x0002

CF_UNICODETEXT = 13
GMEM_MOVEABLE = 0x0042

SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN, SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN = 76, 77, 78, 79


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wt.LONG),
        ("dy", wt.LONG),
        ("mouseData", wt.DWORD),
        ("dwFlags", wt.DWORD),
        ("time", wt.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wt.WORD),
        ("wScan", wt.WORD),
        ("dwFlags", wt.DWORD),
        ("time", wt.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", wt.DWORD), ("wParamL", wt.WORD), ("wParamH", wt.WORD)]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _INPUTUNION)]


user32.SendInput.argtypes = (wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
user32.SendInput.restype = wt.UINT

# JS KeyboardEvent.code -> Windows virtual key
CODE_TO_VK: dict[str, int] = {
    "Backspace": 0x08,
    "Tab": 0x09,
    "Enter": 0x0D,
    "NumpadEnter": 0x0D,
    "ShiftLeft": 0xA0,
    "ShiftRight": 0xA1,
    "ControlLeft": 0xA2,
    "ControlRight": 0xA3,
    "AltLeft": 0xA4,
    "AltRight": 0xA5,
    "MetaLeft": 0x5B,
    "MetaRight": 0x5C,
    "CapsLock": 0x14,
    "Escape": 0x1B,
    "Space": 0x20,
    "PageUp": 0x21,
    "PageDown": 0x22,
    "End": 0x23,
    "Home": 0x24,
    "ArrowLeft": 0x25,
    "ArrowUp": 0x26,
    "ArrowRight": 0x27,
    "ArrowDown": 0x28,
    "PrintScreen": 0x2C,
    "Insert": 0x2D,
    "Delete": 0x2E,
    "ContextMenu": 0x5D,
    "NumLock": 0x90,
    "ScrollLock": 0x91,
    "Pause": 0x13,
    "Semicolon": 0xBA,
    "Equal": 0xBB,
    "Comma": 0xBC,
    "Minus": 0xBD,
    "Period": 0xBE,
    "Slash": 0xBF,
    "Backquote": 0xC0,
    "BracketLeft": 0xDB,
    "Backslash": 0xDC,
    "BracketRight": 0xDD,
    "Quote": 0xDE,
    "NumpadMultiply": 0x6A,
    "NumpadAdd": 0x6B,
    "NumpadSubtract": 0x6D,
    "NumpadDecimal": 0x6E,
    "NumpadDivide": 0x6F,
}
for _i in range(10):
    CODE_TO_VK[f"Digit{_i}"] = 0x30 + _i
    CODE_TO_VK[f"Numpad{_i}"] = 0x60 + _i
for _i in range(26):
    CODE_TO_VK[f"Key{chr(65 + _i)}"] = 0x41 + _i
for _i in range(1, 25):
    CODE_TO_VK[f"F{_i}"] = 0x6F + _i

EXTENDED_KEYS = {
    0x25, 0x26, 0x27, 0x28, 0x21, 0x22, 0x23, 0x24, 0x2D, 0x2E, 0x5B, 0x5C, 0x5D,
    0x6A, 0x6F, 0x2C, 0x90, 0xA3, 0xA5,
}


class InputInjector:
    """Injects mouse/keyboard events into the local desktop session."""

    def __init__(self) -> None:
        self.screen = self._virtual_screen()

    @staticmethod
    def _virtual_screen() -> tuple[int, int, int, int]:
        g = user32.GetSystemMetrics
        return (g(SM_XVIRTUALSCREEN), g(SM_YVIRTUALSCREEN), g(SM_CXVIRTUALSCREEN), g(SM_CYVIRTUALSCREEN))

    def _send(self, *inputs: INPUT) -> None:
        arr = (INPUT * len(inputs))(*inputs)
        user32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT))

    # ------------------------------------------------------------------ mouse
    @staticmethod
    def _mouse(dx: int, dy: int, data: int, flags: int) -> INPUT:
        item = INPUT()
        item.type = INPUT_MOUSE
        item.mi = MOUSEINPUT(dx, dy, data & 0xFFFFFFFF, flags, 0, 0)
        return item

    def move_abs(self, nx: float, ny: float) -> None:
        """Move to normalized (0..1) coordinates of the virtual desktop."""
        vx, vy, vw, vh = self.screen
        x = int(round(vx + nx * max(1, vw - 1)))
        y = int(round(vy + ny * max(1, vh - 1)))
        ax = int(round(x * 65535 / max(1, vw - 1))) if vw > 1 else 0
        ay = int(round(y * 65535 / max(1, vh - 1))) if vh > 1 else 0
        self._send(self._mouse(ax, ay, 0, MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK))

    def move_rel(self, dx: int, dy: int) -> None:
        self._send(self._mouse(dx, dy, 0, MOUSEEVENTF_MOVE))

    def button(self, name: str, down: bool) -> None:
        table = {
            "left": (MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0),
            "right": (MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0),
            "middle": (MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0),
            "back": (MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUT1),
            "forward": (MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUT2),
        }
        flag = table.get(name, table["left"])[0 if down else 1]
        data = table.get(name, table["left"])[2]
        self._send(self._mouse(0, 0, data, flag))

    def click(self, name: str = "left") -> None:
        self.button(name, True)
        self.button(name, False)

    def wheel(self, delta: int, horizontal: bool = False) -> None:
        self._send(self._mouse(0, 0, delta & 0xFFFFFFFF, MOUSEEVENTF_HWHEEL if horizontal else MOUSEEVENTF_WHEEL))

    # --------------------------------------------------------------- keyboard
    @staticmethod
    def _key(vk: int, down: bool, extended: bool | None = None) -> INPUT:
        item = INPUT()
        item.type = INPUT_KEYBOARD
        scan = user32.MapVirtualKeyW(vk, 0) & 0xFF
        flags = 0 if down else KEYEVENTF_KEYUP
        if extended is None:
            extended = vk in EXTENDED_KEYS
        if extended:
            flags |= KEYEVENTF_EXTENDEDKEY
        item.ki = KEYBDINPUT(vk, scan, flags, 0, 0)
        return item

    def key(self, code: str, down: bool) -> bool:
        vk = CODE_TO_VK.get(code)
        if vk is None and len(code) == 1:
            vk = user32.VkKeyScanW(ord(code)) & 0xFF
        if not vk:
            return False
        self._send(self._key(vk, down))
        return True

    def key_vk(self, vk: int, down: bool) -> None:
        self._send(self._key(vk, down))

    def chord(self, *vks: int) -> None:
        for vk in vks:
            self._send(self._key(vk, True))
        for vk in reversed(vks):
            self._send(self._key(vk, False))

    def type_text(self, text: str) -> None:
        """Send arbitrary unicode text (works for CJK, bypasses layouts)."""
        for ch in text:
            code = ord(ch)
            if code > 0xFFFF:
                code -= 0x10000
                units = [0xD800 + (code >> 10), 0xDC00 + (code & 0x3FF)]
            else:
                units = [code]
            for unit in units:
                down = INPUT()
                down.type = INPUT_KEYBOARD
                down.ki = KEYBDINPUT(0, unit, KEYEVENTF_UNICODE, 0, 0)
                up = INPUT()
                up.type = INPUT_KEYBOARD
                up.ki = KEYBDINPUT(0, unit, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP, 0, 0)
                self._send(down, up)

    def ctrl_alt_del(self) -> None:
        self.chord(0xA2, 0xA4, 0x2E)


# --------------------------------------------------------------------------- #
# clipboard
# --------------------------------------------------------------------------- #
def _open_clipboard(retries: int = 12) -> bool:
    for _ in range(retries):
        if user32.OpenClipboard(None):
            return True
        time.sleep(0.01)
    return False


def get_clipboard_text() -> str:
    if not user32.IsClipboardFormatAvailable(CF_UNICODETEXT):
        return ""
    if not _open_clipboard():
        return ""
    try:
        handle = user32.GetClipboardData(CF_UNICODETEXT)
        if not handle:
            return ""
        ptr = kernel32.GlobalLock(handle)
        if not ptr:
            return ""
        try:
            return ctypes.wstring_at(ptr)
        finally:
            kernel32.GlobalUnlock(handle)
    finally:
        user32.CloseClipboard()


def set_clipboard_text(text: str) -> bool:
    if not _open_clipboard():
        return False
    try:
        user32.EmptyClipboard()
        size = (len(text) + 1) * ctypes.sizeof(ctypes.c_wchar)
        handle = kernel32.GlobalAlloc(GMEM_MOVEABLE, size)
        if not handle:
            return False
        ptr = kernel32.GlobalLock(handle)
        if not ptr:
            return False
        ctypes.memmove(ptr, ctypes.create_unicode_buffer(text), size)
        kernel32.GlobalUnlock(handle)
        return bool(user32.SetClipboardData(CF_UNICODETEXT, handle))
    finally:
        user32.CloseClipboard()


def clipboard_sequence() -> int:
    return int(user32.GetClipboardSequenceNumber())


class ClipboardWatcher:
    """Polls the clipboard sequence number; reports text changes."""

    def __init__(self, interval: float = 0.4) -> None:
        self.interval = interval
        self._last = clipboard_sequence()
        self._last_text = get_clipboard_text()

    def poll(self) -> str | None:
        seq = clipboard_sequence()
        if seq == self._last:
            return None
        self._last = seq
        text = get_clipboard_text()
        if text == self._last_text:
            return None
        self._last_text = text
        return text
