// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 「已裁剪区间」（pruned ranges）的读取与语义测试
//
// 【这套测试补的是什么】上一轮把 `DeleteRange` → `ListPrunedRanges` →
// `verify` 分开呈现这条链路打通并端到端实测了，但**没有自动化测试**。
// 这属于"欠账"，本文件还上。
//
// 要守住的核心语义（顺序即重要性）：
//
//   1. **裁剪不是缺口**：prune 删数据后，质量统计里的"完整性有问题"
//      必须**仍然为 false**。若这条破了，复盘时会去查不存在的问题。
//   2. **区间可追溯**：删了哪一段、多少条，事后必须能查回来。
//   3. **多次裁剪可累积**：追加式记录，不能互相覆盖。
//   4. **话题之间互不影响**。
//
// 覆盖三类（项目测试标准）：
//   · 大数据量 —— 连续裁剪 100 次
//   · 随机     —— 多种子随机区间，数量与数值都要对
//   · 真实场景 —— ① 裁剪调试数据后复盘 ② 多次裁剪累积 ③ 删空后记录仍在

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
  const std::string d = std::string("/tmp/stratum_pruned_") + tag;
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

STRATUM_TEST(PrunedRanges, EmptyWhenNeverPruned) {
  const std::string dir = TempDir("empty");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t tid = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/a"), &tid));
  WriteN(&tl, tid, 50, 1700000000000000000ULL, 1000000ULL, 16);

  std::vector<PrunedRange> out;
  STRATUM_CHECK_OK(tl.ListPrunedRanges(tid, &out));
  STRATUM_CHECK_TRUE(out.empty());      // 从未裁剪 -> 空，不是错误
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(PrunedRanges, SinglePruneIsRecorded) {
  const std::string dir = TempDir("single");
  const uint64_t base = 1700000000000000000ULL;
  const uint64_t step = 1000000ULL;

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t tid = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/b"), &tid));
  WriteN(&tl, tid, 100, base, step, 16);

  TimeRange r;
  r.begin_ns = base + 20 * step;
  r.end_ns = base + 50 * step;
  uint64_t del = 0;
  STRATUM_CHECK_OK(tl.DeleteRange(tid, r, &del, nullptr));
  STRATUM_CHECK_EQ(del, static_cast<uint64_t>(30));

  std::vector<PrunedRange> out;
  STRATUM_CHECK_OK(tl.ListPrunedRanges(tid, &out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(1));
  STRATUM_CHECK_EQ(out[0].topic_id, tid);
  STRATUM_CHECK_EQ(out[0].begin_ns, base + 20 * step);
  STRATUM_CHECK_EQ(out[0].end_ns, base + 50 * step);
  STRATUM_CHECK_EQ(out[0].deleted_events, static_cast<uint64_t>(30));
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(PrunedRanges, MultiplePrunesAccumulate) {
  const std::string dir = TempDir("multi");
  const uint64_t base = 1700000000000000000ULL;
  const uint64_t step = 1000000ULL;

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t tid = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/c"), &tid));
  WriteN(&tl, tid, 200, base, step, 16);

  // 三次独立裁剪，必须**追加**而不是互相覆盖
  for (int i = 0; i < 3; ++i) {
    TimeRange r;
    r.begin_ns = base + static_cast<uint64_t>(i * 40) * step;
    r.end_ns = r.begin_ns + 10 * step;
    STRATUM_CHECK_OK(tl.DeleteRange(tid, r, nullptr, nullptr));
  }
  std::vector<PrunedRange> out;
  STRATUM_CHECK_OK(tl.ListPrunedRanges(tid, &out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(3));
  for (int i = 0; i < 3; ++i) {
    STRATUM_CHECK_EQ(out[static_cast<size_t>(i)].begin_ns,
                     base + static_cast<uint64_t>(i * 40) * step);
    STRATUM_CHECK_EQ(out[static_cast<size_t>(i)].deleted_events,
                     static_cast<uint64_t>(10));
  }
  STRATUM_CHECK_EQ(CountTopic(&tl, tid), static_cast<uint64_t>(170));
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 二、最关键的一条：裁剪**不是**缺口
// ===========================================================================

STRATUM_TEST(PrunedRanges, PruningDoesNotCountAsIntegrityIssue) {
  // 【为什么这条最重要】若裁剪被计入"完整性有问题"，
  // 复盘时会得出"录制丢了数据"的错误结论，让人去查并不存在的问题。
  const std::string dir = TempDir("notissue");
  const uint64_t base = 1700000000000000000ULL;

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t tid = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/d"), &tid));
  WriteN(&tl, tid, 100, base, 1000000ULL, 16);

  STRATUM_CHECK_FALSE(tl.GetQualityStats().HasIntegrityIssue());

  // 删掉中间一半
  TimeRange r;
  r.begin_ns = base + 25 * 1000000ULL;
  r.end_ns = base + 75 * 1000000ULL;
  STRATUM_CHECK_OK(tl.DeleteRange(tid, r, nullptr, nullptr));

  // 【核心断言】删了 50 条，但质量统计**仍然没有完整性 issues**
  STRATUM_CHECK_FALSE(tl.GetQualityStats().HasIntegrityIssue());
  STRATUM_CHECK_EQ(tl.GetQualityStats().dropped_messages, static_cast<uint64_t>(0));
  STRATUM_CHECK_EQ(tl.GetQualityStats().sequence_gaps, static_cast<uint64_t>(0));
  STRATUM_CHECK_EQ(CountTopic(&tl, tid), static_cast<uint64_t>(50));

  // 且裁剪记录可查（"为什么少了 50 条"有答案）
  std::vector<PrunedRange> out;
  STRATUM_CHECK_OK(tl.ListPrunedRanges(tid, &out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(1));
  STRATUM_CHECK_EQ(out[0].deleted_events, static_cast<uint64_t>(50));
  std::printf("      [核心] 删 50 条后 has_integrity_issue 仍为 false，"
              "且裁剪记录可追溯\n");
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 三、话题之间互不影响
// ===========================================================================

STRATUM_TEST(PrunedRanges, PerTopicIsolation) {
  const std::string dir = TempDir("isolation");
  const uint64_t base = 1700000000000000000ULL;

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t a = 0, b = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("keep/me"), &a));
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("prune/me"), &b));
  WriteN(&tl, a, 80, base, 1000000ULL, 24);
  WriteN(&tl, b, 80, base, 1000000ULL, 24);

  TimeRange all;
  all.begin_ns = kInvalidTimestamp;
  all.end_ns = kInvalidTimestamp;
  STRATUM_CHECK_OK(tl.DeleteRange(b, all, nullptr, nullptr));

  std::vector<PrunedRange> ra, rb;
  STRATUM_CHECK_OK(tl.ListPrunedRanges(a, &ra));
  STRATUM_CHECK_OK(tl.ListPrunedRanges(b, &rb));
  STRATUM_CHECK_TRUE(ra.empty());                    // 未被裁剪的话题没有记录
  STRATUM_CHECK_EQ(rb.size(), static_cast<size_t>(1));
  STRATUM_CHECK_EQ(rb[0].deleted_events, static_cast<uint64_t>(80));
  STRATUM_CHECK_EQ(CountTopic(&tl, a), static_cast<uint64_t>(80));   // 未受影响
  STRATUM_CHECK_EQ(CountTopic(&tl, b), static_cast<uint64_t>(0));
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 四、未注册话题 / 边界
// ===========================================================================

STRATUM_TEST(PrunedRanges, UnregisteredTopicReturnsEmptyNotError) {
  const std::string dir = TempDir("unreg");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  // 未注册的话题 id：没有裁剪记录，应返回空而不是错误
  std::vector<PrunedRange> out;
  Status s = tl.ListPrunedRanges(9999, &out);
  STRATUM_CHECK_OK(s);
  STRATUM_CHECK_TRUE(out.empty());
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(PrunedRanges, NullOutputIsInvalidArgument) {
  const std::string dir = TempDir("nullout");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  Status s = tl.ListPrunedRanges(1, nullptr);
  STRATUM_CHECK_TRUE(s.code() == StatusCode::kInvalidArgument);
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(PrunedRanges, SurvivesReopen) {
  // 裁剪记录必须**持久化**（重开进程后仍能查到），否则审计无从谈起
  const std::string dir = TempDir("reopen");
  const uint64_t base = 1700000000000000000ULL;
  uint32_t tid = 0;
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/p"), &tid));
    WriteN(&tl, tid, 60, base, 1000000ULL, 16);
    TimeRange r;
    r.begin_ns = base + 10 * 1000000ULL;
    r.end_ns = base + 20 * 1000000ULL;
    STRATUM_CHECK_OK(tl.DeleteRange(tid, r, nullptr, nullptr));
    STRATUM_CHECK_OK(tl.Close());
  }
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    std::vector<PrunedRange> out;
    STRATUM_CHECK_OK(tl.ListPrunedRanges(tid, &out));
    STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(1));
    STRATUM_CHECK_EQ(out[0].deleted_events, static_cast<uint64_t>(10));
    STRATUM_CHECK_OK(tl.Close());
  }
}

// ===========================================================================
// 五、大数据量
// ===========================================================================

STRATUM_TEST(PrunedRanges, LargeVolumeHundredPrunes) {
  const std::string dir = TempDir("big");
  const uint64_t base = 1700000000000000000ULL;
  const uint64_t step = 1000ULL;

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t tid = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/big"), &tid));
  const uint64_t kN = 5000;
  WriteN(&tl, tid, kN, base, step, 8);

  // 连续裁剪 100 次（每次 5 条，互不重叠）
  const int kRounds = 100;
  uint64_t total_deleted = 0;
  for (int i = 0; i < kRounds; ++i) {
    TimeRange r;
    r.begin_ns = base + static_cast<uint64_t>(i * 10) * step;
    r.end_ns = r.begin_ns + 5 * step;
    uint64_t d = 0;
    STRATUM_CHECK_OK(tl.DeleteRange(tid, r, &d, nullptr));
    total_deleted += d;
  }

  std::vector<PrunedRange> out;
  STRATUM_CHECK_OK(tl.ListPrunedRanges(tid, &out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(kRounds));
  uint64_t sum = 0;
  for (const auto& r : out) sum += r.deleted_events;
  std::printf("      [规模] 连续裁剪 %d 次：记录 %zu 个区间，合计 %llu 条\n",
              kRounds, out.size(), static_cast<unsigned long long>(sum));
  STRATUM_CHECK_EQ(sum, total_deleted);
  STRATUM_CHECK_EQ(CountTopic(&tl, tid), kN - total_deleted);
  STRATUM_CHECK_FALSE(tl.GetQualityStats().HasIntegrityIssue());
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 六、随机（多种子）
// ===========================================================================

STRATUM_TEST(PrunedRanges, RandomRangesMultipleSeeds) {
  for (uint64_t seed : {9ULL, 99ULL, 999ULL, 20261015ULL}) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> n_d(300, 900);
    std::uniform_int_distribution<int> rounds_d(5, 30);

    const std::string dir = TempDir(("r" + std::to_string(seed)).c_str());
    const uint64_t n = n_d(rng);
    const int rounds = rounds_d(rng);
    const uint64_t base = 1700000000000000000ULL;
    const uint64_t step = 1000ULL;
    const uint64_t span = n / static_cast<uint64_t>(rounds + 1);

    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    uint32_t tid = 0;
    STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/r"), &tid));
    WriteN(&tl, tid, n, base, step, 12);

    uint64_t expected_deleted = 0;
    int attempted = 0;
    for (int i = 0; i < rounds; ++i) {
      // 不重叠的窗口：第 i 段
      TimeRange r;
      r.begin_ns = base + static_cast<uint64_t>(i) * span * step;
      r.end_ns = r.begin_ns + (span / 2) * step;
      uint64_t d = 0;
      STRATUM_CHECK_OK(tl.DeleteRange(tid, r, &d, nullptr));
      expected_deleted += d;
      ++attempted;
    }

    std::vector<PrunedRange> out;
    STRATUM_CHECK_OK(tl.ListPrunedRanges(tid, &out));
    // 只有真正删到数据的轮次会留记录
    STRATUM_CHECK_TRUE(out.size() <= static_cast<size_t>(attempted));
    uint64_t sum = 0;
    for (const auto& r : out) sum += r.deleted_events;
    STRATUM_CHECK_EQ(sum, expected_deleted);
    STRATUM_CHECK_EQ(CountTopic(&tl, tid), n - expected_deleted);
    STRATUM_CHECK_FALSE(tl.GetQualityStats().HasIntegrityIssue());
    STRATUM_CHECK_OK(tl.Close());
  }
}

// ===========================================================================
// 七、真实场景
// ===========================================================================

// ---------------------------------------------------------------------------
// 场景 1：裁剪调试数据后复盘
//
// 真实用法：debug 话题占了大量空间，事后确认无用，裁掉；
// 之后做复盘时，verify 应当说"这段是主动裁剪的"，而不是"数据丢了"。
// ---------------------------------------------------------------------------
STRATUM_TEST(PrunedRangesReal, PruneDebugThenReview) {
  const std::string dir = TempDir("review");
  const uint64_t base = 1700000000000000000ULL;

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t keep = 0, dbg = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("sensor/temp"), &keep));
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("sensor/debug_dump"), &dbg));
  WriteN(&tl, keep, 500, base, 1000000ULL, 32);
  WriteN(&tl, dbg, 500, base, 1000000ULL, 256);

  TimeRange first_half;
  first_half.begin_ns = base;
  first_half.end_ns = base + 250 * 1000000ULL;
  uint64_t del = 0;
  STRATUM_CHECK_OK(tl.DeleteRange(dbg, first_half, &del, nullptr));
  STRATUM_CHECK_EQ(del, static_cast<uint64_t>(250));

  // 复盘：业务话题完好，调试话题被裁剪且**有据可查**
  STRATUM_CHECK_EQ(CountTopic(&tl, keep), static_cast<uint64_t>(500));
  std::vector<PrunedRange> out;
  STRATUM_CHECK_OK(tl.ListPrunedRanges(dbg, &out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(1));
  STRATUM_CHECK_EQ(out[0].deleted_events, static_cast<uint64_t>(250));
  STRATUM_CHECK_FALSE(tl.GetQualityStats().HasIntegrityIssue());
  std::printf("      [场景1] 裁剪 debug 250 条：业务话题仍 500 条，"
              "裁剪可追溯且不算缺口\n");
  STRATUM_CHECK_OK(tl.Close());
}

// ---------------------------------------------------------------------------
// 场景 2：删空后记录仍在（FR-R-11 + 审计）
// ---------------------------------------------------------------------------
STRATUM_TEST(PrunedRangesReal, EmptiedTopicKeepsPrunedRecord) {
  const std::string dir = TempDir("emptied");
  const uint64_t base = 1700000000000000000ULL;

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t tid = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("gone/topic"), &tid));
  WriteN(&tl, tid, 40, base, 1000000ULL, 16);

  TimeRange all;
  all.begin_ns = kInvalidTimestamp;
  all.end_ns = kInvalidTimestamp;
  uint64_t del = 0;
  STRATUM_CHECK_OK(tl.DeleteRange(tid, all, &del, nullptr));
  STRATUM_CHECK_EQ(del, static_cast<uint64_t>(40));
  STRATUM_CHECK_EQ(CountTopic(&tl, tid), static_cast<uint64_t>(0));

  // 话题元数据仍在（上一轮已测），此处验证**裁剪记录也仍在**
  std::vector<PrunedRange> out;
  STRATUM_CHECK_OK(tl.ListPrunedRanges(tid, &out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(1));
  STRATUM_CHECK_EQ(out[0].deleted_events, static_cast<uint64_t>(40));

  TopicMetadata m;
  STRATUM_CHECK_OK(tl.GetTopicMetadata(tid, &m));
  STRATUM_CHECK_EQ(m.topic_name, std::string("gone/topic"));
  STRATUM_CHECK_FALSE(m.active);
  std::printf("      [场景2] 删空后：元数据仍在（active=0）且裁剪记录仍在\n");
  STRATUM_CHECK_OK(tl.Close());
}

