# 交接手册（Handover）

> 给下一个接手的人（或者三个月后的自己）。目标：**10 分钟内跑起来，1 小时内敢改代码，
> 半天内知道哪些坑已经踩过。**

最后更新：2026-11-22 · 版本 0.7.0 · 当前技术栈：C++20 + Qt 6.8 Widgets

> **迁移状态：后端已完成 C++ 化，Qt 客户端已接入连接页、被控端页和 Viewer 会话页。**
> Python 代码暂时保留作行为参考和迁移前测试，不再承接新功能；真实跨机验收和产品化
> 完成后删除。

---

## 1. 这是什么

**PeerDesk（对端桌面）**：Windows 远程桌面，自研 UDP 传输协议 PDTP/1。
立项原因是网易UU远程把「安全中继」做成了会员权益；PeerDesk 的立场是
**打洞优先、中继永远免费兜底、服务端可自托管、无账号无计费**。

| 项 | 值 |
| --- | --- |
| 仓库 | https://github.com/Bad0RANG3/peerdesk （PUBLIC，默认分支 `main`，Apache-2.0） |
| 本地路径 | `D:\UU\peerdesk` |
| 版本 | `CMakeLists.txt` → `0.7.0`（原生迁移阶段） |
| 入口 | `peerdesk-server`（信令/中继/STUN）、`peerdesk-client`（Qt 控制端与被控端） |
| 依赖 | Qt 6.8.3、MinGW 13.1、CMake 3.24+、Ninja；平台层直接使用 Windows API |
| 平台 | Windows 10/11；控制端与被控端都是 Qt 原生窗口 |
| 测试 | 11 个 CTest 套件，覆盖协议、加密、传输、信令、会话、控制器、平台层和 Qt UI |

当前连接模型：

```
被控端 peerdesk-client ──TCP 信令──▶ peerdesk-server ◀──TCP 信令── 控制端 peerdesk-client
                         └──── PDTP/1 over UDP（打洞，失败自动升级中继）────┘
```

已实现的主路径：

- 9 位连接码、服务端令牌、连接频次限制和每会话中继限速。
- X25519 → HKDF → AEAD、每包加密、重放窗口。
- UDP 打洞、6 秒自动升级中继、可靠控制流、视频 FEC/NACK/关键帧请求。
- DXGI/GDI 抓屏、多显示器、自适应 JPEG、WASAPI loopback、µ-law 声音。
- 键鼠、文本/IME、剪贴板、`Ctrl+Alt+Del`、全屏、双向文件传输。
- Qt 连接页、被控端页、Viewer 会话页、HUD、画质/显示器菜单、拖放和自动隐藏工具栏。
- 设置对话框、系统托盘、关闭到托盘策略，以及主窗口几何/最大化状态持久化。

---

## 2. 十分钟跑起来

准备环境（本机路径按实际 Qt 安装位置调整）：

```powershell
cd D:\UU\peerdesk
$env:PATH = "C:\Qt\6.8.3\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;$env:PATH"
cmake --preset windows-mingw
cmake --build --preset windows-mingw
```

先做一次全量测试：

```powershell
ctest --preset windows-mingw --output-on-failure
```

安全查看 Viewer（合成画面，不读真实桌面，不发真实输入）：

```powershell
.\build\cpp\peerdesk-client.exe --page viewer --viewer-demo
```

本机联调：

```powershell
# 终端 1
.\build\cpp\peerdesk-server.exe --port 7900 --udp-port 7901

# 终端 2：被控端；实际体验会读取本机桌面和声音
.\build\cpp\peerdesk-client.exe --page host --server 127.0.0.1:7900 --code 604173829

# 终端 3：控制端，在界面输入同一服务器和连接码
.\build\cpp\peerdesk-client.exe --server 127.0.0.1:7900
```

