# PDTP/1 协议规范

> PeerDesk Transport Protocol v1。控制面使用 TCP JSON Lines，媒体面使用一条 UDP
> 数据通道；连接码决定会话预共享材料，X25519 + HKDF 派生双向 AEAD 密钥。
> 实现位于 `src/core/`、`src/transport/`、`src/session/`、`src/client/` 和
> `src/server/`。

本文描述的是 **0.9.0 已经落地的协议行为**，不是目标设计稿。已知缺陷会明确标注，
不能把尚未可靠工作的能力当成稳定接口。

## 1. 传输分层

```text
TCP JSON Lines       注册、配对、候选、令牌、中继入口
        │
        ▼
UDP PDTP/1           20 字节明文头 + AEAD 体
        ├─ Video     整帧视频分片（JPEG/H.264 标志）、XOR FEC、NACK、关键帧请求
        ├─ Control   独立可靠流、累积 ACK、应用消息
        ├─ Audio     G.711 µ-law，不可靠，当前帧 + 上一帧冗余
        ├─ Stats     统计流枚举
        └─ Relay     RLY1 包装后的原始 PDTP 数据报
```

信令层的 `sid` 与 UDP 包头的 `session` 不是同一个值。`session` 是链路级随机 tag，
只用于区分和校验当前数据面会话。

## 2. UDP 数据报

### 2.1 固定头

所有 PDTP/1 数据报以 20 字节清晰头开始。多字节整数按 **小端** 打包。

```text
 0      2  3  4                 8                 12  13  14     16       20
+--------+--+--+-----------------+------------------+---+---+------+--------+
| "PD"   |v |t | seq (u32 LE)    | tsMs (u32 LE)    |str|flg| plen |session |
+--------+--+--+-----------------+------------------+---+---+------+--------+
```

| 字段 | 宽度 | 说明 |
| --- | ---: | --- |
| `magic` | 2 | 固定 `PD` |
| `version` | 1 | 固定 `1` |
| `type` | 1 | 包类型，见 2.3 |
| `seq` | 4 | 发送序号；同时进入 AEAD nonce 和重放窗口 |
| `tsMs` | 4 | 会话相对的发送毫秒时间，不是 Unix epoch |
| `stream` | 1 | 逻辑流，见 2.4 |
| `flags` | 1 | 视频/可靠标志，见 2.5 |
| `plen` | 2 | 头部之后的载荷长度 |
| `session` | 4 | 链路随机 tag |

加密包的 `plen = 明文载荷长度 + 16`，16 字节是 Monocypher AEAD tag。明文
`HELLO`、`HELLO_ACK` 和 `PUNCH` 使用原载荷长度。头部本身不加密，但整个 20 字节头
作为 AEAD 的 AAD；修改任一字段都会导致认证失败。

### 2.2 信封与 AEAD

1. 发送端递增 32 位包序号，构造 20 字节头，把 `plen` 设为
   `payload.size() + 16`。
2. 明文载荷用 Monocypher `crypto_aead_init_ietf` 加密，密钥来自 HKDF。
3. 16 字节认证标签追加在密文后，形成 UDP 载荷体。
4. 接收端先按头部长度切包，再以完整头部作为 AAD 解密；认证失败时不能推进重放窗口。

重放窗口宽度为 2048。包序号 `seq` 是包头序号，不等于应用层控制消息序号。

### 2.3 包类型

| 值 | 类型 | 方向/用途 |
| ---: | --- | --- |
| 1 | `HELLO` | 控制端 → 被控端；紧凑 JSON，明文 |
| 2 | `HELLO_ACK` | 被控端 → 控制端；紧凑 JSON，明文 |
| 3 | `HELLO_FIN` | 枚举保留；当前没有三路握手流程 |
| 4 | `PUNCH` | 双向 UDP 打洞探针，明文 |
| 5 | `PUNCH_ACK` | 双向打洞确认 |
| 6 | `DATA` | 视频分片、控制消息或文件流 |
| 7 | `PARITY` | 视频 XOR FEC 校验片 |
| 8 | `ACK` | 控制可靠流累积确认 + 64 位位图 |
| 9 | `NACK` | 视频缺片请求 |
| 10 | `PING` | 保活和握手探测 |
| 11 | `PONG` | 回显对端 `PING` 载荷 |
| 12 | `BYE` | 主动结束链路 |
| 13 | `AUDIO` | G.711 µ-law 音频载荷 |

