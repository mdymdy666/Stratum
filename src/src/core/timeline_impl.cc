// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// Timeline 实现 —— 把"事件时间线"语义映射到"有序 KV"语义
//
// ===========================================================================
// 分层职责（本项目解耦的关键）
// ===========================================================================
//
//   上层 (Recorder / Player / Query)
//        |  只面对 Event 与时间戳，不接触 key
//        v
//   ITimeline  <-- 本文件实现
//        |  唯一知道 TimeKey 编码的地方
//        v
//   IStorageEngine  (插件：memory / lsm / ...)
//        |  只看到"有序字节 KV"
//        v
//   存储后端
//
// ===========================================================================
// 键空间布局
// ===========================================================================
//
//   事件：  <TimeKey(20 字节)>           值 = 原始负载
//   会话：  "META/session"              值 = MetaCodec::EncodeSession
//   话题：  "META/topic/<8 位十六进制>"   值 = MetaCodec::EncodeTopic
//   保留：  "META/retention"            值 = MetaCodec::EncodeRetention
//
// 事件键以 'S'(0x53) 开头（TimeKey magic），元数据键以 'M'(0x4D) 开头，
// 且 'S' < 'M'，因此"全部事件"的范围是 [magic, magic+1)。

#include "stratum/core/timeline_impl.h"

#include <algorithm>

#include "stratum/storage/meta_codec.h"
#include "stratum/storage/time_key.h"

