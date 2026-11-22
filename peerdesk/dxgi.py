"""DXGI Desktop Duplication capture backend.

This module deliberately keeps the COM layer small and dependency-free.  It
loads ``d3d11.dll``/``dxgi.dll`` lazily and calls the interfaces through their
vtables.  A bad vtable index in this file can kill the process, so use
``probe_dxgi()`` (which runs the full path in a child process) before creating
a :class:`DxgiCapture` in the agent process.

Important ABI details:

* ``IDXGIOutput1::DuplicateOutput`` is vtable slot **22**.
* ``IDXGIOutputDuplication`` inherits ``IDXGIObject``, so ``GetDesc`` is slot 7,
  ``AcquireNextFrame`` is 8 and ``ReleaseFrame`` is 14.
* ``DXGI_OUTDUPL_DESC.ModeDesc.RefreshRate`` is a ``DXGI_RATIONAL`` (two
  ``UINT`` values), not one ``UINT``, and ``DXGI_MODE_DESC`` ends with
  ``Scaling`` after ``ScanlineOrdering``.
"""

from __future__ import annotations

import ctypes
import json
import os
import subprocess
import sys
import threading
import time
import uuid
from ctypes import wintypes as wt
from dataclasses import dataclass, field

if sys.platform == "win32":
    from .capture import Monitor
else:  # pragma: no cover - PeerDesk is Windows-only, but keep imports testable.
    Monitor = object  # type: ignore[assignment,misc]

HRESULT = ctypes.c_long
_PTR = ctypes.c_void_p
_WINFUNCTYPE = getattr(ctypes, "WINFUNCTYPE", ctypes.CFUNCTYPE)

_D3D11_SDK_VERSION = 7
_D3D_DRIVER_TYPE_HARDWARE = 1
_D3D11_USAGE_STAGING = 3
_D3D11_CPU_ACCESS_READ = 0x20000
_D3D11_MAP_READ = 1

_DXGI_FORMAT_B8G8R8A8_UNORM = 87
_DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91
_DXGI_MODE_ROTATION_UNSPECIFIED = 0
_DXGI_MODE_ROTATION_IDENTITY = 1

_DXGI_ERROR_NOT_FOUND = 0x887A0002
_DXGI_ERROR_ACCESS_LOST = 0x887A0026
_DXGI_ERROR_WAIT_TIMEOUT = 0x887A0027

_IID_IDXGIDEVICE = "{54ec77fa-1377-44e6-8c32-88fd5f44c84c}"
_IID_IDXGI_OUTPUT1 = "{00cddea8-939b-4b83-a340-a685226666cc}"
_IID_ID3D11_TEXTURE2D = "{6f15aaf2-d208-4e89-9ab4-489535d34f9c}"

_PROBE_TIMEOUT = 10.0
_PROBE_COMMAND = "_dxgi_probe"
_CREATE_NO_WINDOW = 0x08000000

_probe_lock = threading.Lock()
_probe_result: dict | None = None


class DxgiError(RuntimeError):
    """DXGI could not initialize or capture a frame."""


class _DxgiAccessLost(DxgiError):
    pass


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", ctypes.c_ulong),
        ("Data2", ctypes.c_ushort),
        ("Data3", ctypes.c_ushort),
        ("Data4", ctypes.c_ubyte * 8),
    ]


def _guid(text: str) -> GUID:
    raw = uuid.UUID(text.strip("{}")).bytes_le
    return GUID(
        Data1=int.from_bytes(raw[0:4], "little"),
        Data2=int.from_bytes(raw[4:6], "little"),
        Data3=int.from_bytes(raw[6:8], "little"),
        Data4=(ctypes.c_ubyte * 8)(*raw[8:]),
    )


_GUID_IDXGIDEVICE = _guid(_IID_IDXGIDEVICE)
_GUID_IDXGI_OUTPUT1 = _guid(_IID_IDXGI_OUTPUT1)
_GUID_ID3D11_TEXTURE2D = _guid(_IID_ID3D11_TEXTURE2D)


class _RECT(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_long),
        ("top", ctypes.c_long),
        ("right", ctypes.c_long),
        ("bottom", ctypes.c_long),
    ]