`HELLO_FIN` 只是预留值，接收端不能把它解释成当前已经存在的三路握手。

### 2.4 逻辑流

| 值 | 名称 | 状态 |
| ---: | --- | --- |
| 0 | `video` | 当前使用 |
| 1 | `control` | 当前使用 |
| 2 | `file` | 仅枚举；文件实际走控制消息 |
| 3 | `stats` | 枚举保留 |
| 4 | `audio` | 音频使用独立 `AUDIO` 包类型，不以 `DATA` 发送 |

### 2.5 标志位

| 位 | 名称 | 含义 |
| ---: | --- | --- |
| `0x01` | `KEYFRAME` | 当前视频帧是完整关键帧 |
| `0x02` | `LAST_FRAGMENT` | 当前视频分片是本帧最后一片 |
| `0x04` | `RELIABLE` | 控制消息走可靠重传 |
| `0x08` | `H264` | 视频帧载荷是 H.264；清零时表示 JPEG |

### 2.6 尺寸常量

| 常量 | 值 | 说明 |
| --- | ---: | --- |
| `kMaxDatagram` | 1400 字节 | UDP 数据报上限 |
| `kPayloadMtu` | 1364 字节 | 1400 − 20 − 16 |
| `kVideoPayloadMtu` | 1296 字节 | 再减 8 字节视频分片头 |
| 控制分片阈值 | 1200 字节 | 超过后使用 9 字节控制分片头 |

## 3. 连接码与握手

### 3.1 连接码

- 合法连接码为 9 位数字，首位不能是 `0`。
- 用户界面按 `3-3-3` 分组显示，传输和派生使用去分隔符的 9 位数字。
- 连接码是当前会话的主要预共享秘密；没有账号体系。

```text
psk      = SHA256("peerdesk-psk" || salt || 0x00 || digits)
codeId   = SHA256("peerdesk-id" || 0x00 || digits).toHex().left(32)
verifier = SHA256("peerdesk-psk" || psk).toHex().left(16)
```

`salt` 是派生接口保留的附加输入；当前客户端调用使用空 `salt`，但协议要求双方输入
一致。`codeId` 是服务端注册表的查找键，不直接出现在 JSON 信令中。

### 3.2 HELLO

控制端先发送明文 `HELLO`，被控端返回明文 `HELLO_ACK`。载荷是紧凑 JSON，至少包含：

| 字段 | 说明 |
| --- | --- |
| `v` | 协议版本，当前为 `1` |
| `sid` | 本次信令会话 id 的十六进制文本 |
| `pub` | X25519 公钥的 Base64（32 字节） |
| `psk` | 上述 verifier 的十六进制文本 |
| `role` | `client` 或 `host` |
| `id` | 设备标识 |
| `cands` | 候选数组，元素含 `host`、`port`、`kind` |
| `ts` | 发送时的 Unix 毫秒时间 |

重发 `HELLO` 或 `HELLO_ACK` 必须复用第一次构造的原始 JSON 字节，不能重新生成候选、
时间戳或密钥。否则两端 transcript 不同，会出现“握手看似成功但所有加密包都无法解封”。

### 3.3 密钥派生

双方拿到对端 32 字节 X25519 公钥后计算共享秘密，再执行：

```text
transcript = SHA256("peerdesk/1" || 0x00 || helloHost || 0x00 || helloClient)
pskDigest  = SHA256("peerdesk/1" || 0x00 || "psk" || 0x00 || psk)
salt       = SHA256(transcript || pskDigest)

hostToClient = KDF(shared, salt, "peerdesk/1 host->client", 32)
clientToHost = KDF(shared, salt, "peerdesk/1 client->host", 32)
```

`helloHost` 永远是被控端的原始 JSON，`helloClient` 永远是控制端的原始 JSON；
不能按本地发送顺序交换。`KDF` 是基于 HMAC-SHA256 的 RFC 5869
extract-then-expand 派生。共享秘密为全零时派生必须失败。

### 3.4 nonce 与重放

