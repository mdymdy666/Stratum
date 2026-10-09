// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// Stratum 核心数据模型
//
// 这些类型是"存储了什么"的唯一定义。所有层（录制/回放/查询/CLI）共用。
// 修改这里等于修改磁盘格式契约，必须同步版本号。

#ifndef STRATUM_CORE_TYPES_H_
#define STRATUM_CORE_TYPES_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace stratum {

// ---------------------------------------------------------------------------
// 时间戳
//
// Stratum 区分两个时间戳，这是数据可信度的基础（见 docs/06-data-trust.md）：
//   - source：发布者打的时间。回放用它保证时序保真。
//   - reception：录制器收到的本地时刻。用于诊断录制延迟。
// 两者都必须保存，不能只留一个。
// ---------------------------------------------------------------------------
using TimestampNs = uint64_t;

// 用于表示"未设置"
constexpr TimestampNs kInvalidTimestamp = 0;

// ---------------------------------------------------------------------------
// 话题元数据
//
// 在 Session 中每个话题记录一条。必须自描述：脱离原程序也能读懂。
// ---------------------------------------------------------------------------
struct TopicMetadata {
  // 在 Session 内稳定的话题编号。写入 key 的是它，不是名字（省空间且定长）。
  uint32_t topic_id = 0;

  std::string topic_name;
  std::string type_name;

  // 类型定义（可选）。v1 允许为空——此时只能按原始字节回放。
  // 见 docs/03-open-questions.md 的 Q11。
  std::string type_definition;

  // QoS 快照：策略名 → 值（已字符串化，便于跨版本兼容）
  // 见 docs/01-requirements.md 的 FR-R-5。
  std::map<std::string, std::string> qos;

  // 该话题在本次录制中的时间范围
  TimestampNs first_seen_ns = kInvalidTimestamp;
  TimestampNs last_seen_ns = kInvalidTimestamp;

  // 统计（用于质量报告）
  uint64_t message_count = 0;
  uint64_t byte_count = 0;

  bool active = false;
};

// ---------------------------------------------------------------------------
// 一条被录制的事件（消息）
// ---------------------------------------------------------------------------
struct Event {
  uint32_t topic_id = 0;

  // 同一话题内单调递增（由 Recorder 分配）
  uint32_t seq = 0;

  // 同一时间戳内的去重序号（由 Recorder 分配，见 TimeKey 设计）
  uint32_t seq_in_ts = 0;

  TimestampNs source_timestamp_ns = kInvalidTimestamp;
  TimestampNs reception_timestamp_ns = kInvalidTimestamp;

  // 原始序列化字节。存储层不理解其内容。
  std::string payload;

  // 用于 keyed 话题的实例标识（空表示非 keyed）
  std::string instance_key;
};

// ---------------------------------------------------------------------------
// 录制质量统计（对应 docs/06-data-trust.md）
//
// 这是"数据可信度"的唯一凭据。每个字段都必须在录制过程中持续累积，
// 而不是事后计算。
// ---------------------------------------------------------------------------
struct QualityStats {
  // --- 丢弃（录制器自身造成的数据缺失）---
  uint64_t dropped_messages = 0;
  uint64_t dropped_bytes = 0;
  TimestampNs first_drop_ns = kInvalidTimestamp;
  TimestampNs last_drop_ns = kInvalidTimestamp;

  // --- 时间戳异常 ---
  uint64_t timestamp_regressions = 0;
  uint64_t clock_jumps = 0;

  // --- 序号缺口（暗示发布者侧丢失）---
  uint64_t sequence_gaps = 0;
  uint64_t missing_messages = 0;

  // --- 录制中断 ---
  uint64_t interruptions = 0;

  // 源时间戳与接收时间戳偏差统计
  int64_t min_latency_ns = 0;
  int64_t max_latency_ns = 0;
  int64_t total_latency_ns = 0;
  uint64_t latency_samples = 0;

  double AverageLatencyNs() const {
    return latency_samples == 0
               ? 0.0
               : static_cast<double>(total_latency_ns) /
                     static_cast<double>(latency_samples);
  }

  bool HasIntegrityIssue() const {
    return dropped_messages > 0 || timestamp_regressions > 0 ||
           sequence_gaps > 0 || interruptions > 0;
  }
};

// ---------------------------------------------------------------------------
// Session 元信息
// ---------------------------------------------------------------------------
struct SessionMetadata {
  // 格式版本。读取方遇到更高版本必须拒绝（见 FR-S-2）。
  uint32_t format_version = 1;

  std::string stratum_version;
  std::string session_name;

  TimestampNs created_at_ns = kInvalidTimestamp;
  TimestampNs closed_at_ns = kInvalidTimestamp;

  std::vector<TopicMetadata> topics;

  uint64_t total_events = 0;
  uint64_t total_bytes = 0;

  QualityStats quality;
};

// ---------------------------------------------------------------------------
// 保留策略（环形保留，见 FR-R-7）
// ---------------------------------------------------------------------------
struct RetentionPolicy {
  uint64_t retain_duration_ns = 0;  // 0 = 不按时间限制
  uint64_t max_bytes = 0;           // 0 = 不按空间限制
  uint64_t check_interval_ns = 1000000000ULL;  // 默认 1 秒检查一次
};

// ---------------------------------------------------------------------------
// 分片策略（见 FR-R-8）
// ---------------------------------------------------------------------------
struct SplitPolicy {
  uint64_t max_layer_bytes = 256ULL * 1024 * 1024;  // 默认 256MB
  uint64_t max_layer_duration_ns = 0;
  uint64_t max_layer_events = 0;
};

// ---------------------------------------------------------------------------
// 录制配置
// ---------------------------------------------------------------------------
struct RecordConfig {
  // 要录制的话题。空表示"全部"（自动发现模式）。
  // 支持精确匹配与前缀通配（以 '*' 结尾）。
  std::vector<std::string> topics;

  RetentionPolicy retention;
  SplitPolicy split;

  // 内存缓冲上限（字节）。超过则触发丢弃策略。
  uint64_t max_buffer_bytes = 256ULL * 1024 * 1024;

  enum class OverflowPolicy {
    kDropNewest,  // 丢弃新数据（保持业务不受影响）—— 默认
    kDropOldest,  // 丢弃最旧（保持"最近的"）
    kBlock,       // 阻塞等待（保证不丢，但可能影响业务）
  };
  OverflowPolicy overflow_policy = OverflowPolicy::kDropNewest;

  enum class DurabilityLevel {
    kRelaxed,   // 异步写
    kBalanced,  // 批量同步（默认）
    kStrict,    // 每条同步
  };
  DurabilityLevel durability = DurabilityLevel::kBalanced;
};

}  // namespace stratum

#endif  // STRATUM_CORE_TYPES_H_
