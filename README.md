# PeerDesk · 对端桌面

**自研 P2P 协议的远程桌面：9 位连接码、直连优先、中继兜底、无账号、无会员、无时长计费。**

纯 Python 3.11+ 实现（Windows），传输层是自己写的 PDTP/1 —— UDP + X25519/HKDF/ChaCha20-Poly1305 +
候选打洞 + XOR FEC + NACK，**不用 WebRTC、不用浏览器引擎、不用 npm**。UI 是三个静态文件，
本地 HTTP + 手写 WebSocket 桥给浏览器渲染。

[![CI](https://github.com/Bad0RANG3/peerdesk/actions/workflows/ci.yml/badge.svg)](https://github.com/Bad0RANG3/peerdesk/actions/workflows/ci.yml)
![Python](https://img.shields.io/badge/python-3.11%2B-3d7bfd)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-3d7bfd)
![License](https://img.shields.io/badge/license-Apache--2.0-35d07f)

![viewer 会话页](docs/screenshot-viewer.png)

> 上面是本机回环自测：viewer 显示的就是 agent 那台机器的桌面（所以画面里能看到它自己）。
> HUD 实时读数 `P2P / 1600×900 @ 1264×805 / 15.7 fps / 6.67 Mbps / 0.4 ms`。

---

## 为什么写这个

网易UU远程做得很完整（libwebrtc ICE + DTLS-SRTP + QUIC/KCP 双栈 + 自研 FEC + 虚拟显示器/声卡/HID 驱动），
但它的**"安全中继"是会员权益**：P2P 打不通就引导你付费。本项目把这条链路从头实现一遍，
**打洞优先、中继永远免费兜底**，并且服务端可以自己一条命令跑起来。

逆向分析（组件、传输栈、132 条 REST 接口、计费模型）见 [docs/01-reverse-engineering.md](docs/01-reverse-engineering.md)。

## 特性

- **9 位连接码**，3-3-3 分组；服务端只保存 `SHA256` 后的 `code_id`，不落明文码
- **每包 AEAD**：X25519 临时密钥 + HKDF（连接码当 PSK）+ ChaCha20-Poly1305，20 字节头做 AAD
- **打洞 + 中继回退**：候选交换后 100 ms 一发 PUNCH，6 秒直连失败自动升级 UDP 中继（画质不变）
- **视频**：JPEG 质量自适应（32–92）+ XOR FEC（8+1）+ NACK 重传 + 关键帧请求 + 450 ms 帧超时丢弃
- **控制**：独立可靠流（累积 ACK + 64 位位图 + RTO 退避），键鼠/文本/剪贴板/文件/画质/显示器切换
- **多显示器**、静帧跳过、绝对坐标注入、Unicode 文本输入、`Ctrl+Alt+Del`
- **浏览器 UI**：深色玻璃拟态、悬浮会话工具栏、实时 HUD，无框架无构建步骤
- **自托管服务端**：TCP 信令 + UDP 中继 + STUN 复用同一个 UDP 端口

## 快速开始

```bash
git clone https://github.com/Bad0RANG3/peerdesk.git
cd peerdesk
python -m pip install -e .
python -m peerdesk doctor          # 环境自检：依赖 / 显示器 / 输入 / 加密 / UDP / STUN
```

三端各开一个终端：

```bash
# 1) 服务端（一台有公网 IP 的机器；内网直连也可以用它）
python -m peerdesk server --port 7900 --udp-port 7901

# 2) 被控端：给出 9 位连接码，并开一个本地面板
python -m peerdesk agent --server 1.2.3.4:7900 --code 604173829 --web-port 7701

# 3) 控制端：打开 http://127.0.0.1:7702/ 输入连接码
python -m peerdesk viewer --server 1.2.3.4:7900 --web-port 7702
```

不写 `--code` 时 agent 会随机生成一个并显示在面板上（面板里可以复制或更换）。
`viewer` 也可以不带 `--code` 启动，直接在页面里填。

```bash
python -m peerdesk agent --help
python -m peerdesk viewer --help
```

### 常用参数

| 参数 | 角色 | 说明 |
| --- | --- | --- |
| `--server HOST:PORT` | agent/viewer | 信令服务器，默认 `127.0.0.1:7900` |
| `--code 604173829` | agent/viewer | 9 位连接码（首位非 0） |
| `--web-port` | agent/viewer | 本地面板端口（`0` = 随机） |
| `--fps / --width / --quality` | agent | 帧率上限 / 最大宽度 / JPEG 质量（32–92） |
| `--monitor N` | agent | 初始显示器序号（0 起） |
| `--no-input` / `--no-clipboard` / `--no-skip-static` | agent | 禁止键鼠 / 禁止剪贴板 / 关闭静帧跳过 |
| `--relay-only` | agent/viewer | 强制中继（排障、对称 NAT 场景） |
| `-v` / `--log FILE` | 全部 | 详细日志 / 写日志文件 |

## 架构

```
agent ──TCP JSON Lines──▶ server ◀──TCP JSON Lines── viewer
   └──────── PDTP/1 over UDP（打洞直连，失败走中继）────────┘
viewer ──本地 HTTP + WebSocket──▶ 浏览器 canvas
```

| 层 | 文件 | 说明 |
| --- | --- | --- |
| 报文/分片/FEC | `peerdesk/wire.py` | 20 字节头、VIDEO_FRAG/PARITY/ACK/NACK、`FragmentAssembler` |
| 加密 | `peerdesk/crypto.py` | X25519 → HKDF → AEAD，nonce=`(direction, seq)`，重放窗口 2048 |
| 传输 | `peerdesk/transport.py` | `PeerLink` 状态机、`ReliableStream`、`VideoSender/Receiver`、中继封装 |
| 抓屏/编码 | `peerdesk/capture.py` | mss + Pillow，`FramePacer` 按帧大小调质量 |
| 输入 | `peerdesk/wininput.py` | `SendInput`，虚拟桌面归一化，剪贴板读写与监视 |
| 会话 | `peerdesk/session.py` | `HostSession` / `ViewerSession`，控制消息分发 |
| 服务端 | `peerdesk/server.py` | 信令 + 中继（`RLY1` 头 + 配对 tag）+ STUN |
| 面板 | `peerdesk/webui.py` + `web/` | HTTP/WS 桥、Hub 背压丢帧、sticky 状态重放 |

细节见 [docs/02-architecture.md](docs/02-architecture.md) 与协议规范 [docs/03-protocol-pdtp1.md](docs/03-protocol-pdtp1.md)。

## 测试

```bash
python -m unittest discover -s tests -p "test_*.py"   # 79 个单元测试
python tests/e2e_local.py                            # 进程内端到端（直连）
python tests/e2e_local.py --relay                    # 进程内端到端（强制中继）
python tests/e2e_web.py                              # 浏览器桥：页面 + WS + JPEG 帧 + 控制通道
```

本机实测（2560×1440 桌面、1600×900 编码、fps 上限 20）：**13.5–15.7 fps / 6.6 Mbps / RTT 0.4–2 ms**，
中继模式功能等价。瓶颈是抓屏+JPEG（约 30–40 ms/帧），不是网络。

## 文档

- [逆向分析报告：网易UU远程](docs/01-reverse-engineering.md) — 组件、驱动、传输栈证据、REST 面、计费模型
- [架构](docs/02-architecture.md) — 进程模型、数据流、状态机、性能数字、已知限制
- [PDTP/1 协议规范](docs/03-protocol-pdtp1.md) — 报文格式、握手、密钥派生、FEC/NACK、中继
- [UI 复刻对照](docs/04-ui-parity.md) — 界面清单、视觉规范、交互细节
- [路线图](docs/05-roadmap.md) — DXGI 抓屏、音频、H.264、虚拟显示器、远程 Shell

## 安全说明

- 连接码即 PSK：知道 9 位码即可握手（与同类产品同一模型）。服务端不存明文码。
- 服务器只看到元数据（谁连谁、候选地址、流量大小），看不到媒体内容。
- 面板默认只监听 `127.0.0.1`，跨机访问请用 SSH 隧道或反代，别直接暴露到公网。
- 中继目前没有鉴权与限速，公网部署请在前面加白名单（见路线图 0.2）。

## 许可

Apache-2.0。本项目只做**自有设备之间**的远程桌面与自己的协议研究；
不含任何官方客户端的代码、资源或凭据，商标与产品名归各自所有者。
