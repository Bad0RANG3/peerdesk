# PeerDesk 架构

PeerDesk 0.9 是 **C++20 + Qt 6.8** 的 Windows 远程桌面。服务端、传输、会话、
平台层和客户端界面共用同一套原生代码；媒体通过自研 **PDTP/1** UDP 协议传输，
信令使用 TCP JSON Lines。

```text
被控端 peerdesk-client ──TCP 信令──▶ peerdesk-server ◀──TCP 信令── 控制端 peerdesk-client
                         └──── PDTP/1 over UDP：直连或 RLY1 中继 ────┘
STUN 与中继共用 peerdesk-server 的 UDP 端口
```

## 1. 设计原则

- **连接码优先**：输入 9 位码即可建立会话，不引入账号体系。
- **直连优先**：先交换主机地址和 STUN 反射地址，打洞失败后自动升级中继。
- **端到端加密**：连接码参与密钥派生，中继只转发 AEAD 密文。
- **控制可靠、媒体可丢**：键鼠/文件使用可靠消息；视频使用 FEC/NACK；音频使用帧内冗余。
- **原生交互**：连接、被控、会话、设置和托盘都在 Qt 窗口内完成。
- **单实例**：普通启动用本机锁与命名管道保证只有一个客户端；再次启动只唤醒现有窗口。
- **可关闭的登录启动**：当前 Windows 用户可选择登录后以后台模式驻留托盘并保持可被控。
- **可自托管**：一个服务端进程同时提供信令、STUN 和中继，便于自有网络部署。
- **每用户交付**：安装器不需要管理员权限，同时部署客户端、服务端、Qt 与 MinGW 运行库，
  并提供可验证的卸载与用户数据保留边界。

## 2. 进程与模块

### 2.1 可执行程序

| 程序 | 作用 |
| --- | --- |
| `peerdesk-server.exe` | TCP 信令、连接码注册与配对、UDP STUN、UDP 中继、令牌和限速 |
| `peerdesk-client.exe` | Qt 控制端和被控端，内含连接页、被控端面板、Viewer、设置和托盘 |
| `peerdesk-link-harness.exe` | 链路联调工具，用于构造和观察 `PeerLink` 行为 |

正式安装包只包含 `peerdesk-server.exe`、`peerdesk-client.exe`、运行库、许可与用户文档，
不会携带测试程序、链路联调工具、PDB 或中间构建文件。

### 2.2 静态库

| 路径 | 职责 |
| --- | --- |
| `src/core/` | 20 字节报文头、JSON 载荷、视频/控制分片、XOR FEC、可靠流、STUN、X25519/HKDF/AEAD、重放窗口 |
| `src/transport/` | `PeerLink` 握手、候选、打洞、中继、保活、统计、链路状态和视频发送/接收流水线 |
| `src/server/` | TCP JSON Lines 信令、UDP 中继、STUN、连接频次限制和每会话令牌桶 |
| `src/client/` | TCP 信令客户端、注册/配对、候选更新和错误映射 |
| `src/session/` | 被控端/控制端会话、视频、音频、输入、剪贴板、文件和统计 |
| `src/app/` | `HostController`、`ViewerController`、偏好、单实例守卫、五档画质、音频播放 |
| `src/diagnostics/` | 滚动文件日志、脱敏、运行标记和无压缩 ZIP 诊断包 |
| `src/ui/` | Qt 窗口壳、连接页、被控端页、Viewer、设置、主题和通用控件 |
| `src/platform/` | GDI 抓屏、WASAPI loopback、PCM 重采样、Win32 输入、剪贴板和当前用户启动项 |
| `apps/`、`tools/` | 两个正式程序和链路联调入口 |

平台层当前只实现 Windows 路径；非 Windows 构建可以保留合成信号源，但不具备真实桌面采集、
系统声音、输入注入或剪贴板桥接。

## 3. 控制面与数据面

### 3.1 信令与配对

