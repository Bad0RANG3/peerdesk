"""Audio: capture what the host is playing (WASAPI loopback) and ship it as G.711.

Why µ-law at 16 kHz mono: one 20 ms frame is exactly 320 bytes, i.e. 128 kbit/s,
small enough to ride next to the video stream without starving it, and cheap to
decode in the browser (a 256 entry table, no WASM, no third party library).

Pipeline::

    WASAPI loopback (48 kHz float32 stereo, mix format)
        -> downmix to mono + decimate to 16 kHz        (MonoResampler)
        -> 20 ms frames -> µ-law                       (ulaw_encode)
        -> AUDIO_HDR + previous frame + current frame   (wire.pack_audio)
        -> TYPE_AUDIO / STREAM_AUDIO datagram           (PeerLink.send_audio)

The previous frame is repeated inside every datagram: audio cannot wait for a
retransmit, so one lost packet must be inaudible rather than a hiccup. 620 bytes
per 20 ms is still only 250 kbit/s and it collapses to nothing on a lossy link
because the receiver drops the duplicate by sequence number.

Everything here is pure ctypes; the only Windows-specific part is
``LoopbackCapture``. ``ToneSource`` keeps the whole path testable without a
sound card (``python -m peerdesk.agent --audio-test``).
"""

from __future__ import annotations

import ctypes
import logging
import math
import struct
import sys
import threading
import time
import uuid
from array import array
from collections.abc import Callable
from functools import lru_cache

from . import wire

log = logging.getLogger("peerdesk.audio")

RATE = 16000
FRAME_MS = 20
SAMPLES = RATE * FRAME_MS // 1000  # 320
FRAME_BYTES = SAMPLES * 2  # int16 mono
MULAW_BYTES = SAMPLES  # one byte per sample
CODEC = "pcmu"

# 静音抑制：桌面长时间没有声音时不必按 128 kbit/s 送零。
SILENCE_HOLD = 25  # 连续静音 25 帧（0.5 s）后开始抑制
SILENCE_KEEP = 25  # 抑制期间每 25 帧留一个心跳帧，免得接收端彻底断流
SILENT_FRAME = bytes([0xFF]) * MULAW_BYTES  # µ-law 的 0xFF 就是 0

_ULAW_BIAS = 0x84
_ULAW_CLIP = 32635


class AudioError(RuntimeError):
    """采集/播放链路不可用（没有设备、被独占、驱动拒绝混音格式）。"""


# --------------------------------------------------------------------------- #
# G.711 µ-law
# --------------------------------------------------------------------------- #
@lru_cache(maxsize=1)
def _encode_table() -> bytes:
    """65536 项查表：int16 位模式 -> µ-law 字节（与 audioop.lin2ulaw 一致）。"""
    table = bytearray(65536)
    for i in range(65536):
        sample = i - 65536 if i >= 32768 else i
        sign = 0x80 if sample < 0 else 0x00
        if sample < 0:
            sample = -sample
        sample = min(sample, _ULAW_CLIP) + _ULAW_BIAS
        exponent = 7
        mask = 0x4000
        while exponent > 0 and not (sample & mask):
            exponent -= 1
            mask >>= 1
        mantissa = (sample >> (exponent + 3)) & 0x0F
        table[i] = ~(sign | (exponent << 4) | mantissa) & 0xFF
    return bytes(table)


@lru_cache(maxsize=1)
def _decode_table() -> tuple[int, ...]:
    out = []
    for byte in range(256):
        u = ~byte & 0xFF
        value = ((u & 0x0F) << 3) + _ULAW_BIAS
        value <<= (u & 0x70) >> 4
        out.append((_ULAW_BIAS - value) if (u & 0x80) else (value - _ULAW_BIAS))
    return tuple(out)


