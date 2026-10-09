# fastdds_sub_diag —— 最小 Fast DDS 订阅诊断程序

> 目的：拿到 **Fast DDS 为什么没有接受 Stratum 的 SEDP Publication** 的确切原因。
> 背景与事实清单见 [`design/11-rtps-interop-handoff.md`](../../design/11-rtps-interop-handoff.md)
> 与 [`AI_COLLAB.md`](../../AI_COLLAB.md) 续 16。

---

## 1. 为什么需要自己写一个订阅端

Stratum ↔ Stratum 的 RTPS 互通已经打通，但与 Fast DDS 的互通卡在
"我方 SEDP Publication 未被接受"。两条"省事"的取证路径都已被证伪：

| 尝试 | 结果（实测） |
|---|---|
| 环境变量 `FASTDDS_LOG_VERBOSITY` | libfastdds 中不存在该符号 |
| XML `<log><verbosity>` | Fast DDS 3.6 `parseLogConfig` 不接受该标签 |
| `Log::SetVerbosity(Info)` | **API 可用，但本机 libfastdds 是 Release 构建**：`HAVE_LOG_NO_INFO` 使 `EPROSIMA_LOG_INFO_IMPL_` 为空宏，Info 日志**在编译期就被删掉了**；只有 Warning/Error 会输出 |

因此需要一个自己控制的 Fast DDS 订阅端，配合 `STRATUM_DUMP` 的字节级证据，
把"哪一跳出了问题"钉死。

## 2. 目录内容

```
tools/fastdds_sub_diag/
├── README.md                    本文件
├── fastdds_sub_diag.cc          诊断订阅端（主程序）
├── build.sh                     编译主程序（链接真实 libfastdds）
├── capture_udp.cc               原始 UDP 抓包器（不做 RTPS 语义解析）
├── build_capture.sh             编译抓包器（链接 Stratum 的 rtps 基础层）
├── analyze_capture.py           分析抓包文件（按 writer entity 统计到达情况）
├── build_tools.sh               重新链接 tools/ 下的协议诊断工具
├── types/                       复用 examples/cpp/hello_world 的 HelloWorld
│                                TypeSupport（与上游 md5 一致，未重新生成 IDL）
└── scenarios/                   可复现场景脚本（见第 5 节）
```

## 3. 编译

> 工具产物统一放在 **`build/tools/`**（不再用 /tmp —— 它会被系统清理，
> 曾导致「验证工具消失」被误判成「验证失败」）。

```bash
cd /UserData/CodexWorkSpace/Stratum
bash tools/fastdds_sub_diag/build.sh          # → build/tools/fastdds_sub_diag
bash tools/fastdds_sub_diag/build_capture.sh  # → /tmp/capture_udp
bash tools/fastdds_sub_diag/build_tools.sh    # → /tmp/{stratum_rtps_replay,rtps_sink_diag2,rtps_reader_diag}
```

> `tools/` 下的程序**不纳入 Stratum 的 CMake**（保持主项目"核心零外部依赖"）。
> `src/` 改动后必须重新跑 `build_tools.sh`，否则验证用的是旧二进制。

## 4. 主程序用法

```bash
export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu

/tmp/fastdds_sub_diag [seconds] [domain] [topic] [选项]
  --verbosity=info|warning|error
  --reader-reliability=reliable|besteffort
  --reader-durability=volatile|transient_local
  --reader-representation=xcdr1|xcdr2|both
  --meta-port=<n>          固定本端 metatraffic 单播端口
  --user-port=<n>          固定本端 user 单播端口
  --peer=<ip:port>         初始对端（可重复；本环境组播不可靠，用它绕开）
  --local-address=<ip>     只在指定接口收发（如 127.0.0.1）
```

输出三类信息：

1. Fast DDS 自身的 Warning/Error 日志（原样转发，含 `INCOMPATIBLE QOS` 之类的原话）；
2. 事件行：`[diag][PDP]` / `[diag][EDP]` / `[diag][MATCH]` / `[diag][DATA]` /
   `[diag][QOS]` / `[diag][LIVELINESS]` / `[diag][REJECTED]`；
3. 机器可解析的汇总行 `DIAG_SUMMARY ...`（含 `writers_discovered`、
   `matched_events`、`samples`、`data_available_calls`、`invalid_data_samples` 等）。

**三个关键观测量**（用来区分不同失败模式，缺一不可）：

