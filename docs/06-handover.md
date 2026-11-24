# 交接与运维手册

> 面向第一次接手仓库的人：先用 10 分钟完成构建和测试，再用 1 小时看清代码边界，
> 最后按优先级继续开发。本文只描述当前仓库真实具备的能力，不把规划项写成已完成。

最后更新：2026-11-24 · 版本 `0.9.0` · 技术栈：C++20、Qt 6.8 Widgets、CMake、Ninja

## 1. 当前状态

PeerDesk 是 Windows 远程桌面项目。当前服务端、UDP 传输、加密、会话控制与 Qt
客户端都位于同一套原生 C++ 工程中，默认分支为 `main`。

仓库只保留原生实现：旧 Python 包、Web UI、PyInstaller 入口、pytest/e2e 脚本及其缓存
已删除。构建、测试和 CI 不再需要 Python、Node.js 或旧 Web 服务；唯一技术栈是 C++20、
Qt 6.8、CMake 和 Ninja。

已经接通的主路径：

- `peerdesk-server`：TCP 信令、UDP 打洞辅助、中继、STUN、令牌、连接频次限制和每会话限速。
- `peerdesk-client`：9 位连接码、被控端控制、Viewer、键鼠与文本、文本/PNG 图片剪贴板、
  声音、文件传输、多显示器、设置、系统托盘、窗口状态持久化、单实例窗口唤回和当前
  用户登录启动。
- PDTP/1：X25519 握手、HKDF 派生、AEAD 数据包、重放窗口、视频分片/FEC/NACK、
  可靠控制流和音频数据包。
- Windows 平台层：GDI `BitBlt`/`StretchBlt` 抓屏、Win32 `SendInput`、剪贴板 API、
  WASAPI loopback、16 kHz 单声道 PCM16 与 G.711 µ-law。
- Windows 安装器：Inno Setup 每用户安装，部署客户端、服务端、Qt/MinGW 运行库和文档；
  提供中文关键向导文案、开始菜单快捷方式、可选桌面快捷方式和可验证的卸载清理。
- 日志与诊断：客户端和服务端写入滚动文件日志；客户端设置页可打开日志目录、导出脱敏 ZIP
  诊断包，并在发现上次异常退出标记时提示。诊断包不会自动上传，不包含 minidump。

