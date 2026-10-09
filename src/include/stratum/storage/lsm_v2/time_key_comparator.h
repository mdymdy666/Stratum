// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// TimeKey 比较器：把 LSMengine（LevelDB）的排序规则改为时间轴序
//
// ===========================================================================
// 设计依据（ref-01 精读报告第 9 节）
// ===========================================================================
//
// TimeKey（20 字节定长大端）= topic_id(4) | timestamp_ns(8) | seq_in_ts(4)
// 已经满足"字节序 == 时间序"。
//
// 因此本 Comparator 的 Compare 就是 20 字节 memcmp，且：
//   - FindShortestSeparator / FindShortSuccessor 直接 no-op
//     （TimeKey 定长，无共享前缀可缩短，缩短反而破坏定长不变量）
//   - Name 不能用 "leveldb." 前缀（LevelDB 用前缀识别内建比较器）
//
// 配合 LSMengine 的 InternalKeyComparator：
//   user_key 序 = 本比较器序（时间轴序）
//   → 全库有序遍历即时间轴遍历
//   → 范围查询 [t1, t2) 直接映射为 internal key 范围，零索引转换

#ifndef STRATUM_STORAGE_LSM_V2_TIME_KEY_COMPARATOR_H_
#define STRATUM_STORAGE_LSM_V2_TIME_KEY_COMPARATOR_H_

#include "LSMengine/comparator.h"
#include "LSMengine/slice.h"

namespace stratum {
namespace storage {
namespace lsm_v2 {

// 在任意 key 集合上强制 TimeKey 序的比较器。
// （注意：全库混用元数据 key 与事件 key 时，元数据 key 也必须是
// 20 字节 TimeKey 形态 —— 用 magic 字段区分命名空间。）
class TimeKeyComparator : public LSMengine::Comparator {
 public:
  TimeKeyComparator() = default;

  const char* Name() const override { return "stratum.TimeKeyComparator"; }

  // 20 字节定长大端，直接 memcmp。
  // 若两个 key 长度不同（不应发生，元数据 key 也补齐 20 字节），
  // 按"短者更小 + 前缀比较"兜底，保证严格弱序。
  int Compare(const LSMengine::Slice& a, const LSMengine::Slice& b) const override;

  // 定长 key 无可缩短的分隔符；返回原样避免破坏定长不变量。
  void FindShortestSeparator(std::string* /*start*/,
                             const LSMengine::Slice& /*limit*/) const override {}

  void FindShortSuccessor(std::string* /*key*/) const override {}
};

}  // namespace lsm_v2
}  // namespace storage
}  // namespace stratum

#endif  // STRATUM_STORAGE_LSM_V2_TIME_KEY_COMPARATOR_H_
