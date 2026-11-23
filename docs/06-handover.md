# 交接与运维手册

> 面向第一次接手仓库的人：先用 10 分钟完成构建和测试，再用 1 小时看清代码边界，
> 最后按优先级继续开发。本文只描述当前仓库真实具备的能力，不把规划项写成已完成。

最后更新：2026-11-23 · 版本 `0.7.0` · 技术栈：C++20、Qt 6.8 Widgets、CMake、Ninja

## 1. 当前状态

PeerDesk 是 Windows 远程桌面项目。当前服务端、UDP 传输、加密、会话控制与 Qt
客户端都位于同一套原生 C++ 工程中，默认分支为 `main`。

仓库只保留原生实现：旧 Python 包、Web UI、PyInstaller 入口、pytest/e2e 脚本及其缓存
已删除。构建、测试和 CI 不再需要 Python、Node.js 或旧 Web 服务；唯一技术栈是 C++20、
Qt 6.8、CMake 和 Ninja。

已经接通的主路径：

- `peerdesk-server`：TCP 信令、UDP 打洞辅助、中继、STUN、令牌、连接频次限制和每会话限速。
- `peerdesk-client`：9 位连接码、被控端控制、Viewer、键鼠与文本、剪贴板、声音、文件传输、
  多显示器、设置、系统托盘和窗口状态持久化。
- PDTP/1：X25519 握手、HKDF 派生、AEAD 数据包、重放窗口、视频分片/FEC/NACK、
  可靠控制流和音频数据包。
- Windows 平台层：GDI `BitBlt`/`StretchBlt` 抓屏、Win32 `SendInput`、剪贴板 API、
  WASAPI loopback、16 kHz 单声道 PCM16 与 G.711 µ-law。

当前媒体管线是整帧 JPEG。抓屏、编码、发包、音频播放、文件传输和大部分 UI 都在主线程；
唯一额外采集线程是 WASAPI loopback。尚未接入 GPU 抓屏、脏矩形、硬件编码、H.264 或 Opus。

文件发送支持主动取消：发送端最多发出一次 `file_cancel`，接收端回滚临时文件并回执
`file_cancelled`。接收失败、本地读写失败与断链中断使用不同事件；断链只清理本机状态，
不会误发“用户取消”。当前仍不支持断点续传、批量重试或多文件状态中心。

Viewer 已接入回环验证过的断线恢复：实况中断后保留末帧、释放输入、停止旧会话，
30 秒内按同一连接码重新配对和握手；Host 在链路关闭后回到 `Waiting` 并保留注册。
恢复不续传旧文件或媒体队列。

以下能力尚未完成，不应写入发布说明：

- 真实跨机、NAT、弱网、信令长时间不可用和进程重启下的恢复验收，以及文件/媒体断点续传。
- Windows 安装包、自动更新和可配置开机自启。
- 两台真实 Windows 主机上的系统验收与正式性能基线。
- 虚拟显示器、虚拟 HID、拥塞控制和较长会话的稳定性证明。

## 2. 本地构建

### 2.1 前置条件

| 组件 | 当前验证版本 |
| --- | --- |
| Windows | Windows 10/11 |
| Qt | 6.8.3，`mingw_64` 与 `qtmultimedia` |
| 编译器 | MinGW 13.1，`mingw1310_64` |
| CMake | 3.24 或更新 |
| 构建器 | Ninja |

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