Windows 媒体管线优先使用 Media Foundation H.264，编码不可用时自动回退整帧 JPEG。
抓屏、缩放、静帧检测和 H.264/JPEG 编码运行在 `PeerDeskVideo` 媒体线程，Viewer
H.264/JPEG 解码运行在 `PeerDeskViewerVideo` 媒体线程；后者用最新帧覆盖旧待解码帧，
保留尚未交付的关键帧，并在 H.264 首个关键帧前丢弃增量帧。Viewer 画面缩放运行在
`PeerDeskViewerPresent`
工作线程，按窗口与设备像素比生成物理像素目标，连续收帧和缩放采用最新值背压；UI 线程
只执行最终 `drawImage`，源图尺寸仍用于输入坐标归一化。Viewer 音频包校验和 G.711
µ-law 解码运行在
`PeerDeskViewerAudio` 工作线程，按 FIFO 处理，不做最新帧覆盖；PCM16 随后进入
`PeerDeskAudio` 工作线程，由该线程独占最长 360 ms 待播队列、10 ms 泵和 `QAudioSink`。
视频分片、XOR FEC 生成和 NACK 重传缓存检索运行在 `PeerDeskVideoSend` 工作线程，
待发送帧同样采用最新值覆盖并保护待处理关键帧；入站分片解析、重组、XOR FEC 缺片恢复、
超时丢帧和 NACK 判定运行在 `PeerDeskVideoReceive` 工作线程，结果排队回主线程后由
`PeerLink` 更新统计并发布反馈路由快照。视频 DATA/PARITY 用 `FlagH264` 区分
JPEG/H.264，NACK 重传保留 codec 与关键帧标志，跨线程发送/重组和 Viewer 帧信号也
携带 codec。`PeerDeskLinkFeedback` 工作线程异步发送 NACK、
Pong、ACK 和控制 DATA，关键帧请求经认证可靠控制流提交并限制为每 200 ms 一次。
`PeerDeskUdpIo` 工作线程独占 UDP socket 和唯一加密上下文，完成绑定/关闭、
入站类型识别、RLY1 解包、包头解析、接收侧 AEAD/重放检查、发送侧 AEAD、发送序号、
RLY1 封装、实际收写和原子发送统计。只有已认证包会回到主线程，`PeerLink` 完成路径
锁定、统计和会话衔接后，把已认证包排队交给 `PeerDeskPeerLinkRx`。该 worker 独占
控制分片、可靠控制流、ACK 状态、PING/RTT、音视频转发和 Bye 处理，并通过
`LinkWireCommand` 把 ACK、Pong 和控制 DATA 交给反馈线程。反馈、UDP 接收和认证流
代际必须同时匹配，重置或停止后迟到的任务会被丢弃。
`HostSession` 每秒把最近窗口的 RTT、NACK 帧/分片和带 `loss: true` 的自动关键帧
请求交给 `AdaptiveVideoRateController`，动态收紧 `VideoPipeline` 的编码质量与
有效 FPS 上限；最小调整间隔为 0.9 秒，链路持续健康 4 秒后逐步恢复。用户画质/FPS
和 `--relay-mbps` 仍作为外部上限。Host 统计消息新增
`nack`、`nack_shards`、`keyreq`、`aq`、`afps` 和 `health`，便于回环和后续跨机
验收观察。
被控端键鼠注入运行在 `PeerDeskHostInput` 工作线程，UI 线程只解析控制消息并提交命令；
连续绝对移动合并，停止会立即拒绝并清空待处理队列，再等待 worker 释放按键与鼠标按钮。
文件发送分块、接收 Base64 解码/校验、临时文件读写与提交由 `PeerDeskFileTransfer`
工作线程串行处理，UI 线程只选择文件、展示进度并排队提交控制消息；WASAPI loopback
另有采集线程。Windows `src/codec/` 提供 Media Foundation H.264 Annex B 编解码封装，
支持关键帧、动态码率和 RGB32/NV12 转换。Viewer 在会话 `hello.media.codecs` 中声明
H.264/JPEG 能力，Host 在自身 codec 可用时选择 H.264，并通过
`PeerDeskVideo`/`PeerDeskViewerVideo` 完成编解码；编码器启动或编码失败会自动回退
JPEG。首个 H.264 关键帧前不输出增量帧，解码失败后重置并等待下一关键帧。尚未接入
GPU 抓屏、脏矩形、H.264 硬件编码策略、真实跨机媒体性能结论或 Opus。

文件发送支持主动取消：发送端最多发出一次 `file_cancel`，接收端回滚临时文件并回执
`file_cancelled`。接收失败、本地读写失败与断链中断使用不同事件；断链只清理本机状态，
不会误发“用户取消”。Viewer 发送侧已接入多文件队列：同一时间发送一个文件，成功自动
推进，失败项保留并可整文件重试，取消当前项后继续剩余项。链路中断后，发送端保留单文件
续传断点，接收端保留未完成临时文件最多 10 分钟；恢复实况后发送端以新传输 ID 接管旧
临时文件，接收端校验旧 ID、文件名和大小后返回实际偏移，发送端确认后才继续发送尾部。
8 秒未确认或断点失效时本次续传失败，可由用户显式整文件重试。被控端现在与 Viewer
共用 UI 侧文件队列：多选或拖放文件立即入队，同一时间发送一个文件，成功自动推进，
失败项保留并可重试，取消当前项后继续剩余项；链路断开时暂停并中断当前项，重新进入
实况后恢复队列并接管自动续传。接收端为同名目标分配唯一落盘名；冲突名接近 255 字符
basename 上限时缩短主干并保留扩展名与序号。传输存续期间预留路径，同名同大小文件
各自保留断点，不会互相覆盖或串用续传偏移。发送端的文件开始事件携带绝对源路径，
`OutgoingFileQueue` 接管等待项或中断项时先按规范化源路径匹配，再校验文件名和大小；
不同目录下的同名同大小文件不会串用队列进度，源路径为空仅保留旧调用兼容回退。
发送和接收两侧的文件 I/O 由 `PeerDeskFileTransfer` worker 执行，文件数据到达会话层后
只排队进入该线程；接收开始、进度、完成和失败通过排队信号回到主线程。

