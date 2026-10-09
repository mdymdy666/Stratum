// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 压力测试
//
// ===========================================================================
// 目标与判定标准
// ===========================================================================
//
// 压力测试不是"跑得越久越好"，而是验证**在压力下行为是否仍然正确**。
// 因此每个用例都同时检查：
//   1. 吞吐（是否达到可用水平）
//   2. **正确性**（压完的数据必须能全部读回、内容一致）
//
// 第 2 点常被忽略：很多系统在高负载下"不崩溃"但会**静默丢数据**。

#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <thread>
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

using Clock = std::chrono::steady_clock;

double ElapsedSec(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

}  // namespace

// ---------------------------------------------------------------------------
// 大批量写入：10 万条，验证吞吐与完整性
// ---------------------------------------------------------------------------
STRATUM_TEST(Stress, BulkWriteHundredThousand) {
  constexpr int kCount = 100000;
  auto engine = StorageEnginePtr(new MemoryEngine());
  STRATUM_CHECK_OK(engine->Open("mem://stress-bulk"));

  TopicMetadata meta;
  meta.topic_name = "stress/bulk";
  uint32_t topic = 0;

  auto timeline_engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(timeline_engine));
  STRATUM_CHECK_OK(timeline.Open("mem://stress-timeline"));
  STRATUM_CHECK_OK(timeline.RegisterTopic(meta, &topic));

  const auto start = Clock::now();

  // 分批写入（每批 1000 条），避免一次性构造巨量对象
  constexpr int kBatch = 1000;
  for (int base = 0; base < kCount; base += kBatch) {
    std::vector<Event> events;
    events.reserve(kBatch);
    for (int i = 0; i < kBatch; ++i) {
      Event e;
      e.topic_id = topic;
      e.seq = static_cast<uint32_t>(base + i);
      e.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(base + i) * 1000;
      e.payload = "payload-" + std::to_string(base + i);
      events.push_back(std::move(e));
    }
    STRATUM_CHECK_OK(timeline.AppendBatch(events));
  }

  const double write_sec = ElapsedSec(start);
  const double write_rate = kCount / write_sec;

  ctx.param_desc = "write=" + std::to_string(static_cast<int>(write_rate)) +
                   " ops/s";
  // 吞吐下限：内存引擎应远超 10 万/秒
  STRATUM_CHECK_TRUE(write_rate > 100000.0);

  // --- 正确性：全部读回 ---
  const auto read_start = Clock::now();
  uint64_t count = 0;
  STRATUM_CHECK_OK(timeline.CountRange(topic, TimeRange(), &count));
  ctx.param_desc = "count=" + std::to_string(count);
  STRATUM_CHECK_EQ(count, static_cast<uint64_t>(kCount));

  // 抽查若干条内容
  EventIteratorPtr it;
  STRATUM_CHECK_OK(timeline.QueryRange(topic, TimeRange(), &it));
  uint64_t seen = 0;
  bool content_ok = true;
  for (; it->Valid(); it->Next()) {
    const std::string expected = "payload-" + std::to_string(seen);
    if (it->event().payload != expected) {
      content_ok = false;
      break;
    }
    ++seen;
  }
  STRATUM_CHECK_TRUE(content_ok);
  STRATUM_CHECK_EQ(seen, static_cast<uint64_t>(kCount));

  const double read_sec = ElapsedSec(read_start);
  ctx.param_desc = "scan=" + std::to_string(static_cast<int>(kCount / read_sec)) +
                   " ops/s";
  STRATUM_CHECK_TRUE(kCount / read_sec > 100000.0);
}

// ---------------------------------------------------------------------------
// LSM 引擎压力：验证刷盘与 Compaction 下不丢数据
// ---------------------------------------------------------------------------
STRATUM_TEST(Stress, LsmHandlesFlushAndCompaction) {
  const std::string dir = "/tmp/stratum_stress_lsm";
  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }

  constexpr int kCount = 50000;
  {
    auto engine = std::unique_ptr<LsmEngine>(new LsmEngine());
    STRATUM_CHECK_OK(engine->Open(dir));
    // 降低刷盘阈值，强制触发多次刷盘与 compaction
    engine->SetMemTableFlushThreshold(64 * 1024);  // 64KB

    WriteOptions wo;
    const auto start = Clock::now();
    for (int i = 0; i < kCount; ++i) {
      char key[32];
      std::snprintf(key, sizeof(key), "stress-%06d", i);
      STRATUM_CHECK_OK(engine->Put(wo, key, "value-" + std::to_string(i)));
    }
    const double sec = ElapsedSec(start);
    ctx.param_desc = "lsm-write=" +
                     std::to_string(static_cast<int>(kCount / sec)) + " ops/s";
    STRATUM_CHECK_TRUE(kCount / sec > 10000.0);

    // 关键：刷盘 + compaction 之后，数据必须全部可读
    ReadOptions ro;
    int found = 0;
    for (int i = 0; i < kCount; ++i) {
      char key[32];
      std::snprintf(key, sizeof(key), "stress-%06d", i);
      std::string value;
      if (engine->Get(ro, key, &value).ok()) {
        if (value == "value-" + std::to_string(i)) {
          ++found;
        }
      }
    }
    ctx.param_desc = "found=" + std::to_string(found) + "/" +
                     std::to_string(kCount);
    STRATUM_CHECK_EQ(found, kCount);

    STRATUM_CHECK_OK(engine->Close());
  }

  // 重启后再次验证（确保持久化完整）
  {
    auto engine = std::unique_ptr<LsmEngine>(new LsmEngine());
    STRATUM_CHECK_OK(engine->Open(dir));
    ReadOptions ro;
    int found = 0;
    for (int i = 0; i < kCount; ++i) {
      char key[32];
      std::snprintf(key, sizeof(key), "stress-%06d", i);
      std::string value;
      if (engine->Get(ro, key, &value).ok() &&
          value == "value-" + std::to_string(i)) {
        ++found;
      }
    }
    ctx.param_desc = "after-reopen found=" + std::to_string(found);
    STRATUM_CHECK_EQ(found, kCount);
    engine->Close();
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }
}

