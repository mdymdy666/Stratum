// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 时间线（Timeline）接口 —— Stratum 的业务核心抽象
//
// ===========================================================================
// 为什么需要这一层
// ===========================================================================
//
// 存储层（IStorageEngine）只懂"有序字节 KV"。它不知道什么是话题、什么是事件。
// Timeline 层负责把"时间序列语义"映射到"KV 语义"：
//
//     Append(event)            → 编码成 TimeKey + payload 写入 KV
//     QueryRange(topic, from, to) → 构造 key 区间做范围扫描
//     GetStateAt(topic, ts)    → 用快照 + 反向扫描取"某时刻的状态"
//
// 这一层是**唯一**知道 TimeKey 编码的地方。上层（Recorder/Player/Query）
// 只面对 Event 与时间戳，不接触 key。
//
// ===========================================================================
// 与存储后端的关系
// ===========================================================================
//
// Timeline 依赖 IStorageEngine 的抽象，不依赖任何具体后端。
// 因此"换成 LSMengine / 自研 LSM / 内存实现"对上层完全透明。
// 这是插件式架构的关键隔离点。

#ifndef STRATUM_CORE_ITIMELINE_H_
#define STRATUM_CORE_ITIMELINE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "stratum/core/status.h"
#include "stratum/core/types.h"

namespace stratum {

// ---------------------------------------------------------------------------
// 时间范围（闭开区间 [begin, end)）
// ---------------------------------------------------------------------------
struct TimeRange {
  TimestampNs begin_ns = kInvalidTimestamp;
  TimestampNs end_ns = kInvalidTimestamp;  // 0 表示无上界

  bool Empty() const {
    return end_ns != kInvalidTimestamp && begin_ns >= end_ns;
  }
};

// ---------------------------------------------------------------------------
// 迭代器抽象
//
// 刻意不暴露存储层的 Iterator，避免上层依赖后端类型。
// ---------------------------------------------------------------------------
class IEventIterator {
 public:
  virtual ~IEventIterator() = default;

  virtual bool Valid() const = 0;
  virtual void Next() = 0;

  // 当前事件（仅在 Valid() 为 true 时有效）
  virtual const Event& event() const = 0;

  // 迭代过程中的错误（非 OK 表示结果不完整）
  virtual Status status() const = 0;
};

using EventIteratorPtr = std::unique_ptr<IEventIterator>;

// ---------------------------------------------------------------------------
// 查询结果：某时刻的状态
// ---------------------------------------------------------------------------
struct StateAtResult {
  enum class Kind : uint8_t {
    kNotFound,   // 该时刻之前没有任何数据
    kFound,      // 找到（value 有效）
    kDisposed,   // 该时刻之前被注销（dispose）
    kOutOfRange  // 请求的时刻超出录制范围
  };

  Kind kind = Kind::kNotFound;
  Event event;                 // kind == kFound 时有效
  TimestampNs as_of_ns = 0;    // 实际命中的时间戳（可能早于请求时刻）
};

// ---------------------------------------------------------------------------
// 时间线接口
//
// 线程安全：
//   - Append 可多线程调用（内部串行化）
//   - 查询类方法可并发
// ---------------------------------------------------------------------------
class ITimeline {
 public:
  virtual ~ITimeline() = default;

  // --- 生命周期 -----------------------------------------------------------

  // 打开（或创建）时间线。
  virtual Status Open(const std::string& path) = 0;
  virtual Status Close() = 0;

  // --- 写入路径 -----------------------------------------------------------

  // 注册一个话题，返回分配的 topic_id。
  // 同名话题重复注册返回同一个 id（幂等）。
  virtual Status RegisterTopic(const TopicMetadata& metadata,
                               uint32_t* topic_id_out) = 0;

  // 追加一个事件。实现负责分配 seq_in_ts 并编码 key。
  virtual Status Append(const Event& event) = 0;

  // 批量追加（录制器的主力路径，必须高效）
  virtual Status AppendBatch(const std::vector<Event>& events) = 0;

  // --- 元数据 -------------------------------------------------------------

  virtual Status GetSessionMetadata(SessionMetadata* out) const = 0;
  virtual Status GetTopicMetadata(uint32_t topic_id, TopicMetadata* out) const = 0;
  virtual Status ListTopics(std::vector<TopicMetadata>* out) const = 0;
  virtual Status FindTopicByName(const std::string& name,
                                 TopicMetadata* out) const = 0;

  // --- 查询路径 -----------------------------------------------------------

  // 按时间范围读取事件。topic_id == 0 表示"全部话题"。
  virtual Status QueryRange(uint32_t topic_id, const TimeRange& range,
                            EventIteratorPtr* out) = 0;

  // 定点查询：topic 在 as_of_ns 时刻的状态。
  virtual Status GetStateAt(uint32_t topic_id, TimestampNs as_of_ns,
                            StateAtResult* out) = 0;

  // 统计某范围内的消息数（不返回内容，用于报告）
  virtual Status CountRange(uint32_t topic_id, const TimeRange& range,
                            uint64_t* count_out) = 0;

  // --- 快照与保留 ---------------------------------------------------------

  // 创建一个"保留点"：此后其之前的数据不会被保留策略淘汰。
  // 返回保留点标识（可用于后续释放）。
  virtual Status CreateRetentionPoint(TimestampNs at_ns,
                                      uint64_t* point_id_out) = 0;

  virtual Status ReleaseRetentionPoint(uint64_t point_id) = 0;

  // 设置/更新保留策略
  virtual Status SetRetentionPolicy(const RetentionPolicy& policy) = 0;
  virtual RetentionPolicy GetRetentionPolicy() const = 0;

  // 执行一次淘汰（通常由后台线程周期调用）
  virtual Status RunRetention() = 0;

  // --- 运维 ---------------------------------------------------------------

  virtual Status Flush() = 0;

  // 录制质量统计（实时累积）
  virtual QualityStats GetQualityStats() const = 0;

  // 报告录制器丢弃的样本（N-1 修复）。
  //
  // 早期实现 Recorder 的 dropped 计数只加到自己的原子量，
  // QualityStats::dropped_messages 全代码库无人写入 ——
  // 结果丢弃 96% 数据后质量报告仍输出"可信度高"。
  // 本方法把丢弃事件汇入 Timeline 的质量统计。
  virtual Status ReportDrop(uint64_t count, uint64_t bytes,
                            TimestampNs at_ns) = 0;
};

using TimelinePtr = std::unique_ptr<ITimeline>;

}  // namespace stratum

#endif  // STRATUM_CORE_ITIMELINE_H_
