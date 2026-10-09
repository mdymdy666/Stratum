// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// TimeKey 编解码实现

#include "stratum/storage/time_key.h"

#include <cstdio>

namespace stratum {
namespace storage {

std::string TimeKey::DebugString() const {
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "TimeKey{topic=%u, ts=%llu, seq=%u}", topic_id,
                static_cast<unsigned long long>(timestamp_ns), seq_in_ts);
  return std::string(buf);
}

// ---------------------------------------------------------------------------
// 编码
// ---------------------------------------------------------------------------
std::string TimeKeyCodec::Encode(const TimeKey& key) {
  std::string out;
  out.resize(kTimeKeySize);

  char* p = &out[0];
  EncodeFixed32BE(p + 0, kTimeKeyMagic);
  EncodeFixed32BE(p + 4, key.topic_id);
  EncodeFixed64BE(p + 8, key.timestamp_ns);
  EncodeFixed32BE(p + 16, key.seq_in_ts);

  return out;
}

// ---------------------------------------------------------------------------
// 解码
// ---------------------------------------------------------------------------
bool TimeKeyCodec::Decode(const std::string& raw, TimeKey* out) {
  if (out == nullptr || raw.size() != kTimeKeySize) {
    return false;
  }
  const char* p = raw.data();
  if (DecodeFixed32BE(p) != kTimeKeyMagic) {
    return false;
  }

  // 先解析到临时变量，全部成功后再写入 out，保证失败时 out 不被修改。
  TimeKey parsed;
  parsed.topic_id = DecodeFixed32BE(p + 4);
  parsed.timestamp_ns = DecodeFixed64BE(p + 8);
  parsed.seq_in_ts = DecodeFixed32BE(p + 16);

  *out = parsed;
  return true;
}

bool TimeKeyCodec::LooksLikeTimeKey(const std::string& raw) {
  return raw.size() == kTimeKeySize &&
         DecodeFixed32BE(raw.data()) == kTimeKeyMagic;
}

// ---------------------------------------------------------------------------
// 范围构造
//
// 关键性质：因为 key 是定长的，且字段按 (topic, ts, seq) 排列，
// 所以 [ (topic,0,0), (topic+1,0,0) ) 恰好覆盖该 topic 的所有键。
// ---------------------------------------------------------------------------
void TimeKeyCodec::MakeTopicRange(uint32_t topic_id, std::string* begin,
                                  std::string* end) {
  if (begin != nullptr) {
    *begin = Encode(topic_id, 0, 0);
  }
  if (end != nullptr) {
    if (topic_id == 0xFFFFFFFFu) {
      // 话题号已达上界，无法构造"下一个话题"的键，用空串表示无上界。
      end->clear();
    } else {
      *end = Encode(topic_id + 1, 0, 0);
    }
  }
}

void TimeKeyCodec::MakeTimeRange(uint32_t topic_id, uint64_t start_ns,
                                 uint64_t end_ns, std::string* begin,
                                 std::string* end) {
  // begin：闭区间起点
  if (begin != nullptr) {
    *begin = Encode(topic_id, start_ns, 0);
  }

  // end：开区间上界
  if (end != nullptr) {
    if (end_ns == 0) {
      // 无上界：用"下一个话题"作为上界（定长 key 的性质保证正确）
      if (topic_id == 0xFFFFFFFFu) {
        end->clear();
      } else {
        *end = Encode(topic_id + 1, 0, 0);
      }
    } else {
      // 上界是 end_ns 时刻的第一个键（不含该时刻）
      *end = Encode(topic_id, end_ns, 0);
    }
  }
}

void TimeKeyCodec::MakePrefixRangeUpTo(uint32_t topic_id, uint64_t ts_inclusive,
                                       std::string* begin,
                                       std::string* end) {
  // 该函数用于"查 ts 时刻（含）之前的状态"。
  // 需要一个**开区间上界**，使所有 timestamp <= ts_inclusive 的键都严格小于它。
  //
  // 做法：取 ts_inclusive + 1 时刻的第一个键作为上界。
  // 由于 seq_in_ts 是 4 字节无符号，ts_inclusive 时刻的全部键都 <
  // (topic, ts_inclusive+1, 0)。
  if (begin != nullptr) {
    *begin = Encode(topic_id, 0, 0);
  }
  if (end != nullptr) {
    const uint64_t bound = ts_inclusive + 1;  // 溢出时回绕，由下分支兜底
    if (ts_inclusive == UINT64_MAX || bound == 0) {
      if (topic_id == 0xFFFFFFFFu) {
        end->clear();
      } else {
        *end = Encode(topic_id + 1, 0, 0);
      }
    } else {
      *end = Encode(topic_id, bound, 0);
    }
  }
}

// ---------------------------------------------------------------------------
// 字段窥探（调用前应确保 LooksLikeTimeKey 为真）
// ---------------------------------------------------------------------------
uint32_t TimeKeyCodec::PeekTopicId(const std::string& raw) {
  return raw.size() >= 8 ? DecodeFixed32BE(raw.data() + 4) : 0;
}

uint64_t TimeKeyCodec::PeekTimestamp(const std::string& raw) {
  return raw.size() >= kTimeKeySize ? DecodeFixed64BE(raw.data() + 8) : 0;
}

uint32_t TimeKeyCodec::PeekSeqInTs(const std::string& raw) {
  return raw.size() >= kTimeKeySize ? DecodeFixed32BE(raw.data() + 16) : 0;
}

}  // namespace storage
}  // namespace stratum
