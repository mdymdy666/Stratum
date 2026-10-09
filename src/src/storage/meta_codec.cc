// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 元数据编解码实现

#include "stratum/storage/meta_codec.h"

#include <cstring>

namespace stratum {
namespace storage {

// 本文件支持的元数据格式版本（与 core/timeline_impl.h 保持一致）。
// 就地定义以避免头文件循环依赖。
constexpr uint32_t kCurrentFormatVersion = 1;

namespace {

// ---------------------------------------------------------------------------
// 小端读写工具（元数据不参与范围扫描，无需大端）
// ---------------------------------------------------------------------------
void PutU32(std::string* out, uint32_t v) {
  char buf[4];
  buf[0] = static_cast<char>(v & 0xFF);
  buf[1] = static_cast<char>((v >> 8) & 0xFF);
  buf[2] = static_cast<char>((v >> 16) & 0xFF);
  buf[3] = static_cast<char>((v >> 24) & 0xFF);
  out->append(buf, 4);
}

void PutU64(std::string* out, uint64_t v) {
  char buf[8];
  for (int i = 0; i < 8; ++i) {
    buf[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
  }
  out->append(buf, 8);
}

void PutI64(std::string* out, int64_t v) {
  PutU64(out, static_cast<uint64_t>(v));
}

void PutString(std::string* out, const std::string& s) {
  PutU32(out, static_cast<uint32_t>(s.size()));
  out->append(s);
}

// 读取游标：所有读取都经过它，保证越界可检测
class Reader {
 public:
  explicit Reader(const std::string& data) : data_(data), pos_(0) {}

  bool ReadU32(uint32_t* out) {
    if (pos_ + 4 > data_.size()) return false;
    *out = static_cast<uint32_t>(static_cast<uint8_t>(data_[pos_])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data_[pos_ + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data_[pos_ + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data_[pos_ + 3])) << 24);
    pos_ += 4;
    return true;
  }

  bool ReadU64(uint64_t* out) {
    if (pos_ + 8 > data_.size()) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<uint64_t>(static_cast<uint8_t>(data_[pos_ + i])) << (8 * i);
    }
    pos_ += 8;
    *out = v;
    return true;
  }

  bool ReadI64(int64_t* out) {
    uint64_t v = 0;
    if (!ReadU64(&v)) return false;
    *out = static_cast<int64_t>(v);
    return true;
  }

  bool ReadString(std::string* out) {
    uint32_t len = 0;
    if (!ReadU32(&len)) return false;
    if (pos_ + len > data_.size()) return false;
    out->assign(data_, pos_, len);
    pos_ += len;
    return true;
  }

  size_t remaining() const { return data_.size() - pos_; }
  bool AtEnd() const { return pos_ == data_.size(); }

 private:
  const std::string& data_;
  size_t pos_;
};

// 校验：解码结束后必须正好读完
Status CheckFullyConsumed(const Reader& r, const char* what) {
  if (!r.AtEnd()) {
    return Status::Corruption(std::string(what) + ": trailing bytes detected");
  }
  return Status::OK();
}

}  // namespace

// ---------------------------------------------------------------------------
// 质量统计
// ---------------------------------------------------------------------------
namespace {

void PutQuality(std::string* out, const QualityStats& q) {
  PutU64(out, q.dropped_messages);
  PutU64(out, q.dropped_bytes);
  PutU64(out, q.first_drop_ns);
  PutU64(out, q.last_drop_ns);
  PutU64(out, q.timestamp_regressions);
  PutU64(out, q.clock_jumps);
  PutU64(out, q.sequence_gaps);
  PutU64(out, q.missing_messages);
  PutU64(out, q.interruptions);
  PutI64(out, q.min_latency_ns);
  PutI64(out, q.max_latency_ns);
  PutI64(out, q.total_latency_ns);
  PutU64(out, q.latency_samples);
}

Status ReadQuality(Reader* r, QualityStats* q) {
  bool ok = r->ReadU64(&q->dropped_messages) &&
            r->ReadU64(&q->dropped_bytes) &&
            r->ReadU64(&q->first_drop_ns) &&
            r->ReadU64(&q->last_drop_ns) &&
            r->ReadU64(&q->timestamp_regressions) &&
            r->ReadU64(&q->clock_jumps) &&
            r->ReadU64(&q->sequence_gaps) &&
            r->ReadU64(&q->missing_messages) &&
            r->ReadU64(&q->interruptions) &&
            r->ReadI64(&q->min_latency_ns) &&
            r->ReadI64(&q->max_latency_ns) &&
            r->ReadI64(&q->total_latency_ns) &&
            r->ReadU64(&q->latency_samples);
  if (!ok) {
    return Status::Corruption("quality stats truncated");
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 话题元数据
// ---------------------------------------------------------------------------
std::string EncodeTopicInternal(const TopicMetadata& m) {
  std::string out;
  PutU32(&out, m.topic_id);
  PutString(&out, m.topic_name);
  PutString(&out, m.type_name);
  PutString(&out, m.type_definition);
  PutU32(&out, static_cast<uint32_t>(m.qos.size()));
  for (const auto& kv : m.qos) {
    PutString(&out, kv.first);
    PutString(&out, kv.second);
  }
  PutU64(&out, m.first_seen_ns);
  PutU64(&out, m.last_seen_ns);
  PutU64(&out, m.message_count);
  PutU64(&out, m.byte_count);
  PutU32(&out, m.active ? 1u : 0u);
  return out;
}

Status DecodeTopicInternal(const std::string& raw, TopicMetadata* m) {
  Reader r(raw);
  if (!r.ReadU32(&m->topic_id)) {
    return Status::Corruption("topic: topic_id truncated");
  }
  if (!r.ReadString(&m->topic_name) || !r.ReadString(&m->type_name) ||
      !r.ReadString(&m->type_definition)) {
    return Status::Corruption("topic: strings truncated");
  }
  uint32_t qos_count = 0;
  if (!r.ReadU32(&qos_count)) {
    return Status::Corruption("topic: qos count truncated");
  }
  // 防御：恶意/损坏数据可能声明巨大的条目数，导致内存爆炸
  if (qos_count > 10000) {
    return Status::Corruption("topic: unreasonable qos count");
  }
  m->qos.clear();
  for (uint32_t i = 0; i < qos_count; ++i) {
    std::string k;
    std::string v;
    if (!r.ReadString(&k) || !r.ReadString(&v)) {
      return Status::Corruption("topic: qos entry truncated");
    }
    m->qos.emplace(std::move(k), std::move(v));
  }
  uint32_t active = 0;
  if (!r.ReadU64(&m->first_seen_ns) || !r.ReadU64(&m->last_seen_ns) ||
      !r.ReadU64(&m->message_count) || !r.ReadU64(&m->byte_count) ||
      !r.ReadU32(&active)) {
    return Status::Corruption("topic: trailing fields truncated");
  }
  m->active = (active != 0);
  return CheckFullyConsumed(r, "topic");
}

}  // namespace

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------
std::string MetaCodec::EncodeSession(const SessionMetadata& meta) {
  std::string out;
  PutU32(&out, meta.format_version);
  PutString(&out, meta.stratum_version);
  PutString(&out, meta.session_name);
  PutU64(&out, meta.created_at_ns);
  PutU64(&out, meta.closed_at_ns);
  PutU64(&out, meta.total_events);
  PutU64(&out, meta.total_bytes);
  PutQuality(&out, meta.quality);
  return out;
}

Status MetaCodec::DecodeSession(const std::string& raw, SessionMetadata* out) {
  if (out == nullptr) {
    return Status::InvalidArgument("out is null");
  }

  Reader r(raw);
  SessionMetadata meta;

  if (!r.ReadU32(&meta.format_version)) {
    return Status::Corruption("session: truncated header");
  }
  // 格式版本检查：遇到更高版本必须拒绝（FR-S-2），而不是猜测解析
  if (meta.format_version > kCurrentFormatVersion) {
    return Status::Corruption(
        "session: format version " + std::to_string(meta.format_version) +
        " is newer than supported version " +
        std::to_string(kCurrentFormatVersion));
  }

  if (!r.ReadString(&meta.stratum_version) || !r.ReadString(&meta.session_name) ||
      !r.ReadU64(&meta.created_at_ns) || !r.ReadU64(&meta.closed_at_ns) ||
      !r.ReadU64(&meta.total_events) || !r.ReadU64(&meta.total_bytes)) {
    return Status::Corruption("session: truncated fields");
  }

  Status s = ReadQuality(&r, &meta.quality);
  if (!s.ok()) {
    return s;
  }

  s = CheckFullyConsumed(r, "session");
  if (!s.ok()) {
    return s;
  }

  *out = std::move(meta);
  return Status::OK();
}

std::string MetaCodec::EncodeTopic(const TopicMetadata& meta) {
  return EncodeTopicInternal(meta);
}

Status MetaCodec::DecodeTopic(const std::string& raw, TopicMetadata* out) {
  if (out == nullptr) {
    return Status::InvalidArgument("out is null");
  }
  TopicMetadata meta;
  Status s = DecodeTopicInternal(raw, &meta);
  if (!s.ok()) {
    return s;
  }
  *out = std::move(meta);
  return Status::OK();
}

std::string MetaCodec::EncodeRetention(const RetentionPolicy& policy) {
  std::string out;
  PutU64(&out, policy.retain_duration_ns);
  PutU64(&out, policy.max_bytes);
  PutU64(&out, policy.check_interval_ns);
  return out;
}

Status MetaCodec::DecodeRetention(const std::string& raw,
                                  RetentionPolicy* out) {
  if (out == nullptr) {
    return Status::InvalidArgument("out is null");
  }
  Reader r(raw);
  RetentionPolicy policy;
  if (!r.ReadU64(&policy.retain_duration_ns) || !r.ReadU64(&policy.max_bytes) ||
      !r.ReadU64(&policy.check_interval_ns)) {
    return Status::Corruption("retention: truncated");
  }
  Status s = CheckFullyConsumed(r, "retention");
  if (!s.ok()) {
    return s;
  }
  *out = policy;
  return Status::OK();
}

std::string MetaCodec::MakeTopicKey(uint32_t topic_id) {
  // 固定宽度十六进制，保证字典序 == 数值序（便于遍历所有话题）
  static const char* kHex = "0123456789abcdef";
  std::string key(kMetaTopicPrefix);
  for (int shift = 28; shift >= 0; shift -= 4) {
    key.push_back(kHex[(topic_id >> shift) & 0xF]);
  }
  return key;
}

}  // namespace storage
}  // namespace stratum