连接成功后控制端自动进入 Viewer。跨机时把 `127.0.0.1` 换成可达地址，并放行
TCP 7900 与 UDP 7901。调试 UI 时可以加 `--capture test --audio-test` 使用合成屏幕和
440 Hz 音源；此时被控端不会自动启动，需要在界面点“开始被控”。

UI 用例必须走 CTest：

```powershell
ctest --preset windows-mingw -R peerdesk-ui --output-on-failure -V
```

Windows GUI 测试程序直接运行不一定会把 QtTest 摘要打印到 PowerShell；CTest 会设置
`QT_QPA_PLATFORM=offscreen` 并正确收集退出码。

---

## 3. 代码地图

| 路径 | 什么时候要看它 |
| --- | --- |
| `src/transport/peer_link.*` | **90% 的链路 bug 在这里**：握手、打洞、中继、可靠流、FEC、统计 |
| `src/core/protocol.*` | 20 字节头、JSON 载荷、视频分片、ACK/NACK、音频包 |
| `src/core/crypto.*` | X25519/HKDF/AEAD、重放窗口 |
| `src/core/reliable.*` | 控制流累积 ACK、位图补洞、RTO |
| `src/core/video.*` | 视频/parity 分片、重组、FEC |
| `src/server/server.*` | 信令 + UDP 中继（`RLY1`）+ STUN + 令牌/频次/限速 |
| `src/client/signaling_client.*` | TCP JSON Lines 信令客户端 |
| `src/session/host_session.*` | 抓屏循环、输入/剪贴板分发、声音、带宽自适应、推文件 |
| `src/session/viewer_session.*` | 视频/声音接收、键鼠/文本/剪贴板、上传下载 |
| `src/session/file_transfer.*` | 双向文件块与落盘 |
| `src/platform/windows_screen_source.*` | DXGI/GDI 抓屏、显示器枚举、JPEG 编码 |
| `src/platform/windows_input.*` | `SendInput`、坐标映射、按键注入 |
| `src/platform/windows_clipboard.*` | 剪贴板读写与变化检测 |
| `src/platform/wasapi_audio.*` | WASAPI loopback、重采样、µ-law |
| `src/app/viewer_controller.*` | 控制端会话状态、画质、统计、文件、音频、错误 |
| `src/app/host_controller.*` | 被控端生命周期、设置、日志和文件 |
| `src/ui/viewer_page.*` | Viewer 画布、输入合帧、HUD、工具栏、遮罩、拖放、全屏 |
| `src/ui/app_window.*` | 页面导航、自动进 Viewer、挂断/结束返回连接页 |
| `src/ui/connection_page.*` / `host_panel.*` | 9 位码入口、被控端设置/统计/日志 |
| `src/ui/settings_dialog.*` | 服务器/令牌/画质/声音/接收目录/托盘设置与校验 |
| `src/app/preferences.*` | 主窗口、控制端、被控端偏好持久化 |
| `apps/peerdesk-client/main.cpp` | 启动参数、平台后端装配、demo 与截图入口 |
| `tests/ui_test.cpp` | 画布坐标、输入、连接/结束状态、demo 隔离、页面导航、托盘与窗口恢复 |

### 关键入口

- `PeerLink`：状态机 `idle → punching → direct|relay → connected|closed`；
  负责候选、握手、直连/中继、控制/视频/音频发送和链路统计。
- `ViewerController`：Qt UI 的唯一会话门面；`frameDecoded` 驱动画布，
  `stateChanged`/`sessionEnded` 驱动遮罩和页面切换。
- `ViewerPage` / `ViewerCanvas`：按比例缩放并居中，坐标按实际画面矩形归一化并夹紧；
  鼠标移动 16 ms 合帧，滚轮归并，失焦/断开时释放按键和鼠标按钮。
- `HostSession`：收到控制消息后分发到输入、剪贴板、音视频或文件逻辑；
  采集循环与周边设施分离，单个输入/剪贴板错误不能弄死画面。
