// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 真实形态测试
//
// ===========================================================================
// 为什么需要这一类测试
// ===========================================================================
//
// 前面的测试用的是"理想数据"：时间戳规整递增、负载大小均匀、无丢失。
// 但真实传感器数据具有以下特征，必须专门验证：
//
//   1. **周期性采样 + 抖动**：名义 10ms 一条，实际在 9~12ms 间波动
//   2. **偶发丢帧**：网络抖动导致个别采样丢失（时间轴上出现空洞）
//   3. **时钟漂移**：源时钟与接收时钟不同步，两个时间戳差值缓慢变化
//   4. **突发**：设备重连后一次性回补多条历史数据
//   5. **多话题不同频率**：温度 1Hz、IMU 200Hz、控制指令不定时
//
// 这些特征会触发理想数据测不出的问题（如质量报告误报、空洞检测失效）。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl.h"
#include "stratum/storage/memory/memory_engine.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::storage;

namespace {

constexpr uint64_t kBaseTs = 1700000000000000000ULL;

// 模拟一个真实传感器话题的采样序列
struct SensorSpec {
  const char* name;
  uint64_t period_ns;      // 名义采样周期
  uint64_t jitter_ns;      // 抖动幅度（±）
  double drop_rate;        // 丢帧率
  uint64_t payload_size;   // 负载大小
};

// 生成符合规格的样本序列，返回实际生成的事件与期望的丢帧数
struct GeneratedStream {
  std::vector<Event> events;
  uint64_t expected_drops = 0;   // 名义上应发生的丢帧数
  uint64_t nominal_count = 0;    // 按周期计算的名义条数
};

GeneratedStream GenerateStream(uint32_t topic_id, const SensorSpec& spec,
                               uint64_t duration_ns, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> jitter(
      -static_cast<int64_t>(spec.jitter_ns),
      static_cast<int64_t>(spec.jitter_ns));
  std::uniform_real_distribution<double> uniform(0.0, 1.0);

  GeneratedStream result;
  result.nominal_count = duration_ns / spec.period_ns;

  uint64_t cursor = kBaseTs;
  uint32_t seq = 0;
  while (cursor < kBaseTs + duration_ns) {
    // 抖动（真实传感器不会精确按周期采样）
    const int64_t offset = jitter(rng);
    const uint64_t ts =
        static_cast<uint64_t>(static_cast<int64_t>(cursor) + offset);

    // 丢帧模拟
    if (uniform(rng) < spec.drop_rate) {
      ++result.expected_drops;
      cursor += spec.period_ns;
      continue;
    }

    Event e;
    e.topic_id = topic_id;
    e.seq = seq++;
    e.source_timestamp_ns = ts;
    // 接收延迟：真实系统有处理与传输延迟，且随负载波动
    const uint64_t base_latency = 200000;  // 200us
    const uint64_t latency_jitter = static_cast<uint64_t>(rng() % 100000);
    e.reception_timestamp_ns = ts + base_latency + latency_jitter;
    e.payload.assign(spec.payload_size, 'D');
    result.events.push_back(std::move(e));

    cursor += spec.period_ns;
  }
  return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// 多话题不同频率：模拟真实采集系统的混合负载
// ---------------------------------------------------------------------------
STRATUM_TEST(RealWorld, MultiTopicDifferentRates) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://real-multi"));

  // 三类典型话题
  const SensorSpec specs[] = {
      {"sensor/temperature", 1000000000ULL, 5000000ULL, 0.01, 32},   // 1 Hz
      {"sensor/imu", 5000000ULL, 200000ULL, 0.05, 128},              // 200 Hz
      {"control/command", 200000000ULL, 50000000ULL, 0.10, 64},      // 5 Hz
  };

  constexpr uint64_t kDurationNs = 5000000000ULL;  // 5 秒
  uint64_t total_events = 0;

  for (int i = 0; i < 3; ++i) {
    TopicMetadata meta;
    meta.topic_name = specs[i].name;
    meta.type_name = "SensorData";
    uint32_t topic = 0;
    STRATUM_CHECK_OK(timeline.RegisterTopic(meta, &topic));

    auto stream = GenerateStream(topic, specs[i], kDurationNs,
                                 1000 + static_cast<uint64_t>(i));
    STRATUM_CHECK_OK(timeline.AppendBatch(stream.events));
    total_events += stream.events.size();

    // 每话题数据量必须与生成的条数一致
    uint64_t count = 0;
    STRATUM_CHECK_OK(timeline.CountRange(topic, TimeRange(), &count));
    ctx.param_desc = std::string(specs[i].name) + " count=" +
                     std::to_string(count);
    STRATUM_CHECK_EQ(count, static_cast<uint64_t>(stream.events.size()));
  }

  ctx.param_desc = "total=" + std::to_string(total_events);
  // 5 秒内 200Hz 名义 1000 条，含 5% 丢帧后约 950 条；
  // 加上 1Hz 与 5Hz 两个话题，总量应在 900 以上
  STRATUM_CHECK_TRUE(total_events > 900);

  // 话题数正确
  std::vector<TopicMetadata> topics;
  STRATUM_CHECK_OK(timeline.ListTopics(&topics));
  STRATUM_CHECK_EQ(topics.size(), static_cast<size_t>(3));
}

// ---------------------------------------------------------------------------
// 丢帧检测：质量报告必须能发现真实的丢帧
// ---------------------------------------------------------------------------
STRATUM_TEST(RealWorld, QualityReportDetectsRealDrops) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://real-drops"));

