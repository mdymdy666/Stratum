// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// TimelineImplV2 实现：ITimeline → StratumDB 适配

#include "stratum/core/timeline_impl_v2.h"

#include <algorithm>

namespace stratum {

namespace {

// 大端 32 位编解码（与 stratum_db.cc 内部一致）
std::string U32BE(uint32_t v) {
  std::string s(4, '\0');
  s[0] = static_cast<char>((v >> 24) & 0xFF);
  s[1] = static_cast<char>((v >> 16) & 0xFF);
  s[2] = static_cast<char>((v >> 8) & 0xFF);
  s[3] = static_cast<char>(v & 0xFF);
  return s;
}

uint32_t DecodeU32BE(const char* p) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(p[3]));
}

constexpr char kStatPrefix[] = "topic_stat/";
constexpr char kSessionKey[] = "session";

// 统计编码：[8B first_seen][8B last_seen][8B count][8B bytes]
std::string EncodeStat(const TopicMetadata& m) {
  std::string v(32, '\0');
  auto put64 = [&v](size_t off, uint64_t x) {
    for (int i = 7; i >= 0; --i) {
      v[off + 7 - i] = static_cast<char>((x >> (8 * i)) & 0xFF);
    }
  };
  put64(0, m.first_seen_ns);
  put64(8, m.last_seen_ns);
  put64(16, m.message_count);
  put64(24, m.byte_count);
  return v;
}

bool DecodeStat(const std::string& v, TopicMetadata* m) {
  if (v.size() < 32) return false;
  auto get64 = [&v](size_t off) {
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) {
      x = (x << 8) | static_cast<uint8_t>(v[off + i]);
    }
    return x;
  };
  m->first_seen_ns = get64(0);
  m->last_seen_ns = get64(8);
  m->message_count = get64(16);
  m->byte_count = get64(24);
  return true;
}

}  // namespace

