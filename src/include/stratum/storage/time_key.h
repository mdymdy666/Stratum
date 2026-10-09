// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 时间轴键编码（TimeKey）
//
// 这是 Stratum 存储层的**核心数据结构**。整个"可回溯数据平面"的能力
// 都建立在"key 的字节序等于时间序"这一性质上。
//
// ===========================================================================
// 为什么这样设计
// ===========================================================================
//
// 目标：让"按时间范围扫描"成为 LSM 的最优操作（一次 seek + 顺序读）。
//
// 手段：把时间戳放在 key 的**高位**，并用**大端编码**保证
//       "字节序 == 数值序"。
//
// key 布局（全部大端）：
//
//   offset  size  field
//   ------  ----  ------------------------------------------------------
//        0     4  magic/version（便于未来格式演进时识别）
//        4     4  topic_id（话题标识，由元数据表分配，稳定不变）
//        8     8  timestamp_ns（纳秒时间戳，Unix epoch）
//       16     4  seq_in_ts（同一时间戳内的去重序号）
//   ------  ----  ------------------------------------------------------
//        20       固定长度，无变长部分
//
// 固定长度的好处：
//   - 前缀比较可提前短路
//   - 不需要长度字段，解析零成本
//   - 便于用 topic_id + 时间范围构造区间边界（见 MakeRange）
//
// ===========================================================================
// 与存储后端的契约
// ===========================================================================
//
// 本编码**不依赖任何后端特性**：
//   - 不要求后端理解时间语义
//   - 不要求自定义 Comparator（字节序排序即可）
//   - 因此所有符合 IStorageEngine 的后端都能直接使用
//
// 这正是"同时间戳去重不需要改后端"的原因：去重因子 seq_in_ts 是
// key 的一部分，后端只看到普通的有序字节串。

#ifndef STRATUM_STORAGE_TIME_KEY_H_
#define STRATUM_STORAGE_TIME_KEY_H_

#include <cstdint>
#include <cstring>
#include <string>

namespace stratum {
namespace storage {

// 编码后的固定长度
constexpr size_t kTimeKeySize = 20;

// 格式版本，写在 key 头部。将来格式演进时用于识别。
constexpr uint32_t kTimeKeyMagic = 0x53545231u;  // "STR1"

// ---------------------------------------------------------------------------
// 解析后的时间键
// ---------------------------------------------------------------------------
struct TimeKey {
  uint32_t topic_id = 0;
  uint64_t timestamp_ns = 0;
  uint32_t seq_in_ts = 0;

  TimeKey() = default;

  TimeKey(uint32_t topic, uint64_t ts, uint32_t seq)
      : topic_id(topic), timestamp_ns(ts), seq_in_ts(seq) {}

  // 全序比较（与字节序一致）
  bool operator<(const TimeKey& o) const {
    if (topic_id != o.topic_id) return topic_id < o.topic_id;
    if (timestamp_ns != o.timestamp_ns) return timestamp_ns < o.timestamp_ns;
    return seq_in_ts < o.seq_in_ts;
  }
  bool operator==(const TimeKey& o) const {
    return topic_id == o.topic_id && timestamp_ns == o.timestamp_ns &&
           seq_in_ts == o.seq_in_ts;
  }

  std::string DebugString() const;
};

// ---------------------------------------------------------------------------
// 编解码
//
// 全部为静态纯函数，无状态，线程安全。
// ---------------------------------------------------------------------------
class TimeKeyCodec {
 public:
  // --- 编码 ---------------------------------------------------------------

  // 编码为 20 字节。
  static std::string Encode(const TimeKey& key);

  // 便捷重载
  static std::string Encode(uint32_t topic_id, uint64_t timestamp_ns,
                            uint32_t seq_in_ts) {
    return Encode(TimeKey(topic_id, timestamp_ns, seq_in_ts));
  }

  // --- 解码 ---------------------------------------------------------------

  // 解析。失败（长度不对、magic 不匹配）返回 false，out 不被修改。
  static bool Decode(const std::string& raw, TimeKey* out);

  // 判断是否像一个合法的时间键（不带完整解析的开销）
  static bool LooksLikeTimeKey(const std::string& raw);

  // --- 范围构造（扫描的关键）----------------------------------------------

  // 构造某个话题的完整区间 [begin, end)：
  //   begin = (topic_id, 0, 0)
  //   end   = (topic_id + 1, 0, 0)   —— 因为固定长度，这样恰好覆盖该 topic 全部键
  //
  // 注意：topic_id 为 0xFFFFFFFF 时 end 会溢出，此时 end 返回空串表示"无上界"。
  static void MakeTopicRange(uint32_t topic_id, std::string* begin,
                             std::string* end);

  // 构造某个话题在 [start_ns, end_ns) 时间区间内的扫描范围。
  // start_ts 为 0 表示从头；end_ts 为 0 表示到结尾。
  static void MakeTimeRange(uint32_t topic_id, uint64_t start_ns,
                            uint64_t end_ns, std::string* begin,
                            std::string* end);

  // 构造"小于等于某时刻"的上界（用于定点查询：查 ts 时刻的状态）。
  // 返回的 end 是**开区间上界**，即所有 timestamp <= ts 的键都 < end。
  static void MakePrefixRangeUpTo(uint32_t topic_id, uint64_t ts_inclusive,
                                  std::string* begin, std::string* end);

  // --- 从原始 key 提取用于比较的字段（零拷贝友好）--------------------------

  static uint32_t PeekTopicId(const std::string& raw);
  static uint64_t PeekTimestamp(const std::string& raw);
  static uint32_t PeekSeqInTs(const std::string& raw);
};

// ---------------------------------------------------------------------------
// 大端编解码工具（内部使用，也供其他模块复用）
// ---------------------------------------------------------------------------
inline void EncodeFixed32BE(char* dst, uint32_t value) {
  dst[0] = static_cast<char>((value >> 24) & 0xFF);
  dst[1] = static_cast<char>((value >> 16) & 0xFF);
  dst[2] = static_cast<char>((value >> 8) & 0xFF);
  dst[3] = static_cast<char>(value & 0xFF);
}

inline void EncodeFixed64BE(char* dst, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    dst[i] = static_cast<char>((value >> (56 - 8 * i)) & 0xFF);
  }
}

inline uint32_t DecodeFixed32BE(const char* src) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(src[0])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(src[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(src[2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(src[3]));
}

inline uint64_t DecodeFixed64BE(const char* src) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<uint8_t>(src[i]);
  }
  return value;
}

}  // namespace storage
}  // namespace stratum

#endif  // STRATUM_STORAGE_TIME_KEY_H_