- 方向值写入 nonce 前 4 字节，使用 **大端**：被控端发送为 `0`，控制端发送为 `1`。
- 后 8 字节是包序号 `seq`，同样使用 **大端**。
- nonce 固定 12 字节，AEAD tag 固定 16 字节。
- 解密失败、方向不匹配或 `session` tag 不匹配都不应更新重放窗口。
- 窗口宽度为 2048；新包成功认证后才标记为已接收。

### 3.5 打洞、保活与中继

- `HELLO` 默认每 250 ms 重发一次，握手超时为 15 秒。
- 收到候选后每 100 ms 对候选发送 `PUNCH`；收到有效响应即可锁定直连路径。
- 网络路径确认后每 4 秒发送保活 PING；握手阶段每秒发送加密 PING。
- 25 秒没有收到有效数据视为链路超时。
- 开始握手后 6 秒仍未建立直连，且服务器提供了中继端点，则升级为 RLY1 中继。
- 中继只是 UDP 转发：PDTP 载荷始终为密文，中继端不能解密媒体。

## 4. 视频数据

### 4.1 分片

视频帧按最多 1296 字节切成 `DATA` 包。每个视频载荷先放 8 字节分片头：

```text
frameId u32 LE + index u16 LE + count u16 LE + encoded frame bytes
```

- `frameId` 由发送端递增，用于重组、NACK 和 FEC。
- `index` 从 0 开始，`count` 是整帧分片数。
- 最后一片设置 `LAST_FRAGMENT`；关键帧的每一片都设置 `KEYFRAME`。
- 每个 DATA 分片只在 `H264` 标志中携带 codec；清零表示 JPEG。PARITY 也带相同标志，
  NACK 重传必须保留 `KEYFRAME` 和 `H264`，并按原分片位置设置 `LAST_FRAGMENT`。
- Windows Viewer 在会话控制消息 `hello.media.codecs` 中声明 `h264`/`jpeg` 能力；
  Host 仅在自身 codec 可用且 Viewer 声明 H.264 时编码 H.264，否则使用整帧 JPEG。
- Host 通过控制消息 `t: "media"` 报告当前 `codec`、`negotiated` 和可选 `reason`；
  接收端仍以每个视频 DATA/PARITY 的 `H264` 标志为准，不能只依赖该状态消息。
- H.264 编码器启动或运行失败时，Host 切换到 JPEG 并发送新的 `media` 状态。Viewer
  在 H.264 首个关键帧前丢弃增量帧，解码失败后重置解码器并等待下一关键帧。

### 4.2 XOR FEC

每 8 个连续分片组成一组：

- 少于 3 片时不发送 parity。
- `PARITY` 载荷头为 8 字节：
  `frameId u32 LE + groupIndex u8 + shards u8 + 2 字节保留`。
- 校验数据是对组内所有分片逐字节 XOR，长度取组内最大分片。
- 取长度时短分片按零补齐。单个缺片可由同组 parity 恢复；多个缺片需要 NACK。

### 4.3 NACK 与关键帧

- 接收端每 50 ms 检查最多 4 个未完成帧，每次最多请求 32 个缺片。
- 相同帧的 NACK 有 50 ms 限频，避免重复请求风暴。
- 一帧超过 450 ms 仍未拼齐则丢弃并增加丢帧计数。
- 丢弃的是关键帧时，接收端请求新的关键帧。
- 发送端缓存最近 10 帧的分片，用于响应 NACK。
- 视频不参与控制流的可靠重传；丢包由 FEC、NACK 和关键帧刷新处理。

## 5. 控制可靠流

### 5.1 消息编号与分片

控制消息使用单独的 `ReliableStream`。每个 `DATA/control` 载荷前有一个
**u32 小端消息序号**，随后才是消息或分片：

```text
[u32 acknowledgeable sequence][control payload]
```

当 JSON 载荷超过 1200 字节时分片。每个分片头为 9 字节：

```text
0xF1 + messageId u32 LE + index u16 LE + count u16 LE + body bytes
```

`messageId` 只在一次分片重组内标识消息，和可靠流的 u32 序号不是同一个概念。
接收端同时最多保留 16 个未完成分片消息。

### 5.2 ACK 与重传