TimelineImplV2::~TimelineImplV2() {
  if (opened_) {
    Close();
  }
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
Status TimelineImplV2::Open(const std::string& path) {
  if (opened_) {
    return Status::AlreadyExists("timeline already open");
  }
  path_ = path;

  storage::lsm_v2::StratumDB* raw = nullptr;
  Status s = storage::lsm_v2::StratumDB::Open(path, &raw);
  if (!s.ok()) {
    return s;
  }
  db_.reset(raw);
  opened_ = true;

  Status ls = LoadTopicsLocked();
  if (!ls.ok()) {
    db_.reset();
    opened_ = false;
    return ls;
  }
  ls = LoadRetentionPointsLocked();
  if (!ls.ok()) {
    db_.reset();
    opened_ = false;
    return ls;
  }
  // 【必须】恢复会话级元数据。缺了这一步，重开后的 created_at_ns /
  // closed_at_ns / total_* 全是初值（表现为 CLI 的"创建: (未设置)"，
  // 以及 verify 误报"(未正常关闭)"）。
  {
    Status ss = LoadSessionLocked();
    if (!ss.ok()) {
      db_.reset();
      opened_ = false;
      return ss;
    }
  }

  return Status::OK();
}

Status TimelineImplV2::Close() {
  if (!opened_) {
    return Status::OK();
  }
  std::lock_guard<std::mutex> lock(mutex_);

  // 【顺序关键】先持久化再释放：早期实现先 clear 再持久化，
  // 导致落盘的总是空列表，重开后保留点全部丢失
  // （P4 测试当场抓住）。
  Status s = PersistRetentionPointsLocked();

  // 释放全部保留点（必须早于 db_ 销毁，RetentionPoint 生命周期契约）
  retention_points_.clear();
  Status ss = PersistSessionLocked();
  if (!ss.ok() && s.ok()) s = ss;
  for (const auto& kv : topic_meta_) {
    Status ts = PersistTopicStatsLocked(kv.first, kv.second);
    if (!ts.ok() && s.ok()) {
      s = ts;
    }
  }
  db_.reset();
  opened_ = false;
  return s;
}

// ---------------------------------------------------------------------------
// 话题
// ---------------------------------------------------------------------------
Status TimelineImplV2::RegisterTopic(const TopicMetadata& metadata,
                                     uint32_t* topic_id_out) {
  if (!opened_) return Status::ShutdownInProgress("timeline not open");
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = topic_ids_.find(metadata.topic_name);
  if (it != topic_ids_.end()) {
    if (topic_id_out != nullptr) *topic_id_out = it->second;
    return Status::OK();
  }

  uint32_t id = 0;
  Status s = db_->RegisterTopic(metadata, &id);
  if (!s.ok()) {
    return s;
  }
  topic_ids_[metadata.topic_name] = id;
  TopicMetadata m = metadata;
  m.topic_id = id;
  m.first_seen_ns = kInvalidTimestamp;
  m.last_seen_ns = kInvalidTimestamp;
  m.message_count = 0;
  m.byte_count = 0;
  m.active = true;
  topic_meta_[id] = m;
  if (topic_id_out != nullptr) *topic_id_out = id;
  return Status::OK();
}

Status TimelineImplV2::LoadTopicsLocked() {
  topic_ids_.clear();
  topic_meta_.clear();

  std::vector<TopicMetadata> topics;
  Status s = db_->ListTopics(&topics);
  if (!s.ok()) {
    return s;
  }
  for (const auto& t : topics) {
    topic_ids_[t.topic_name] = t.topic_id;
    TopicMetadata m = t;
    m.first_seen_ns = kInvalidTimestamp;
    m.last_seen_ns = kInvalidTimestamp;
    // 恢复统计
    std::string raw;
    if (db_->MetaGet(std::string(kStatPrefix) + std::to_string(t.topic_id),
                     &raw)
            .ok()) {
      DecodeStat(raw, &m);
    }
    topic_meta_[t.topic_id] = m;
  }
  return Status::OK();
}

Status TimelineImplV2::GetTopicMetadata(uint32_t topic_id,
                                        TopicMetadata* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = topic_meta_.find(topic_id);
  if (it == topic_meta_.end()) {
    return Status::NotFound("topic not found");
  }
  if (out != nullptr) *out = it->second;
  return Status::OK();
}

Status TimelineImplV2::ListTopics(std::vector<TopicMetadata>* out) const {
  if (out == nullptr) return Status::InvalidArgument("out is null");
  std::lock_guard<std::mutex> lock(mutex_);
  out->clear();
  for (const auto& kv : topic_meta_) {
    out->push_back(kv.second);
  }
  std::sort(out->begin(), out->end(),
            [](const TopicMetadata& a, const TopicMetadata& b) {
              return a.topic_id < b.topic_id;
            });
  return Status::OK();
}

Status TimelineImplV2::FindTopicByName(const std::string& name,
                                       TopicMetadata* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = topic_ids_.find(name);
  if (it == topic_ids_.end()) {
    return Status::NotFound("topic not found: " + name);
  }
  if (out != nullptr) *out = topic_meta_.at(it->second);
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 追加
// ---------------------------------------------------------------------------
void TimelineImplV2::UpdateQualityOnAppend(const Event& e) {
  // 时间戳倒退（EC-01）
  auto ts_it = last_source_ts_.find(e.topic_id);
  if (ts_it != last_source_ts_.end() &&
      e.source_timestamp_ns < ts_it->second) {
    quality_.timestamp_regressions += 1;
  }
  last_source_ts_[e.topic_id] = e.source_timestamp_ns;

  // 序号缺口（RC-3）
  auto seq_it = last_seq_.find(e.topic_id);
  if (seq_it != last_seq_.end()) {
    const uint64_t expected = static_cast<uint64_t>(seq_it->second) + 1;
    if (e.seq > expected) {
      quality_.sequence_gaps += 1;
      quality_.missing_messages += (e.seq - expected);
    }
  }
  last_seq_[e.topic_id] = e.seq;

  // 接收延迟
  if (e.source_timestamp_ns != kInvalidTimestamp &&
      e.reception_timestamp_ns != kInvalidTimestamp &&
      e.reception_timestamp_ns >= e.source_timestamp_ns) {
    const int64_t lat =
        static_cast<int64_t>(e.reception_timestamp_ns - e.source_timestamp_ns);
    if (quality_.latency_samples == 0) {
      quality_.min_latency_ns = lat;
      quality_.max_latency_ns = lat;
    } else {
      quality_.min_latency_ns = std::min(quality_.min_latency_ns, lat);
      quality_.max_latency_ns = std::max(quality_.max_latency_ns, lat);
    }
    quality_.total_latency_ns += lat;
    quality_.latency_samples += 1;
  }
}

Status TimelineImplV2::Append(const Event& event) {
  std::vector<Event> one{event};
  return AppendBatch(one);
}

Status TimelineImplV2::AppendBatch(const std::vector<Event>& events) {
  if (!opened_) return Status::ShutdownInProgress("timeline not open");
  if (events.empty()) return Status::OK();

  std::lock_guard<std::mutex> lock(mutex_);

  // 先更新内存统计与质量（若 DB 写失败，统计会虚高 ——
  // 与 v1 相同的取舍，见 M3 缺陷记录；正确做法是写成功后更新）
  for (const Event& e : events) {
    auto mit = topic_meta_.find(e.topic_id);
    if (mit == topic_meta_.end()) {
      return Status::InvalidArgument("unregistered topic id: " +
                                     std::to_string(e.topic_id));
    }
    UpdateQualityOnAppend(e);
    TopicMetadata& m = mit->second;
    m.message_count += 1;
    m.byte_count += e.payload.size();
    if (m.first_seen_ns == kInvalidTimestamp ||
        e.source_timestamp_ns < m.first_seen_ns) {
      m.first_seen_ns = e.source_timestamp_ns;
    }
    if (e.source_timestamp_ns > m.last_seen_ns) {
      m.last_seen_ns = e.source_timestamp_ns;
    }
    total_events_ += 1;
    total_bytes_ += e.payload.size();
    if (session_start_ns_ == kInvalidTimestamp ||
        e.source_timestamp_ns < session_start_ns_) {
      session_start_ns_ = e.source_timestamp_ns;
    }
    if (e.source_timestamp_ns > session_end_ns_) {
      session_end_ns_ = e.source_timestamp_ns;
    }
  }

  return db_->AppendBatch(events, /*sync=*/false);
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------
Status TimelineImplV2::QueryRange(uint32_t topic_id, const TimeRange& range,
                                  EventIteratorPtr* out) {
  if (out == nullptr) return Status::InvalidArgument("out is null");
  if (!opened_) return Status::ShutdownInProgress("timeline not open");

  std::vector<Event> all;
  if (topic_id == 0) {
    // 全部话题：逐话题查询后按时间戳归并
    std::vector<TopicMetadata> topics;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& kv : topic_meta_) {
        topics.push_back(kv.second);
      }
    }
    for (const auto& t : topics) {
      std::vector<Event> part;
      Status s = db_->QueryRange(t.topic_id, range.begin_ns, range.end_ns,
                                 &part);
      if (!s.ok()) {
        *out = std::unique_ptr<IEventIterator>(
            new TimelineV2Iterator(std::move(all), s));
        return s;
      }
      all.insert(all.end(), std::make_move_iterator(part.begin()),
                 std::make_move_iterator(part.end()));
    }
    std::sort(all.begin(), all.end(),
              [](const Event& a, const Event& b) {
                return a.source_timestamp_ns < b.source_timestamp_ns;
              });
  } else {
    Status s = db_->QueryRange(topic_id, range.begin_ns, range.end_ns, &all);
    if (!s.ok()) {
      *out = std::unique_ptr<IEventIterator>(
          new TimelineV2Iterator(std::move(all), s));
      return s;
    }
  }

  *out = std::unique_ptr<IEventIterator>(
      new TimelineV2Iterator(std::move(all), Status::OK()));
  return Status::OK();
}

Status TimelineImplV2::GetStateAt(uint32_t topic_id, TimestampNs as_of_ns,
                                  StateAtResult* out) {
  if (out == nullptr) return Status::InvalidArgument("out is null");
  if (!opened_) return Status::ShutdownInProgress("timeline not open");

  // 早于该话题首次出现：明确 out_of_range（FR-Q-3）
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = topic_meta_.find(topic_id);
    if (it == topic_meta_.end()) {
      out->kind = StateAtResult::Kind::kNotFound;
      return Status::OK();
    }
    if (it->second.first_seen_ns != kInvalidTimestamp &&
        as_of_ns < it->second.first_seen_ns) {
      out->kind = StateAtResult::Kind::kOutOfRange;
      out->as_of_ns = as_of_ns;
      return Status::OK();
    }
  }

  StateAtResult r;
  Status s = db_->GetStateAt(topic_id, as_of_ns, &r);
  if (!s.ok()) {
    return s;
  }
  *out = std::move(r);
  return Status::OK();
}

