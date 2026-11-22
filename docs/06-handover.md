# 交接手册（Handover）

> 给下一个接手的人（或者三个月后的自己）。目标：**10 分钟内跑起来，1 小时内敢改代码，半天内知道哪些坑已经踩过。**

最后更新：2026-11-22 · 版本 0.7.0 · 迁移目标：C++20 + Qt 6

> **迁移状态：后端已是 C++，前端正在用 Qt 重建。**
> 当前 `peerdesk-client` 有连接页与被控端页，Viewer 视频页尚未接入；下面的 Python
> 快速开始、代码地图和排障内容在删除 Python 之前仍可作为行为参考，但不再是新功能入口。
>
> 原生构建（PowerShell）：
>
> ```powershell
> $env:PATH = "C:\Qt\6.8.3\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;$env:PATH"
> cmake --preset windows-mingw
> cmake --build --preset windows-mingw
> ctest --preset windows-mingw --output-on-failure
> .\build\cpp\peerdesk-client.exe --capture test --page remote
> ```
>
> 原生代码地图：`src/core/`（协议/加密/FEC/STUN）、`src/server/`（信令与中继）、
> `src/transport/`（PDTP 链路）、`src/session/`（会话与文件）、`src/platform/`
> （Windows 抓屏/输入/剪贴板/WASAPI）、`src/app/`（控制器/偏好/音频输出）、
> `src/ui/`（Qt 页面与控件）、`apps/`（可执行入口）、`tests/`（CTest）。

---

## 1. 这是什么

**PeerDesk（对端桌面）**：纯 Python 的 Windows 远程桌面，自研 UDP 传输协议 PDTP/1。
立项原因是网易UU远程把「安全中继」做成了会员权益；PeerDesk 的立场是
**打洞优先、中继永远免费兜底、服务端可自托管、无账号无计费**。

| 项 | 值 |
| --- | --- |
| 仓库 | https://github.com/Bad0RANG3/peerdesk （PUBLIC，默认分支 `main`，Apache-2.0） |
| 本地路径 | `D:\UU\peerdesk` |
| 版本 | `peerdesk/__init__.py` → `__version__ = "0.6.0"`（文件传输 0.2，声音 0.3，鉴权/限速 0.4，打包/GDI 0.5，截图与合成源 0.5.1，DXGI 抓屏 0.6） |
| 代码量 | 约 7900 行（`peerdesk/*.py` 6407 + `peerdesk/web/` 1516，加 `tests/` 与文档） |
| 依赖 | Pillow / numpy / cryptography / mss；**没有** npm / Electron / Qt / ffmpeg / WebRTC 库 |
| 平台 | Windows 10/11 被控端与控制端（控制端只需浏览器） |
| 测试 | 150 个单测（含 `test_dxgi.py` 16 个）+ 4 个端到端（直连/中继/浏览器桥/真 exe）+ 3 条负路径（`--token`/`--bad-token`/`--relay-mbps`） |

三端模型：

```
被控端 agent ──TCP 信令──▶ server ◀──TCP 信令── 控制端 viewer ──HTTP/WS──▶ 浏览器
        └────────── PDTP/1 over UDP（打洞，失败自动升级中继）──────────┘
```

---

## 2. 十分钟跑起来

```bash
cd D:\UU\peerdesk
python -m pip install -e . --no-deps     # 依赖已在本机：Pillow 13 / numpy 2.5 / cryptography 50 / mss 10.2
python -m peerdesk doctor                # 自检：依赖、显示器、输入注入、声音采集、AEAD、UDP、STUN
```

三个终端：

```bash
python -m peerdesk server --port 7900 --udp-port 7901
python -m peerdesk agent  --server 127.0.0.1:7900 --code 604173829 --web-port 7701 --fps 20 --width 1600
python -m peerdesk viewer --server 127.0.0.1:7900 --web-port 7702
```

浏览器开 `http://127.0.0.1:7702/`，输入 `604173829` → 应看到桌面 + HUD 有 `P2P / 分辨率 / fps / Mbps / ms`。
被控端面板 `http://127.0.0.1:7701/` 显示连接码、分辨率/帧率/画质、日志、以及「发送文件到控制端」。