- ACK 载荷为 12 字节：`cumulative u32 LE + bitmapLow u32 LE + bitmapHigh u32 LE`。
- 位图统一采用 `bit i = cumulative + 1 + i`，`i` 范围为 0..63。
- 接收端尚未按序交付序号 0 时，`cumulative` 使用哨兵 `0xFFFFFFFF`，位图基址为 0；
  因此首个乱序包不会把尚未收到的序号 0 伪确认。
- 接收端把乱序包缓存在当前交付点之后最多 256 个序号的窗口内，并立即标记需要 ACK；
  重复包、过旧包和窗口外数据不会再次交付，但会触发一次当前 ACK 重发。
- 发送端忽略没有发送记录时的累积确认，以及 `cumulative >= nextSeq` 的非法确认；
  只移除累积确认及其位图范围内的待确认项。
- 初始 RTO 为 150 ms，按指数退避，最大 1200 ms。
- 最多重传 14 次；超过后丢弃该待确认项。
- 重置可靠流会清空发送记录、接收缓存、序号、窗口状态和 ACK 脏标记。
- 回环测试覆盖累积确认、乱序补洞、重复包、窗口越界、非法 ACK、重传上限和重置；
  真实丢包网络、长时间序号回绕和跨机恢复仍需正式验收。

## 6. 音频

### 6.1 采集与编码

- Windows WASAPI loopback 采集系统播放声音。
- 重采样为 16 kHz、单声道、PCM16。
- 每帧 20 ms，即 320 个采样、640 字节 PCM。
- 使用 G.711 µ-law 编码，每帧约 320 字节。

### 6.2 音频载荷

`AUDIO` 载荷头固定 12 字节：

```text
sequence u32 LE + timestamp u32 LE + samples u16 LE + previousLen u16 LE
+ previous µ-law bytes + current µ-law bytes
```

- `samples` 是当前 PCM 采样数，正常为 320。
- `previousLen` 是上一帧编码长度，首帧可以为 0。
- 当前帧和上一帧同时携带，接收端可在丢一帧时用冗余覆盖。
- 连续静音 25 帧后，每 25 帧只保留一个静音帧，降低空口占用。
- Viewer 使用 `QAudioSink` 播放，缓存上限约 360 ms。

音频不进入视频 FEC 组，也不使用可靠重传。

## 7. 控制消息、文件与剪贴板

### 7.1 主要控制消息

| `t` | 说明 |
| --- | --- |
| `hello` | 加密链路建立后的角色确认 |
| `media` | Host 报告协商后的视频 codec、是否完成协商和回退原因 |
| `settings` | 画质、帧率、显示器、声音、剪贴板等会话设置 |
| `keyframe_req` | 请求新的完整视频关键帧 |
| `mouse` | 绝对坐标、相对位移、按钮与滚轮 |
| `key` | 键码、按下/释放、文本输入 |
| `clip` | 文本或 PNG 图片剪贴板同步 |
| `audio` | 音频采集开关与状态；Host 回报 `on`、`enabled`、编码参数、音源和错误 |
| `file_begin` / `file_resume` / `file_data` / `file_end` | 文件开始、续传确认、分块和结束 |
| `file_cancel` / `file_cancelled` | 发送端取消与接收端回执 |
| `file_error` | 任一端拒绝、读写或校验失败 |
| `reboot` | 只回提示，不执行重启 |
| `stats` | 会话统计 |

控制消息是 JSON；一次控制流发送的载荷由 u32 可靠序号包裹，超过 1200 字节再走
`0xF1` 分片头。

### 7.2 鼠标控制

`mouse` 消息通过 `act` 区分动作：

- `move`：携带归一化画面坐标 `x`、`y`。
- `down` / `up` / `click`：携带 `x`、`y` 和 `btn`，默认按钮为 `left`。
- `wheel`：携带 `dy` 与可选布尔字段 `h`；`h=true` 表示水平滚轮。
- `rel`：只携带相对位移整数 `dx`、`dy`，被控端调用相对移动接口，不更新绝对坐标。
- `reldown` / `relup`：只携带 `btn`，默认 `left`；只改变按键状态，不移动绝对坐标。

Viewer 将相对移动每 16 ms 合并一次。退出游戏鼠标、输入失焦、链路恢复、最终失败和
会话结束时，控制端会对已按下的相对按钮发送 `relup`。

### 7.3 文件

