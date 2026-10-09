// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// v2 DB 层：随机 + 大数据量测试
//
// ===========================================================================
// 数据量标准（design/07-testing-standards.md）
// ===========================================================================
//
//   存储引擎测试 ≥ 50,000 条，随机测试必须固定种子可复现，
//   持久化测试必须包含"重启后再写"。
//
// 本文件：
//   A. 5 万条**乱序时间戳**随机事件（含重复/倒退/跳变），
//      影子模型（std::map）逐条比对
//   B. 100 次随机范围查询与影子比对
//   C. 重启后再写 2 万条，验证计数器恢复后**零覆盖**
//   D. 10 万条批量写入吞吐与完整性

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "stratum/storage/lsm_v2/stratum_db.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::storage::lsm_v2;

namespace {

constexpr uint64_t kBaseTs = 1700000000000000000ULL;
constexpr int kNumTopics = 3;

// 影子模型：key = 20 字节 TimeKey 的字符串形式（简化：用三元组排序），
// 这里直接用 map<pair<topic, pair<ts, seq>>, payload> 模拟"按 key 有序"。
struct ShadowKey {
  uint32_t topic;
  uint64_t ts;
  uint32_t seq;
  bool operator<(const ShadowKey& o) const {
    if (topic != o.topic) return topic < o.topic;
    if (ts != o.ts) return ts < o.ts;
    // 【重要】seq 降序：反转编码下 stored = ~seq，
    // 同 ts 内 stored 升序 = seq 降序。影子模型必须与真实
    // key 字节序一致，否则逐条比对会错位。
    return seq > o.seq;
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// A+B：5 万条乱序随机事件 + 随机范围查询，全部与影子模型比对
// ---------------------------------------------------------------------------
STRATUM_TEST(LsmV2Random, A_FiftyThousandChaoticEventsMatchShadow) {
  constexpr uint64_t kSeed = 20261020;
  ctx.param_desc = "seed=" + std::to_string(kSeed);

  const std::string dir = "/tmp/stratum_v2_rand";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  StratumDB* db = nullptr;
  STRATUM_CHECK_OK(StratumDB::Open(dir, &db));

  // 注册 3 个话题
  uint32_t topics[kNumTopics];
  for (int i = 0; i < kNumTopics; ++i) {
    TopicMetadata meta;
    meta.topic_name = "rand/topic" + std::to_string(i);
    STRATUM_CHECK_OK(db->RegisterTopic(meta, &topics[i]));
  }

  std::mt19937_64 rng(kSeed);
  std::map<ShadowKey, std::string> shadow;  // 影子模型

  constexpr int kTotal = 50000;
  constexpr int kBatch = 500;

  for (int base = 0; base < kTotal; base += kBatch) {
    const int n = std::min(kBatch, kTotal - base);
    std::vector<Event> events;
    events.reserve(n);

    for (int i = 0; i < n; ++i) {
      Event e;
      e.topic_id = topics[rng() % kNumTopics];

      // 混沌时间戳：60% 递增、20% 重复、15% 倒退、5% 大幅跳变
      const int kind = static_cast<int>(rng() % 100);
      static uint64_t cursor = kBaseTs;  // 单线程测试，static 可接受
      if (kind < 60) {
        cursor += 1 + (rng() % 100000);  // 递增
      } else if (kind < 80) {
        // 重复（同一时间戳）
      } else if (kind < 95) {
        cursor -= 1 + (rng() % 50000);  // 倒退
      } else {
        cursor += 1000000 + (rng() % 10000000);  // 大幅跳变（设备断连后回补）
      }
      e.source_timestamp_ns = cursor;

      // 随机负载：多数小、少数大、偶发空
      const int pk = static_cast<int>(rng() % 100);
      size_t size = 0;
      if (pk < 5) size = 0;
      else if (pk < 95) size = 1 + (rng() % 128);
      else size = 4096 + (rng() % 16384);
      e.payload.resize(size);
      for (size_t j = 0; j < size; ++j) {
        e.payload[j] = static_cast<char>(rng() % 256);
      }
      events.push_back(std::move(e));
    }

    STRATUM_CHECK_OK(db->AppendBatch(events));

    // 影子模型同步（与 StratumDB 相同的 seq_in_ts 分配规则：
    // per-topic 全局递增；同一批内按出现顺序）
    std::map<uint32_t, uint32_t> seq_counters;
    for (const auto& e : events) {
      const uint32_t seq = seq_counters[e.topic_id]++;
      ShadowKey k{e.topic_id, e.source_timestamp_ns, seq};
      shadow[k] = e.payload;
    }
  }

  ctx.param_desc = "写入 " + std::to_string(kTotal) +
                   " 条，影子 " + std::to_string(shadow.size()) + " 条";

  // --- 100 次随机范围查询与影子比对 ---
  int checked = 0;
  for (int q = 0; q < 100; ++q) {
    const uint32_t topic = topics[rng() % kNumTopics];
    const uint64_t t1 = kBaseTs + (rng() % 100) * 1000000ULL;
    const uint64_t t2 = t1 + (rng() % 200) * 1000000ULL + 1000;

    std::vector<Event> out;
    STRATUM_CHECK_OK(db->QueryRange(topic, t1, t2, &out));

    // 影子模型中的期望结果（shadow 的 key 序 = 真实字节序，
    // 因此直接顺序遍历即可与 QueryRange 输出对齐）
    std::vector<std::string> expected;
    for (const auto& kv : shadow) {
      if (kv.first.topic == topic && kv.first.ts >= t1 && kv.first.ts < t2) {
        expected.push_back(kv.second);
      }
    }

    ctx.param_desc = "q=" + std::to_string(q) + " got=" +
                     std::to_string(out.size()) + " expect=" +
                     std::to_string(expected.size());
    STRATUM_CHECK_EQ(out.size(), expected.size());
    for (size_t i = 0; i < out.size(); ++i) {
      STRATUM_CHECK_EQ(out[i].payload, expected[i]);
    }
    ++checked;
  }
  ctx.param_desc = "随机范围查询 " + std::to_string(checked) + " 次全部一致";

  db->Close();
  delete db;
  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

// ---------------------------------------------------------------------------
// C：重启后再写 2 万条 → 零覆盖（P1 的大数据量版）
// ---------------------------------------------------------------------------
STRATUM_TEST(LsmV2Random, C_RestartAndAppendNoOverwrite) {
  constexpr uint64_t kSeed = 20261021;
  ctx.param_desc = "seed=" + std::to_string(kSeed);

  const std::string dir = "/tmp/stratum_v2_restart";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  std::mt19937_64 rng(kSeed);
  uint32_t topic = 0;

  constexpr int kFirst = 30000;
  constexpr int kSecond = 20000;

  // 首次运行：3 万条，时间戳集中在一个小区间（制造大量同 ts 与重复）
  {
    StratumDB* db = nullptr;
    STRATUM_CHECK_OK(StratumDB::Open(dir, &db));
    TopicMetadata meta;
    meta.topic_name = "restart/sole";
    STRATUM_CHECK_OK(db->RegisterTopic(meta, &topic));
    constexpr int kBatch = 1000;
    const uint64_t ts_base = kBaseTs + 777;  // 故意非零偏移
    for (int base = 0; base < kFirst; base += kBatch) {
      std::vector<Event> events;
      for (int i = 0; i < kBatch; ++i) {
        Event e;
        e.topic_id = topic;
        // 时间戳只在 1000 个值内重复 → 平均每个 ts 30 条
        e.source_timestamp_ns = ts_base + (rng() % 1000);
        e.payload = "first-" + std::to_string(base + i);
        events.push_back(std::move(e));
      }
      STRATUM_CHECK_OK(db->AppendBatch(events));
    }
    db->Close();
    delete db;
  }

  // 第二次运行：再写 2 万条，时间戳范围与第一次**重叠**
  {
    StratumDB* db = nullptr;
    STRATUM_CHECK_OK(StratumDB::Open(dir, &db));

    constexpr int kBatch = 1000;
    const uint64_t ts_base = kBaseTs + 777;
    for (int base = 0; base < kSecond; base += kBatch) {
      std::vector<Event> events;
      for (int i = 0; i < kBatch; ++i) {
        Event e;
        e.topic_id = topic;
        e.source_timestamp_ns = ts_base + (rng() % 1000);  // 与首次重叠
        e.payload = "second-" + std::to_string(base + i);
        events.push_back(std::move(e));
      }
      STRATUM_CHECK_OK(db->AppendBatch(events));
    }

    // 【核心断言】重叠时间戳区间内必须是 30000 + 20000 = 50000 条。
    // 修复前（计数器重启归零）：第二次写入覆盖第一次 → 只有 ~20000 条。
    uint64_t n = 0;
    STRATUM_CHECK_OK(db->CountRange(topic, ts_base, ts_base + 1000, &n));
    ctx.param_desc = "重叠区间条数=" + std::to_string(n) + "（期望 50000）";
    STRATUM_CHECK_EQ(n, static_cast<uint64_t>(kFirst + kSecond));

    db->Close();
    delete db;
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

// ---------------------------------------------------------------------------
// D：10 万条批量写入吞吐与完整性（触发多次 compaction）
// ---------------------------------------------------------------------------
STRATUM_TEST(LsmV2Random, D_HundredThousandBulkThroughput) {
  const std::string dir = "/tmp/stratum_v2_bulk";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  constexpr int kTotal = 100000;
  constexpr int kBatch = 2000;

  StratumDB* db = nullptr;
  STRATUM_CHECK_OK(StratumDB::Open(dir, &db));
  TopicMetadata meta;
  meta.topic_name = "bulk/single";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(db->RegisterTopic(meta, &topic));

  const auto start = std::chrono::steady_clock::now();
  for (int base = 0; base < kTotal; base += kBatch) {
    std::vector<Event> events;
    events.reserve(kBatch);
    for (int i = 0; i < kBatch; ++i) {
      Event e;
      e.topic_id = topic;
      e.source_timestamp_ns =
          kBaseTs + static_cast<uint64_t>(base + i) * 1000;
      e.payload.assign(64, 'x');
      events.push_back(std::move(e));
    }
    STRATUM_CHECK_OK(db->AppendBatch(events));
  }
  const double sec = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - start)
                         .count();

  uint64_t n = 0;
  STRATUM_CHECK_OK(db->CountRange(topic, kBaseTs,
                                  kBaseTs + kTotal * 1000ULL, &n));
  const double rate = kTotal / sec;
  ctx.param_desc = "吞吐=" + std::to_string(static_cast<int>(rate)) +
                   " ops/s, count=" + std::to_string(n);
  STRATUM_CHECK_EQ(n, static_cast<uint64_t>(kTotal));
  // LSMengine 写入吞吐下界（保守，WSL 虚拟化环境）
  STRATUM_CHECK_TRUE(rate > 20000.0);

  // 强制 compaction 后完整性不受影响
  STRATUM_CHECK_OK(db->Compact());
  n = 0;
  STRATUM_CHECK_OK(db->CountRange(topic, kBaseTs,
                                  kBaseTs + kTotal * 1000ULL, &n));
  ctx.param_desc += " compaction后=" + std::to_string(n);
  STRATUM_CHECK_EQ(n, static_cast<uint64_t>(kTotal));

  db->Close();
  delete db;
  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

STRATUM_TEST_MAIN()
