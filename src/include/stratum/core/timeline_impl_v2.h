// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// TimelineImplV2：基于 StratumDB（LSMengine 架构）的 ITimeline 实现
//
// 这是 v2 接入上层的适配层：让 Recorder/Player/Query 无感知地跑在
// LSMengine 之上，兑现"插件式架构"的承诺（现有上层代码零修改）。

#ifndef STRATUM_CORE_TIMELINE_IMPL_V2_H_
#define STRATUM_CORE_TIMELINE_IMPL_V2_H_

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "stratum/core/itimeline.h"
#include "stratum/storage/lsm_v2/stratum_db.h"

namespace stratum {

// 物化迭代器：把 StratumDB 的范围查询结果包装为 IEventIterator。
class TimelineV2Iterator : public IEventIterator {
 public:
  explicit TimelineV2Iterator(std::vector<Event> events, Status status)
      : events_(std::move(events)), status_(std::move(status)) {}

  bool Valid() const override { return pos_ < events_.size(); }
  void Next() override {
    if (Valid()) ++pos_;
  }
  const Event& event() const override { return events_[pos_]; }
  Status status() const override { return status_; }

 private:
  std::vector<Event> events_;
  size_t pos_ = 0;
  Status status_;
};

class TimelineImplV2 : public ITimeline {
 public:
  TimelineImplV2() = default;
  ~TimelineImplV2() override;

  TimelineImplV2(const TimelineImplV2&) = delete;
  TimelineImplV2& operator=(const TimelineImplV2&) = delete;

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
  Status DeleteRange(uint32_t topic_id, const TimeRange& range,
                     uint64_t* deleted_events = nullptr,
                     uint64_t* deleted_bytes = nullptr) override;
  Status ListPrunedRanges(uint32_t topic_id,
                         std::vector<PrunedRange>* out) override;
  Status SetTopicTypeDefinition(const std::string& topic_name,
                               const std::string& definition) override;
  Status GetTopicTypeDefinition(const std::string& topic_name,
                               std::string* definition) override;

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

 private:
  // 追加时更新质量统计（时间戳倒退/序号缺口/延迟），复用 v1 语义
  void UpdateQualityOnAppend(const Event& e);
  // 加载话题元数据（注册信息 + 统计）
  Status LoadTopicsLocked();
  // 读取会话级元数据（created/closed/total_*）。
  // 与 PersistSessionLocked 配对；缺失它会导致重开后会话时间戳归零。
  Status LoadSessionLocked();
  // 持久化话题统计（first/last/count/bytes）
  Status PersistTopicStatsLocked(uint32_t topic_id, const TopicMetadata& meta);
  // 持久化会话元数据
  Status PersistSessionLocked();
  // 持久化/恢复保留点列表（P4：跨重启）
  Status PersistRetentionPointsLocked();
  Status LoadRetentionPointsLocked();

  std::unique_ptr<storage::lsm_v2::StratumDB> db_;
  std::string path_;
  bool opened_ = false;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, uint32_t> topic_ids_;
  std::unordered_map<uint32_t, TopicMetadata> topic_meta_;  // id → 完整元数据

  // 保留点：id → (at_ns, 底层快照)
  uint64_t next_retention_point_id_ = 1;
  std::map<uint64_t,
           std::pair<TimestampNs,
                     std::unique_ptr<storage::lsm_v2::RetentionPoint>>>
      retention_points_;

  RetentionPolicy retention_;
  QualityStats quality_;

  // 质量统计辅助状态
  std::unordered_map<uint32_t, TimestampNs> last_source_ts_;
  std::unordered_map<uint32_t, uint32_t> last_seq_;

  // 会话统计
  uint64_t total_events_ = 0;
  uint64_t total_bytes_ = 0;
  TimestampNs session_start_ns_ = kInvalidTimestamp;
  TimestampNs session_end_ns_ = kInvalidTimestamp;
};

}  // namespace stratum

#endif  // STRATUM_CORE_TIMELINE_IMPL_V2_H_