- 文件使用 `file_begin`、`file_data`、`file_end` 控制消息。
- 内容字段 `d` 使用 Base64，每块 24 KiB。
- 单文件上限 512 MiB。
- 文件名只允许安全 basename；接收端拒绝路径分隔符、空名和超长名称。
- 接收端使用 `QSaveFile` 写入，存在同名文件时生成唯一路径。
- 同名目标使用 `名称 (1).扩展名` 等唯一路径；接近 255 字符 basename 上限时缩短
  主干并保留扩展名与序号，且不会切断 UTF-16 代理对。传输存续期间预留目标路径，
  避免并发同名传输互相覆盖。
- 每一侧同一时间只能有一个主动发送；接收端可以管理多个传输。
- 默认接收目录是系统 Downloads 下的 `PeerDesk`。

链路中断后，未完成的接收临时文件最多保留 10 分钟。恢复实况后，发送端使用新的传输
ID 发起续传：

```json
{"t":"file_begin","id":"新id","name":"a.bin","size":1000,"resume":true,"resume_of":"旧id"}
{"t":"file_resume","id":"新id","offset":4200}
```

- 接收端要求 `resume_of` 指向仍保留的旧传输，且文件名和大小完全一致；新 ID 不能与
  旧 ID 重复。校验失败时以 `file_error`（`reason: "resume"`）拒绝，不移动旧临时文件。
- 同一文件名的不同传输保留独立目标路径；接收端只把指定 `resume_of` 旧 ID 的临时
  文件转交给新 ID，因此同名同大小传输不会串用断点。
- 校验成功后，接收端把旧临时文件和已接收字节数转交给新 ID，并用实际 `offset` 回复。
  发送端从该偏移继续，且确认前不发送 `file_data`。
- 发送端等待确认 8 秒，超时后发送 `file_error`（`reason: "resume_timeout"`）并报告
  本地失败。接收端返回非法偏移时，发送端同样终止本次传输。
- `file_in` 状态事件会携带 `resumed` 和 `offset`，供 UI 区分新传输与续传。

结束状态按原因分流：

- 用户主动取消：发送端发出一次 `file_cancel`（`reason: "cancelled"`）并停止分块。
  接收端取消 `QSaveFile`、删除临时文件、释放目标名称预留，再回发
  `file_cancelled`。发送端只产生一次本机取消事件，不把回执当成失败。
- 接收端拒绝或落盘失败：接收端发送 `file_error`；发送端停止读取源文件并以对端给出的
  原因结束，不再回复另一个协议包。
- 发送端本地读取失败：发送端发送 `file_error`，随后清理资源并报告本地失败。
- 链路关闭或会话挂起：发送端保留续传断点，接收端保留未完成临时文件，只产生
  `interrupted`，不发送 `file_cancel`。永久停止会话时才清理续传状态。因此“用户取消”
  和“网络中断”不会在协议层混为一谈。
- 取消与错误处理是幂等的；重复点击、重复取消或迟到的回执不会再次触发终态事件。

### 7.4 剪贴板与重启

- 剪贴板双向同步文本和 PNG 图片。文本消息保持旧格式：

```json
{"t":"clip","s":"文本"}
```

- 图片消息使用 `kind: "image"` 和 `mime: "image/png"`，宽高字段用于交叉校验，
  `d` 是 PNG 字节的严格 Base64：

```json
{"t":"clip","kind":"image","mime":"image/png","w":640,"h":360,"d":"<base64>"}
```

- 文本单次上限 256 KiB。PNG 编码后上限 4 MiB；图片单边不超过 8192 像素，总像素
  不超过 1600 万。
- 接收端拒绝非法 Base64、非 PNG、超限数据及声明尺寸与实际解码尺寸不一致的图片。
  非法图片只记录日志，不覆盖当前剪贴板；未知 `kind` 会被忽略，无 `kind` 保持文本
  兼容语义。
- Host 每 400 ms 轮询本机剪贴板，图片优先于文本；图片按规范化 RGBA8888 像素和宽高
  计算 SHA-256 指纹，只有变化时才发送。Viewer 通过 Ctrl+V 或工具栏发送图片时同样
  图片优先，没有图片才回退文本。
- 剪贴板变化检测在会话运行期启用；关闭“剪贴板同步”后不会把新内容写入链路。
- `reboot` 当前只提示“收到重启指令（未执行）”，不是可用的远程重启能力。