1. 被控端向服务器发送 `register`，携带 9 位数字码的摘要、名称和本地 UDP 候选。
2. 服务器保存注册信息，返回 `registered`、STUN/中继地址、令牌要求和带宽配置。
3. 控制端发送 `connect`，服务器检查在线状态、令牌和连接频次。
4. 被控端收到 `viewer`，控制端收到 `peer`，双方获得 `sid`、对端候选、
   中继地址和各自的中继 tag。
5. 候选变化通过 `cands`/`peer_cands` 增量同步；额外路径通过
   `relay_hello`/`peer_relay_hello` 保持中继可达。
6. 任一端 `bye` 或 TCP 断开时，服务器向另一端发送 `peer_bye`/`peer_gone`/
   `viewer_gone` 并释放配对。

Viewer 的信令客户端支持自动重连；服务端返回 `offline`/`not_found` 时保持 Viewer
角色，并通过指数退避重新请求配对，而不是进入终态。相同的 `sid` 再次配对只刷新候选，
不会重复触发新的 `peerJoined`。重试首帧延迟默认 250 ms，用于避开被控端 UDP `Bye`
已经到达、信令服务器尚未处理完 TCP 注销的短暂陈旧注册窗口。

曾进入实况的 Viewer 在信令断开或 UDP 链路关闭后进入 `Reconnecting`，而不是返回连接页：

- 保留连接码、对端信息和最后一帧，停止旧会话并重置 `PeerLink`。
- 只释放一次本机输入，恢复期间禁用键鼠注入。
- 在同一信令服务上按原连接码重新配对，重新完成 HELLO/AEAD 握手，恢复画质与声音设置，
  并请求关键帧。
- 默认给予 30 秒恢复期限；成功握手后回到 `Streaming`，超时才进入终态。
- 首次连接失败不发送 `sessionEnded`；实况后恢复失败最多发送一次；用户主动挂断后保持静默。

Host 在传输链路关闭后挂起旧会话并回到 `Waiting`，但保持注册和连接码不变，以便 Viewer
重新接入。双方发送侧会保留单文件续传断点：重新握手后发送端用新传输 ID 接管接收端未完成
的临时文件，接收端校验旧 ID、文件名、大小和 10 分钟保留期后返回实际偏移，发送端再从该
偏移继续。Viewer 的 UI 文件队列同时保留进度，恢复实况后自动推进。确认超时或断点失效时
失败项仍可显式整文件重试。旧媒体队列不保留，当前也没有真实跨 NAT/弱网验收结论。

### 3.2 PeerLink

`PeerLink` 是 UDP 数据面的核心，负责：

- 绑定本地 UDP socket，枚举本地 IPv4 候选，并通过服务器 STUN 发现反射地址。
- 交换 HELLO/HELLO_ACK，派生双向密钥并校验连接码验证值。
- 向全部候选每 100 ms 发送 PUNCH，收到响应后锁定直连路径。
- 6 秒未直连时把原始 PDTP 数据报封装为 `RLY1 + tag`，改走服务端中继。
- 维护 4 秒保活、握手阶段每秒加密 PING、25 秒 RX 超时和 20 ms 链路 tick。
- 接收视频分片、FEC、NACK、控制消息、音频和统计；把入站视频分片与 parity 提交给
  接收流水线，并在主线程实际发送 NACK/关键帧请求，把结果回调给会话层。

状态主线：

```text
idle ──开始握手──▶ punching ──路径响应──▶ direct
                         │
                         └─6 秒未直连──▶ relay

任意状态 ──BYE / RX 超时 / 主动关闭──▶ closed

Viewer 实况中断 ──▶ Reconnecting ──同码重新配对/握手──▶ Streaming
                         │
                         └─30 秒恢复期限──▶ Error（仅曾实况时发一次 sessionEnded）
```

`session` 包头字段是每端随机生成的链路 tag，不是信令层的 `sid`。两者不能混用。

### 3.3 视频数据流

