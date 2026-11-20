# PDTP/1 协议规范

> PeerDesk Transport Protocol v1 —— 一条 UDP 连接上同时跑控制流、视频流、文件流，
> 自带加密、打洞、中继回退与 FEC。实现见 `peerdesk/wire.py`、`peerdesk/crypto.py`、
> `peerdesk/transport.py`。

## 1. 设计取舍

| 维度 | UU远程（被逆向对象） | PeerDesk | 理由 |
| --- | --- | --- | --- |
| NAT 穿透 | libwebrtc ICE（`basic_ice_controller`、`p2p_transport_channel`） | 自实现候选交换 + UDP 打洞 + 6s 后中继 | ICE 完整实现体量过大，两端都是我们的进程，候选集可以直接信令交换 |
| 加密 | DTLS-SRTP（`dtls_srtp_transport.cc`、`srtp_filter.cc`） | X25519 + HKDF + ChaCha20-Poly1305（每包 AEAD） | 免去 DTLS 握手状态机；连接码直接进 HKDF 当 PSK |
| 拥塞/可靠 | QUIC(ngtcp2) + KCP + 自研 mux + FEC | 视频不可靠 + 应用层 NACK/FEC；控制流独立可靠重传 | 视频不需要逐包重传，控制必须可靠 |
| 信令 | HTTPS REST（`/api/v1/transport/rpc/request`）+ 长连 | 纯 TCP JSON Lines | 自托管部署简单，无证书依赖（生产可套 TLS 反代） |
| 中继 | `Socks5HttpRelay`/`Socks5RelayRunnable` + secured relay | 服务端 UDP 转发，`RLY1` 头带配对 tag | 一个 UDP 端口即可服务全部会话 |

## 2. 报文格式

每个 UDP 数据报 = 20 字节明文头 + 密文体（AEAD 密文 + 16 字节 tag）。

```
 0        2   3   4               8               12  13  14    16        20
 +---------+---+---+---------------+---------------+---+---+-----+---------+
 |  magic  |ver|typ|      seq      |     ts_ms     |str|flg|plen | session |
 |  "PD"   | 1 |   |   uint32 BE   |   uint32 BE   | B | B | u16 | u32 BE  |
 +---------+---+---+---------------+---------------+---+---+-----+---------+
```

`struct "<2sBBIIBBHI"`（20 B，小端）：

| 字段 | 宽度 | 说明 |
| --- | --- | --- |
| `magic` | 2 | 固定 `b"PD"` |
| `ver` | 1 | 固定 `1` |
| `typ` | 1 | 见下表 |
| `seq` | 4 | 每包自增，同时是 AEAD nonce 计数器（重放窗口按它判重） |
| `ts_ms` | 4 | 发送端单调毫秒（会话相对，**不是** epoch 毫秒） |
| `stream` | 1 | 0=video 1=control 2=file 3=stats |
| `flags` | 1 | bit0 KEYFRAME，bit1 LAST_FRAG，bit2 RELIABLE |
| `plen` | 2 | 载荷长度 **+16**（含 AEAD tag），作为 AAD 一部分被校验 |
| `session` | 4 | 会话标识（握手后双方一致） |

`plen` 把 16 字节 tag 算进去，是为了让接收端在解密前就能按长度切包，且 AAD 里
天然包含"这一包应该有多长"这一事实——截断/追加都会解密失败。

### 2.1 消息类型

| 值 | 名称 | 方向 | 载荷 |
| --- | --- | --- | --- |
| 1 | HELLO | 双向 | JSON：`{role, nonce, ts, pub(32B hex), cands:[...], dev, v}` |
| 2 | HELLO_ACK | host→client | 同 HELLO 结构（host 侧） |
| 3 | HELLO_FIN | 双向 | 空，确认己方已收齐对端 HELLO |
| 4 | PUNCH | 双向 | 空，纯打洞探针（不可靠，0.1s 间隔） |
| 5 | PUNCH_ACK | 双向 | 空 |
| 6 | DATA | 双向 | 视频分片 / 控制消息 / 文件块，见 `stream` |
| 7 | PARITY | 双向 | `<IHBB>` frame_id, group_idx, shards, rsv + XOR 校验片 |
| 8 | ACK | 双向 | `<III>` ack_cum, bitmap_lo, bitmap_hi（64 位位图） |
| 9 | NACK | 双向 | `<IH>` frame_id, count + `count × u16` 缺失分片号 |
| 10 | PING | 双向 | 空，4s 心跳 |
| 11 | PONG | 双向 | 空 |
| 12 | BYE | 双向 | 空，优雅关闭 |