自测命令（**改完代码必跑**）：

```bash
python -m unittest discover -s tests -p "test_*.py"   # 150 tests，约 1.3s
python tests/e2e_local.py                            # 直连：约 85–100 帧 + 约 300–320 个声音包/6s
python tests/e2e_local.py --relay                    # 强制中继（声音同样要过）
python tests/e2e_web.py                              # 页面+WS+JPEG+声音+"A" 二进制+上传+下载+/dl
python -m ruff check peerdesk tests                   # CI 会跑（advisory）
```

真机联调时后台三件套（历史上常用的端口）：

```bash
python -m peerdesk server --port 7930 --udp-port 7931   # 日志 logs/server.log
python -m peerdesk agent  --server 127.0.0.1:7930 --code 604173829 --web-port 7701
python -m peerdesk viewer --web-port 7702 --server 127.0.0.1:7930
```

---

## 3. 代码地图（按修改频率排序）

| 文件 | 行数 | 什么时候要看它 |
| --- | --- | --- |
| `peerdesk/transport.py` | 876 | **90% 的 bug 在这里**：握手、打洞、中继、可靠流、FEC、统计 |
| `peerdesk/session.py` | 642 | 抓屏循环、输入分发、剪贴板、双向文件传输、声音起停、带宽自适应 |
| `peerdesk/audio.py` | 640 | WASAPI loopback（ctypes COM）、重采样、µ-law、`AudioPump` 定拍 |
| `peerdesk/web/viewer.js` | 578 | 控制端交互：绘制、鼠标键盘、画质菜单、拖拽上传、WebAudio 播放 |
| `peerdesk/web/style.css` | 499 | 视觉规范（token 在文件顶部 `:root`） |
| `peerdesk/webui.py` | 393 | 本地 HTTP + WS 桥、Hub 背压丢帧、sticky 重放、`/dl` 下载、声音旁路广播 |
| `peerdesk/wininput.py` | 353 | `SendInput` 注入、虚拟桌面坐标、剪贴板监视 |
| `peerdesk/server.py` | 446 | 信令 + UDP 中继（`RLY1` 头 + 配对 tag）+ STUN + 令牌/频次/限速 |
| `peerdesk/wire.py` | 268 | 20 字节头、分片、parity、ACK/NACK 载荷、`AUDIO_HDR` |
| `peerdesk/agent.py` / `viewer.py` | 277 / 206 | 角色编排、信令交互、CLI |
| `peerdesk/crypto.py` | 214 | X25519 → HKDF → AEAD、重放窗口 |
| `peerdesk/dxgi.py` | 736 | **DXGI Desktop Duplication**：D3D11 设备/输出、staging texture、`RowPitch` 整理、子进程探测、access-lost 重试 |
| `peerdesk/capture.py` | 606 | **抓屏调度**（auto：DXGI → GDI → mss）、GDI 持久 DIB+StretchBlt、`TestPattern`、JPEG、自适应质量 |
| `peerdesk/websocket.py` | 166 | 手写 RFC6455 服务端（无扩展、8MB 上限） |
| `peerdesk/stun.py` / `util.py` / `doctor.py` / `signal.py` | 143 / 137 / 140 / 94 | STUN 子集、连接码与路径、自检（含声音）、信令客户端 |

### 关键数据结构与入口

- `PeerLink`（transport.py）：`start(port, host)` / `set_candidates()` / `begin_handshake()` /
  `offer_relay()` / `discover_reflexive()` / `send_control(obj)` / `send_video(jpeg, keyframe)`。
  状态机 `idle → punching → direct|relay → connected|closed`。