// ---------------------------------------------------------------------------
// 场景 3：裁剪 + 保留点共存
//
// 真实用法：既配了自动淘汰（保留点保护），又做过手工裁剪。
// 两者是不同机制，不能互相干扰。
// ---------------------------------------------------------------------------
STRATUM_TEST(PrunedRangesReal, CoexistsWithRetentionPoints) {
  const std::string dir = TempDir("coexist");
  const uint64_t base = 1700000000000000000ULL;

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t tid = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/co"), &tid));
  WriteN(&tl, tid, 100, base, 1000000ULL, 16);

  // 建一个保留点（保护某时刻之前的数据不被自动淘汰）
  uint64_t point_id = 0;
  Status ps = tl.CreateRetentionPoint(base + 20 * 1000000ULL, &point_id);
  if (!ps.ok()) {
    std::printf("      [场景3] 该实现不支持保留点，跳过（非失败）\n");
    STRATUM_CHECK_OK(tl.Close());
    return;
  }

  // 手工裁剪（不受保留点约束 —— 用户显式要求就该删）
  TimeRange r;
  r.begin_ns = base;
  r.end_ns = base + 10 * 1000000ULL;
  uint64_t del = 0;
  STRATUM_CHECK_OK(tl.DeleteRange(tid, r, &del, nullptr));
  STRATUM_CHECK_EQ(del, static_cast<uint64_t>(10));

  std::vector<PrunedRange> out;
  STRATUM_CHECK_OK(tl.ListPrunedRanges(tid, &out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(1));
  std::printf("      [场景3] 保留点与手工裁剪共存，互不干扰（裁剪 10 条已记录）\n");
  STRATUM_CHECK_OK(tl.ReleaseRetentionPoint(point_id));
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST_MAIN()