- `SignalingServer`：先注册连接码，再匹配 `connect`，交换候选和中继信息；
  信令与媒体完全分离。

---

## 4. 协议速查

改协议前先读 `docs/03-protocol-pdtp1.md`。

**头 20 字节**：magic `PD`、版本、类型、包序号、会话相对毫秒、stream、flags、
payload 长度、session id。

- 包类型 1–13：hello / hello_ack / hello_fin / punch / punch_ack / data / parity /
  ack / nack / ping / pong / bye / audio。
- stream：video / control / file / stats / audio；控制、文件、统计共用可靠流语义。
- `ts_ms` 是 **uint32 会话相对毫秒**，不能塞 epoch 毫秒。
- 包 `seq` 是 AEAD nonce 计数器，必须严格单调；控制消息自己的序号在 payload 内，
  两者不能混用。
- 握手 transcript 绑定双方 hello 的原始字节；hello 必须缓存复用，重发不能重建。
- 候选交换后每 100 ms 发 PUNCH；6 秒未通自动升级中继。
- 中继帧 = `RLY1` + 配对 tag；信令（TCP JSON Lines）负责
  `register` / `connected` / `viewer` / `peer` / `cands` / `relay_hello` / `bye`。
- 媒体 datagram 上限 1400 字节；视频分片和 FEC 头都从 payload MTU 中扣除。

---

## 5. 已经踩过的坑（别再踩）

1. **握手报文必须缓存**：每次重建 hello（含时间戳）会让两侧 transcript 不同，
   派生密钥不同，表现为静默丢包。
2. **控制流序号不能塞进包头 seq**：会复用 AEAD nonce、触发重放窗口，
   ACK 全丢、控制通道永不确认。
3. **可靠流 ACK 是累积语义**：`cumulative` 之前的全部确认，再按 bitmap 补洞；
   只处理自己那一条会让 pending 永不归零。
4. **parity 必须把自己的位置算进去**：`missing = parity XOR 其他所有片`。
5. **控制消息超过 MTU 必须自己分片**：不能依赖 IP 分片，否则丢一片就整条消息丢失。
6. **STUN 解析必须校验 address family**，否则坏包会被解释成“合法但错误”的地址。
7. **强制中继时 host 不能等 viewer 先发包**：host 回 HELLO_ACK 后立即发加密 PING；
   双方在 established 前每秒重发，避免首个包丢失后永久停在 punching。
8. **画布坐标必须相对实际画面矩形**：不能直接用控件宽高；黑边区域要夹紧到 `[0,1]`。
9. **输入状态必须可释放**：失焦、窗口停用、进全屏、断线都要发 keyup/mouseup，
   否则远端会卡住 Shift/Ctrl 或鼠标左键。
10. **DXGI 结构体 ABI 必须完全匹配**：`DXGI_MODE_DESC` 末尾有 `Scaling`；
    漏字段会导致 `GetDesc` 越界并破坏相邻 COM 对象。staging texture 的 `RowPitch`
    可能大于 `width×4`，必须按行去 padding。
11. **WASAPI COM 单元是线程私有的**：采集必须在音频线程初始化和释放；
    主线程只发停止请求。vtable 序号数错通常直接 access violation。
12. **Win32 剪贴板句柄必须按 64 位声明**：`GlobalLock`/`GetClipboardData` 等
    不能退回默认 `c_int`/32 位，否则句柄被截断后崩溃。
13. **采集循环里的输入/剪贴板要各自容错**：输入初始化失败不能让整个采集 task 退出；
    先保证画面，再报周边设施错误。
14. **限速先降质量、再降帧率**：直接砍帧率会变成幻灯片但单帧仍超预算；
    画质压缩到预算仍超，再按实测帧大小降帧率。
15. **连接频次表要有硬上限**：只清过期项挡不住海量新 IP；超限丢最旧或拒绝，
    避免扫描器顶爆内存。