- `ReliableStream`：控制流。`send(payload)` 返回 seq；`on_ack(cum, bitmap)` 累积确认；RTO 0.15→1.2s，最多重传 14 次。
- `ControlFragments`：**>1200 字节的控制消息自动切片**（否则靠 IP 分片，一丢全丢）。首字节 `0xF1`，头 `<B4sHH`。
- `VideoSender` / `VideoReceiver`：`FEC_K=8`，每 8 片算 1 个 parity；`VideoReceiver.tick()` 450ms 超时放弃并请求关键帧。
- `HostSession`：`_capture_loop`（run_in_executor 抓屏）、`_dispatch`（控制消息路由）、`send_file(path)`。
- `ViewerSession`：`_video`（帧上行）、`begin_upload/upload_chunk/end_upload`（浏览器拖拽上传）、
  `_rx_begin/_rx_data/_rx_end`（接收被控端推来的文件 → `/dl/<name>`）。
- `WebUI` + `Hub`：浏览器一轮一连接；`post_frame` 由抓屏线程调用（线程安全，背压时丢旧帧）。
- `audio`：`LoopbackCapture`（IMMDeviceEnumerator → IAudioClient loopback → IAudioCaptureClient）→
  `MonoResampler` → `AudioPump`（20 ms 定拍）→ `wire.pack_audio` → `PeerLink.send_audio`；
  接收侧 `ViewerSession.on_audio` → `Hub.post_audio`（**不走** busy/pending 帧通道）→ `b"A"` 二进制。

---

## 4. 协议速查（改协议前先读 `docs/03-protocol-pdtp1.md`）

**头 20 字节** `<2sBBIIBBHI` = magic `PD`、ver、type、seq、ts_ms、stream、flags、plen、session。

- `ts_ms` 是 **uint32 会话相对毫秒**，不能塞 epoch 毫秒（会 `struct.error`）。
- 包 `seq` = AEAD nonce 计数器，**必须严格单调**；控制流的消息序号在 payload 前 4 字节 `<I` 里，两者不要混。
- 类型 1–12：hello / hello_ack / hello_fin / punch / punch_ack / data / parity / ack / nack / ping / pong / bye。
- 密钥：X25519 → HKDF-SHA256(salt = SHA256(transcript‖psk))，psk = `SHA256("peerdesk-psk" ‖ code)`；
  nonce = `<IQ>`(direction, seq)，direction host=0 / viewer=1；AAD = 20 字节头。
- 握手 transcript 绑定双方 hello **原始字节** → hello 报文必须缓存复用（重发不能重新构造，见 §5 坑 1）。
- 打洞：候选交换后每 100ms 发 PUNCH；`RELAY_ESCALATE_AFTER=6s` 未通即升级；中继帧 = `RLY1` + `>4sI`(magic, tag)。
- 强制中继：host 回 HELLO_ACK 后立即发加密 PING；双方未 `established` 时每 1s 重发，
  避免首个探测包被 server 丢弃后永久停在 `punching`。
- 信令（TCP JSON Lines）：`register{code,name,cands}` → `registered{code,relay,server_time}`；
  `connect{code}` → 对端收 `viewer{}` / `peer{}`；之后 `cands` / `relay_hello` / `bye` / `ping` 互转。

---

## 5. 已经踩过的坑（别再踩）

1. **握手报文必须缓存**：`_send_hello` 每次重建 body（含 ts）会让两侧 transcript 不同 → 密钥不同 → 静默丢包。
2. **控制流序号不能塞进包头 seq**：那样会复用 AEAD nonce、触发重放窗口，ACK 全丢、控制通道永不确认。
3. **`FragmentAssembler.fill_parity` 要把 parity 自己算进去**：`missing = parity ^ (其他所有片)`。
4. **`ReliableStream.on_ack` 是累积语义**：`ack_cum` 之前的全部清掉，再按位图补洞；只 pop 自己会导致 pending 永不归零。
5. **STUN `parse_mapped` 必须校验 family 字节**，否则坏包会被解析成「合法但错误」的地址。
6. **WS 握手不要读第二次请求头**：HTTP 分支已 `readuntil`，要传 `request=` 给 `WSConnection`；且 WS 分支处理完必须 `return`。
7. **canvas 尺寸别用 `clientWidth`**：父容器未布局时会算出 0，画面缩成左上角一小块。用 `window.innerWidth * dpr`。CSS 上 `#screen` 用 `position:fixed; inset:0`。
8. **控制消息 > MTU 必须自己分片**（`ControlFragments`）。文件传输 24KB base64 一次发，靠 IP 分片在真实网络必然出事。
9. **统计 fps/mbps 要双边累计**，否则 viewer 侧永远显示 0。
10. **`KeyPair.public_bytes` 是字段不是方法**；`CryptoContext.seal()` 只返回密文（不含 20 字节头）。
11. **WASAPI 的 COM 单元是线程私有的**：`LoopbackCapture` 必须在 `AudioPump` 线程里 `open`，
    主线程只准碰 `stop()`；否则 `CoInitializeEx` 返回 `RPC_E_CHANGED_MODE` 或拿到空指针。vtable 序号
    全靠手数（`IAudioClient::GetMixFormat=8`、`Start=10`、`GetService=14`），数错就是 `0xC0000005`。