```text
PeerDeskVideo 媒体线程：抓屏/缩放 → 静帧检测 → JPEG → FramePacer
  → queued frameReady
  → HostSession → PeerLink::sendVideo
  → PeerDeskVideoSend：视频分片 DATA + XOR PARITY
  → queued packet batch
  → queued UdpSendJob
  → PeerDeskUdpIo：包头 + AEAD + socket 写
  → UDP 直连或 RLY1 中继
  → PeerDeskUdpIo 认证 → PeerLink 协议分发
  → PeerDeskVideoReceive：分片解析/重组、XOR FEC、超时丢帧、NACK 判定
  → queued frameReady / nackReady / keyframeRequested
  → PeerLink 主线程：统计、实际发送 NACK/关键帧请求
  → PeerDeskViewerVideo 解码（保留最新帧）
  → PeerDeskViewerPresent 等比缩放（物理像素/DPR，最新帧背压）
  → 主线程 paintEvent：最终 drawImage
```

- `VideoPipeline` 在独立的 `PeerDeskVideo` 线程中执行抓屏、缩放、静帧指纹和 JPEG
  编码，并用单次定时器在每帧编码完成后串行调度下一帧。
- `HostSession` 在主线程接收编码完成的帧并交给 `PeerLink`；`PeerLink` 把 JPEG 提交到
  `PeerDeskVideoSend`，由该线程独占 `VideoSender` 并完成分片、XOR FEC 和 NACK 重传
  缓存检索。批次以排队信号回到主线程；`PeerLink` 选定路径后，把包头构造、AEAD 加密、
  发送序号和 UDP 写入排队交给 `PeerDeskUdpIo` 线程。
- `FramePacer` 根据实际帧大小调整 JPEG 质量，带宽上限同时影响质量预算和有效发帧率。
- 静帧可跳过发送；首帧和显式关键帧请求会提供新的编码基准。
- `PeerDeskVideoReceive` 在独立线程中串行执行分片解析、重组、XOR FEC 缺片恢复、
  超时丢帧和 NACK 判定；结果以排队信号回到主线程。`PeerLink` 更新接收统计并实际
  发送 NACK/关键帧请求，重传包由 `PeerDeskVideoSend` 检索缓存并构造。关键帧丢失
  会请求新的关键帧。
- Viewer 把收到的 JPEG 交给 `PeerDeskViewerVideo` 线程解码；解码慢时只保留最新帧，
  尚未交付的关键帧不会被普通帧覆盖。解码完成的 `QImage` 再交给
  `PeerDeskViewerPresent` 线程，按当前窗口与 `devicePixelRatio` 生成等比缩放后的
  物理像素图像；缩放同样只保留最新请求。主线程画布只执行最终 `drawImage`，输入坐标
  仍按源图对应的实际画面矩形归一化。

### 3.4 音频数据流

```text
WASAPI loopback 采集线程
  → 重采样为 16 kHz 单声道 PCM16
  → G.711 µ-law，20 ms/帧
  → 当前帧 + 上一帧冗余
  → TYPE_AUDIO，不可靠发送
  → PeerDeskViewerAudio：长度校验 + µ-law 解码（FIFO）
  → queued PCM16 → PeerDeskAudio：360 ms 待播队列 + 10 ms 泵
  → QAudioSink（160 ms 配置缓冲）
```

声音不进入视频 FEC 组，也不使用可靠重传。连续静音后会降低发帧频率；控制端关闭声音时，
会通知被控端停止采集，而不是只在本机静音。被控端通过带 `t: "audio"` 的状态消息回报
采集开关、编码参数、音源和错误；Viewer 以该状态更新 HUD，并在重连后重新同步。
Viewer 不丢音频包，也不做“最新帧覆盖”：`PeerDeskViewerAudio` 按到达顺序串行执行包
校验和 G.711 µ-law 解码。解码后的 PCM16 再排队到 `PeerDeskAudio` 工作线程；该线程
独占待播 FIFO、精确计时器和 `QAudioSink`。待播队列最多保留 360 ms，设备空间不足时
按 10 ms 周期继续排空；静音、重置和停止会丢弃待播音频并停止设备。