16. **demo 必须真正隔离**：`--viewer-demo` 只显示合成帧，键鼠、文件、音频都不发；
    否则截图/演示可能误操作真实桌面。
17. **关闭到托盘要考虑托盘不可用**：系统没有托盘时不能只隐藏窗口，否则用户既看不到界面
    也无法退出；应回退为正常关闭并发出退出信号。
18. **窗口恢复要校验可见性**：保存的几何可能来自已拔掉的显示器；恢复后必须与当前屏幕
    求交，否则窗口会出现在屏幕外。

---

## 6. 已知限制（诚实版）

| 限制 | 说明 | 出路 |
| --- | --- | --- |
| 尚未真跨机验收 | 当前证据主要是本机回环和合成 demo，不能代表 NAT/丢包/弱网体验 | 两台 Windows + 公网 server，覆盖直连、中继、5% 丢包、长会话 |
| Viewer 未完全对齐 UU远程 | 相对鼠标/游戏模式、工具栏淡入淡出、传输中取消尚未实现 | 按 `docs/05-roadmap.md` 继续 |
| C++ 抓屏性能尚未重测 | 原生实现已经接通，但没有对 p50/p95 重新出基准 | 合成源 + 真实桌面分别测，再决定脏矩形/H.264 |
| MJPEG 码率高 | 静态清晰但弱网浪费带宽 | H.264，失败回退 MJPEG |
| 声音是 G.711 µ-law | 人声清楚、音乐发毛 | Opus 双栈 |
| 无虚拟显示器/虚拟 HID | 分辨率受真实桌面限制；个别游戏拦截 `SendInput` | 驱动阶段，需要签名 |
| 中继限速是开环的 | 没有 RTT/丢包反馈 | GCC/BBR 式拥塞控制 |
| 信令令牌走明文 TCP | 媒体仍是 AEAD，但令牌可被同链路嗅探 | TLS 反代或 Noise 握手 |
| 连接码即 PSK | 知道 9 位码即可连，和同类产品模型一致 | 后续增加显式确认 |
| 没有安装器/开机自启 | 目前直接运行两个 exe；托盘与关闭到托盘已实现 | 产品化收尾 |

> 合成 Viewer 的 HUD 数字是固定演示值，不是实测性能；文档和验收报告不得混用。

---

## 7. 故障排查

| 现象 | 先看什么 |
| --- | --- |
| 一直“正在打洞…” | `peerdesk-server.exe -v` 的候选/中继日志；对称 NAT 应自动升中继；检查 UDP 端口和防火墙 |
| “连接已断开” | 双方状态、UDP 可达性、`RX_TIMEOUT`；确认两端与服务器令牌一致 |
| 画面卡住 | HUD 的 fps；被控端抓屏是否成功；关键帧请求是否到达；先用 `--capture test` 排除真实桌面 |
| 键鼠不动 | 被控端「允许键鼠控制」；UAC 安全桌面无法注入；确认 Viewer 画布有焦点 |
| 文本/剪贴板不同步 | 被控端「剪贴板同步」；是否非文本剪贴板；输入法组合文本是否已提交 |
| 文件失败 | Viewer 左下角错误和 toast；文件是否存在、为空、超过 512 MB，或已有文件在发送 |
| 启动缺少 Qt DLL | 把 Qt 和 MinGW 的 `bin` 放到 `PATH` 最前 |
| UI 测试无输出 | 用 `ctest --preset windows-mingw -R peerdesk-ui -V`，不要直接双击测试 exe |
| 截图是黑屏 | 必须带 `--page viewer --viewer-demo`；截图固定等待 450 ms 首帧渲染 |

当前 C++ 程序把日志输出到启动它的控制台，没有独立日志目录。
逆向产物：`D:\UU\re_work\{gv_*.txt, cjk_*.txt, extract_*.py}`。

---

## 8. 发布流程