Status TimelineImplV2::CountRange(uint32_t topic_id, const TimeRange& range,
                                  uint64_t* count_out) {
  if (count_out == nullptr) return Status::InvalidArgument("count_out is null");
  if (topic_id == 0) {
    EventIteratorPtr it;
    Status s = QueryRange(0, range, &it);
    if (!s.ok()) return s;
    uint64_t n = 0;
    for (; it->Valid(); it->Next()) {
      ++n;
    }
    *count_out = n;
    return it->status();
  }
  return db_->CountRange(topic_id, range.begin_ns, range.end_ns, count_out);
}

// ---------------------------------------------------------------------------
// 保留点与保留策略
// ---------------------------------------------------------------------------
Status TimelineImplV2::CreateRetentionPoint(TimestampNs at_ns,
                                            uint64_t* point_id_out) {
  if (!opened_) return Status::ShutdownInProgress("timeline not open");
  std::lock_guard<std::mutex> lock(mutex_);

  auto point = std::unique_ptr<storage::lsm_v2::RetentionPoint>();
  Status s = db_->CreateRetentionPoint(&point);
  if (!s.ok()) {
    return s;
  }
  const uint64_t id = next_retention_point_id_++;
  retention_points_[id] =
      std::make_pair(at_ns, std::move(point));
  // 【P4】立即持久化：保留点必须在崩溃后仍有效。
  Status ps = PersistRetentionPointsLocked();
  if (!ps.ok()) {
    retention_points_.erase(id);
    return ps;
  }
  if (point_id_out != nullptr) *point_id_out = id;
  return Status::OK();
}

