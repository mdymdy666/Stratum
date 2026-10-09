# 14 · 剩余工作队列（长期推进依据）

> 注：本文件最初误用 13 号，与既有的 `13-next-session-prompt.md` 冲突，
> 已更名为 `14-remaining-work.md`。

> 建立于 2026-10-08。**每轮推进结束都要回到本文件更新勾选状态与证据**，
> 并在 `AI_COLLAB.md` 追加一条带时间戳的记录。
> 目标：**基本功能补齐 + 全方位测试完成**。
> 相关：`design/12-rtps-interop-rootcause.md`（根因与证据）、`tools/fastdds_sub_diag/`（12 个场景脚本）。

---

## 队列状态：**全部闭合**（2026-10-08）

| 组 | 项 | 状态 |
|---|---|---|
| A 功能 | A1 >64 KiB 显式失败 / A2 多话题 / A3 非默认 QoS / A4 keyed | ✅ 全部完成 |
| A 增强 | A5 DATA_FRAG 分片 | 明确**不做**（超出基本功能；A1 已保证超限样本显式失败、不发坏包） |
| B 测试 | B1 压测 / B1b 延迟抖动 / B2 随机套件 / B3 soak / B4 跨主机 | ✅ 全部完成（B4 未实测已标注） |
| B 限制 | B5 回放流控 | ✅ 实现完成、默认关闭；高速丢包列为**已知限制 L1** |
| C 验收 | C1 场景脚本（19 个）/ C2 测试报告 / C3 文档同步 | ✅ 全部完成 |

**验收口径与量化结果见 [`design/15-test-report.md`](15-test-report.md)。**

> **本文件的维护教训（踩过一次）**：批量替换队列条目时，**不要**用
> `find("下一个条目标题")` 当替换终点 —— 中间若有别的条目会被整段删除。
> 本次 `B2/B3/B4/B5` 就是这样被误删的（已按 `AI_COLLAB.md` 的记录重建）。
> 正确做法：以"下一个 `- [` 行"或条目自身的结尾为界，并在替换后核对条目数量。

## 0. 当前基线（每轮先确认，不得退化）

| 基线项 | 当前值 | 复现 |
|---|---|---|
| 全量 `ctest` | **33/33** | `cd build && ctest` |
| 条数对账 A（200 条 → 回放） | **200/200** | `scenarios/08_accounting.sh` |
| 条数对账 B（真实录制 → 回放） | **119/119** | `scenarios/08_accounting.sh` |
| 大 payload（1200/8000/60000 B） | **全部完整送达** | `scenarios/09_payload_sizes.sh` |
| 回放 → 上游 subscriber | PASS | `scenarios/07_upstream_subscriber.sh` |
| Stratum ↔ Stratum | PASS | `scenarios/05_stratum_to_stratum.sh` |
| 录制方向（真实 publisher → Stratum） | `序号缺口: 0` | `scenarios/08` B 段 |
| 重传量（8 秒） | `retrans` 恒定 18 | `scenarios/02_direct_sink.sh` |

**纪律**：任何一项退化都必须先修好再推进新项。

---

## A. 功能补齐

- [x] **A1 · >64 KiB 样本必须显式失败** ✅ 2026-10-08（续 24）
  现状：`octetsToNextHeader`（16 位）会溢出，构造出畸形报文并被对端静默丢弃。
  要求：在报文构造处按上限校验，返回明确错误（`Status::InvalidArgument`，
  提示需要 `DATA_FRAG`），并计入失败统计；**不得发出坏包**。
  验证：`make_timeline <dir> 5 70000` → 回放端明确报错、订阅端 0 条且失败可见；
  60000 B 仍正常（回归）。
  **证据**：70000 B × 5 → 回放端 `已发布样本=0`、逐条打印「样本过大 … 需要 DATA_FRAG」（5 次）、订阅端 0/5 且失败可见；
  60000 B × 10 回归 10/10；`ctest` 33/33；条数对账 200/200 与 119/119。
  实现：`packet::BuildDataPacket` 入口按 65535 校验；`RtpsWriter::Publish`
  对空报文计 `publications_build_failed`；`Player` 不再忽略 `Publish` 返回值。