12. **声音不能走 `Hub` 的帧通道**：那条通道有 busy/pending 背压，会**丢**数据；视频丢一帧无所谓，
    声音丢一片就是爆音。声音走独立的 `broadcast_audio`，和视频并发写 WS（`WSConnection._send_frame`
    内部有 `asyncio.Lock`，安全）。
13. **µ-law 表要按"取反位"写**：`u = ~i & 0xFF` 之后再拆符号/指数/尾数，0 必须映射成 `0xFF`
    （否则静音判断和 `audioop` 对不上）。
14. **DXGI 结构体 ABI 必须完全匹配**：`DXGI_MODE_DESC` 末尾有 `Scaling`，漏掉会让
    28 字节变 24 字节；`DXGI_OUTDUPL_DESC` 随之从 36 变 32 字节，`GetDesc` 实际写 36 字节，
    越界 4 字节破坏相邻 COM 对象。症状不是立刻报错，而是**首轮抓屏正常、第二轮 `close()`
    access violation**（`0xC0000005`）。`DXGI_RATIONAL` 是两个 `UINT`。必须保留
    `DuplicateOutput=22`、`GetDesc=7`、`AcquireNextFrame=8`、`ReleaseFrame=14`。
    staging texture 的 `RowPitch` 可能大于 `width×4`，要用 `_pack_rows` 去 padding；
    `tests/test_dxgi.py` 已固定这些大小/offset 与 vtable 序号。
15. **`TokenBucket.last` 初值必须是 `0.0`**，不能用 `time.monotonic()`：单测会注入 `now=0.0`
    来模拟时钟，真实时钟的初值会让"补充令牌"那段永远不执行，测试里一路 `False`。
16. **`attempts` 只清过期项挡不住海量新 IP**（每个 IP 一条、都还没过期），必须再加 8192 条硬顶，
    超了丢最早的一批；否则一个扫描器就能把服务端内存顶爆。
17. **GDI 缩放要用 `StretchBlt` 而不是 Pillow**：`StretchBlt` + `SetStretchBltMode`
    （COLORONCOLOR=3 最快 / HALFTONE=4 平滑但慢 11 ms）在驱动里做缩放，
    比"全尺寸 BitBlt 后再 `img.resize`"快 10–15 ms；DIB 高度必须用负数才是自上而下。
18. **`PEERDESK_SCALE=pil` 时别让 GDI 也缩放**：两边都缩会二次失真，
    `grab_rgb(mon, 0)` 传 0 表示不缩放、交给 Pillow。
19. **限速要"先降质量、再降帧率"**：直接砍帧率会让画面变幻灯片但单帧依旧超上限；
    正确顺序是 `set_budget_kb(每帧预算×0.75)` 让 `FramePacer` 压质量，仍超再用
    `_effective_fps` 按实测帧大小降帧率。实测 1.5 Mbps 上限：1 帧/6 s → 4 fps / 44 KB。
20. **剪贴板 API 必须声明 `restype`**：`GetClipboardData`/`GlobalLock`/`GlobalAlloc`/`SetClipboardData`
    不声明的话 ctypes 按 `c_int` 返回，64 位进程里句柄被截成低 32 位，
    `GlobalLock(截断句柄)` 直接 `access violation`（`0xFFFFFFFF8CFB75D0` 这种地址）。
    曾经的真实现象：**控制端一连上，被控端采集循环就崩，画面全黑、HUD 全是 `—`**。
    见 `peerdesk/wininput.py` 顶部那段 `argtypes/restype` 和 `tests/test_clipboard.py`。
