// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/include/stratum/probe/probe_config.h
//
// 埋点专用配置：自动生成 / 热重载 / 按 topic 覆盖 / 双语义错误处理。
//
// 需求依据：docs/13-probe-requirements.md
//   PR-05 首次调用时自动生成配置文件（含注释、异步、O_EXCL、绝不改写用户文件）
//   PR-06 改配置热生效（enabled / format / sample_rate / QoS）
//   PR-07 非法或半写配置 → 保留旧值
//   PR-08 **全局 enabled 是 fail-safe**（解析失败 → 关闭），其余是 fail-preserve
//   PR-09 `.lock` 存在时暂停重载；锁超时强制重载（S08）
//
// 【为什么不直接用 config::Config】
// `Config` 是"强类型配置项 + 监听器"的中心，适合**进程自己**的配置；
// 而埋点配置的特点是：**按 topic 覆盖**、**允许未知键**（用户可能先写后埋）、
// **解析失败要能区分两种语义**。用 `config::IniFile` 做底座（它已支持注释与分节），
// 在其上叠一层轻量的语义，比硬套 `Config` 更直接。
//
// 【两条错误语义（本文件的重点）】
//
//   全局 enabled ：解析失败 -> **关闭**（fail-safe）
//   其余所有项   ：解析失败 -> **保留旧值**（fail-preserve）
//
// 理由见 docs/13 S06：运维改配置的动机往往是"出事了要关掉它"，
// 若因为文件写坏而保留旧值（开着），熔断就失效了。

#ifndef STRATUM_PROBE_PROBE_CONFIG_H_
#define STRATUM_PROBE_PROBE_CONFIG_H_

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace stratum {
namespace probe {

// 单个 topic 的生效设置。全局默认与按 topic 覆盖共用这个结构。
struct TopicSettings {
  bool enabled = true;             // topic 开关（核心诉求）
  std::string format = "kv";       // 信封格式：kv | json | raw
  double sample_rate = 1.0;        // (0, 1]，1 = 不采样
  uint32_t max_rate_hz = 0;        // 0 = 不限
  bool reliable = false;           // DDS 可靠性
  uint32_t depth = 8;              // DDS 历史深度

  bool operator==(const TopicSettings& o) const;
  bool operator!=(const TopicSettings& o) const { return !(*this == o); }
};

// 整份配置
struct ProbeSettings {
  bool global_enabled = true;      // 全局总开关（fail-safe 语义）
  TopicSettings defaults;          // 全局默认（被 topic 段覆盖）
  // 按 topic 的覆盖项；键是探针里的标签（如 "order/submit"）
  std::map<std::string, TopicSettings> topics;

  bool operator==(const ProbeSettings& o) const;
};

// 加载结果。**必须能区分**，否则运维无法判断"为什么没生效"。
enum class LoadOutcome : uint8_t {
  kOk = 0,          // 成功，配置已更新
  kMissing,         // 文件不存在（调用方应生成默认）
  kUnreadable,      // 打开失败（权限 / IO）
  kParseError,      // 语法错误（含非法取值）
  kLockedByEditor,  // .lock 非空，本轮跳过（不视为错误）
};

const char* LoadOutcomeName(LoadOutcome o);

// 一次加载的详细结果（供日志与 status 使用）
struct LoadReport {
  LoadOutcome outcome = LoadOutcome::kMissing;
  std::string detail;        // 人类可读的原因
  bool applied = false;      // 是否真的更新了内存中的配置
  bool global_failsafe_triggered = false;  // 是否因解析失败而全局关闭
};

// ---------------------------------------------------------------------------
// ProbeConfig
//
// 生命周期：一个进程一个实例。线程安全：Load 与 Resolve 可并发调用
// （内部用 swap-on-success，读侧无锁读取不可变快照）。
// ---------------------------------------------------------------------------
class ProbeConfig {
 public:
  ProbeConfig() = default;

  // 生成一份带注释的默认配置文件（PR-05 / S07）。
  //
  // 语义：
  //   · 用 O_EXCL 原子创建 —— 若已存在则**不覆盖**，返回 false 且 errno 语义在 error
  //   · 绝不改写用户文件（S07 的核心裁决）
  //   · 内容含每个可配置项的注释说明
  //
  // known_topics：调用方已见的探针标签，会被写入 `[topic."..."]` 段（默认开启）
  static bool GenerateDefault(const std::string& path,
                              const std::vector<std::string>& known_topics,
                              std::string* error);

  // 加载配置（PR-06/07/08/09）。
  //
  // 错误语义（见文件头）：
  //   · 全局 enabled：解析失败 -> false（fail-safe）
  //   · 其余项：解析失败 -> 保留旧值（fail-preserve）
  //   · .lock 非空且未超时 -> 跳过，内存配置不变
  //
  // lock_timeout_ms：锁的年龄超过它就**强制重载**（S08 的裁决 ——
  // 否则一个崩溃的编辑器会让监控永久冻结）
  LoadReport Load(const std::string& path,
                  std::chrono::milliseconds lock_timeout =
                      std::chrono::milliseconds(5 * 60 * 1000));

  // 取某 topic 的实际生效设置：defaults 被 topics[topic] 覆盖（PR-06）
  TopicSettings Resolve(const std::string& topic) const;

  // 当前快照（不可变视图，供无锁读取）
  ProbeSettings Snapshot() const;

  // 配置里出现了、但业务还没调用过的探针（S04 允许多声明）
  std::vector<std::string> DeclaredTopics() const;

  // 统计：自上次 Load 以来成功/失败次数（供 status 与 tests）
  struct Counters {
    uint64_t loads_ok = 0;
    uint64_t loads_parse_error = 0;
    uint64_t loads_locked_skipped = 0;
    uint64_t loads_failsafe = 0;
    uint64_t lock_forced_reloads = 0;
  };
  Counters counters() const;

 private:
  mutable std::mutex mutex_;
  ProbeSettings settings_;
  Counters counters_;
};

// 解析一个 INI 文本为 ProbeSettings（供测试与 Load 复用）。
//
// 返回 false 表示语法错误或取值非法；error 给出定位信息。
bool ParseSettings(const std::string& text, ProbeSettings* out, std::string* error);

// 把一个 topic 标签转成 INI 里安全的 section 名（处理引号与特殊字符）。
std::string TopicToSection(const std::string& topic);

}  // namespace probe
}  // namespace stratum

#endif  // STRATUM_PROBE_PROBE_CONFIG_H_