- [x] **A2 · 多话题并发** ✅ 2026-10-08（续 27）
  **发现真缺陷**：RTPS 中一个 DataWriter 只对应一个 topic，而此前所有话题
  共用固定 `0x00000103`，且 `next_sn_` 全局共享（单 writer SN 会跳号）。
  **修复**：① 每话题独立 entityId（entityKey 递增，key=1 即原值 → 单话题零回归）；
  ② 每 writer 独立 SN（`writer_sn_` + `NextSnFor`）；③ 心跳遍历所有 writer。
  **验证**：`hello_world_topic` 100 条 + `hello_world_topic_2` 60 条（index 故意错开
  到 1001..1060），两个诊断程序实例分别订阅 →
  样本总数 100 / 61，**区间外（串扰）0 条**；单话题对账 200/200、119/119。
  **工具**：`make_timeline <dir> <count> [payload_bytes] [topic2_count]`。
- [x] **A3 · 非默认 QoS 组合** ✅ 2026-10-08（续 28）
  **工具**：新增 `fastdds_pub_diag`（QoS 可配置的最小发布端）+ `rtps_reader_diag`
  的 `--reader-durability/--reader-reliability`；矩阵固化为 `scenarios/11_qos_matrix.sh`。
  **结果 6/6 符合规则**：writer TL + reader VOLATILE → 接受（10 条）；
  writer VOLATILE + reader TL → **正确拒绝**（qos_rej=1）；writer BE + reader RE → **拒绝**；
  writer TL + reader TL → 接受；writer BE + reader 默认 → 接受（修复后 10 条）。
  **顺带修复真缺陷**：`BuildSedpAnnouncementForReader` 无论 `requested_qos` 是什么
  都声明 RELIABLE+VOLATILE → 对端按声明判定 ⇒ **BEST_EFFORT writer 完全无法匹配**
  （实测 publisher `matched_now=0`、本端 0 条）。现改为传 `config_.requested_qos`。
- [x] **A4 · keyed topic** ✅ 2026-10-08（续 30）
  **决定性根因**：Fast DDS 的 **topic_kind 由 entityId 末字节决定**
  （Writer `0x03`=NO_KEY / `0x02`=WITH_KEY；Reader `0x04`=NO_KEY / `0x07`=WITH_KEY，
  见 `WriterProxyData.cpp:1443-1450`、`ReaderProxyData.cpp:1378-1385`）。
  我们固定用 `0x00000004`(NO_KEY)，对 unkeyed 话题恰好一致；遇到 keyed writer
  （`ShapeType` 带 `@Key`，末字节 `0x02`）就 topic_kind 不匹配 → 对端拒绝匹配，
  表现为"收到 Publication、qos_rej=0、持续回 Subscription，但零数据"。
  **修复**：按对端 writer 的 kind 选择我方 reader entityId（`PairReaderEntity` 与
  订阅公告同步；unkeyed 时仍是 `0x04` → 零回归）。
  **配套修复**：① `UserData.instance_key` + reader 解析 `PID_KEY_HASH`；
  ② `PID_DATA_REPRESENTATION(0x0073)` 此前从未序列化（元素宽度是 **2 字节**的 int16）；
  ③ 声明与 `requested_qos` 对齐。
  **验证**：上游 keyed 示例从"0 行发送"变为 **20 行 SENT**；Stratum `samples=24`
  （20 数据 + 4 dispose）、**key_len=16**、**4 种不同 key**（对应 4 个颜色实例）。
  脚本：`scenarios/12_keyed_topic.sh`。
  **回归**：`ctest` 33/33；对账 200/200、119/119；场景 05/07 PASS；QoS 矩阵 6/6。