class _DXGI_RATIONAL(ctypes.Structure):
    _fields_ = [
        ("Numerator", ctypes.c_uint),
        ("Denominator", ctypes.c_uint),
    ]


class _DXGI_MODE_DESC(ctypes.Structure):
    _fields_ = [
        ("Width", ctypes.c_uint),
        ("Height", ctypes.c_uint),
        ("RefreshRate", _DXGI_RATIONAL),
        ("Format", ctypes.c_uint),
        ("ScanlineOrdering", ctypes.c_uint),
        ("Scaling", ctypes.c_uint),
    ]


class _DXGI_OUTDUPL_DESC(ctypes.Structure):
    _fields_ = [
        ("ModeDesc", _DXGI_MODE_DESC),
        ("Rotation", ctypes.c_uint),
        ("DesktopImageInSystemMemory", wt.BOOL),
    ]


class _DXGI_OUTPUT_DESC(ctypes.Structure):
    _fields_ = [
        ("DeviceName", ctypes.c_wchar * 32),
        ("DesktopCoordinates", _RECT),
        ("AttachedToDesktop", wt.BOOL),
        ("Rotation", ctypes.c_uint),
        ("Monitor", wt.HMONITOR),
    ]


class _D3D11_SAMPLE_DESC(ctypes.Structure):
    _fields_ = [
        ("Count", ctypes.c_uint),
        ("Quality", ctypes.c_uint),
    ]


class _D3D11_TEXTURE2D_DESC(ctypes.Structure):
    _fields_ = [
        ("Width", ctypes.c_uint),
        ("Height", ctypes.c_uint),
        ("MipLevels", ctypes.c_uint),
        ("ArraySize", ctypes.c_uint),
        ("Format", ctypes.c_uint),
        ("SampleDesc", _D3D11_SAMPLE_DESC),
        ("Usage", ctypes.c_uint),
        ("BindFlags", ctypes.c_uint),
        ("CPUAccessFlags", ctypes.c_uint),
        ("MiscFlags", ctypes.c_uint),
    ]


class _D3D11_MAPPED_SUBRESOURCE(ctypes.Structure):
    _fields_ = [
        ("pData", _PTR),
        ("RowPitch", ctypes.c_uint),
        ("DepthPitch", ctypes.c_uint),
    ]


class _DXGI_OUTDUPL_POINTER_POSITION(ctypes.Structure):
    _fields_ = [
        ("Position", wt.POINT),
        ("Visible", wt.BOOL),
    ]


class _DXGI_OUTDUPL_FRAME_INFO(ctypes.Structure):
    _fields_ = [
        ("LastPresentTime", ctypes.c_longlong),
        ("LastMouseUpdateTime", ctypes.c_longlong),
        ("AccumulatedFrames", ctypes.c_uint),
        ("RectsCoalesced", wt.BOOL),
        ("ProtectedContentMaskedOut", wt.BOOL),
        ("PointerPosition", _DXGI_OUTDUPL_POINTER_POSITION),
        ("TotalMetadataBufferSize", ctypes.c_uint),
        ("PointerShapeBufferSize", ctypes.c_uint),
    ]


class _MONITORINFO(ctypes.Structure):
    _fields_ = [
        ("cbSize", ctypes.c_ulong),
        ("rcMonitor", _RECT),
        ("rcWork", _RECT),
        ("dwFlags", ctypes.c_ulong),
    ]


_d3d11 = None
_dxgi = None


def _load_libraries() -> None:
    global _d3d11, _dxgi
    if os.name != "nt":
        raise DxgiError("DXGI is only available on Windows")
    if _d3d11 is not None:
        return
    try:
        d3d11 = ctypes.WinDLL("d3d11")
        dxgi = ctypes.WinDLL("dxgi")
    except OSError as exc:
        raise DxgiError(f"cannot load d3d11/dxgi: {exc}") from exc
    d3d11.D3D11CreateDevice.argtypes = [
        _PTR,
        ctypes.c_uint,
        _PTR,
        ctypes.c_uint,
        _PTR,
        ctypes.c_uint,
        ctypes.c_uint,
        ctypes.POINTER(_PTR),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(_PTR),
    ]
    d3d11.D3D11CreateDevice.restype = HRESULT
    _d3d11 = d3d11
    _dxgi = dxgi


