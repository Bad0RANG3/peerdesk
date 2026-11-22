# 路线图

按"对使用者价值 / 实现成本"排序。✅ 已实现，🔜 下一版，🧭 中期，🌱 远期。

## 0.7（当前）：C++ / Qt 原生化

- ✅ **后端重写为 C++20 + Qt 6**：协议、可靠流、X25519/HKDF/AEAD、STUN、UDP 打洞/中继、
  信令服务端、会话、文件传输、Windows 抓屏/输入/剪贴板/WASAPI 均已迁到 `src/`；
  `peerdesk-server` 与 `peerdesk-client` 不依赖 Python 运行时。
- ✅ **应用控制器**：`HostController` / `ViewerController` 封装会话生命周期、画质档位、
  偏好设置、统计、文件、音频与错误状态，Qt UI 只消费稳定接口。
- ✅ **原生应用壳**：`src/ui/` 已实现统一窗口、连接页、被控端面板、主题、玻璃卡片、
  开关/状态胶囊/toast；9 位码自动分组，连接状态与断开按钮由控制器驱动。
- ✅ **构建与测试**：CMake presets、Windows MinGW 构建通过；11 个 CTest 套件全绿，
  其中 `peerdesk-ui` 覆盖连接码、输入校验、连接派发、页面切换和被控端渲染。
- 🔜 **Viewer 会话页**：接 `ViewerController::frameDecoded` 的真实画布、自适应缩放、
  连接中/断线遮罩、HUD 与悬浮工具栏自动隐藏。
- 🔜 **输入与桌面交互**：键鼠捕获、相对/绝对坐标、滚轮合并、`Ctrl+Alt+Del`、文本输入、
  剪贴板双向、全屏、拖放上传与下载。
- 🔜 **产品化收尾**：设置页、托盘、重连、窗口状态持久化、安装包、README/CI 切到 C++/Qt，
  最后删除全部 Python 代码与打包脚本。

> 本阶段只完成"原生壳 + 连接/被控端页面"。Viewer 视频页尚未接入，远程会话体验仍未达到
> UU远程 的完整交互水平，后续按上面的顺序继续。

## 0.6（已完成）

- ✅ **DXGI Desktop Duplication 抓屏**：新增 `peerdesk/dxgi.py`，用纯 ctypes 走
  `D3D11CreateDevice` → `IDXGIAdapter::EnumOutputs` → `IDXGIOutput1::DuplicateOutput` →
  `AcquireNextFrame` → staging texture → `Map`，并整理 `RowPitch` 行尾 padding。
  `--capture auto` 现在按 **DXGI → GDI → mss** 选择；运行期 access lost 自动回退，
  显式 `--capture dxgi` 失败则报错，方便排障。
- ✅ **DXGI 崩溃隔离**：探测在隐藏子进程 `python -m peerdesk _dxgi_probe` 中跑完整
  创建/抓帧/关闭流程，10 秒超时，结果进程内缓存；`PEERDESK_DXGI_PROBE=0` 可禁用。
  frozen exe 同样支持 `peerdesk.exe _dxgi_probe`，打包 spec 已加入 hidden import。
- ✅ **修掉 DXGI 生命周期 ABI bug**：`DXGI_MODE_DESC` 漏了末尾 `Scaling`，导致
  28 字节变 24 字节、`DXGI_OUTDUPL_DESC` 变 32 而非 36 字节，`GetDesc` 越界 4 字节
  破坏相邻 COM 对象，表现为首轮正常、第二轮 `close()` access violation。
  `tests/test_dxgi.py` 固定结构体大小/offset、vtable 序号、`RowPitch` 整理、探测与回退。
- ✅ **本机性能**：2560×1440 → 1600×900、JPEG q60 的完整链路 p50 从 GDI 22.0 ms
  降到 DXGI 10.1 ms（p95 12.3 vs 28.1）；`ScreenCapture.grab` 1600 宽为
  10.9 vs 18.3 ms。直连 e2e 的 `enc_ms` 从 22.2 降到 11.9，约一半。
- ⚠️ 当前 DXGI 仍每帧整帧 `CopyResource`/`Map`，没有脏矩形或共享纹理；
  JPEG 也仍是 CPU 编码。下一阶段见 0.7。
- ✅ 150 个单元测试（新增 `tests/test_dxgi.py` 16 个）。

## 0.5.1（已完成）

- ✅ **合成信号源** `--capture test`：`TestPattern` 画渐变底 + 网格 + 时钟 + 移动方块，
  **完全不读真实屏幕**。文档截图、演示、CI 都用它——之前的 README 截图是真桌面，
  把人家的聊天窗口拍进了公开仓库。
- ✅ **URL 直达** `?code=<9位>&server=<host:port>&auto=1&hud=1`：一条链接点开就连，
  截图脚本、给同事发"帮我看一下"都靠它。