- [ ] **A5 ·（视 A1 结论）DATA_FRAG 分片支持**
  若要真正支持 >64 KiB 样本，需要实现 `DATA_FRAG` 子消息与重组。
  属较大工作量；A1 先保证"显式失败"，A5 作为增强项。

## B. 测试补齐（用户特别强调"全方位"）

- [x] **B1 · 压测：吞吐 + 完整率（回放与录制双向）** ✅ 2026-10-08（续 26）
  **证据**：`scenarios/10_bench.sh`（速率阶梯 5 档 × 500 条）
  | rate | 实测发送窗口 | 收到 | 完整率 |
  |---|---|---|---|
  | 1.0（50 条/秒） | 50 条/秒 | 500 | 100.0% |
  | 100（5000 条/秒） | 5004 条/秒 | 499 | 99.8% |
  | 500（2.5 万条/秒） | 24648 条/秒 | 500 | 100.0% |
  | 2000 | 79323 条/秒 | 500 | 100.0% |
  | 0（尽快） | **80059 条/秒** | 495 | 99.0% |
  录制闭环（Stratum→Stratum）：发送 500 → 录到 **501**（100.2%，多出的是首条补救副本）。
  **重要纠正**：上一轮报的「420 条/秒」是**测量错误**（shell 计时把进程启动与
  收尾 ACK 等待算进了发送耗时）；改为 Player 内部"发送窗口"统计后为 **≈8 万条/秒**。
  **语义结论**：回放方向**测不出传输延迟**（INFO_TS 是源时间戳，回放是时间旅行），
  详见 `scenarios/10_bench.sh` 顶部注释。
- [x] **B1b · 到达抖动 + 录制侧实时延迟** ✅ 2026-10-08（续 34）
  **工具**：`rtps_reader_diag` 输出 `READER_LATENCY_MS` / `READER_JITTER_MS`；
  `fastdds_sub_diag` 新增 `DIAG_JITTER_MS`。脚本 `scenarios/15_latency_jitter.sh`。
  **录制侧实时延迟**（实时发布 150 条 @100/秒）：
  **P50 = 0.136 ms、P95 = 0.225 ms、P99 = 0.363 ms、max = 0.429 ms**（本机，亚毫秒）。
  **回放方向到达抖动**（200 条，样本间隔 20 ms）：
  原速 `rate=1.0` → P50 **20.000 ms**；尽快 `rate=0` → P50 **0.005 ms** —— 与期望逐一吻合，
  证明**速率控制精确**。（回放方向的 now-source 不是延迟，理由见续 26。）
  **顺带修 bug**：`printf` 参数求值顺序未定义 → "max < p99"；改为先排序再取分位与 max。

- [x] **B2 · 随机测试扩展** ✅ 2026-10-08（续 35）
  `scenarios/14_random_suite.sh`（10 轮随机：payload 跨 2 KiB / 64 KiB / 超限，条数、速率、话题数）。
  **可靠模式**（`mode=reliable`，原速 rate=1.0 —— 真实使用场景）：**10/10 全绿**，
  含 8000 B×120、60000 B×37，以及带第二话题的档位。
  **full 模式**（含"尽快 / 数千条每秒"）：PASS=6、丢包=2~3 —— 属**已知限制 L1**
  （见 `design/15-test-report.md` §3），与基本功能无关。两档分开陈述，
  避免把"高速突发限制"误读成"基本功能不可用"。

- [x] **B3 · soak 长时稳定性** ✅ 2026-10-08（续 32）
  录制器长跑 32 分钟，每 30 秒触发一次 100 条回放。
  **完整 55 轮（退出码 0）**：**RSS 恒定 2048 KB、FD 恒定 4**（零增长）——
  长时运行**无内存/句柄泄漏**。
  注：脚本里的"录制条数"判据**无效**（每轮回放进程的 SN 都从 1 重新开始，
  被录制端按 (writer,SN) 去重正确丢弃）；有效判据只有 RSS / FD。

