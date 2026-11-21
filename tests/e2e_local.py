"""In-process end-to-end check: server + agent + viewer on loopback.

    python tests/e2e_local.py            # direct P2P path
    python tests/e2e_local.py --relay    # forced relay path

It spins the real components, captures the real desktop, and asserts frames
arrive at the viewer. Exit code 0 means the whole stack works.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import statistics
import sys
import time
import types
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from peerdesk.agent import AgentApp  # noqa: E402
from peerdesk.server import SignalingServer  # noqa: E402
from peerdesk.viewer import ViewerApp  # noqa: E402

CODE = "731845926"


def ns(**kw):
    return types.SimpleNamespace(**kw)


async def run(relay: bool, seconds: float) -> int:
    sig_port, udp_port = 7910, 7911
    srv = SignalingServer("127.0.0.1", sig_port, udp_port)
    asyncio.get_event_loop().create_task(srv.start())
    await asyncio.sleep(0.4)

    agent = AgentApp(
        ns(
            code=CODE,
            server=f"127.0.0.1:{sig_port}",
            fps=15,
            width=1280,
            quality=55,
            monitor=0,
            no_skip_static=True,
            no_clipboard=True,
            no_audio=False,
            audio_test=True,
            no_input=True,
            relay_only=relay,
            web_port=0,
            udp_port=0,
            open=False,
            verbose=False,
        )
    )
    asyncio.get_event_loop().create_task(agent.start())
    await asyncio.sleep(1.2)

    viewer = ViewerApp(
        ns(
            code=CODE,
            server=f"127.0.0.1:{sig_port}",
            relay_only=relay,
            web_port=0,
            udp_port=0,
            open=False,
            verbose=False,
        )
    )
    asyncio.get_event_loop().create_task(viewer.start())

    frames: list[int] = []
    audio: list[bytes] = []
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        await asyncio.sleep(0.25)
        session = viewer.session
        if session is None:
            continue
        if getattr(session, "_wrapped", False) is False:
            original = session.on_frame

            def hook(data, meta, _orig=original):
                frames.append(len(data))
                _orig(data, meta)

            session.on_frame = hook

            original_audio = session.on_audio

            def hook_audio(payload, _orig=original_audio):
                audio.append(payload)
                _orig(payload)

            session.on_audio = hook_audio
            session._wrapped = True
        state = viewer.link.state if viewer.link else "?"
        if state in ("direct", "relay") and frames:
            break

    started = time.monotonic()
    while time.monotonic() - started < seconds:
        await asyncio.sleep(0.5)

    state = viewer.link.state if viewer.link else "none"
    path = viewer.link.stats.path if viewer.link else ""
    stats = viewer.session.stats if viewer.session else {}
    audio_ok = False
    audio_detail = "未收到声音"
    if audio:
        from peerdesk import audio as audio_mod
        from peerdesk import wire

        good = 0
        for payload in audio:
            seq, _ts, samples, _prev, cur = wire.unpack_audio(payload)
            if samples != audio_mod.SAMPLES or len(cur) != audio_mod.MULAW_BYTES:
                continue
            dec = audio_mod.ulaw_decode(cur)
            if any(dec):
                good += 1
        audio_ok = good >= 20
        audio_detail = f"{len(audio)} 帧（{good} 帧非静音 ≈ {len(audio) * 0.32:.0f} KB / {len(audio) * 0.02:.1f}s）"

    ok = state in ("direct", "relay") and len(frames) >= 10 and audio_ok

    print(f"path state : {state}  ({path})  relay={relay}")
    print(f"frames     : {len(frames)} in ~{seconds + 2:.0f}s")
    if frames:
        print(f"frame size : avg {statistics.mean(frames) / 1024:.0f} KB, max {max(frames) / 1024:.0f} KB")
    print(f"audio      : {audio_detail}")
    print(f"host stats : {stats}")
    if viewer.session:
        print(f"link stats : rtt={viewer.link.stats.rtt_ms:.0f}ms mode={viewer.link.stats.mode} "
              f"mbps={viewer.link.stats.mbps:.2f} fps={viewer.link.stats.fps:.1f}")
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--relay", action="store_true")
    ap.add_argument("--seconds", type=float, default=6.0)
    args = ap.parse_args()
    os.environ.setdefault("PEERDESK_LOGLEVEL", "WARNING")
    try:
        return asyncio.run(run(args.relay, args.seconds))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
