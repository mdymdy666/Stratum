# Stratum

**面向 DDS 的可回溯数据平面** —— 把 DDS 域中流动的实时数据持续落盘为不可变的"地层"，
并提供按时间轴的精确回放、查询与状态重建。

> 对标物：**rosbag2**。差异在于 Stratum 自带 RTPS 协议栈、把**数据可信度**作为一等公民，
> 并针对长期运行的现场环境做了健壮性设计。

**版本**：`1.0.0` · **状态**：V1 稳定 · **许可**：Apache-2.0

---

## 为什么需要它

DDS（含 Fast DDS）的设计目标是**实时分发**，不是**历史留存**：

| 事实 | 后果 |
|---|---|
| 历史缓存是为**传输**服务的（重传、补发给晚加入者），默认每实例仅 1 条 | 数据量稍大就留不住 |
| `take` 类操作会把数据从缓存移除 | 应用取走后数据在系统中消失 |
| 历史缓存随进程生命周期存在 | 发布者一重启，历史即丢失 |
| 没有"按时间范围查询"的读取语义 | 无法回答"3 分钟前是什么状态" |

**DDS 能保证"数据活着的时候送得到"，但不保证"数据能被找回"。Stratum 补的就是这一层。**

最危险的不是"知道数据不全"，而是**"以为录全了，实际漏了"**——因此 Stratum 把
**数据可信度**做成一等公民：序号缺口检测、双时间戳（源/接收）、质量报告都是一级功能。

---

## 核心特性

### 协议与互通
- **自带 RTPS 协议栈**：SPDP / SEDP / DATA / HEARTBEAT / ACKNACK / **DATA_FRAG** 完整实现，
  与 Fast DDS 双向互通（实测：Stratum 回放 → Fast DDS 订阅端 **20/20**；反向亦通）
- **大 payload 支持**：`DATA_FRAG` 分片发送 + 接收侧缓存重组；
  **路径 MTU 自动探测**（`getsockopt(IP_MTU)`，按对端探测），超限样本自动切片
- **退避重传**：指数退避（50→1000ms，上限 6 次），由心跳周期驱动；分片样本重传时重新切片
- **QoS 兼容判定**：reliability / durability / ownership / deadline / liveliness / 数据表示

### 录制与回放
- **按时间轴回放**：保真原始源时间戳间隔，支持变速、跳转、循环
- **序号缺口检测**：`RecordReceivedSn` 滑动窗口去重 + ACKNACK 依据**真实收到的 SN 集合**
  报告缺口（而非声称"某个 SN 之前全都收到"）
- **录制保护**：拒绝写入非空目录（退出码 3），需显式 `--overwrite`

### 存储
- **LSM 存储层**（基于自研 LSMengine 移植）：时间轴按 key 有序，区间扫描高效
- **不可变地层**：每次录制产出一个 Session，是回放/查询/管理的基本单位

### 跨主机
- **NAT + UDP 中继拓扑**可用（本项目实测环境：WSL ↔ 独立虚拟机，双向打通）
- Windows 侧中继与一键环境准备脚本随仓库提供

---

## 快速开始

### 构建

```bash
# 依赖：CMake >= 3.16、Ninja、g++ (C++17)
# Fast DDS 仅用于工具与互操作测试；核心库不依赖它
cmake -S . -B build -G Ninja
cmake --build build

# 构建诊断工具（不需要 Fast DDS 头文件）
bash tools/fastdds_sub_diag/build_tools.sh
```

### 运行测试

```bash
cd build && ctest            # 期望 34/34
cd .. && bash tools/fastdds_sub_diag/scenarios/08_accounting.sh
#   期望：对账 A 200/200，序号缺口 0
```

### 最小示例

四个可直接运行的示例见 [`examples/`](examples/)：

| 示例 | 内容 |
|---|---|
| [`01_hello_pubsub`](examples/01_hello_pubsub/) | 最小发布订阅（Stratum ↔ Fast DDS 互通验证） |
| [`02_record_replay`](examples/02_record_replay/) | 录制 → 回放 → 校验完整性与时序 |
| [`03_large_payload`](examples/03_large_payload/) | 大样本自动分片（DATA_FRAG）与重组 |
| [`04_cross_host`](examples/04_cross_host/) | 跨主机（NAT + UDP 中继）双向收发 |

```bash
bash examples/02_record_replay/run.sh      # 每个示例都是一键脚本
```

---

## 架构概览

```
                    ┌─────────────────────────────────────────┐
   DDS 域  ────────▶│  protocol/rtps   自带 RTPS 协议栈        │
   (Fast DDS 等)    │   SPDP SEDP DATA HEARTBEAT ACKNACK      │
                    │   DATA_FRAG  ← 大样本分片 / 重组         │
                    └───────────────┬─────────────────────────┘
                                    │ ReceivedSample
                    ┌───────────────▼─────────────────────────┐
                    │  core/   Recorder / Player              │
                    │   序号去重 · 缺口检测 · 变速回放          │
                    └───────────────┬─────────────────────────┘
                                    │
                    ┌───────────────▼─────────────────────────┐
                    │  storage/  时间轴 + LSM 引擎             │
                    │   不可变地层 · 区间扫描 · 环形保留        │
                    └─────────────────────────────────────────┘
```

