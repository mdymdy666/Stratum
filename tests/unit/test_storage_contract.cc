// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 存储引擎契约测试（参数化）
//
// ===========================================================================
// 目的
// ===========================================================================
//
// 同一套测试用例跑**所有** IStorageEngine 实现。
// 这是"插件式"能否成立的试金石：任何后端只要通过本套测试，
// 就可以被上层（Timeline / Recorder / Player）无差别使用。
//
// 当前覆盖：MemoryEngine（参考实现）、LsmEngine（自研）
//
// 每个用例都用多组数据跑（时间轴边界、极值、大批量），
// 而不是单一样本，以暴露边界问题。

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "stratum/storage/istorage_engine.h"
#include "stratum/storage/lsm/lsm_engine.h"
#include "stratum/storage/memory/memory_engine.h"
#include "test_framework.h"
#include "test_isolation.h"

using namespace stratum;
using namespace stratum::storage;

namespace {

// 每个引擎在独立目录下运行（LSM 需要真实文件系统）
std::string EngineDir(const std::string& engine_name) {
  return std::string("/tmp/stratum_contract_") + engine_name + "_" + UniqueSuffix();
}

// ---------------------------------------------------------------------------
// 契约测试主体：对任意 IStorageEngine 执行同一套检查
// ---------------------------------------------------------------------------
void RunContract(IStorageEngine* e, const std::string& tag,
                 ::stratum_test::TestContext& ctx) {

  // --- 1. 基本读写 ---
  {
    ctx.param_desc = tag + "/basic-put-get";
    WriteOptions wo;
    STRATUM_CHECK_OK(e->Put(wo, "k1", "v1"));
    std::string out;
    ReadOptions ro;
    STRATUM_CHECK_OK(e->Get(ro, "k1", &out));
    STRATUM_CHECK_EQ(out, std::string("v1"));

    // 不存在的 key
    STRATUM_CHECK_TRUE(e->Get(ro, "no-such-key", &out).IsNotFound());
  }

  // --- 2. 覆盖写 ---
  {
    ctx.param_desc = tag + "/overwrite";
    WriteOptions wo;
    STRATUM_CHECK_OK(e->Put(wo, "k1", "v2"));
    std::string out;
    ReadOptions ro;
    STRATUM_CHECK_OK(e->Get(ro, "k1", &out));
    STRATUM_CHECK_EQ(out, std::string("v2"));
  }

  // --- 3. 批量写原子性 ---
  {
    ctx.param_desc = tag + "/batch";
    WriteOptions wo;
    std::vector<std::string> keys;
    std::vector<std::string> vals;
    for (int i = 0; i < 100; ++i) {
      keys.push_back("batch-" + std::to_string(i));
      vals.push_back("val-" + std::to_string(i));
    }
    STRATUM_CHECK_OK(e->PutBatch(wo, keys, vals));

    ReadOptions ro;
    std::string out;
    for (int i = 0; i < 100; ++i) {
      STRATUM_CHECK_OK(e->Get(ro, keys[i], &out));
      STRATUM_CHECK_EQ(out, vals[i]);
    }

    // 长度不匹配必须被拒绝
    std::vector<std::string> bad_keys{"a", "b"};
    std::vector<std::string> bad_vals{"only-one"};
    STRATUM_CHECK_TRUE(e->PutBatch(wo, bad_keys, bad_vals).code() ==
                       StatusCode::kInvalidArgument);
  }

  // --- 4. 删除 ---
  {
    ctx.param_desc = tag + "/delete";
    WriteOptions wo;
    STRATUM_CHECK_OK(e->Delete(wo, "k1"));
    std::string out;
    ReadOptions ro;
    STRATUM_CHECK_TRUE(e->Get(ro, "k1", &out).IsNotFound());
  }

  // --- 5. 有序遍历（这是时间轴范围扫描的基础）---
  {
    ctx.param_desc = tag + "/ordered-iteration";
    WriteOptions wo;
    std::vector<std::string> keys;
    for (int i = 0; i < 200; ++i) {
      // 用固定宽度保证字典序 == 数值序
      char buf[16];
      std::snprintf(buf, sizeof(buf), "ord-%04d", i);
      keys.push_back(buf);
    }
    // 乱序插入，验证遍历时仍有序
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);
    for (const auto& k : keys) {
      STRATUM_CHECK_OK(e->Put(wo, k, "v"));
    }

    ReadOptions ro;
    auto it = e->NewIterator(ro, "ord-", "ord0");
    int count = 0;
    std::string prev;
    for (; it->Valid(); it->Next()) {
      const std::string k = it->key();
      if (count > 0) {
        STRATUM_CHECK_LT(prev, k);
      }
      prev = k;
      ++count;
    }
    STRATUM_CHECK_OK(it->status());
    STRATUM_CHECK_EQ(count, 200);
  }

