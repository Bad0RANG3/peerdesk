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
| `wire.py` | 268 | 报文格式、分片、XOR FEC、ACK/NACK 位图 | `Header`、`FragmentAssembler`、`ParityGroup` |
| `crypto.py` | 214 | X25519 → HKDF → ChaCha20-Poly1305、重放窗口 | `KeyPair`、`SessionKeys`、`CryptoContext` |
| `transport.py` | 875 | UDP 传输：握手、打洞、中继、可靠控制流、视频发送/接收、统计 | `PeerLink`、`ReliableStream`、`VideoSender`、`VideoReceiver` |
| `stun.py` | 143 | 自实现 STUN Binding 子集（与媒体共用 socket） | `build_request/parse_mapped/query_stun` |
| `signal.py` | 94 | 信令客户端（TCP JSON Lines） | `SignalingClient` |
| `server.py` | 446 | 信令服务器 + UDP 中继 + STUN 响应 | `SignalingServer`、`Peer`、`Pair` |
| `session.py` | 624 | 会话层：抓屏循环、控制消息分发、输入注入、文件接收、剪贴板 | `HostSession`、`ViewerSession`、`VideoSettings` |
| `capture.py` | 320 | GDI 持久 DIB + `BitBlt`/`StretchBlt` 抓屏（回退 mss）+ JPEG 编码 + 自适应质量 + 静帧跳过 | `ScreenCapture`、`GdiCapture`、`FramePacer` |
| `audio.py` | 640 | WASAPI loopback（ctypes COM）→ 重采样 → G.711 µ-law，静音抑制 | `LoopbackCapture`、`LoopbackSource`、`AudioPump`、`MonoResampler` |
| `wininput.py` | 327 | `SendInput` 键鼠注入、虚拟桌面归一化、剪贴板读写 | `InputInjector`、`ClipboardWatcher` |
| `websocket.py` | 166 | 最小 RFC6455 服务端（文本/二进制/分片/ping-pong） | `WSConnection` |
| `webui.py` | 393 | 本地 HTTP 静态资源 + WS 桥 + Hub 扇出（丢旧帧背压） | `Hub`、`ViewerHub`、`AgentHub`、`WebUI` |
| `web/*` | 1482 | 浏览器 UI（深色玻璃拟态 + 连接码页 + 悬浮工具栏 + HUD） | `viewer.js`、`agent.js`、`style.css` |
| `agent.py` / `viewer.py` | 419 | 两个角色的主编排（CLI → 信令 → PeerLink → 面板） | `AgentApp`、`ViewerApp` |
| `doctor.py` | 139 | 自检 | `run_checks()` |

服务端策略对象：`SignalingServer` 持有 `token` / `relay_mbps` / `connect_max` / `connect_window`；
`TokenBucket` 是每对会话的中继令牌桶（`rate = mbps × 125000` B/s，1 s 突发），
`Pair.bucket` 在转发前 `allow(len(data))`，不通过就丢包。被控端侧对应
`HostSession.set_rate_limit()` + `FramePacer.set_budget_kb()` + `_effective_fps()`。

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

### 3.0.1 抓屏后端（GDI vs mss）

```
GdiCapture（默认）                        mss（回退）
  GetDC(NULL) 常驻                       每次 grab 新建上下文
  CreateDIBSection 分配一次，跨帧复用      内部同样 BitBlt 到自建 DIB
  BitBlt(SRCCOPY|CAPTUREBLT) → 全尺寸     ── 结构与 GDI 相同，但多一层封装/拷贝
  StretchBlt + SetStretchBltMode → 目标尺寸（GDI 里缩放，省掉 Pillow resize）
  ctypes.string_at → Image.frombytes("RGB", ..., "raw", "BGRX")
```

- 缩放模式 `PEERDESK_SCALE`（或 `--scale-mode`）：`fast`（COLORONCOLOR，最快）、
  `halftone`（平滑，慢约 11 ms）、`pil`（把缩放交回 Pillow，最清晰但最慢）。
- 后端选择 `PEERDESK_CAPTURE`（或 `--capture`）：`auto` 先 GDI 再回退 mss；`test` 走
  `TestPattern` 合成信号源（渐变底 + 网格 + 时钟 + 移动方块），**完全不碰真实屏幕**，
  文档截图和 CI 都用它，避免把桌面内容拍进公开仓库。
- 实测（2560×1440 → 1600×900，JPEG q60，含 `Image.frombytes`）：GDI 22.7 ms，mss 36.1 ms。

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
| 抓屏（2560×1440 全尺寸） | BitBlt(DIB) 18.2 ms · mss 18.1 ms（两者都在驱动层花同样的时间） |
| 抓屏 + 缩放（GDI `StretchBlt`，1600×900） | 12–14 ms（缩放发生在显存/驱动路径，不进 Python） |
| 抓屏 + 缩放 + `frombytes` + JPEG q60 | **GDI 22.7 ms** vs mss 36.1 ms |
| `Image.frombytes` | 3.2 ms（只做 BGRX→RGB 重排） |
| 端到端（直连，1600×900，fps=20） | ~13–16 fps，6.5 Mbps，RTT 0.4–2.6 ms，单帧 86–103 KB，`enc_ms` 23–24（mss 后端 37） |
| 端到端（强制中继） | ~14 fps，6.6 Mbps，功能等价 |
| 本机回环 Web 桥 | 1024×576 JPEG 26–28 KB/帧，控制通道 `pending=0` |
| 声音（直连，合成音） | 267 帧 / 5.3 s（20 ms/帧，0 丢帧），85 KB ≈ 128 kbit/s |
| 声音（强制中继） | 214 帧 / 4.3 s，68 KB ≈ 128 kbit/s，功能等价 |
| 声音（loopback 实测） | 48 kHz 立体声混音 → 16 kHz 单声道，1 s 抓满 16000 采样，rms 8293（真实扬声器输出） |

瓶颈仍是**抓屏 + JPEG 编码**（串行 22–24 ms），不是网络。GDI 后端已经把 Python 侧的
缩放/重排压到最小，再往下只剩 DXGI Desktop Duplication（脏矩形 + GPU 共享纹理）
或硬件 MJPEG 编码器（见路线图）。

## 7. 已知限制

1. MJPEG 无帧间压缩：同画质下码率约为 H.264 的 5–10 倍（本机 6.6 Mbps vs 同类 1–2 Mbps）。
2. 声音是 G.711 µ-law（8 bit/16 kHz），够"听得到、听得清人声"，但不是音乐级；Opus 需要额外依赖。
3. 无虚拟显示器/虚拟 HID 驱动：分辨率跟随真实桌面，部分反作弊会拦 `SendInput`。
4. 剪贴板只同步文本（图片/文件待补）。
   限速是"先降质量再降帧率"的粗粒度控制，没有基于 RTT/丢包的拥塞控制（无 BBR/GCC）。
5. 中继是单进程 UDP 转发：已有 `--token` 鉴权、`--relay-mbps` 令牌桶限速与
   `--connect-max` 连接频次限制，但仍是单机单进程，没有分布式配额与计费，
   也没有 TLS——信令靠连接码派生 PSK 保护媒体，令牌本身走明文 TCP。
6. 浏览器桥只监听 `127.0.0.1`，跨机访问需要 SSH 隧道或反代（有意为之）。