| 观测量 | 含义 |
|---|---|
| `[diag][EDP] DISCOVERED_WRITER` | Fast DDS 是否**成功解析并登记**了我方 Publication（能到这一步说明参数表、KEY_HASH、type_name 都没问题） |
| `[diag][MATCH]` | 是否通过 `EDP::valid_matching` 的匹配判定 |
| `data_available_calls` | 数据是否**真的到达**了 DataReader（0 表示数据平面没通，而不是反序列化失败） |

## 5. 场景脚本

| 脚本 | 回答什么问题 |
|---|---|
| `scenarios/01_stratum_replay.sh` | 核心场景：诊断端 ← Stratum 回放（`/tmp/tl_final`，32 条） |
| `scenarios/01b_port_compare.sh` | Stratum 的发送目标 vs 诊断端真实监听端口 |
| `scenarios/01c_sedp_recv.sh` | 用 Stratum 自带 `sedp_recv` 看它自己发出的 SEDP |
| `scenarios/01d_capture.sh` | **原始抓包**（不做语义解析）确认哪些报文真的到达 |
| `scenarios/02_direct_sink.sh` | 诊断端 ← `RtpsSink`（带 `sedp_sub`/`acks`/`retrans` 协议统计） |
| `scenarios/03_fastdds_publisher.sh` | **对照组**：诊断端 ← 上游 hello_world publisher |
| `scenarios/04_stress_random.sh` | 随机 + 压测（多轮随机参数，断言摘要自洽并统计资源） |
| `scenarios/05_stratum_to_stratum.sh` | **回归**：确认新增的 HEARTBEAT/心跳改动没有破坏 Stratum↔Stratum 通路 |
| `scenarios/07_upstream_subscriber.sh` | **端到端验收**：Stratum 回放 → 上游 hello_world subscriber（32/32 判据） |
| `scenarios/06_spdp_selfcheck.sh` | 抓取诊断程序自身发出的 SPDP，核对它公告的 metatraffic port |

统一用法：

```bash
DOMAIN=3 DIAG_SECONDS=25 bash tools/fastdds_sub_diag/scenarios/01_stratum_replay.sh
```


> **本文件的第 6~8 节已被两轮修复推翻/更新**：端到端（Stratum → 上游
> `hello_world subscriber`）现已打通 32/32。最新的根因链、证据与遗留项见
> [`design/12-rtps-interop-rootcause.md`](../../design/12-rtps-interop-rootcause.md)。

## 6. 结论摘要（本次诊断拿到的事实）

### 6.1 直接原因：Stratum 从来没有发出 Publication

`gdb` 在 `RtpsWriter::BuildPublication` 的**返回值**上给出直接证据：

```
Breakpoint 1, RtpsWriter::BuildPublication (topic="hello_world_topic", type="")
657     if (!pub.empty()) {
Value returned is $1 = std::vector of length 0, capacity 0
```

`type` 是**空字符串**。根因链（每一跳都有证据）：

```
StratumDB::ListTopics 只回填 topic_name/topic_id，丢掉元数据里已存好的 type_name
  → TimelineImplV2::LoadTopicsLocked → topic_meta_.type_name 为空
  → player.cc: item.sample.type_name = t.type_name  （空）
  → RtpsWriter::Publish(topic, "")
  → PID_TYPE_NAME 序列化失败（ParameterString_t 拒绝空串）
  → BuildPublication 返回空 vector
  → 旧代码 `if (!pub.empty())` **静默跳过发送**
  → Fast DDS 永远发现不了我方 Writer → 不回 SEDP Subscription
  → has_user 恒为 0 → 32 条数据全被发到 metatraffic 端口（7410/7438）
  → 对端 DataReader 收到 0 条
```

**这解释了为什么"补 QoS / 补 KEY_HASH / 改 DATA_REPRESENTATION"全都没有效果**——
那些字段根本没有离开本机。

### 6.2 第二个真缺陷：发布端不响应 HEARTBEAT

修好 6.1 之后，Fast DDS 侧立刻出现：

```
[diag][EDP] DISCOVERED_WRITER guid=010f5354:00000103 topic="hello_world_topic"
            type="HelloWorld" reliability=2 durability=0
[diag][MATCH] change=1 current_count=1
```

但 Stratum 侧 `sedp_sub` 仍为 0。逐子消息 dump 给出原因：

```
[rx] DATA writer=0x000100c2   × 294   （SPDP）
[rx] HEARTBEAT                ×  78   （对端 SEDP writer 在等我们请求）
[rx] DATA writer=0x000004c2   ×   0   （SEDP Subscription —— 一条都没到）
```

