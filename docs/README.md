# 项目文档索引（docs/）

> 想了解**需求与设计意图** → 看根目录 [`00`~`07-*.md`](../00-overview.md)
> 想了解**实现原理** → 看 [`../design/`](../design/)
> 想知道**踩过哪些坑、现在什么状态** → 就是本目录

---

## 文档地图

| 位置 | 内容 | 什么时候看 |
|---|---|---|
| **[`../README.md`](../README.md)** | **项目门面**：特性、快速开始、架构、实测数据 | 第一次接触项目 |
| **根目录 `00`~`07-*.md`** | 需求规范：项目定义、需求 33 条、非功能、场景 12 个、边缘场景 23 个 | 想知道"要做什么、为什么" |
| **[`../design/`](../design/)** | 设计文档与过程记录；`12-rtps-interop-rootcause.md` 是互通根因链，`15-test-report.md` 是验收量化报告 | 想知道"怎么实现的" |
| **[`../examples/`](../examples/)** | 4 个可运行示例 | 想快速跑起来 |
| **[`../AI_COLLAB.md`](../AI_COLLAB.md)** | 逐轮协作记录（含每次改动、验证结果、被修正的结论） | 想追溯某个结论的来历 |
| **本目录** | 已解决/遗留问题清单、验证矩阵、部署手册 | 想知道"踩过哪些坑、现在什么状态" |

### 本目录文件

| 文档 | 内容 |
|---|---|
| [`01-resolved-issues.md`](01-resolved-issues.md) | **已解决问题**：现象 → 根因 → 修复 → 验证；开头归纳了**四类问题模式** |
| [`02-open-issues.md`](02-open-issues.md) | **遗留问题**（🔴 / 🟡 / 🟢 + 环境约束） |
| [`03-verification-matrix.md`](03-verification-matrix.md) | **验证矩阵**：28 个场景脚本 ↔ 覆盖的问题 ↔ 判据 |
| [`04-deployment.md`](04-deployment.md) | **部署与跨主机运行手册**：必做系统配置、中继用法、端口公式、环境坑速查 |
| [`06-test-gaps.md`](06-test-gaps.md) | **测试缺口清单**（审计结果：真缺口 / 间接覆盖 / 死接口；含补法建议） |
| [`07-cli-design.md`](07-cli-design.md) | **生产级 CLI 设计**（32 个场景，每条含需求来源 + 命令形态 + 延伸） |
| [`08-cli-spec.md`](08-cli-spec.md) | **CLI 参数级规范**：通用参数、选择器 BNF、各命令完整参数、状态机、锁与并发矩阵 |
| [`09-cli-scenarios-deep.md`](09-cli-scenarios-deep.md) | **32 场景再发散**：边界分支 / 失败模式 / 场景交互，回收 18 条新约束 |
| [`11-type-system.md`](11-type-system.md) | **类型系统的决策与已知边界**：定义格式选型（简化自描述）、两处已知有损、元数据独立 key 约定、端到端链路现状 |
| [`12-roadmap.md`](12-roadmap.md) | **进度快照与下一步任务**：当前状态、已完成清单、用户期望（eBPF/topic 动态检测）及评估、三层下一步任务、遗留问题、方法学纪律 |
| [`13-probe-requirements.md`](13-probe-requirements.md) | **埋点工程需求分析**：12 个真实场景（含监控回调/格式动态指定/QoS 动态调配/topic 动态配置）、16 条需求（PR-01~16）、边界与非目标 |
| [`handoff-readme.md`](handoff-readme.md) | 历史：本项目早期的"文档集索引"（已由根 README 取代） |

---

## 当前能力（V1.0.0，实测）

**单机**
- 双向互通：回放条数对账 **200/200**；录制方向 `序号缺口: 0`
- 大 payload：1200B / 8000B / 60000B 全部完整
- 多话题并发：**零串扰**；keyed 话题：4 个实例 4 种 key 正确
- QoS 判定矩阵 **6/6**（含两个"正确拒绝"）
- 实时延迟：**P50 0.136 ms / P95 0.225 / P99 0.363 / max 0.429**
- 速率控制：抖动与期望逐一吻合
- 长稳：soak 55 轮 RSS/FD 恒定（无泄漏）
- `ctest`：**34/34**

