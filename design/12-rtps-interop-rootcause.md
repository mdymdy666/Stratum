# RTPS 互通：根因定位与端到端打通（接 11 号交接文档，取代其结论）

> 定位日期：2026-10-08
> 工具说明见 [`tools/fastdds_sub_diag/README.md`](../tools/fastdds_sub_diag/README.md)；
> 时序记录见 [`AI_COLLAB.md`](../AI_COLLAB.md) 续 16、续 17。

---

## 0. 一句话结论

**端到端已经打通**：Stratum 回放 32 条 → 上游 `hello_world subscriber` 收到 **32/32**。

11 号交接文档的假设（"Fast DDS 在解析或 QoS 判定阶段拒绝了我方 Publication"）
**不成立**。真实原因是 Stratum 自身一连串**静默失败**，共 5 个缺陷：

| # | 缺陷 | 症状 | 证据 |
|---|---|---|---|
| 1 | `StratumDB::ListTopics` 丢 `type_name` → `BuildPublication` 返回空 → **Publication 静默不发** | Fast DDS 完全看不到我方 Writer | gdb：`BuildPublication(topic=…, type="")` 返回 length 0 |
| 2 | 发布端**不处理 HEARTBEAT** → 不回 ACKNACK → 拿不到对方 SEDP Subscription | `sedp_sub=0`，`has_user=0` | 逐子消息 dump：收到 78 个 HEARTBEAT、0 条 Subscription |
| 3 | `Player`/`Recorder` **丢弃调用方的 domain**（自造默认 `BackendConfig`） | 非 0 域永不互通；进程声称 domain=3 实际绑 domain 0 的 9060 | `/proc/net/udp` 端口对照 |
| 4 | SEDP Publication 与用户数据**共用 SN 计数器** → 用户数据 SN 跳号 | 对端认为有缺口，**一条样本都不交付** | 心跳声明 `last=65` 而实际只发 32 条 |
| 5 | 学到的 user locator **写给所有 peer** | 多订阅端时只有一个能收到数据 | `[send]` 只出现一个目标端口 |

缺陷 3~5 是在修掉 1、2 之后逐层暴露的 —— 每修一层，问题就前移一层。

---

## 1. 证据链（可复现）

### 1.1 缺陷 1：Publication 从未发出

```bash
gdb -batch -ex 'break stratum::protocol::rtps::RtpsWriter::BuildPublication' \
    -ex 'run /tmp/tl_final 0 10.0' -ex 'finish' --args /tmp/stratum_rtps_replay
# → topic="hello_world_topic", type=""   Value returned is length 0, capacity 0
```

原始抓包（`capture_udp`，不做任何语义解析）：

```
修复前：DATA writer 统计 → USER_W ×32、SPDP_W ×13、SEDP_PUB_W ×0
修复后：DATA writer 统计 → SEDP_PUB_W ×34
```

### 1.2 缺陷 2：收到心跳却不回应

```
[rx] DATA writer=0x000100c2  × 294     （SPDP）
[rx] HEARTBEAT               ×  78     （对端在等我们 ACKNACK）
[rx] DATA writer=0x000004c2  ×   0     （SEDP Subscription 一条都没到）
```

### 1.3 缺陷 3：domain 被中间层丢弃

```bash
/tmp/stratum_rtps_replay /tmp/tl_final 3 10.0 &   # 声称 domain=3
sleep 4; grep -c ':2382' /proc/net/udp            # 9090: 0
grep -c ':2364' /proc/net/udp                     # 9060: 1  ← 实际在 domain 0
```

### 1.4 缺陷 4：SN 跳号

```
[hb] -> …:8161 writer=0x00000103 first=1 last=65   # 而只发布了 32 条
修复后：[hb] … first=1 last=32
```

### 1.5 缺陷 5：locator 写给所有 peer

```
修复前：[send] has_user=1 -> 172.17.211.143:7439                    （唯一目标）
        上游 subscriber RECEIVED=30；自研诊断程序 samples=0
修复后：[send] has_user=1 -> …:7411 / …:7413 / …:7439               （各自独立）
        上游 subscriber RECEIVED=32（32/32）
```

### 1.6 端到端结果（历史首次）