- [x] **B4 · 跨主机可行性** ✅ 2026-10-08（续 33）
  **发现真缺陷**：对外声明的 locator **全部硬编码 `127.0.0.1`**（SEDP 公告、SPDP
  metatraffic/user、Publication，共 5 处）—— 同机"恰好"能通，**跨主机时对端会把数据
  发到它自己的回环**。
  **修复**：新增 `DetectLocalAddress()`（UDP connect + getsockname 按路由表选出口地址），
  5 处改用探测/配置地址；新增 `advertised_address` 配置。本机探测结果 `172.17.211.143`。
  **回归零退化**：`ctest` 33/33；对账 200/200、118/118；场景 05/07 PASS；QoS 矩阵 6/6。
  **跨主机配置要求（未实测，无第二台主机）**：① 组播 `239.255.0.1:7400+d*250` 需网段可达，
  否则对端配 `initialPeersList`；② 放行 UDP `7400+d*250`、`9060+d*10`、`9061+d*10`、
  对端 `7410+d*250+2*pid` / `7411+d*250+2*pid`；③ 多网卡时显式指定 `advertised_address`。

- [x] **B5 · 回放流控（发送窗口）** 实现完成、**默认关闭**（续 32 / 定稿续 35）
  **实现**：`RtpsWriter::WaitForSendWindow`（未确认条数 + 字节双限 + 超时兜底 + 统计），
  在 `Publish` 入口调用；配置 `send_window`（默认 **0 = 关闭**）、`send_window_bytes`、
  `send_window_timeout_ms` 保留，需要时显式开启。
  **实测教训**：只限条数时 8000 B 仍丢 43%/35%；加字节限后**更差**（PASS 4）——
  根因是**重传路径（HandleAcknack 补发、收尾重发）绕过窗口**；
  且开启后**损伤正常回放**（对账 A 200/200 → 184、首条补救被拖后）。
  **结论**：高速突发丢包（L1）未根治，已作为已知限制写入 `design/15-test-report.md`；
  **按录制速率回放完整**（B2 reliable 模式 10/10）。

## C. 验收与文档

- [x] **C1 · 每个维度一个可复现场景脚本** ✅ 2026-10-08（续 35）
  现有 **17 个**脚本（`scenarios/01`~`16` 含变体），覆盖：核心回放、端口对照、SEDP 抓取、
  原始抓包、协议统计、上游对照、压测、条数对账、大 payload、QoS 矩阵、keyed、
  soak、随机套件、延迟抖动、**多话题隔离（本轮补）**。
- [x] **C2 · 测试报告** ✅ 2026-10-08（续 35）
  独立报告 **`design/15-test-report.md`**：功能矩阵（A1–A5）、性能量化
  （吞吐 8 万条/秒、延迟 P50 0.136 ms / P99 0.363 ms、抖动与期望逐一吻合）、
  条数对账、soak 55 轮无泄漏、随机套件、**已知限制 4 项**、回归基线、复现方式与验收口径。
- [x] **C3 · `design/12` 遗留项与 `AI_COLLAB.md` 每轮同步** ✅ 2026-10-08（续 35）
  每轮均更新（见 `AI_COLLAB.md` 续 24~35），`design/12` 遗留项已随修复逐条收敛。

---

## 推进规则（每轮照此执行）

1. 先跑基线（`ctest` + 对账 A/B）确认无退化；
2. 从 A→B→C 顺序取**一项**推进（可拆成多轮，但每轮必须有可验证产出）；
3. 新增/修改的路径必须有断言或量化输出，**禁止只靠"看起来正常"**；
4. 本轮结束时：更新本文件勾选、追加 `AI_COLLAB.md`、必要时同步 `design/12`；
5. 遗留/被推翻的假设都要写下来（本项目多次靠"诚实记录失败尝试"避免了重复踩坑）。
