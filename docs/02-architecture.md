# PeerDesk 架构

PeerDesk（对端桌面）是一个**纯 Python 3.11+** 的 Windows 远程桌面，不依赖浏览器引擎、
不依赖 WebRTC 后端、不依赖 npm 生态。三个进程角色共用一个包：

```
        ┌──────────────┐   信令 TCP(JSON Lines)   ┌──────────────┐
        │    agent     │◀────────────────────────▶│    server    │
        │  (被控端)     │                          │  (自托管)     │
        └──────┬───────┘                          └──────┬───────┘
               │  PDTP/1 over UDP（打洞直连/中继）        │
               │                                         │
        ┌──────▼───────┐        同一 socket 兼做 STUN ────┘
        │    viewer    │
        │  (控制端)     │──▶ 本地 HTTP+WS ──▶ 浏览器 canvas
        └──────────────┘
```

## 1. 进程与部署形态

| 角色 | 命令 | 作用 |
| --- | --- | --- |
| agent | `peerdesk agent --server HOST:7900 --code 604173829 --web-port 7701` | 显示 9 位连接码，抓屏、注入输入，本地面板给操作者看状态/改参数 |
| viewer | `peerdesk viewer --web-port 7702` | 打开页面输入连接码，接收视频、转发键鼠 |
| server | `peerdesk server --port 7900 --udp-port 7901` | 信令（TCP）+ 中继与 STUN（UDP 同端口） |
| doctor | `peerdesk doctor` | 环境自检（依赖/显示器/输入/加密/UDP/STUN） |

三种典型部署：

1. **完全自托管**：一台有公网 IP 的机器跑 `peerdesk server`，两端指向它。
2. **内网直连**：同一局域网可以不起 server（把候选写死），或起在内网任意一台机器上。
3. **中继兜底**：对称 NAT/企业防火墙下 6 秒自动升级中继，画质不变、延迟略高。

## 2. 模块划分

| 文件 | 行数 | 职责 | 关键对象 |
| --- | --- | --- | --- |
| `wire.py` | 243 | 报文格式、分片、XOR FEC、ACK/NACK 位图 | `Header`、`FragmentAssembler`、`ParityGroup` |
| `crypto.py` | 215 | X25519 → HKDF → ChaCha20-Poly1305、重放窗口 | `KeyPair`、`SessionKeys`、`CryptoContext` |
| `transport.py` | 791 | UDP 传输：握手、打洞、中继、可靠控制流、视频发送/接收、统计 | `PeerLink`、`ReliableStream`、`VideoSender`、`VideoReceiver` |
| `stun.py` | 144 | 自实现 STUN Binding 子集（与媒体共用 socket） | `build_request/parse_mapped/query_stun` |
| `signal.py` | 88 | 信令客户端（TCP JSON Lines） | `SignalingClient` |
| `server.py` | 297 | 信令服务器 + UDP 中继 + STUN 响应 | `SignalingServer`、`Peer`、`Pair` |
| `session.py` | 407 | 会话层：抓屏循环、控制消息分发、输入注入、文件接收、剪贴板 | `HostSession`、`ViewerSession`、`VideoSettings` |
| `capture.py` | 192 | mss 抓屏 + JPEG 编码 + 自适应质量 + 静帧跳过 | `ScreenCapture`、`FramePacer` |
| `audio.py` | 646 | WASAPI loopback（ctypes COM）→ 重采样 → G.711 µ-law，静音抑制 | `LoopbackCapture`、`LoopbackSource`、`AudioPump`、`MonoResampler` |
| `wininput.py` | 327 | `SendInput` 键鼠注入、虚拟桌面归一化、剪贴板读写 | `InputInjector`、`ClipboardWatcher` |
| `websocket.py` | 167 | 最小 RFC6455 服务端（文本/二进制/分片/ping-pong） | `WSConnection` |
| `webui.py` | 335 | 本地 HTTP 静态资源 + WS 桥 + Hub 扇出（丢旧帧背压） | `Hub`、`ViewerHub`、`AgentHub`、`WebUI` |
| `web/*` | 1113 | 浏览器 UI（深色玻璃拟态 + 连接码页 + 悬浮工具栏 + HUD） | `viewer.js`、`agent.js`、`style.css` |
| `agent.py` / `viewer.py` | 419 | 两个角色的主编排（CLI → 信令 → PeerLink → 面板） | `AgentApp`、`ViewerApp` |
| `doctor.py` | 131 | 自检 | `run_checks()` |

