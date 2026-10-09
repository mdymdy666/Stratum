// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// Timeline 实现
//
// 分层职责见 .cc 文件头注释。

#ifndef STRATUM_CORE_TIMELINE_IMPL_H_
#define STRATUM_CORE_TIMELINE_IMPL_H_

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "stratum/core/itimeline.h"
#include "stratum/core/types.h"
#include "stratum/storage/istorage_engine.h"

namespace stratum {

// 支持的磁盘格式版本（读取方遇到更高版本必须拒绝）
constexpr uint32_t kSupportedFormatVersion = 1;

// 生产该 Session 的 Stratum 版本
constexpr const char* kStratumVersionString = "0.1.0";

// ---------------------------------------------------------------------------
// 事件迭代器：把存储层的字节迭代器翻译成 Event 流
// ---------------------------------------------------------------------------
class TimelineEventIterator : public IEventIterator {
 public:
  TimelineEventIterator(
      std::unique_ptr<storage::IStorageEngine::Iterator> storage_iterator,
      const std::map<uint32_t, TopicMetadata>* topics);

  bool Valid() const override { return valid_; }
  void Next() override;
  const Event& event() const override { return current_; }
  Status status() const override { return status_; }

 private:
  void DecodeCurrent();

  std::unique_ptr<storage::IStorageEngine::Iterator> it_;
  const std::map<uint32_t, TopicMetadata>* topics_;
  Event current_;
  bool valid_ = false;
  Status status_;
};

// ---------------------------------------------------------------------------
// 保留点：一个被长期保护的快照
// ---------------------------------------------------------------------------
struct RetentionPoint {
  uint64_t id = 0;
  TimestampNs at_ns = 0;
  storage::SnapshotPtr snapshot;
};

// ---------------------------------------------------------------------------
// TimelineImpl：ITimeline 的默认实现
//
// 依赖一个 IStorageEngine 插件（构造时注入），与具体后端解耦。
// 所有 *Locked 后缀的私有方法都要求调用者已持有 write_mutex_。
// ---------------------------------------------------------------------------
class TimelineImpl : public ITimeline {
 public:
  explicit TimelineImpl(storage::StorageEnginePtr engine)
      : engine_(std::move(engine)) {}

  ~TimelineImpl() override = default;

  // --- ITimeline ---
  Status Open(const std::string& path) override;
  Status Close() override;

  Status RegisterTopic(const TopicMetadata& metadata,
                       uint32_t* topic_id_out) override;
  Status Append(const Event& event) override;
  Status AppendBatch(const std::vector<Event>& events) override;

  Status GetSessionMetadata(SessionMetadata* out) const override;
  Status GetTopicMetadata(uint32_t topic_id, TopicMetadata* out) const override;
  Status ListTopics(std::vector<TopicMetadata>* out) const override;
  Status FindTopicByName(const std::string& name,
                         TopicMetadata* out) const override;

  Status QueryRange(uint32_t topic_id, const TimeRange& range,
                    EventIteratorPtr* out) override;
  Status GetStateAt(uint32_t topic_id, TimestampNs as_of_ns,
                    StateAtResult* out) override;
  Status CountRange(uint32_t topic_id, const TimeRange& range,
                    uint64_t* count_out) override;

  Status CreateRetentionPoint(TimestampNs at_ns,
                              uint64_t* point_id_out) override;
  Status ReleaseRetentionPoint(uint64_t point_id) override;
  Status SetRetentionPolicy(const RetentionPolicy& policy) override;
  RetentionPolicy GetRetentionPolicy() const override;
  Status RunRetention() override;

  Status Flush() override;
  QualityStats GetQualityStats() const override;

  Status ReportDrop(uint64_t count, uint64_t bytes,
                    TimestampNs at_ns) override;

  // --- 诊断（非 ITimeline 接口，供测试与 CLI 使用）---
  size_t RetentionPointCount() const;
  storage::IStorageEngine* engine() { return engine_.get(); }

  // 设置持久性档位
  void SetDurability(RecordConfig::DurabilityLevel level);

 private:
  Status PersistSessionLocked();
  Status PersistTopicLocked(const TopicMetadata& topic);
  Status LoadTopicsLocked();

  void UpdateQualityOnAppend(const Event& event);
  TimestampNs ComputeRetentionCutoffLocked(TimestampNs now) const;
  uint32_t NextSeqInTsLocked(uint32_t topic_id, TimestampNs ts);

  static TimestampNs NowNs() {
    return static_cast<TimestampNs>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
  }
  static storage::SyncPolicy ToSyncPolicy(
      RecordConfig::DurabilityLevel level);
  static std::string NextPrefix(const std::string& prefix);

  storage::StorageEnginePtr engine_;

  mutable std::mutex write_mutex_;
  bool opened_ = false;

  SessionMetadata session_;
  std::map<uint32_t, TopicMetadata> topics_;
  std::unordered_map<std::string, uint32_t> name_to_id_;
  uint32_t next_topic_id_ = 1;

  RetentionPolicy retention_;
  std::map<uint64_t, RetentionPoint> retention_points_;
  uint64_t next_retention_point_id_ = 1;

  QualityStats quality_;
  std::unordered_map<uint32_t, TimestampNs> last_source_ts_;
  std::unordered_map<uint32_t, uint32_t> last_seq_;

  // 同一时间戳内的序号分配：topic_id -> (last_ts, last_seq_in_ts)
  std::unordered_map<uint32_t, std::pair<TimestampNs, uint32_t>> seq_in_ts_state_;

  uint64_t total_events_ = 0;
  uint64_t total_bytes_ = 0;
  uint64_t retained_deletions_ = 0;
  uint64_t pending_meta_updates_ = 0;

  RecordConfig::DurabilityLevel durability_ =
      RecordConfig::DurabilityLevel::kBalanced;

  // 累积多少条事件后更新一次话题元数据
  static constexpr uint64_t kMetaFlushThreshold = 1000;
};

}  // namespace stratum

#endif  // STRATUM_CORE_TIMELINE_IMPL_H_
