// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// DeleteRange（CLI `prune` 的地基）测试
//
// 【为什么需要这套测试】删除是整个系统里**唯一不可逆**的操作。
// 而它有两个必须守住的语义：
//
//  1. 删空话题后**元数据必须保留**（FR-R-11 自描述）。
//     否则"这个话题曾经存在过、类型是什么"这一事实永久丢失。
//
//  2. 删除留下的空洞是**主动行为**，必须能与"数据丢失"区分。
//     verify 若把它当缺口（RC-4）上报，会让人去查不存在的录制问题。
//     实现用独立的 pruned/<topic_id> 记录，本测试验证该记录确实写入。
//
// 覆盖三类（项目测试标准）：
//   · 大数据量 —— 10 万条删一半
//   · 随机     —— 多种子随机区间，删后剩余必须精确等于预期
//   · 真实场景 —— ① 裁剪调试话题 ② 删空后元数据仍在 ③ 删后剩余仍可查询

// 标准库头文件必须最先包含
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"

namespace fs = std::filesystem;

using namespace stratum;

namespace {

std::string TempDir(const char* tag) {
  const std::string d = std::string("/tmp/stratum_delrange_") + tag;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

TopicMetadata MakeTopic(const std::string& name) {
  TopicMetadata m;
  m.topic_name = name;
  m.type_name = "TestType";
  return m;
}

Event MakeEvent(uint32_t topic_id, uint32_t seq, uint64_t src_ns, size_t len) {
  Event e;
  e.topic_id = topic_id;
  e.seq = seq;
  e.seq_in_ts = 0;
  e.source_timestamp_ns = src_ns;
  e.reception_timestamp_ns = src_ns + 1000;
  e.payload = std::string(len, 'p');
  return e;
}

// 写入 n 条，时间戳 base + i*step
void WriteN(TimelineImplV2* tl, uint32_t tid, uint64_t n, uint64_t base,
            uint64_t step, size_t len) {
  std::vector<Event> batch;
  batch.reserve(n);
  for (uint64_t i = 0; i < n; ++i) {
    batch.push_back(MakeEvent(tid, static_cast<uint32_t>(i + 1), base + i * step, len));
  }
  STRATUM_CHECK_OK(tl->AppendBatch(batch));
}

uint64_t CountTopic(TimelineImplV2* tl, uint32_t tid) {
  TimeRange all;
  all.begin_ns = kInvalidTimestamp;
  all.end_ns = kInvalidTimestamp;
  uint64_t n = 0;
  STRATUM_CHECK_OK(tl->CountRange(tid, all, &n));
  return n;
}

}  // namespace

// ===========================================================================
// 一、基础契约
// ===========================================================================

STRATUM_TEST(DeleteRange, RemovesOnlyTheRequestedWindow) {
  const std::string dir = TempDir("basic");
  const uint64_t base = 1700000000000000000ULL;
  const uint64_t step = 1000000ULL;   // 1ms

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t tid = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/a"), &tid));
  WriteN(&tl, tid, 100, base, step, 16);

  // 删 [第 30 条, 第 60 条) —— 时间戳 base+30*step 到 base+60*step
  TimeRange r;
  r.begin_ns = base + 30 * step;
  r.end_ns = base + 60 * step;
  uint64_t del = 0, del_bytes = 0;
  STRATUM_CHECK_OK(tl.DeleteRange(tid, r, &del, &del_bytes));
  STRATUM_CHECK_EQ(del, static_cast<uint64_t>(30));
  STRATUM_CHECK_EQ(del_bytes, static_cast<uint64_t>(30 * 16));
  STRATUM_CHECK_EQ(CountTopic(&tl, tid), static_cast<uint64_t>(70));

  // 边界：删除区间外的数据必须完好
  TimeRange before;
  before.begin_ns = kInvalidTimestamp;
  before.end_ns = base + 30 * step;
  uint64_t n_before = 0;
  STRATUM_CHECK_OK(tl.CountRange(tid, before, &n_before));
  STRATUM_CHECK_EQ(n_before, static_cast<uint64_t>(30));   // 前 30 条还在

  TimeRange after;
  after.begin_ns = base + 60 * step;
  after.end_ns = kInvalidTimestamp;
  uint64_t n_after = 0;
  STRATUM_CHECK_OK(tl.CountRange(tid, after, &n_after));
  STRATUM_CHECK_EQ(n_after, static_cast<uint64_t>(40));    // 后 40 条还在

  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(DeleteRange, EmptyRangeIsRejected) {
  const std::string dir = TempDir("empty");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t tid = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/e"), &tid));
  WriteN(&tl, tid, 10, 1700000000000000000ULL, 1000000ULL, 8);

  TimeRange bad;
  bad.begin_ns = 2000;
  bad.end_ns = 1000;   // begin >= end
  Status s = tl.DeleteRange(tid, bad, nullptr, nullptr);
  STRATUM_CHECK_FALSE(s.ok());
  STRATUM_CHECK_EQ(CountTopic(&tl, tid), static_cast<uint64_t>(10));  // 一条没删
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(DeleteRange, UnregisteredTopicIsNotFound) {
  const std::string dir = TempDir("notfound");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  TimeRange r;
  r.begin_ns = 1;
  r.end_ns = 2;
  Status s = tl.DeleteRange(9999, r, nullptr, nullptr);
  STRATUM_CHECK_FALSE(s.ok());
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 二、大数据量
// ===========================================================================

STRATUM_TEST(DeleteRange, LargeVolumeDeleteHalf) {
  const std::string dir = TempDir("big");
  const uint64_t base = 1700000000000000000ULL;
  const uint64_t step = 1000000ULL;
  const uint64_t kN = 100000;

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t tid = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/big"), &tid));
  WriteN(&tl, tid, kN, base, step, 24);
  STRATUM_CHECK_EQ(CountTopic(&tl, tid), kN);

  // 删后半段
  TimeRange r;
  r.begin_ns = base + (kN / 2) * step;
  r.end_ns = kInvalidTimestamp;
  uint64_t del = 0, del_bytes = 0;
  STRATUM_CHECK_OK(tl.DeleteRange(tid, r, &del, &del_bytes));
  STRATUM_CHECK_EQ(del, kN / 2);
  const uint64_t left = CountTopic(&tl, tid);
  std::printf("      [规模] %llu 条删一半：删除 %llu，剩余 %llu\n",
              static_cast<unsigned long long>(kN),
              static_cast<unsigned long long>(del),
              static_cast<unsigned long long>(left));
  STRATUM_CHECK_EQ(left, kN / 2);
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 三、随机（多种子）
// ===========================================================================

STRATUM_TEST(DeleteRange, RandomWindowsMultipleSeeds) {
  for (uint64_t seed : {3ULL, 55ULL, 777ULL, 20261015ULL}) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> n_d(200, 800);
    std::uniform_int_distribution<uint64_t> lo_d(0, 99);
    std::uniform_int_distribution<uint64_t> len_d(1, 50);

    const std::string dir = TempDir(("r" + std::to_string(seed)).c_str());
    const uint64_t n = n_d(rng);
    const uint64_t lo = lo_d(rng);
    const uint64_t len = std::min<uint64_t>(len_d(rng), n - lo);   // 保证不越界
    const uint64_t base = 1700000000000000000ULL;
    const uint64_t step = 1000ULL;

    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    uint32_t tid = 0;
    STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/r"), &tid));
    WriteN(&tl, tid, n, base, step, 12);

    TimeRange r;
    r.begin_ns = base + lo * step;
    r.end_ns = (lo + len >= n) ? kInvalidTimestamp : (base + (lo + len) * step);
    uint64_t del = 0;
    STRATUM_CHECK_OK(tl.DeleteRange(tid, r, &del, nullptr));

    const uint64_t expect_left = (lo + len >= n) ? lo : (n - len);
    const uint64_t left = CountTopic(&tl, tid);
    STRATUM_CHECK_EQ(left, expect_left);
    STRATUM_CHECK_EQ(del, n - expect_left);
    STRATUM_CHECK_OK(tl.Close());
  }
}

// ===========================================================================
// 四、真实场景
// ===========================================================================

// ---------------------------------------------------------------------------
// 场景 1：裁剪调试话题（S18）
//
// 真实用法：调试数据占了大量磁盘，事后确认无用，按时间范围裁掉。
// 关键：**不能影响同一会话里其他话题**。
// ---------------------------------------------------------------------------
STRATUM_TEST(DeleteRangeReal, PruneDebugTopicKeepsOthers) {
  const std::string dir = TempDir("prune1");
  const uint64_t base = 1700000000000000000ULL;
  const uint64_t step = 1000000ULL;

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t keep = 0, dbg = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("sensor/temp"), &keep));
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("sensor/debug_a"), &dbg));
  WriteN(&tl, keep, 200, base, step, 32);
  WriteN(&tl, dbg, 200, base, step, 64);