- ✅ **文档截图重截**：viewer 会话页（合成画面 + HUD 读数）、连接码门页、被控端面板。
  工具是 `scratch/shot_docs.py` + `scratch/shot_cdp.py`（手写最小 CDP 客户端），
  因为 `chrome --headless --screenshot` 抓不到 `<canvas>` 内容。
- ✅ **修掉一个真 bug**：剪贴板 API 未声明 `restype`，64 位句柄被截断，`GlobalLock` 直接
  access violation —— 控制端一连上，被控端采集循环就崩、画面全黑。顺手让采集循环对
  "输入/剪贴板初始化失败"免疫（置 `_input_failed` 继续推画面）。
- ✅ 被控端面板：`--fps 20` 这种下拉里没有的值不再显示成空白；统计行补上质量/KB；
  打洞期间不再刷屏。
- ✅ 133 个单元测试（新增 `tests/test_clipboard.py` 5 个）。

## 0.5（已完成）

- ✅ **单文件 exe 打包**：`packaging/peerdesk.spec` + `packaging/build.py`，
  PyInstaller onefile 产出 `dist/peerdesk.exe`（33 MB，agent/viewer/server/doctor 四端合一，
  前端资源内嵌），目标机器**不需要 Python**。`tests/e2e_exe.py` 用真 exe 起三个进程做黑盒互连，
  CI 每次推 main 构建并上传 artifact。
- ✅ **GDI 抓屏后端**：`GdiCapture` 用常驻 `CreateDIBSection` + `BitBlt`，
  并且直接用 `StretchBlt` 在驱动层缩放到目标宽高（省掉 Pillow resize）。
  实测 2560×1440 → 1600×900 的"抓屏+缩放+重排+JPEG q60"从 36.1 ms 降到 22.7 ms，
  e2e 的 `enc_ms` 从 37 降到 23，帧率 13 fps → 16 fps。`--capture auto|gdi|mss`、
  `--scale-mode fast|halftone|pil` 可选，`mss` 保留为回退路径。

## 0.4（已完成）

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
- ✅ 133 个单元测试（新增 `tests/test_server.py` 13 个、`tests/test_ratelimit.py` 7 个、
  `tests/test_clipboard.py` 5 个（剪贴板 ctypes 签名 + 采集循环容错）、
  `tests/test_capture.py` 9 个），
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

## 0.7 🔜

- 🔜 **DXGI 脏矩形 + GPU 共享纹理**：当前 DXGI 已能抓屏，但每帧仍把整帧拷到 staging
  texture 再 `Map` 到 CPU。下一步读 `DXGI_OUTDUPL_FRAME_INFO` 的移动/脏矩形元数据，
  只编码变化区域；并评估 D3D11 共享纹理直接交给编码器，避免 CPU 往返。
- 🔜 **DXGI 输出拓扑刷新与多适配器枚举**：当前只在初始化时枚举默认 D3D11 适配器上的输出；
  运行中改分辨率或热插拔需重启 agent，多 GPU 还可能漏显示器。后续在拓扑变化后重建输出列表，
  或发现 DXGI 数量少于 GDI 时降级/合并。
- 🔜 **H.264 编码**：优先 `openh264` 的 Python 绑定或 `libx264` 动态库；
  拿不到就退回 MJPEG。收益：同画质码率降到 1/5，弱网质变。
- 🔜 剪贴板图片/文件（`CF_DIB` → PNG 走文件流）。
- 🔜 真跨机（非回环）验证：两台机器 + 一个公网 server，覆盖对称 NAT 与 5% 丢包。

## 0.8 🧭

- 🧭 **Opus 音频**：把 µ-law 换成 Opus（16 kHz 语音模式，约 24 kbit/s 起），
  需要 `libopus` 动态库；先做"能加载就用、加载失败退回 G.711"的双栈。
- 🧭 **虚拟显示器**：IddCx 驱动（需要签名/测试签名），对应 UU 的 `GameViewerIddDriver`。
  在此之前用"改变被控端分辨率"作为替代。
- 🧭 独立文件流（`stream=FILE` + 拥塞感知限速），替代当前 base64 分块。
- 🧭 远程 Shell（对应 UU 的 `uuyc-cli term`）：ConPTY + 浏览器 xterm 风格面板。
- 🧭 Wake-on-LAN（对应 `/api/v1/device/wake_on_lan`）。

## 0.9 🌱

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
| 没有虚拟显示器/虚拟 HID | 分辨率受真实桌面限制；个别游戏拦截 `SendInput` | 0.8 / 0.9 |
| 声音是 8 bit µ-law | 人声清楚，音乐发毛 | 0.8 起评估 Opus |
| MJPEG 码率高 | 弱网/流量敏感场景体验差 | 0.7 |
| 无账号/设备列表 | 每次要输码，设备多时不便 | 不打算做账号；后续做"本地收藏 + 二维码" |
| 无远程终端 | 运维场景弱 | 0.8 |