def ulaw_encode(pcm) -> bytes:
    """int16 采样（bytes / array / 可迭代）-> µ-law 字节。"""
    table = _encode_table()
    if isinstance(pcm, (bytes, bytearray, memoryview)):
        view = memoryview(pcm).cast("h")
    else:
        view = pcm
    return bytes([table[sample & 0xFFFF] for sample in view])


def ulaw_decode(data: bytes) -> bytes:
    """µ-law 字节 -> int16 原生序 bytes。"""
    table = _decode_table()
    return array("h", [table[b] for b in data]).tobytes()


# --------------------------------------------------------------------------- #
# 设备格式 -> 16 kHz 单声道
# --------------------------------------------------------------------------- #
class MonoResampler:
    """把任意采样率/声道数的设备数据变成 16 kHz 单声道 int16。

    降采样时先做 3 抽头均值（box）抑制混叠，再线性插值取点；升采样直接线性。
    实例带状态，可以按任意大小的块连续喂。
    """

    def __init__(self, rate: int, channels: int, sample_format: str = "f32") -> None:
        self.rate = max(1, int(rate))
        self.channels = max(1, int(channels))
        self.sample_format = sample_format
        self.step = self.rate / float(RATE)
        self._buf: list[float] = []
        self._pos = 0.0
        self._scale = 32767.0 if sample_format == "f32" else 1.0

    # ------------------------------------------------------------------ input
    def _mono(self, raw: bytes) -> list[float]:
        ch = self.channels
        if self.sample_format == "f32":
            count = len(raw) // 4
            if not count:
                return []
            values = struct.unpack_from(f"<{count}f", raw, 0)
        elif self.sample_format == "s16":
            count = len(raw) // 2
            if not count:
                return []
            values = array("h")
            values.frombytes(raw[: count * 2])
        else:  # pragma: no cover - 其它格式在打开设备时就拒绝了
            raise AudioError(f"不支持的采样格式 {self.sample_format}")
        if ch == 1:
            return [float(v) for v in values]
        usable = count - (count % ch)
        mono = []
        inv = 1.0 / ch
        for i in range(0, usable, ch):
            total = 0.0
            for c in range(ch):
                total += values[i + c]
            mono.append(total * inv)
        return mono

    def feed(self, raw: bytes) -> array:
        mono = self._mono(raw)
        if not mono:
            return array("h")
        buf = self._buf
        buf.extend(mono)
        out = array("h")
        step = self.step
        pos = self._pos
        last = len(buf) - 1
        limit = len(buf) - 2
        scale = self._scale
        box = step >= 2.0
        while pos <= limit:
            idx = int(pos)
            if box:
                left = buf[idx - 1] if idx > 0 else buf[idx]
                value = (left + buf[idx] + buf[idx + 1]) / 3.0
            else:
                frac = pos - idx
                value = buf[idx] * (1.0 - frac) + buf[idx + 1] * frac
            sample = int(round(value * scale))
            out.append(-32768 if sample < -32768 else (32767 if sample > 32767 else sample))
            pos += step
        keep = int(pos)
        if keep > 0:
            del buf[: min(keep, last + 1)]
            pos -= keep
        if len(buf) > 4096:  # 设备卡住时兜底，避免无限增长
            del buf[:-4]
            pos = 0.0
        self._pos = pos
        return out


# --------------------------------------------------------------------------- #
# 采集源
# --------------------------------------------------------------------------- #
class AudioSource:
    """协议：``open()`` / ``read(timeout) -> int16 mono @16 kHz`` / ``close()``。"""

    label = "none"

    def open(self) -> None:  # pragma: no cover - 抽象
        raise NotImplementedError

    def read(self, timeout: float = 0.04) -> bytes:  # pragma: no cover - 抽象
        raise NotImplementedError

    def close(self) -> None:  # pragma: no cover - 抽象
        raise NotImplementedError


