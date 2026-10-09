# 03 · 验证矩阵（问题 ↔ 脚本 ↔ 判据）

> 每个已知问题都要有**一个能复现它的脚本**。本文档建立对照索引。
> 脚本位于 `tools/fastdds_sub_diag/scenarios/`，日志输出在 `build/tools/logs/`。

---

## 一、场景脚本总览（19 个）

| 脚本 | 用途 | 关键判据 |
|---|---|---|
| `01_stratum_replay.sh` | 核心回放链路 | `[diag][MATCH]` + `samples>0` |
| `01b_port_compare.sh` | 端口对照（诊断 vs 上游） | 端口号差异定位 |
| `01c_sedp_recv.sh` | SEDP 接收侧检查 | `sedp_pub>0` |
| `01d_capture.sh` | 原始抓包核对 | 逐子消息统计 |
| `02_direct_sink.sh` | 直接驱动 `RtpsSink` | `sedp_sub>0`、`retrans` 稳定 |
| `03_fastdds_publisher.sh` | **Fast DDS → Stratum**（录制方向） | `samples>0`、`key_len` |
| `04_stress_random.sh` | 早期随机 + 压测 | 无崩溃、RSS 有界 |
| `05_stratum_to_stratum.sh` | Stratum ↔ Stratum 回归 | Reader 收到样本 |
| `06_spdp_selfcheck.sh` | SPDP 自检 | 公告端口正确 |
| `07_upstream_subscriber.sh` | **回放 → 上游 subscriber**（端到端验收） | 收到条数 == 发送条数 |
| `08_accounting.sh` | **条数对账**（A: 生成时间线 / B: 真实录制） | A **200/200**、B **N/N** |
| `09_payload_sizes.sh` | 大 payload（1200/8000/60000 B） | 全部完整、日志行长度正确 |
| `10_bench.sh` | **吞吐 + 速率-完整率阶梯** | 发送窗口 ≈8 万条/秒 |
| `11_qos_matrix.sh` | **QoS 兼容性矩阵**（6 例） | 接受/拒绝均符合规则 |
| `12_keyed_topic.sh` | **keyed 话题** | `key_len=16`、**4 种不同 key** |
| `13_soak.sh` | **长时稳定性** | RSS/FD **恒定**（无泄漏） |
| `14_random_suite.sh` | **随机套件**（`reliable` / `full` 两模式） | reliable **10/10**；full 暴露环境限制 |
| `15_latency_jitter.sh` | **延迟 + 抖动** | 延迟 P50 亚毫秒；抖动与期望吻合 |
| `16_multitopic.sh` | **多话题隔离** | 两话题完整、**区间外 0 条** |
| `17_edge_cases.sh` | **边界与并发**（0/1/5000 条、3 reader 并发） | 全 PASS；发送目标数 = 3 |
| `18_crash_recovery.sh` | **进程中断恢复**（`kill -9`） | 时间线数据完好（301 条）；重启清空问题已由 `19` 守护 |
| `19_overwrite_guard.sh` | **录制器覆盖保护**（O19） | 非空目录必须拒绝；`--overwrite` 才清空 |
| `20_packet_loss.sh` | **丢包收敛**（E1） | 1% 完全收敛；5%/10% 残留 |
| `21_regression_loop.sh` | **反复回归**（E4） | 连续 N 轮 ctest + 关键场景无 flaky |

### 辅助工具

| 工具 | 用途 |
|---|---|
| `fastdds_sub_diag` | Fast DDS 订阅诊断（QoS/发现/样本/延迟/抖动） |
| `fastdds_pub_diag` | **可指定 QoS 的** Fast DDS 发布端（A3 的前提） |
| `capture_udp` | 原始 UDP 抓包（不做语义解析） |
| `make_timeline` | 生成"内容已知"的时间线（可指定 payload 大小/话题数） |
| `rtps_reader_diag` | Stratum `RtpsReader` 逐秒指标 + 延迟/抖动 |
| `rtps_sink_diag2` | Stratum `RtpsSink` 逐秒指标 |
| `stratum_rtps_replay` / `stratum_rtps_record` | 回放 / 录制 |

---

## 二、问题 → 脚本的对照

### 已解决的问题（见 `01-resolved-issues.md`）

