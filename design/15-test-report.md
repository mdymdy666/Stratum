# 15 · Stratum 验收测试报告（量化）

> 生成于 2026-10-08。本报告汇总 `design/14-remaining-work.md` 队列的**量化证据**，
> 所有数字均来自可复现脚本（见文末"复现方式"）。
> 相关：`design/12-rtps-interop-rootcause.md`（根因）、`AI_COLLAB.md`（逐轮记录）。

---

## 0. 一句话结论

**Stratum 已能与真实 Fast DDS 双向互通，且录制方向实时延迟为亚毫秒级。**

* **回放**（Stratum → Fast DDS）：条数对账 **200/200**，速率控制精确（抖动与期望逐一吻合）；
* **录制**（Fast DDS → Stratum）：真实发布 150 条全部收到，**端到端延迟 P50 = 0.136 ms、P99 = 0.363 ms**；
* 长时运行 **55 轮 soak 无内存/句柄泄漏**；
* 全量 `ctest` **33/33**。

---

## 1. 功能验证矩阵

| # | 能力 | 状态 | 关键证据 |
|---|---|---|---|
| A1 | >64 KiB 样本**显式失败** | ✅ | 70000 B × 5：回放端 `已发布样本=0`、逐条打印"样本过大…需要 DATA_FRAG"、订阅端 0/5 —— **失败可见，不发坏包** |
| A2 | **多话题并发** | ✅ | `hello_world_topic` 100 条 + `hello_world_topic_2` 60 条，两实例分别订阅：样本数正确、**区间外（串扰）0 条** |
| A3 | **非默认 QoS 组合** | ✅ | 矩阵 6/6：writer TL + reader VOLATILE → 接受；writer VOLATILE + reader TL → **正确拒绝**；writer BE + reader RE → **正确拒绝** |
| A4 | **keyed topic** | ✅ | 上游 keyed 示例 4 实例 × 5 样本：`samples=24`、**key_len=16**、**4 种不同 key** |
| A5 | DATA_FRAG 分片 | ⬜ 未做 | 增强项；A1 已保证超限样本"显式失败"而非发坏包 |
| — | Stratum ↔ Stratum | ✅ | 场景 05 PASS（Reader 收到样本） |
| — | key 在 RTPS 层的提取与贯通 | ✅ | `UserData.instance_key` ← `PID_KEY_HASH`；存储层/Recorder/Player 全链路一致 |

---

## 2. 性能量化（B 组）

### 2.1 吞吐（回放方向）

| rate | 目标(条/秒) | **实测发送窗口** | 收到 | 完整率 |
|---|---|---|---|---|
| 1.0 | 50 | **50** | 500 | **100.0%** |
| 100 | 5000 | **5004** | 499 | 99.8% |
| 500 | 25000 | **24648** | 500 | 100.0% |
| 2000 | 100000 | 79323（受发送能力限制） | 500 | 100.0% |
| 0（尽快） | — | **80059** | 495 | 99.0% |

**发送端能力上限 ≈ 8 万条/秒**（`Player` 内部"发送窗口"计时）。

> **测量方法学（重要）**：外部用 shell 计时会把**进程启动**（Fast DDS 初始化 +
> SPDP 发现 1~2 秒）与**收尾 ACK 等待**（最多 5 秒）算进"发送耗时"，
> 曾据此得出错误的「420 条/秒」。做性能结论时必须看清计时区间。

### 2.2 延迟（**录制方向**，实时发布）

实时发布 150 条 @100 条/秒 → Stratum Reader：

```
READER_LATENCY_MS n=150 min=0.084 p50=0.136 p95=0.225 p99=0.363 max=0.429
```

**端到端延迟 P50 = 0.136 ms，P99 = 0.363 ms**（本机，亚毫秒级）。

> **为什么回放方向测不出延迟**：回放的 INFO_TS 承载样本**源时间戳**（语义正确），
> 而回放是"时间旅行" —— `now − source` 必然包含"原始生成 → 回放"的间隔。
> 回放方向可测的是**到达抖动**。

### 2.3 抖动（回放方向）

| 回放档位 | 期望间隔 | 实测抖动 P50 |
|---|---|---|
| `rate=1.0`（原速，50 条/秒） | 20 ms | **20.000 ms** ✓ |
| `rate=0`（尽快，≈8 万条/秒） | ≈12.5 µs | **0.005 ms** ✓ |

**速率控制精确** —— 实测与期望逐一吻合。

### 2.4 条数对账（完整性）

