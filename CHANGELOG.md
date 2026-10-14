# Changelog

本项目所有值得记录的变更。格式参考 [Keep a Changelog](https://keepachangelog.com/)，
版本号遵循 [语义化版本](https://semver.org/)。

---

## [1.0.0] — 2026-10-13

**首个稳定版本。** 面向 DDS 的可回溯数据平面：自带 RTPS 协议栈，
录制/回放/查询打通，并通过跨主机与工业级压力测试。

### 新增 · 协议与互通

- **自带 RTPS 协议栈**：SPDP / SEDP / DATA / HEARTBEAT / ACKNACK / DATA_FRAG
  的报文构造与解析，与 Fast DDS 双向互通。
- **`DATA_FRAG` 分片**（发送侧）：超过路径 MTU 的样本自动切片，
  每片不超过 MTU，因此可穿过 NAT / UDP 中继这类会丢弃 IP 分片的路径。
- **`DATA_FRAG` 重组**（接收侧）：按 `(guidPrefix, writerId, SN)` 缓存分片，
  收齐后走与 DATA 相同的交付路径；30 秒未收齐即淘汰，避免内存无界增长。
- **路径 MTU 自动探测**：`getsockopt(IP_MTU)`，**按对端探测**
  （有 `initial_peers` 用第一个对端，否则探测 `127.0.0.1` 取 loopback）。
  单机因此不会被强行分片，跨主机自动切片。
- **退避重传**：指数退避 50→100→200→400→800→1000ms（上限），最多 6 次，超限标记放弃；
  由心跳周期驱动。分片样本重传时按原始 payload **重新切片**（不缓存超大报文）。
- **分片去重**：重传的分片会再次收齐，用 `RecordReceivedSn` 去重，避免重复交付。
- **QoS 兼容判定**：reliability / durability / ownership / deadline / liveliness /
  数据表示（`PID_DATA_REPRESENTATION`）。

### 新增 · 录制与回放

- 按时间轴回放，保真源时间戳间隔；支持变速、跳转、循环。
- **序号缺口检测**：滑动窗口去重 + ACKNACK 依据**真实收到的 SN 集合**报告缺口。
- 录制保护：拒绝写入非空目录（退出码 3），需显式 `--overwrite`。
- 双时间戳：源时间戳（发布者）与接收时间戳（录制器）分别保留。

### 新增 · 跨主机

- NAT + UDP 中继拓扑下双向打通（WSL ↔ 独立虚拟机）。
- `tools/udp_relay.py`：Windows 侧双向 UDP 中继（单 RX socket 绑定 + 独立未绑定 TX socket）。
- `tools/start_cross_host.py`：跨主机环境一键准备（启动虚拟机 / 取 WSL IP / 算端口启中继 / 健康检查）。

### 新增 · 诊断与测试

- `tools/fastdds_sub_diag/`：Fast DDS 订阅诊断程序（`Log::SetVerbosity(Info)`）。
- **28 个真实场景脚本** + **34 个 ctest**。
- `scenarios/26_frag_stress.sh`：DATA_FRAG 接收侧健壮性（多 writer 交错 / 缓存上限 /
  畸形分片 / 部分分片不误交付）。
- `tools/frag_fuzz.cc`：分片注入工具（`partial` / `overflow` / `malformed`）。
- `tests/unit/test_ack_bitmap.cc`：ACKNACK 位图判定三条路径 + 大端位序单测。

### 修复 · 关键缺陷

- **`SendAcknackForReceived` 的 base 语义错误**：原先每收到一个样本就传
  `base = sn+1, hi = sn`，等价于声称"该 SN 之前全部收到"。只要有一个靠后的样本到达，
  对端就会把所有更早的 pending 标记为已确认，**重传永不触发**。
  改为把 `[1, sn]` 交给 `SendAcknack`，由它用收到的 SN 集合计算 base 与位图。
  修复后单包样本收敛率：30% 丢包下 53% → 80%，50% 丢包下 50% → 66%。
- **SEDP 公告未使用 `advertised_address`**：`BuildSedpAnnouncementForReader` 未设置
  `unicast_address`，构造时回退到 `DetectLocalAddress()` 的真实地址。在 NAT + 中继拓扑下
  该地址对端不可达，导致"发现成功、SEDP 正常、Writer 已匹配，但一条数据都收不到"。
- **自动 MTU 探测被覆盖**：探测结果写在 `config_ = config;` 之前，赋值时被覆盖回 0，
  分片分支永不生效。
- **MTU 探测目标选错**：早期对固定外部地址探测，得到默认路由 MTU，
  导致单机 loopback 也被强行分片（大 payload 从 21/20 掉到 5/20）。
- **中继端口公式错误**：`metatraffic`/`user` 应为 `9050/9051 + domain*10`，
  原用 `9060/9061 + domain*10`，差 10 个端口，导致 SEDP Publication 全落到无人监听端口。
- **`SO_RCVBUF` 被静默截断**：`net.core.rmem_max` 默认仅 208KB，请求 8MB 会被截断。
  改为优先 `SO_RCVBUFFORCE`，失败回退并**打印实际生效值**。

### 新增 · 示例与测试全面覆盖（10 月 14 日）

**C++ 示例（`examples/cpp/`）** —— 5 个模块各两个文件，共 **10 个**：

| 模块 | demo | full | full 演示的 3 个真实场景 |
|---|---|---|---|
| core | 196 行 | 468 行 | 产线录制 / 故障复盘 / 回归回放 |
| protocol | 291 行 | 917 行 | 非 DDS 数据源 / 自定义去向 / 能力协商 |
| storage | 246 行 | 580 行 | 100 万键时间轴 / 双引擎契约 / 元数据版本 |
| types | 348 行 | 623 行 | 双时间戳 / 质量报告 / 序列化器降级 |
| rtps | 264 行 | 506 行 | 字节级报文 / 发送侧配置 / 端到端 |

全部编译通过并实测运行，CMake 入口 **10/10** 目标构建成功，且**连跑两次幂等**。

**测试（`tests/`）** —— ctest 从 34 增至 **37**，5 个模块均覆盖三类：

* **大数据量**：100 万条样本回调交付（76 ms）、100 万时间键（顺序不变量）、
  10 万条报文构造（23.6 ms）、10 万个 `DynamicValue` 元素
* **随机**：4 个种子 × 随机规模/形状/字段，且断言均为**确定性不变量**
  （不用"依赖概率"的 flaky 断言）
* **真实场景**：共 15 个（每模块 3 个），贴近实际用法

新增测试文件：`tests/types/test_types_serializer.cc`（763 行）、
`tests/protocol/test_protocol_backend.cc`（976 行）、
`tests/rtps/test_rtps_backend_usage.cc`（563 行）。

**实测吞吐**（单机 loopback，`rate=0` 尽可能快，取"仅发送循环"耗时）：

| payload | 发送速率 | 折算吞吐 | 订阅端收到 |
|---|---|---|---|
| 64 B | 88 538 条/s | 5.4 MB/s | 100% |
| 1 KB | 78 527 条/s | 76.8 MB/s | 100% |
| 8 KB | 40 785 条/s | 319 MB/s | 98.9% |
| 60 KB | 13 699 条/s | 783 MB/s | 95.5% |

**已知实现缺口**：`types::DynamicValue` 的 `MakeXxx` 工厂与 `ToString`
已在头文件声明但**尚未实现**（调用会链接失败）；`tests/types/` 中有
`KnownGap_*` 用例显式记录此事。

### 已知限制

见 [`docs/02-open-issues.md`](docs/02-open-issues.md) 与 README 的"已知限制"一节。
要点：WSL 内无 `sysctl` 写权限；高丢包（50%）下收敛约 66%；
DATA_FRAG 尚未接入持续 fuzzing；仅支持 IPv4。

---

## [0.1.0] — 规范阶段

- 完成 8 份需求规范（`00-overview.md` … `07-deployment.md`）：
  功能需求 33 条、核心场景 12 个、边缘场景 23 个。
- 确定三个核心判断：DDS 只有"现在"没有"过去"；数据可信度是一等公民；
  真正让系统失效的是运维问题而非功能缺失。
