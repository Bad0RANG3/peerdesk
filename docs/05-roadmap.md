# 路线图

按"对使用者价值 / 实现成本"排序。✅ 已实现，🔜 下一版，🧭 中期，🌱 远期。

## 0.4（当前）

- ✅ **服务端鉴权**：`--token` / `PEERDESK_TOKEN`，`register` 与 `connect` 都校验
  （`hmac.compare_digest`），失败回 `{"t":"error","reason":"bad_token"}`；两端都有 `--token`，
  浏览器连接码面板有令牌输入框（记住在 localStorage）。
- ✅ **连接频次限制**：`--connect-max`（默认 10）/ `--connect-window`（默认 60 s）按来源 IP 滑动窗口，
  超限回 `reason=rate_limited`；`attempts` 字典会清过期项并在 8192 条时硬顶丢最早，挡住海量 IP。
- ✅ **中继限速**：`--relay-mbps` / `PEERDESK_RELAY_MBPS`，每对会话一个令牌桶（1 s 突发）；
  超出的报文直接丢弃（视频是 UDP 流，丢包由 FEC/NACK 兜）。
- ✅ **带宽自适应**：服务器把 `relay_mbps` 下发，被控端 `HostSession.set_rate_limit()` 先压 JPEG
  质量（`FramePacer.set_budget_kb`），仍超就按实测帧大小降帧率（`_effective_fps`）；
  实测 1.5 Mbps 上限下从"1 帧 / 6 s"变成 4 fps / 44 KB/帧 / 链路 1.51 Mbps。
- ✅ 119 个单元测试（新增 `tests/test_server.py` 13 个、`tests/test_ratelimit.py` 7 个），
  `e2e_local --token` / `--bad-token` / `--relay-mbps` 三条负路径也覆盖。

## 0.3（已完成）

- ✅ **声音通道**：WASAPI loopback（纯 ctypes，不装驱动）→ 16 kHz 单声道 → G.711 µ-law，
  20 ms 一帧，帧内带上一帧冗余、0.5 s 静音后自动抑制到 2.5 kbit/s；
  浏览器 WebAudio 抖动缓冲播放，工具栏 / HUD / 被控端面板三处都能开关。
  逆向里对应 `Nevaudiosdk.dll` 虚拟声卡，我们不装驱动，直接抓默认播放设备的混音。
- ✅ `--audio-test`（440 Hz 合成音）让无声卡环境也能联调；e2e 直连 267 帧、中继 214 帧。
- ✅ 99 个单元测试（新增 `tests/test_audio.py` 14 个），三个端到端全部覆盖声音。

## 0.2（已完成）

- ✅ 9 位连接码，3-3-3 分组，服务端只存 `code_id`
- ✅ PDTP/1：X25519 + HKDF + AEAD，每包加密，重放窗口
- ✅ 候选交换 + UDP 打洞 + 6 秒自动升级中继 + `--relay-only`
- ✅ 可靠控制流（累积 ACK + 位图 + RTO 退避）与视频 FEC/NACK/关键帧请求
- ✅ 多显示器抓屏、自适应 JPEG 质量、静帧跳过
- ✅ 键鼠注入（绝对定位、Unicode 输入、`Ctrl+Alt+Del`）、文本剪贴板双向
- ✅ 双向文件传输（浏览器拖拽上传 / 被控端按路径推送）
- ✅ 浏览器 UI（连接码页 / 悬浮工具栏 / HUD），无框架无构建
- ✅ 自托管 server（信令 + 中继 + STUN 同端口）
- ✅ `doctor` 自检

## 0.4 🔜

- 🔜 **DXGI Desktop Duplication 抓屏**：绕开 GDI `BitBlt`，把抓屏 25 ms 降到 3–5 ms，
  同时能拿到脏矩形（省掉整帧编码）。Windows 8+ 可用，`ctypes` 直接调 `d3d11.dll` + `dxgi.dll`。
  ⚠️ 本机（RTX 4070 Ti SUPER + 官方驱动）实测 `IDXGIOutput1::DuplicateOutput` 恒定
  access violation（`0xC0000005`，读 `0xFFFF...FF`），已排除设备创建路径 / COM 单元 / GUID 布局 /
  DPI 感知；该功能必须先做**子进程探测**再启用，否则会连带杀掉主进程。
- 🔜 持久 DIB + `BitBlt` 抓屏（无新依赖的兜底优化，实测 19.35 ms → 12.30 ms，约 -35%）。
- 🔜 剪贴板图片/文件（`CF_DIB` → PNG 走文件流）。

## 0.5 🧭

- 🧭 **H.264 编码**：优先 `openh264` 的 Python 绑定或 `libx264` 动态库；
  拿不到就退回 MJPEG。收益：同画质码率降到 1/5，弱网质变。
- 🧭 **Opus 音频**：把 µ-law 换成 Opus（16 kHz 语音模式，约 24 kbit/s 起），
  需要 `libopus` 动态库；先做"能加载就用、加载失败退回 G.711"的双栈。
- 🧭 **虚拟显示器**：IddCx 驱动（需要签名/测试签名），对应 UU 的 `GameViewerIddDriver`。
  在此之前用"改变被控端分辨率"作为替代。
- 🧭 独立文件流（`stream=FILE` + 拥塞感知限速），替代当前 base64 分块。
- 🧭 远程 Shell（对应 UU 的 `uuyc-cli term`）：ConPTY + 浏览器 xterm 风格面板。
- 🧭 Wake-on-LAN（对应 `/api/v1/device/wake_on_lan`）。

## 0.6 🌱

- 🌱 虚拟 HID 驱动（游戏兼容，对应 `uuycinput`）。
- 🌱 硬件编码（NVENC/QSV/AMF）——需要与 `ffmpeg` 动态库协作，破坏"零重依赖"，单独插件化。
- 🌱 WebRTC 兼容层（可选）：把 PDTP 媒体封装成 RTP，让浏览器原生 `<video>` 也能看。
- 🌱 macOS/Linux 被控端（抓屏走 `xdg-desktop-portal`/`CGDisplayStream`，输入走 `uinput`）。
- 🌱 中继集群（多地域、带宽计费可选），但仍保持"P2P 优先、中继免费兜底"。

## 明确不做

- ❌ 账号体系、会员、时长计费、中继加速包。
- ❌ 把"安全中继"当付费权益（这是本项目立项的直接动机）。
- ❌ 静默安装驱动/服务、开机自启不可关闭。

## 与 UU远程 的功能差距（诚实版）

| 差距 | 影响 | 计划 |
| --- | --- | --- |
| 没有虚拟显示器/虚拟 HID | 分辨率受真实桌面限制；个别游戏拦截 `SendInput` | 0.5 / 0.6 |
| 声音是 8 bit µ-law | 人声清楚，音乐发毛 | 0.5 起评估 Opus |
| MJPEG 码率高 | 弱网/流量敏感场景体验差 | 0.5 |
| 无账号/设备列表 | 每次要输码，设备多时不便 | 不打算做账号；后续做"本地收藏 + 二维码" |
| 无远程终端 | 运维场景弱 | 0.5 |