```powershell
cd D:\UU\peerdesk
$env:PATH = "C:\Qt\6.8.3\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;$env:PATH"
cmake --preset windows-mingw
cmake --build --preset windows-mingw
ctest --preset windows-mingw --output-on-failure
git diff --check
git add -A
git commit -m "feat(qt): ..."
git push origin main
```

版本号目前只在 `CMakeLists.txt` 的 `project(... VERSION 0.7.0)`。
安装包尚未实现，发布说明不能写“已完成安装器”。

### 本阶段验证

- [x] 原生 Windows MinGW 构建
- [x] 11 个 CTest 套件（`--repeat until-fail:3` 全通过）
- [x] `peerdesk-ui` 20 个用例连续 100 次通过
- [x] `docs/screenshot-native-viewer.png` 布局检查
- [x] `git diff --cached --check`
- [ ] 真实跨机、真实丢包、长时间会话和安装包（本阶段不宣称完成）

---

## 9. 下一步优先级

1. **真实跨机验收**：两台 Windows + 公网 server；直连、中继、NAT、5% 丢包、长会话。
2. **产品化收尾**：自动重连、开机自启策略、安装包、README/CI 全部切到 C++/Qt。
3. **交互细节拉齐**：相对鼠标/游戏模式、工具栏淡入淡出、传输中取消、多文件状态。
4. **DXGI 增量优化**：脏矩形、GPU 共享纹理，减少整帧 CPU 往返。
5. **H.264 / Opus**：动态加载，失败回退 MJPEG / G.711。
6. 独立文件流、远程 Shell、Wake-on-LAN、基于 RTT/丢包的拥塞控制。
7. 完成迁移验收后删除全部 Python 文件与旧打包脚本。

明确不做：账号体系、会员、时长计费、把中继当付费权益。

---

## 10. 交接清单

- [x] 后端协议、传输、会话、Windows 平台层均为 C++20
- [x] Qt 连接页、被控端页、Viewer 会话页与统一窗口壳
- [x] Viewer 等比缩放、坐标映射、键鼠/滚轮/IME、剪贴板、文本、文件拖放、全屏、HUD
- [x] demo 模式只显示合成画面，禁止真实输入和文件发送
- [x] 设置对话框、系统托盘、关闭到托盘与窗口状态持久化
- [x] 逆向报告留存（`docs/01`），证据来自本机 strings + 运行期抓包，不含官方代码/资源
- [ ] 真跨机（非回环）验证
- [ ] Windows 安装包与可配置开机自启
- [ ] 完整迁移验收后删除全部 Python 文件与旧打包脚本

### 重截原生文档截图（不会泄露桌面）

```powershell
$env:PATH = "C:\Qt\6.8.3\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;$env:PATH"
.\build\cpp\peerdesk-client.exe --capture test --page remote `
  --screenshot docs\screenshot-native-connect.png
.\build\cpp\peerdesk-client.exe --capture test --page host `
  --screenshot docs\screenshot-native-host.png
.\build\cpp\peerdesk-client.exe --capture test --page viewer --viewer-demo `
  --screenshot docs\screenshot-native-viewer.png
.\build\cpp\peerdesk-client.exe --capture test --page settings `
  --screenshot docs\screenshot-native-settings.png
```

- 使用 `--capture test` / `--viewer-demo` 时不读真实桌面，临时偏好也不会污染用户配置。
- Viewer 画面来自 `SyntheticScreenSource`；HUD 数字是固定 demo 值，不能当性能报告。
- 设置截图直接抓取设置对话框；该模式使用临时偏好，不写真实配置。
- 截图入口等待 450 ms 后抓窗口，不要用系统截图工具抢在首帧前。

> 交接联系人：Bad0RANG3 · 协议疑问先读 `docs/03-protocol-pdtp1.md`，再看
> `src/core/protocol.h` 与 `src/transport/peer_link.h` 的常量/状态机。