Fast DDS 的 SEDP 公告走 RTPS 可靠传输："HEARTBEAT → 对端 ACKNACK → 重传历史"。
`RtpsWriter::ReceiveLoop` 只处理 `ACKNACK` 与 `DATA`，**没有 HEARTBEAT 分支**，
于是永远不回 ACKNACK，也就永远拿不到对端的 Subscription。
修复后 `sedp_sub` 从 0 → 240，`[send] has_user=1 -> …:7439`（数据终于发到 user 端口）。

### 6.3 仍然阻塞在环境层（有对照证据）

即使把数据发到了正确的 user 端口，Fast DDS 的 `data_available_calls` 仍为 0。
**对照实验**（`scenarios/03_fastdds_publisher.sh`）证明这与 Stratum 无关：

```
上游 hello_world publisher → 本诊断程序：
  Publisher matched.
  Message: 'Hello world' with index: '1' SENT   ... '10' SENT
  DIAG_SUMMARY ... matched_events=1 samples=0 data_available_calls=0
```

即**上游 Fast DDS 自己的 publisher 与 subscriber 在本环境也传不了用户数据**。
环境证据：

* `[TRANSPORT_UDPV4 Warning] Error joining multicast group on 10.255.255.254:
  Address already in use`（组播加入失败）；
* `/proc/net/udp` 显示 7400 / 7438 / 7439 已被绑定，但**遍历 `/proc/*/fd`
  找不到属主进程** —— 典型的 WSL mirrored networking（端口被 WSL 之外的进程占用）；
* 非 0 domain（3/5）下诊断端的 `participants_discovered=0`，连 SPDP 都收不到，
  而 Stratum 从 SPDP 解析出的唯一 peer 是 **domain 0 的端口 7438**
  （`[spdp] peer prefix=010f1b9b locators=2: 10.255.255.254:7438 172.17.211.143:7438`）。

已尝试且**无效**的绕行手段：禁用 SHM（`FASTDDS_BUILTIN_TRANSPORTS=UDPv4`）、
固定 metatraffic/user 端口、`initialPeersList` 单播、`interfaceWhiteList=127.0.0.1`、
更换 domain（3/5）。**结论：端到端样本投递需要在网络模式正常的机器上复验**
（见第 8 节）。

## 7. 本次改动

| 文件 | 改动 |
|---|---|
| `src/src/storage/lsm_v2/stratum_db.cc` | `ListTopics` 解码并回填 `type_name`（根因修复） |
| `src/rtps/protocol/rtps_writer.h/.cc` | `BuildPublication` 失败不再静默：返回 `InvalidArgument` + `publications_build_failed` 计数；新增 HEARTBEAT→ACKNACK（`HandleHeartbeat`/`SendAcknackToPeer`，复用 `ProcHeartbeat`）；心跳改发对端 user locator；`STRATUM_DUMP` 增加逐子消息与 SPDP peer dump |
| `tests/review/test_topic_meta_type_name.cc` | 新增回归：type_name 跨重开存活 + Player 正确传递 |
| `tests/rtps/test_publication_guard.cc` | 新增回归：空 type_name 必须显式报错并计数 |
| `tests/CMakeLists.txt` | 注册上述两个测试 |

## 8. 未完成项与下一步建议

* **端到端样本投递（`samples>0`）在本 WSL 环境未打通**，根因是网络环境
  （第 6.3 节），不是 Stratum 的逻辑缺陷。建议在以下任一条件下复验：
  * 关闭 WSL mirrored networking（或改用 NAT 模式）；
  * 在原生 Linux / 独立主机上跑 `scenarios/01_stratum_replay.sh`；
  * 确认 `7400`、`7410`、`7411`、`7438`、`7439` 等端口没有 WSL 之外的占用者。
* **ACKNACK 内容仍不理想**：Fast DDS 侧持续打印
  `Inconsistent acknack received ... requested range is [1, 9]`。
  当前实现每收到一次 HEARTBEAT 就请求整个 `[firstSN, lastSN]`，
  应该改成"只请求当前缺失区间"（`RtpsReader` 侧同样值得核对）。
* **`HandleSedpSubscription` 会把同一个对端 locator 写给所有 peer**
  （`for (auto& p : peers_)`），在多对端场景下会把数据发错目标；
  应按 GUID 前缀匹配对应 peer 再设置 `has_user`。
* **重传放大**：`FlushPendingToPeers` 在每次 ACKNACK/Subscription 后重发全部 pending，
  实测 8 秒内 `retrans` 涨到 920。应按 SN 精确重传。