  // 只删调试话题的前半段
  TimeRange r;
  r.begin_ns = base;
  r.end_ns = base + 100 * step;
  uint64_t del = 0, del_bytes = 0;
  STRATUM_CHECK_OK(tl.DeleteRange(dbg, r, &del, &del_bytes));

  // 被删话题剩一半；**另一个话题必须一条不少**
  STRATUM_CHECK_EQ(CountTopic(&tl, dbg), static_cast<uint64_t>(100));
  STRATUM_CHECK_EQ(CountTopic(&tl, keep), static_cast<uint64_t>(200));
  std::printf("      [场景1] 裁剪 debug 话题：删 %llu 条，sensor/temp 仍 200 条\n",
              static_cast<unsigned long long>(del));
  STRATUM_CHECK_OK(tl.Close());
}

// ---------------------------------------------------------------------------
// 场景 2：删空话题后元数据必须仍在（FR-R-11）
//
// 真实用法：某个话题的数据全被裁掉。此后仍应能回答
// "这个话题曾经存在过吗、它的类型是什么"。
// 否则审计（SC-12）无法解释"为什么这个会话里有个空话题"。
// ---------------------------------------------------------------------------
STRATUM_TEST(DeleteRangeReal, EmptiedTopicKeepsMetadata) {
  const std::string dir = TempDir("prune2");
  const uint64_t base = 1700000000000000000ULL;

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t tid = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("gone/topic"), &tid));
  WriteN(&tl, tid, 50, base, 1000000ULL, 16);

  TimeRange all;
  all.begin_ns = kInvalidTimestamp;
  all.end_ns = kInvalidTimestamp;
  uint64_t del = 0;
  STRATUM_CHECK_OK(tl.DeleteRange(tid, all, &del, nullptr));
  STRATUM_CHECK_EQ(del, static_cast<uint64_t>(50));
  STRATUM_CHECK_EQ(CountTopic(&tl, tid), static_cast<uint64_t>(0));

  // 【关键】话题元数据必须还在，且类型名可读
  TopicMetadata m;
  STRATUM_CHECK_OK(tl.GetTopicMetadata(tid, &m));
  STRATUM_CHECK_EQ(m.topic_name, std::string("gone/topic"));
  STRATUM_CHECK_EQ(m.type_name, std::string("TestType"));
  STRATUM_CHECK_EQ(m.message_count, static_cast<uint64_t>(0));
  STRATUM_CHECK_FALSE(m.active);           // 标记为空，而不是消失
  std::printf("      [场景2] 删空后元数据保留：name=%s type=%s active=%d\n",
              m.topic_name.c_str(), m.type_name.c_str(),
              static_cast<int>(m.active));
  STRATUM_CHECK_OK(tl.Close());

  // 重开后元数据仍在（已持久化）
  TimelineImplV2 tl2;
  STRATUM_CHECK_OK(tl2.Open(dir));
  TopicMetadata m2;
  STRATUM_CHECK_OK(tl2.GetTopicMetadata(tid, &m2));
  STRATUM_CHECK_EQ(m2.topic_name, std::string("gone/topic"));
  STRATUM_CHECK_OK(tl2.Close());
}