def _vtable_method(ptr, index: int, restype=HRESULT, *argtypes):
    address = ctypes.cast(ptr, ctypes.POINTER(ctypes.POINTER(_PTR))).contents[index]
    return _WINFUNCTYPE(restype, _PTR, *argtypes)(address)


def _void_method(ptr, index: int, *argtypes):
    return _vtable_method(ptr, index, None, *argtypes)


def _check(hr, what: str) -> None:
    if hr < 0:
        raise DxgiError(f"{what} failed: 0x{hr & 0xFFFFFFFF:08X}")


def _is(hr, code: int) -> bool:
    return (hr & 0xFFFFFFFF) == code


def _release(ptr) -> None:
    if not ptr:
        return
    try:
        _vtable_method(ptr, 2, ctypes.c_ulong)(ptr)
    except Exception:
        pass


def _qi(ptr, iid: GUID) -> _PTR:
    out = _PTR()
    hr = _vtable_method(ptr, 0, HRESULT, ctypes.POINTER(GUID), ctypes.POINTER(_PTR))(
        ptr, ctypes.byref(iid), ctypes.byref(out)
    )
    _check(hr, "QueryInterface")
    if not out:
        raise DxgiError("QueryInterface returned a null interface")
    return out


def _primary_monitor(hmonitor) -> bool:
    if not hmonitor:
        return False
    try:
        prototype = _WINFUNCTYPE(
            wt.BOOL, wt.HMONITOR, ctypes.POINTER(_MONITORINFO)
        )
        get_info = prototype(ctypes.windll.user32.GetMonitorInfoW)
        info = _MONITORINFO()
        info.cbSize = ctypes.sizeof(_MONITORINFO)
        if get_info(hmonitor, ctypes.byref(info)):
            return bool(info.dwFlags & 1)
    except Exception:
        pass
    return False


@dataclass
class _Output:
    index: int
    monitor: Monitor
    pointer: _PTR
    rotation: int
    width: int
    height: int
    args: tuple = field(default_factory=tuple, repr=False)


def _pack_rows(raw: bytes, row_pitch: int, width: int, height: int) -> bytes:
    """Drop the GPU row padding when ``RowPitch != width * 4``."""
    stride = width * 4
    if row_pitch == stride:
        return raw
    return b"".join(
        raw[row * row_pitch : row * row_pitch + stride] for row in range(height)
    )


