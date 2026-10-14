# Stratum 示例

## C++ 示例（`cpp/`）—— 给要把 Stratum 作为库嵌入的程序员

**五个模块，各两个文件**：

| 模块 | `_demo.cpp`（简单用法，≥100 行） | `_full.cpp`（全面用法，≥300 行 + ≥3 真实场景） |
|---|---|---|
| **core** | `core_demo.cpp`（196 行） | `core_full.cpp`（450 行）：产线录制 / 故障复盘 / 回归回放 |
| **protocol** | `protocol_demo.cpp`（291 行） | `protocol_full.cpp`（896 行）：非 DDS 数据源 / 自定义去向 / 能力协商 |
| **storage** | `storage_demo.cpp`（246 行） | `storage_full.cpp`（580 行）：100 万键时间轴 / 引擎契约 / 元数据版本 |
| **types** | `types_demo.cpp`（348 行） | `types_full.cpp`（625 行）：双时间戳 / 质量报告 / 序列化器降级 |
| **rtps** | `rtps_demo.cpp`（264 行） | `rtps_full.cpp`（506 行）：字节级报文 / 发送侧配置 / 端到端 |

合计 **demo 1345 行 + full 3057 行，15 个真实场景**，全部编译通过并实测运行。

共用件：`common/memory_backend.h` —— 内存后端（同时是 `IDdsBackend` 扩展点的
最小完整实现），让示例不依赖外部 DDS 参与者就能演示 API 本身。

### 构建与运行

```bash
# 1) 先构建 Stratum 库
cmake -S . -B build -G Ninja && cmake --build build

# 2) 构建示例
cmake -S examples/cpp -B build-examples -G Ninja
cmake --build build-examples

# 3) 跑（选一个）
./build-examples/core_demo
./build-examples/protocol_full
./build-examples/storage_full      # 100 万键，约数十秒
./build-examples/types_full
./build-examples/rtps_full         # 含 loopback 收发尝试
```

### 建议的阅读顺序

1. `core_demo.cpp` —— 先看清 `Recorder` / `Player` / `Query` 三件套与时间线的关系
2. `protocol_demo.cpp` —— 再看"数据从哪来、到哪去"这个扩展点
3. `storage_demo.cpp` —— 然后理解"键的字节序 == 时间序"这条地基
4. `types_demo.cpp` —— 补齐数据模型（尤其**两种时间戳**的分工）
5. `rtps_demo.cpp` —— 最后看自带的 RTPS 配置与报文层
6. 各 `_full.cpp` —— 每个都演示三个真实场景，代码里写明了"为什么这样写"

### 关于"真实场景"的说明

每个 `_full.cpp` 的三个场景都是**贴近实际用法**的，不是为凑数造的：

* **core**：产线传感器长时间录制（容量控制 + 环形保留 + 优雅停止）；
  故障复盘（按时间段查询 + 定点状态重建 + 完整性核对）；
  回归测试数据回放（区间裁剪 + 变速 + 话题重映射 + 中途取消）
* **protocol**：接非 DDS 数据源（有界队列 + 三种溢出策略 + 守恒计数）；
  转发到自定义去向（连接状态驱动就绪、有语义的超时、注入失败）；
  能力协商与优雅降级（同一业务代码面对三种能力组合）
* **storage**：100 万随机键的时间轴扫描与顺序不变量；存储引擎契约（含边界与快照）；
  元数据版本演进与损坏恢复（高版本必须被拒绝）
* **types**：两种时间戳对回放时序保真的影响（含时钟回退/跳变）；
  质量报告（能否识破"看起来完整"）；自定义序列化器与降级契约
* **rtps**：报文构造的字节级正确性（1 万条随机 + 2000 组随机分片）；
  发送侧配置对行为的影响；loopback 端到端

---

## Shell 示例（`0x_*/`）—— 端到端可运行链路

| 示例 | 验证内容 | 实测结果 |
|---|---|---|
| [`01_hello_pubsub`](01_hello_pubsub/) | Stratum ↔ Fast DDS 最小互通 | 回放 32 → 收到 **33** |
| [`02_record_replay`](02_record_replay/) | 录制 → 回放 → 完整性校验 | 录制 86 条、缺口 0 → 收到 **86 条** |
| [`03_large_payload`](03_large_payload/) | 大样本自动分片（DATA_FRAG） | 1200B/8000B/60000B **全 PASS** |
| [`04_cross_host`](04_cross_host/) | 跨主机双向（NAT + UDP 中继） | 正向 **51/50**、反向 **51/50** |

```bash
bash examples/02_record_replay/run.sh
```

> **说明**：早期的 `cpp/01_record_and_replay.cc`、`02_query_timeline.cc`、
> `03_playback_control.cc`、`04_custom_backend.cc` 四个文件已被上述
> 按模块组织的结构取代（内容分别并入 `core_*` / `protocol_*`），因此移除。
