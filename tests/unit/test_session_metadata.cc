// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 会话元数据的持久化与重载测试
//
// 【为什么单独测这个】曾经有一个**假阳性**缺陷：
//   TimelineImplV2::Close 会 PersistSessionLocked（写会话元数据），
//   但 Open **没有**对应的读取。于是新进程重开会话后
//   created_at_ns / closed_at_ns / total_events_ / total_bytes_ 全部回到初值：
//     · CLI 显示「创建: (未设置)」「关闭: (未正常关闭 —— 可能崩溃)」
//     · verify 据此**误报**"(未正常关闭)"—— 假阳性比缺失更危险
//   更糟的是：Open 后若未 append 就 Close，会把这套初值**覆盖**已存的真实元数据，
//   属于**数据损坏**，且症状隐蔽（事后只看到"未设置"，不知道被谁清的）。
//
// 覆盖三类（项目测试标准）：
//   · 大数据量 —— 10 万条事件的统计与时间跨度
//   · 随机     —— 多种子随机条数/时间戳，重开后必须一致
//   · 真实场景 —— ① 录制后复盘 ② 只读查询幂等 ③ 崩溃后重开

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
  const std::string d = std::string("/tmp/stratum_sessmeta_") + tag;
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

Event MakeEvent(uint32_t topic_id, uint32_t seq, uint64_t src_ns,
                const std::string& payload) {
  Event e;
  e.topic_id = topic_id;
  e.seq = seq;
  e.seq_in_ts = 0;
  e.source_timestamp_ns = src_ns;
  e.reception_timestamp_ns = src_ns + 1000;
  e.payload = payload;
  return e;
}

// 写入 n 条事件，时间戳从 base 起每条 +step
void WriteEvents(TimelineImplV2* tl, uint32_t topic_id, uint64_t n,
                 uint64_t base, uint64_t step) {
  std::vector<Event> batch;
  batch.reserve(n);
  for (uint64_t i = 0; i < n; ++i) {
    batch.push_back(MakeEvent(topic_id, static_cast<uint32_t>(i + 1),
                              base + i * step, std::string(16, 'x')));
  }
  STRATUM_CHECK_OK(tl->AppendBatch(batch));
}

}  // namespace

// ===========================================================================
// 一、基础契约
// ===========================================================================

STRATUM_TEST(SessionMeta, PersistAndReload) {
  const std::string dir = TempDir("persist");

  const uint64_t base = 1700000000000000000ULL;
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    uint32_t tid = 0;
    STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/a"), &tid));
    WriteEvents(&tl, tid, 3, base, 1000000ULL);
    STRATUM_CHECK_OK(tl.Close());
  }
  {
    // 重开：必须能读回上次持久化的会话元数据
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    SessionMetadata m;
    STRATUM_CHECK_OK(tl.GetSessionMetadata(&m));
    STRATUM_CHECK_EQ(m.created_at_ns, base);
    STRATUM_CHECK_EQ(m.closed_at_ns, base + 2 * 1000000ULL);
    STRATUM_CHECK_EQ(m.total_events, static_cast<uint64_t>(3));
    STRATUM_CHECK_EQ(m.total_bytes, static_cast<uint64_t>(3 * 16));
    STRATUM_CHECK_OK(tl.Close());
  }
}

STRATUM_TEST(SessionMeta, ReopenThenCloseMustNotClobber) {
  // 【核心保护】只读地打开再关闭，绝不能把已有元数据清零
  const std::string dir = TempDir("clobber");

  const uint64_t base = 1700000000000000000ULL;
  uint64_t created0 = 0, closed0 = 0, events0 = 0;
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    uint32_t tid = 0;
    STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/b"), &tid));
    WriteEvents(&tl, tid, 5, base, 500000ULL);
    STRATUM_CHECK_OK(tl.Close());
  }
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    SessionMetadata m;
    STRATUM_CHECK_OK(tl.GetSessionMetadata(&m));
    created0 = m.created_at_ns;
    closed0 = m.closed_at_ns;
    events0 = m.total_events;
    STRATUM_CHECK_OK(tl.Close());   // 不 append 任何东西
  }
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    SessionMetadata m;
    STRATUM_CHECK_OK(tl.GetSessionMetadata(&m));
    STRATUM_CHECK_EQ(m.created_at_ns, created0);
    STRATUM_CHECK_EQ(m.closed_at_ns, closed0);
    STRATUM_CHECK_EQ(m.total_events, events0);
    STRATUM_CHECK_TRUE(m.created_at_ns != 0);
    STRATUM_CHECK_OK(tl.Close());
  }
}

