// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 并发注册 / 追加 / 查询的压力测试
//
// 【为什么写这套测试】
// 上一轮实测中 `record` 段出现过一次
//   `timeout: the monitored command dumped core`
// 复现时未再出现。唯一差异是**并发**：
// 那次同时跑着两个 fastdds_pub_diag，record 与它们争用。
//
// 【静态审查发现的疑点】
// `TimelineImplV2::RegisterTopic` 的第一行是：
//     if (!opened_) return Status::ShutdownInProgress("timeline not open");
//     std::lock_guard<std::mutex> lock(mutex_);      // 锁在这一行才拿到
// 即 `opened_` 是**在锁外读取**的普通 bool。若此时另一个线程正在
// `Close()`（写 opened_ = false），就是一个**数据竞争**（TSan 会报）。
// 同样的形态在 `Close()` / `Flush()` 等入口也可能存在。
//
// 【本测试要覆盖的并发场景】
//   1. 多线程抢注**同一个**话题 —— 必须幂等，只有一个 topic_id
//   2. 多线程注册**不同**话题 —— 各自都要拿到 id
//   3. 注册 + 追加 + 查询 + 列表 混合并发
//   4. 多线程注册的同时**关闭** —— 最可能触发 opened_ 竞争
//   5. 大数据量：上百话题 × 并发追加
//
// 说明：普通构建下这些用例会通过（也可能偶然崩溃）。真正的价值在于
// 用 **TSan** 跑（`-fsanitize=thread`）—— 那才会稳定指出竞争位置。

// 标准库头文件必须最先包含
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"

namespace fs = std::filesystem;

using namespace stratum;