```
$ tools/stratum_rtps_replay /tmp/tl_final 0 1.0
$ hello_world subscriber -s 40
Subscriber matched.
Message: 'Hello world' with index: '4' RECEIVED
… RECEIVED = 32
```

---

## 2. 修复清单

| 文件 | 改动 |
|---|---|
| `src/src/storage/lsm_v2/stratum_db.cc` | `ListTopics` 解码回填 `type_name` |
| `src/rtps/protocol/rtps_writer.{h,cc}` | ① Publication 构造失败返回 `InvalidArgument` + `publications_build_failed` 计数；② `HandleHeartbeat`/`SendAcknackToPeer`（复用 `ProcHeartbeat`，位图覆盖 `[firstSN,lastSN]`）；③ 心跳改发对端 user locator；④ 独立 `sedp_sn_` 计数器；⑤ `HandleSedpSubscription` 按 GUID 前缀定向写 locator；⑥ `STRATUM_DUMP` 增加逐子消息 / SPDP peer（含来源）/ 心跳 / 负载头 dump |
| `src/include/stratum/core/recorder.h`、`src/src/core/{player,recorder}.cc` | `PlayerConfig::backend` / `RecorderConfig::backend`，把调用方的后端配置交给后端 |
| `tools/stratum_rtps_{replay,record}.cc` | 显式传 `backend`（工具侧） |
| `tests/support/mock_backend.h` | 记录 `CreateSource/CreateSink` 收到的 config |
| `tests/review/test_topic_meta_type_name.cc` | 新增 2 用例（type_name 跨重开 / Player 传递） |
| `tests/rtps/test_publication_guard.cc` | 新增 2 用例（空 type_name 显式失败 / 只公告一次） |
| `tests/review/test_backend_config_propagation.cc` | 新增 3 用例（Recorder/Player 传 domain / 默认不变） |
| `tools/fastdds_sub_diag/*` | 诊断程序、抓包器、分析器、8 个场景脚本、README |

---

## 3. 测试

| 项目 | 结果 |
|---|---|
| 全量 `ctest` | **33/33 通过**（原 30 + 新增 3 个测试文件 / 7 个用例） |
| 交叉验证 | 上游 `hello_world subscriber` 收到 **32/32** |
| 场景脚本 | `01` 核心、`01b` 端口对照、`01c` sedp_recv、`01d` 原始抓包、`02` 协议统计、`03` 上游对照、`04` 随机+压测、`05` Stratum↔Stratum 回归、`06` SPDP 自检 |

---

## 4. 仍然遗留

## 5. 遗留项（截至续 20）

**已全部解决的主要项**：

* 自研诊断程序 `samples=0` → participant 事件掩码（续 18）；
* 录制方向只收到 1 条 → SN 读取偏移 + readerId 一致性（续 20，见下）；
* `stratum_rtps_record` 参数顺序误用导致的「录制不通」误判（续 19）。

**续 20 的两个字节级 bug**：

1. **writerSN 读取偏移少 4 字节**：writerSN 是 8 字节，`ids+8` 是高 4 字节
   （恒 0），低 4 字节在 `ids+12`。旧实现让**所有样本 sequence=0**，
   数据质量报告的「序号缺口」检测形同虚设。
2. **声明的 reader entityId 与 ACKNACK 不一致**：
   `reader_guid.entityId.value[3]` 是单字节，声明出去变成 `0x00000004`，
   而 ACKNACK 回 `0x00000104` → 对端静默忽略 ACKNACK → writer history
   永不释放 → 上游 publisher 只发 1 条。现统一为常量 `kUserReaderEntityId`。

**续 22 已修复**：

1. **默认 GUID 前缀不符合 RTPS 9.3.1.1**：此前是常量，导致同机每次运行、
   每个进程 GUID 相同。已改为 `vendorId|hostId|processId|counter`。
   （注：我原本假设它能解释「缺首条」，**该假设已被实测推翻**，规范修复本身保留。）
2. **ACKNACK 按 SN 精确记账**（Reader + Writer 双侧）：旧实现是「收到一次就
   全部置 acked」与「整段报缺失」，现改为按 `base_sn` + 大端位图判定真实缺口。
   → 8 秒 `retrans` 从 **920 降到 18（恒定）**，Stratum↔Stratum 从 11 降到 1。
