# 交接手册（Handover）

> 给下一个接手的人（或者三个月后的自己）。目标：**10 分钟内跑起来，1 小时内敢改代码，半天内知道哪些坑已经踩过。**

最后更新：2026-11-21 · 版本 0.3.0 · 提交见 `git log -1`

---

## 1. 这是什么

**PeerDesk（对端桌面）**：纯 Python 的 Windows 远程桌面，自研 UDP 传输协议 PDTP/1。
立项原因是网易UU远程把「安全中继」做成了会员权益；PeerDesk 的立场是
**打洞优先、中继永远免费兜底、服务端可自托管、无账号无计费**。

| 项 | 值 |
| --- | --- |
| 仓库 | https://github.com/Bad0RANG3/peerdesk （PUBLIC，默认分支 `main`，Apache-2.0） |
| 本地路径 | `D:\UU\peerdesk` |
| 版本 | `peerdesk/__init__.py` → `__version__ = "0.3.0"`（文件传输起于 0.2，声音起于 0.3） |
| 代码量 | 约 7200 行（`peerdesk/` 6431 含 `web/` 1464，加 `tests/` 与文档） |
| 依赖 | Pillow / numpy / cryptography / mss；**没有** npm / Electron / Qt / ffmpeg / WebRTC 库 |
| 平台 | Windows 10/11 被控端与控制端（控制端只需浏览器） |
| 测试 | 99 个单测 + 3 个端到端（直连/中继/浏览器桥，均覆盖声音、双向文件传输与 /dl 下载） |

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
python -m unittest discover -s tests -p "test_*.py"   # 99 tests，约 0.5s
python tests/e2e_local.py                            # 直连：约 90 帧 + 267 个声音包/5s
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
| `peerdesk/transport.py` | 875 | **90% 的 bug 在这里**：握手、打洞、中继、可靠流、FEC、统计 |
| `peerdesk/session.py` | 598 | 抓屏循环、输入分发、剪贴板、双向文件传输、声音起停 |
| `peerdesk/audio.py` | 640 | WASAPI loopback（ctypes COM）、重采样、µ-law、`AudioPump` 定拍 |
| `peerdesk/web/viewer.js` | 551 | 控制端交互：绘制、鼠标键盘、画质菜单、拖拽上传、WebAudio 播放 |
| `peerdesk/web/style.css` | 499 | 视觉规范（token 在文件顶部 `:root`） |
| `peerdesk/webui.py` | 393 | 本地 HTTP + WS 桥、Hub 背压丢帧、sticky 重放、`/dl` 下载、声音旁路广播 |
| `peerdesk/wininput.py` | 327 | `SendInput` 注入、虚拟桌面坐标、剪贴板监视 |
| `peerdesk/server.py` | 296 | 信令 + UDP 中继（`RLY1` 头 + 配对 tag）+ STUN |
| `peerdesk/wire.py` | 268 | 20 字节头、分片、parity、ACK/NACK 载荷、`AUDIO_HDR` |
| `peerdesk/agent.py` / `viewer.py` | 238 / 189 | 角色编排、信令交互、CLI |
| `peerdesk/crypto.py` | 214 | X25519 → HKDF → AEAD、重放窗口 |
| `peerdesk/capture.py` | 192 | mss 抓屏、JPEG、自适应质量、静帧跳过 |
| `peerdesk/websocket.py` | 166 | 手写 RFC6455 服务端（无扩展、8MB 上限） |
| `peerdesk/stun.py` / `util.py` / `doctor.py` / `signal.py` | 143 / 137 / 139 / 88 | STUN 子集、连接码与路径、自检（含声音）、信令客户端 |

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
14. **DXGI `DuplicateOutput` 本机会崩整个进程**（`0xC0000005`，读 `0xFFFF...FF`）。
    想上 DXGI 必须先 fork 子进程探测，见 §9。

---

## 6. 已知限制（对方会问）

| 限制 | 说明 | 出路 |
| --- | --- | --- |
| 抓屏 25–40 ms/帧 | GDI `BitBlt` 是瓶颈，不是网络 | 0.4 试 DXGI（本机会崩，需子进程探测）；兜底做持久 DIB（-35%） |
| MJPEG 码率高 | 1600×900 约 6.6 Mbps | 0.5 上 H.264 |
| 声音是 G.711 µ-law | 人声清楚、音乐发毛，128 kbit/s | 0.5 评估 Opus 双栈 |
| 无虚拟显示器/虚拟 HID | 分辨率受真实桌面限制；个别游戏拦 `SendInput` | 0.3 / 0.4（需驱动签名） |
| 中继无鉴权/限速 | 公网部署会被白嫖 | 0.4 加 `--token` + 限速 |
| 面板只监听 127.0.0.1 | 跨机访问要 SSH 隧道/反代 | 设计如此，别改默认 |
| 连接码即 PSK | 知道 9 位码即可连（同类产品同一模型） | 服务端只存 `code_id`；要更强就加确认弹窗 |

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
git add -A && git commit -m "feat: ..." && git push origin main
gh run watch     # 或 gh run list --limit 3
```

版本号到处要改的地方：`peerdesk/__init__.py` 的 `__version__`、`pyproject.toml` 的 `version`、`README` 徽章描述（如涉及）。

---

## 9. 下一步优先级（与 `docs/05-roadmap.md` 一致）

1. **真跨机验证**（回环测不出 NAT/丢包；需要一个公网 server + 两台机器）
2. **DXGI 抓屏**（收益最大：25–40 ms → 3–5 ms）——但本机 `DuplicateOutput` 会崩，
   先写 `scratch/dxgi_probe.py` 那种子进程探针，探针通过才在主进程启用；退路是持久 DIB（-35%）
3. **中继鉴权 + 限速**（公网可用性）
4. **PyInstaller 打包**（`--onefile` + `--add-data peerdesk/web`）
5. **H.264 / Opus**（码率与音质；都要外部动态库，做成"加载得到就用"的插件层）
6. 独立文件流（当前文件走控制通道，会跟键鼠抢带宽）、远程 Shell、Wake-on-LAN

明确不做：账号体系、会员、时长计费、把中继当付费权益。

---

## 10. 交接清单（Checklist）

- [x] 仓库公开、README/文档/许可证齐全、CI 通过
- [x] 三端可跑、直连与中继都验证过、双向文件传输可用
- [x] 单测 99 个、端到端 3 套（均验证声音）、`doctor` 自检
- [x] 声音通道：WASAPI loopback → G.711 µ-law → WebAudio，直连/中继/浏览器桥全部实测
- [x] 逆向报告留存（`docs/01`），证据来自本机安装目录 strings + 运行期抓包，不含官方代码/资源
- [ ] 真跨机（非回环）验证：需要两台机器 + 一个有公网 IP 或端口映射的 server
- [ ] 打包成 exe（PyInstaller 方案未做；当前是 `pip install -e .` + `python -m peerdesk`）
- [ ] 截图更新（加入文件传输/中继 HUD 的新图）

> 交接联系人：Bad0RANG3 · 有任何与协议相关的疑问，先读 `docs/03-protocol-pdtp1.md`，再看 `peerdesk/transport.py` 顶部的常量区。