class DxgiCapture:
    """One D3D11 device, with a lazily-created duplication per active output."""

    def __init__(self) -> None:
        _load_libraries()
        self._device = _PTR()
        self._context = _PTR()
        self._outputs: list[_Output] = []
        self._monitors: list[Monitor] = []
        self._dup = _PTR()
        self._staging = _PTR()
        self._active = None
        self._width = 0
        self._height = 0
        self._format = 0
        self._last_frame: tuple[bytes, int, int] | None = None
        try:
            self._create_device()
            self._enumerate_outputs()
        except Exception:
            self.close()
            raise

    # ------------------------------------------------------------------ setup
    def _create_device(self) -> None:
        feature = ctypes.c_int()
        hr = _d3d11.D3D11CreateDevice(
            None,
            _D3D_DRIVER_TYPE_HARDWARE,
            None,
            0,
            None,
            0,
            _D3D11_SDK_VERSION,
            ctypes.byref(self._device),
            ctypes.byref(feature),
            ctypes.byref(self._context),
        )
        _check(hr, "D3D11CreateDevice")
        if not self._device or not self._context:
            raise DxgiError("D3D11CreateDevice returned a null device")

    def _enumerate_outputs(self) -> None:
        dxgi_device = _qi(self._device, _GUID_IDXGIDEVICE)
        adapter = _PTR()
        raw_outputs = []
        transferred = False
        try:
            hr = _vtable_method(dxgi_device, 7, HRESULT, ctypes.POINTER(_PTR))(
                dxgi_device, ctypes.byref(adapter)
            )
            _check(hr, "IDXGIDevice::GetAdapter")
            if not adapter:
                raise DxgiError("IDXGIDevice::GetAdapter returned null")
            while True:
                output = _PTR()
                hr = _vtable_method(
                    adapter, 7, HRESULT, ctypes.c_uint, ctypes.POINTER(_PTR)
                )(adapter, len(raw_outputs), ctypes.byref(output))
                if _is(hr, _DXGI_ERROR_NOT_FOUND):
                    break
                _check(hr, f"IDXGIAdapter::EnumOutputs({len(raw_outputs)})")
                if not output:
                    break
                desc = _DXGI_OUTPUT_DESC()
                hr = _vtable_method(output, 7, HRESULT, ctypes.POINTER(_DXGI_OUTPUT_DESC))(
                    output, ctypes.byref(desc)
                )
                if hr < 0 or not desc.AttachedToDesktop:
                    _release(output)
                else:
                    rect = desc.DesktopCoordinates
                    raw_outputs.append(
                        (
                            int(rect.left),
                            int(rect.top),
                            int(rect.right - rect.left),
                            int(rect.bottom - rect.top),
                            int(desc.Rotation),
                            bool(_primary_monitor(desc.Monitor)),
                            output,
                        )
                    )
            raw_outputs.sort(key=lambda item: (item[0], item[1]))
            self._outputs = []
            self._monitors = []
            for index, (left, top, width, height, rotation, primary, output) in enumerate(raw_outputs):
                monitor = Monitor(
                    index=index,
                    left=left,
                    top=top,
                    width=width,
                    height=height,
                    primary=primary,
                )
                self._monitors.append(monitor)
                self._outputs.append(
                    _Output(
                        index=index,
                        monitor=monitor,
                        pointer=output,
                        rotation=rotation,
                        width=width,
                        height=height,
                    )
                )
            if not self._outputs:
                raise DxgiError("no attached DXGI outputs found")
            transferred = True
        finally:
            _release(adapter)
            _release(dxgi_device)
            if not transferred:
                for item in raw_outputs:
                    _release(item[6])

    # --------------------------------------------------------------- monitors
    def monitors(self) -> list[Monitor]:
        return list(self._monitors)

    def _find_output(self, mon: Monitor) -> _Output:
        for output in self._outputs:
            if output.monitor.index == mon.index and (
                output.monitor.left,
                output.monitor.top,
                output.monitor.width,
                output.monitor.height,
            ) == (mon.left, mon.top, mon.width, mon.height):
                return output
        for output in self._outputs:
            if (
                output.monitor.left,
                output.monitor.top,
                output.monitor.width,
                output.monitor.height,
            ) == (mon.left, mon.top, mon.width, mon.height):
                return output
        raise DxgiError(f"DXGI output not found for monitor {mon.index}")

    # -------------------------------------------------------------- duplication
    def _release_duplication(self) -> None:
        if self._dup:
            _release(self._dup)
        self._dup = _PTR()
        if self._staging:
            _release(self._staging)
        self._staging = _PTR()
        self._active = None
        self._width = 0
        self._height = 0
        self._format = 0
        self._last_frame = None

    def _ensure_duplication(self, mon: Monitor) -> None:
        if self._active is not None and self._active.monitor == mon:
            return
        output = self._find_output(mon)
        if output.rotation not in (_DXGI_MODE_ROTATION_UNSPECIFIED, _DXGI_MODE_ROTATION_IDENTITY):
            raise DxgiError(f"rotated output ({output.rotation}) is not supported by DXGI backend")
        self._release_duplication()

        output1 = _qi(output.pointer, _GUID_IDXGI_OUTPUT1)
        duplicate = _PTR()
        try:
            hr = _vtable_method(
                output1, 22, HRESULT, _PTR, ctypes.POINTER(_PTR)
            )(output1, self._device, ctypes.byref(duplicate))
            _check(hr, "IDXGIOutput1::DuplicateOutput")
        finally:
            _release(output1)
        if not duplicate:
            raise DxgiError("DuplicateOutput returned a null interface")

        desc = _DXGI_OUTDUPL_DESC()
        _void_method(duplicate, 7, ctypes.POINTER(_DXGI_OUTDUPL_DESC))(
            duplicate, ctypes.byref(desc)
        )
        width = int(desc.ModeDesc.Width)
        height = int(desc.ModeDesc.Height)
        fmt = int(desc.ModeDesc.Format)
        if width <= 0 or height <= 0:
            _release(duplicate)
            raise DxgiError(f"invalid duplication size {width}x{height}")
        if fmt not in (_DXGI_FORMAT_B8G8R8A8_UNORM, _DXGI_FORMAT_B8G8R8A8_UNORM_SRGB):
            _release(duplicate)
            raise DxgiError(f"unsupported duplication format {fmt}")

        staging = _PTR()
        texture_desc = _D3D11_TEXTURE2D_DESC()
        texture_desc.Width = width
        texture_desc.Height = height
        texture_desc.MipLevels = 1
        texture_desc.ArraySize = 1
        texture_desc.Format = fmt
        texture_desc.SampleDesc.Count = 1
        texture_desc.SampleDesc.Quality = 0
        texture_desc.Usage = _D3D11_USAGE_STAGING
        texture_desc.BindFlags = 0
        texture_desc.CPUAccessFlags = _D3D11_CPU_ACCESS_READ
        texture_desc.MiscFlags = 0
        hr = _vtable_method(
            self._device,
            5,
            HRESULT,
            ctypes.POINTER(_D3D11_TEXTURE2D_DESC),
            _PTR,
            ctypes.POINTER(_PTR),
        )(self._device, ctypes.byref(texture_desc), None, ctypes.byref(staging))
        if hr < 0 or not staging:
            _release(duplicate)
            _check(hr, "ID3D11Device::CreateTexture2D")
            raise DxgiError("CreateTexture2D returned a null texture")

        self._dup = duplicate
        self._staging = staging
        self._active = output
        self._width = width
        self._height = height
        self._format = fmt
        self._last_frame = None

    # ------------------------------------------------------------------- frames
    def grab_rgb(self, mon: Monitor) -> tuple[bytes, int, int]:
        """Return a full-size top-down BGRA frame for ``mon``."""
        for attempt in range(2):
            self._ensure_duplication(mon)
            try:
                frame = self._acquire(mon)
                if frame is not None:
                    return frame
                if self._last_frame is not None:
                    return self._last_frame
                if attempt:
                    raise DxgiError("AcquireNextFrame timed out before the first frame")
            except _DxgiAccessLost:
                if attempt:
                    raise
                self._release_duplication()
                continue
        raise DxgiError("DXGI capture failed")

    def _acquire(self, mon: Monitor) -> tuple[bytes, int, int] | None:
        timeout_ms = 0 if self._last_frame is not None else 100
        info = _DXGI_OUTDUPL_FRAME_INFO()
        resource = _PTR()
        hr = _vtable_method(
            self._dup,
            8,
            HRESULT,
            ctypes.c_uint,
            ctypes.POINTER(_DXGI_OUTDUPL_FRAME_INFO),
            ctypes.POINTER(_PTR),
        )(self._dup, timeout_ms, ctypes.byref(info), ctypes.byref(resource))
        if _is(hr, _DXGI_ERROR_WAIT_TIMEOUT):
            return None
        if _is(hr, _DXGI_ERROR_ACCESS_LOST):
            raise _DxgiAccessLost("desktop duplication access lost")
        _check(hr, "IDXGIOutputDuplication::AcquireNextFrame")
        if not resource:
            raise DxgiError("AcquireNextFrame returned a null resource")

        fresh = bool(info.AccumulatedFrames) or self._last_frame is None
        try:
            if fresh:
                texture = _qi(resource, _GUID_ID3D11_TEXTURE2D)
                try:
                    _void_method(self._context, 47, _PTR, _PTR)(
                        self._context, self._staging, texture
                    )
                finally:
                    _release(texture)
        finally:
            _release(resource)
            hr = _vtable_method(self._dup, 14, HRESULT)(self._dup)
            if _is(hr, _DXGI_ERROR_ACCESS_LOST):
                raise _DxgiAccessLost("desktop duplication access lost while releasing frame")
            _check(hr, "IDXGIOutputDuplication::ReleaseFrame")

        if not fresh:
            return self._last_frame
        return self._map_staging(mon)

    def _map_staging(self, mon: Monitor) -> tuple[bytes, int, int]:
        mapped = _D3D11_MAPPED_SUBRESOURCE()
        hr = _vtable_method(
            self._context,
            14,
            HRESULT,
            _PTR,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.POINTER(_D3D11_MAPPED_SUBRESOURCE),
        )(self._context, self._staging, 0, _D3D11_MAP_READ, 0, ctypes.byref(mapped))
        _check(hr, "ID3D11DeviceContext::Map")
        if not mapped.pData:
            raise DxgiError("ID3D11DeviceContext::Map returned null")
        try:
            pitch = int(mapped.RowPitch)
            raw = ctypes.string_at(mapped.pData, pitch * self._height)
            raw = _pack_rows(raw, pitch, self._width, self._height)
        finally:
            _void_method(self._context, 15, _PTR, ctypes.c_uint)(
                self._context, self._staging, 0
            )
        frame = (raw, self._width, self._height)
        self._last_frame = frame
        return frame

    def close(self) -> None:
        self._release_duplication()
        for output in self._outputs:
            _release(output.pointer)
        self._outputs = []
        self._monitors = []
        if self._context:
            _release(self._context)
            self._context = _PTR()
        if self._device:
            _release(self._device)
            self._device = _PTR()