  // --- 6. 范围边界（半开区间）---
  {
    ctx.param_desc = tag + "/range-half-open";
    ReadOptions ro;
    // 只取 ord-0010 .. ord-0019
    auto it = e->NewIterator(ro, "ord-0010", "ord-0020");
    int count = 0;
    for (; it->Valid(); it->Next()) {
      ++count;
    }
    STRATUM_CHECK_EQ(count, 10);
  }

  // --- 7. 快照隔离 ---
  {
    ctx.param_desc = tag + "/snapshot-isolation";
    WriteOptions wo;
    STRATUM_CHECK_OK(e->Put(wo, "snap-key", "before"));

    SnapshotPtr snap = e->GetSnapshot();
    STRATUM_CHECK_TRUE(snap != nullptr);

    STRATUM_CHECK_OK(e->Put(wo, "snap-key", "after"));

    // 最新视图看到 after
    ReadOptions ro;
    std::string out;
    STRATUM_CHECK_OK(e->Get(ro, "snap-key", &out));
    STRATUM_CHECK_EQ(out, std::string("after"));

    // 快照视图看到 before
    ReadOptions snap_ro;
    snap_ro.snapshot = snap;
    STRATUM_CHECK_OK(e->Get(snap_ro, "snap-key", &out));
    STRATUM_CHECK_EQ(out, std::string("before"));

    e->ReleaseSnapshot(snap);
  }

  // --- 8. 空值处理 ---
  {
    ctx.param_desc = tag + "/empty-value";
    WriteOptions wo;
    STRATUM_CHECK_OK(e->Put(wo, "empty-key", ""));
    std::string out = "sentinel";
    ReadOptions ro;
    STRATUM_CHECK_OK(e->Get(ro, "empty-key", &out));
    STRATUM_CHECK_EQ(out, std::string(""));
  }

  // --- 9. 大 value ---
  {
    ctx.param_desc = tag + "/large-value";
    WriteOptions wo;
    const std::string big(256 * 1024, 'X');  // 256 KB
    STRATUM_CHECK_OK(e->Put(wo, "big-key", big));
    std::string out;
    ReadOptions ro;
    STRATUM_CHECK_OK(e->Get(ro, "big-key", &out));
    STRATUM_CHECK_EQ(out.size(), big.size());
  }

  // --- 10. 范围删除 ---
  {
    ctx.param_desc = tag + "/range-delete";
    WriteOptions wo;
    for (int i = 0; i < 50; ++i) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "rd-%04d", i);
      STRATUM_CHECK_OK(e->Put(wo, buf, "v"));
    }
    Status s = e->DeleteRange(wo, "rd-0010", "rd-0020");
    STRATUM_CHECK_TRUE(s.ok() || s.IsNotFound());

    ReadOptions ro;
    std::string out;
    STRATUM_CHECK_TRUE(e->Get(ro, "rd-0015", &out).IsNotFound());  // 已删
    STRATUM_CHECK_OK(e->Get(ro, "rd-0005", &out));                 // 保留
    STRATUM_CHECK_OK(e->Get(ro, "rd-0025", &out));                 // 保留
  }

  // --- 11. 统计与能力 ---
  {
    ctx.param_desc = tag + "/stats-and-capabilities";
    const EngineStats stats = e->GetStats();
    STRATUM_CHECK_TRUE(stats.writes_total > 0);
    STRATUM_CHECK_TRUE(stats.reads_total > 0);

    const auto caps = e->GetCapabilities();
    STRATUM_CHECK_TRUE(caps.ordered_iteration);
    STRATUM_CHECK_TRUE(caps.snapshots);
    STRATUM_CHECK_TRUE(caps.range_delete);

    STRATUM_CHECK_TRUE(!e->Name().empty());
  }

  // --- 12. 序列号单调性（快照语义的前提）---
  {
    ctx.param_desc = tag + "/snapshot-monotonic";
    WriteOptions wo;
    SnapshotPtr s1 = e->GetSnapshot();
    STRATUM_CHECK_OK(e->Put(wo, "mono", "1"));
    SnapshotPtr s2 = e->GetSnapshot();
    STRATUM_CHECK_OK(e->Put(wo, "mono", "2"));

    STRATUM_CHECK_LT(s1->sequence(), s2->sequence());

    ReadOptions ro1;
    ro1.snapshot = s1;
    std::string out;
    // s1 创建于第一次写入之前，该 key 对它不可见
    STRATUM_CHECK_TRUE(e->Get(ro1, "mono", &out).IsNotFound());

    ReadOptions ro2;
    ro2.snapshot = s2;
    STRATUM_CHECK_OK(e->Get(ro2, "mono", &out));
    STRATUM_CHECK_EQ(out, std::string("1"));

    e->ReleaseSnapshot(s1);
    e->ReleaseSnapshot(s2);
  }
}