Viewer 已接入回环验证过的断线恢复：实况中断后保留末帧、释放输入、停止旧会话，
30 秒内按同一连接码重新配对和握手；Host 在链路关闭后回到 `Waiting` 并保留注册。
Viewer 文件队列在恢复期间保留批次、进度并暂停，重连成功后优先从接收端确认的偏移续传；
Host 发送侧在重新进入实况时自动续传。旧媒体队列不保留。

剪贴板已从纯文本扩展为文本和 PNG 图片双向同步。文本上限 256 KiB；PNG 编码后上限
4 MiB，单边不超过 8192 像素、总像素不超过 1600 万。Host 每 400 ms 优先轮询图片，
按规范化 RGBA8888 像素和宽高计算 SHA-256 指纹去重；Viewer 的 Ctrl+V 与工具栏入口
也图片优先，无图片时回退文本。双端严格校验 Base64、PNG 魔数、声明尺寸和实际解码
尺寸，非法图片只记日志且不会覆盖当前剪贴板。Windows 侧读取 `CF_DIBV5`/`CF_DIB`，
写入 `CF_DIB`。当前图片轮询、PNG 编解码和 DIB 转换仍在主线程；HTML/RTF、文件列表、
超限大图和真实跨机大图时延尚未验收。

Viewer 工具栏使用 160 ms 透明度动画：鼠标活动时淡入，静止超时后淡出并隐藏；恢复、
失败和结束遮罩会立即隐藏工具栏，避免动画与遮罩叠加。该行为已由 UI 自动化覆盖。

Viewer 已实现应用层游戏鼠标：勾选工具栏按钮后隐藏本机光标，按 16 ms 合并相对位移，
发送 `act: "rel"`；按键使用 `reldown`/`relup`，不更新绝对坐标。Windows 下光标接近
画面边缘时回中并过滤伪位移。`Esc` 优先退出游戏鼠标；失焦、恢复、失败、结束和会话
重置会退出模式并释放输入。传输、控制器、会话和 UI 回环测试均已覆盖。

连接页已接入本地最近连接：Viewer 仅在真正进入 `Streaming` 后记录最近 4 个对端，
按“连接码 + 服务器”去重并把最新记录置顶。点击记录只回填连接码和服务器，保留当前
令牌，不自动连接；失败或仍在连接的尝试不会写入。记录保存在本机 Qt 偏好中，不包含
服务器令牌，可从连接页一键清除。

连接页还提供“常用设备”2×2 卡片区：可从最近记录用星标收藏最多 4 个对端，按
“连接码 + 规范化服务器”大小写不敏感去重，最新收藏置顶。点星标只切换收藏，不会
回填或发起连接；常用设备卡单击会复用当前服务器令牌直接连接，卡片上的星标可取消
收藏。收藏仅保存在本机，不包含令牌；列表为空时整个区域隐藏，忙碌或已连接时快速
操作禁用。偏好持久化以及连接页/窗口的收藏、取消和快速连接路径均有自动化测试。

普通客户端启动使用用户级锁保证单实例。第二次启动会连接本机命名管道发送唤醒请求，
由已有进程恢复、置顶并激活窗口，然后立即退出；不会创建第二个托盘图标。正常退出会
释放锁，异常终止后新进程也能识别并回收陈旧锁。`--screenshot`、`--capture test`、
`--audio-test` 和 `--viewer-demo` 属于隔离模式，不参与单实例锁，可并行运行。

设置对话框可维护当前 Windows 用户
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下的 `PeerDesk` 值。启用时写入
带引号的绝对可执行路径加 `--background`，关闭时删除该值，不需要管理员权限。
`--background` 仍参与单实例锁，会开始本机被控并在托盘可用时隐藏窗口；托盘不可用时
回退显示主窗口。注册表读写、UI 开关和后台启动已有回环测试，但尚未完成真实 Windows
注销/重启验收。

以下能力尚未完成，不应写入发布说明：

- 真实跨机、NAT、弱网、信令长时间不可用和进程重启下的恢复验收，以及跨机文件续传和
  媒体断点续传。
- 真实登录/重启后的启动项、托盘驻留和窗口回退验收。
- 安装包代码签名、自动更新、失败回滚、跨版本升级和正式发布渠道。
- 两台真实 Windows 主机上的系统验收与正式性能基线。
- H.264 在复杂桌面、视频播放、弱网、带宽突变和长会话中的画质/延迟/CPU 验收，
  以及硬件编码策略。