**跨主机**（WSL ↔ 独立虚拟机，经 Windows UDP 中继）
- **双向打通**：正向 201 条、反向 81/80
- **大 payload 全自动**：MTU 自动探测 → 超限自动分片 → 对端重组
  （8000B ×20 → 21；60000B ×8 → 9）
- **工业级压测**：多 topic **2400 条 × 8KB = 19.2MB，零丢包零串扰**；
  吞吐 50 / 200 / 1000 条每秒**全部 100%**；随机 6 轮全过
- **健壮性**：多 writer 交错 / 缓存上限 / 畸形分片 / 部分分片不误交付 —— **4 项全 PASS**

---

## 可靠性边界（实测，取代早期结论）

| 条件 | 结果 |
|---|---|
| 无丢包 | 100% |
| **接收侧注入 10% 丢包** | **100%**（退避重传生效） |
| 接收侧注入 30% 丢包 | **80%**（关闭重传只有 53%） |
| 接收侧注入 50% 丢包 | **66%** |
| 跨主机高吞吐（调大 `rmem_max` 后） | 100% |

> **早期结论已修正**：此前记录的"≥5% 丢包会残留、成因未定位"，
> 根因是 **ACKNACK 的 `base` 语义错误**导致重传永不触发 —— 已修复（见
> `01-resolved-issues.md`）。退避参数尚未调优，因此高丢包下仍有残留。

**当前不承诺**：单条 > 64 KiB 样本（`DATA_FRAG` 已实现，但未做该量级的压测）、
IPv6（全项目 `AF_INET`，已决策不做）、持续 fuzzing。

---

## 复现前必读

### 环境

```bash
export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu

cmake -S . -B build -G Ninja && cmake --build build
bash tools/fastdds_sub_diag/build_tools.sh        # 改了 src/ 之后必须重建工具
```

上游 Fast DDS 位于 `/UserData/Fast-DDS`（**只读**，Release 构建 ——
`Log::SetVerbosity(Info)` 拿不到 EDP 的 Info 分支，取证要用行为证据而非日志）。

**接收端必须先调大缓冲**（否则高突发丢包）：

```bash
bash tools/setup_sysctl.sh --persist
```

### 排查纪律（用返工换来的）

1. **不要凭推断说"上游拒绝了什么"** —— 拿返回值 / 抓包 / 字节 dump；
2. **静默失败优先怀疑**：本项目多数缺陷都表现为"看起来正常、实际没通"；
3. **"共用标识 / 重复实现"优先怀疑**：每处都是"修了一个副本、另一个没修"；
4. **声明必须与实际一致**：每处都是"本端以为兼容、对端认为不兼容"；
5. **先测量再优化**：凭直觉的优化多半白做；
6. **失败也要写下来**：否则下一个 agent 会重复同样的弯路；
7. **脚本 FAIL 时，先怀疑判据，再怀疑被测对象**；
8. **`ninja: no work to do` 不可忽略**：改了代码要看效果时，
   必须先确认改动真的进了二进制（删 `.o` 重编 + `nm` 验符号）；
9. **写 WSL 文本文件时显式 `newline=""`**：否则 CRLF 会让 shell 脚本
   以 `set: pipefail: invalid option name` 的形式整个失效。

---

## 目录约定

```
Stratum/
├── README.md           项目门面
├── CHANGELOG.md        版本变更
├── VERSION             1.0.0
├── CONTRIBUTING.md     开发约定（含上面那些纪律）
├── 00-…07-*.md         需求规范（8 份交接文档，保持原位以免破坏引用）
├── examples/           4 个可运行示例
├── docs/               本目录：问题、验证、部署
├── design/             设计文档与过程记录
├── src/                核心库（49 个 .cc/.h，约 15 900 行）
├── tests/              单元与集成测试（ctest 34 个）
├── tools/              诊断工具与 28 个场景脚本
│   ├── fastdds_sub_diag/scenarios/
│   ├── udp_relay.py            Windows 侧跨主机 UDP 中继
│   └── start_cross_host.py     跨主机环境一键准备
├── benchmarks/         基准
└── build/tools/logs/   所有脚本的日志（不要写 /tmp）
```