# --------------------------------------------------------------------- probe
def _probe_command() -> list[str]:
    if getattr(sys, "frozen", False):
        return [sys.executable, _PROBE_COMMAND]
    return [sys.executable, "-m", "peerdesk", _PROBE_COMMAND]


def _run_probe_subprocess() -> dict:
    command = _probe_command()
    kwargs = {
        "capture_output": True,
        "text": True,
        "encoding": "utf-8",
        "errors": "replace",
        "timeout": _PROBE_TIMEOUT,
    }
    if os.name == "nt":
        kwargs["creationflags"] = _CREATE_NO_WINDOW
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(command, **kwargs)
    except subprocess.TimeoutExpired:
        return {
            "ok": False,
            "error": f"DXGI probe timed out after {_PROBE_TIMEOUT:g}s",
            "elapsed_ms": round((time.perf_counter() - t0) * 1000, 1),
        }
    except OSError as exc:
        return {"ok": False, "error": f"DXGI probe could not start: {exc}"}

    elapsed_ms = round((time.perf_counter() - t0) * 1000, 1)
    result = None
    for line in reversed((proc.stdout or "").splitlines()):
        try:
            candidate = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(candidate, dict) and "ok" in candidate:
            result = candidate
            break
    if result is None:
        detail = (proc.stderr or "").strip().splitlines()
        tail = detail[-1] if detail else "no diagnostic output"
        return {
            "ok": False,
            "error": f"DXGI probe exited with code {proc.returncode}: {tail}",
            "returncode": proc.returncode,
            "elapsed_ms": elapsed_ms,
        }
    result["returncode"] = proc.returncode
    result["elapsed_ms"] = elapsed_ms
    if proc.returncode != 0:
        result["ok"] = False
        result.setdefault("error", f"DXGI probe exited with code {proc.returncode}")
    return result