- 虚拟显示器、虚拟 HID、广泛游戏兼容性、真实弱网下的速率自适应调参和较长会话的
  稳定性证明。
- 跨机连续图片剪贴板、大图时延、HTML/RTF 和文件列表剪贴板；当前只承诺回环验证过的
  文本与受限 PNG 图片。

## 2. 本地构建

### 2.1 前置条件

| 组件 | 当前验证版本 |
| --- | --- |
| Windows | Windows 10/11 |
| Qt | 6.8.3，`mingw_64` 与 `qtmultimedia` |
| 编译器 | MinGW 13.1，`mingw1310_64` |
| CMake | 3.24 或更新 |
| 构建器 | Ninja |
| 安装器 | Inno Setup 6（仅打包时需要） |

把 Qt 和 MinGW 的 `bin` 放到 `PATH` 最前面，然后使用仓库预设：

```powershell
$env:PATH = "C:\Qt\6.8.3\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;$env:PATH"
cmake --preset windows-mingw
cmake --build --preset windows-mingw
ctest --preset windows-mingw --output-on-failure
```

构建产物位于 `build/cpp/`：

- `peerdesk-server.exe`
- `peerdesk-client.exe`
- `peerdesk-link-harness.exe`

CMake 预设中的 Qt 路径若与你的安装位置不同，请在本机预设中调整，不要提交机器专属路径。

### 2.2 CTest

测试共 15 个套件：

```text
peerdesk-core
peerdesk-crypto
peerdesk-transport
peerdesk-stun
peerdesk-server
peerdesk-peer-link
peerdesk-session
peerdesk-signaling-client
peerdesk-app-controller
peerdesk-audio-output
peerdesk-single-instance
peerdesk-ui
peerdesk-logging
peerdesk-platform
peerdesk-codec
```

常用命令：

```powershell
ctest --preset windows-mingw --output-on-failure
ctest --preset windows-mingw -R peerdesk-transport --output-on-failure -V
ctest --preset windows-mingw -R peerdesk-ui --output-on-failure -V
```

Qt UI 测试必须通过 CTest 执行。CTest 会设置离屏平台并收集退出码；直接启动 GUI 测试
程序时，QtTest 摘要不一定会显示在当前 PowerShell 中。

### 2.3 Windows 安装包

安装链路位于 `packaging/windows/`。构建 Release 客户端和服务端后运行：

```powershell
$env:PATH = "C:\Qt\6.8.3\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;$env:PATH"
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release `
  --target peerdesk-client peerdesk-server
.\packaging\windows\build-installer.ps1
```

脚本会：

1. 从 `CMakeLists.txt` 读取版本，并清理 `dist/stage`。
2. 执行 `cmake --install`，只安装 `peerdesk-client.exe`、`peerdesk-server.exe`、
   `LICENSE`、`README.md` 和 `USER-GUIDE.txt`。
3. 用 `windeployqt` 部署 Qt DLL、平台插件（含安装冒烟使用的离屏插件）、图像格式、
   多媒体和 TLS 插件，并补齐 MinGW 运行库。
4. 拒绝测试程序、PDB、ILK 等开发文件进入载荷。
5. 生成带 BOM 的 Inno Setup 脚本并输出 `dist/PeerDesk-<version>-setup.exe` 与
   `dist/SHA256SUMS.txt`。

验证实际安装包：

```powershell
.\packaging\windows\test-installer.ps1 `
  -InstallerPath .\dist\PeerDesk-0.9.0-setup.exe
```

该脚本会静默安装到 `dist/installer-test/app`，验证客户端、服务端、卸载器、Qt 平台
插件、用户指南和开始菜单快捷方式；运行服务端 `--version`；使用
`QT_QPA_PLATFORM=offscreen` 启动安装版客户端并保存 PNG 截图；随后写入测试启动项，
静默卸载并检查程序、快捷方式、卸载注册表项和启动项已清除，同时确认
`%APPDATA%\PeerDesk` 标记文件仍存在。测试前不能已有同 `AppId` 的每用户安装。