class ToneSource(AudioSource):
    """合成音源：没有声卡也能跑通整条链路（--audio-test / 单测）。"""

    label = "tone 440 Hz"

    def __init__(self, freq: float = 440.0) -> None:
        self.freq = float(freq)
        self._phase = 0.0
        self._closed = False

    def open(self) -> None:
        self._closed = False

    def read(self, timeout: float = 0.0) -> bytes:
        step = 2.0 * math.pi * self.freq / RATE
        out = array("h")
        for _ in range(SAMPLES):
            out.append(int(round(12000.0 * math.sin(self._phase))))
            self._phase += step
            if self._phase > 2.0 * math.pi:
                self._phase -= 2.0 * math.pi
        return out.tobytes()

    def close(self) -> None:
        self._closed = True


class LoopbackCapture:
    """WASAPI 共享模式 loopback：抓默认播放设备的混音。"""

    CLSID_MMDeviceEnumerator = "{BCDE0395-E52F-467C-8E3D-C4579291692E}"
    IID_IMMDeviceEnumerator = "{A95664D2-9614-4F35-A746-DE8DB63617E6}"
    IID_IAudioClient = "{1CB9AD4C-DBFA-4C32-B178-C2F568A703B2}"
    IID_IAudioCaptureClient = "{C8ADBD64-E71E-48A0-A4DE-185C395CD317}"
    SUBTYPE_IEEE_FLOAT = "{00000003-0000-0010-8000-00AA00389B71}"

    CLSCTX_ALL = 0x17
    COINIT_APARTMENTTHREADED = 0x02
    AUDCLNT_SHAREMODE_SHARED = 0
    AUDCLNT_STREAMFLAGS_LOOPBACK = 0x00020000
    AUDCLNT_BUFFERFLAGS_SILENT = 0x00000002
    eRENDER = 0
    eCONSOLE = 0
    BUFFER_HNS = 2_000_000  # 200 ms，单位 100ns

    def __init__(self) -> None:
        self._ole32 = None
        self._enumerator = None
        self._device = None
        self._client = None
        self._capture = None
        self._started = False
        self._com = False
        self.format = "f32"
        self.channels = 2
        self.rate = 48000
        self._block_align = 8

    # ---------------------------------------------------------------- helpers
    def _vt(self, pointer, index: int, restype, *argtypes):
        table = ctypes.cast(pointer, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        return ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *argtypes)(table[index])

    @staticmethod
    def _check(hr: int, what: str) -> None:
        if hr < 0:
            raise AudioError(f"{what} 失败 hr=0x{hr & 0xFFFFFFFF:08X}")

    @staticmethod
    def _release(pointer) -> None:
        if not pointer:
            return
        try:
            table = ctypes.cast(pointer, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
            ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(table[2])(pointer)
        except Exception:  # pragma: no cover - 释放失败不该拖垮调用方
            pass

    # ------------------------------------------------------------------- setup
    def open(self) -> None:
        if sys.platform != "win32":  # pragma: no cover - 项目本身只支持 Windows
            raise AudioError("WASAPI 仅在 Windows 可用")
        ole32 = ctypes.windll.ole32
        self._ole32 = ole32
        hr = ole32.CoInitializeEx(None, self.COINIT_APARTMENTTHREADED)
        # S_FALSE(1) 表示本线程已经初始化过，同样可以用
        if hr < 0:
            raise AudioError(f"CoInitializeEx 失败 hr=0x{hr & 0xFFFFFFFF:08X}")
        self._com = True
        try:
            self._open_device()
        except Exception:
            self.close()
            raise

    def _open_device(self) -> None:
        ole32 = self._ole32
        ole32.CoCreateInstance.restype = ctypes.HRESULT
        ole32.CoCreateInstance.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        clsid = _guid(self.CLSID_MMDeviceEnumerator)
        iid = _guid(self.IID_IMMDeviceEnumerator)
        enum = ctypes.c_void_p()
        self._check(ole32.CoCreateInstance(ctypes.byref(clsid), None, self.CLSCTX_ALL, ctypes.byref(iid),
                                           ctypes.byref(enum)), "CoCreateInstance(IMMDeviceEnumerator)")
        self._enumerator = enum

        device = ctypes.c_void_p()
        get_default = self._vt(self._enumerator, 4, ctypes.HRESULT, ctypes.c_int, ctypes.c_int,
                               ctypes.POINTER(ctypes.c_void_p))
        self._check(get_default(self._enumerator, self.eRENDER, self.eCONSOLE, ctypes.byref(device)),
                    "GetDefaultAudioEndpoint(eRender)")
        self._device = device

        client = ctypes.c_void_p()
        activate = self._vt(self._device, 3, ctypes.HRESULT, ctypes.c_void_p, ctypes.c_uint32,
                            ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))
        iid_client = _guid(self.IID_IAudioClient)
        self._check(activate(self._device, ctypes.byref(iid_client), self.CLSCTX_ALL, None,
                             ctypes.byref(client)), "IMMDevice::Activate(IAudioClient)")
        self._client = client

        mix = ctypes.c_void_p()
        get_mix = self._vt(self._client, 8, ctypes.HRESULT, ctypes.POINTER(ctypes.c_void_p))
        self._check(get_mix(self._client, ctypes.byref(mix)), "IAudioClient::GetMixFormat")
        try:
            self._parse_format(mix.value)
            initialize = self._vt(
                self._client,
                3,
                ctypes.HRESULT,
                ctypes.c_int,
                ctypes.c_uint32,
                ctypes.c_int64,
                ctypes.c_int64,
                ctypes.c_void_p,
                ctypes.c_void_p,
            )
            self._check(
                initialize(self._client, self.AUDCLNT_SHAREMODE_SHARED, self.AUDCLNT_STREAMFLAGS_LOOPBACK,
                           self.BUFFER_HNS, 0, mix, None),
                "IAudioClient::Initialize(loopback)",
            )
        finally:
            if mix:
                ole32.CoTaskMemFree(mix)

        capture = ctypes.c_void_p()
        get_service = self._vt(self._client, 14, ctypes.HRESULT, ctypes.c_void_p,
                               ctypes.POINTER(ctypes.c_void_p))
        iid_capture = _guid(self.IID_IAudioCaptureClient)
        self._check(get_service(self._client, ctypes.byref(iid_capture), ctypes.byref(capture)),
                    "IAudioClient::GetService(IAudioCaptureClient)")
        self._capture = capture

        start = self._vt(self._client, 10, ctypes.HRESULT)
        self._check(start(self._client), "IAudioClient::Start")
        self._started = True
        log.info("loopback %d Hz %dch %s", self.rate, self.channels, self.format)

    def _parse_format(self, address: int) -> None:
        if not address:
            raise AudioError("GetMixFormat 返回空指针")
        block = ctypes.string_at(address, 40)
        tag, channels, rate, _avg, block_align, bits = struct.unpack_from("<HHIIHH", block, 0)
        cb_size = struct.unpack_from("<H", block, 16)[0]
        sub = block[24:40] if cb_size >= 22 else b""
        float_guid = uuid.UUID(self.SUBTYPE_IEEE_FLOAT).bytes_le
        if tag == 3 or (tag == 0xFFFE and sub == float_guid):
            fmt = "f32"
        elif tag == 1 and bits == 16:
            fmt = "s16"
        else:
            raise AudioError(f"混音格式不支持 tag={tag} bits={bits}")
        if channels < 1 or channels > 8:
            raise AudioError(f"声道数不支持 {channels}")
        self.format = fmt
        self.channels = channels
        self.rate = rate
        self._block_align = block_align or max(1, channels * bits // 8)

    # -------------------------------------------------------------------- read
    def read_raw(self, timeout: float = 0.04) -> bytes:
        if not self._capture:
            raise AudioError("loopback 尚未打开")
        get_next = self._vt(self._capture, 5, ctypes.HRESULT, ctypes.POINTER(ctypes.c_uint32))
        get_buffer = self._vt(
            self._capture,
            3,
            ctypes.HRESULT,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint64),
            ctypes.POINTER(ctypes.c_uint64),
        )
        release = self._vt(self._capture, 4, ctypes.HRESULT, ctypes.c_uint32)

        out = bytearray()
        deadline = time.monotonic() + max(0.0, timeout)
        pending = ctypes.c_uint32(0)
        while True:
            self._check(get_next(self._capture, ctypes.byref(pending)), "GetNextPacketSize")
            if pending.value:
                break
            if time.monotonic() >= deadline:
                return b""
            time.sleep(0.004)

        while pending.value:
            data = ctypes.c_void_p()
            frames = ctypes.c_uint32(0)
            flags = ctypes.c_uint32(0)
            dev_pos = ctypes.c_uint64(0)
            qpc = ctypes.c_uint64(0)
            self._check(
                get_buffer(self._capture, ctypes.byref(data), ctypes.byref(frames), ctypes.byref(flags),
                           ctypes.byref(dev_pos), ctypes.byref(qpc)),
                "IAudioCaptureClient::GetBuffer",
            )
            size = frames.value * self._block_align
            if size:
                if flags.value & self.AUDCLNT_BUFFERFLAGS_SILENT or not data:
                    out.extend(b"\x00" * size)
                else:
                    out.extend(ctypes.string_at(data, size))
            release(self._capture, frames.value)
            self._check(get_next(self._capture, ctypes.byref(pending)), "GetNextPacketSize")
        return bytes(out)

    # ------------------------------------------------------------------- close
    def close(self) -> None:
        if self._started and self._client:
            try:
                stop = self._vt(self._client, 11, ctypes.HRESULT)
                stop(self._client)
            except Exception:  # pragma: no cover - 设备已移除时 Stop 会失败
                pass
        self._started = False
        for attr in ("_capture", "_client", "_device", "_enumerator"):
            pointer = getattr(self, attr, None)
            if pointer:
                self._release(pointer)
                setattr(self, attr, None)
        if self._com and self._ole32:
            try:
                self._ole32.CoUninitialize()
            except Exception:  # pragma: no cover
                pass
        self._com = False


