# 路线图

按"对使用者价值 / 实现成本"排序。✅ 已实现，🔜 下一版，🧭 中期，🌱 远期。

## 0.1（当前）
- ✅ 9 位连接码，3-3-3 分组，服务端只存 `code_id`
- ✅ PDTP/1：X25519 + HKDF + ChaCha20-Poly1305，每包 AEAD，重放窗口
- ✅ 候选交换 + UDP 打洞 + 6 秒自动升级中继 + `--relay-only`
- ✅ 可靠控制流（累积 ACK + 位图 + RTO 退避）与视频 FEC/NACK/关键帧请求
- ✅ 多显示器抓屏、自适应 JPEG 质量、静帧跳过
- ✅ 键鼠注入（绝对定位、Unicode 输入、`Ctrl+Alt+Del`）、文本剪贴板双向
- ✅ 浏览器 UI（连接码页 / 悬浮工具栏 / HUD），无框架无构建
- ✅ 自托管 server（信令 + 中继 + STUN 同端口）
- ✅ `doctor` 自检、79 个单元测试、3 个端到端测试

## 0.2 🔜
- 🔜 **DXGI Desktop Duplication 抓屏**：绕开 GDI `BitBlt`，把抓屏 25 ms 降到 3–5 ms，
  同时能拿到脏矩形（省掉整帧编码）。Windows 8+ 可用，`ctypes` 直接调 `d3d11.dll`+`dxgi.dll`。
- 🔜 **音频通道**（WASAPI loopback 采集 → Opus/`audioop`-free 的 ADPCM 兜底）。
  逆向里对应的就是 `Nevaudiosdk.dll` 虚拟声卡；我们不装驱动，先做"能听到"。
- 🔜 剪贴板图片/文件（`CF_DIB` → PNG 走文件流）。
- 🔜 服务端连接码尝试频率限制 + 可选 `--token`（自托管时的最小鉴权）。

## 0.3 🧭
- 🧭 **H.264 编码**：优先 `openh264` 的 Python 绑定或 `libx264` 动态库；
  拿不到就退回 MJPEG。收益：同画质码率降到 1/5，弱网质变。
- 🧭 **虚拟显示器**：IddCx 驱动（需要签名/测试签名），对应 UU 的 `GameViewerIddDriver`。
  在此之前用"改变被控端分辨率"作为替代。
- 🧭 独立文件流（`stream=FILE` + 拥塞感知限速），替代当前 base64 分块。
- 🧭 远程 Shell（对应 UU 的 `uuyc-cli term`）：ConPTY + 浏览器 xterm 风格面板。
- 🧭 Wake-on-LAN（对应 `/api/v1/device/wake_on_lan`）。

## 0.4 🌱
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
| 没有虚拟显示器/虚拟 HID | 分辨率受真实桌面限制；个别游戏拦截 `SendInput` | 0.3 / 0.4 |
| 无音频 | 只能看不能听 | 0.2 |
| MJPEG 码率高 | 弱网/流量敏感场景体验差 | 0.3 |
| 无账号/设备列表 | 每次要输码，设备多时不便 | 不打算做账号；后续做"本地收藏 + 二维码" |
| 无远程终端 | 运维场景弱 | 0.3 |