本机 Inno Setup 常见路径为
`%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe` 或
`%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe`；脚本也接受 `-IsccPath`。产物未签名，
不能把本机冒烟结果描述成已发布或已通过真实升级验收。

## 3. 运行方式

### 3.1 服务端

默认监听 TCP `7900` 和 UDP `7901`：

```powershell
.\build\cpp\peerdesk-server.exe --port 7900 --udp-port 7901
```

常用参数：

| 参数 | 用途 |
| --- | --- |
| `--token <value>` | 要求信令客户端携带相同令牌 |
| `--relay-mbps <n>` | 限制单个会话的中继带宽 |
| `--connect-max 10` | 限制时间窗口内的连接尝试次数 |
| `--connect-window 60` | 设置连接频次窗口的秒数 |
| `--log-dir <path>` | 覆盖日志目录；也可设置 `PEERDESK_LOG_DIR` |
| `-v` | 输出更详细的连接、候选和中继日志 |

跨机使用时必须放行 TCP `7900` 与 UDP `7901`，或显式使用其他可达端口。

### 3.2 本机联调

```powershell
# 终端 1
.\build\cpp\peerdesk-server.exe --port 7900 --udp-port 7901 --token local-dev

# 终端 2：被控端
.\build\cpp\peerdesk-client.exe --server 127.0.0.1:7900 --token local-dev `
  --page host --code 604173829

# 终端 3：控制端
.\build\cpp\peerdesk-client.exe --server 127.0.0.1:7900 --token local-dev `
  --page remote --code 604173829
```

客户端常用参数：

| 参数 | 用途 |
| --- | --- |
| `--server <host:port>` | 指定信令服务器 |
| `--token <value>` | 指定服务端令牌 |
| `--code <9 digits>` | 预填连接码 |
| `--page remote\|host\|viewer` | 指定启动页面 |
| `--background` | 登录启动模式：驻留托盘并保持本机可被控；托盘不可用时显示窗口 |
| `--viewer-demo` | 使用 Viewer 合成画面，禁止真实输入、文件与音频副作用 |
| `--capture native\|test` | 选择真实 GDI 抓屏或合成屏幕 |
| `--audio-test` | 使用合成测试音源 |
| `--screenshot <path>` | 渲染页面后保存截图 |
| `--exit-after <ms>` | 按指定延时退出，便于自动化检查 |
| `--log-dir <path>` | 覆盖日志目录；隔离模式默认写入临时目录 |

调试 UI 时建议使用 `--capture test --audio-test`。此时被控端不会自动开始共享，
需要在界面中点击“开始被控”；`--viewer-demo` 只显示合成画面，不会操作真实桌面。

## 4. Windows CI

`.github/workflows/ci.yml` 在 `windows-latest` 上执行完整原生检查和测试：

1. `build-and-test` 安装 Qt 6.8.3、`qtmultimedia` 和 MinGW 13.1，校验工具链。
2. 使用 `windows-mingw-ci` 预设配置、构建并运行全部 CTest。
3. `package` 安装 Qt 与 Inno Setup，使用 `windows-mingw-ci-release` 构建客户端和服务端。
4. 生成 Release 安装包并执行静默安装、安装版客户端离屏截图和静默卸载。
5. 成功后上传安装包与 SHA-256 清单；失败时上传 CTest、安装和卸载诊断日志。

CI 会在推送到 `main`、Pull Request 和手动触发时运行。推送后本地可检查：

```powershell
gh run list --branch main --limit 3
gh run watch <run-id> --exit-status
```

## 5. 日志与诊断

客户端和服务端默认把日志写入 `%APPDATA%\PeerDesk\logs`，文件名分别以
`peerdesk-client` 和 `peerdesk-server` 开头。每行包含 UTC 时间、级别、PID 和线程 ID；
单文件达到 4 MiB 后轮转，默认保留 5 个日志文件。控制台仍同步输出。服务端可用
`--log-dir` 或 `PEERDESK_LOG_DIR` 覆盖目录，客户端可用 `--log-dir`；截图、合成采集、
合成音源和 demo 使用隔离临时目录，且不跟踪异常退出标记。

设置页“日志与诊断”提供“打开日志目录”和“导出诊断包…”：