## 3. 数据流

### 3.0 声音（agent → viewer）

```
AudioPump 线程 (20 ms 定拍)                事件循环                浏览器
  LoopbackCapture.read_raw()  ─┐
  MonoResampler → 320 采样      ├─ ulaw_encode → 320 B
  wire.pack_audio(seq, ts, 320, 当前帧, 上一帧)
      └──TYPE_AUDIO / STREAM_AUDIO──▶ PeerLink.send_audio （不重传，丢包靠冗余帧顶）
                                     └─ ViewerSession.on_audio → Hub.post_audio
                                          └─WS 二进制 "A" + AUDIO_HDR──▶ WebAudio 抖动缓冲
```

要点：
- **20 ms 一帧、128 kbit/s**，与视频同一条 UDP 通道但**不走**可靠流也不进 FEC 组——
  声音等不起重传，宁可丢一个 20 ms 断点。
- 每个报文里带**上一帧**（`prev_len`），单包丢失时接收端先补上一帧，听感上不出现爆音。
- **静音抑制**：连续 0.5 s 纯静音后只每 25 帧发一个心跳帧（约 2.5 kbit/s），桌面安静时不白烧带宽。
- 采集线程拥有自己的 COM 单元（WASAPI 要求），跨线程只经 `loop.call_soon_threadsafe`。
- 浏览器用 `AudioContext` 直接把 µ-law 展开成 `Float32Array`，无需 WASM 或第三方库。

### 3.1 视频（agent → viewer）

```
抓屏线程 (executor)           事件循环 (asyncio)                     浏览器
  ScreenCapture.grab() ─┐
  JPEG + FramePacer     │  HostSession._capture_loop
  (质量自适应/静帧跳过)  └──▶ PeerLink.send_video(jpeg, keyframe)
                             VideoSender.build() → DATA×n + PARITY×m
                             _send_packet() → header(20B) + AEAD(plen)
                             ──UDP──▶ viewer: PeerLink.datagram_received
                                      VideoReceiver.on_fragment/on_parity
                                      交付 → ViewerSession._on_frame
                                      ViewerHub.post_frame → WS 二进制帧("J"+JPEG)
                                      ──WS──▶ viewer.js: createImageBitmap → canvas
```

关键点：

- 抓屏在**线程池**里跑（`run_in_executor`），JPEG 编码也在那里，事件循环只做发包与拼帧，
  避免 22–28 ms 的 `BitBlt` 卡住控制通道。
- `FramePacer` 按"上一帧字节数 / 目标 140 KB"调整 JPEG 质量（32–92），
  上限受 `VideoSettings.quality` 约束；静帧时跳过发送（`skip_static`）。
- Web UI 的 Hub 有**背压丢帧**：浏览器来不及消费时只保留最新一帧，宁可跳帧不排队。

### 3.2 控制（双向）

```
浏览器键鼠事件 ──WS──▶ ViewerHub.on_json → 映射为 {t:"m"|"k"|"text"|"clip"|...}
   → ViewerSession.send() → PeerLink.send_control(obj)
   → ReliableStream.send(json) → DATA(stream=CONTROL, payload="<I"seq + json)
   ──UDP──▶ agent: ReliableStream.on_data → HostSession._dispatch
   → InputInjector.move_abs/button/key/type_text  或  set_clipboard_text
   → 回 ACK（累积 + 位图）
```

