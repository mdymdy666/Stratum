// 深入调试：修正类型后重跑
#include <cstdio>
#include <string>
#include <vector>

#include "stratum/storage/lsm/memtable.h"

using namespace stratum::storage::lsm;

static void PrintKey(const char* label, const std::string& k) {
  std::printf("%s (size=%zu): ", label, k.size());
  for (unsigned char c : k) std::printf("%02x ", c);
  std::printf("\n");
}

int main() {
  MemTable m;
  m.Add(1, kTypeValue, "k1", "v1");

  const std::string seek = MakeInternalKey("k1", kMaxSequenceNumber, kTypeValue);
  const std::string actual = MakeInternalKey("k1", 1, kTypeValue);
  PrintKey("seek  ", seek);
  PrintKey("actual", actual);

  std::printf("kMaxSequenceNumber = %llu\n",
              static_cast<unsigned long long>(kMaxSequenceNumber));
  std::printf("actual < seek (std::string) = %s\n",
              actual < seek ? "true" : "false");
  std::printf("CompareInternalKey(actual, seek) = %d\n",
              CompareInternalKey(actual, seek));

  std::printf("--- Get 探测 ---\n");
  std::string out;
  bool del = false;
  const std::vector<uint64_t> snaps = {kMaxSequenceNumber, 100ULL, 10ULL, 1ULL,
                                       0ULL};
  for (uint64_t snap : snaps) {
    out.clear();
    const bool f = m.Get("k1", snap, &out, &del);
    std::printf("  snapshot=%llu -> found=%d value='%s'\n",
                static_cast<unsigned long long>(snap), f ? 1 : 0, out.c_str());
  }
  return 0;
}