  TopicMetadata meta;
  meta.topic_name = "sensor/with_drops";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(timeline.RegisterTopic(meta, &topic));

  // 10Hz 采样 10 秒，丢 20%
  SensorSpec spec{"sensor/with_drops", 100000000ULL, 1000000ULL, 0.20, 32};
  auto stream = GenerateStream(topic, spec, 10000000000ULL, 42);
  STRATUM_CHECK_OK(timeline.AppendBatch(stream.events));

  ctx.param_desc = "generated=" + std::to_string(stream.events.size()) +
                   " nominal=" + std::to_string(stream.nominal_count);
  // 生成器确实丢了帧
  STRATUM_CHECK_TRUE(stream.events.size() < stream.nominal_count);

  // 质量报告应检出空洞（用比采样周期大的阈值）
  QualityReporter reporter(&timeline);
  QualityReport report;
  // 阈值设为周期的 1.5 倍，这样正常采样不会误报，丢帧会报
  STRATUM_CHECK_OK(reporter.Generate(150000000ULL, &report));

  STRATUM_CHECK_EQ(report.topics.size(), static_cast<size_t>(1));
  ctx.param_desc = "gaps=" + std::to_string(report.topics[0].gaps);
  STRATUM_CHECK_TRUE(report.topics[0].gaps > 0);
  // 有空洞 → 评级不能是 Good
  STRATUM_CHECK_TRUE(report.level != QualityReport::Level::kGood);
}

// ---------------------------------------------------------------------------
// 抖动容忍：正常抖动不应被误报为空洞
// ---------------------------------------------------------------------------
STRATUM_TEST(RealWorld, JitterDoesNotCauseFalsePositives) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://real-jitter"));

  TopicMetadata meta;
  meta.topic_name = "sensor/no_drops";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(timeline.RegisterTopic(meta, &topic));

  // 10Hz，抖动 ±10%（1ms），**不丢帧**
  SensorSpec spec{"sensor/no_drops", 100000000ULL, 10000000ULL, 0.0, 32};
  auto stream = GenerateStream(topic, spec, 10000000000ULL, 43);
  STRATUM_CHECK_OK(timeline.AppendBatch(stream.events));

  // 阈值必须大于"正常间隔 + 抖动"（100ms + 10ms），否则正常采样会被误报。
  // 设 150ms：正常间隔（90~110ms）不报，丢帧（约 200ms）才报。
  QualityReporter reporter(&timeline);
  QualityReport report;
  STRATUM_CHECK_OK(reporter.Generate(150000000ULL, &report));

  ctx.param_desc = "gaps=" + std::to_string(report.topics[0].gaps);
  STRATUM_CHECK_EQ(report.topics[0].gaps, static_cast<uint64_t>(0));
  // 无丢帧 → 评级应为 Good
  STRATUM_CHECK_EQ(static_cast<int>(report.level),
                   static_cast<int>(QualityReport::Level::kGood));
}

// ---------------------------------------------------------------------------
// 突发回补：设备重连后一次性补发历史数据（时间戳倒挂）
// ---------------------------------------------------------------------------
STRATUM_TEST(RealWorld, BurstBackfillWithOutOfOrderTimestamps) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://real-burst"));

  TopicMetadata meta;
  meta.topic_name = "device/reconnect";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(timeline.RegisterTopic(meta, &topic));

  // 正常采集 1 秒（10Hz）
  auto normal = GenerateStream(topic, {"device/reconnect", 100000000ULL, 1000000ULL,
                                       0.0, 64},
                               1000000000ULL, 44);
  std::vector<Event> all = normal.events;

  // 突发回补：补发之前的 0.5 秒（时间戳**早于**已有数据）
  auto backfill = GenerateStream(topic, {"device/reconnect", 100000000ULL,
                                         1000000ULL, 0.0, 64},
                                 1000000000ULL, 45);
  for (auto& e : backfill.events) {
    if (e.source_timestamp_ns < kBaseTs + 500000000ULL) {
      e.seq += 10000;  // 序号区分
      all.push_back(std::move(e));
    }
  }

  STRATUM_CHECK_OK(timeline.AppendBatch(all));

  // 关键：乱序写入不论顺序如何，读取必须按时间戳升序，且一条不丢
  uint64_t count = 0;
  STRATUM_CHECK_OK(timeline.CountRange(topic, TimeRange(), &count));
  ctx.param_desc = "appended=" + std::to_string(all.size()) +
                   " read=" + std::to_string(count);
  STRATUM_CHECK_EQ(count, static_cast<uint64_t>(all.size()));

  EventIteratorPtr it;
  STRATUM_CHECK_OK(timeline.QueryRange(topic, TimeRange(), &it));
  TimestampNs prev = 0;
  size_t n = 0;
  for (; it->Valid(); it->Next()) {
    if (n > 0) {
      STRATUM_CHECK_TRUE(it->event().source_timestamp_ns >= prev);
    }
    prev = it->event().source_timestamp_ns;
    ++n;
  }
  STRATUM_CHECK_EQ(n, all.size());
}