- 客户端诊断包只读取 `peerdesk-client*.log`，不会混入同目录的服务端日志。
- ZIP 是无压缩归档，包含 `manifest.json`、`settings-summary.json` 和日志副本。
- 默认上限 20 MiB；超出时跳过部分日志并在清单中标记 `truncated`。
- 设置摘要只记录布尔值和数值，如服务器/令牌/连接码是否配置、画质、帧率、权限开关，
  不写入地址、令牌、连接码、接收目录或路径。
- 日志在写入和导出时都会按模式脱敏，包括令牌、Bearer、password/secret/psk、连接码、
  URL、IPv4/IPv6、`host:port`、Windows 路径和常见文件名。
- 诊断包只保存在用户选择的位置，不会自动上传；模式化脱敏不能替代人工检查，分享前仍应
  打开归档抽查。当前没有 minidump、集中日志或远程诊断。

客户端会在日志目录创建 `<应用>.running` 标记，正常退出时删除。启动时若发现陈旧标记，
会在主窗口显示 toast，并在设置页显示“上次运行未正常退出”提示。该提示只代表上次未正常
收尾，不代表已经完成崩溃归因。

CI 仅在测试失败时上传 CTest 日志；不会运行或上传用户诊断包。

Viewer HUD 显示窗口尺寸、帧数和带宽信息。`--viewer-demo` 中的 HUD 数字是固定演示值，
不能当作性能测试结果。需要真实指标时，应使用两台主机、真实采集和固定网络条件测试。

## 6. PDTP/1 速查

完整格式以 `docs/03-protocol-pdtp1.md` 为准。

- 包头固定 20 字节，使用小端序；`ts_ms` 是相对会话起点的 `uint32` 毫秒。
- 包类型包括 hello、hello_ack、hello_fin、punch、punch_ack、data、parity、ack、nack、
  ping、pong、bye 和 audio。
- stream 分为 video、control、file、stats、audio；控制、文件与统计使用可靠消息语义。
- 网络包 `seq` 是严格单调的 AEAD nonce 计数器；控制消息自身序号位于加密载荷内。
- HELLO transcript 必须绑定双方原始 hello 字节，重发时必须复用同一份缓存。
- 候选交换后约每 100 ms 发送 PUNCH；约 6 秒仍无法直连则自动升级中继。
- 中继数据包带 `RLY1` 前缀和配对标识；信令走 TCP JSON Lines，媒体走 UDP。
- 媒体 datagram 上限为 1400 字节；视频分片和 parity 头都计入 payload MTU。
- AEAD 关联数据包含包头与 stream 等上下文；同一密钥下不得重复 nonce。

可靠控制流使用累积 ACK 加 64 位选择性位图补洞。位图语义为
`bit i = cumulative + 1 + i`；未收到序号 0 时用 `0xFFFFFFFF` 表示尚无可累积确认。
接收窗口限制为 256 个序号，重复、过旧和窗口外数据只触发 ACK 重发，不会重复交付。

## 7. 代码地图

| 路径 | 职责 |
| --- | --- |
| `src/core/` | PDTP/1 协议、加密、可靠流、视频、STUN 与通用工具 |
| `src/transport/` | `PeerLink` 握手、打洞、直连/中继、UDP I/O、已认证包处理、链路统计和视频发送/接收流水线 |
| `src/server/` | TCP 信令、UDP 中继、STUN、令牌、频次限制与中继限速 |
| `src/client/` | TCP JSON Lines 信令客户端 |
| `src/codec/` | Windows Media Foundation H.264 Annex B 编解码、关键帧与码率调整封装 |
| `src/session/` | 被控/Viewer 会话、`VideoPipeline` 媒体线程、`HostInputPipeline` 输入线程、音视频处理和文件传输 |
| `src/app/` | 页面控制器、偏好、最近连接、单实例守卫、画质决策、音频播放和错误状态 |
| `src/diagnostics/` | 文件日志、轮转、脱敏、运行标记、诊断服务和无压缩 ZIP 打包 |
| `src/ui/` | 连接页、被控端页、Viewer、设置对话框、托盘与窗口壳 |
| `src/platform/` | GDI 抓屏、WASAPI、Win32 输入、剪贴板和当前用户启动项 |
| `apps/peerdesk-server/` | 服务端进程入口 |
| `apps/peerdesk-client/` | Qt 客户端入口、启动参数、demo 和截图流程 |
| `apps/peerdesk-link-harness/` | 双端 UDP 链路诊断工具 |
| `packaging/windows/` | Windows 版本资源、Inno Setup 模板、打包脚本、安装冒烟和用户说明 |
| `tests/` | 15 个 CTest 套件、跨进程单实例宿主与共享测试支持 |

