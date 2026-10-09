// 最小复现：LSM 覆盖写与范围扫描
#include <cstdio>
#include <string>

#include "stratum/storage/lsm/lsm_engine.h"

using namespace stratum;
using namespace stratum::storage;

int main() {
  system("rm -rf /tmp/lsm_min");

  auto e = StorageEnginePtr(new LsmEngine());
  Status s = e->Open("/tmp/lsm_min");
  std::printf("open: %s\n", s.ToString().c_str());

  WriteOptions wo;
  s = e->Put(wo, "k1", "v1");
  std::printf("put v1: %s\n", s.ToString().c_str());

  std::string out;
  ReadOptions ro;
  s = e->Get(ro, "k1", &out);
  std::printf("get after v1: %s value=%s\n", s.ToString().c_str(),
              out.c_str());

  s = e->Put(wo, "k1", "v2");
  std::printf("put v2: %s\n", s.ToString().c_str());

  out.clear();
  s = e->Get(ro, "k1", &out);
  std::printf("get after v2: %s value=%s\n", s.ToString().c_str(),
              out.c_str());
  e->Close();

  // 重开，验证从磁盘恢复
  auto e2 = StorageEnginePtr(new LsmEngine());
  s = e2->Open("/tmp/lsm_min");
  std::printf("reopen: %s\n", s.ToString().c_str());
  out.clear();
  s = e2->Get(ro, "k1", &out);
  std::printf("get after reopen: %s value=%s\n", s.ToString().c_str(),
              out.c_str());
  e2->Close();
  return 0;
}
