# 网易UU远程 逆向分析报告

> 对象：本机安装的 **网易UU远程**（`C:\Program Files\NetEase\GameViewer`，内部代号 **NRD** = NetEase Remote Desktop，
> 版本 `4.48.0.5446`，构建时间 2026-11-16，CompanyName `NetEase`）。
> 手段：静态字符串/结构体提取 + 运行期进程与网络观测 + 配置/缓存文件分析 + 官方 CLI（`uuyc-cli.exe`）行为观察。
> 目的：为 PeerDesk 的功能复刻与协议设计提供依据。文中所有路径、字符串均为真实提取结果。

## 1. 组件构成

安装目录 `C:\Program Files\NetEase\GameViewer`（376 MB），`bin/` 下关键文件：

| 文件 | 体积 | 职责（据字符串与运行行为推断） |
| --- | --- | --- |
| `GameViewer.exe` | 78 MB | 主客户端（Qt5 + WebView2 双栈 UI），登录/设备列表/连接码/充值页 |
| `GameViewerServer.exe` | 34 MB | 本机后台服务（被控端常驻），负责注册上线、接受连接、拉起会话 |
| `GameViewerService.exe` | — | Windows 服务宿主（服务名 `GameViewerService`，Automatic，Running） |
| `GameViewerHealthd.exe` | — | 健康守护/看门狗 |
| `streamer.dll` | 18.6 MB | **媒体与传输核心**：内嵌 libwebrtc、ngtcp2、KCP、H.264/HEVC 编解码调度 |
| `uuyc-cli.exe` | 12.6 MB | Rust + clap 的官方 CLI，子命令 `version/status/user/device/cloudpc/echo/assist/term/lterm` |
| `uuyc-mux.exe` | 10 MB | 终端多路复用（psmux 配置：`history-limit 50000`、`set-clipboard on`、`UUYC_IN_MUX 1`） |
| `Nevaudiosdk.dll` / `nemu-api.dll` / `remote-nemu-api.dll` | — | 虚拟声卡、模拟器（MuMu）联动 |
| `nrd_renderer_ipc.dll` / `conpty_bridge.exe` | — | 渲染进程 IPC、ConPTY 桥（远程终端） |
| `PsExec64.exe` / `aria2c.exe` | — | 提权启动、下载更新 |

`bin/plugins/{imageformats,platforms}` 是 Qt 插件；`bin/html` 只有 `error/skeleton` 骨架页和
`uuycbridge.js`——**业务 UI 不在本地，由 WebView2 从服务端下发**，这也是为什么 PE 里提取不到中文文案。

### 1.1 内核驱动（`bin/drivers`）

| 驱动 | 作用 |
| --- | --- |
| `GameViewerIddDriver` | 虚拟显示器（IddCx），`config/server/*.ini` 里 `[default_virtual_screen] width=2560 height=1440 resolution_type=3/4`、`[super_screen] open=1 width=1920 height=1080 dpi_1920_1080=100` |
| `ViGEmBus` | 虚拟手柄（Xbox/DS4 模拟） |
| `nevaudio` | 虚拟声卡（把被控端音频变成可采集设备） |
| `uuycinput` / `uuycinputmf` | 虚拟键鼠 HID（绕过部分游戏对 SendInput 的过滤） |

被控端"虚拟屏 + 虚拟输入 + 虚拟声卡"三件套是它能做到 2560×1440 高帧率与游戏兼容的关键，
PeerDesk 目前走真实显示器抓屏 + `SendInput`（差异见 §5）。

## 2. 运行期拓扑

```
GameViewer.exe (pid)  ──TCP 443──▶ 42.186.118.109        # 控制/信令（api.nrd.nie.163.com 同族）
GameViewerServer.exe  ──UDP 0.0.0.0:64237──▶ 对端        # 媒体 + 打洞
GameViewerService.exe ──▶ 守护上述进程
```

- 观测时唯一已建立的 TCP 连接指向 `42.186.118.109:443`（HTTP/2 + JSON REST）。
- 媒体端口是一个固定 UDP 监听（示例 `64237`），端口随会话变化，**打洞与媒体复用同一 socket**。
- 注册表/配置文件位置：
  - `%LOCALAPPDATA%\GameViewer\setting.ini`：`trusted_web_app_domain=["^.+\\.webapp\\.163\\.com$"]`
  - `setting_<devid>.ini`：每台被控设备的窗口几何 `x/y/w/h`、`quality=2`、`auto_quality=3`、`smart_mouse=1`
  - `security.ini`、`saved_device_code_*.ini`（历史连接码，**本报告不记录明文**）
  - `webviewcache/EBWebView`：WebView2 缓存（业务页面离线副本）
  - `config/streamer/encoder_codec_capability_cache.json`：`video_codec 1/2`（H.264/HEVC）、
    `codec_impl 0/5`（硬件/软件）、3840×2160、`bit_depth 8/10`、`chroma 1/3`；
    `streamer-config.json` 会按 GPU 型号禁用 H.265

## 3. 传输栈（`streamer.dll` 证据）

