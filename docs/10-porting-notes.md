# 移植记录：日志模块与配置模块（来自 Go2Cpp）

> **移植时间**：2026-10-15
> **上游**：`/UserData/CodexWorkSpace/Go2Cpp`（MIT License，Copyright (c) 2026 Go2Cpp contributors）
> **许可声明**：见根目录 [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md) 第 5 节
> **决策依据**：用户要求"WSL 里有现成的日志和配置模块，改个名字移植进来加速开发"，
> 与项目既有的**移植优先**策略一致（参考 `design/09-rtps-porting-strategy.md`）。

---

## 1 为什么移植而不是自己写

CLI 需要两样基础设施：

| 需求 | 裸写要做什么 | 上游给了什么 |
|---|---|---|
| **日志** | 级别过滤、格式化、文件写入、**轮转**、异步不阻塞、可替换输出目标 | `LogFilter → LogItemWorker → LogFormatter → LogSink` 四层可替换架构，**零第三方依赖** |
| **配置** | INI 解析、类型转换与校验、热更新、**两阶段事务回滚** | `IniFile` / `ValueCodec` / `ConfigVar` / `Config`，含事务与监听器 |

自己写一遍的代价不只是行数，还有**边界情况**：轮转时的并发写、异步队列在退出时的
刷盘、配置热更新时的半套状态。上游已经处理过这些，且**零第三方依赖**（符合
`NFR-3.4 不引入过重依赖`）。

---

## 2 移植清单

| 源文件（Go2Cpp） | 目标（Stratum） | 行数 | 改动 |
|---|---|---|---|
| `include/go2cpp/logging/log.hpp` | `src/include/stratum/logging/log.hpp` | 506 | 改名 |
| `include/go2cpp/log.hpp` | `src/include/stratum/log.hpp` | 4 | 改名 |
| `src/logging/log.cpp` | `src/logging/log.cpp` | 1024 | 改名 |
| `include/go2cpp/config/config.hpp` | `src/include/stratum/config/config.hpp` | 496 | 改名 + **裁 scheduler** |
| `include/go2cpp/config.hpp` | `src/include/stratum/config.hpp` | 4 | 改名 |
| `src/config/config.cpp` | `src/config/config.cpp` | 745 | 改名 + **裁 scheduler** |

**合计 2779 行**（日志 1534 + 配置 1245）。

### 2.1 改名映射

| 上游 | Stratum | 出现位置 |
|---|---|---|
| `go2cpp::log` | `stratum::log` | 命名空间 |
| `go2cpp::config` | `stratum::config` | 命名空间 |
| `go2cpp/log.hpp` | `stratum/log.hpp` | include 路径 |
| `GO2CPP_LOG_NAME` 等 | `STRATUM_LOG_NAME` 等 | 8 个宏 |
| `GO2CPP` | `STRATUM` | 命名空间别名 |
| `go2cpp.ini` | `stratum.ini` | 默认配置文件名 |
| `log/go2cpp.log` | `log/stratum.log` | 默认日志路径 |

### 2.2 裁剪：为什么去掉 `RuntimeConfig`

上游的配置模块有一个 `RuntimeConfig` 结构体，它把 `scheduler.*` 与 `log.*`
两组配置变量绑定到 Go2Cpp 的调度器：

```cpp
struct RuntimeConfig final {
    scheduler::SchedulerConfig scheduler{};   // ← 依赖调度器
    ...
};
```

`config.hpp` 因此 `#include "go2cpp/scheduler/scheduler.hpp"`，
`config.cpp` 里 scheduler 相关代码有 **105 处**。

**Stratum 没有调度器**，保留它只会带来一堆无关依赖。因此裁掉了 5 个函数：

| 被裁函数 | 理由 |
|---|---|
| `RuntimeConfig::FromIni` | 需要一个含 `SchedulerConfig` 的结构体 |
| `RuntimeConfig::Validate` | 校验的是调度器参数范围 |
| `RuntimeConfig::ApplyLogging` | 应用到调度器 |
| `LoadRuntimeConfig`（自由函数） | 上述三者的组合入口 |
| `BindRuntimeConfig`（自由函数） | 把 `scheduler.*` 绑定到 `Config` |