STRATUM_TEST(SessionMeta, EmptySessionHasNoTimestamps) {
  const std::string dir = TempDir("empty");
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    STRATUM_CHECK_OK(tl.Close());   // 从未 append
  }
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    SessionMetadata m;
    STRATUM_CHECK_OK(tl.GetSessionMetadata(&m));
    // 没有数据就不该有"创建/关闭时间" —— 而不是编一个 0 当成时间戳
    STRATUM_CHECK_EQ(m.created_at_ns, kInvalidTimestamp);
    STRATUM_CHECK_EQ(m.closed_at_ns, kInvalidTimestamp);
    STRATUM_CHECK_EQ(m.total_events, static_cast<uint64_t>(0));
    STRATUM_CHECK_OK(tl.Close());
  }
}

// ===========================================================================
// 二、大数据量
// ===========================================================================

STRATUM_TEST(SessionMeta, LargeVolumeHundredThousand) {
  const std::string dir = TempDir("big");
  const uint64_t kN = 100000;
  const uint64_t base = 1700000000000000000ULL;
  const uint64_t step = 1000000ULL;   // 1ms

  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    uint32_t tid = 0;
    STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/big"), &tid));
    WriteEvents(&tl, tid, kN, base, step);
    STRATUM_CHECK_OK(tl.Close());
  }
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    SessionMetadata m;
    STRATUM_CHECK_OK(tl.GetSessionMetadata(&m));
    STRATUM_CHECK_EQ(m.total_events, kN);
    STRATUM_CHECK_EQ(m.created_at_ns, base);
    STRATUM_CHECK_EQ(m.closed_at_ns, base + (kN - 1) * step);
    const double span_s = static_cast<double>(m.closed_at_ns - m.created_at_ns) / 1e9;
    std::printf("      [规模] %llu 条：跨度 %.3f 秒，字节 %llu\n",
                static_cast<unsigned long long>(m.total_events), span_s,
                static_cast<unsigned long long>(m.total_bytes));
    STRATUM_CHECK_TRUE(span_s > 99.0 && span_s < 101.0);
    STRATUM_CHECK_OK(tl.Close());
  }
}

// ===========================================================================
// 三、随机（多种子）
// ===========================================================================

STRATUM_TEST(SessionMeta, RandomReloadConsistentMultipleSeeds) {
  for (uint64_t seed : {11ULL, 222ULL, 3333ULL, 20261015ULL}) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> n_d(1, 400);
    std::uniform_int_distribution<uint64_t> step_d(1, 10000000);
    std::uniform_int_distribution<uint64_t> base_d(1, 4000000000000000000ULL);

    const std::string dir = TempDir(("rnd" + std::to_string(seed)).c_str());
    const uint64_t n = n_d(rng);
    const uint64_t step = step_d(rng);
    const uint64_t base = base_d(rng);
    const uint64_t plen = 8 + (seed % 32);

    {
      TimelineImplV2 tl;
      STRATUM_CHECK_OK(tl.Open(dir));
      uint32_t tid = 0;
      STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/r"), &tid));
      std::vector<Event> batch;
      batch.reserve(n);
      for (uint64_t i = 0; i < n; ++i) {
        batch.push_back(MakeEvent(tid, static_cast<uint32_t>(i + 1),
                                  base + i * step, std::string(plen, 'y')));
      }
      STRATUM_CHECK_OK(tl.AppendBatch(batch));
      STRATUM_CHECK_OK(tl.Close());
    }
    {
      TimelineImplV2 tl;
      STRATUM_CHECK_OK(tl.Open(dir));
      SessionMetadata m;
      STRATUM_CHECK_OK(tl.GetSessionMetadata(&m));
      STRATUM_CHECK_EQ(m.total_events, n);
      STRATUM_CHECK_EQ(m.total_bytes, n * plen);
      STRATUM_CHECK_EQ(m.created_at_ns, base);
      STRATUM_CHECK_EQ(m.closed_at_ns, base + (n - 1) * step);
      STRATUM_CHECK_OK(tl.Close());
    }
  }
}

// ===========================================================================
// 四、真实场景
// ===========================================================================

// ---------------------------------------------------------------------------
// 场景 1：录制完成后做复盘
//
// 真实用法：record 写到一半/结束 → 事后用 session info / verify 查看
// "这段录制是什么时候录的、录了多少"。这正是之前会显示
// "创建: (未设置)" 的场景。
// ---------------------------------------------------------------------------
STRATUM_TEST(SessionMetaReal, RecordingThenReview) {
  const std::string dir = TempDir("review");
  const uint64_t base = 1792044082000000000ULL;   // 贴近真实录制的量级
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    uint32_t tid = 0;
    STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("sensor/temp"), &tid));
    // 模拟 6 秒、每秒 60 条
    for (int sec = 0; sec < 6; ++sec) {
      std::vector<Event> batch;
      for (int i = 0; i < 60; ++i) {
        batch.push_back(MakeEvent(tid, static_cast<uint32_t>(sec * 60 + i + 1),
                                  base + static_cast<uint64_t>(sec * 1000 + i * 16) * 1000000ULL,
                                  std::string(24, 'z')));
      }
      STRATUM_CHECK_OK(tl.AppendBatch(batch));
    }
    STRATUM_CHECK_OK(tl.Close());
  }

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  SessionMetadata m;
  STRATUM_CHECK_OK(tl.GetSessionMetadata(&m));
  // 复盘的关键三问必须都能回答
  STRATUM_CHECK_TRUE(m.created_at_ns != kInvalidTimestamp);   // 什么时候开始录的
  STRATUM_CHECK_TRUE(m.closed_at_ns != kInvalidTimestamp);    // 什么时候结束的
  STRATUM_CHECK_EQ(m.total_events, static_cast<uint64_t>(360));  // 录了多少
  std::printf("      [场景1] 复盘：创建=%llu 关闭=%llu 事件=%llu\n",
              static_cast<unsigned long long>(m.created_at_ns),
              static_cast<unsigned long long>(m.closed_at_ns),
              static_cast<unsigned long long>(m.total_events));
  STRATUM_CHECK_OK(tl.Close());
}