### 3.5 控制、文件与剪贴板

控制消息经独立可靠流发送，载荷内包含自己的消息序号。主要消息包括：

`hello`、`settings`、`keyframe_req`、`mouse`、`key`、`text`、`clip`、
`file_begin`、`file_resume`、`file_data`、`file_end`、`file_cancel`、
`file_cancelled`、`file_error`、`reboot`、`stats`。

- 绝对鼠标使用归一化画面坐标，被控端再映射到 Windows 虚拟桌面。
- 游戏鼠标以 `act: "rel"` 发送相对位移，Viewer 按 16 ms 合帧；`reldown`/`relup`
  只转发按钮状态，不移动绝对坐标。Windows 侧在光标接近画面边缘时回中，并过滤回中
  产生的伪位移。
- 键盘和鼠标按键使用 Win32 `SendInput`；失焦、断开、恢复、失败、结束和全屏切换会
  释放已按下的键和鼠标按钮，避免远端卡键。
- 剪贴板只同步文本，单次上限 256 KiB，双向变化均有限流。
- 文件通过 base64 控制消息分块，每块 24 KiB，单文件上限 512 MiB。
- 文件名只接受安全 basename，落盘使用临时文件加重命名，默认写入系统 Downloads
  下的 `PeerDesk` 目录。
- 同名目标会分配 `名称 (1).扩展名` 等唯一落盘路径；冲突名接近 255 字符 basename
  上限时先缩短主干，再追加序号与扩展名，且不会切断 UTF-16 代理对。临时文件存在期间
  路径同时加入预留集合，避免并发传输互相覆盖。
- 断线续传由发送端发起新 ID 的 `file_begin`（携带 `resume` 与 `resume_of`），接收端
  校验旧临时文件后以 `file_resume` 返回实际 `offset`。发送端在收到确认前不发送分块；
  8 秒未确认会发送 `file_error`（`resume_timeout`）并结束本次续传。
- 用户取消发送时只发送一次 `file_cancel`，接收端回滚临时文件与名称预留并回执
  `file_cancelled`。接收端拒绝、读写失败或校验失败时发送 `file_error`；链路关闭和
  会话停止只触发本机 `interrupted`，不会伪装成用户取消。
- Viewer 和 `HostPanel` 发送侧都用 `OutgoingFileQueue` 串行处理多文件批次：成功项
  自动推进，取消当前项后继续剩余项，失败项保留并可手动整文件重试。断线会暂停队列并
  保留批次与进度；恢复后，被中断文件优先从接收端确认的偏移续传，状态回调按传输 ID
  匹配，旧回调会被忽略。发送端 `started` 事件同时携带绝对源路径；接管被中断或等待项
  时先比较规范化源路径，再校验文件名和大小，因此不同目录下同名同大小的文件不会串用
  队列进度。源路径为空仅作为旧调用兼容回退。回环测试覆盖了跨目录同名同大小文件的
  等待/中断项接管与错误路径拒绝；真实跨机续传仍待验收。
- `reboot` 目前只返回“收到重启指令（未执行）”，不是实际的远程重启能力。

可靠性窗口、选择性 ACK 与重传细节见 [PDTP/1 协议规范](03-protocol-pdtp1.md)。

## 4. 控制器与 UI

`HostController` 和 `ViewerController` 是 Qt UI 的稳定门面：

- 管理 `SignalingClient`、`PeerLink`、`HostSession`/`ViewerSession` 的生命周期。
- 把会话状态、统计、错误、文件进度和音频状态转换为 Qt 信号。
- 保存被控端和控制端偏好，包括服务器、令牌、连接码、画质、显示器和声音选项；
  另存最近 4 个成功连接的连接码、对端名称、服务器和时间，按“连接码 + 服务器”去重。
  最近连接不包含服务器令牌，只用于连接页回填，不会自动发起连接。