测试共 11 个套件：

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
peerdesk-ui
peerdesk-platform
```

常用命令：

```powershell
ctest --preset windows-mingw --output-on-failure
ctest --preset windows-mingw -R peerdesk-transport --output-on-failure -V
ctest --preset windows-mingw -R peerdesk-ui --output-on-failure -V
```

Qt UI 测试必须通过 CTest 执行。CTest 会设置离屏平台并收集退出码；直接启动 GUI 测试
程序时，QtTest 摘要不一定会显示在当前 PowerShell 中。

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
| `--viewer-demo` | 使用 Viewer 合成画面，禁止真实输入、文件与音频副作用 |
| `--capture native\|test` | 选择真实 GDI 抓屏或合成屏幕 |
| `--audio-test` | 使用合成测试音源 |
| `--screenshot <path>` | 渲染页面后保存截图 |
| `--exit-after <ms>` | 按指定延时退出，便于自动化检查 |

调试 UI 时建议使用 `--capture test --audio-test`。此时被控端不会自动开始共享，
需要在界面中点击“开始被控”；`--viewer-demo` 只显示合成画面，不会操作真实桌面。

## 4. Windows CI

`.github/workflows/ci.yml` 在 `windows-latest` 上执行完整原生检查和测试：

1. 安装 Qt 6.8.3、`qtmultimedia` 和 MinGW 13.1。
2. 校验 CMake、Ninja 与 MinGW。
3. 使用 `windows-mingw-ci` 预设配置。
4. 使用同一预设构建并运行 CTest。
5. 失败时上传 `build/cpp/Testing` 日志。

CI 会在推送到 `main`、Pull Request 和手动触发时运行。推送后本地可检查：

```powershell
gh run list --branch main --limit 3
gh run watch <run-id> --exit-status
```

## 5. 日志与诊断

服务端和客户端当前都把日志写到启动它们的控制台，没有独立日志目录，也没有滚动日志文件。
排查问题时先保留完整控制台输出；CI 仅在测试失败时上传 CTest 日志。

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
| `src/transport/` | `PeerLink` 握手、打洞、直连/中继、媒体发送和链路统计 |
| `src/server/` | TCP 信令、UDP 中继、STUN、令牌、频次限制与中继限速 |
| `src/client/` | TCP JSON Lines 信令客户端 |
| `src/session/` | 被控会话、Viewer 会话、音视频处理和文件传输 |
| `src/app/` | 页面控制器、偏好、画质决策、音频播放和错误状态 |
| `src/ui/` | 连接页、被控端页、Viewer、设置对话框、托盘与窗口壳 |
| `src/platform/` | GDI 抓屏、WASAPI、Win32 输入和剪贴板 |
| `apps/peerdesk-server/` | 服务端进程入口 |
| `apps/peerdesk-client/` | Qt 客户端入口、启动参数、demo 和截图流程 |
| `apps/peerdesk-link-harness/` | 双端 UDP 链路诊断工具 |
| `tests/` | 11 个 CTest 套件与共享测试支持 |

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
| 画面卡住或帧率很低 | 检查抓屏状态、发送队列、NACK 与关键帧请求；先用合成采集排除桌面问题 |
| 声音异常 | 检查 Windows 默认播放设备、WASAPI loopback 和测试音源 |
| 剪贴板不同步 | 检查被控端剪贴板开关，并确认内容是否为当前支持的文本 |
| 文件传输失败 | 检查源文件、目标目录、大小限制和是否已有传输在进行 |
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
3. 建立可复现的抓屏、编码、端到端延迟与带宽基线，再决定优化顺序。
4. 产品化：安装包、自动更新、开机自启策略、崩溃诊断和版本升级路径。
5. 媒体升级：H.264 与 Opus 动态接入，失败时保留整帧 JPEG 与 G.711 回退。
6. 平台增强：脏矩形、GPU 共享纹理、虚拟显示器、虚拟 HID 与拥塞控制。
7. 仓库清理已完成；后续改动继续保证构建、测试、文档和发布入口只引用原生资产。

## 11. 提交前检查

```powershell
git diff --check
cmake --build --preset windows-mingw
ctest --preset windows-mingw --output-on-failure
git ls-files | Select-String -Pattern '(?i)(\.py$|\.pyi$|\.pyw$|\.pyc$|pyproject|pytest|ruff|pyinstaller|\.spec$)'
git status --short
```

只暂存本次改动涉及的文件，确认暂存区后再提交并推送。版本号位于 `CMakeLists.txt`
的 `project(... VERSION 0.7.0)`；安装器与自动更新尚未实现，发布说明不得提前宣称。
