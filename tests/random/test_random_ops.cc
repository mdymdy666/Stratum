// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 随机化测试
//
// ===========================================================================
// 为什么需要随机测试
// ===========================================================================
//
// 前面的测试都是"我想到的边界"。但真实世界的输入组合远超人的枚举能力。
// 随机测试的价值在于**用大量随机序列触发我没想到的组合**。
//
// 关键原则：**固定种子**。任何失败都必须能复现，否则随机测试毫无价值。
// 因此每个测试都显式使用固定种子，并在失败信息中带上种子值。

#include <map>
#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl.h"
#include "stratum/storage/lsm/lsm_engine.h"
#include "stratum/storage/memory/memory_engine.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::storage;

namespace {

constexpr uint64_t kBaseTs = 1700000000000000000ULL;

// 生成指定数量的随机字符键（可含任意字节，考验编码正确性）
std::string RandomKey(std::mt19937_64* rng, size_t max_len) {
  std::uniform_int_distribution<size_t> len_dist(1, max_len);
  std::uniform_int_distribution<int> ch_dist(1, 255);  // 避免 0（便于打印）
  const size_t len = len_dist(*rng);
  std::string key;
  key.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    key.push_back(static_cast<char>(ch_dist(*rng)));
  }
  return key;
}

}  // namespace

// ---------------------------------------------------------------------------
// 随机键空间：写 / 读 / 删 的随机交织
// ---------------------------------------------------------------------------
STRATUM_TEST(Random, InterleavedCrudOnRandomKeys) {
  constexpr uint64_t kSeed = 20261007;
  ctx.param_desc = "seed=" + std::to_string(kSeed);

  std::mt19937_64 rng(kSeed);
  auto engine = StorageEnginePtr(new MemoryEngine());
  STRATUM_CHECK_OK(engine->Open("mem://random-crud"));

  // 影子模型：用 std::map 记录"应该是什么"，最后逐一比对
  std::map<std::string, std::string> shadow;

  WriteOptions wo;
  ReadOptions ro;
  constexpr int kOps = 5000;
  int writes = 0;
  int deletes = 0;
  int reads = 0;

  for (int i = 0; i < kOps; ++i) {
    const int op = static_cast<int>(rng() % 100);
    const std::string key = RandomKey(&rng, 24);

    if (op < 50) {
      // 写
      const std::string value = RandomKey(&rng, 64);
      STRATUM_CHECK_OK(engine->Put(wo, key, value));
      shadow[key] = value;
      ++writes;
    } else if (op < 70) {
      // 删
      STRATUM_CHECK_OK(engine->Delete(wo, key));
      shadow.erase(key);
      ++deletes;
    } else {
      // 读：必须与影子模型一致
      std::string actual;
      const Status s = engine->Get(ro, key, &actual);
      auto it = shadow.find(key);
      if (it == shadow.end()) {
        STRATUM_CHECK_TRUE(s.IsNotFound());
      } else {
        STRATUM_CHECK_OK(s);
        STRATUM_CHECK_EQ(actual, it->second);
      }
      ++reads;
    }
  }

  // 最终全量比对
  int matched = 0;
  for (const auto& kv : shadow) {
    std::string actual;
    STRATUM_CHECK_OK(engine->Get(ro, kv.first, &actual));
    STRATUM_CHECK_EQ(actual, kv.second);
    ++matched;
  }

  ctx.param_desc = "writes=" + std::to_string(writes) +
                   " deletes=" + std::to_string(deletes) +
                   " reads=" + std::to_string(reads) +
                   " final=" + std::to_string(matched);
  STRATUM_CHECK_TRUE(matched > 0);
  engine->Close();
}