- 向 UI 暴露启动、停止、挂断、切画质、切显示器、发送输入和文件等动作。

`AppWindow` 只负责页面导航和窗口级行为；`ConnectionPage`、`HostPanel`、
`ViewerPage`、`SettingsDialog` 各自处理自己的控件状态。Viewer 画布不直接操作传输层；
Viewer 与被控端的文件批次状态都由 UI 侧 `OutgoingFileQueue` 管理，控制器和会话只负责
单文件传输及其事件。

画质档位：

| 档位 | 最大宽度 | JPEG 质量 | 目标帧率 |
| --- | ---: | ---: | ---: |
| 流畅 | 1280 | 45 | 24 |
| 均衡 | 1600 | 62 | 30 |
| 高清 | 1920 | 72 | 40 |
| 超清 | 2560 | 80 | 50 |
| 原画 | 3840 | 88 | 60 |

实际帧率还会受到抓屏耗时、编码耗时和服务器中继带宽上限影响。

## 5. 线程模型

Qt 事件循环是主执行体。以下工作都在主线程完成：

- Qt 窗口、画布最终 `drawImage`、菜单、托盘和偏好读写。
- TCP 信令、已认证 UDP 包的协议分发、ACK/重传决策和画布更新。
- 文件分块、剪贴板和大部分会话控制。

`VideoPipeline` 创建一个名为 `PeerDeskVideo` 的 `QThread`。GDI 抓屏、缩放、静帧检测、
JPEG 编码和 `FramePacer` 都在该线程串行执行；启动、停止和设置更新通过 Qt 排队/阻塞
调用进入 worker，编码结果再以排队信号交回主线程。停止或重启会递增 generation，
丢弃旧线程路径尚未交付的帧。

`ViewerVideoPipeline` 创建一个名为 `PeerDeskViewerVideo` 的 `QThread`，串行执行
JPEG 解码。输入帧使用最新值覆盖策略限制积压，并保留尚未解码的关键帧；停止或重启同样
通过 generation 丢弃旧结果。

`ViewerFramePipeline` 创建一个名为 `PeerDeskViewerPresent` 的 `QThread`，串行执行
Viewer 画面的等比平滑缩放。窗口尺寸或设备像素比变化时会重新提交当前源图，缩放目标按
`viewport * devicePixelRatio` 生成物理像素尺寸；连续收帧和窗口拖动使用单槽最新值覆盖，
不在 UI 线程积压缩放任务。清帧会递增 generation 并使迟到结果失效。主线程仅把预缩放
图像绘制到现有画面矩形，源图尺寸继续作为鼠标坐标归一化基准。

`ViewerAudioPipeline` 创建一个名为 `PeerDeskViewerAudio` 的 `QThread`，串行执行音频
包长度校验和 G.711 µ-law 解码。输入使用 FIFO 队列，不丢弃尚未处理的音频包；停止或
重启通过 generation 丢弃旧结果。PCM16 以排队调用进入 `PcmAudioPlayer` 门面，由内部
名为 `PeerDeskAudio` 的 `QThread` 独占 360 ms 待播队列、10 ms 泵、错误状态和
`QAudioSink`；主线程不执行播放设备 I/O。

`VideoSendPipeline` 创建一个名为 `PeerDeskVideoSend` 的 `QThread`，由 worker 独占
唯一的 `VideoSender`，串行执行 JPEG 分片、XOR FEC 生成和 NACK 重传缓存检索。待发送
帧使用最新值覆盖策略限制积压，尚未开始处理的普通帧不会覆盖待处理关键帧；停止、重置
或重启通过 generation 丢弃旧批次。生成的数据包批次以排队信号回到主线程，再由
`PeerLink` 交给 UDP 发送管线。

