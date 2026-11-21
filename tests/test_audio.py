"""音频通道单测：µ-law 编解码、重采样、帧打包与采集泵。"""

from __future__ import annotations

import array
import math
import time
import unittest

from peerdesk import audio, wire


def _sine(rate: int, freq: float, frames: int, *, amp: float = 9000.0, channels: int = 1) -> bytes:
    out = array.array("h")
    for i in range(frames):
        v = int(amp * math.sin(2 * math.pi * freq * i / rate))
        for _ in range(channels):
            out.append(v)
    return out.tobytes()


class UlawTest(unittest.TestCase):
    def test_silence_roundtrip(self):
        enc = audio.ulaw_encode(b"\x00\x00" * audio.SAMPLES)
        self.assertEqual(len(enc), audio.MULAW_BYTES)
        self.assertEqual(set(enc), {0xFF})
        dec = audio.ulaw_decode(enc)
        self.assertEqual(len(dec), audio.FRAME_BYTES)
        self.assertEqual(set(dec), {0})

    def test_roundtrip_error_small(self):
        pcm = _sine(audio.RATE, 440, audio.SAMPLES)
        dec = audio.ulaw_decode(audio.ulaw_encode(pcm))
        src = array.array("h")
        src.frombytes(pcm)
        got = array.array("h")
        got.frombytes(dec)
        self.assertEqual(len(src), len(got))
        err = max(abs(a - b) for a, b in zip(src, got, strict=True))
        self.assertLess(err / 32768, 0.02, f"µ-law 误差过大: {err}")

    def test_negative_side_symmetric(self):
        pcm = _sine(audio.RATE, 220, 320, amp=12000)
        dec = audio.ulaw_decode(audio.ulaw_encode(pcm))
        got = array.array("h")
        got.frombytes(dec)
        # 负半轴不能被 µ-law 的符号位吃掉（经典 bug：负数全变 0）
        self.assertLess(min(got), -9000)
        self.assertGreater(max(got), 9000)
        self.assertGreater(min(got), -32768)

    def test_empty(self):
        self.assertEqual(audio.ulaw_encode(b""), b"")
        self.assertEqual(audio.ulaw_decode(b""), b"")


class ResamplerTest(unittest.TestCase):
    def test_48k_stereo_f32_to_16k_mono(self):
        frames = 4800  # 100 ms
        buf = array.array("f")
        for i in range(frames):
            v = 0.5 * math.sin(2 * math.pi * 1000 * i / 48000)
            buf.append(v)
            buf.append(v)
        rs = audio.MonoResampler(48000, 2, "f32")
        out = rs.feed(buf.tobytes())
        self.assertAlmostEqual(len(out), 1600, delta=2)
        # 1 kHz 在 16 kHz 采样下每 16 个采样过零一次
        zeros = sum(1 for i in range(1, len(out)) if (out[i - 1] < 0) != (out[i] < 0))
        self.assertAlmostEqual(zeros, 200, delta=8)

    def test_16k_passthrough_keeps_stream(self):
        rs = audio.MonoResampler(16000, 1, "s16")
        first = rs.feed(_sine(16000, 300, 320))
        second = rs.feed(_sine(16000, 300, 320))
        self.assertAlmostEqual(len(first) + len(second), 640, delta=2)

    def test_dc_offset_preserved(self):
        rs = audio.MonoResampler(32000, 1, "s16")
        out = rs.feed(array.array("h", [1234] * 3200).tobytes())
        self.assertTrue(out)
        self.assertLess(max(abs(v - 1234) for v in out), 40)


class WireAudioTest(unittest.TestCase):
    def test_pack_unpack(self):
        cur = b"\x7f" * 320
        prev = b"\x11" * 320
        pkt = wire.pack_audio(7, 1600, 320, cur, prev)
        seq, ts, samples, p, c = wire.unpack_audio(pkt)
        self.assertEqual((seq, ts, samples), (7, 1600, 320))
        self.assertEqual(p, prev)
        self.assertEqual(c, cur)

    def test_no_previous(self):
        pkt = wire.pack_audio(1, 20, 320, b"\x00" * 320)
        seq, _ts, samples, p, c = wire.unpack_audio(pkt)
        self.assertEqual(p, b"")
        self.assertEqual(len(c), 320)
        self.assertEqual(seq, 1)
        self.assertEqual(samples, 320)

    def test_type_is_unreliable(self):
        self.assertIn(wire.TYPE_AUDIO, wire.UNRELIABLE_TYPES)
        self.assertEqual(wire.STREAM_AUDIO, 4)

    def test_header_size(self):
        self.assertEqual(wire.AUDIO_HDR.size, 12)
        self.assertEqual(wire.HEADER.size + wire.AUDIO_HDR.size + audio.MULAW_BYTES * 2, 20 + 12 + 640)


class PumpTest(unittest.TestCase):
    def test_tone_source_frames(self):
        frames: list[bytes] = []
        pump = audio.AudioPump(audio.ToneSource(440), frames.append)
        pump.start()
        time.sleep(0.32)
        pump.stop()
        pump.join(2.0)
        self.assertGreaterEqual(len(frames), 10, f"帧数太少: {len(frames)}")
        self.assertFalse(pump.error)
        seqs = []
        prevs = []
        for pkt in frames:
            seq, _ts, samples, prev, cur = wire.unpack_audio(pkt)
            self.assertEqual(samples, audio.SAMPLES)
            self.assertEqual(len(cur), audio.MULAW_BYTES)
            seqs.append(seq)
            prevs.append(prev)
        self.assertEqual(seqs, list(range(seqs[0], seqs[0] + len(seqs))))
        for i in range(1, len(frames)):
            self.assertEqual(prevs[i], wire.unpack_audio(frames[i - 1])[4])

    def test_source_error_reported(self):
        class Bad:
            label = "bad"

            def open(self):
                raise audio.AudioError("没有可用的输出设备")

            def read(self, timeout):  # pragma: no cover - 不会被调用
                return b""

            def close(self):
                pass

        errors: list[str] = []
        pump = audio.AudioPump(Bad(), lambda _p: None, on_error=errors.append)
        pump.start()
        pump.join(2.0)
        self.assertTrue(errors)
        self.assertIn("输出设备", errors[0])
        self.assertFalse(pump.is_alive())

    def test_probe_reports_detail(self):
        ok, detail = audio.probe(timeout=0.05)
        self.assertIsInstance(ok, bool)
        self.assertTrue(detail)


if __name__ == "__main__":
    unittest.main()
