// C3 验证：并发写 + 迭代读必须安全
//
// 修复前（TSAN 实证）：MemoryIterator 持 &map_ 裸指针 + 锁外遍历，
// 与并发 Put 的 versions.insert 形成数据竞争，ASAN 读到损坏值。
// 修复后：迭代器物化快照，无共享状态。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "stratum/storage/memory/memory_engine.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::storage;

namespace {

// 验证 value 合法（写线程只写 "v-<n>" 格式）
bool IsValidValue(const std::string& v) {
  if (v.empty()) return true;
  if (v.size() < 3) return false;
  if (v[0] != 'v' || v[1] != '-') return false;
  for (size_t i = 2; i < v.size(); ++i) {
    if (v[i] < '0' || v[i] > '9') return false;
  }
  return true;
}

}  // namespace

STRATUM_TEST(Concurrency, C3_ConcurrentWriteAndIterateIsSafe) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  STRATUM_CHECK_OK(engine->Open("mem://c3"));

  std::atomic<bool> stop{false};
  std::atomic<int> read_rounds{0};
  std::atomic<int> bad_values{0};
  std::atomic<bool> crashed{false};

  // 写线程：持续 Put（小 key 空间制造频繁 versions.insert → vector 重分配）
  std::thread writer([&]() {
    WriteOptions wo;
    for (int i = 0; i < 100000; ++i) {
      const std::string key = "k" + std::to_string(i % 50);
      const std::string value = "v-" + std::to_string(i);
      engine->Put(wo, key, value);
    }
    stop.store(true);
  });

  // 读线程：反复创建迭代器并全遍历
  std::thread reader([&]() {
    ReadOptions ro;
    while (!stop.load()) {
      auto it = engine->NewIterator(ro, "", "");
      for (; it->Valid(); it->Next()) {
        const std::string v = it->value();
        // 任何时刻读到的值都必须是合法的（旧值或新值，绝不能损坏）
        if (!IsValidValue(v)) {
          bad_values.fetch_add(1);
        }
      }
      if (!it->status().ok()) {
        bad_values.fetch_add(1);
      }
      read_rounds.fetch_add(1);
    }
  });

  writer.join();
  reader.join();

  ctx.param_desc = "读轮次=" + std::to_string(read_rounds.load()) +
                   " 非法值=" + std::to_string(bad_values.load());
  // 修复前：数据竞争导致读到损坏值（ASAN 实证）/ 遍历失控
  STRATUM_CHECK_EQ(bad_values.load(), 0);
  STRATUM_CHECK_TRUE(read_rounds.load() > 0);

  engine->Close();
}

// 快照视图在并发写下的正确性
STRATUM_TEST(Concurrency, C3_SnapshotViewUnderConcurrentWrites) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  STRATUM_CHECK_OK(engine->Open("mem://c3b"));

  WriteOptions wo;
  STRATUM_CHECK_OK(engine->Put(wo, "sk", "before"));

  SnapshotPtr snap = engine->GetSnapshot();

  // 并发大量写（触发版本链增长与潜在裁剪）
  std::thread writer([&]() {
    WriteOptions w2;
    for (int i = 0; i < 50000; ++i) {
      engine->Put(w2, "sk", "after-" + std::to_string(i));
    }
  });
  writer.join();

  // 快照视图必须仍看到 "before"
  ReadOptions snap_ro;
  snap_ro.snapshot = snap;
  std::string v;
  STRATUM_CHECK_OK(engine->Get(snap_ro, "sk", &v));
  ctx.param_desc = "快照视图值=" + v;
  STRATUM_CHECK_EQ(v, std::string("before"));

  // 迭代器 + 快照同样正确
  auto it = engine->NewIterator(snap_ro, "sk", "sl");
  int n = 0;
  for (; it->Valid(); it->Next()) {
    if (it->key() == "sk") {
      STRATUM_CHECK_EQ(it->value(), std::string("before"));
      ++n;
    }
  }
  ctx.param_desc += " 快照迭代命中=" + std::to_string(n);
  STRATUM_CHECK_EQ(n, 1);

  engine->ReleaseSnapshot(snap);
  engine->Close();
}

STRATUM_TEST_MAIN()