`VideoReceivePipeline` 创建一个名为 `PeerDeskVideoReceive` 的 `QThread`，由 worker
独占唯一的 `VideoReceiver`，串行执行入站视频分片解析、重组、XOR FEC 缺片恢复、
超时丢帧、NACK 判定和关键帧请求判定。worker 内每 20 ms 运行一次接收器 tick；数据
提交使用排队调用，启动、停止和会话重置使用阻塞调用。停止、重启或重置会递增
generation，主线程据此丢弃旧会话路径尚未交付的帧、NACK 和关键帧请求；主线程只更新
接收统计并实际发送 NACK/关键帧请求。回环测试覆盖跨线程重组、Parity 恢复、NACK、
关键帧请求、停止隔离、重启和 reset 隔离。

`UdpIoPipeline` 创建一个名为 `PeerDeskUdpIo` 的 `QThread`，由 worker 独占唯一的
`QUdpSocket` 和唯一的 `CryptoContext`，负责绑定和关闭 socket、入站报文类型识别、
RLY1 解包、PDTP 包头解析、接收侧 AEAD 与重放检查、构造发送包头、发送侧 AEAD、
分配发送序号、封装 RLY1、写入 UDP 并维护原子发送统计。只有已解析的握手包、STUN
候选包或通过认证的加密包会以排队信号回到主线程；`PeerLink` 随后完成协议分发、
可靠流 ACK 和重传决策。会话密钥通过阻塞调用同步到 worker；每次设置或重置会话都会
递增接收代际，主线程据此丢弃重置前排队的旧包。回环测试覆盖接收认证、篡改拒绝、
重放拒绝、RLY1 包装、旧密钥失效、接收代际递增和实际 UDP 收写。

`WasapiLoopbackSource` 内部另有一个 `std::thread`，负责读取 WASAPI loopback PCM，
随后把数据交回会话路径。

代码中没有线程池、GPU 编码、硬件编码或正式吞吐基准；已认证包的协议分发、
NACK/关键帧请求的实际发送、窗口最终合成和大部分 UI 仍在主线程，不能据此宣称完整
异步媒体管线或已达到某个性能指标。

## 6. 配置、日志与运行边界

- 正常模式把服务器、令牌、连接码、画质、显示器和窗口状态保存为 Qt 偏好。
- 截图、合成采集、合成音源和 Viewer demo 使用临时偏好，不覆盖日常配置。
- 普通模式使用用户级锁文件保证单实例，并以本机命名管道发送一次唤醒请求；锁在进程
  正常退出或异常终止后均可重新获取。隔离模式不参与单实例锁，可并行截图或运行 demo。
- 设置页直接读取和修改当前用户 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
  下的 `PeerDesk` 值；命令为带引号的绝对可执行路径加 `--background`。后台启动参与
  单实例锁、激活被控端，并在托盘可用时保持窗口隐藏；托盘不可用时回退显示主窗口。
  真实注销/重启验收尚未完成。
- 客户端和服务端默认把带 UTC 时间、级别、PID 和线程 ID 的日志写入
  `%APPDATA%\PeerDesk\logs`。单文件上限 4 MiB，按序号轮转并默认保留 5 个；控制台输出
  仍同步保留。服务端可用 `--log-dir` 或 `PEERDESK_LOG_DIR` 覆盖目录，客户端可用
  `--log-dir` 覆盖；隔离演示和截图模式默认使用临时日志目录，不参与崩溃标记。
- 客户端启动时创建 `<应用>.running` 标记并在正常退出时删除。若启动时发现陈旧标记，
  设置页会显示异常退出提示，主窗口也会给出 toast；该机制只表示上次未正常收尾，
  不包含 minidump，也不等同于完整崩溃分析。
- 设置页可打开当前日志目录，或导出无压缩 ZIP 诊断包。诊断包最多 20 MiB，包含
  `manifest.json`、布尔/数值型 `settings-summary.json` 和当前客户端日志；服务端日志、
  地址、令牌、连接码、路径和常见文件名不会按原值打包。日志在写入和导出时都会脱敏。
  诊断包不会自动上传，分享前仍应人工检查。