`UNRELIABLE_TYPES = {PUNCH, PUNCH_ACK, DATA, PARITY, PING, PONG}` —— 其余（HELLO*、ACK、NACK、BYE）走重传/独立处理。

## 3. 握手与密钥

```
client                                    host
  |-- HELLO {pub_c, cands, nonce_c} ------->|   (0.25s 一发，15s 超时)
  |<------------- HELLO_ACK {pub_h,...} ---|
  |-- HELLO_FIN --------------------------->|
  |<-------------------------- HELLO_FIN ---|
  |========= 双方各自派生密钥，开始 DATA ===|
```

1. 双方各生成一对 X25519 临时密钥，`pub` 放进 HELLO。
2. `transcript = SHA256("peerdesk/1" || 0x00 || hello_host_bytes || 0x00 || hello_client_bytes)`
   —— **按角色固定顺序**拼接，这样两端在不同时刻计算也得到同一份摘要。
3. `psk` 来自 9 位连接码：`psk = SHA256("peerdesk-psk" || salt || 0x00 || digits)`；
   `psk_digest = SHA256("peerdesk/1" || 0x00 || "psk" || 0x00 || psk)`。
4. `salt = SHA256(transcript || psk_digest)`；`shared = X25519(priv, peer_pub)`。
5. 方向密钥：
   - `K_h2c = HKDF-SHA256(ikm=shared, salt=salt, info="peerdesk/1 host->client", 32)`
   - `K_c2h = HKDF-SHA256(ikm=shared, salt=salt, info="peerdesk/1 client->host", 32)`
6. 每包 `nonce = struct ">IQ"(direction, seq)`：host 发送用 direction=0，client 用 1；
   `AAD = 20 字节头`。接收端方向相反（host 收 client 的包用 direction=1）。

> HELLO 重发必须复用**同一份 body 字节**（`_hello_host_bytes` / `_hello_client_bytes` 缓存）。
> 早期实现每次重发都重建带时间戳的 body，导致两端 transcript 不一致、密钥不同——
> 表现为"握手成功但所有数据解密失败"，是联调时踩过的坑。

### 3.1 重放/乱序

`ReplayWindow(width=2048)`：维护"已见最高位 + 位图"，`seq` 已在窗口内则丢弃；
解密失败不推进窗口之外的状态。ACK/NACK 与视频乱序靠各自序号独立处理。

## 4. 控制流（可靠）

控制消息装在 `stream=CONTROL` 的 DATA 包里，载荷前 4 字节是**消息序号**：

```
DATA payload = struct "<I"(msg_seq) || body
```

**注意**：包头 `seq` 是 AEAD nonce 计数器（每包自增），消息序号必须单独放在载荷里。
把消息序号塞进包头会造成 nonce 复用 + 重放窗口丢弃 → ACK 全丢、控制通道永远不确认。

`ReliableStream`：

- 发送：`seq` 从 0 递增，`pending[seq] = [payload, deadline, retries]`。
- RTO：初始 150 ms，每次重传翻倍，上限 1200 ms；重传 14 次后放弃（视为链路失效）。
- 接收：严格按序交付，乱序先缓存；重复丢弃。
- ACK：`ack_cum = recv_next - 1` + 64 位位图（位 i 表示 `ack_cum-i` 已收到）。
  **累积语义**：收到 `ack_cum` 即认为 ≤ `ack_cum` 全部落地。

上层消息：`hello/settings/keyframe_req/mouse/key/text/clip/file_begin|data|end/reboot/stats/info`（JSON，UTF-8）。

## 5. 视频流（不可靠 + FEC + NACK）

1. 抓屏 → JPEG（自适应质量 32–92，目标 ~140 KB/帧）→ 分片：
   `VIDEO_FRAG = struct "<IHH"(frame_id, frag_idx, frag_cnt) || chunk`，每片 ≤ `PAYLOAD_MTU - 6`。