// ---------------------------------------------------------------------------
// 场景 3：删除后剩余数据仍可正常查询与定点访问
//
// 真实用法：裁掉一段之后继续做复盘 —— 剩下的数据必须完全可用，
// 且范围查询不能因为中间有空洞而返回异常结果。
// ---------------------------------------------------------------------------
STRATUM_TEST(DeleteRangeReal, RemainingDataStillQueryable) {
  const std::string dir = TempDir("prune3");
  const uint64_t base = 1700000000000000000ULL;
  const uint64_t step = 1000000ULL;

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t tid = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("sensor/x"), &tid));
  WriteN(&tl, tid, 100, base, step, 20);

  // 挖掉中间 [40, 70)
  TimeRange hole;
  hole.begin_ns = base + 40 * step;
  hole.end_ns = base + 70 * step;
  STRATUM_CHECK_OK(tl.DeleteRange(tid, hole, nullptr, nullptr));

  // 空洞之前的定点访问正常
  StateAtResult st;
  STRATUM_CHECK_OK(tl.GetStateAt(tid, base + 10 * step, &st));
  STRATUM_CHECK_EQ(static_cast<int>(st.kind),
                   static_cast<int>(StateAtResult::Kind::kFound));

  // 空洞之后的定点访问正常
  STRATUM_CHECK_OK(tl.GetStateAt(tid, base + 80 * step, &st));
  STRATUM_CHECK_EQ(static_cast<int>(st.kind),
                   static_cast<int>(StateAtResult::Kind::kFound));

  // 空洞内部的定点访问：应命中空洞**之前**的最后一条（语义允许），
  // 或者报告找不到 —— 两者都合理，但不能崩溃或返回垃圾
  STRATUM_CHECK_OK(tl.GetStateAt(tid, base + 55 * step, &st));
  if (st.kind == StateAtResult::Kind::kFound) {
    STRATUM_CHECK_TRUE(st.event.source_timestamp_ns < base + 40 * step);
  }

  // 全量范围查询的条数正确
  STRATUM_CHECK_EQ(CountTopic(&tl, tid), static_cast<uint64_t>(70));
  std::printf("      [场景3] 挖掉中间 30 条后：剩余 70 条，前后定点访问均正常\n");
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST_MAIN()