修改链路问题时，先从 `src/transport/peer_link.*` 和 `src/core/protocol.*` 开始；
修改界面与会话状态时，从 `src/app/viewer_controller.*`、`src/session/viewer_session.*`
和 `src/ui/viewer_page.*` 开始。

## 8. 故障排查

| 现象 | 检查项 |
| --- | --- |
| 启动提示缺少 Qt DLL | 确认 Qt 与 MinGW 的 `bin` 位于 `PATH` 最前，或完整部署运行库 |
| 一直停留在“正在打洞” | 服务端加 `-v` 查看候选与中继；检查 UDP 端口和防火墙 |
| 信令连接失败 | 核对服务端地址、TCP 端口和双方令牌是否一致 |
| 连接后立即断开 | 检查 UDP 可达性、链路日志和 `RX_TIMEOUT` |
| Viewer 一直显示“正在恢复” | 确认同一连接码的 Host 已重新上线、信令 Token 一致且 UDP 可达 |
| 恢复 30 秒后结束 | 查看 Host 是否仍注册、连接码是否变化；回环测试可用缩短的恢复期限复现 |
| 有画面但键鼠不动 | 检查被控端是否允许控制、Viewer 是否获得焦点、安全桌面是否阻止注入 |
| 游戏鼠标不移动或卡键 | 确认会话正在实况、工具栏按钮已勾选；按 `Esc` 退出后重试，并检查受保护界面或游戏是否拦截注入 |
| 画面卡住或帧率很低 | 检查抓屏状态、发送队列、NACK 与关键帧请求；先用合成采集排除桌面问题 |
| 声音异常 | 检查 Windows 默认播放设备、WASAPI loopback 和测试音源 |
| 剪贴板不同步 | 检查被控端剪贴板开关；确认文本不超过 256 KiB，图片为 PNG 可达格式且不超过 4 MiB、单边 8192、总像素 1600 万 |
| 文件传输失败 | 检查源文件、目标目录和大小限制；失败项会保留，可点“重试”整文件重发 |
| 断线后文件没有续传 | 确认接收端临时文件在 10 分钟保留期内、源文件名称和大小未变，并检查 `file_resume` 确认是否在 8 秒内到达 |
| 同名文件被覆盖或断点串用 | 确认版本包含接收端路径消歧与发送端源路径匹配修复；同名目标应分别落盘，续传只从指定旧传输 ID 和对应源路径的断点继续 |
| 最近连接为空 | 只有成功进入实况后才会记录；检查偏好是否可写，并可尝试完成一次同服务器连接 |
| 最近连接没有显示预期服务器 | 记录按连接码和规范化后的服务器去重；确认端口及服务器输入是否一致 |
| 找不到日志 | 默认在 `%APPDATA%\PeerDesk\logs`；检查是否用 `--log-dir` 或 `PEERDESK_LOG_DIR` 覆盖，设置页可直接打开 |
| 诊断包缺少服务端日志 | 这是客户端诊断包的预期隔离；服务端日志需从同一日志目录单独收集 |
| 启动提示上次未正常退出 | 这是陈旧 `.running` 标记触发的提示；先导出诊断包并检查日志，再正常退出以清理标记 |
| 第二次启动直接退出 | 这是单实例预期行为；已有窗口应被置顶。若未出现，检查同一用户会话、桌面环境和实例锁 |
| 提示已有实例正在运行但看不到窗口 | 从托盘恢复，或结束残留的 `peerdesk-client.exe` 后重试；不要删除其他用户的实例锁 |
| 设置页无法启用登录启动 | 查看对话框错误文本，并检查当前用户是否有权限写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`；组策略可能限制启动项 |
| 登录后没有看到窗口 | `--background` 在托盘可用时会隐藏窗口，从托盘恢复；若托盘不可用则本应回退显示，检查 Explorer 托盘状态和应用是否被单实例唤醒 |
| 需要彻底关闭登录启动 | 在设置中关闭“登录时启动 PeerDesk”，或删除 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下名为 `PeerDesk` 的值 |
| 安装时提示缺少 Inno Setup | 安装 Inno Setup 6，或给 `build-installer.ps1` 传 `-IsccPath` 指向 `ISCC.exe` |
| 安装包启动时提示缺少 Qt 插件 | 重新运行 `build-installer.ps1`，并检查 `dist/stage/platforms/qwindows.dll` 与 `qoffscreen.dll` 是否存在 |
| 安装冒烟提示已安装 | 先从“应用和功能”卸载同一用户的 PeerDesk；测试脚本不会覆盖已有安装 |
| SmartScreen 提示未知发布者 | 当前安装包未做代码签名，这是已知发布限制；仅运行可信构建产物并核对 SHA-256 |
| UI 测试没有摘要 | 改用 `ctest --preset windows-mingw -R peerdesk-ui -V` |
| Viewer 截图为空 | 使用 `--page viewer --viewer-demo`，并留出首帧渲染时间 |

## 9. 文档截图

只在合成模式下生成截图，避免泄露真实桌面：

```powershell
.\build\cpp\peerdesk-client.exe --capture test --page remote `
  --screenshot docs\screenshot-native-connect.png

.\build\cpp\peerdesk-client.exe --capture test --page host `
  --screenshot docs\screenshot-native-host.png