| 目录 | 内容 | 规模 |
|---|---|---|
| `src/include/stratum/protocol/rtps/` | RTPS 协议栈头文件 | — |
| `src/rtps/` | RTPS 实现（报文构造/解析、writer/reader） | — |
| `src/src/core/` | Recorder / Player | — |
| `src/include/stratum/storage/` | 存储抽象与 LSM 接入 | — |
| `src/` 合计 | **49 个 .cc/.h，约 15 900 行** | — |

---

## 验证与质量

**测试哲学**：禁止冒烟测试。必须有压测、随机测试与真实场景模拟。

| 类别 | 数量 | 说明 |
|---|---|---|
| 单元/集成测试（ctest） | **34** | `ctest` 34/34 |
| 真实场景脚本 | **28** | 覆盖互通、录制回放、QoS、keyed、崩溃恢复、丢包、跨主机 |
| 随机测试 | — | `14_random_suite.sh`、`25_cross_host_stress.sh` 内置随机 |

### 关键实测数据

| 指标 | 结果 |
|---|---|
| 条数对账（A / B） | **200/200**（序号缺口 0）、**157~159** |
| 大 payload（单机） | 1200B / 8000B / 60000B 全部完整 |
| **跨主机多 topic 大数据量** | **2400 条 × 8KB = 19.2MB，零丢包、零串扰** |
| 吞吐阶梯（2KB） | 50 / 200 / 1000 条每秒 **全部 100%** |
| 跨主机大 payload（自动分片） | 8000B ×20 → 21；60000B ×8 → 9 |
| 录制侧实时延迟 | P50 **0.136ms** / P95 0.225 / P99 0.363 / max 0.429 |
| 长稳（55 轮） | RSS 恒定 2048KB，FD 恒定 4 |
| DATA_FRAG 接收侧健壮性 | 多 writer 交错 / 缓存上限 / 畸形分片 / 部分分片不误交付 —— **4 项全 PASS** |

### 排查纪律（写入 `docs/README.md`）

本项目的多数返工都源于**观测手段不可靠**，而非实现本身。因此固化了几条纪律，其中最关键的一条：

> **实现"看起来没生效"时，先怀疑判据与观测手段，并直接看原始输出。**
>
> 典型陷阱：`ninja: no work to do`（改动没进二进制）、`kill` 掉进程导致汇总行从未打印、
> 用猜测的字符串去 grep、CRLF 污染让脚本报 `invalid option name`。

---

## 文档索引

| 文档 | 内容 |
|---|---|
| [`docs/README.md`](docs/README.md) | **文档总索引 + 排查纪律**（建议先看） |
| [`docs/01-resolved-issues.md`](docs/01-resolved-issues.md) | 已解决的问题（含四类问题模式） |
| [`docs/02-open-issues.md`](docs/02-open-issues.md) | 未决问题与已知限制 |
| [`docs/03-verification-matrix.md`](docs/03-verification-matrix.md) | 需求 → 验证项对照矩阵 |
| [`docs/04-deployment.md`](docs/04-deployment.md) | **部署与跨主机运行手册**（含必做系统配置） |
| [`design/`](design/) | 设计文档与开发过程记录 |
| [`00-overview.md`](00-overview.md) … [`07-deployment.md`](07-deployment.md) | 需求规范（8 份交接文档） |
| [`AI_COLLAB.md`](AI_COLLAB.md) | 协作记录（按轮次，含每次改动与验证结果） |

---

## 已知限制

如实列出，不粉饰：

| 限制 | 说明 |
|---|---|
| **接收缓冲上限需部署时调整** | `net.core.rmem_max` 默认仅 208KB，不改会让高突发丢包（实测 87%）；代码已用 `SO_RCVBUFFORCE` 优先绕过，仍无权限时需 `tools/setup_sysctl.sh` |
| **WSL 内无 `sysctl` 写权限** | 因此 WSL 不适合作为高吞吐接收端 |
| **分片样本对丢包更敏感** | 一个 N 片样本任一片丢失即整体失败，等效失败率约为单片样本的 N 倍；退避重传可补回大部分（30% 丢包下 20/20 vs 关闭重传 19/20） |
| **高丢包下收敛有限** | 50% 丢包时约 66%（退避参数未调优） |
| **DATA_FRAG 未做 RFC 级模糊测试** | 已有 4 类异常输入覆盖，但未接入持续 fuzzing |
| **仅支持 IPv4** | 全项目 `AF_INET`，无 `AF_INET6` |

---

## 目录结构

```
Stratum/
├── src/             核心库（RTPS 协议栈 / core / storage）
├── tests/           单元与集成测试（ctest）
├── tools/           诊断工具与场景脚本
│   ├── fastdds_sub_diag/
│   │   └── scenarios/   28 个真实场景脚本
│   ├── udp_relay.py     Windows 侧跨主机 UDP 中继
│   └── start_cross_host.py  跨主机环境一键准备
├── examples/        可运行示例（4 个）
├── docs/            项目文档
├── design/          设计文档与过程记录
├── benchmarks/      基准
└── 00-…07-*.md      需求规范（8 份交接文档）
```

---

## 许可

[Apache License 2.0](LICENSE)。各源文件头部含 `SPDX-License-Identifier: Apache-2.0`。
