// 调试：LSM 范围扫描（修正类型转换）
#include <cstdio>
#include <cstdlib>
#include <string>

#include "stratum/storage/istorage_engine.h"
#include "stratum/storage/lsm/lsm_engine.h"

using namespace stratum;
using namespace stratum::storage;

int main() {
  if (system("rm -rf /tmp/lsm_range") != 0) {
    // 忽略
  }

  LsmEngine engine;
  Status s = engine.Open("/tmp/lsm_range");
  std::printf("open: %s\n", s.ToString().c_str());

  WriteOptions wo;
  for (int i = 0; i < 20; ++i) {
    char k[32];
    std::snprintf(k, sizeof(k), "ord-%04d", i);
    s = engine.Put(wo, k, "v");
    if (!s.ok()) std::printf("put %s failed: %s\n", k, s.ToString().c_str());
  }
  std::printf("MemTableCount = %zu\n", engine.MemTableCount());

  ReadOptions ro;
  auto it = engine.NewIterator(ro, "ord-", "ord0");
  int n = 0;
  for (; it->Valid(); it->Next()) {
    if (n < 3) std::printf("  [%d] key='%s'\n", n, it->key().c_str());
    ++n;
  }
  std::printf("full range count = %d (expect 20)\n", n);

  auto it2 = engine.NewIterator(ro, "ord-0005", "ord-0010");
  int n2 = 0;
  for (; it2->Valid(); it2->Next()) ++n2;
  std::printf("sub range count = %d (expect 5)\n", n2);

  engine.Close();
  return 0;
}