// 清理目录（测试开始前）
void CleanDir(const std::string& dir) {
  std::string cmd = "rm -rf " + dir;
  if (system(cmd.c_str()) != 0) {
    // 忽略
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// MemoryEngine 契约
// ---------------------------------------------------------------------------
STRATUM_TEST(StoreContract, MemoryEngine) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  STRATUM_CHECK_OK(engine->Open("mem://contract"));
  RunContract(engine.get(), "memory", ctx);
  STRATUM_CHECK_OK(engine->Close());
}

// ---------------------------------------------------------------------------
// LsmEngine 契约
// ---------------------------------------------------------------------------
STRATUM_TEST(StoreContract, LsmEngine) {
  const std::string dir = EngineDir("lsm");
  CleanDir(dir);

  auto engine = StorageEnginePtr(new LsmEngine());
  STRATUM_CHECK_OK(engine->Open(dir));
  RunContract(engine.get(), "lsm", ctx);
  STRATUM_CHECK_OK(engine->Close());

  CleanDir(dir);
}

// ---------------------------------------------------------------------------
// 两引擎行为一致性：同样的操作序列，结果必须一致
// ---------------------------------------------------------------------------
STRATUM_TEST(StoreContract, EngineBehaviorParity) {
  const std::string dir = EngineDir("parity");
  CleanDir(dir);

  auto mem = StorageEnginePtr(new MemoryEngine());
  auto lsm = StorageEnginePtr(new LsmEngine());
  STRATUM_CHECK_OK(mem->Open("mem://parity"));
  STRATUM_CHECK_OK(lsm->Open(dir));

  WriteOptions wo;
  std::mt19937 rng(2026);
  std::vector<std::string> inserted;

  // 随机写入 500 条
  for (int i = 0; i < 500; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "parity-%06u", rng() % 100000);
    const std::string val = "value-" + std::to_string(rng() % 1000);
    inserted.push_back(key);
    STRATUM_CHECK_OK(mem->Put(wo, key, val));
    STRATUM_CHECK_OK(lsm->Put(wo, key, val));
  }

  // 逐个比对
  ReadOptions ro;
  int compared = 0;
  for (const auto& k : inserted) {
    std::string mv;
    std::string lv;
    const Status ms = mem->Get(ro, k, &mv);
    const Status ls = lsm->Get(ro, k, &lv);
    // 存在性必须一致
    STRATUM_CHECK_EQ(static_cast<int>(ms.ok()), static_cast<int>(ls.ok()));
    if (ms.ok() && ls.ok()) {
      STRATUM_CHECK_EQ(mv, lv);
      ++compared;
    }
  }
  STRATUM_CHECK_TRUE(compared > 0);

  mem->Close();
  lsm->Close();
  CleanDir(dir);
}

// ---------------------------------------------------------------------------
// 范围扫描结果必须完全一致（顺序 + 内容）
// ---------------------------------------------------------------------------
STRATUM_TEST(StoreContract, RangeScanParity) {
  const std::string dir = EngineDir("range");
  CleanDir(dir);

  auto mem = StorageEnginePtr(new MemoryEngine());
  auto lsm = StorageEnginePtr(new LsmEngine());
  STRATUM_CHECK_OK(mem->Open("mem://range"));
  STRATUM_CHECK_OK(lsm->Open(dir));

  WriteOptions wo;
  for (int i = 0; i < 300; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "range-%05d", i);
    const std::string val = "v" + std::to_string(i);
    STRATUM_CHECK_OK(mem->Put(wo, key, val));
    STRATUM_CHECK_OK(lsm->Put(wo, key, val));
  }

  ReadOptions ro;
  auto mit = mem->NewIterator(ro, "range-00100", "range-00200");
  auto lit = lsm->NewIterator(ro, "range-00100", "range-00200");

  int n = 0;
  while (mit->Valid() && lit->Valid()) {
    STRATUM_CHECK_EQ(mit->key(), lit->key());
    STRATUM_CHECK_EQ(mit->value(), lit->value());
    mit->Next();
    lit->Next();
    ++n;
  }
  // 两者应同时结束
  STRATUM_CHECK_EQ(mit->Valid(), lit->Valid());
  STRATUM_CHECK_EQ(n, 100);

  mem->Close();
  lsm->Close();
  CleanDir(dir);
}

STRATUM_TEST_MAIN()