class LoopbackSource(AudioSource):
    """默认播放设备 -> 16 kHz 单声道 int16。"""

    def __init__(self) -> None:
        self._capture = LoopbackCapture()
        self._resampler: MonoResampler | None = None
        self.label = "loopback"

    def open(self) -> None:
        self._capture.open()
        self._resampler = MonoResampler(self._capture.rate, self._capture.channels, self._capture.format)
        self.label = f"loopback {self._capture.rate} Hz {self._capture.channels}ch"

    def read(self, timeout: float = 0.04) -> bytes:
        if self._resampler is None:
            raise AudioError("采集源尚未打开")
        raw = self._capture.read_raw(timeout)
        if not raw:
            return b""
        return self._resampler.feed(raw).tobytes()

    def close(self) -> None:
        self._capture.close()


def open_source(test: bool = False) -> AudioSource:
    """按配置挑采集源；test=True 用合成音（无声卡也能验证链路）。"""
    return ToneSource() if test else LoopbackSource()


def probe(timeout: float = 0.3) -> tuple[bool, str]:
    """doctor 用：开 300ms 采集，确认设备可用。"""
    source = LoopbackSource()
    try:
        source.open()
        got = 0
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            got += len(source.read(0.05))
        return True, f"{source.label}，{got // 2} 采样"
    except Exception as exc:
        return False, str(exc)
    finally:
        try:
            source.close()
        except Exception:
            pass


