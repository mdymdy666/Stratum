// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// StratumDB 实现：时间轴语义层，底层 LSMengine（LevelDB 架构）

#include "stratum/storage/lsm_v2/stratum_db.h"

#include <cstring>
#include <map>
#include <sstream>

#include "LSMengine/iterator.h"
#include "LSMengine/options.h"
#include "LSMengine/slice.h"
#include "LSMengine/write_batch.h"
#include "stratum/storage/time_key.h"

namespace stratum {
namespace storage {
namespace lsm_v2 {

namespace {

constexpr char kMetaTopicPrefix[] = "META/topic/";
constexpr char kMetaNextTopicId[] = "META/next_topic_id";
constexpr char kMetaSession[] = "META/session";

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

// 元数据值：topic 注册信息 = [4B topic_id][4B type_len][type_name]
std::string EncodeTopicMeta(uint32_t topic_id, const std::string& type_name) {
  std::string v = U32BE(topic_id) + U32BE(static_cast<uint32_t>(type_name.size()));
  v += type_name;
  return v;
}

bool DecodeTopicMeta(const std::string& v, uint32_t* topic_id,
                     std::string* type_name) {
  if (v.size() < 8) return false;
  *topic_id = DecodeU32BE(v.data());
  const uint32_t len = DecodeU32BE(v.data() + 4);
  if (v.size() < 8 + len) return false;
  *type_name = v.substr(8, len);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
StratumDB::StratumDB() = default;

StratumDB::~StratumDB() { Close(); }

Status StratumDB::Open(const std::string& path, StratumDB** out) {
  if (out == nullptr) {
    return Status::InvalidArgument("out is null");
  }
  auto* db = new StratumDB();
  db->path_ = path;

  LSMengine::Options options;
  options.comparator = &db->comparator_;
  options.create_if_missing = true;

  LSMengine::DB* raw = nullptr;
  LSMengine::Status s = LSMengine::DB::Open(options, path, &raw);
  if (!s.ok()) {
    delete db;
    return Status::IOError("LSMengine open failed: " + s.ToString());
  }
  db->db_ = raw;
  db->opened_ = true;

  Status load = db->LoadTopics();
  if (!load.ok()) {
    delete db;
    return load;
  }

  *out = db;
  return Status::OK();
}

Status StratumDB::Close() {
  // 【B-M7 修复】加锁：早期实现 Close 与并发操作存在数据竞争，
  // 另一线程在 delete db_ 后继续使用 = use-after-free。
  std::lock_guard<std::mutex> lock(mutex_);
  if (db_ != nullptr) {
    delete db_;
    db_ = nullptr;
  }
  opened_ = false;
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 键编码
// ---------------------------------------------------------------------------
std::string StratumDB::EventKey(uint32_t topic_id, TimestampNs ts_ns,
                                uint32_t seq_in_ts) {
  // seq_in_ts 按位取反后编码：
  //   同 (topic, ts) 内，seq 越大 → stored 越小 → 排在越前面
  //   时间点查询 Seek(topic, T, seq=0xFFFFFFFF) 即命中 T 时刻最新条目
  return TimeKeyCodec::Encode(topic_id, ts_ns, ~seq_in_ts);
}

bool StratumDB::DecodeEventKey(const std::string& key, uint32_t* topic_id,
                               TimestampNs* ts_ns, uint32_t* seq_in_ts) {
  TimeKey tk;
  if (!TimeKeyCodec::Decode(key, &tk)) {
    return false;
  }
  if (topic_id != nullptr) *topic_id = tk.topic_id;
  if (ts_ns != nullptr) *ts_ns = tk.timestamp_ns;
  if (seq_in_ts != nullptr) *seq_in_ts = ~tk.seq_in_ts;  // 反转回来
  return true;
}

std::string StratumDB::MetaKey(const std::string& name) {
  return "META/" + name;
}

std::string StratumDB::EncodeEventValue(const Event& e) {
  std::string v;
  // reception 时间戳（8B 大端）
  for (int i = 7; i >= 0; --i) {
    v.push_back(static_cast<char>((e.reception_timestamp_ns >> (8 * i)) & 0xFF));
  }
  v += U32BE(e.seq);
  v += U32BE(static_cast<uint32_t>(e.instance_key.size()));
  v += e.instance_key;
  v += e.payload;
  return v;
}

bool StratumDB::DecodeEventValue(const std::string& v, Event* out) {
  // 最小 value = 8(reception) + 4(seq) + 4(iklen) + 0 + 0 = 16 字节。
  // 早期写成 <20，导致 16~19 字节的短 payload 全部解码失败
  //（实测 Corruption: event value decode failed）。
  if (out == nullptr || v.size() < 16) {
    return false;
  }
  uint64_t reception = 0;
  for (int i = 0; i < 8; ++i) {
    reception = (reception << 8) | static_cast<uint8_t>(v[i]);
  }
  const uint32_t seq = DecodeU32BE(v.data() + 8);
  const uint32_t iklen = DecodeU32BE(v.data() + 12);
  if (v.size() < 16 + iklen) {
    return false;
  }
  out->reception_timestamp_ns = reception;
  out->seq = seq;
  out->instance_key = v.substr(16, iklen);
  out->payload = v.substr(16 + iklen);
  return true;
}

// ---------------------------------------------------------------------------
// 话题注册
// ---------------------------------------------------------------------------
Status StratumDB::LoadTopics() {
  std::lock_guard<std::mutex> lock(mutex_);
  topic_ids_.clear();
  next_topic_id_ = 1;

  LSMengine::ReadOptions ro;
  std::unique_ptr<LSMengine::Iterator> it(db_->NewIterator(ro));
  if (it == nullptr) {
    return Status::Internal("failed to create iterator");
  }

  // 扫描 "META/topic/" 前缀（"0" 是 '/' 的下一字符，构成半开区间）
  it->Seek(LSMengine::Slice(kMetaTopicPrefix));
  const std::string limit =
      std::string(kMetaTopicPrefix, strlen(kMetaTopicPrefix) - 1) + "0";
  for (; it->Valid(); it->Next()) {
    if (it->key().ToString() >= limit) {
      break;
    }
    const std::string name =
        it->key().ToString().substr(strlen(kMetaTopicPrefix));
    uint32_t id = 0;
    std::string type_name;
    if (DecodeTopicMeta(it->value().ToString(), &id, &type_name)) {
      topic_ids_[name] = id;
      topic_names_[id] = name;
      if (id >= next_topic_id_) {
        next_topic_id_ = id + 1;
      }
    }
  }
  if (!it->status().ok()) {
    return Status::IOError(it->status().ToString());
  }

  // 【P1 修复】恢复每个话题的 seq_in_ts 计数器。
  //
  // seq_in_ts 编码在事件 key 里（反转后），若不恢复计数器，
  // 重启后从 0 重新分配 —— 写入与历史相同的 (topic, ts) 时
  // key 与历史条目完全相同，LevelDB 视为覆盖写，
  // **历史数据被静默覆盖**（v1 LSM 引擎已实测踩到同一缺陷）。
  //
  // 恢复方法：读 AppendBatch 随批原子写入的计数器 key
  // （"META/seq_counter/<id>"）。为什么不从事件 key 扫描恢复：
  // 反转编码下"该话题最后一条"恰好是 seq 最小的条目（stored=~seq，
  // 最后一条 stored 最大 → seq 最小），取它会得到 0；而扫描全部
  // 事件取 max 是 O(事件总数)，不可接受。计数器 key 是精确且
  // O(1) 的方案（第一次实现踩过 Prev 的坑，测试当场抓住）。
  for (const auto& kv : topic_ids_) {
    const uint32_t id = kv.second;
    std::string counter_raw;
    LSMengine::ReadOptions ro2;
    LSMengine::Status gs = db_->Get(
        ro2, MetaKey("seq_counter/" + std::to_string(id)), &counter_raw);
    if (gs.ok() && counter_raw.size() >= 4) {
      seq_in_ts_state_[id].second = DecodeU32BE(counter_raw.data());
    }
  }

  return Status::OK();
}

Status StratumDB::RegisterTopic(const TopicMetadata& meta, uint32_t* topic_id) {
  if (!opened_) {
    return Status::ShutdownInProgress("db not open");
  }
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = topic_ids_.find(meta.topic_name);
  if (it != topic_ids_.end()) {
    if (topic_id != nullptr) *topic_id = it->second;
    return Status::OK();  // 幂等
  }

  const uint32_t id = next_topic_id_++;

  LSMengine::WriteOptions wo;
  wo.sync = true;  // 话题注册必须持久化（否则重启后事件无归属）

  LSMengine::WriteBatch batch;
  batch.Put(std::string(kMetaTopicPrefix) + meta.topic_name,
            EncodeTopicMeta(id, meta.type_name));
  batch.Put(kMetaNextTopicId, U32BE(next_topic_id_));

  LSMengine::Status s = db_->Write(wo, &batch);
  if (!s.ok()) {
    --next_topic_id_;
    return Status::IOError("register topic failed: " + s.ToString());
  }

  topic_ids_[meta.topic_name] = id;
  topic_names_[id] = meta.topic_name;
  if (topic_id != nullptr) *topic_id = id;
  return Status::OK();
}

Status StratumDB::ListTopics(std::vector<TopicMetadata>* out) {
  if (out == nullptr) return Status::InvalidArgument("out is null");
  std::lock_guard<std::mutex> lock(mutex_);
  out->clear();
  for (const auto& kv : topic_ids_) {
    TopicMetadata m;
    m.topic_name = kv.first;
    m.topic_id = kv.second;
    // 【缺陷修复】话题注册值里存着 type_name，必须解出来一起返回。
    //
    // 为什么关键：回放路径是
    //   StratumDB::ListTopics → TimelineImplV2 → Player
    //   → ReceivedSample.type_name → RtpsWriter::Publish
    //   → BuildPublication 里的 PID_TYPE_NAME
    // type_name 为空时 ParameterString_t 拒绝序列化（空字符串返回 false），
    // BuildPublication 直接返回空 vector，于是 **SEDP Publication 静默不发**：
    // 对端（Fast DDS）永远发现不了我方 Writer → 不回 SEDP Subscription
    // → 我们学不到对端 user locator → 用户数据被发到 metatraffic 端口
    // → 对端 DataReader 收到 0 条。
    // 外部症状与"QoS 不兼容"一模一样，但根因在存储层丢了字段。
    std::string raw;
    LSMengine::ReadOptions ro;
    if (db_->Get(ro, std::string(kMetaTopicPrefix) + kv.first, &raw).ok()) {
      uint32_t stored_id = 0;
      std::string type_name;
      if (DecodeTopicMeta(raw, &stored_id, &type_name)) {
        m.type_name = type_name;
      }
    }
    out->push_back(std::move(m));
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 追加
// ---------------------------------------------------------------------------
uint32_t StratumDB::NextSeqInTs(uint32_t topic_id, TimestampNs ts_ns) {
  // seq_in_ts 仅需在"同一 (topic, ts)" 内唯一。
  //
  // 注意：**必须全局递增而非"时间戳变化就重置"**——
  // 时间戳乱序（A, A, B, A）时重置会让第 4 条与第 1 条 key 完全相同，
  // 静默覆盖（v1 已实测踩到该缺陷）。
  // 全局递增唯一性天然成立，且反转编码下同 ts 内的相对顺序不变。
  (void)ts_ns;
  auto& counter = seq_in_ts_state_[topic_id].second;
  return counter++;
}

Status StratumDB::AppendBatch(const std::vector<Event>& events,
                              bool sync) {
  if (!opened_) {
    return Status::ShutdownInProgress("db not open");
  }
  if (events.empty()) {
    return Status::OK();
  }

  LSMengine::WriteBatch batch;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // 本批各话题的最新计数器（随同一 WriteBatch 原子提交）
    std::map<uint32_t, uint32_t> batch_next_seq;
    for (const Event& e : events) {
      // 验证话题已注册：未注册的 topic_id 会产生"孤儿数据"
      // （写入成功但无元数据归属，重开后不可见）。
      if (topic_names_.find(e.topic_id) == topic_names_.end()) {
        return Status::InvalidArgument(
            "unregistered topic id: " + std::to_string(e.topic_id));
      }
      const uint32_t seq_in_ts = NextSeqInTs(e.topic_id, e.source_timestamp_ns);
      batch.Put(EventKey(e.topic_id, e.source_timestamp_ns, seq_in_ts),
                EncodeEventValue(e));
      batch_next_seq[e.topic_id] = seq_in_ts + 1;
      bytes_written_.fetch_add(e.payload.size(), std::memory_order_relaxed);
    }
    // 【P1】seq 计数器与事件**同批原子提交**：
    // 崩溃时要么都生效（计数器与事件一致），要么都不生效（重放 WAL
    // 后一致）。不额外增加 fsync 次数，零写放大。
    for (const auto& kv : batch_next_seq) {
      batch.Put(MetaKey("seq_counter/" + std::to_string(kv.first)),
                U32BE(kv.second));
    }
  }

  LSMengine::WriteOptions wo;
  // 早期硬编码 sync=false，Recorder 的 durability 配置无法接入。
  // 现在由调用方决定（kStrict → true，其余 → false）。
  wo.sync = sync;

  LSMengine::Status s = db_->Write(wo, &batch);
  if (!s.ok()) {
    return Status::IOError("append failed: " + s.ToString());
  }
  writes_.fetch_add(events.size(), std::memory_order_relaxed);
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 时间范围查询
// ---------------------------------------------------------------------------
Status StratumDB::QueryRange(uint32_t topic_id, TimestampNs begin_ns,
                             TimestampNs end_ns, std::vector<Event>* out,
                             const RetentionPoint* at_point) {
  if (out == nullptr) return Status::InvalidArgument("out is null");
  if (!opened_) return Status::ShutdownInProgress("db not open");
  out->clear();

  LSMengine::ReadOptions ro;
  if (at_point != nullptr && at_point->raw() != nullptr) {
    ro.snapshot = at_point->raw();
  }
  std::unique_ptr<LSMengine::Iterator> it(db_->NewIterator(ro));
  if (it == nullptr) return Status::Internal("failed to create iterator");

  // 范围 [begin, end)：
  //   begin_key = begin 时刻 seq 最大的条目（stored=0）
  //   end_key   = end 时刻 seq 最大的条目（stored=0），半开区间排除 end 时刻全部
  const std::string begin_key =
      begin_ns == kInvalidTimestamp
          ? EventKey(topic_id, 0, 0xFFFFFFFF)
          : EventKey(topic_id, begin_ns, 0xFFFFFFFF);
  const std::string end_key = EventKey(topic_id, end_ns, 0xFFFFFFFF);

  it->Seek(begin_key);
  for (; it->Valid(); it->Next()) {
    if (end_ns != kInvalidTimestamp && it->key().ToString() >= end_key) {
      break;
    }
    uint32_t t = 0;
    TimestampNs ts = 0;
    uint32_t seq = 0;
    if (!DecodeEventKey(it->key().ToString(), &t, &ts, &seq)) {
      continue;  // 非事件 key（不可能，但防御）
    }
    if (t != topic_id) {
      break;  // 已越过该话题
    }
    Event e;
    e.topic_id = t;
    e.source_timestamp_ns = ts;
    e.seq_in_ts = seq;
    // 完整解码（含 reception/seq/instance_key）
    if (!DecodeEventValue(it->value().ToString(), &e)) {
      return Status::Corruption("event value decode failed");
    }
    out->push_back(std::move(e));
  }
  if (!it->status().ok()) {
    return Status::IOError(it->status().ToString());
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 时间点查询：Seek + Prev，O(log n)
// ---------------------------------------------------------------------------
Status StratumDB::GetStateAt(uint32_t topic_id, TimestampNs as_of_ns,
                             StateAtResult* out,
                             const RetentionPoint* at_point) {
  if (out == nullptr) return Status::InvalidArgument("out is null");
  if (!opened_) return Status::ShutdownInProgress("db not open");

  LSMengine::ReadOptions ro;
  if (at_point != nullptr && at_point->raw() != nullptr) {
    ro.snapshot = at_point->raw();
  }
  std::unique_ptr<LSMengine::Iterator> it(db_->NewIterator(ro));
  if (it == nullptr) return Status::Internal("failed to create iterator");

  // Seek 到 (topic, as_of, seq 最大) —— as_of 时刻的最新条目
  const std::string seek_key = EventKey(topic_id, as_of_ns, 0xFFFFFFFF);
  it->Seek(seek_key);

  // 修正定位：若命中的是更晚时刻（as_of 无数据）或其他话题，回退一步
  if (it->Valid()) {
    uint32_t t = 0;
    TimestampNs ts = 0;
    if (DecodeEventKey(it->key().ToString(), &t, &ts, nullptr) &&
        (t != topic_id || ts > as_of_ns)) {
      it->Prev();
      if (!it->Valid()) {
        // 【B-M2 修复】Prev 越过库首 = 不存在。
        // 早期实现这里调 SeekToLast()，会把光标移到库尾
        // 的"未来数据"并返回它 —— 查询早于全部数据的时间点
        // 会错误地返回未来值。
        out->kind = StateAtResult::Kind::kNotFound;
        return Status::OK();
      }
    }
  } else {
    // Seek 超出库尾：回退到最后一条（as_of 晚于全部数据时
    // 返回最新值，与"as_of 或之前的最新值"语义一致）
    it->SeekToLast();
  }
  if (!it->Valid()) {
    out->kind = StateAtResult::Kind::kNotFound;
    return Status::OK();
  }

  uint32_t t = 0;
  TimestampNs ts = 0;
  uint32_t seq = 0;
  if (!DecodeEventKey(it->key().ToString(), &t, &ts, &seq)) {
    out->kind = StateAtResult::Kind::kNotFound;
    return Status::OK();
  }
  if (t != topic_id) {
    out->kind = StateAtResult::Kind::kNotFound;
    return Status::OK();
  }

  out->kind = StateAtResult::Kind::kFound;
  out->as_of_ns = ts;
  Event e;
  e.topic_id = t;
  e.source_timestamp_ns = ts;
  e.seq_in_ts = seq;
  if (!DecodeEventValue(it->value().ToString(), &e)) {
    return Status::Corruption("event value decode failed");
  }
  // 【B-M1 修复】迭代器状态检查：IO 错误不得伪装成成功。
  // 早期实现漏掉此检查，底层 IO 错误会以 kFound 返回。
  if (!it->status().ok()) {
    return Status::IOError(it->status().ToString());
  }
  out->event = std::move(e);
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 计数与删除
// ---------------------------------------------------------------------------
Status StratumDB::CountRange(uint32_t topic_id, TimestampNs begin_ns,
                             TimestampNs end_ns, uint64_t* count,
                             const RetentionPoint* at_point) {
  if (count == nullptr) return Status::InvalidArgument("count is null");
  std::vector<Event> events;
  Status s = QueryRange(topic_id, begin_ns, end_ns, &events, at_point);
  if (!s.ok()) return s;
  *count = events.size();
  return Status::OK();
}

Status StratumDB::DeleteRange(uint32_t topic_id, TimestampNs begin_ns,
                              TimestampNs end_ns) {
  if (!opened_) return Status::ShutdownInProgress("db not open");

  // LevelDB 无原生 range delete：先收集范围内的 key，再批量删除。
  // 更优做法是引入 range tombstone（LevelDB 本身也没有），
  // 目前保留策略按"整个 topic 分段"使用，key 数有限。
  std::vector<std::string> keys;
  {
    LSMengine::ReadOptions ro;
    std::unique_ptr<LSMengine::Iterator> it(db_->NewIterator(ro));
    const std::string begin_key =
        begin_ns == kInvalidTimestamp
            ? EventKey(topic_id, 0, 0xFFFFFFFF)
            : EventKey(topic_id, begin_ns, 0xFFFFFFFF);
    const std::string end_key = EventKey(topic_id, end_ns, 0xFFFFFFFF);
    it->Seek(begin_key);
    for (; it->Valid(); it->Next()) {
      if (end_ns != kInvalidTimestamp && it->key().ToString() >= end_key) break;
      uint32_t t = 0;
      if (!DecodeEventKey(it->key().ToString(), &t, nullptr, nullptr)) continue;
      if (t != topic_id) break;
      keys.push_back(it->key().ToString());
    }
    if (!it->status().ok()) {
      return Status::IOError(it->status().ToString());
    }
  }

  if (keys.empty()) {
    return Status::NotFound("no events in range");
  }

  LSMengine::WriteBatch batch;
  for (const auto& k : keys) {
    batch.Delete(k);
  }
  LSMengine::WriteOptions wo;
  wo.sync = true;  // 删除是保留策略的一部分，需持久
  LSMengine::Status s = db_->Write(wo, &batch);
  if (!s.ok()) {
    return Status::IOError("delete range failed: " + s.ToString());
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 保留点 / Flush / Compact / Stats
// ---------------------------------------------------------------------------
Status StratumDB::CreateRetentionPoint(
    std::unique_ptr<RetentionPoint>* out) {
  if (out == nullptr) return Status::InvalidArgument("out is null");
  if (!opened_) return Status::ShutdownInProgress("db not open");
  const LSMengine::Snapshot* snap = db_->GetSnapshot();
  if (snap == nullptr) {
    return Status::Internal("failed to create snapshot");
  }
  out->reset(new RetentionPoint(snap, db_));
  return Status::OK();
}

Status StratumDB::Flush() {
  if (!opened_) return Status::ShutdownInProgress("db not open");
  // 【P5 修复】早期实现调 CompactRange(nullptr, nullptr) ——
  // 那是**全库 compaction**（归并所有层）。Recorder 的 writer
  // 线程每 200ms 空闲时调一次 Flush，等于每 200ms 全库归并，
  // 灾难性写放大。
  //
  // 数据持久性由 WAL 保证（重启时重放），Flush 的语义应收缩为
  // "把 WAL 刷到磁盘"。LSMengine 无公开的轻量 Flush API，
  // 这里用"写一个固定 marker key + sync=true"实现 WAL fsync。
  LSMengine::WriteOptions wo;
  wo.sync = true;
  LSMengine::WriteBatch batch;
  batch.Put(MetaKey("flush_marker"), std::string("1"));
  LSMengine::Status s = db_->Write(wo, &batch);
  if (!s.ok()) {
    return Status::IOError("flush marker failed: " + s.ToString());
  }
  return Status::OK();
}

Status StratumDB::Compact() {
  if (!opened_) return Status::ShutdownInProgress("db not open");
  db_->CompactRange(nullptr, nullptr);
  return Status::OK();
}


Status StratumDB::MetaPut(const std::string& key, const std::string& value) {
  if (!opened_) return Status::ShutdownInProgress("db not open");
  LSMengine::WriteOptions wo;
  wo.sync = true;  // 元数据必须持久
  LSMengine::Status s = db_->Put(wo, MetaKey(key), value);
  if (!s.ok()) {
    return Status::IOError("meta put failed: " + s.ToString());
  }
  return Status::OK();
}

Status StratumDB::MetaGet(const std::string& key, std::string* value) {
  if (value == nullptr) return Status::InvalidArgument("value is null");
  if (!opened_) return Status::ShutdownInProgress("db not open");
  LSMengine::ReadOptions ro;
  LSMengine::Status s = db_->Get(ro, MetaKey(key), value);
  if (s.IsNotFound()) {
    return Status::NotFound("meta key not found");
  }
  if (!s.ok()) {
    return Status::IOError("meta get failed: " + s.ToString());
  }
  return Status::OK();
}

StratumDB::Stats StratumDB::GetStats() {
  Stats stats;
  stats.writes = writes_.load(std::memory_order_relaxed);
  stats.total_bytes = bytes_written_.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace lsm_v2
}  // namespace storage
}  // namespace stratum