namespace stratum {

using storage::MetaCodec;
using storage::TimeKey;
using storage::TimeKeyCodec;

namespace {
// 事件键的 magic 前缀（4 字节），用于"全部话题"的范围扫描
constexpr size_t kEventMagicSize = 4;
}  // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
Status TimelineImpl::Open(const std::string& path) {
  if (engine_ == nullptr) {
    return Status::InvalidArgument("storage engine is null");
  }

  std::unique_lock<std::mutex> lock(write_mutex_);

  Status s = engine_->Open(path);
  if (!s.ok()) {
    return s;
  }

  storage::ReadOptions ropts;
  std::string raw;

  // 会话元数据：存在则续用，不存在则新建
  s = engine_->Get(ropts, storage::kMetaSessionKey, &raw);
  if (s.ok()) {
    s = MetaCodec::DecodeSession(raw, &session_);
    if (!s.ok()) {
      return s;
    }
    s = LoadTopicsLocked();
    if (!s.ok()) {
      return s;
    }
  } else if (s.IsNotFound()) {
    session_ = SessionMetadata();
    session_.format_version = kSupportedFormatVersion;
    session_.stratum_version = kStratumVersionString;
    session_.created_at_ns = NowNs();
    s = PersistSessionLocked();
    if (!s.ok()) {
      return s;
    }
  } else {
    return s;
  }

  // 保留策略
  s = engine_->Get(ropts, storage::kMetaRetentionKey, &raw);
  if (s.ok()) {
    s = MetaCodec::DecodeRetention(raw, &retention_);
    if (!s.ok()) {
      return s;
    }
  }

  opened_ = true;
  return Status::OK();
}

Status TimelineImpl::Close() {
  std::unique_lock<std::mutex> lock(write_mutex_);
  if (!opened_) {
    return Status::OK();
  }

  session_.closed_at_ns = NowNs();
  session_.total_events = total_events_;
  session_.total_bytes = total_bytes_;
  session_.quality = quality_;

  Status s = PersistSessionLocked();
  Status flush = engine_->Flush();

  opened_ = false;
  if (!s.ok()) return s;
  return flush;
}

Status TimelineImpl::PersistSessionLocked() {
  storage::WriteOptions wopts;
  wopts.sync = ToSyncPolicy(durability_);
  const std::string encoded = MetaCodec::EncodeSession(session_);
  return engine_->Put(wopts, storage::kMetaSessionKey, encoded);
}

Status TimelineImpl::PersistTopicLocked(const TopicMetadata& topic) {
  storage::WriteOptions wopts;
  wopts.sync = ToSyncPolicy(durability_);
  const std::string key = MetaCodec::MakeTopicKey(topic.topic_id);
  const std::string encoded = MetaCodec::EncodeTopic(topic);
  return engine_->Put(wopts, key, encoded);
}

Status TimelineImpl::LoadTopicsLocked() {
  storage::ReadOptions ropts;
  const std::string prefix = storage::kMetaTopicPrefix;
  auto it = engine_->NewIterator(ropts, prefix, NextPrefix(prefix));
  if (!it) {
    return Status::Internal("failed to create metadata iterator");
  }

  topics_.clear();
  name_to_id_.clear();
  uint32_t max_id = 0;

  for (; it->Valid(); it->Next()) {
    TopicMetadata topic;
    // 单条元数据损坏不应导致整个时间线无法打开（对应 EC-13 的降级要求）
    if (!MetaCodec::DecodeTopic(it->value(), &topic).ok()) {
      continue;
    }
    max_id = std::max(max_id, topic.topic_id);
    name_to_id_[topic.topic_name] = topic.topic_id;
    topics_[topic.topic_id] = topic;
  }

  Status s = it->status();
  if (!s.ok()) {
    return s;
  }

  next_topic_id_ = max_id + 1;
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 话题注册
// ---------------------------------------------------------------------------
Status TimelineImpl::RegisterTopic(const TopicMetadata& metadata,
                                   uint32_t* topic_id_out) {
  if (metadata.topic_name.empty()) {
    return Status::InvalidArgument("topic name is empty");
  }

  std::unique_lock<std::mutex> lock(write_mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("timeline not open");
  }

  // 幂等：同名话题复用已有 id（对应 EC-05：话题反复出现不应重复建元数据）
  auto it = name_to_id_.find(metadata.topic_name);
  if (it != name_to_id_.end()) {
    const uint32_t existing_id = it->second;
    TopicMetadata& stored = topics_[existing_id];
    if (stored.type_name.empty() && !metadata.type_name.empty()) {
      stored.type_name = metadata.type_name;
    }
    if (stored.type_definition.empty() && !metadata.type_definition.empty()) {
      stored.type_definition = metadata.type_definition;
    }
    if (stored.qos.empty() && !metadata.qos.empty()) {
      stored.qos = metadata.qos;
    }
    if (topic_id_out != nullptr) {
      *topic_id_out = existing_id;
    }
    return PersistTopicLocked(stored);
  }

  TopicMetadata topic = metadata;
  topic.topic_id = next_topic_id_++;
  topic.message_count = 0;
  topic.byte_count = 0;
  topic.first_seen_ns = kInvalidTimestamp;
  topic.last_seen_ns = kInvalidTimestamp;
  topic.active = true;

  Status s = PersistTopicLocked(topic);
  if (!s.ok()) {
    return s;
  }

  name_to_id_[topic.topic_name] = topic.topic_id;
  topics_[topic.topic_id] = topic;

  session_.topics.push_back(topic);
  s = PersistSessionLocked();
  if (!s.ok()) {
    return s;
  }

  if (topic_id_out != nullptr) {
    *topic_id_out = topic.topic_id;
  }
  return Status::OK();
}

Status TimelineImpl::FindTopicByName(const std::string& name,
                                     TopicMetadata* out) const {
  std::lock_guard<std::mutex> lock(write_mutex_);
  auto it = name_to_id_.find(name);
  if (it == name_to_id_.end()) {
    return Status::NotFound("topic not found: " + name);
  }
  auto tit = topics_.find(it->second);
  if (tit == topics_.end()) {
    return Status::NotFound("topic metadata missing: " + name);
  }
  if (out != nullptr) {
    *out = tit->second;
  }
  return Status::OK();
}

Status TimelineImpl::GetTopicMetadata(uint32_t topic_id,
                                      TopicMetadata* out) const {
  std::lock_guard<std::mutex> lock(write_mutex_);
  auto it = topics_.find(topic_id);
  if (it == topics_.end()) {
    return Status::NotFound("topic id not found: " + std::to_string(topic_id));
  }
  if (out != nullptr) {
    *out = it->second;
  }
  return Status::OK();
}

Status TimelineImpl::ListTopics(std::vector<TopicMetadata>* out) const {
  if (out == nullptr) {
    return Status::InvalidArgument("out is null");
  }
  std::lock_guard<std::mutex> lock(write_mutex_);
  out->clear();
  out->reserve(topics_.size());
  for (const auto& kv : topics_) {  // std::map：按 topic_id 有序
    out->push_back(kv.second);
  }
  return Status::OK();
}

Status TimelineImpl::GetSessionMetadata(SessionMetadata* out) const {
  if (out == nullptr) {
    return Status::InvalidArgument("out is null");
  }
  std::lock_guard<std::mutex> lock(write_mutex_);
  SessionMetadata snapshot = session_;
  snapshot.total_events = total_events_;
  snapshot.total_bytes = total_bytes_;
  snapshot.quality = quality_;
  snapshot.topics.clear();
  for (const auto& kv : topics_) {
    snapshot.topics.push_back(kv.second);
  }
  *out = std::move(snapshot);
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 写入
// ---------------------------------------------------------------------------
Status TimelineImpl::Append(const Event& event) {
  std::vector<Event> one;
  one.push_back(event);
  return AppendBatch(one);
}

Status TimelineImpl::AppendBatch(const std::vector<Event>& events) {
  if (events.empty()) {
    return Status::OK();
  }

  std::unique_lock<std::mutex> lock(write_mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("timeline not open");
  }

  std::vector<std::string> keys;
  std::vector<std::string> values;
  keys.reserve(events.size());
  values.reserve(events.size());

  for (const Event& e : events) {
    auto tit = topics_.find(e.topic_id);
    if (tit == topics_.end()) {
      return Status::InvalidArgument("unregistered topic id: " +
                                     std::to_string(e.topic_id));
    }

    // 同一时间戳内的去重序号：调用方未指定时自动分配
    uint32_t seq_in_ts = e.seq_in_ts;
    if (seq_in_ts == 0) {
      seq_in_ts = NextSeqInTsLocked(e.topic_id, e.source_timestamp_ns);
    }

    keys.push_back(
        TimeKeyCodec::Encode(e.topic_id, e.source_timestamp_ns, seq_in_ts));
    values.push_back(e.payload);

    UpdateQualityOnAppend(e);

    TopicMetadata& topic = tit->second;
    topic.message_count += 1;
    topic.byte_count += e.payload.size();
    if (topic.first_seen_ns == kInvalidTimestamp ||
        e.source_timestamp_ns < topic.first_seen_ns) {
      topic.first_seen_ns = e.source_timestamp_ns;
    }
    if (e.source_timestamp_ns > topic.last_seen_ns) {
      topic.last_seen_ns = e.source_timestamp_ns;
    }

    total_events_ += 1;
    total_bytes_ += e.payload.size();
  }

  storage::WriteOptions wopts;
  wopts.sync = ToSyncPolicy(durability_);
  Status s = engine_->PutBatch(wopts, keys, values);
  if (!s.ok()) {
    return s;
  }

  // 元数据落盘节流：避免每条消息都写元数据（这对 LSM 是显著开销）
  pending_meta_updates_ += events.size();
  if (pending_meta_updates_ >= kMetaFlushThreshold) {
    for (auto& kv : topics_) {
      kv.second.active = true;
    }
    pending_meta_updates_ = 0;
  }

  return Status::OK();
}

uint32_t TimelineImpl::NextSeqInTsLocked(uint32_t topic_id, TimestampNs ts) {
  // seq_in_ts 的作用：让**同一 (topic, timestamp)** 的多条消息拥有不同的 key。
  // index 的排序以 timestamp 为主序、seq_in_ts 为次序，因此只要保证唯一即可。
  //
  // 早期实现是"时间戳变化就重置为 0"，这在**时间戳乱序**时会与历史 key 冲突：
  //   序列 A, A, B, A 的第 4 条会重新拿到 seq=0，与第 1 条 key 完全相同，
  //   于是第 1 条被静默覆盖 —— 表现为"写入 N 条、只能读回 N-1 条"。
  //   该场景在真实系统中很常见（设备重连回补、多源时钟不同步）。
  //
  // 现改为**每个 topic 内单调递增**：
  //   - 唯一性：同一 topic 内永不重复，天然满足
  //   - 顺序性：index 以 timestamp 为主序，seq 递增不破坏时间顺序
  //
  // 已知限制：seq_in_ts 计数器未持久化，重开时间线后从 0 重新计数，
  //   若继续写入**同一时间戳**的数据仍可能冲突。见 design/05-known-gaps.md。
  (void)ts;
  auto& counter = seq_in_ts_state_[topic_id].second;
  return counter++;
}

void TimelineImpl::UpdateQualityOnAppend(const Event& event) {
  // --- 时间戳倒退（EC-01）---
  auto it = last_source_ts_.find(event.topic_id);
  if (it != last_source_ts_.end() &&
      event.source_timestamp_ns < it->second) {
    quality_.timestamp_regressions += 1;
  }
  last_source_ts_[event.topic_id] = event.source_timestamp_ns;

  // --- 序号缺口（暗示发布者侧丢失，RC-3）---
  auto sit = last_seq_.find(event.topic_id);
  if (sit != last_seq_.end()) {
    const uint32_t expected = sit->second + 1;
    if (event.seq > expected) {
      quality_.sequence_gaps += 1;
      quality_.missing_messages += (event.seq - expected);
    }
  }
  last_seq_[event.topic_id] = event.seq;

  // --- 接收延迟（源时间戳 vs 接收时间戳）---
  if (event.reception_timestamp_ns != 0 && event.source_timestamp_ns != 0) {
    const int64_t latency =
        static_cast<int64_t>(event.reception_timestamp_ns) -
        static_cast<int64_t>(event.source_timestamp_ns);
    if (quality_.latency_samples == 0) {
      quality_.min_latency_ns = latency;
      quality_.max_latency_ns = latency;
    } else {
      quality_.min_latency_ns = std::min(quality_.min_latency_ns, latency);
      quality_.max_latency_ns = std::max(quality_.max_latency_ns, latency);
    }
    quality_.total_latency_ns += latency;
    quality_.latency_samples += 1;
  }
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------
Status TimelineImpl::QueryRange(uint32_t topic_id, const TimeRange& range,
                                EventIteratorPtr* out) {
  if (out == nullptr) {
    return Status::InvalidArgument("out is null");
  }

  std::lock_guard<std::mutex> lock(write_mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("timeline not open");
  }

  std::string begin;
  std::string end;

  if (topic_id == 0) {
    // 全部话题：事件键以 magic 开头，元数据键以 'M' 开头且 'S' < 'M'，
    // 故 [magic, magic+1) 恰好覆盖全部事件键。
    begin = TimeKeyCodec::Encode(0, 0, 0).substr(0, kEventMagicSize);
    end = begin;
    end[kEventMagicSize - 1] = static_cast<char>(end[kEventMagicSize - 1] + 1);
  } else {
    TimeKeyCodec::MakeTimeRange(topic_id, range.begin_ns, range.end_ns, &begin,
                                &end);
  }

  storage::ReadOptions ropts;
  auto storage_it = engine_->NewIterator(ropts, begin, end);
  if (!storage_it) {
    return Status::Internal("failed to create iterator");
  }

  *out = EventIteratorPtr(
      new TimelineEventIterator(std::move(storage_it), &topics_));
  return Status::OK();
}

Status TimelineImpl::GetStateAt(uint32_t topic_id, TimestampNs as_of_ns,
                                StateAtResult* out) {
  if (out == nullptr) {
    return Status::InvalidArgument("out is null");
  }

  std::lock_guard<std::mutex> lock(write_mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("timeline not open");
  }

  auto tit = topics_.find(topic_id);
  if (tit == topics_.end()) {
    out->kind = StateAtResult::Kind::kNotFound;
    return Status::OK();
  }

  const TopicMetadata& topic = tit->second;

  // 时刻早于该话题首次出现：明确返回 out_of_range，不猜测（FR-Q-3）
  if (topic.first_seen_ns != kInvalidTimestamp &&
      as_of_ns < topic.first_seen_ns) {
    out->kind = StateAtResult::Kind::kOutOfRange;
    out->as_of_ns = as_of_ns;
    return Status::OK();
  }

  std::string begin;
  std::string end;
  TimeKeyCodec::MakePrefixRangeUpTo(topic_id, as_of_ns, &begin, &end);

  storage::ReadOptions ropts;
  auto it = engine_->NewIterator(ropts, begin, end);
  if (!it) {
    return Status::Internal("failed to create iterator");
  }

  // 正向扫描取最后一条可见记录。
  //
  // 为什么不用反向迭代（SeekToLast）：
  //   反向迭代依赖后端对 Prev()/SeekToLast() 的实现质量，且语义更易出错；
  //   而"正向扫描到末尾取最后一条"只依赖最基本的 Next() 顺序遍历，
  //   在键有序的前提下结果等价。对于"查某时刻状态"这种低频操作，
  //   扫描成本可接受（且可被上层缓存）。
  Event last_event;
  bool found = false;
  for (; it->Valid(); it->Next()) {
    TimeKey key;
    if (!TimeKeyCodec::Decode(it->key(), &key)) {
      return Status::Corruption("event key format invalid");
    }
    last_event.topic_id = key.topic_id;
    last_event.source_timestamp_ns = key.timestamp_ns;
    last_event.seq_in_ts = key.seq_in_ts;
    last_event.payload = it->value();
    found = true;
  }
  Status iter_status = it->status();
  if (!iter_status.ok()) {
    return iter_status;
  }

  if (!found) {
    out->kind = StateAtResult::Kind::kNotFound;
    return Status::OK();
  }

  out->kind = StateAtResult::Kind::kFound;
  out->as_of_ns = last_event.source_timestamp_ns;
  out->event = std::move(last_event);
  return Status::OK();
}
Status TimelineImpl::CountRange(uint32_t topic_id, const TimeRange& range,
                                uint64_t* count_out) {
  if (count_out == nullptr) {
    return Status::InvalidArgument("count_out is null");
  }

  EventIteratorPtr it;
  Status s = QueryRange(topic_id, range, &it);
  if (!s.ok()) {
    return s;
  }

  uint64_t count = 0;
  for (; it->Valid(); it->Next()) {
    ++count;
  }
  s = it->status();
  if (!s.ok()) {
    return s;
  }
  *count_out = count;
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 保留策略
// ---------------------------------------------------------------------------
Status TimelineImpl::CreateRetentionPoint(TimestampNs at_ns,
                                          uint64_t* point_id_out) {
  std::unique_lock<std::mutex> lock(write_mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("timeline not open");
  }

  auto snapshot = engine_->GetSnapshot();
  if (!snapshot) {
    return Status::NotSupported("storage engine does not support snapshots");
  }

  const uint64_t id = next_retention_point_id_++;
  RetentionPoint point;
  point.id = id;
  point.at_ns = at_ns;
  point.snapshot = snapshot;
  retention_points_[id] = point;

  if (point_id_out != nullptr) {
    *point_id_out = id;
  }
  return Status::OK();
}

Status TimelineImpl::ReleaseRetentionPoint(uint64_t point_id) {
  std::unique_lock<std::mutex> lock(write_mutex_);
  auto it = retention_points_.find(point_id);
  if (it == retention_points_.end()) {
    return Status::NotFound("retention point not found: " +
                            std::to_string(point_id));
  }
  engine_->ReleaseSnapshot(it->second.snapshot);
  retention_points_.erase(it);
  return Status::OK();
}

Status TimelineImpl::SetRetentionPolicy(const RetentionPolicy& policy) {
  std::unique_lock<std::mutex> lock(write_mutex_);
  retention_ = policy;

  storage::WriteOptions wopts;
  wopts.sync = ToSyncPolicy(durability_);
  return engine_->Put(wopts, storage::kMetaRetentionKey,
                      MetaCodec::EncodeRetention(policy));
}

RetentionPolicy TimelineImpl::GetRetentionPolicy() const {
  std::lock_guard<std::mutex> lock(write_mutex_);
  return retention_;
}

TimestampNs TimelineImpl::ComputeRetentionCutoffLocked(TimestampNs now) const {
  TimestampNs cutoff = 0;
  if (retention_.retain_duration_ns > 0 && now > retention_.retain_duration_ns) {
    cutoff = now - retention_.retain_duration_ns;
  }
  // 保留点保护：被保留点覆盖的时刻之后的数据不移除
  for (const auto& kv : retention_points_) {
    if (kv.second.at_ns > 0 && (cutoff == 0 || kv.second.at_ns < cutoff)) {
      cutoff = kv.second.at_ns;
    }
  }
  return cutoff;
}

Status TimelineImpl::RunRetention() {
  std::unique_lock<std::mutex> lock(write_mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("timeline not open");
  }
  if (retention_.retain_duration_ns == 0 && retention_.max_bytes == 0) {
    return Status::OK();  // 未配置保留策略
  }

  const TimestampNs cutoff = ComputeRetentionCutoffLocked(NowNs());
  if (cutoff == 0) {
    return Status::OK();
  }

  storage::WriteOptions wopts;
  wopts.sync = ToSyncPolicy(durability_);

  for (auto& kv : topics_) {
    const uint32_t topic_id = kv.first;
    TopicMetadata& topic = kv.second;

    if (topic.first_seen_ns != kInvalidTimestamp &&
        topic.first_seen_ns >= cutoff) {
      continue;  // 该话题数据全在保留窗口内
    }

    // 删除 [话题起点, cutoff)：利用 key 有序性，一次范围删除
    const std::string begin = TimeKeyCodec::Encode(topic_id, 0, 0);
    const std::string end = TimeKeyCodec::Encode(topic_id, cutoff, 0);

    Status s = engine_->DeleteRange(wopts, begin, end);
    if (!s.ok() && !s.IsNotFound()) {
      return s;
    }
    if (s.ok()) {
      retained_deletions_ += 1;
      topic.first_seen_ns = cutoff;
    }
  }

  return Status::OK();
}

// ---------------------------------------------------------------------------
// 运维
// ---------------------------------------------------------------------------
Status TimelineImpl::Flush() {
  std::unique_lock<std::mutex> lock(write_mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("timeline not open");
  }

  for (auto& kv : topics_) {
    Status s = PersistTopicLocked(kv.second);
    if (!s.ok()) {
      return s;
    }
  }

  session_.total_events = total_events_;
  session_.total_bytes = total_bytes_;
  session_.quality = quality_;

  Status s = PersistSessionLocked();
  if (!s.ok()) {
    return s;
  }
  return engine_->Flush();
}

Status TimelineImpl::ReportDrop(uint64_t count, uint64_t bytes,
                                TimestampNs at_ns) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  quality_.dropped_messages += count;
  quality_.dropped_bytes += bytes;
  if (quality_.first_drop_ns == kInvalidTimestamp) {
    quality_.first_drop_ns = at_ns;
  }
  quality_.last_drop_ns = at_ns;
  return Status::OK();
}

QualityStats TimelineImpl::GetQualityStats() const {
  std::lock_guard<std::mutex> lock(write_mutex_);
  return quality_;
}

void TimelineImpl::SetDurability(RecordConfig::DurabilityLevel level) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  durability_ = level;
}

size_t TimelineImpl::RetentionPointCount() const {
  std::lock_guard<std::mutex> lock(write_mutex_);
  return retention_points_.size();
}

// ---------------------------------------------------------------------------
// 静态工具
// ---------------------------------------------------------------------------
storage::SyncPolicy TimelineImpl::ToSyncPolicy(
    RecordConfig::DurabilityLevel level) {
  switch (level) {
    case RecordConfig::DurabilityLevel::kRelaxed:
      return storage::SyncPolicy::kNone;
    case RecordConfig::DurabilityLevel::kStrict:
      return storage::SyncPolicy::kAlways;
    case RecordConfig::DurabilityLevel::kBalanced:
    default:
      return storage::SyncPolicy::kBatch;
  }
}

// 前缀上界：把最后一个字节 +1，用于"以某前缀开头的所有键"的范围扫描
std::string TimelineImpl::NextPrefix(const std::string& prefix) {
  std::string result = prefix;
  for (size_t i = result.size(); i > 0; --i) {
    const unsigned char c = static_cast<unsigned char>(result[i - 1]);
    if (c < 0xFF) {
      result[i - 1] = static_cast<char>(c + 1);
      result.resize(i);
      return result;
    }
  }
  return std::string();  // 全 0xFF：无上界
}

// ---------------------------------------------------------------------------
// TimelineEventIterator
// ---------------------------------------------------------------------------
TimelineEventIterator::TimelineEventIterator(
    std::unique_ptr<storage::IStorageEngine::Iterator> storage_iterator,
    const std::map<uint32_t, TopicMetadata>* topics)
    : it_(std::move(storage_iterator)), topics_(topics) {
  (void)topics_;  // 保留以便未来做话题级过滤
  DecodeCurrent();
}

void TimelineEventIterator::Next() {
  if (!valid_) {
    return;
  }
  it_->Next();
  DecodeCurrent();
}

void TimelineEventIterator::DecodeCurrent() {
  valid_ = false;
  if (!it_) {
    status_ = Status::Internal("null iterator");
    return;
  }
  if (!it_->Valid()) {
    status_ = it_->status();
    return;
  }

  TimeKey key;
  if (!TimeKeyCodec::Decode(it_->key(), &key)) {
    status_ = Status::Corruption("event key format invalid");
    return;
  }

  current_ = Event();
  current_.topic_id = key.topic_id;
  current_.source_timestamp_ns = key.timestamp_ns;
  current_.seq_in_ts = key.seq_in_ts;
  current_.payload = it_->value();
  valid_ = true;
}

}  // namespace stratum