提取到的源文件路径直接暴露了架构（`D:\nrd-windows-client-gitlab\work\1131\406406\source\src\shared\business\...`）：

```
..\..\p2p\base\basic_ice_controller.cc      ..\..\p2p\base\p2p_transport_channel.cc
..\..\p2p\base\basic_packet_socket_factory.cc ..\..\p2p\base\port_allocator.cc
..\..\p2p\base\dtls_transport.cc            ..\..\p2p\base\quic_transport.cc
..\..\p2p\base\quic_client_connection.cc    ..\..\p2p\base\quic_server_connection.cc
..\..\pc\dtls_srtp_transport.cc             ..\..\pc\srtp_filter.cc
..\..\pc\srtp_session.cc                    ..\..\pc\srtp_transport.cc
..\..\media\engine\webrtc_voice_engine.cc
```

结论：**libwebrtc（ICE + DTLS-SRTP + P2PTransportChannel）为底座**，并在其上叠了自研层：

- ICE 相关字符串：`iceServers`、`stun:`、`turn:`、`STUN_ATTR_XOR_RELAYED_ADDRESS`、
  `AllocationSequence: No relay server configured, skipping.`、`based on ICE RTT`、
  `local_nat_type/remote_nat_type/select_local_nat_type`（NAT 类型探测 + 路径选择）。
- QUIC（ngtcp2）与 KCP 共存：`kcp mss=`、`kcp_version=`、`mix_kcp=`、`quic_enabled=`、
  `dkcp fec gen find error`、`OnQuicChannelClosed`、`test_mix_kcp1/2`、`test_quic_enable1/2`
  → 自研 mux 在 QUIC/KCP 之上做 **FEC 与多路复用**，按网络质量在 QUIC/KCP 之间切换。
- 中继与商业模式耦合（注意这些字符串本身就是产品逻辑）：
  `current_is_relay_`、`relay_switched:`、`relay_connection`、
  `"Claim to use secure relay connections for smoother remote control."`、
  `"Claim to transfer files over high-speed relay connections."`、
  `%1 Secure Connection On Relay`、`Start startP2PConnection`、`P2p has connected`。
  → **"安全中继"是会员权益的一部分**：P2P 打洞失败或用户购买权益时走中继。
- 内建 SOCKS5/HTTP 代理中继：`Socks5HttpRelay`、`Socks5RelayRunnable`。
- `-l,--disable-lite-punch`（CLI 参数）中的 **lite punch** 即"轻量打洞"，说明其打洞有多档策略。

## 4. 服务端接口面

主机名：`api.nrd.nie.163.com`、`api-dcdn.nrd.nie.163.com`、`api-event.nrd.nie.163.com`、
`api-ipv4.nrd.nie.163.com`、`uuyc.webapp.163.com`（WebView 业务页）、
`adl.netease.com/d/g/uuremote/c/{membership_benefits,customer_service,wol,uuwol,...}`、`sentry.netease.com`。

从二进制中抽出 132 条 `/api/...` 路径，按功能分组（节选）：

| 域 | 端点 |
| --- | --- |
| 房间/会话 | `/api/v1/room/create`、`/api/v1/room/join/by_device`、`/api/v1/room/join/refetch`、`/api/v1/room/join/share`、`/api/v2/room/join/share/by_code`、`/api/v2/room/join/share/by_confirmation` |
| 分享/授权 | `/api/v2/room/share/confirmation`、`/share/control_mode`、`/share/upload_sign`、`/cancel_remote_assist` |
| 访客 | `/api/v1/guest/create`、`/api/v1/guest/room/create`、`guest/share/info` |
| **传输信令** | `/api/v1/transport/%s/rpc/request` + `/api/v1/transport/rpc/response`（RPC 双向通道）、`/api/v1/echo_server`、`/api/v1/network_state` |
| 设备 | `/api/v1/device/list`、`/device/controllable`、`/device/%s`、`/device/%s/reboot`、`/device/%s/screensaver`、`/device/%s/wake_on_lan/req`、`/device/router_wol_support` |
| 会员/计费 | `/api/v1/member/entitlements`、`/member/trial`、充值页 `recharge/{member,cards}/win`、`consumption-record` |
| 云电脑 | `/api/v1/cloud_pc/{create,boot,disks,queue,ticket,streaming_configure,cost_info}` |
| 登录 | `/api/v1/login/by_{mobile,qrcode,uu_token,mumu_token}`、`/api/v1/user/{info,ticket/for/plugin,ticket/for/wvlogin}` |
| 版本/运营 | `/api/v1/plugin-release/details`、`/api/v1/release/fetch/{update,popups/info}`、`/api/v1/campaign/*` |
| 事件埋点 | `api-event.nrd.nie.163.com/api/v1/event/report/batch` |

要点：

1. **信令走 HTTPS 长连接 + 双向 RPC**（`transport/rpc/request|response` 成对出现），
   说明会话内的控制消息（不只是连接建立）也复用同一条通道。