21. **采集循环里的"周边设施"必须各自 try/except**：`_lazy_input()`（输入注入 + 剪贴板监视）
    和 `_poll_clipboard()` 以前裸奔在 `_capture_loop` 里，任何异常都会把整个 task 带走，
    而 `create_task` 的异常只打一行 `Task exception was never retrieved`，界面上就是"连上了但没画面"。
    现在失败一次就置 `_input_failed` 并继续推画面。
22. **DXGI 必须先在子进程里探测**：即使结构体修对，坏驱动仍可能让 `DuplicateOutput`
    直接杀进程。`probe_dxgi()` 用隐藏命令 `_dxgi_probe` 在子进程跑完整
    创建/输出枚举/抓一帧/关闭流程，10 秒超时并缓存结果；`PEERDESK_DXGI_PROBE=0`
    可禁用。`auto` 探测失败回退 GDI，显式 `--capture dxgi` 则报错，不能静默降级。
23. **强制中继时 host 不能等 viewer 先发探针**：viewer 的 HELLO 可能比 agent HELLO
    更早到 server，server 尚无 agent UDP 地址，会丢掉 HELLO 与首个加密 PING。
    host 派生密钥并回 ACK 后必须立即 `_send_ping()`；双方在 `established` 前每
    `HANDSHAKE_PING_INTERVAL=1.0s` 重发加密 PING，否则单包丢失就让双方永久停在 `punching`。

---

## 6. 已知限制（对方会问）

| 限制 | 说明 | 出路 |
| --- | --- | --- |
| 抓屏 10–12 ms/帧（DXGI）/ 18–23 ms（GDI） | 瓶颈是整帧抓屏拷贝 + JPEG，不是网络；DXGI 已约为 GDI 一半，但尚未做脏矩形/GPU 共享纹理 | 0.7 脏矩形 + 共享纹理；H.264 |
| DXGI 显示器拓扑只在启动时枚举 | 运行中修改分辨率、热插拔或扩展坞切换后，`monitors()` 仍返回旧快照，可能选不到新显示器或帧尺寸不一致 | 重启 agent；0.7 在拓扑变化后重建输出列表，且不破坏当前 duplication |
| DXGI 仅枚举默认 D3D11 适配器 | 多 GPU 机器上挂在其他适配器上的显示器可能缺失，DXGI 列表少于 GDI | 0.7 跨适配器枚举，或在数量不一致时降级/合并 |
| MJPEG 码率高 | 1600×900 约 6.6 Mbps | 0.7 上 H.264 |
| 声音是 G.711 µ-law | 人声清楚、音乐发毛，128 kbit/s | 0.8 评估 Opus 双栈 |
| 无虚拟显示器/虚拟 HID | 分辨率受真实桌面限制；个别游戏拦 `SendInput` | 0.8 / 0.9（需驱动签名） |
| 中继限速是开环的 | 令牌桶 + 被控端"质量→帧率"节流，没有 RTT/丢包反馈 | 0.8+ 考虑 GCC/BBR 式拥塞控制 |
| 令牌走明文 TCP | 信令没有 TLS，令牌可被同链路嗅探（媒体本身仍是 AEAD） | 反代/WSS 或加 Noise 握手 |
| 面板只监听 127.0.0.1 | 跨机访问要 SSH 隧道/反代 | 设计如此，别改默认 |
| 连接码即 PSK | 知道 9 位码即可连（同类产品同一模型） | 服务端只存 `code_id`；要更强就加确认弹窗 |

> 低风险防御项：`Map` 成功但 `pData == NULL` 时当前直接抛错，未调用 `Unmap`；
> 正常驱动不会触发，后续修复资源清理时一并补上。

---

## 7. 故障排查