.\build\cpp\peerdesk-client.exe --capture test --page viewer --viewer-demo `
  --screenshot docs\screenshot-native-viewer.png
```

截图模式使用隔离的临时偏好，不读取真实桌面，也不写入用户的正常设置。

## 10. 下一步优先级

1. 用两台真实 Windows 主机验收直连、中继、弱网、断线恢复、多屏、文件传输和长会话。
2. 建立真实网络故障注入矩阵，记录恢复期限、输入隔离、重复断线和可靠流补洞行为。
3. 建立可复现的抓屏、编码、分片、发送、解码、输入注入和端到端延迟与带宽基线，再决定
   优化顺序；评估渲染线程和线程池的收益与队列边界。
4. 产品化：真实登录/重启验收、安装包签名、自动更新、失败回滚、升级验证，以及在现有
   诊断包之上评估 minidump 与远程诊断。
5. 媒体升级：为已接入的 H.264 建立真实跨机画质、延迟、带宽、CPU 和长会话基线，
   再评估硬件编码与 Opus；失败时始终保留整帧 JPEG 与 G.711 回退。
6. 平台增强：脏矩形、GPU 共享纹理、虚拟显示器、虚拟 HID、真实游戏兼容性与真实弱网
   下的拥塞参数校准。
7. 交互低摩擦入口：在本地历史与常用设备之上评估二维码和账号云同步，明确隐私边界后
   再实现。
8. 仓库清理已完成；后续改动继续保证构建、测试、文档和发布入口只引用原生资产。

## 11. 提交前检查

```powershell
git diff --check
cmake --build --preset windows-mingw
ctest --preset windows-mingw --output-on-failure
git ls-files | Select-String -Pattern '(?i)(\.py$|\.pyi$|\.pyw$|\.pyc$|pyproject|pytest|ruff|pyinstaller|\.spec$)'
git status --short
```

只暂存本次改动涉及的文件，确认暂存区后再提交并推送。版本号位于 `CMakeLists.txt`
的 `project(... VERSION 0.9.0)`；被控端媒体线程、Viewer 音视频解码线程、音频播放线程、
视频发送/接收流水线、UDP I/O 工作线程（含接收认证、重放拒绝和接收代际隔离）、
已认证包工作线程（含控制分片/重组、可靠流 ACK、PING/RTT、NACK、BYE 和 reset 代际
隔离）以及文件 I/O 线程（含线程归属、取消、续传和异步落盘）已有回环测试，被控端输入
线程的 FIFO、连续移动合并、释放和停止丢弃也有回环覆盖，
登录启动、双向文件队列、文件断点续传与日志诊断也已完成回环测试，接收端
同名同大小文件路径消歧和发送端跨目录同名同大小队列匹配同样覆盖；Windows 安装/卸载
已通过本机静默安装冒烟。真实登录/重启、跨机续传、安装包签名、自动更新、失败回滚和
跨版本升级尚未完成，发布说明不得提前宣称。