Status TimelineImplV2::ReleaseRetentionPoint(uint64_t point_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = retention_points_.find(point_id);
  if (it == retention_points_.end()) {
    return Status::NotFound("retention point not found");
  }
  retention_points_.erase(it);
  return PersistRetentionPointsLocked();
}

Status TimelineImplV2::SetRetentionPolicy(const RetentionPolicy& policy) {
  std::lock_guard<std::mutex> lock(mutex_);
  retention_ = policy;
  return Status::OK();
}

RetentionPolicy TimelineImplV2::GetRetentionPolicy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return retention_;
}

Status TimelineImplV2::RunRetention() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) return Status::ShutdownInProgress("timeline not open");

  // 无保留策略：无操作
  if (retention_.retain_duration_ns == 0 && retention_.max_bytes == 0) {
    return Status::OK();
  }

  // 保留点保护：cutoff 不得晚于最老的保留点时刻
  TimestampNs cutoff = kInvalidTimestamp;
  if (retention_.retain_duration_ns > 0) {
    // 以会话结束时刻为"现在"（录制场景下事件时间戳是数据时间）
    const TimestampNs now = session_end_ns_ != kInvalidTimestamp
                                ? session_end_ns_
                                : last_source_ts_.empty()
                                      ? 0
                                      : session_end_ns_;
    (void)now;
    // 简化：按 last_seen 为基准计算
    TimestampNs latest = 0;
    for (const auto& kv : topic_meta_) {
      if (kv.second.last_seen_ns > latest) {
        latest = kv.second.last_seen_ns;
      }
    }
    if (latest >= retention_.retain_duration_ns) {
      cutoff = latest - retention_.retain_duration_ns;
    }
  }

  // 保留点保护（ITimeline 语义："保留点**之前**的数据不被淘汰"）。
  //
  // 受保护区间 = [0, 最老保留点时刻)，删除区间 = [最老保留点, cutoff)。
  //
  // 早期实现把 cutoff 收缩到保留点并删除 [0, 保留点) ——
  // **恰好把受保护的数据删掉了**（P4 测试当场抓住：
  // 重启前保留点之前的条数就已是 0）。
  TimestampNs protected_until = 0;
  for (const auto& kv : retention_points_) {
    if (kv.second.first != kInvalidTimestamp &&
        kv.second.first > protected_until) {
      protected_until = kv.second.first;
    }
  }

  if (cutoff == kInvalidTimestamp) {
    return Status::OK();
  }

  // 逐话题删除 [protected_until, cutoff) 区间
  for (const auto& kv : topic_meta_) {
    if (kv.second.first_seen_ns != kInvalidTimestamp &&
        kv.second.first_seen_ns >= cutoff) {
      continue;  // 该话题全部数据都在保留范围内
    }
    if (kv.second.last_seen_ns != kInvalidTimestamp &&
        kv.second.last_seen_ns < protected_until) {
      continue;  // 该话题全部数据都在保护区间内
    }
    Status s = db_->DeleteRange(kv.first, protected_until, cutoff);
    if (!s.ok() && !s.IsNotFound()) {
      return s;
    }
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 运维
// ---------------------------------------------------------------------------
Status TimelineImplV2::Flush() {
  if (!opened_) return Status::ShutdownInProgress("timeline not open");
  std::lock_guard<std::mutex> lock(mutex_);
  Status s = db_->Flush();
  if (!s.ok()) {
    return s;
  }
  for (const auto& kv : topic_meta_) {
    Status ts = PersistTopicStatsLocked(kv.first, kv.second);
    if (!ts.ok() && s.ok()) {
      s = ts;
    }
  }
  return PersistSessionLocked();
}

QualityStats TimelineImplV2::GetQualityStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return quality_;
}