// ---------------------------------------------------------------------------
// 时钟漂移：源与接收时间戳差值缓慢变化
// ---------------------------------------------------------------------------
STRATUM_TEST(RealWorld, ClockDriftIsObservable) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://real-drift"));

  TopicMetadata meta;
  meta.topic_name = "sensor/drift";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(timeline.RegisterTopic(meta, &topic));

  // 构造 1000 条，接收延迟从 100us 线性增长到 5ms（模拟时钟漂移）
  constexpr int kCount = 1000;
  std::vector<Event> events;
  events.reserve(kCount);
  for (int i = 0; i < kCount; ++i) {
    Event e;
    e.topic_id = topic;
    e.seq = static_cast<uint32_t>(i);
    e.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(i) * 10000000ULL;
    const uint64_t latency = 100000ULL +
                             static_cast<uint64_t>(i) * 4900ULL;  // 递增
    e.reception_timestamp_ns = e.source_timestamp_ns + latency;
    e.payload = "x";
    events.push_back(std::move(e));
  }
  STRATUM_CHECK_OK(timeline.AppendBatch(events));

  const QualityStats stats = timeline.GetQualityStats();
  ctx.param_desc = "min=" + std::to_string(stats.min_latency_ns) +
                   " max=" + std::to_string(stats.max_latency_ns);
  // 延迟应被完整记录，且 min < max（漂移可见）
  STRATUM_CHECK_EQ(stats.latency_samples, static_cast<uint64_t>(kCount));
  STRATUM_CHECK_LT(stats.min_latency_ns, stats.max_latency_ns);
  // 漂移不应被误判为时间戳倒退（接收时间戳仍是递增的）
  STRATUM_CHECK_EQ(stats.timestamp_regressions, static_cast<uint64_t>(0));
}

// ---------------------------------------------------------------------------
// 真实形态的端到端：录制 → 回放 → 时序保真
// ---------------------------------------------------------------------------
STRATUM_TEST(RealWorld, ReplayPreservesTiming) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://real-replay"));

  TopicMetadata meta;
  meta.topic_name = "sensor/timing";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(timeline.RegisterTopic(meta, &topic));

  auto stream = GenerateStream(topic, {"sensor/timing", 10000000ULL, 500000ULL,
                                       0.02, 64},
                               1000000000ULL, 46);  // 100Hz 跑 1 秒
  STRATUM_CHECK_OK(timeline.AppendBatch(stream.events));

  // 说明：这里直接验证时间轴读取的时序保真性，不构造 Player
  // （Player 需要 backend，且已在 test_record_replay 中覆盖）。

  EventIteratorPtr it;
  STRATUM_CHECK_OK(timeline.QueryRange(topic, TimeRange(), &it));

  // 时间间隔必须保持原始量级（抖动不能放大）
  std::vector<int64_t> intervals;
  TimestampNs prev = 0;
  for (; it->Valid(); it->Next()) {
    if (prev != 0) {
      intervals.push_back(static_cast<int64_t>(it->event().source_timestamp_ns) -
                          static_cast<int64_t>(prev));
    }
    prev = it->event().source_timestamp_ns;
  }

  STRATUM_CHECK_TRUE(intervals.size() > 50);
  // 名义周期 10ms，抖动 ±0.5ms → 间隔应在 [9ms, 11ms]
  int in_range = 0;
  for (int64_t iv : intervals) {
    if (iv >= 8000000 && iv <= 12000000) {
      ++in_range;
    }
  }
  const double ratio =
      static_cast<double>(in_range) / static_cast<double>(intervals.size());
  ctx.param_desc = "in-range=" + std::to_string(static_cast<int>(ratio * 100)) +
                   "%";
  STRATUM_CHECK_TRUE(ratio > 0.95);  // 95% 以上应落在合理范围
}

STRATUM_TEST_MAIN()