# --------------------------------------------------------------------------- #
# 打包线程
# --------------------------------------------------------------------------- #
class AudioPump(threading.Thread):
    """把采集源切成 20 ms 帧，编码后交给 ``on_frame(payload)``。

    线程负责全部设备交互（WASAPI 的 COM 单元是线程私有的），发送侧只需要一个
    ``loop.call_soon_threadsafe``。节拍以"已发出的帧数"为准，所以设备一次吐
    200 ms 也不会把节奏打乱。
    """

    def __init__(
        self,
        source: AudioSource,
        on_frame: Callable[[bytes], None],
        *,
        on_error: Callable[[str], None] | None = None,
        name: str = "peerdesk-audio",
    ) -> None:
        super().__init__(name=name, daemon=True)
        self.source = source
        self.on_frame = on_frame
        self.on_error = on_error
        self.frames = 0
        self.bytes = 0
        self.dropped = 0
        self.silent = 0
        self.started = threading.Event()
        self._stop = threading.Event()
        self._seq = 0
        self._t0 = 0.0
        self.error = ""

    # ------------------------------------------------------------------ public
    def stop(self) -> None:
        self._stop.set()

    @property
    def label(self) -> str:
        return self.source.label

    # -------------------------------------------------------------------- loop
    def run(self) -> None:  # pragma: no cover - 线程体，由单测间接覆盖
        try:
            self.source.open()
        except Exception as exc:
            self.error = str(exc)
            log.warning("audio source open failed: %s", exc)
            if self.on_error:
                self.on_error(self.error)
            self.started.set()
            return
        self.started.set()
        self._t0 = time.monotonic()
        carry = bytearray()
        previous = b""
        try:
            while not self._stop.is_set():
                try:
                    chunk = self.source.read(0.04)
                except Exception as exc:  # 设备被拔掉/被独占
                    self.error = str(exc)
                    log.warning("audio read failed: %s", exc)
                    if self.on_error:
                        self.on_error(self.error)
                    break
                if chunk:
                    carry += chunk
                emitted = 0
                while len(carry) >= FRAME_BYTES:
                    pcm = bytes(carry[:FRAME_BYTES])
                    del carry[:FRAME_BYTES]
                    current = ulaw_encode(pcm)
                    if current == SILENT_FRAME:
                        self.silent += 1
                    else:
                        self.silent = 0
                    if self.silent > SILENCE_HOLD and self.silent % SILENCE_KEEP:
                        self.dropped += 1
                        continue  # 长时间静音，只留稀疏心跳
                    payload = wire.pack_audio(self._next_seq(), self._now_ms(), SAMPLES, current, previous)
                    previous = current
                    self.frames += 1
                    self.bytes += len(payload)
                    emitted += 1
                    try:
                        self.on_frame(payload)
                    except Exception:
                        log.exception("audio on_frame failed")
                if not emitted:
                    # 设备暂时没有新数据（静音时 loopback 会给空包），别空转
                    self._stop.wait(0.004)
                target = self._t0 + self.frames * (FRAME_MS / 1000.0)
                delay = target - time.monotonic()
                if delay > 0:
                    self._stop.wait(min(delay, FRAME_MS / 1000.0))
        finally:
            try:
                self.source.close()
            except Exception:
                pass
            log.info("audio pump stop: frames=%d bytes=%d", self.frames, self.bytes)

    def _next_seq(self) -> int:
        seq = self._seq & 0xFFFFFFFF
        self._seq += 1
        return seq

    def _now_ms(self) -> int:
        return int((time.monotonic() - self._t0) * 1000.0) & 0xFFFFFFFF


def _guid(text: str):
    class GUID(ctypes.Structure):
        _fields_ = [
            ("Data1", ctypes.c_uint32),
            ("Data2", ctypes.c_uint16),
            ("Data3", ctypes.c_uint16),
            ("Data4", ctypes.c_ubyte * 8),
        ]

    return GUID.from_buffer_copy(uuid.UUID(text).bytes_le)