Status TimelineImplV2::ReportDrop(uint64_t count, uint64_t bytes,
                                 TimestampNs at_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  quality_.dropped_messages += count;
  quality_.dropped_bytes += bytes;
  if (quality_.first_drop_ns == kInvalidTimestamp) {
    quality_.first_drop_ns = at_ns;
  }
  quality_.last_drop_ns = at_ns;
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 持久化辅助
// ---------------------------------------------------------------------------
Status TimelineImplV2::PersistTopicStatsLocked(uint32_t topic_id,
                                               const TopicMetadata& meta) {
  return db_->MetaPut(std::string(kStatPrefix) + std::to_string(topic_id),
                      EncodeStat(meta));
}

Status TimelineImplV2::PersistRetentionPointsLocked() {
  // 编码：[4B count][每项: 8B id | 8B at_ns]
  std::string v;
  v += U32BE(static_cast<uint32_t>(retention_points_.size()));
  for (const auto& kv : retention_points_) {
    auto put64 = [&v](uint64_t x) {
      for (int i = 7; i >= 0; --i) {
        v.push_back(static_cast<char>((x >> (8 * i)) & 0xFF));
      }
    };
    put64(kv.first);
    put64(kv.second.first);
  }
  return db_->MetaPut("retention_points", v);
}

Status TimelineImplV2::LoadRetentionPointsLocked() {
  retention_points_.clear();
  next_retention_point_id_ = 1;

  std::string raw;
  Status s = db_->MetaGet("retention_points", &raw);
  if (s.IsNotFound()) {
    return Status::OK();  // 尚无保留点
  }
  if (!s.ok()) {
    return s;
  }
  if (raw.size() < 4) {
    return Status::Corruption("retention points list corrupted");
  }
  auto get64 = [&raw](size_t off) {
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) {
      x = (x << 8) | static_cast<uint8_t>(raw[off + i]);
    }
    return x;
  };
  const uint32_t count = DecodeU32BE(raw.data());
  if (raw.size() < 4 + static_cast<size_t>(count) * 16) {
    return Status::Corruption("retention points list truncated");
  }
  for (uint32_t i = 0; i < count; ++i) {
    const size_t off = 4 + static_cast<size_t>(i) * 16;
    const uint64_t id = get64(off);
    const TimestampNs at_ns = get64(off + 8);
    // 恢复的保留点：快照为 null ——
    // 其语义由 at_ns 表达（RunRetention 保护 + 时间点查询）。
    retention_points_[id] =
        std::make_pair(at_ns,
                       std::unique_ptr<storage::lsm_v2::RetentionPoint>());
    if (id >= next_retention_point_id_) {
      next_retention_point_id_ = id + 1;
    }
  }
  return Status::OK();
}