def probe_dxgi() -> dict:
    """Probe DXGI once per process, in a crash-isolated child process."""
    global _probe_result
    disabled = (os.environ.get("PEERDESK_DXGI_PROBE") or "").strip().lower()
    if disabled in ("0", "off", "false", "no"):
        return {"ok": False, "error": "DXGI probe disabled by PEERDESK_DXGI_PROBE"}
    with _probe_lock:
        if _probe_result is None:
            _probe_result = _run_probe_subprocess()
        return dict(_probe_result)


def _probe_once() -> dict:
    t0 = time.perf_counter()
    cap = DxgiCapture()
    try:
        monitors = cap.monitors()
        if not monitors:
            raise DxgiError("DXGI probe found no monitors")
        data, width, height = cap.grab_rgb(monitors[0])
        if not data or width <= 0 or height <= 0:
            raise DxgiError("DXGI probe captured an empty frame")
        return {
            "ok": True,
            "width": width,
            "height": height,
            "monitors": len(monitors),
            "elapsed_ms": round((time.perf_counter() - t0) * 1000, 1),
        }
    finally:
        cap.close()


def probe_main() -> int:
    """Hidden child-process entry point used by :func:`probe_dxgi`."""
    try:
        result = _probe_once()
    except Exception as exc:
        result = {"ok": False, "error": f"{type(exc).__name__}: {exc}"}
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result.get("ok") else 1


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(probe_main())