// ---------------------------------------------------------------------------
// 随机键空间：顺序遍历必须与影子模型完全一致
// ---------------------------------------------------------------------------
STRATUM_TEST(Random, OrderedIterationMatchesShadow) {
  constexpr uint64_t kSeed = 20261008;
  ctx.param_desc = "seed=" + std::to_string(kSeed);

  std::mt19937_64 rng(kSeed);
  auto engine = StorageEnginePtr(new MemoryEngine());
  STRATUM_CHECK_OK(engine->Open("mem://random-iter"));

  std::map<std::string, std::string> shadow;
  WriteOptions wo;

  // 随机写入 2000 条（键用可打印字符，便于范围构造）
  for (int i = 0; i < 2000; ++i) {
    std::string key;
    const int len = 1 + static_cast<int>(rng() % 20);
    for (int j = 0; j < len; ++j) {
      key.push_back(static_cast<char>('a' + (rng() % 26)));
    }
    const std::string value = "v" + std::to_string(i);
    STRATUM_CHECK_OK(engine->Put(wo, key, value));
    shadow[key] = value;
  }

  // 全量遍历，逐条与影子模型比对（顺序 + 内容）
  ReadOptions ro;
  auto it = engine->NewIterator(ro, "", "");
  auto shadow_it = shadow.begin();
  size_t count = 0;
  for (; it->Valid(); it->Next(), ++shadow_it) {
    if (shadow_it == shadow.end()) {
      STRATUM_CHECK_TRUE(false);  // 迭代器多出了数据
      break;
    }
    STRATUM_CHECK_EQ(it->key(), shadow_it->first);
    STRATUM_CHECK_EQ(it->value(), shadow_it->second);
    ++count;
  }
  ctx.param_desc = "iterated=" + std::to_string(count) +
                   " expected=" + std::to_string(shadow.size());
  STRATUM_CHECK_EQ(count, shadow.size());
  STRATUM_CHECK_TRUE(shadow_it == shadow.end());

  engine->Close();
}

// ---------------------------------------------------------------------------
// 随机时间戳序列：验证时间轴对乱序/重复/倒退的处理
// ---------------------------------------------------------------------------
STRATUM_TEST(Random, TimelineHandlesChaoticTimestamps) {
  constexpr uint64_t kSeed = 20261009;
  ctx.param_desc = "seed=" + std::to_string(kSeed);

  std::mt19937_64 rng(kSeed);
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://random-ts"));

  TopicMetadata meta;
  meta.topic_name = "chaotic";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(timeline.RegisterTopic(meta, &topic));

  // 生成 3000 条时间戳"混乱"的事件：
  //   - 有递增、有重复、有倒退
  constexpr int kCount = 3000;
  std::vector<Event> events;
  events.reserve(kCount);
  uint64_t cursor = kBaseTs;
  for (int i = 0; i < kCount; ++i) {
    const int kind = static_cast<int>(rng() % 10);
    if (kind < 6) {
      cursor += 1 + (rng() % 1000);  // 递增
    } else if (kind < 8) {
      // 重复（同一时间戳）
    } else {
      cursor -= 1 + (rng() % 500);  // 倒退
    }

    Event e;
    e.topic_id = topic;
    e.seq = static_cast<uint32_t>(i);
    e.source_timestamp_ns = cursor;
    e.payload = "p" + std::to_string(i);
    events.push_back(std::move(e));
  }

  STRATUM_CHECK_OK(timeline.AppendBatch(events));

  // 关键断言：**一条都不能丢**（时间戳混乱不是丢数据的理由）
  uint64_t count = 0;
  STRATUM_CHECK_OK(timeline.CountRange(topic, TimeRange(), &count));
  ctx.param_desc = "seed=" + std::to_string(kSeed) +
                   " appended=" + std::to_string(kCount) +
                   " read=" + std::to_string(count);
  STRATUM_CHECK_EQ(count, static_cast<uint64_t>(kCount));

  // 遍历必须按时间戳非递减返回
  EventIteratorPtr it;
  STRATUM_CHECK_OK(timeline.QueryRange(topic, TimeRange(), &it));
  TimestampNs prev = 0;
  size_t n = 0;
  for (; it->Valid(); it->Next()) {
    const TimestampNs ts = it->event().source_timestamp_ns;
    if (n > 0) {
      STRATUM_CHECK_TRUE(ts >= prev);
    }
    prev = ts;
    ++n;
  }
  STRATUM_CHECK_EQ(n, static_cast<size_t>(kCount));
}