3. **只在首次匹配时补发** pending：此前每次收到 Subscription（~100ms 周期公告）
   都补发，重传线性增长；现在只在 `has_user` 首次置位时补发。
4. **Writer 侧 DATA 的 writerSN 读取偏移**同步修正（`ids+12` 才是低 4 字节）。

**续 23 已修复**：

1. **首条边界 → 已解决**：Player 记录首条样本，收尾用**新 SN** 重新发布。
   条数对账 199/200 → **200/200**、118/119 → **119/119**。
   （被放弃的路径：按 ACK 的 acked 标志判断 —— `base_sn` 无法区分
   「已收到」与「不想要」，实测两头落空。）
2. **大 payload 完全不可用 → 已修复**（本轮发现）：payload ≥ ~2 KiB 时
   回放完全不可达（8000B: 0/20、60000B: 0/10）。根因是
   `RtpsWriter::BuildDataPacket` 这份**重复实现**用固定 2048 字节缓冲，
   `addData` 失败被忽略 → 发出只有头部（56 字节）的空负载报文；
   而 `packet::BuildDataPacket` 早已修好同一问题（有注释与单测守护），
   副本未同步 —— 典型的「重复实现导致修复遗漏」。已改为委托，消除分叉。
   修复后：1200B/8000B/60000B **全部完整送达**。
3. 发送失败与 `addData` 失败不再静默（计数 + errno/容量 dump）。

**仍然遗留**：

1. **>64 KiB 的单条样本**需要 `DATA_FRAG`，本实现未支持；当前会因
   `octetsToNextHeader`（16 位）溢出而失败 —— 应显式报错而非发坏包。
2. **验证覆盖仍窄**：keyed topic、多话题并发、非默认 QoS 组合、跨主机未验证。
3. 组播加入失败告警与 WSL 之外的端口占用仍在（干扰 domain 0 偶发失败）。

**验收脚本**：`scenarios/01`~`09`（含 `08_accounting.sh` 条数对账、
`09_payload_sizes.sh` 大 payload 验证）。

## 6. 工具与验收（不再依赖 /tmp）

`/tmp` 会被系统清理，曾有「工具与时间线一起消失导致误判失败」的事故。现在：

* 工具产物固定在 **`build/tools/`**（`fastdds_sub_diag`、`capture_udp`、
  `stratum_rtps_replay/record`、`rtps_sink_diag2`、`rtps_reader_diag`、`make_timeline`）；
* `make_timeline`（新增）用 `TimelineImplV2` API 生成**内容已知**的时间线，
  端到端验收自包含、可重复；
* 验收命令：

```bash
cd /UserData/CodexWorkSpace/Stratum
bash tools/fastdds_sub_diag/build.sh && bash tools/fastdds_sub_diag/build_tools.sh
build/tools/make_timeline build/tools/tl_accept 32
bash tools/fastdds_sub_diag/scenarios/07_upstream_subscriber.sh build/tools/tl_accept
# 期望：已发布样本 = 32 / 订阅端收到样本：32 / 判定：PASS
```

---

## 遗留项状态同步（2026-10-08）

根因链已全部闭环；以下是**后续发现并处理**的项，与 `docs/02-open-issues.md` 对应：

| 项 | 状态 |
|---|---|
| 对外 locator 硬编码 `127.0.0.1`（跨主机致命） | ✅ 已修（`DetectLocalAddress` + `advertised_address`）；**跨主机本身未实测**（环境不允许） |
| 录制器启动 `rm -rf` 静默清空时间线（O19） | ✅ 已修（默认拒绝非空目录） |
| 高速回放丢包（O2） | ✅ 已定性：接收端内核 `RcvbufErrors`；录制方向已加大 `SO_RCVBUF` 修复 |
| ≥5% 丢包不能完全收敛（O20） | ⚠️ 已量化（1% 收敛 / 5% 94.5% / 10% 92.5%），**成因未定位** |
| ACKNACK 位图解析分支 | ⚠️ **实测从未触发**（防御性代码，保守路径兜底），**无实测证据** |
| `pending_` 裁剪（O11） | ✅ 已被 `17` 的 5000 条用例覆盖 |
| DATA_FRAG / IPv6 | ⛔ **已决策不做**（理由见 `design/16` D2/D3） |