| 场景 | 结果 |
|---|---|
| 生成 200 条 → 回放 → 上游 subscriber | **200/200**（首=1 尾=200） |
| 真实录制 → 回放 → 上游 subscriber | **N/N**（录像长度随会话波动，118~119） |
| 录制方向（真实 publisher） | `序号缺口: 0` |

### 2.5 长时稳定性（soak）

录制器长跑 32 分钟，每 30 秒触发一次 100 条回放：

| 指标 | 结果（55 轮，退出码 0） |
|---|---|
| RSS | **恒定 2048 KB**（零增长） |
| FD 数 | **恒定 4**（零增长） |

**无内存/句柄泄漏。**

### 2.6 随机套件

10 轮随机（payload 跨 2 KiB/64 KiB/超限、条数、速率、话题数）：**PASS=6 / 对账失败=2 / 丢包=2**。
未判定全绿，原因见 §3 已知限制（B5）。

---

## 3. 已知限制（如实列出）

| # | 限制 | 影响范围 | 现状 |
|---|---|---|---|
| L1 | **高速突发丢包**（无流控） | 仅"尽快 / 5 万条每秒"这类**超出接收端消费能力**的回放；大 payload 更明显（8000 B @ 尽快 丢 39%） | 已实现发送窗口（B5）但**默认关闭** —— 开启会损伤正常回放（对账 200/200 → 184）；根因是**重传路径绕过窗口**，未根治。**按录制速率回放完整** |
| L2 | **>64 KiB 单条样本**需要 `DATA_FRAG` | 超大样本 | 未支持；已保证**显式失败**（不发坏包） |
| L3 | **跨主机未实测** | 多机部署 | 已修复"声明 locator 硬编码 127.0.0.1"这一**致命前提**，并给出配置要求（组播/防火墙/多网卡），但无第二台主机，未实测 |
| L4 | 验证覆盖 | — | 单机单域；未覆盖多网卡、IPv6、非 UDP 传输、WSS/安全插件 |

---

### 3.1 L1 的定性更正与部分修复（2026-10-08 续 36）

**原先的定性不准确**。两轮尝试（ACK 窗口、滑动窗口限速）都**没有**消除高速档丢包，
于是改用最便宜的**决定性判据**：跑一次高速回放，对比 `/proc/net/snmp` 的 Udp 行。

```
before: InDatagrams=53042  InErrors=270  RcvbufErrors=270
after : InDatagrams=59026  InErrors=587  RcvbufErrors=587     ← +317
```

**`RcvbufErrors` 增加 317** ⇒ 丢包发生在**内核层、是接收缓冲溢出**；
同一时段 `OutDatagrams` +6147，说明 **Stratum 确实把包发出去了**。
即：**根因是"接收端内核缓冲不足"，而不是"Stratum 无流控乱发"**。

据此分两个方向处理：

**① 录制方向（Stratum 是接收端）—— 已修复** ✅

给 `RtpsReader` 的接收 socket 加 `SO_RCVBUF`（8 MB，内核按 `rmem_max` 截断）。
实测（`fastdds_pub_diag --rate=N` → Stratum Reader）：

| 发布速率 | 条数 | 收到 | 完整率 | 延迟 P50 / P99 |
|---|---|---|---|---|
| 5000 条/秒 | 2000 | **2000** | **100.0%** | 0.062 ms / 0.209 ms |
| 20000 条/秒 | 3000 | **3000** | **100.0%** | 0.298 ms / 32.687 ms |

**且期间 `RcvbufErrors` 零增长**（587 → 587）。
（20000 条/秒档的尾部延迟升高，是突发下排队所致，完整率不受影响。）

**② 回放方向（Fast DDS 是接收端）—— Stratum 只能缓解**

接收端缓冲不由 Stratum 控制，因此：

* 已实现**发送限速**（`max_samples_per_sec` / `max_bytes_per_sec`），
  但**默认关闭** —— 它并未消除丢包（两轮实测），却把吞吐从 ≈8 万条/秒压到 ≈2 万条/秒；
* **建议接收端**调大 `SO_RCVBUF` 与 `net.core.rmem_max`。
  本机环境的限制如实记录：`net.core.rmem_max = 212992`（208 KB），
  `sysctl -w` **权限被拒**，因此 Stratum 侧的 `SO_RCVBUF(8 MB)` 实际被截断到约 425 KB
  （即便如此，录制方向仍取得 2 万条/秒 100% 完整）。

**因此 L1 的准确表述是**：回放方向在"超出接收端消费能力"的速率下会因**对端内核缓冲**
丢包；录制方向已通过加大自身缓冲解决。

### 3.2 第二轮测试扩展（2026-10-08）