**保留**：`IniFile`、`ValueCodec`（含 `string` / `bool` / `log::Level` / `vector<T>` 特化）、
`ConfigVarBase` / `ConfigVar<T>`、`Config`（含 `AddCommitListener` / `AddTransactionValidator`
/ `StartWatcher` 等事务与热更新能力）。

> 裁剪后 `config.cpp` 仍有 11 行提到 "scheduler"，但都是
> `ValidateSchedulerSize` 内部的**字符串与注释**（形如 `"scheduler." + key + reason`），
> 不构成编译期依赖。保留它们无害，删除会扩大改动面。

---

## 3 移植中踩的坑（值得记录）

### 3.1 裁剪过度，把必需的 `IniFile::Has` 一起删了

**第一版裁剪**用"从 `RuntimeConfig` 出现处向前找空行"的方式定位函数起点。
问题在于 `IniFile::Has`（原 397 行）**紧邻** `RuntimeConfig::FromIni`（原 412 行）
之前 —— 向前搜索跨过了它，把两个函数一起删了。

**症状**：编译两个库都通过，但**链接测试时**报：

```
undefined reference to `stratum::config::IniFile::Has(std::string const&, std::string const&) const'
```

**教训**：**能编译 ≠ 裁剪正确**。静态库允许未定义符号，只有最终链接才暴露。
第二版改为**按函数签名精确定位** + 删除后**显式核对保留函数清单**：

```
保留的类方法:
  bool IniFile::Load(          ✓
  bool IniFile::Parse(         ✓
  std::string IniFile::Get(    ✓
  bool IniFile::Has(           ✓  ← 第一版丢的就是它
  ...
```

### 3.2 头文件守卫被破坏

裁剪 `config.hpp` 时在末尾硬加了一句
`#endif // STRATUM_CONFIG_CONFIG_H_`，但上游用的是 `#pragma once`，
于是编译报 `#endif without #if`。改为在文件首部补 `#pragma once`、删掉孤立 `#endif`。

### 3.3 又踩了 `wsl bash -lc` 吃变量

`wsl bash -lc 'D=/path; grep ... "$D/tests/..."'` 里的 `$D` 被外层 shell 吃掉，
`grep` 收到的是 `/tests/CMakeLists.txt`。**这在本项目已是第 5 次**，
纪律是：**涉及变量的命令一律写进脚本文件再执行**。

### 3.4 测试自己成了瓶颈

第一版大数据量日志测试的等待循环是 `2000 次 × sleep(10ms)`，**每次都 `ReadAll`
整个 6MB 日志文件**，结果报出"落盘耗时 91125 ms"。这个数字主要花在**测试的
读文件**上，不是日志落盘慢。

改为固定观察窗 `12 × 250ms`，并调整断言口径（见 4.2）。测试耗时从 **38 秒降到 3.6 秒**。

---

## 4 测试与实测数据

测试文件：`tests/unit/test_log_config.cc`（489 行，**16 个用例**）

### 4.1 覆盖情况

| 类别 | 用例 |
|---|---|
| **基础契约** | INI 解析（节/键/注释/fallback）、重复 Parse 清空、缺文件报错；`ValueCodec` 标量往返、**拒绝尾部垃圾**（`"42abc"` 必须失败）、逗号列表、日志级别编解码；日志写文件后可读回、级别过滤生效 |
| **大数据量** | 10 万条日志、5000 键的 INI |
| **随机（多种子）** | 4 种子 × 随机键值 INI；3 种子 × 3000 次数值往返 |
| **真实场景（3 个）** | ① 配置文件驱动日志行为（level/dir/stdout）② 配置校验拒绝非法值 ③ 轮转文件 |

### 4.2 实测数据

```
[规模] 写 100000 条：入队 86.8 ms
       → 约 115 万条/秒，且**不阻塞调用方**（异步日志的核心价值）
[规模] 观察窗结束时未落盘 5 条（占 0.0050%）
       → 异步 worker 在结尾允许极少量未落盘，测试如实打印，不做"一条不丢"的假断言
[规模] 解析 5000 键的 INI：1.07 ms
[场景3] 轮转目录下日志文件 1 个，合计 3887915 字节
```

**断言口径的说明**（这一点是刻意设计的）：

* 断言 **入队耗时 < 2000 ms** —— 这是选这套日志库的**真正原因**（不拖慢业务线程）
* 断言 **落盘 ≥ 99.9%** —— 允许异步尾部少量丢失，但**必须如实打印缺口条数**
* 不写"以 10 万条日志跑满 200 MB/s"这类**看起来漂亮但无意义**的断言

### 4.3 回归结果

```
test_log_config:            16 passed, 0 failed（3.6 秒）
全量 ctest:                  38 / 38 通过（21.88 秒）
```

移植前基线是 37 个测试，新增 1 个（`test_log_config`，label `config`），
**既有测试无回归**。

---

## 5 上游同步

移植后本项目的副本与上游**独立演进**。若上游修复了重要缺陷：

```bash
# 1) 对比差异
diff -u /UserData/CodexWorkSpace/Go2Cpp/src/logging/log.cpp \
        /UserData/CodexWorkSpace/Stratum/src/logging/log.cpp

# 2) 本项目相对上游的改动是确定且可机械重放的：
#    · 改名（go2cpp -> stratum / GO2CPP -> STRATUM）
#    · 裁掉 5 个 RuntimeConfig 相关函数
#    因此上游更新可以重新走一遍移植脚本，而不是手工合并
```

**建议**：改动累积到一定程度后，把移植步骤固化成脚本
（本轮的脚本逻辑：复制 → 改名 → 按签名裁剪 → 核对保留函数清单），
放在 `tools/` 下，使重复移植可审计。

---

## 6 后续计划

移植来的两个模块**尚未接入 CLI**，这是下一步：

| 模块 | 计划接入点 |
|---|---|
| **日志** | CLI 的 `--verbose` / `--log-format json` / `--log-file`（当前 `main.cc` 里只是记录参数，未生效） |
| **配置** | `stratum config validate/explain` 改为使用 `IniFile` 的完整解析能力（当前是自写的极简解析器，只支持 `key: value` 与一层嵌套）；`record --config` 使用 `Config` 的热更新 |
| **两者结合** | `Config` 的 `log.*` 变量直接驱动 `LoggerManager`，实现"改配置即改日志行为" |

---

## 7 移植模块的已知缺陷（对上游的发现）

移植过程中发现一处**注释与实现不一致**，记录在此以便上游修复后重新核对：

### 7.1 `PatternFormatter` 声称支持 `{time}`，实际未注册

* **头文件注释**（`src/include/stratum/logging/log.hpp`）写着：
  > 默认格式支持 `{time}` `{level}` `{logger}` `{thread}` `{fiber}` `{file}` `{line}`

* **实际注册表**（`src/logging/log.cpp` 的 `PatternFormatter::add(...)`）：
  ```
  m p N e E f l t F c
  logger level message thread fiber elapse thread_name file line function
  ```
  **没有 `time`**。`{elapse}` 是**进程启动以来的秒数**，不是 wall-clock 时间戳。

* **症状**：模式里写 `{time}` 会**原样输出字面量** `{time}`（未识别项按文档说明原样保留）。

* **CLI 的规避**：`--log-format json` **不带时间字段**。
  原因：`{elapse}` 同样不会被替换（实测输出字面量 `{elapse}`），
  说明本模块没有可用的 wall-clock 占位符。
  而 JSON 日志的消费者（journald / loki / filebeat）本来就会补采集时间戳，
  因此**宁可不带，也不输出字面量或语义可疑的运行秒数**。
  需要绝对时间时用默认的 text 格式（实测可用，形如
  `2026-10-15 13:39:04.941 [INFO] stratum (file:line) msg`）。

* **建议上游**：要么注册 `time` 字段（需要把 `LogRecord` 里的时间戳格式化为
  wall-clock 字符串），要么修正头文件注释，避免使用者按注释写错。