`HostSession._dispatch` 支持：`hello`、`settings`、`keyframe_req`、`mouse`、`key`、`text`、
`clip`、`file_begin|data|end`、`reboot`。输入坐标是**归一化帧坐标**，
`_map_to_virtual` 负责映射到虚拟桌面（多显示器时按 `SM_XVIRTUALSCREEN` 系列常量换算）。

### 3.3 状态机（`PeerLink`）

```
idle ──begin_handshake()──▶ punching ──收到对端 HELLO/PUNCH──▶ direct
                                  │                              │
                                  └─6s 未直连 或 --relay-only──▶ relay
                                                                 │
任何状态 ──close()/对端 BYE/RX 超时 25s──▶ closed
```

`_set_state` 只在状态变化或每 1 秒回调一次 `on_state`（早期实现 20 ms 一次，把 UI 刷爆了）。

## 4. 线程与调度

| 执行体 | 内容 |
| --- | --- |
| asyncio 主循环 | UDP 传输、信令、WS 桥、HTTP、控制分发 |
| 线程池（默认 executor） | 抓屏 + JPEG 编码（每次抓屏一拍） |
| `thread_capture()` | 每线程一个 `ScreenCapture` 实例（mss 不是线程安全的） |
| 定时任务 | 心跳 4s、ACK 刷新、NACK 扫描、stats 1s、RX 超时检查 25s |

## 5. 配置与运行期文件

- 配置目录：`%APPDATA%\peerdesk`（`config_dir()`）
- 接收文件：`%USERPROFILE%\Downloads\PeerDesk`（`download_dir()`）
- 日志：`--log` 指定的文件，或控制台；`-v` 开 DEBUG
- 会话参数（`VideoSettings`）：`fps`、`max_width`、`quality`、`monitor`、`skip_static`、`clipboard`、`pixel_format`

## 6. 实测性能（本机 2560×1440 / i7 级 CPU / 无独显加速路径）

| 场景 | 结果 |
| --- | --- |
| 抓屏（2560×1440，mss） | 22–28 ms |
| `Image.frombytes` | 10–11 ms |
| JPEG q60 编码 | 4.7 ms（首帧 57.9 ms 预热），产物 194 KB |
| 端到端（直连，1600×900，fps=20） | ~13.5–15.7 fps，6.6 Mbps，RTT 0.3–2 ms，单帧 70–80 KB |
| 端到端（强制中继） | ~14 fps，6.6 Mbps，功能等价 |
| 本机回环 Web 桥 | 1024×576 JPEG 26–28 KB/帧，控制通道 `pending=0` |
| 声音（直连，合成音） | 267 帧 / 5.3 s（20 ms/帧，0 丢帧），85 KB ≈ 128 kbit/s |
| 声音（强制中继） | 214 帧 / 4.3 s，68 KB ≈ 128 kbit/s，功能等价 |
| 声音（loopback 实测） | 48 kHz 立体声混音 → 16 kHz 单声道，1 s 抓满 16000 采样，rms 8293（真实扬声器输出） |

瓶颈是**抓屏 + JPEG 编码**（串行 30–40 ms），不是网络。若要用满 60 fps，需要
DXGI Desktop Duplication 或硬件 MJPEG 编码器（见路线图）。

## 7. 已知限制

1. MJPEG 无帧间压缩：同画质下码率约为 H.264 的 5–10 倍（本机 6.6 Mbps vs 同类 1–2 Mbps）。
2. 声音是 G.711 µ-law（8 bit/16 kHz），够"听得到、听得清人声"，但不是音乐级；Opus 需要额外依赖。
3. 无虚拟显示器/虚拟 HID 驱动：分辨率跟随真实桌面，部分反作弊会拦 `SendInput`。
4. 剪贴板只同步文本（图片/文件待补）。
5. 中继是单进程 UDP 转发，未做鉴权与限速（生产部署需要在外层加白名单/限流）。
6. 浏览器桥只监听 `127.0.0.1`，跨机访问需要 SSH 隧道或反代（有意为之）。