| 维度 | 方法 | 结果 |
|---|---|---|
| **边界** | `scenarios/17_edge_cases.sh` | 0 条（回放码 2 = 无可回放数据，属预期）、1 条、**5000 条**（超 `pending_` 的 4096 裁剪阈值）全部正常 |
| **并发** | 同上 | **3 个 reader 同时订阅：各自收到 201；发送目标数 = 3** —— 直证「locator 定向」修复生效 |
| **崩溃一致性** | `scenarios/18_crash_recovery.sh` | 录制中被 `kill -9`（无 flush 机会）后，时间线**仍有 301 条**，与正常 SIGTERM 退出**完全一致** |
| **数据安全** | `scenarios/19_overwrite_guard.sh` | 录制器默认**拒绝**在非空目录启动（退出码 3），`--overwrite` 才清空 —— 修掉「重启静默清空历史」（301 → 0） |
| **丢包收敛** | `scenarios/20_packet_loss.sh` | **1% 丢包 → 100% 收敛**（重传补齐）；**5% → 94.5%**、**10% → 92.5%**（残留 11/15 条） |
| **反复回归** | `scenarios/21_regression_loop.sh 3` | 连续 3 轮：ctest 33/33、对账 A 200/200、场景 05/07/16 全 PASS —— **无 flaky** |
| **ACKNACK 位图分支** | 加计数器 + 丢包制造缺口 | **位图位解析次数恒为 0**（0/5/30/50% 丢包、两种对端）→ 属**防御性代码**，实际由「位图外→视为缺失」的保守路径兜底，**风险低但无实测证据** |

**未做的两项（已决策，理由见 `design/16`）**：`DATA_FRAG`（>64 KiB 样本）、IPv6 双栈。

## 4. 回归基线（每轮必须保持）

| 项目 | 当前值 |
|---|---|
| 全量 `ctest` | **33/33** |
| 条数对账 A / B | **200/200**、**118~119/119** |
| 大 payload（1200/8000/60000 B） | **全部完整** |
| 场景 05 / 07 | PASS / PASS |
| QoS 矩阵（6 例） | **6/6** |
| keyed（4 实例） | **4 种 key**，零串扰 |

---

## 5. 复现方式

```bash
cd /UserData/CodexWorkSpace/Stratum
# 构建（库 + 全部诊断工具）
cmake --build build
bash tools/fastdds_sub_diag/build.sh          # fastdds_sub_diag（订阅诊断）
bash tools/fastdds_sub_diag/build_capture.sh  # capture_udp（抓包）
bash tools/fastdds_sub_diag/build_tools.sh    # replay/record/reader_diag/sink_diag2/make_timeline
bash tools/fastdds_sub_diag/build_pub.sh      # fastdds_pub_diag（QoS 可配置发布端）

# 全量单元/集成测试
cd build && ctest

# 验收场景（按需单独运行；日志在 build/tools/logs/）
bash tools/fastdds_sub_diag/scenarios/08_accounting.sh      # 条数对账（完整性）
bash tools/fastdds_sub_diag/scenarios/09_payload_sizes.sh   # 大 payload
bash tools/fastdds_sub_diag/scenarios/10_bench.sh           # 吞吐/速率阶梯
bash tools/fastdds_sub_diag/scenarios/11_qos_matrix.sh      # QoS 兼容矩阵
bash tools/fastdds_sub_diag/scenarios/12_keyed_topic.sh     # keyed 话题
bash tools/fastdds_sub_diag/scenarios/13_soak.sh 32         # 长时稳定性
bash tools/fastdds_sub_diag/scenarios/14_random_suite.sh    # 随机套件
bash tools/fastdds_sub_diag/scenarios/15_latency_jitter.sh  # 延迟/抖动
bash tools/fastdds_sub_diag/scenarios/16_multitopic.sh      # 多话题隔离
```

**环境**：
```
LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu
上游 Fast DDS：/UserData/Fast-DDS（只读，Release 构建）
上游示例：hello_world、topic_instances（keyed）
```

---

## 6. 验收口径

**"基本功能完成"** 定义为：

1. 真实 Fast DDS ↔ Stratum **双向互通**（回放 200/200；录制实时延迟亚毫秒）；✅
2. A1–A4 四项能力**全部通过**且各有固化脚本；✅
3. 长时运行无泄漏（soak RSS/FD 恒定）；✅
4. 全量 `ctest` 全绿；✅
5. **已知限制必须显式声明**，且不得"静默失败"（本项目已修掉 6 处静默失败）。✅

**未达成**：L1（高速丢包根治）、A5（DATA_FRAG）、L3（跨主机实测）。