## 8. 信令与中继

### 8.1 TCP JSON Lines

信令是长度有界的 JSON Lines：每行一条 JSON，单行上限 1 MiB。客户端消息包括：

`register`、`connect`、`reconnect`、`ping`、`cands`、`relay_hello`、`bye`。

服务端下行包括：

`registered`、`viewer`、`peer`、`peer_gone`、`viewer_gone`、`pong`、`error`，
以及转交给对端的 `peer_cands`、`peer_relay_hello`、`peer_bye`。

### 8.2 注册与配对

1. 被控端发送 `register`，携带 `codeId`、设备名、会话 `sid`、本地候选和令牌。
2. 服务端返回 `registered`，包含连接码、STUN/中继 UDP 地址、`server_time`、
   `token_required` 和 `relay_mbps`。
3. 控制端发送 `connect`，服务端检查令牌、在线状态和每 IP 连接频次。
4. 被控端收到 `viewer`，控制端收到 `peer`；双方得到相同 `sid`、对端候选、
   中继地址、各自的 `relay_tag` 和 `relay_mbps`。
5. 双方用同一连接码派生 psk，在 UDP 候选上执行 PDTP 握手。

信令层 `reconnect` 消息只向被控端发送 `viewer_gone`；Viewer 的会话恢复由客户端重新
执行配对、UDP 握手和会话启动完成，不复用旧媒体密钥。重新进入实况后，文件发送端保留的
续传断点可接管对端临时文件并从确认偏移继续；Viewer 的 UI 队列同时保留批次与进度。
该恢复已在本机回环测试覆盖，真实弱网、跨 NAT 与跨机续传仍需独立验收。

### 8.3 错误与限制

服务端错误 `reason` 包括：

`bad_token`、`bad_code`、`offline`、`rate_limited`、`unknown_type`。

- 默认每个 IP 在 60 秒内最多 10 次连接尝试。
- 令牌只保护信令入口；内置信令 TCP 本身不是 TLS。
- 中继默认按每会话 `relay_mbps × 125000 B/s` 限速，允许 1 秒突发；超限数据报直接丢弃。

### 8.4 STUN

STUN 与中继共用服务端 UDP 端口：

- 使用 Binding Request/Success。
- Magic Cookie 为 `0x2112A442`。
- 事务 id 固定 12 字节。
- 支持 MAPPED-ADDRESS 和 XOR-MAPPED-ADDRESS 的解析/生成。
- 反射地址只用于候选和打洞，不替代端到端加密。

### 8.5 RLY1 中继封装

中继数据报为：

```text
RLY1 (4 bytes) + tag u32 BE + PDTP packet
```

服务端为每个会话随机生成 tag 并保证唯一。中继只检查 tag 和目标，不尝试解密 PDTP。
中继超限会丢包，因此弱网/高码率场景必须依靠视频自适应和重试策略。

## 9. 安全边界

- 9 位连接码就是会话的预共享秘密；知道码且网络可达的人可以尝试配对。
- 服务端保存连接码摘要，不使用明文连接码注册；但能看到连接元数据、候选地址、
  流量大小和时序，也能丢包或阻断会话。
- 中继只转发密文，不能解密媒体，但它不是匿名网络，也不能隐藏流量特征。
- `--token` 只保护信令入口，不能替代传输加密，也不能阻止已获得连接码的人发起请求。
- 公网部署应把明文 TCP 信令放在 TLS 终结代理或可信内网之后，并限制 UDP 入口暴露。
- 被控端应只在可信网络中开放键鼠、剪贴板和文件接收权限。

## 10. 兼容性与维护规则

- `version` 变化时，旧客户端必须拒绝或进入明确迁移路径。
- 新字段应保持 JSON 向后兼容；关键安全字段缺失时拒绝握手。
- 包头布局、nonce 构造、HKDF info、FEC 分片头和 ACK 语义属于协议兼容性边界，
  修改必须同时更新测试和本文档。
- 视频、音频和可靠流的具体参数可以在版本内演进，但不能让接收端把缺片、伪 ACK
  或超时帧误判为完整数据。
- 真实跨机验收覆盖 NAT、弱网和长会话之前，本文档不提供性能承诺。
