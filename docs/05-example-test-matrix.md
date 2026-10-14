# 示例与测试覆盖矩阵

> **这份矩阵是"绝不允许漏测"的核查依据。** 每个模块都必须有：
> * `examples/cpp/<模块>_demo.cpp` —— 简单用法，**≥100 行**
> * `examples/cpp/<模块>_full.cpp` —— 全面用法，**≥300 行**，**≥3 个真实场景**
> * `tests/` 下的测试覆盖**大数据量 / 随机 / 真实场景**三类
>
> 状态图例：✅ 完成并实测 · 🚧 进行中 · ⬜ 未开始

---

## 一、模块清单（依据 `src/include/stratum/`）

| # | 模块 | 头文件 | 核心类型 |
|---|---|---|---|
| 1 | **core** | `core/recorder.h` `core/itimeline.h` `core/timeline_impl_v2.h` `core/types.h` `core/status.h` | `Recorder` `Player` `Query` `TimeRange` `SessionMetadata` `Status` |
| 2 | **protocol** | `protocol/idds_backend.h` `protocol/itransport.h` | `IDdsBackend` `ISource` `ISink` `BackendConfig` `ReceivedSample` |
| 3 | **storage** | `storage/istorage_engine.h` `storage/time_key.h` `storage/meta_codec.h` | `IStorageEngine` `ITimeline` `TimeKeyCodec` `MetaCodec` |
| 4 | **types** | `core/types.h` `types/iserializer.h` | `TimeRange` `TopicMetadata` `SessionMetadata` `Event` `ISerializer` |
| 5 | **rtps** | `protocol/rtps/rtps_backend.h` `protocol/rtps/rtps_packet_utils.h` `rtps_writer.h` `rtps_reader.h` | `RtpsBackend` `BuildDataPacket` `BuildDataFragPackets` |

> `src/include/stratum/rtps/**`（83 个头文件）是**移植的 Fast DDS 源码**（见
> `design/09-rtps-porting-strategy.md`），**不属于本项目对外 API**，因此不为它写示例；
> 它的正确性由 `tests/rtps/` 下的移植验证测试保证。

---

## 二、examples/cpp 覆盖矩阵

| 模块 | `_demo.cpp`（≥100 行） | 行数 | `_full.cpp`（≥300 行） | 行数 | 真实场景数 |
|---|---|---|---|---|---|
| **core** | ✅ `core_demo.cpp` | **196** | ✅ `core_full.cpp` | **450** | **3**（产线录制 / 故障复盘 / 回归回放） |
| **protocol** | ✅ `protocol_demo.cpp` | **291** | ✅ `protocol_full.cpp` | **896** | **3**（非 DDS 数据源 / 自定义去向 / 能力协商） |
| **storage** | ✅ `storage_demo.cpp` | **246** | ✅ `storage_full.cpp` | **580** | **3**（时间轴扫描 100 万键 / 引擎契约 / 元数据版本） |
| **types** | ✅ `types_demo.cpp` | **348** | ✅ `types_full.cpp` | **625** | **3**（双时间戳 / 质量报告 / 序列化器降级） |
| **rtps** | ✅ `rtps_demo.cpp` | **264** | ✅ `rtps_full.cpp` | **506** | **3**（报文字节级 / 发送侧配置 / 端到端） |

**历史文件**（旧的 4 个示例，内容会被新结构吸收后移除）：
`01_record_and_replay.cc`(186) `02_query_timeline.cc`(157)
`03_playback_control.cc`(180) `04_custom_backend.cc`(268)

---

> **examples/cpp 覆盖已完成 5/5**：每个模块都有 `_demo.cpp`(≥100 行) 与
> `_full.cpp`(≥300 行 + ≥3 个真实场景)，全部编译并实测运行通过。

## 三、tests 覆盖矩阵

每个模块都需要三类测试。已有测试若已覆盖某类，标注来源文件。

| 模块 | 大数据量 | 随机 | 真实场景（≥3） |
|---|---|---|---|
| **core** | ⬜ | ⬜ | ⬜ |
| **protocol** | ⬜ | ⬜ | ⬜ |
| **storage** | 🚧 `tests/stress/test_stress_throughput.cc` | 🚧 `tests/random/test_lsm_v2_random.cc` `tests/random/test_random_ops.cc` | 🚧 `tests/real/test_lsm_v2_real.cc` `tests/real/test_real_sensor_data.cc` |
| **types** | ⬜ | ⬜ | ⬜ |
| **rtps** | ⬜ | ⬜ | ⬜ |

**已有 ctest（38 个源文件）分类**：

| 目录 | 文件 | 性质 |
|---|---|---|
| `tests/unit/` | `test_ack_bitmap` `test_storage_contract` `test_time_key` | 单元 |
| `tests/integration/` | `test_record_replay` `test_record_replay_v2` `test_timeline_basic` `test_timeline_v2` | 集成 |
| `tests/random/` | `test_lsm_v2_random` `test_random_ops` | 随机 |
| `tests/real/` | `test_lsm_v2_real` `test_real_sensor_data` | 真实场景 |
| `tests/review/` | 12 个（并发/队列/快照/WAL 损坏/元数据…） | 回归 |
| `tests/rtps/` | 11 个（CDR/子消息/SPDP/SEDP/QoS/报文工具…） | 移植验证 |
| `tests/stress/` | `test_stress_throughput` | 压测 |

---

## 四、进度记录

| 时间 | 完成内容 |
|---|---|
| 2026-10-14 12:30 | 建立本矩阵；摸清 5 个模块；确认旧示例行数均不足 300 |
| 2026-10-14 13:20 | **examples 可重复性修复 + LSM 引擎覆盖**：全部 10 个示例连跑两次均 PASS（修前 core_full 第二次 FAIL 3 项）；storage_full 的契约用例参数化，MemoryEngine 与 LsmEngine 各 21 项全 OK |
| 2026-10-14 13:35 | **rtps 模块完成**：`rtps_demo.cpp`(264 行)；`rtps_full.cpp`(506 行)三个场景全通过。**examples 覆盖矩阵 5/5 全部完成** |
| 2026-10-14 13:30 | **types 模块完成**：`types_demo.cpp`(348 行)；`types_full.cpp`(625 行)三个场景全通过。发现 `DynamicValue` 的 MakeXxx 工厂与 ToString 尚未实现（链接时 undefined reference） |
| 2026-10-14 13:10 | **storage 模块完成**：`storage_demo.cpp`(246 行, 30 项检查全 OK)；`storage_full.cpp`(536 行) 三个场景全通过（100 万键顺序不变量 / 引擎契约 / 元数据版本拒绝） |
| 2026-10-14 12:55 | **protocol 模块完成**：`protocol_demo.cpp`(291 行) 编译+运行通过；`protocol_full.cpp`(896 行) 三个真实场景全通过（宠数守恒 4000=129+3871） |
| 2026-10-14 12:5x | **core 模块完成**：`core_demo.cpp`(196 行) 编译+运行通过；`core_full.cpp`(450 行) 三个真实场景全通过（15 项检查 0 失败）。新增共用件 `common/memory_backend.h`(279 行) |