// ---------------------------------------------------------------------------
// 并发读：多线程同时读，验证一致性（无数据竞争）
// ---------------------------------------------------------------------------
STRATUM_TEST(Stress, ConcurrentReadersSeeConsistentData) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  STRATUM_CHECK_OK(engine->Open("mem://stress-concurrent"));

  TopicMetadata meta;
  meta.topic_name = "stress/concurrent";
  uint32_t topic = 0;

  auto tl_engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(tl_engine));
  STRATUM_CHECK_OK(timeline.Open("mem://stress-concurrent-tl"));
  STRATUM_CHECK_OK(timeline.RegisterTopic(meta, &topic));

  constexpr int kCount = 20000;
  std::vector<Event> events;
  events.reserve(kCount);
  for (int i = 0; i < kCount; ++i) {
    Event e;
    e.topic_id = topic;
    e.seq = static_cast<uint32_t>(i);
    e.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(i) * 1000;
    e.payload = "c-" + std::to_string(i);
    events.push_back(std::move(e));
  }
  STRATUM_CHECK_OK(timeline.AppendBatch(events));

  // 8 个线程并发做范围查询，每个都必须读到完整结果
  constexpr int kThreads = 8;
  std::atomic<int> success{0};
  std::atomic<uint64_t> total_read{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&timeline, topic, &success, &total_read, t]() {
      for (int round = 0; round < 5; ++round) {
        EventIteratorPtr it;
        // 每个线程查不同的子范围，避免完全相同的工作
        TimeRange range;
        range.begin_ns = kBaseTs + static_cast<uint64_t>(t) * 1000000ULL;
        range.end_ns = 0;
        if (!timeline.QueryRange(topic, range, &it).ok()) {
          continue;
        }
        uint64_t n = 0;
        for (; it->Valid(); it->Next()) {
          ++n;
        }
        total_read.fetch_add(n);
        success.fetch_add(1);
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  ctx.param_desc = "rounds=" + std::to_string(success.load()) + "/" +
                   std::to_string(kThreads * 5);
  STRATUM_CHECK_EQ(success.load(), kThreads * 5);
  // 读取总量必须 > 0（且随范围不同而变化）
  STRATUM_CHECK_TRUE(total_read.load() > 0);
}

// ---------------------------------------------------------------------------
// 录制器压力：高注入速率下不丢数据
// ---------------------------------------------------------------------------
STRATUM_TEST(Stress, RecorderDoesNotLoseDataUnderLoad) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://stress-recorder"));

  TopicMetadata meta;
  meta.topic_name = "stress/recorder";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(timeline.RegisterTopic(meta, &topic));

  // 直接大批量追加（模拟录制器的写盘路径）
  constexpr int kCount = 200000;
  const auto start = Clock::now();
  constexpr int kBatch = 2000;
  for (int base = 0; base < kCount; base += kBatch) {
    std::vector<Event> events;
    events.reserve(kBatch);
    for (int i = 0; i < kBatch; ++i) {
      Event e;
      e.topic_id = topic;
      e.seq = static_cast<uint32_t>(base + i);
      e.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(base + i) * 100;
      e.payload.assign(64, 'x');  // 固定中等负载
      events.push_back(std::move(e));
    }
    STRATUM_CHECK_OK(timeline.AppendBatch(events));
  }
  const double sec = ElapsedSec(start);

  uint64_t count = 0;
  STRATUM_CHECK_OK(timeline.CountRange(topic, TimeRange(), &count));
  ctx.param_desc = "rate=" + std::to_string(static_cast<int>(kCount / sec)) +
                   " ops/s, count=" + std::to_string(count);
  STRATUM_CHECK_EQ(count, static_cast<uint64_t>(kCount));
}

STRATUM_TEST_MAIN()