| 现象 | 先看什么 |
| --- | --- |
| 一直「正在打洞…」 | `agent -v` 日志里候选列表；对称 NAT 下正常会 6s 后升中继；用 `--relay-only` 验证中继链路是否本身可用 |
| 「连接已断开」 | 双方 `link state`；`RX_TIMEOUT=25s` 无包即断；检查 UDP 端口是否被防火墙拦 |
| 画面卡在某一帧 | HUD 的 fps=0 说明没帧到；看 host 的 `enc_ms`；`keyframe_req` 是否被处理 |
| 键鼠不动 | 被控端面板「允许键鼠控制」；`--no-input`；UAC 安全桌面注入不进去（系统限制） |
| 上传/下载失败 | 看 `logs` 里 `upload_chunk`/`_rx_data` 异常；`/dl/<文件名>` 只允许下载目录内的单层文件名 |
| 中文乱码（控制台） | `__main__._force_utf8_console()` 已处理；自己写脚本输出中文要 `sys.stdout.reconfigure(encoding="utf-8")` |
| CI 红 | Lint 与 e2e 是 advisory；真红通常是单测。e2e 在 GitHub runner 上可能因无交互桌面抓屏失败（已知） |
| pip 装依赖极慢 | 本机 pip 出网约 20–40 KB/s，能 `--no-deps` 就 `--no-deps`；本机依赖已齐 |