// ---------------------------------------------------------------------------
// 随机负载的端到端：录制 → 回放 → 逐条比对
// ---------------------------------------------------------------------------
STRATUM_TEST(Random, EndToEndWithRandomPayloads) {
  constexpr uint64_t kSeed = 20261010;
  ctx.param_desc = "seed=" + std::to_string(kSeed);

  std::mt19937_64 rng(kSeed);
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://random-e2e"));

  TopicMetadata meta;
  meta.topic_name = "random/payloads";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(timeline.RegisterTopic(meta, &topic));

  // 随机大小的负载（含空负载、大负载）
  constexpr int kCount = 500;
  std::vector<Event> events;
  std::vector<std::string> expected_payloads;
  events.reserve(kCount);
  for (int i = 0; i < kCount; ++i) {
    Event e;
    e.topic_id = topic;
    e.seq = static_cast<uint32_t>(i);
    e.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(i) * 1000;
    // 大小分布：多数小、少数大
    const int kind = static_cast<int>(rng() % 100);
    size_t size = 0;
    if (kind < 5) {
      size = 0;  // 空
    } else if (kind < 95) {
      size = 1 + (rng() % 64);
    } else {
      size = 10000 + (rng() % 50000);  // 大负载
    }
    e.payload.resize(size);
    for (size_t j = 0; j < size; ++j) {
      e.payload[j] = static_cast<char>(rng() % 256);
    }
    expected_payloads.push_back(e.payload);
    events.push_back(std::move(e));
  }

  STRATUM_CHECK_OK(timeline.AppendBatch(events));

  // 取出并逐条比对（含二进制内容）
  EventIteratorPtr it;
  STRATUM_CHECK_OK(timeline.QueryRange(topic, TimeRange(), &it));
  size_t n = 0;
  for (; it->Valid(); it->Next()) {
    if (n < expected_payloads.size()) {
      STRATUM_CHECK_EQ(it->event().payload, expected_payloads[n]);
      STRATUM_CHECK_EQ(it->event().payload.size(),
                       expected_payloads[n].size());
    }
    ++n;
  }
  ctx.param_desc = "seed=" + std::to_string(kSeed) + " count=" +
                   std::to_string(n);
  STRATUM_CHECK_EQ(n, static_cast<size_t>(kCount));
}

// ---------------------------------------------------------------------------
// 随机操作 vs 契约：LSM 与 Memory 在随机序列下必须一致
// ---------------------------------------------------------------------------
STRATUM_TEST(Random, TwoEnginesAgreeUnderRandomOps) {
  constexpr uint64_t kSeed = 20261011;
  ctx.param_desc = "seed=" + std::to_string(kSeed);

  const std::string dir = "/tmp/stratum_random_parity";
  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }

  auto mem = StorageEnginePtr(new MemoryEngine());
  auto lsm = StorageEnginePtr(new LsmEngine());
  STRATUM_CHECK_OK(mem->Open("mem://parity"));
  STRATUM_CHECK_OK(lsm->Open(dir));

  std::mt19937_64 rng(kSeed);
  WriteOptions wo;
  ReadOptions ro;

  for (int i = 0; i < 3000; ++i) {
    const int op = static_cast<int>(rng() % 100);
    // 键用小范围，制造大量覆盖与边界碰撞
    const std::string key = "k" + std::to_string(rng() % 500);

    if (op < 60) {
      const std::string value = "v" + std::to_string(rng() % 1000);
      STRATUM_CHECK_OK(mem->Put(wo, key, value));
      STRATUM_CHECK_OK(lsm->Put(wo, key, value));
    } else if (op < 75) {
      mem->Delete(wo, key);
      lsm->Delete(wo, key);
    } else {
      std::string mv;
      std::string lv;
      const Status ms = mem->Get(ro, key, &mv);
      const Status ls = lsm->Get(ro, key, &lv);
      ctx.param_desc = "seed=" + std::to_string(kSeed) + " op=" +
                       std::to_string(i) + " key=" + key;
      STRATUM_CHECK_EQ(static_cast<int>(ms.ok()), static_cast<int>(ls.ok()));
      if (ms.ok() && ls.ok()) {
        STRATUM_CHECK_EQ(mv, lv);
      }
    }
  }

  mem->Close();
  lsm->Close();
  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }
}

STRATUM_TEST_MAIN()
