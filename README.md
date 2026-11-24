# PeerDesk · 对端桌面

**自托管的 Windows 远程桌面：原生 Qt 6 客户端、C++20 后端、9 位连接码、UDP 直连优先、自建中继兜底。**

PeerDesk 以逆向分析得到的 UU远程 交互为参照，持续补齐连接、会话和桌面操作体验。
它不是 UU远程 的等价替代品，也不包含对方的代码、资源或凭据。

[![CI](https://github.com/Bad0RANG3/peerdesk/actions/workflows/ci.yml/badge.svg)](https://github.com/Bad0RANG3/peerdesk/actions/workflows/ci.yml)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-3d7bfd)
![C%2B%2B](https://img.shields.io/badge/C%2B%2B-20-00599c)
![Qt](https://img.shields.io/badge/Qt-6.8-41cd52)
![License](https://img.shields.io/badge/license-Apache--2.0-35d07f)

![PeerDesk 原生会话页](docs/screenshot-native-viewer.png)

> 上图由 `--page viewer --viewer-demo` 生成。画面来自合成信号源，不读取真实桌面；
> HUD 中的 `1600×900 / 59.8 fps / 22.6 Mbps / 6.4 ms` 是固定演示值，**不是实测性能**。

## 当前状态

版本 **0.9.0**，处于原生迁移与产品化之间。服务端、传输、会话、Windows 平台层和
Qt 应用壳均已切换到同一套 C++ 实现，主连接路径可用；真实跨机、弱网和长时间会话
仍需单独验收。被控端的抓屏、缩放、静帧检测和 JPEG 编码已迁入独立媒体线程，Viewer
JPEG 解码也已迁入独立解码线程并按最新帧背压，画面等比缩放由
`PeerDeskViewerPresent` 工作线程按物理像素生成，UI 线程只绘制最终图像；音频包校验与
G.711 µ-law 解码已迁入 FIFO 工作线程，抖动缓冲和 `QAudioSink` 播放已迁入
`PeerDeskAudio` 工作线程；视频发送分片、XOR FEC 生成和 NACK 重传缓存检索已迁入
独立发送工作线程，入站分片解析、重组、XOR FEC 恢复、超时丢帧和 NACK 判定已迁入
独立接收工作线程；UDP socket 绑定/收发、入站类型识别、RLY1 解包、包头解析、双向
AEAD、重放检查、发送序号和统计已迁入
`PeerDeskUdpIo` 工作线程；已认证包的类型分发、控制分片、可靠流 ACK、PING/RTT、
音视频转发和 Bye 处理已迁入 `PeerDeskPeerLinkRx` 工作线程，主线程只负责路径锁定、
统计、实际 UDP 发送和会话衔接；
被控端每秒汇总 RTT、NACK 帧/分片和自动丢包关键帧请求，按网络反馈收紧 JPEG 质量与
发帧率上限，并在链路持续健康 4 秒后逐步恢复；不同网络条件下的参数仍需真实验收；
被控端键鼠输入由 `PeerDeskHostInput` 工作线程按命令顺序串行注入，UI 线程只解析控制
消息并提交命令；文件发送分块、接收校验与临时文件落盘由 `PeerDeskFileTransfer` 工作
线程串行处理，UI 线程只负责文件选择、进度展示和状态更新；
Viewer 在同一连接码下的断线恢复已经接入并通过回环测试，中断的文件可在重新握手后按接收端
确认的偏移继续传输；Viewer 与被控端都支持可重试的多文件发送队列；
连接页会保留最近 4 个成功连接的本地记录，支持一键回填连接码与服务器；
普通模式采用单实例运行，再次启动会唤醒现有窗口。设置页可维护当前 Windows 用户的登录
启动项，`--background` 会在托盘可用时静默驻留并自动开始本机被控；真实注销/重启验收
仍未完成。客户端和服务端会把滚动日志写入 `%APPDATA%\PeerDesk\logs`；设置页可打开
日志目录并导出脱敏诊断包，上次异常退出会在下次启动时提示。Windows 每用户安装包已接入
Qt/MinGW 运行库部署、开始菜单快捷方式、
中文关键向导文案和静默安装/卸载冒烟；安装包尚未签名，自动更新和跨版本升级路径仍未完成。
旧 Python 实现、Web 前端、PyInstaller 打包和 pytest 资产已经从仓库移除，构建、测试与
CI 只依赖原生工具链。

## 体验目标

- 像 UU远程 一样用 9 位码快速连接，不要求账号，不把中继能力做成会员项。
- 原生窗口承载连接页、被控端页和会话页，避免多开终端或跳转外部界面。
- 连接后直接进入桌面画面；悬浮工具栏、HUD、画质、显示器、声音、文件与全屏操作都在会话内完成。
- 短暂断线时不退回连接页，保留最后一帧并自动尝试恢复原会话。
- 打洞成功时走直连，网络不允许时自动升级到自托管 UDP 中继。

## 已实现

- **原生客户端**：Qt 6 Widgets 连接页、被控端面板、Viewer、设置对话框、系统托盘和窗口状态记忆。
- **单实例运行**：普通启动只保留一个客户端进程；再次启动通过本机命名管道唤醒已有窗口。
  截图、合成采集、合成音源和 Viewer demo 使用隔离模式，仍可与日常实例并行运行。
- **登录启动与后台托盘**：设置页可写入或删除当前用户
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下的 `PeerDesk` 项，命令为
  带引号的绝对可执行路径加 `--background`，无需管理员权限。后台启动参与单实例锁、
  自动开始本机被控，并在托盘可用时隐藏窗口；托盘不可用时回退显示主窗口。
- **最近连接**：成功进入实况后记录最近 4 个对端，按“连接码 + 服务器”去重并置顶；
  点击只回填连接码与服务器，不自动连接，也不保存服务器令牌。记录仅保存在本机，
  可随时从连接页清除。
- **Windows 安装/卸载**：Inno Setup 按当前用户安装到
  `%LOCALAPPDATA%\Programs\PeerDesk`，不需要管理员权限；同时部署客户端、服务端、
  Qt 6 平台/图像/多媒体插件与 MinGW 运行库。安装器创建开始菜单快捷方式，桌面快捷方式
  可选，卸载入口出现在 Windows“应用和功能”中。卸载会移除程序文件、快捷方式和当前用户
  登录启动项，保留 `%APPDATA%\PeerDesk` 与用户接收目录中的文件。
- **纯原生仓库**：服务端、客户端、测试和发布入口统一由 CMake/C++20/Qt 构建，不依赖
  Python、Node.js 或旧 Web 服务。
- **日志与诊断**：客户端和服务端写入带时间、级别、PID 和线程 ID 的滚动文件日志，默认
  位于 `%APPDATA%\PeerDesk\logs`，单文件 4 MiB、最多保留 5 个；设置页可打开日志目录或
  导出脱敏 ZIP 诊断包。诊断包包含清单、设置摘要和当前客户端日志，不自动上传，也不含
  minidump；上次运行异常退出时会在下次启动给出提示。
- **连接与信令**：9 位连接码、3-3-3 显示、直连优先、6 秒升级中继、服务器令牌、连接频次限制。
- **加密传输**：X25519 临时密钥、HKDF-SHA256、每数据包 AEAD、重放窗口和中继密文转发；
  入站解析、认证和重放检查在 UDP I/O 线程完成，接收代际隔离重置前排队的旧会话包。
- **视频**：GDI 抓屏、多显示器、独立媒体线程内的缩放/静帧检测/JPEG 编码、网络反馈
  驱动的质量/帧率上限自适应、Viewer 独立解码与呈现线程、发送工作线程内的分片与
  XOR FEC、缓存检索，以及接收工作线程内的分片解析、重组、XOR FEC 恢复、超时丢帧和
  NACK 判定；PDTP 的 DATA、PARITY 和 NACK 重传已携带 JPEG/H.264 codec 元数据，
  当前产品发送仍为整帧 JPEG；
  `PeerDeskPeerLinkRx` 负责已认证包分发、控制可靠流和 ACK/PING 处理；
  `PeerDeskLinkFeedback` 异步发送 NACK、Pong、ACK 和控制 DATA，关键帧请求经认证
  可靠控制流提交并限制为每 200 ms 一次；`PeerLink` 主线程只负责路径锁定、接收统计、
  反馈路由快照和会话衔接；`PeerDeskUdpIo`
  工作线程独占 UDP socket，并完成入站类型识别、RLY1 解包、包头解析、双向 AEAD、
  重放检查、发送序号、发送统计和实际收发。
- **声音**：WASAPI loopback、16 kHz 单声道、20 ms G.711 µ-law 帧、上一帧冗余和静音抑制；
  Viewer 在 `PeerDeskViewerAudio` 工作线程按 FIFO 校验并解码音频包，PCM16 再排队到
  `PeerDeskAudio` 工作线程，由 10 ms 泵、最长 360 ms 待播队列和配置为 160 ms 的
  `QAudioSink` 缓冲完成播放。
- **桌面控制**：绝对坐标键鼠、滚轮、文本/IME、剪贴板文本、`Ctrl+Alt+Del`、
  相对鼠标输入和按键释放；被控端在独立的 `PeerDeskHostInput` 工作线程执行注入，
  连续绝对移动会合并，停止、禁用输入和断线会在工作线程释放按键与鼠标按钮。
- **游戏鼠标**：Viewer 捕获并每 16 ms 合并相对位移，左右键使用独立按下/释放语义；`Esc`、
  失焦、断线、失败和结束都会退出模式并释放输入。
- **文件**：双向传输、24 KiB 分块、进度/错误提示、发送中取消、断链中断提示和单文件
  512 MiB 上限；接收端在落盘前校验路径，并在同名目标下生成 `名称 (1).扩展名` 等唯一
  路径；名称接近 255 字符上限时缩短主干并保留扩展名与序号，避免同名同大小传输互相
  覆盖或复用错误断点。接收端按旧传输 ID、文件名、大小和 10 分钟保留期校验断点。
  续传确认前发送端不发送剩余数据，确认后从实际偏移继续。Viewer 与被控端发送侧均支持
  多文件队列、成功自动推进、失败项重试、取消当前项后继续剩余队列，以及断线暂停和
  恢复后自动续传；续传回调携带发送端源路径，不同目录下的同名同大小文件不会串用
  队列进度或断点。
  文件分块、Base64 编解码、接收校验和落盘提交在 `PeerDeskFileTransfer` 工作线程执行，
  接收开始、进度、完成和失败通过排队信号回到会话层，不占用 UI 线程。
- **会话交互**：五档画质、显示器切换、游戏鼠标、声音开关、全屏、160 ms 淡入淡出并
  自动隐藏的悬浮工具栏、实时 HUD、拖放上传、批量发送和取消发送。
- **断线恢复**：Viewer 实况中断后保留最后一帧和连接上下文，隔离旧输入，并在 30 秒内
  按同一连接码重新配对、重新握手和请求关键帧；未完成的文件队列与进度会保留，恢复后
  使用新传输 ID 接续接收端临时文件，从确认的实际偏移继续发送。
- **自托管服务端**：TCP 信令、UDP 中继和 STUN 共用一套服务；每会话中继限速，超限丢包。

五档画质分别为：`流畅`、`均衡`、`高清`、`超清`、`原画`。

## 架构

```text
被控端 peerdesk-client ──TCP 信令──▶ peerdesk-server ◀──TCP 信令── 控制端 peerdesk-client
                         └──── PDTP/1 over UDP：直连或 RLY1 中继 ────┘
STUN 与中继共用服务端 UDP 端口
```

| 路径 | 职责 |
| --- | --- |
| `src/core/` | 报文、分片、FEC、可靠流、STUN、X25519/HKDF/AEAD、重放窗口 |
| `src/transport/` | `PeerLink` 握手、候选、打洞、中继、保活、统计和视频发送/接收流水线 |
| `src/server/` | TCP 信令、连接码注册、UDP 中继、STUN、令牌与限速 |
| `src/client/` | TCP JSON Lines 信令客户端 |
| `src/session/` | 被控端/控制端会话、音视频、输入、剪贴板、文件 |
| `src/app/` | 控制器、偏好、画质预设、声音播放 |
| `src/diagnostics/` | 滚动文件日志、脱敏、运行标记和无压缩 ZIP 诊断包 |
| `src/ui/` | 原生窗口、连接页、被控端面板、Viewer、设置和主题 |
| `src/platform/` | GDI 抓屏、WASAPI、Win32 输入和剪贴板 |

协议细节见 [PDTP/1 协议规范](docs/03-protocol-pdtp1.md)，模块与数据流见
[架构说明](docs/02-architecture.md)。

## 构建

要求：

- Windows 10/11 x64
- CMake 3.24+
- Ninja
- Qt 6.8.3 `mingw_64`
- MinGW 13.1（Qt 安装器自带）

```powershell
git clone https://github.com/Bad0RANG3/peerdesk.git
cd peerdesk
$env:PATH = "C:\Qt\6.8.3\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;$env:PATH"
cmake --preset windows-mingw
cmake --build --preset windows-mingw
```

产物位于 `build/cpp/`：

- `peerdesk-server.exe`
- `peerdesk-client.exe`
- `peerdesk-link-harness.exe`

### Windows 安装包

安装器使用 Inno Setup 6。先构建 Release 客户端与服务端：

```powershell
$env:PATH = "C:\Qt\6.8.3\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;$env:PATH"
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release `
  --target peerdesk-client peerdesk-server
.\packaging\windows\build-installer.ps1
```

`build-installer.ps1` 会执行 `cmake --install`、`windeployqt`，校验 Qt 插件与运行库，
然后生成：

- `dist/PeerDesk-0.9.0-setup.exe`
- `dist/SHA256SUMS.txt`

可在本机验证静默安装、安装版启动截图和静默卸载：

```powershell
.\packaging\windows\test-installer.ps1 `
  -InstallerPath .\dist\PeerDesk-0.9.0-setup.exe
```

该测试会临时安装到 `dist/installer-test/app`，检查快捷方式、卸载器、服务端版本、
Qt 平台插件、用户数据保留和登录启动项清理，随后卸载。测试要求当前用户尚未安装
PeerDesk，并会恢复测试前的同名启动项。

## 测试

```powershell
ctest --preset windows-mingw --output-on-failure
```

当前有 15 个 CTest 套件：`peerdesk-core`、`peerdesk-crypto`、`peerdesk-transport`、
`peerdesk-stun`、`peerdesk-server`、`peerdesk-peer-link`、`peerdesk-session`、
`peerdesk-signaling-client`、`peerdesk-app-controller`、`peerdesk-single-instance`、
`peerdesk-audio-output`、`peerdesk-ui`、`peerdesk-logging`、`peerdesk-platform`、
`peerdesk-codec`。
Windows CI 会配置、编译并执行
全部套件，并在独立打包任务中生成 Release 安装包，执行静默安装、离屏启动截图和静默卸载
冒烟。

## 运行

### 1. 启动服务端

```powershell
.\build\cpp\peerdesk-server.exe --port 7900 --udp-port 7901
```

跨机部署时需要放行 TCP `7900` 和 UDP `7901`。建议为公开服务器设置令牌与中继带宽：

```powershell
.\build\cpp\peerdesk-server.exe --port 7900 --udp-port 7901 `
  --token "<shared-token>" --relay-mbps 50 --connect-max 10 --connect-window 60
```

### 2. 启动被控端

```powershell
.\build\cpp\peerdesk-client.exe --page host `
  --server 127.0.0.1:7900 --code 604173829 --token "<shared-token>"
```

在被控端面板确认连接码、服务器、令牌、键鼠/剪贴板权限和接收目录，然后开始被控。

### 3. 启动控制端

```powershell
.\build\cpp\peerdesk-client.exe --server 127.0.0.1:7900 --token "<shared-token>"
```

在连接页输入相同的 9 位码。连接成功后自动进入会话页。

### 安全预览会话页

```powershell
.\build\cpp\peerdesk-client.exe --page viewer --viewer-demo
```

该模式只渲染合成画面，不读取屏幕、不注入输入、不联网传输。

## 服务端参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--host` | `0.0.0.0` | TCP/UDP 监听地址 |
| `--port` | `7900` | TCP 信令端口，`0` 为随机 |
| `--udp-port` | `7901` | UDP 中继与 STUN 端口，`0` 为随机 |
| `--token` | 空 | 客户端共享令牌；设置后两端必须一致 |
| `--relay-mbps` | `0` | 每会话中继上限，`0` 表示不限 |
| `--connect-max` | `10` | 每个 IP 在窗口内的连接尝试上限 |
| `--connect-window` | `60` | 连接尝试计数窗口，单位秒 |
| `--log-dir PATH` | `%APPDATA%\PeerDesk\logs` | 覆盖文件日志目录 |
| `-v`, `--verbose` | 关闭 | 输出调试日志 |
| `--version` | — | 输出版本后退出 |

环境中可设置 `PEERDESK_BIND`、`PEERDESK_PORT`、`PEERDESK_UDP_PORT`、
`PEERDESK_TOKEN`、`PEERDESK_RELAY_MBPS` 和 `PEERDESK_LOG_DIR` 作为对应默认值。

## 客户端参数

| 参数 | 说明 |
| --- | --- |
| `--server HOST:PORT` | 信令服务器，默认取已保存偏好或 `127.0.0.1:7900` |
| `--token TOKEN` | 服务器令牌 |
| `--code CODE` | 本机被控连接码或预填连接码 |
| `--page remote\|host\|viewer\|settings` | 启动页面，默认 `remote` |
| `--background` | 登录启动模式：驻留托盘并保持本机可被控；托盘不可用时显示窗口 |
| `--viewer-demo` | 合成 Viewer，仅能配合 `--page viewer` |
| `--capture native\|test` | 使用真实 GDI 抓屏或合成信号源 |
| `--audio-test` | 使用 440 Hz 合成音源 |
| `--screenshot PATH` | 保存窗口截图后退出 |
| `--exit-after MS` | 指定毫秒后自动退出 |
| `--log-dir PATH` | 覆盖文件日志目录；隔离模式下默认使用临时目录 |

`--capture test`、`--audio-test`、`--viewer-demo` 或截图模式会使用临时偏好，
不会覆盖日常配置。

## 安全边界

- 9 位连接码是会话预共享密钥的一部分。知道连接码且网络可达的人可以发起配对。
- 服务端保存连接码摘要，不保存用于派生的明文码；中继只转发加密后的 PDTP 数据报。
- 服务端仍能看到连接元数据、候选地址、流量大小和时序，也能丢包或阻断会话。
- `--token` 只保护信令入口，不能替代传输加密。内置信令是明文 TCP，公网部署应放在
  TLS 终结代理或可信内网之后。
- 中继限速默认按单会话令牌桶执行；超过 `--relay-mbps` 的数据报会被丢弃。
- 被控端必须在可信网络中运行，并谨慎开放键鼠、剪贴板和文件接收权限。
- 日志和诊断包只保存在本机，诊断包不会自动上传。导出时会按模式替换令牌、连接码、
  服务器地址、网络端点、Windows 路径和常见文件名，但模式化脱敏不能替代人工检查；
  分享前仍应确认内容。当前不包含 minidump 或远程诊断。

## 已知限制

- **断线恢复仍需跨机验收**：回环测试已覆盖同码重配对、保帧、输入隔离、恢复超时、挂断
  静默和文件断点续传；真实 NAT、信令长时间中断、进程重启、跨机文件续传及媒体断点续传
  尚未形成正式验收结论。
- **可靠补洞仍需真实网络验收**：回环测试已覆盖乱序补洞、重复包、窗口越界、非法 ACK、
  重传上限和重置；真实丢包网络与长时间序号回绕尚未形成正式验收结论。
- **反馈异步化仍需真实网络验收**：回环测试已覆盖 NACK、关键帧请求限频、反馈线程归属，
  以及反馈/UDP/认证流代际隔离；真实弱网和长会话下的发送时序与恢复效果尚未形成正式
  验收结论。
- **速率自适应仍需真实弱网验收**：回环测试已覆盖 RTT/NACK/自动丢包关键帧请求触发的
  质量与帧率降级、调整冷却、健康恢复和静态带宽上限叠加；真实丢包、抖动、带宽突变和
  长时间会话下的阈值与恢复节奏尚未完成参数校准。
- 真实跨机、NAT 组合、丢包、抖动和长时间会话尚未形成正式验收报告。
- 产品视频路径仍是整帧 JPEG。仓库已具备 Windows Media Foundation H.264 编解码封装，
  并通过 Annex B 编解码、关键帧和码率调整回环测试，且 PDTP 的 DATA、PARITY、NACK
  重传和接收信号已贯通 codec 元数据；但尚未接入会话协商、产品 H.264 编解码路径和
  自动回退，因此还不能宣称端到端 H.264 或硬件编码能力。
- 抓屏仅有 GDI 路径，每帧完整抓取/缩放/编码；被控端媒体线程、Viewer 解码与呈现线程、
  `PeerDeskVideoSend` 分片线程、`PeerDeskVideoReceive` 重组线程和 `PeerDeskUdpIo`
  UDP 收发/解析/认证线程以及 `PeerDeskPeerLinkRx` 已认证包处理线程已拆分，但
  窗口最终合成和大部分会话控制仍在主线程；NACK、关键帧请求和控制反馈已迁入
  `PeerDeskLinkFeedback`，但没有脏矩形、GPU 纹理或增量编码。
- 声音仍是 G.711 µ-law，适合语音，不适合音乐；Viewer 音频包校验、µ-law 解码和
  `PeerDeskAudio` 播放线程已拆分，但还没有 Opus 双栈、默认输出设备热切换或真实跨机
  音质验收。
- 输入使用 Win32 `SendInput`，注入已移至 `PeerDeskHostInput` 工作线程；游戏鼠标仍是
  应用层相对输入，没有虚拟 HID，个别游戏或受保护界面仍可能无法注入。回环测试已覆盖
  捕获、合帧、跨线程注入、停止隔离和按键释放，真实游戏兼容性仍需跨机验收。
- 分辨率受真实桌面限制；没有虚拟显示器驱动。
- 剪贴板只同步文本，图片和文件列表尚未纳入剪贴板通道。
- 最近连接是本机历史，不是账号云设备列表；还没有收藏、自动发现或二维码入口。
- Windows 安装/卸载已通过本机安装、启动截图、启动项清理和卸载冒烟；安装包尚未签名，
  Windows SmartScreen 可能提示未知发布者，自动更新、失败回滚和跨版本升级路径仍未完成。
- 当前用户登录启动项和后台托盘启动已实现并通过回环测试，但尚未完成真实 Windows
  注销/重启验收。
- 日志仅在本地滚动保留，没有日志上传、集中采集或远程诊断；异常退出提示依赖 `.running`
  运行标记，不等于崩溃转储或完整崩溃分析。
- 与 UU远程 的交互仍有差距：弱网策略、媒体断点续传和更完整的游戏输入兼容性尚待补齐。

## 路线图

1. 两台真实 Windows 主机完成直连、中继、弱网、多屏、文件、长会话、断线恢复与速率
   自适应验收。
2. 完成真实注销/重启下的登录启动验收，并补齐安装包签名、升级验证和更新机制。
3. 引入 H.264 与 Opus，并在不可用时回退到 JPEG / G.711。
4. 评估虚拟显示器、虚拟 HID，并完成真实游戏输入兼容性验收。
5. 建立正式性能基线后，继续评估 GPU/渲染线程、窗口合成和线程池的边界，
   并扩展真实跨机验收、媒体质量、队列诊断和输入兼容性；旧技术栈清理已经完成。

更细的优先级见 [路线图](docs/05-roadmap.md)。

## 文档

- [逆向分析：网易UU远程](docs/01-reverse-engineering.md)
- [架构与数据流](docs/02-architecture.md)
- [PDTP/1 协议规范](docs/03-protocol-pdtp1.md)
- [UI 与交互对齐](docs/04-ui-parity.md)
- [路线图](docs/05-roadmap.md)
- [交接手册](docs/06-handover.md)

## 许可

Apache-2.0。仅用于自有设备之间的远程访问与协议研究。网易UU远程及其他产品名称、
商标和资源归各自所有者。
