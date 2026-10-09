# Stratum 开发进展报告

> 项目路径：`/UserData/CodexWorkSpace/Stratum`
> 本报告记录第一阶段（骨架打通）的成果。

---

## 1 本阶段完成内容

### 1.1 构建系统（可用）

| 项 | 状态 |
|---|---|
| CMake 配置 | ✅ 通过（GCC 13.3.0，C++17，Ninja） |
| 编译 | ✅ 0 error |
| 测试 | ✅ 10/10 通过 |
| 严格警告 | ✅ `-Wall -Wextra -Wpedantic` 已开启 |

### 1.2 已实现的模块

| 模块 | 文件 | 状态 |
|---|---|---|
| 状态码 | `src/include/stratum/core/status.h` + `src/core/status.cc` | ✅ |
| 时间键编解码 | `src/include/stratum/storage/time_key.h` + `src/storage/time_key.cc` | ✅ |
| 核心数据模型 | `src/include/stratum/core/types.h` | ✅ |
| 存储引擎接口 | `src/include/stratum/storage/istorage_engine.h` | ✅ |
| 插件注册表 | `src/include/stratum/core/plugin_registry.h` | ✅ |
| 时间线接口 | `src/include/stratum/core/itimeline.h` | ✅ |
| 传输接口 | `src/include/stratum/protocol/itransport.h` | ✅ |
| 序列化接口 | `src/include/stratum/types/iserializer.h` | ✅ |
| 内存存储引擎 | `src/storage/memory/memory_engine.{h,cc}` | ✅ |
| 测试框架 | `tests/test_framework.h` | ✅ |
| TimeKey 测试 | `tests/unit/test_time_key.cc` | ✅ 10 用例 |

**代码规模**：约 2,700 行（含注释）

### 1.3 测试结果

```
registered cases: 10
------------------------------------------------------------------------
  [PASS] TimeKey.EncodeDecodeRoundTrip
  [PASS] TimeKey.ByteOrderMatchesLogicalOrder          <- 2000 条随机数据
  [PASS] TimeKey.SameTimestampProducesDistinctKeys     <- 1000 条同时刻消息
  [PASS] TimeKey.TopicRangeCoversExactlyThatTopic
  [PASS] TimeKey.TimeRangeIsHalfOpen
  [PASS] TimeKey.PrefixRangeUpToIsExclusiveUpperBound
  [PASS] TimeKey.ExtremeValuesHandledCorrectly
  [PASS] TimeKey.DecodeRejectsMalformedInput
  [PASS] TimeKey.PeekMatchesDecode
  [PASS] TimeKey.LooksLikeTimeKeyIsStrict
------------------------------------------------------------------------
result: 10 passed, 0 failed
```

---

## 2 关键设计决策（已落地）

### 2.1 时间键：字节序 == 时间序

整个项目的地基。20 字节定长编码：

```
offset  size  field
     0     4  magic（格式版本）
     4     4  topic_id
     8     8  timestamp_ns（大端）
    16     4  seq_in_ts（同一时间戳去重）
```

**定长的价值**：区间边界可用"下一个 topic 的起始键"精确表达，
范围扫描无需处理变长前缀歧义。

**验证**：`ByteOrderMatchesLogicalOrder` 用 2000 条随机数据确认
"逻辑排序"与"编码后字节排序"完全一致。

### 2.2 同时间戳去重：放在 key 里，不改后端

`seq_in_ts` 使同一时间戳的多条消息产生**不同的键**，不会互相覆盖。
**这避免修改 LSM 的比较器与跳表** —— 设计阶段的发现，现已落地。

### 2.3 插件式：统一注册表

四类插件（存储/传输/协议/序列化）共用同一个 `PluginRegistry` 模板。
新增后端只需：实现接口 + 实现工厂 + 一行注册。

### 2.4 快照是一等公民

`IStorageEngine` 把快照作为核心接口而非可选优化。内存引擎已实现
**版本链 + 快照可见性**：每个 key 维护按序号降序的版本链，
快照读时取第一个 `sequence <= snapshot` 的版本。

---

## 3 开发中解决的问题（备查）

| # | 问题 | 根因 | 解决 |
|---|---|---|---|
| 1 | 测试框架多行宏编译失败 | `#define` 续行符被工具链去除行尾空格时删除 | 放弃多行宏，断言改 inline 函数 |
| 2 | 命名空间未闭合 | 宏未正确闭合 `namespace {` | 重构为单行宏 + 显式函数 |
| 3 | `std::sqrt` 歧义 | include 顺序导致标准库被卷入自定义命名空间 | 规定标准库头必须最先包含 |
| 4 | `uint8_t` 未定义 | 缺 `<cstdint>` | 补齐 include |
| 5 | `Version` 类型与方法重名 | 嵌套结构体与虚函数冲突 | 重命名 `ValueVersion` |

**确立的规范**：宏只用于"无法用函数表达"的场景（如自动注册），
其余一律用函数。

---

## 4 后续计划

| 阶段 | 内容 | 验收标准 |
|---|---|---|
| 2 | Timeline 实现 | 可注册话题、追加事件、范围查询、定点查询 |
| 3 | 自研 LSM 引擎 | 与内存引擎跑通同一套测试 |
| 4 | Recorder / Player / Query / 质量报告 | 端到端：录制 N 条 → 回放 → 逐条比对一致 |
| 5 | RTPS 协议层 | 编解码测试向量通过 |
| 6 | CLI + 30+ 测试场景 + 性能报告 | 全部通过 + 压测报告 |

---

## 5 验证命令

```bash
cd /UserData/CodexWorkSpace/Stratum
cmake --build build
./build/tests/test_time_key
```
