// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// Stratum 性能基准
//
// 输出可直接用于性能报告的原始数据。
// 运行：./benchmarks/stratum_bench [--quick]
//
// 测量项：
//   1. 存储引擎写入吞吐（Memory / LSM）
//   2. 范围扫描吞吐
//   3. 点查延迟
//   4. 时间线追加吞吐
//   5. 定点查询延迟
//   6. 启动恢复时间（LSM 重开）

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "stratum/core/timeline_impl.h"
#include "stratum/storage/lsm/lsm_engine.h"
#include "stratum/storage/memory/memory_engine.h"

using namespace stratum;
using namespace stratum::storage;

namespace {

using Clock = std::chrono::steady_clock;

double Sec(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

constexpr uint64_t kBaseTs = 1700000000000000000ULL;

struct BenchConfig {
  int write_count = 200000;
  int scan_count = 100000;
  int point_query_count = 50000;
  int fixed_point_count = 2000;
  bool quick = false;
};

void PrintHeader(const char* title) {
  std::printf("\n===== %s =====\n", title);
}

// ---------------------------------------------------------------------------
// 1. 存储引擎写入吞吐
// ---------------------------------------------------------------------------
void BenchWriteThroughput(const BenchConfig& cfg) {
  PrintHeader("1. 存储引擎写入吞吐");

  // Memory
  {
    auto engine = StorageEnginePtr(new MemoryEngine());
    if (!engine->Open("mem://bench").ok()) {
      std::printf("  [SKIP] memory open failed\n");
      return;
    }
    WriteOptions wo;
    const auto start = Clock::now();
    for (int i = 0; i < cfg.write_count; ++i) {
      char key[32];
      std::snprintf(key, sizeof(key), "bench-%08d", i);
      engine->Put(wo, key, "value-payload-0123456789");
    }
    const double sec = Sec(start, Clock::now());
    std::printf("  MemoryEngine : %8d ops in %6.3f s = %10.0f ops/s\n",
                cfg.write_count, sec, cfg.write_count / sec);
    engine->Close();
  }

  // LSM
  {
    const std::string dir = "/tmp/stratum_bench_lsm";
    if (system(("rm -rf " + dir).c_str()) != 0) {
      // 忽略
    }
    std::remove(dir.c_str());

    auto engine = StorageEnginePtr(new LsmEngine());
    if (!engine->Open(dir).ok()) {
      std::printf("  [SKIP] lsm open failed\n");
      return;
    }
    WriteOptions wo;
    const auto start = Clock::now();
    for (int i = 0; i < cfg.write_count; ++i) {
      char key[32];
      std::snprintf(key, sizeof(key), "bench-%08d", i);
      engine->Put(wo, key, "value-payload-0123456789");
    }
    const double sec = Sec(start, Clock::now());
    std::printf("  LsmEngine    : %8d ops in %6.3f s = %10.0f ops/s\n",
                cfg.write_count, sec, cfg.write_count / sec);

    // 恢复时间
    const auto close_start = Clock::now();
    engine->Close();
    const double close_sec = Sec(close_start, Clock::now());
    std::printf("  LSM 关闭(含刷盘): %6.3f s\n", close_sec);

    auto engine2 = StorageEnginePtr(new LsmEngine());
    const auto open_start = Clock::now();
    const Status s = engine2->Open(dir);
    const double open_sec = Sec(open_start, Clock::now());
    if (s.ok()) {
      std::printf("  LSM 重开(含恢复): %6.3f s\n", open_sec);
      engine2->Close();
    }
    if (system(("rm -rf " + dir).c_str()) != 0) {
      // 忽略
    }
  }
}

// ---------------------------------------------------------------------------
// 2. 范围扫描吞吐
// ---------------------------------------------------------------------------
void BenchRangeScan(const BenchConfig& cfg) {
  PrintHeader("2. 时间线范围扫描吞吐");

  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  if (!timeline.Open("mem://bench-scan").ok()) {
    std::printf("  [SKIP] open failed\n");
    return;
  }

  TopicMetadata meta;
  meta.topic_name = "bench/scan";
  uint32_t topic = 0;
  timeline.RegisterTopic(meta, &topic);

  // 准备数据
  const int total = std::max(cfg.scan_count, 10000);
  {
    constexpr int kBatch = 2000;
    for (int base = 0; base < total; base += kBatch) {
      std::vector<Event> events;
      events.reserve(kBatch);
      for (int i = 0; i < kBatch && base + i < total; ++i) {
        Event e;
        e.topic_id = topic;
        e.seq = static_cast<uint32_t>(base + i);
        e.source_timestamp_ns =
            kBaseTs + static_cast<uint64_t>(base + i) * 1000;
        e.payload.assign(64, 'x');
        events.push_back(std::move(e));
      }
      timeline.AppendBatch(events);
    }
  }

  const auto start = Clock::now();
  EventIteratorPtr it;
  if (!timeline.QueryRange(topic, TimeRange(), &it).ok()) {
    std::printf("  [SKIP] query failed\n");
    return;
  }
  uint64_t n = 0;
  for (; it->Valid(); it->Next()) {
    ++n;
  }
  const double sec = Sec(start, Clock::now());
  std::printf("  全量扫描 %8llu 条 in %6.3f s = %10.0f 条/s\n",
              static_cast<unsigned long long>(n), sec, n / sec);
}

// ---------------------------------------------------------------------------
// 3. 点查与定点查询延迟
// ---------------------------------------------------------------------------
void BenchPointQuery(const BenchConfig& cfg) {
  PrintHeader("3. 点查与定点查询延迟");

  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  if (!timeline.Open("mem://bench-point").ok()) {
    std::printf("  [SKIP] open failed\n");
    return;
  }

  TopicMetadata meta;
  meta.topic_name = "bench/point";
  uint32_t topic = 0;
  timeline.RegisterTopic(meta, &topic);

  constexpr int kTotal = 50000;
  {
    constexpr int kBatch = 2000;
    for (int base = 0; base < kTotal; base += kBatch) {
      std::vector<Event> events;
      events.reserve(kBatch);
      for (int i = 0; i < kBatch && base + i < kTotal; ++i) {
        Event e;
        e.topic_id = topic;
        e.seq = static_cast<uint32_t>(base + i);
        e.source_timestamp_ns =
            kBaseTs + static_cast<uint64_t>(base + i) * 1000000ULL;
        e.payload.assign(32, 'y');
        events.push_back(std::move(e));
      }
      timeline.AppendBatch(events);
    }
  }

  // 定点查询（随机时刻）
  const int n = std::min(cfg.fixed_point_count, kTotal);
  const auto start = Clock::now();
  int found = 0;
  for (int i = 0; i < n; ++i) {
    const uint64_t ts = kBaseTs + static_cast<uint64_t>(i) * 1000000ULL * 25;
    StateAtResult result;
    if (timeline.GetStateAt(topic, ts, &result).ok() &&
        result.kind == StateAtResult::Kind::kFound) {
      ++found;
    }
  }
  const double sec = Sec(start, Clock::now());
  std::printf("  定点查询 %8d 次 in %6.3f s = %10.0f 次/s (命中 %d)\n", n, sec,
              n / sec, found);
  if (n > 0) {
    std::printf("  定点查询平均延迟: %.1f us\n", sec / n * 1e6);
  }
}

// ---------------------------------------------------------------------------
// 4. 追加吞吐（含不同负载大小）
// ---------------------------------------------------------------------------
void BenchAppendWithPayloadSizes() {
  PrintHeader("4. 追加吞吐 vs 负载大小");

  const size_t sizes[] = {16, 64, 256, 1024, 4096};
  const char* labels[] = {"16 B", "64 B", "256 B", "1 KB", "4 KB"};

  for (int si = 0; si < 5; ++si) {
    auto engine = StorageEnginePtr(new MemoryEngine());
    TimelineImpl timeline(std::move(engine));
    if (!timeline.Open("mem://bench-append").ok()) {
      continue;
    }
    TopicMetadata meta;
    meta.topic_name = "bench/append";
    uint32_t topic = 0;
    timeline.RegisterTopic(meta, &topic);

    constexpr int kCount = 50000;
    constexpr int kBatch = 2000;
    const auto start = Clock::now();
    for (int base = 0; base < kCount; base += kBatch) {
      std::vector<Event> events;
      events.reserve(kBatch);
      for (int i = 0; i < kBatch; ++i) {
        Event e;
        e.topic_id = topic;
        e.seq = static_cast<uint32_t>(base + i);
        e.source_timestamp_ns =
            kBaseTs + static_cast<uint64_t>(base + i) * 1000;
        e.payload.assign(sizes[si], 'z');
        events.push_back(std::move(e));
      }
      timeline.AppendBatch(events);
    }
    const double sec = Sec(start, Clock::now());
    const double mb = static_cast<double>(kCount) *
                      static_cast<double>(sizes[si]) / (1024.0 * 1024.0);
    std::printf("  负载 %-5s : %8d 条 in %6.3f s = %10.0f 条/s = %8.1f MB/s\n",
                labels[si], kCount, sec, kCount / sec, mb / sec);
  }
}

}  // namespace

int main(int argc, char** argv) {
  BenchConfig cfg;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--quick") == 0) {
      cfg.quick = true;
      cfg.write_count = 50000;
      cfg.scan_count = 50000;
      cfg.point_query_count = 10000;
      cfg.fixed_point_count = 500;
    }
  }

  std::printf("=============================================================\n");
  std::printf(" Stratum 性能基准\n");
  std::printf(" 写入条数: %d\n", cfg.write_count);
  std::printf("=============================================================\n");

  BenchWriteThroughput(cfg);
  BenchRangeScan(cfg);
  BenchPointQuery(cfg);
  BenchAppendWithPayloadSizes();

  std::printf("\n完成。\n");
  return 0;
}