namespace {

std::string TempDir(const char* tag) {
  const std::string d = std::string("/tmp/stratum_conc_") + tag;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

TopicMetadata MakeTopic(const std::string& name) {
  TopicMetadata m;
  m.topic_name = name;
  m.type_name = "ConcType";
  return m;
}

Event MakeEvent(uint32_t topic_id, uint32_t seq, uint64_t src_ns, size_t len) {
  Event e;
  e.topic_id = topic_id;
  e.seq = seq;
  e.seq_in_ts = 0;
  e.source_timestamp_ns = src_ns;
  e.reception_timestamp_ns = src_ns + 1000;
  e.payload = std::string(len, 'c');
  return e;
}

// 把异常（若有）转成失败信号，避免线程里抛异常直接 terminate
std::atomic<int> g_errors{0};

}  // namespace

// ===========================================================================
// 一、抢注同一话题（幂等性）
// ===========================================================================

STRATUM_TEST(Concurrent, SameTopicFromManyThreadsIsIdempotent) {
  const std::string dir = TempDir("same");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  const int kThreads = 16;
  std::vector<std::thread> ts;
  std::vector<uint32_t> ids(kThreads, 0);
  std::vector<Status> sts(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    ts.emplace_back([&, i]() {
      sts[static_cast<size_t>(i)] = tl.RegisterTopic(MakeTopic("hot/topic"), &ids[static_cast<size_t>(i)]);
    });
  }
  for (auto& t : ts) t.join();

  // 全部应成功，且拿到**同一个** topic_id（幂等）
  for (int i = 0; i < kThreads; ++i) {
    STRATUM_CHECK_OK(sts[static_cast<size_t>(i)]);
  }
  for (int i = 1; i < kThreads; ++i) {
    STRATUM_CHECK_EQ(ids[static_cast<size_t>(i)], ids[0]);
  }
  STRATUM_CHECK_TRUE(ids[0] != 0);

  // 列表里只能有**一个**话题
  std::vector<TopicMetadata> topics;
  STRATUM_CHECK_OK(tl.ListTopics(&topics));
  STRATUM_CHECK_EQ(topics.size(), static_cast<size_t>(1));
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 二、并发注册不同话题
// ===========================================================================

STRATUM_TEST(Concurrent, DistinctTopicsFromManyThreads) {
  const std::string dir = TempDir("distinct");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  const int kThreads = 12;
  const int kPerThread = 10;
  std::vector<std::thread> ts;
  std::atomic<int> ok_count{0};

  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t]() {
      for (int k = 0; k < kPerThread; ++k) {
        uint32_t id = 0;
        const std::string name = "t/" + std::to_string(t) + "/" + std::to_string(k);
        if (tl.RegisterTopic(MakeTopic(name), &id).ok() && id != 0) {
          ok_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& x : ts) x.join();

  STRATUM_CHECK_EQ(ok_count.load(), kThreads * kPerThread);
  std::vector<TopicMetadata> topics;
  STRATUM_CHECK_OK(tl.ListTopics(&topics));
  STRATUM_CHECK_EQ(topics.size(), static_cast<size_t>(kThreads * kPerThread));

  // topic_id 必须唯一
  std::vector<uint32_t> ids;
  for (const auto& m : topics) ids.push_back(m.topic_id);
  std::sort(ids.begin(), ids.end());
  STRATUM_CHECK_TRUE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
  std::printf("      [并发] %d 线程注册 %d 个不同话题，id 全部唯一\n",
              kThreads, kThreads * kPerThread);
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 三、注册 + 追加 + 查询 混合并发（最接近真实录制）
// ===========================================================================

STRATUM_TEST(Concurrent, MixedRegisterAppendQuery) {
  const std::string dir = TempDir("mixed");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  // 预热：先注册好话题，避免"注册"与"追加"完全同时（那种情况下面单独测）
  const int kTopics = 8;
  std::vector<uint32_t> ids(kTopics, 0);
  for (int i = 0; i < kTopics; ++i) {
    STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("mix/" + std::to_string(i)),
                                      &ids[static_cast<size_t>(i)]));
  }

  const int kWriters = 8;
  const int kPerWriter = 200;
  const int kReaders = 4;
  std::atomic<int> appended{0};
  std::atomic<bool> stop{false};

  std::vector<std::thread> ts;
  for (int w = 0; w < kWriters; ++w) {
    ts.emplace_back([&, w]() {
      const uint32_t tid = ids[static_cast<size_t>(w % kTopics)];
      for (int k = 0; k < kPerWriter; ++k) {
        Event e = MakeEvent(tid, static_cast<uint32_t>(k + 1),
                            1700000000000000000ULL + static_cast<uint64_t>(k) * 1000000ULL,
                            24);
        if (tl.Append(e).ok()) appended.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (int r = 0; r < kReaders; ++r) {
    ts.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        std::vector<TopicMetadata> topics;
        (void)tl.ListTopics(&topics);
        TimeRange all;
        all.begin_ns = kInvalidTimestamp;
        all.end_ns = kInvalidTimestamp;
        uint64_t n = 0;
        (void)tl.CountRange(ids[0], all, &n);
      }
    });
  }

  // 等 writer 完成
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true);
  for (auto& x : ts) x.join();

  STRATUM_CHECK_TRUE(appended.load() > 0);
  std::printf("      [混合] 追加成功 %d 条（并发读期间）\n", appended.load());
  STRATUM_CHECK_FALSE(tl.GetQualityStats().HasIntegrityIssue());
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 四、注册的同时关闭（★ 最可能触发 opened_ 竞争）
// ===========================================================================

STRATUM_TEST(Concurrent, RegisterWhileClosing) {
  // 这是静态审查发现的可疑点：RegisterTopic 在**锁外**读 opened_，
  // Close() 在锁内写 opened_。两者并发即数据竞争。
  // 普通构建下这里可能"看起来没事"，TSan 会直接报出来。
  for (int round = 0; round < 20; ++round) {
    const std::string dir = TempDir(("closing" + std::to_string(round)).c_str());
    TimelineImplV2 tl;
    if (!tl.Open(dir).ok()) continue;

    std::atomic<bool> go{false};
    std::thread writer([&]() {
      while (!go.load(std::memory_order_acquire)) { /* spin */ }
      for (int k = 0; k < 50; ++k) {
        uint32_t id = 0;
        (void)tl.RegisterTopic(MakeTopic("race/t" + std::to_string(k)), &id);
      }
    });
    std::thread closer([&]() {
      while (!go.load(std::memory_order_acquire)) { /* spin */ }
      (void)tl.Close();
    });

    go.store(true, std::memory_order_release);
    writer.join();
    closer.join();
    // 只要没崩就算过（竞争由 TSan 报告）
  }
  std::printf("      [关闭竞争] 20 轮注册与关闭并发，未崩溃\n");
  STRATUM_CHECK_TRUE(true);
}

// ===========================================================================
// 五、大数据量：上百话题并发注册 + 追加
// ===========================================================================

STRATUM_TEST(Concurrent, LargeScaleManyTopics) {
  const std::string dir = TempDir("large");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  const int kThreads = 10;
  const int kTopicsPerThread = 30;      // 共 300 个话题
  const int kEventsPerTopic = 20;

  std::vector<std::thread> ts;
  std::atomic<int> registered{0};
  std::atomic<int> appended{0};

  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t]() {
      for (int k = 0; k < kTopicsPerThread; ++k) {
        uint32_t id = 0;
        const std::string name = "big/" + std::to_string(t) + "_" + std::to_string(k);
        if (!tl.RegisterTopic(MakeTopic(name), &id).ok()) continue;
        registered.fetch_add(1, std::memory_order_relaxed);
        for (int e = 0; e < kEventsPerTopic; ++e) {
          Event ev = MakeEvent(id, static_cast<uint32_t>(e + 1),
                               1700000000000000000ULL + static_cast<uint64_t>(e) * 1000ULL, 16);
          if (tl.Append(ev).ok()) appended.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& x : ts) x.join();

  STRATUM_CHECK_EQ(registered.load(), kThreads * kTopicsPerThread);
  STRATUM_CHECK_EQ(appended.load(), kThreads * kTopicsPerThread * kEventsPerTopic);
  std::printf("      [规模] %d 话题 / %d 条，全部并发写入成功\n",
              registered.load(), appended.load());

  std::vector<TopicMetadata> topics;
  STRATUM_CHECK_OK(tl.ListTopics(&topics));
  STRATUM_CHECK_EQ(topics.size(), static_cast<size_t>(kThreads * kTopicsPerThread));
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST_MAIN()