// ---------------------------------------------------------------------------
// 场景 2：只读查询必须幂等（不能把只读操作变成写操作）
//
// 真实用法：CI 里反复跑 `stratum session info` / `verify` / `query`，
// 或在录制结束后多次查看。这些命令都会 Open+Close，
// 若 Close 会覆盖元数据，反复查询就会把数据"查坏"。
// ---------------------------------------------------------------------------
STRATUM_TEST(SessionMetaReal, ReadOnlyQueriesAreIdempotent) {
  const std::string dir = TempDir("idem");
  const uint64_t base = 1700000000000000000ULL;
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    uint32_t tid = 0;
    STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/idem"), &tid));
    WriteEvents(&tl, tid, 20, base, 1000000ULL);
    STRATUM_CHECK_OK(tl.Close());
  }

  uint64_t created0 = 0, closed0 = 0, events0 = 0, bytes0 = 0;
  for (int round = 0; round < 10; ++round) {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    SessionMetadata m;
    STRATUM_CHECK_OK(tl.GetSessionMetadata(&m));
    if (round == 0) {
      created0 = m.created_at_ns;
      closed0 = m.closed_at_ns;
      events0 = m.total_events;
      bytes0 = m.total_bytes;
      STRATUM_CHECK_TRUE(created0 != 0);
    } else {
      // 10 轮只读查询后必须完全一致
      STRATUM_CHECK_EQ(m.created_at_ns, created0);
      STRATUM_CHECK_EQ(m.closed_at_ns, closed0);
      STRATUM_CHECK_EQ(m.total_events, events0);
      STRATUM_CHECK_EQ(m.total_bytes, bytes0);
    }
    STRATUM_CHECK_OK(tl.Close());
  }
  std::printf("      [场景2] 10 轮只读查询后元数据不变（创建=%llu 事件=%llu）\n",
              static_cast<unsigned long long>(created0),
              static_cast<unsigned long long>(events0));
}

// ---------------------------------------------------------------------------
// 场景 3：崩溃后重开
//
// 真实用法：录制器被强杀（EC-12），没有走 Close。
// 此时最后一批的统计可能未落盘，但**已落盘的部分**必须能正确读回 ——
// 不能因为"没正常关闭"就把已有的元数据丢掉。
// ---------------------------------------------------------------------------
STRATUM_TEST(SessionMetaReal, CrashThenReopenKeepsPersisted) {
  const std::string dir = TempDir("crash");
  const uint64_t base = 1700000000000000000ULL;

  // 第一批：正常写入并关闭（模拟"已安全落盘的部分"）
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    uint32_t tid = 0;
    STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/crash"), &tid));
    WriteEvents(&tl, tid, 30, base, 1000000ULL);
    STRATUM_CHECK_OK(tl.Flush());
    STRATUM_CHECK_OK(tl.Close());
  }

  // 第二批：写入后**不 Close** 就直接让对象析构/重开（模拟崩溃）
  uint64_t after_crash_events = 0;
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    uint32_t tid = 0;
    STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/crash"), &tid));
    WriteEvents(&tl, tid, 10, base + 100000000ULL, 1000000ULL);
    STRATUM_CHECK_OK(tl.Flush());
    SessionMetadata m;
    STRATUM_CHECK_OK(tl.GetSessionMetadata(&m));
    after_crash_events = m.total_events;   // 内存里已含新增
    // 故意不 Close
  }

  // 重开：已落盘的历史必须还在，且不能是 0
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  SessionMetadata m;
  STRATUM_CHECK_OK(tl.GetSessionMetadata(&m));
  STRATUM_CHECK_TRUE(m.total_events >= 30);   // 至少保住第一批
  STRATUM_CHECK_TRUE(m.created_at_ns != 0);
  std::printf("      [场景3] 崩溃重开：落盘事件=%llu（崩溃时内存=%llu）\n",
              static_cast<unsigned long long>(m.total_events),
              static_cast<unsigned long long>(after_crash_events));
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST_MAIN()