日志位置：`D:\UU\peerdesk\logs\{server,agent,viewer}.log`；调试脚本在 `D:\UU\peerdesk\scratch\`（gitignore，别提交）。
逆向产物：`D:\UU\re_work\{gv_*.txt, cjk_*.txt, extract_*.py}`。

---

## 8. 发布流程

```bash
cd D:\UU\peerdesk
python -m ruff check peerdesk tests
python -m unittest discover -s tests -p "test_*.py"
python tests/e2e_local.py && python tests/e2e_local.py --relay && python tests/e2e_web.py
python packaging/build.py --clean                       # 打包
dist\peerdesk.exe _dxgi_probe                           # frozen exe 的 DXGI 探测
python tests/e2e_exe.py                                 # 真 exe 三进程互连
git add -A && git commit -m "feat: ..." && git push origin main
gh run watch     # 或 gh run list --limit 3
```

版本号到处要改的地方：`peerdesk/__init__.py` 的 `__version__`、`pyproject.toml` 的 `version`、`README` 徽章描述（如涉及）。

### 本次交接验证（2026-11-22）

- `python -m unittest discover -s tests -p "test_*.py"`：150 项通过（约 1.3s）；
  `python -m ruff check peerdesk tests` 通过。
- 源码 e2e：`e2e_local.py` 直连 87 帧 / 316 声音帧，`--relay` 90 帧 / 317 声音帧；
  `e2e_web.py` 的页面、WS、JPEG、声音、上传/下载全部 PASS。
- 冻结包：`packaging/build.py --clean` 成功（约 33 MB）；`peerdesk.exe doctor` 全项通过，
  `_dxgi_probe` 返回 `{"ok": true, "width": 2560, "height": 1440, "elapsed_ms": 129.2}`。
- `e2e_exe.py --seconds 9` 连续 3 次 PASS（relay）：视频 195 / 190 / 174 帧，
  声音 437 / 435 / 435 帧，结束后无残留进程。

---

## 9. 下一步优先级（与 `docs/05-roadmap.md` 一致）

1. **真跨机验证**（回环测不出 NAT/丢包；需要一个公网 server + 两台机器）
2. **DXGI 增量优化**：读脏矩形元数据只编码变化区域；评估 GPU 共享纹理，避免整帧
   `CopyResource`/`Map` 的 CPU 往返
3. **H.264 / Opus**（码率与音质；都要外部动态库，做成"加载得到就用"的插件层）
4. 独立文件流（当前文件走控制通道，会跟键鼠抢带宽）、远程 Shell、Wake-on-LAN
5. 基于 RTT/丢包的拥塞控制（现在只有"上限→质量→帧率"的开环节流）

已完成：DXGI Desktop Duplication（10–12 ms/帧）、子进程探测与 access-lost 回退、
GDI 抓屏后端（37 ms → 22 ms 单帧）、中继 `--token` 鉴权、`--connect-max` 连接频次、
`--relay-mbps` 令牌桶限速、被控端带宽自适应、PyInstaller 单文件 exe +
`tests/e2e_exe.py` 黑盒验证。

明确不做：账号体系、会员、时长计费、把中继当付费权益。

---

## 10. 交接清单（Checklist）

- [x] 仓库公开、README/文档/许可证齐全、CI 通过
- [x] 三端可跑、直连与中继都验证过、双向文件传输可用
- [x] 单测 150 个、端到端 4 套（直连/中继/浏览器桥/真 exe，均验证声音）、`doctor` 自检
- [x] 服务端 `--token` / `--connect-max` / `--relay-mbps` 与负路径 e2e（错误令牌必须被拒）
- [x] 声音通道：WASAPI loopback → G.711 µ-law → WebAudio，直连/中继/浏览器桥全部实测
- [x] 逆向报告留存（`docs/01`），证据来自本机安装目录 strings + 运行期抓包，不含官方代码/资源
- [ ] 真跨机（非回环）验证：需要两台机器 + 一个有公网 IP 或端口映射的 server
- [x] DXGI 后端：子进程探测、DXGI/GDI/mss 回退、显式 `dxgi` 失败报错、连续多轮创建/关闭
- [x] 打包成 exe：`packaging/*.spec|build.py` → `dist/peerdesk.exe`（33 MB，四端合一），
      `tests/e2e_exe.py` 用真 exe 跑通 relay + 实时视频/声音
- [x] 截图更新（0.5.1 重截：合成信号源 + HUD + 连接码门页 + 被控端面板）

### 重截文档截图（不会泄露桌面）

```bash
python scratch/shot_docs.py     # 起 server/agent(--capture test)/viewer + 两个面板，CDP 截图
```

- 画面来自 `--capture test` 的合成源，**不读真实屏幕**；以前用真桌面截图，把聊天窗口拍进了公开仓库。
- 截图走 `scratch/shot_cdp.py`（自己实现的最小 RFC6455 + CDP `Page.captureScreenshot`）。
  `chrome --headless --screenshot` 抓不到画布：要么 load 事件时就截（黑的），要么用
  `--virtual-time-budget` 快进定时器（`createImageBitmap` 来不及解码，还是黑的）。
- viewer 支持 URL 直达：`?code=604173829&server=127.0.0.1:7900&auto=1&hud=1`
  （`auto=0` 只填不连；`hud=1` 打开 HUD），截图脚本就靠它免点击。

### 打包成 exe（怎么打、为什么这么打）

```bash
python -m pip install pyinstaller
python packaging/build.py --clean      # → dist/peerdesk.exe
dist/peerdesk.exe doctor               # 自检（抓屏/声音 probe 都会跑到）
dist/peerdesk.exe _dxgi_probe          # 隐藏命令：验证 frozen exe 能加载 peerdesk.dxgi
python tests/e2e_exe.py                # 黑盒：server+agent+viewer 三个 exe 互连
```

几条不能改的地方：
- 前端资源必须按 `peerdesk/web` 的相对路径进包（`peerdesk.spec` 里 `datas` 的 dest），
  因为 `webui.WEB_DIR = Path(__file__).parent / "web"`；打漏了就是"页面只有 web assets missing"。
- `excludes` 里去掉了 `tkinter`/`unittest`，体积从 ~40 MB 降到 33 MB。
- `upx=False`：UPX 会让每次启动多解压一遍，启动时间从 ~1 s 涨到 3 s 以上。
- `tests/e2e_exe.py` 刻意用 `--relay-only`：本机虚拟网卡多（WireGuard/Hyper-V/GameViewer IDD），
  打洞候选经常挑到不通的那张，打包冒烟不该被 NAT 行为干扰。

> 交接联系人：Bad0RANG3 · 有任何与协议相关的疑问，先读 `docs/03-protocol-pdtp1.md`，再看 `peerdesk/transport.py` 顶部的常量区。