2. 每 8 片一组（`FEC_K=8`，组内 ≥3 片才发校验）：
   `PARITY = "<IHBB"(frame_id, group_idx, shards, 0) || XOR(组内各片, 按最长片 0 补齐)`。
3. 接收端按 `frame_id` 建 `FragmentAssembler`；收齐即交付 → 送 Web UI。
4. 缺片时：
   - 有同组校验片且只缺 1 片 → 直接 XOR 恢复（`parity ^ 其余片 = 缺失片`）。
   - 否则每 50 ms 对在途帧（≤4 帧）发 NACK，最多带 32 个缺片号；发送端从 10 帧回滚缓存里重传。
5. 帧超过 **450 ms** 未拼齐即丢弃；若丢的是关键帧，回一条 `keyframe_req` 要新的 IDR。
6. 发送端每秒推一次 `stats`（fps/Mbps/RTT/质量/编码耗时）。

## 6. 打洞与中继

- 候选来源：本机全部 IPv4（`local_ipv4s()`）+ 服务端 STUN 得到的 srflx + 对端经信令交换的候选。
- 打洞：向全部候选每 100 ms 发 PUNCH，收到任意 PUNCH/PUNCH_ACK 即锁定该路径（direct）。
- 6 秒仍未直连 → 升级中继：数据报外面套 `RLY1` 头

```
 +--------+----------------+------------------+
 | "RLY1" | tag (uint32)   | 原始 PDTP 报文   |
 +--------+----------------+------------------+
```

  服务端按 tag 找到配对的另一端转发（tag 由服务端在 `connect` 时生成，奇数为 agent、偶数为 viewer）。
- 中继模式下 RTT/带宽由服务端承担，客户端 UI 显示"中继"。
- `--relay-only` 强制走中继（自测/对称 NAT 排障用）。

## 7. STUN（自实现子集）

与媒体共用同一个 UDP socket（保证 srflx 端口就是媒体端口），靠报文前两字节区分：

- `0x0001` = Binding Request，`0x0101` = Binding Success。
- magic cookie `0x2112A442`，12 字节 txid。
- 只实现 `XOR-MAPPED-ADDRESS(0x0020)`；解析时**必须校验 family 字节**
  （0x01=IPv4 需要 4 字节，0x02=IPv6 需要 16 字节），否则坏包会被拼成"合法但错误"的地址。

## 8. 信令协议（JSON Lines over TCP）

```
client → server: {"t":"register","code":"604173829","name":"PC-01","cands":[{host,port,kind}]}
server → client: {"t":"registered","code":"604 173 829","relay":[host,port],"server_time":...}

viewer → server: {"t":"connect","code":"604173829","cands":[...],"name":"..."}
server → agent : {"t":"viewer","sid":..,"cands":[viewer 候选],"name":..,"relay":[h,p],"relay_tag":T_a}
server → viewer: {"t":"peer","sid":..,"cands":[agent 候选],"name":..,"relay":[h,p],"relay_tag":T_v}

两端后续增量候选：{"t":"cands","cands":[...]} → 对端收到 "peer_cands"
保活：{"t":"ping"} → {"t":"pong"}
关闭：{"t":"bye"} → 对端 "peer_bye"；TCP 断开 → 对端 "peer_gone"
错误：{"t":"error","reason":"bad_code|offline|unknown_type"}
```

服务端只保存 `code_id = SHA256("peerdesk-id"||0x00||digits)[:32]`，**不落明文连接码**。

## 9. 安全边界（如实说明）

- 连接码即 PSK：知道 9 位码的人可以完成握手（与 UU远程同一模型）。9 位数字空间 10⁹，
  服务端建议加尝试频率限制（路线图）。
- 服务器能看到元数据（谁连谁、候选地址、流量大小），看不到媒体内容（无密钥）。
- 中继是明文转发密文包，无法解密，但可以丢包——属可用性问题不是机密性问题。
- 控制通道有重放保护；视频通道不做逐包重放保护（丢一帧无害，且 seq 单调配合窗口过滤）。