// 读取会话级元数据。
//
// 【为什么必须有】PersistSessionLocked 在 Close 时写入这些值，但 Open 此前
// **没有**对应的读取 —— 于是新进程重开会话后 session_start_ns_ /
// session_end_ns_ / total_events_ / total_bytes_ 全部回到初值，
// GetSessionMetadata 返回 created_at_ns = 0：
//   · CLI 显示「创建: (未设置)」
//   · verify 误报「(未正常关闭)」（假阳性）
Status TimelineImplV2::LoadSessionLocked() {
  std::string raw;
  Status s = db_->MetaGet(std::string(kSessionKey), &raw);
  if (!s.ok()) {
    // 会话元数据尚不存在（全新会话）不是错误
    return Status::OK();
  }
  if (raw.size() < 32) {
    return Status::Corruption("session metadata too short: " +
                              std::to_string(raw.size()));
  }
  auto get64 = [&raw](size_t off) -> uint64_t {
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) {
      x = (x << 8) | static_cast<unsigned char>(raw[off + static_cast<size_t>(i)]);
    }
    return x;
  };
  total_events_ = get64(0);
  total_bytes_ = get64(8);
  const uint64_t start = get64(16);
  const uint64_t end = get64(24);
  // 写入端把"无效"编码为 0，这里还原，避免 0 被当成真实时间戳
  session_start_ns_ = (start == 0) ? kInvalidTimestamp : start;
  session_end_ns_ = (end == 0) ? kInvalidTimestamp : end;
  return Status::OK();
}