- 被控端默认接收目录是系统 Downloads 下的 `PeerDesk`。
- 服务端可选共享令牌、连接频次限制和每会话中继上限。

### 6.1 Windows 安装与卸载

`packaging/windows/` 提供 Inno Setup 安装链路：

- `version.rc.in` 为客户端和服务端生成 Windows 文件版本资源。
- `build-installer.ps1` 从 CMake 读取版本，安装 Release 载荷到 `dist/stage`，用
  `windeployqt` 部署 Qt 运行库与插件，并检查客户端、服务端、平台插件、图像/多媒体插件、
  文档和 MinGW 运行库。
- `PeerDesk.iss` 使用固定的每用户 `AppId`、`PrivilegesRequired=lowest` 和默认目录
  `%LOCALAPPDATA%\Programs\PeerDesk`；关键向导文案为简体中文。
- `test-installer.ps1` 执行静默安装、安装内容校验、服务端 `--version`、离屏客户端启动
  截图、启动项清理、静默卸载和用户数据保留检查。

安装器创建当前用户开始菜单快捷方式，桌面快捷方式为可选任务。卸载由 Inno Setup 的
每用户卸载器完成，删除程序文件和快捷方式，并在卸载阶段删除
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\PeerDesk`。应用自身创建的用户偏好
`%APPDATA%\PeerDesk` 以及接收目录中的文件不属于安装载荷，卸载时保留。

CI 的独立打包任务会生成 `PeerDesk-<version>-setup.exe` 和 SHA-256 清单，再对实际安装包
执行同一套静默安装/启动/卸载冒烟。当前安装包未签名，没有自动更新、失败回滚或已验收的
跨版本升级路径。

## 7. 安全边界

- 9 位连接码是会话预共享密钥的一部分；知道码且网络可达的人可以发起配对。
- 服务器不保存明文连接码，只保存摘要；中继无法解出媒体内容。
- 服务器仍能看到连接关系、候选地址、流量大小和时序，也能丢包或阻断会话。
- HTTP/TCP 信令当前是明文；`--token` 只保护入口，不能替代 TLS 或传输加密。
- 公网部署应使用 TLS 终结代理或可信内网，并限制服务端入口暴露。

## 8. 当前架构限制

- 视频是整帧 JPEG，没有帧间压缩、脏矩形或硬件编码。
- 抓屏只有 GDI，每帧需要完整抓取和缩放；抓屏、缩放、静帧检测、JPEG 编码、Viewer
  解码与呈现、视频发送分片/FEC/NACK 缓存检索、入站视频重组/FEC/NACK 判定已移至
  独立线程，音频包校验和 µ-law 解码也已移至 FIFO 工作线程，抖动缓冲和 `QAudioSink`
  播放已移至 `PeerDeskAudio` 工作线程，UDP socket 收发、入站类型识别、RLY1 解包、
  包头解析、双向 AEAD、重放检查和发送统计已移至 `PeerDeskUdpIo` 工作线程；但
  已认证包的协议分发、NACK/关键帧请求的实际发送、窗口最终合成和大部分 UI 仍在主线程。
- 声音是固定 G.711 µ-law，没有 Opus。
- 文件支持单文件断线续传，但断点只保留 10 分钟，跨机续传仍待真实验收；断点失效时
  回退为整文件重试。接收端同名同大小文件已通过回环测试分配独立路径并隔离断点，但
  发送端也已按源路径隔离跨目录同名同大小队列项；Viewer 与被控端的真实跨机恢复仍待验收。
- 游戏鼠标已在应用层实现并通过回环测试，但仍缺虚拟显示器、虚拟 HID 和真实游戏
  兼容性验收。
- Windows 安装与卸载仅完成本机冒烟和自动化链路验证；未做代码签名、真实跨机分发、
  自动更新、失败回滚和跨版本升级验收。
- 真实跨机、弱网和长会话仍需验收；回环测试与合成画面不能代替。
