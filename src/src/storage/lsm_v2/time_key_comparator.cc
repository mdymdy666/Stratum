// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0

#include "stratum/storage/lsm_v2/time_key_comparator.h"

#include <algorithm>
#include <cstring>

namespace stratum {
namespace storage {
namespace lsm_v2 {

int TimeKeyComparator::Compare(const LSMengine::Slice& a,
                               const LSMengine::Slice& b) const {
  const size_t n = std::min(a.size(), b.size());
  if (n > 0) {
    const int cmp = std::memcmp(a.data(), b.data(), n);
    if (cmp != 0) {
      return cmp;
    }
  }
  // 前缀相同：短者更小。TimeKey 定长 20 字节时不会走到这里，
  // 此分支只是保证任意输入下都满足严格弱序。
  if (a.size() < b.size()) return -1;
  if (a.size() > b.size()) return 1;
  return 0;
}

}  // namespace lsm_v2
}  // namespace storage
}  // namespace stratum
