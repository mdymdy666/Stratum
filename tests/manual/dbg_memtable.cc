// 调试：直接验证 MemTable 的读写
#include <cstdio>
#include <string>

#include "stratum/storage/lsm/memtable.h"

using namespace stratum::storage::lsm;

int main() {
  MemTable m;
  m.Add(1, kTypeValue, "k1", "v1");
  std::printf("count after add = %zu\n", m.Count());

  std::string out;
  bool del = false;
  bool found = m.Get("k1", kMaxSequenceNumber, &out, &del);
  std::printf("Get(k1, maxseq) found=%d value=%s\n", found ? 1 : 0, out.c_str());

  found = m.Get("k1", 1, &out, &del);
  std::printf("Get(k1, seq=1) found=%d value=%s\n", found ? 1 : 0, out.c_str());

  m.Add(2, kTypeValue, "k1", "v2");
  std::printf("count after overwrite = %zu\n", m.Count());

  found = m.Get("k1", kMaxSequenceNumber, &out, &del);
  std::printf("Get(k1, maxseq) after v2: found=%d value=%s\n", found ? 1 : 0,
              out.c_str());

  // 打印内部键的字节，确认编码
  const std::string ik = MakeInternalKey("k1", 1, kTypeValue);
  std::printf("internal key size = %zu bytes, hex = ", ik.size());
  for (unsigned char c : ik) {
    std::printf("%02x ", c);
  }
  std::printf("\n");

  std::string uk;
  uint64_t seq = 0;
  ValueType t = kTypeValue;
  const bool ok = ParseInternalKey(ik, &uk, &seq, &t);
  std::printf("parse: ok=%d user_key=%s seq=%llu type=%d\n", ok ? 1 : 0,
              uk.c_str(), static_cast<unsigned long long>(seq),
              static_cast<int>(t));
  return 0;
}