2. 计费模型是"会员时长 + 云电脑按时长"，`consumption-record` + `cloud_pc/cost_info` 对应
   你提到的不满点：**P2P 本来是免费的，但"安全中继"被包装成付费权益**。
3. 远程协助有 `share/by_confirmation`（对方确认）与 `control_mode`（控制/观看）两种授权路径，
   对应"远程协助"与"远程控制"两个产品形态。

### 4.1 官方 CLI 暴露的能力

`uuyc-cli.exe`（Rust/clap）子命令：`version`、`status`、`user`、`device`、`cloudpc`、`echo`、`assist`、`term`、`lterm`。
遗留参数：`-d/--device-id`、`--list-devices`、`-c/--reset-custom-code`、`-n/--connect`、
`-b/--set-bitrate-limit <Mbps>`、`-l/--disable-lite-punch`、`input-diag`。

- `term` 子命令能对远端开终端会话，语义是 tmux 风格（`new/attach/ls/kill/rename`），配合 `uuyc-mux.exe`。
  → 远程终端是独立产品面（对开发/运维很实用），PeerDesk 的路线图里对应"远程 Shell"。
- `input-diag` 说明它对输入注入有诊断工具（游戏兼容性问题的排查入口）。

## 5. 与 PeerDesk 的能力对照

| 能力 | UU远程 | PeerDesk 0.1 | 备注 |
| --- | --- | --- | --- |
| 连接码 | 9 位数字（3-3-3 分组） | 9 位数字（同格式），`code_id` 上服务端 | 一致 |
| 打洞 | ICE + NAT 类型探测 + lite punch 多档 | 候选交换 + 100 ms PUNCH，6s 升级中继 | 简化但有效 |
| 加密 | DTLS-SRTP | X25519 + HKDF + ChaCha20-Poly1305（每包） | 无证书依赖 |
| 视频编码 | H.264/HEVC，硬/软编，8/10bit，4:2:0/4:4:4 | Motion-JPEG（质量自适应） | MJPEG 无帧间压缩，码率高但零依赖、零花屏 |
| 虚拟显示器 | IddCx 驱动 2560×1440 | 真实显示器 | 需要签名驱动，路线图 |
| 虚拟输入 | HID 驱动（游戏兼容） | `SendInput`（含绝对定位/Unicode 输入） | 部分反作弊会拦 |
| 音频 | 虚拟声卡双向 | 未实现 | 路线图 |
| 文件传输 | 中继加速 + 会员权益 | 控制通道 base64 分块（≤512 MB） | 已可用，未做独立流控 |
| 剪贴板 | 文本/图片 | 文本双向 + 变更监视 | 图片待补 |
| 多显示器 | 支持选择 | 支持选择（`monitors` 列表） | 一致 |
| 远程终端 | `term` 子命令 + MUX | 未实现 | 路线图 |
| Wake-on-LAN | `/device/wake_on_lan` | 未实现 | 路线图 |
| 自托管 | 只有官方云 | `peerdesk server` 一条命令 | **PeerDesk 的差异点** |

## 6. 复刻过程中被验证的设计细节

这些是"看起来能跑、实际会坏"的点，逆向与自研两边都踩过：

1. **握手字节必须可重现**：任何带时间戳/nonce 的握手体，重发时必须复用同一份字节，
   否则 transcript 变、密钥变、握手"成功"但数据全解不开。
2. **包头序号不能兼任消息序号**：AEAD nonce 计数器必须是每包单调且唯一的，
   可靠层的消息序号要放在载荷里（我们第一版混用，结果 ACK 永远丢）。
3. **plen 必须包含 tag**：接收端在解密前就要知道切多长；把 tag 排除在外会让 AAD 与
   实际长度不一致，表现为"偶发解密失败"。
4. **FEC 恢复要带校验片本身**：`lost = parity ^ survivors`，漏掉 parity 会恢复出垃圾数据。
   （这条是单元测试抓出来的，见 `tests/test_wire.py::test_parity_recovers_single_loss`。）
5. **控制流 ACK 用累积语义**：位图只用于补洞，不能把"已确认"误算成只剩最后一条。
6. **STUN 地址族要校验**：坏包只给 family 字节时按 `raw[4:]` 切会拼出"合法但错误"的地址。
7. **服务端只存 `code_id`**：明文连接码等于 PSK，不该落在服务器上。

## 7. 方法与局限

- 静态：`strings`/UTF-16 扫描 + 自定义提取脚本（`D:\UU\re_work\extract_strings.py`、`extract_cjk.py`），
  对每个 DLL 产出 `gv_*.txt` / `cjk_*.txt`。
- 动态：进程/服务枚举、`netstat` 连接与监听、配置文件读写观测、CLI `--help` 与子命令探测。
- 局限：业务 UI 由 WebView2 从服务端下发，未抓包；未做协议级抓包解密（DTLS-SRTP），
  因此"中继切换阈值、码率控制算法"等只能从字符串与配置推断，标注为推断而非结论。
  本报告不包含任何账号凭据、设备码明文或可用于绕过授权的信息。