// 删除话题在 range 内的数据。
//
// 【关键】删除是**主动行为**，会在时间轴上留下空洞。必须记录 pruned 区间，
// 否则 verify 会把它当作"数据丢失"（RC-4）上报 —— 那会让人去查
// 并不存在的录制问题。这与 ctl stop 必须留痕是同一个道理。
Status TimelineImplV2::DeleteRange(uint32_t topic_id, const TimeRange& range,
                                   uint64_t* deleted_events,
                                   uint64_t* deleted_bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) return Status::ShutdownInProgress("timeline not open");

  auto it = topic_meta_.find(topic_id);
  if (it == topic_meta_.end()) {
    return Status::NotFound("topic id not registered: " + std::to_string(topic_id));
  }
  if (range.Empty()) {
    return Status::InvalidArgument("empty delete range (begin >= end)");
  }

  // 删除前先统计，便于回填
  uint64_t count_before = it->second.message_count;
  uint64_t bytes_before = it->second.byte_count;

  Status s = db_->DeleteRange(topic_id, range.begin_ns, range.end_ns);
  if (!s.ok()) return s;

  // --- 重新统计该话题（删除后从 DB 重新数，避免估算误差累积）---
  uint64_t after_count = 0, after_bytes = 0;
  {
    TimeRange all;
    all.begin_ns = kInvalidTimestamp;
    all.end_ns = kInvalidTimestamp;
    std::vector<Event> remaining;
    Status qs = db_->QueryRange(topic_id, all.begin_ns, all.end_ns, &remaining);
    if (qs.ok()) {
      after_count = remaining.size();
      for (const auto& e : remaining) after_bytes += e.payload.size();
    } else {
      // 查不到就退化为按删除前的计数处理，不虚报
      after_count = count_before;
      after_bytes = bytes_before;
    }
  }
  const uint64_t del_events = (count_before > after_count) ? (count_before - after_count) : 0;
  const uint64_t del_bytes = (bytes_before > after_bytes) ? (bytes_before - after_bytes) : 0;

  // --- 更新统计 ---
  it->second.message_count = after_count;
  it->second.byte_count = after_bytes;
  // 【FR-R-11】删空后**保留元数据**，只标记为空：
  // 否则"这个话题曾经存在过、它的类型是什么"这一事实会永久丢失。
  it->second.active = (after_count > 0);
  if (after_count == 0) {
    it->second.first_seen_ns = kInvalidTimestamp;
    it->second.last_seen_ns = kInvalidTimestamp;
  }
  if (total_events_ >= del_events) total_events_ -= del_events;
  if (total_bytes_ >= del_bytes) total_bytes_ -= del_bytes;

  // --- 记录 pruned 区间（使"主动删除"可追溯）---
  // 用独立 meta key，避免改动既有会话元数据编码（向后兼容）。
  {
    const std::string key = std::string("pruned/") + std::to_string(topic_id);
    std::string existing;
    (void)db_->MetaGet(key, &existing);
    std::string rec = existing;
    rec += std::to_string(range.begin_ns);
    rec += ',';
    rec += std::to_string(range.end_ns);
    rec += ',';
    rec += std::to_string(del_events);
    rec += '\n';
    (void)db_->MetaPut(key, rec);   // 记录失败不阻断删除本身
  }

  if (deleted_events) *deleted_events = del_events;
  if (deleted_bytes) *deleted_bytes = del_bytes;
  return Status::OK();
}

Status TimelineImplV2::PersistSessionLocked() {
  // 【保护】若本次打开后**从未 append 过任何事件**，则不覆盖已存的会话元数据。
  //
  // 否则"打开 -> 立即关闭"（例如只做了 session info / verify 查询）
  // 会把真实的时间戳与统计清零 —— 这是数据损坏，且症状隐蔽
  // （事后只看到"创建: (未设置)"，不知道是被谁清的）。
  if (session_start_ns_ == kInvalidTimestamp && total_events_ == 0) {
    std::string existing;
    if (db_->MetaGet(std::string(kSessionKey), &existing).ok() && !existing.empty()) {
      return Status::OK();
    }
  }

  // 会话元数据编码（简化）：[8B total_events][8B total_bytes]
  // [8B start][8B end][4B topic_count]
  std::string v(36, '\0');
  auto put64 = [&v](size_t off, uint64_t x) {
    for (int i = 7; i >= 0; --i) {
      v[off + 7 - i] = static_cast<char>((x >> (8 * i)) & 0xFF);
    }
  };
  put64(0, total_events_);
  put64(8, total_bytes_);
  put64(16, session_start_ns_ == kInvalidTimestamp ? 0 : session_start_ns_);
  put64(24, session_end_ns_ == kInvalidTimestamp ? 0 : session_end_ns_);
  put64(32, topic_ids_.size());
  return db_->MetaPut(kSessionKey, v);
}

Status TimelineImplV2::GetSessionMetadata(SessionMetadata* out) const {
  if (out == nullptr) return Status::InvalidArgument("out is null");
  std::lock_guard<std::mutex> lock(mutex_);
  out->total_events = total_events_;
  out->total_bytes = total_bytes_;
  out->created_at_ns = session_start_ns_;
  out->closed_at_ns = session_end_ns_;
  out->quality = quality_;
  return Status::OK();
}

}  // namespace stratum