| 问题 | 验证脚本 | 具体判据 |
|---|---|---|
| 1.1 Publication 静默不发 | `01`、`07` | 对端能看到 Writer 且收到数据 |
| 1.2/1.3 发送与 `addData` 失败被忽略 | `09`、`14` | 70000 B 档必须"显式失败" |
| 1.4 `Player` 忽略返回值 | `14` | `已发布样本=0` + 明确错误行 |
| 1.5/1.6 延迟统计 | `15` | 有 `DIAG_LATENCY_MS` 且 `max ≥ p99` |
| 2.1 共用 SN 计数器 | `02` | 心跳 `first=1 last=N` == 实际条数 |
| 2.2 重复 `BuildDataPacket` | `09` | 8000/60000 B 完整 |
| 2.3 多话题共用 entityId | `16` | 区间外 0 条 |
| 2.4 locator 写给所有 peer | `02` | `[send]` 各自独立目标 |
| 3.1 domain 被丢弃 | `08`、`tests/review/test_backend_config_propagation.cc` | 端口对照 + 单测 |
| 3.2 QoS 声明不一致 | `11` | 6/6 符合规则 |
| 3.3 locator 硬编码回环 | 全量回归（`design/15` §3） | 零退化 |
| 4.1 不处理 HEARTBEAT | `01c`、`02` | `sedp_sub>0` |
| 4.2 writerSN 偏移 | `08`（`序号缺口`）、字节 dump | 序号不再恒 0 |
| 4.3 reader entityId 被截断 | `08`（B 段录制方向） | publisher 持续发送 |
| 4.4 entityId 的 topic_kind | `12` | 4 种 key |
| 4.5 `PID_DATA_REPRESENTATION` | `12` | 对端匹配成功 |
| 4.6 录制端不去重 | `13`、`10` 录制段 | 录到条数 ≈ 发送条数 |
| 5.1 固定历史时间基准 | `15` | 延迟回到毫秒级 |
| 5.3 首条 VOLATILE 边界 | `08` | `首=1`（不再是 2） |
| 6.1 诊断程序 `samples=0` | `03`（`--participant-mask`） | `samples>0` |
| 6.2 吞吐测量错误 | `10` | 用 `Player` 发送窗口统计 |
| 6.3 高速丢包归因 | `/proc/net/snmp` 对比（见 `design/15` §3.1） | `RcvbufErrors` 增量 |

### 遗留问题与脚本的对应（2026-10-08 续 43 复核后）

**结论：矩阵里不再有"含糊的未覆盖项"。** 7 项原来的"无脚本"里，
3 项其实早已被覆盖、3 项已决策为"不做/环境不可行"，**真正仍需专项脚本的只剩 1 项**。

| 问题 | 状态 | 说明 |
|---|---|---|
| **O1 跨主机** | ⛔ **环境不可行（已决策）** | 实测 `ip netns` / `veth` / `tc netem` **全部不可用**（WSL 权限）；同机因 socket 绑 `INADDR_ANY`，**所有本地地址都可达**，"对端发往哪个地址"无法区分。**需第二台主机**才能验证。不是"缺脚本"，是**环境不允许** |
| **O2 高速丢包** | ✅ **已覆盖** | `14_random_suite.sh`（full 模式）+ `/proc/net/snmp` 的 `RcvbufErrors` 对比判据（`design/15` §3.1） |
| **O4 >64 KiB** | ✅ **已决策：暂不实现** | 现状是**显式失败**（不发坏包），已由 `14` 的 70000 B 档守护 |
| **O5 IPv6** | ⛔ **已决策：暂不实现** | 双栈整体缺失（`AF_INET6` 零出现），改造清单 6 项见 `design/16` D3 |
| **O6 多网卡** | ⛔ **已决策：代码就绪、语义单机不可验证** | 自动探测 + `advertised_address` 已就绪；与 O1 同因（`INADDR_ANY`），合并跟踪 |
| **O10 ACKNACK 位图路径** | ✅ **已查清：实测从未触发（防御性代码）** | 已加计数器并在两种对端、0/5/30/50% 丢包下实测：**位图位解析次数恒为 0**；实际起作用的是「位图外 → 视为缺失」这条**保守路径**（不漏重传，风险低）。位图解析**仅有规范依据、无实测证据**；要覆盖需单元测试直接构造带位图的 ACKNACK（需暴露 `HandleAcknack`）。按「适当即可」未做，如实记录 |
| **O11 `pending_` 裁剪** | ✅ **已覆盖** | `17_edge_cases.sh` 的 **5000 条用例**已超过 4096 的裁剪阈值，验证了长回放完整性 |
| **O19 录制器清空历史** | ✅ **已有脚本** | `19_overwrite_guard.sh`（3 用例，含"非空目录必须拒绝"） |
| **O20 ≥5% 丢包未完全收敛** | ✅ **已有脚本** | `20_packet_loss.sh`（0/1/5/10% 四档，量化收敛边界） |

## 三、跑全部验收（一条命令）

```bash
cd /UserData/CodexWorkSpace/Stratum
cmake --build build
bash tools/fastdds_sub_diag/build.sh
bash tools/fastdds_sub_diag/build_capture.sh
bash tools/fastdds_sub_diag/build_tools.sh
bash tools/fastdds_sub_diag/build_pub.sh

cd build && ctest                      # 全量单测/集成测试（33/33）

cd .. && for s in 08_accounting 09_payload_sizes 11_qos_matrix 12_keyed_topic \
                 15_latency_jitter 16_multitopic; do
    echo "=== $s ==="; bash tools/fastdds_sub_diag/scenarios/$s.sh 2>&1 | tail -5
done
```

**注意**：`10_bench.sh`、`13_soak.sh`、`14_random_suite.sh` 耗时较长（分钟级到半小时级），
按需单独运行。所有日志在 `build/tools/logs/`（**不要用 `/tmp`**：本会话被系统清理过三次）。
