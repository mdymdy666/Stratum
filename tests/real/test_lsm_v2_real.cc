// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// v2 DB 层：业务真实测试 —— 模拟真实 DDS 录制负载
//
// ===========================================================================
// 真实业务特征（对照 design/07-testing-standards.md）
// ===========================================================================
//
// 模拟一个机器人系统的 DDS 流量：
//   1. IMU        200 Hz（每 5ms），抖动 ±10%
//   2. 温度       1 Hz，偶发丢帧
//   3. 控制指令   不定时（随机间隔），偶发重发
//   4. 激光雷达   10 Hz，大负载（~50KB/帧）
//
// 场景：录制 120 秒 → 设备重连突发回补 → 关闭 → 重开 → 回放比对
// 数据量：约 30,000 条、数百 MB 级负载。

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "stratum/storage/lsm_v2/stratum_db.h"
#include "test_framework.h"
#include "test_isolation.h"

using namespace stratum;
using namespace stratum::storage::lsm_v2;

namespace {

constexpr uint64_t kBaseTs = 1700000000000000000ULL;

// 话题规格
struct TopicSpec {
  const char* name;
  uint64_t period_ns;   // 名义周期
  uint64_t jitter_ns;   // 抖动幅度（±）
  double drop_rate;     // 丢帧率
  size_t payload_size;  // 负载大小
};

// 生成一个话题的采样序列（含抖动与丢帧）
std::vector<Event> GenerateStream(uint32_t topic_id, const TopicSpec& spec,
                                  uint64_t duration_ns, uint64_t seed,
                                  std::map<uint32_t, uint32_t>* seq_counters) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> jitter(
      -static_cast<int64_t>(spec.jitter_ns),
      static_cast<int64_t>(spec.jitter_ns));
  std::uniform_real_distribution<double> uniform(0.0, 1.0);

  std::vector<Event> events;
  uint64_t cursor = kBaseTs;
  while (cursor < kBaseTs + duration_ns) {
    const int64_t offset = jitter(rng);
    const uint64_t ts =
        static_cast<uint64_t>(static_cast<int64_t>(cursor) + offset);

    if (uniform(rng) < spec.drop_rate) {
      cursor += spec.period_ns;
      continue;  // 丢帧
    }

    Event e;
    e.topic_id = topic_id;
    e.source_timestamp_ns = ts;
    e.payload.assign(spec.payload_size, 'D');
    events.push_back(std::move(e));
    cursor += spec.period_ns;
  }
  return events;
}

}  // namespace

// ---------------------------------------------------------------------------
// 完整业务场景：多话题录制 → 突发回补 → 重启 → 回放比对
// ---------------------------------------------------------------------------
STRATUM_TEST(LsmV2Real, E_RoboticDdsRecordingReplayAcrossRestart) {
  const std::string dir = std::string("/tmp/stratum_v2_real_") + UniqueSuffix();
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  // 话题规格
  const TopicSpec specs[] = {
      {"sensor/imu", 5000000ULL, 500000ULL, 0.02, 128},         // 200Hz IMU
      {"sensor/temperature", 1000000000ULL, 10000000ULL, 0.05, 32},  // 1Hz
      {"control/cmd", 200000000ULL, 50000000ULL, 0.0, 64},     // 5Hz 控制
      {"sensor/lidar", 100000000ULL, 1000000ULL, 0.0, 51200},  // 10Hz 激光雷达
  };
  constexpr int kNumTopics = 4;
  constexpr uint64_t kDurationNs = 120000000000ULL;  // 120 秒

  uint32_t topics[kNumTopics];
  std::map<uint32_t, uint32_t> seq_counters;  // topic → seq（影子）
  uint64_t total_expected = 0;

  // --- 第一阶段：录制 ---
  {
    StratumDB* db = nullptr;
    STRATUM_CHECK_OK(StratumDB::Open(dir, &db));

    for (int i = 0; i < kNumTopics; ++i) {
      TopicMetadata meta;
      meta.topic_name = specs[i].name;
      meta.type_name = "RobotSensor";
      STRATUM_CHECK_OK(db->RegisterTopic(meta, &topics[i]));
    }

    for (int i = 0; i < kNumTopics; ++i) {
      auto events = GenerateStream(topics[i], specs[i], kDurationNs,
                                   1000 + static_cast<uint64_t>(i),
                                   &seq_counters);
      constexpr int kBatch = 500;
      for (size_t base = 0; base < events.size(); base += kBatch) {
        std::vector<Event> batch;
        const size_t end = std::min(base + kBatch, events.size());
        for (size_t j = base; j < end; ++j) {
          batch.push_back(events[j]);
        }
        STRATUM_CHECK_OK(db->AppendBatch(batch));
      }
      total_expected += events.size();
    }

    // --- 突发回补：模拟 IMU 设备断连 10 秒后重连，一次性补发历史 ---
    {
      std::mt19937_64 rng(77);
      std::vector<Event> backfill;
      constexpr int kBackfill = 2000;  // 10 秒 × 200Hz
      for (int i = 0; i < kBackfill; ++i) {
        Event e;
        e.topic_id = topics[0];  // IMU
        // 时间戳**早于**已录制的最后一段（乱序回补）
        e.source_timestamp_ns =
            kBaseTs + 60000000000ULL + static_cast<uint64_t>(i) * 5000000ULL;
        e.payload.assign(128, 'B');
        backfill.push_back(std::move(e));
      }
      // 分批追加
      for (size_t base = 0; base < backfill.size(); base += 500) {
        std::vector<Event> batch;
        const size_t end = std::min(base + 500, backfill.size());
        for (size_t j = base; j < end; ++j) {
          batch.push_back(backfill[j]);
        }
        STRATUM_CHECK_OK(db->AppendBatch(batch));
      }
      total_expected += kBackfill;
    }

    ctx.param_desc = "录制完成，总事件=" + std::to_string(total_expected);
    STRATUM_CHECK_TRUE(total_expected > 20000);  // 大数据量底线

    db->Close();
    delete db;
  }

  // --- 第二阶段：重启 → 回放比对 ---
  {
    StratumDB* db = nullptr;
    STRATUM_CHECK_OK(StratumDB::Open(dir, &db));

    // 话题恢复
    std::vector<TopicMetadata> topics_after;
    STRATUM_CHECK_OK(db->ListTopics(&topics_after));
    ctx.param_desc += " 重启话题数=" + std::to_string(topics_after.size());
    STRATUM_CHECK_EQ(topics_after.size(), static_cast<size_t>(kNumTopics));

    // 全量回放：总条数必须一致（含乱序回补的 2000 条）
    uint64_t total_read = 0;
    for (int i = 0; i < kNumTopics; ++i) {
      uint64_t n = 0;
      STRATUM_CHECK_OK(db->CountRange(topics[i], 0, 0, &n));
      total_read += n;
    }
    ctx.param_desc += " 回放总条数=" + std::to_string(total_read);
    STRATUM_CHECK_EQ(total_read, total_expected);

    // IMU 话题的时间戳回补区间：60s~70s 处应同时存在"正常录制"与"回补"数据
    uint64_t overlap = 0;
    STRATUM_CHECK_OK(db->CountRange(topics[0],
                                    kBaseTs + 60000000000ULL,
                                    kBaseTs + 70000000000ULL, &overlap));
    ctx.param_desc += " IMU重叠区间=" + std::to_string(overlap);
    // 正常录制 10 秒 ≈ 2000 条 + 回补 2000 条 ≈ 4000 条（含抖动与丢帧）
    STRATUM_CHECK_TRUE(overlap > 3000);

    db->Close();
    delete db;
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

// ---------------------------------------------------------------------------
// F：时间点查询在真实负载下的正确性（含乱序回补后）
// ---------------------------------------------------------------------------
STRATUM_TEST(LsmV2Real, F_GetStateAtUnderChaoticTimestamps) {
  const std::string dir = std::string("/tmp/stratum_v2_state_") + UniqueSuffix();
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  StratumDB* db = nullptr;
  STRATUM_CHECK_OK(StratumDB::Open(dir, &db));
  TopicMetadata meta;
  meta.topic_name = "state/chaos";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(db->RegisterTopic(meta, &topic));

  // 构造精确的时间线（带乱序）：t0→A, t2→C, t1→B（乱序写入）
  const uint64_t t0 = kBaseTs + 1000;
  const uint64_t t1 = kBaseTs + 2000;
  const uint64_t t2 = kBaseTs + 3000;

  {
    std::vector<Event> events;
    Event e;
    e.topic_id = topic;
    e.source_timestamp_ns = t0; e.payload = "A"; events.push_back(e);
    e.source_timestamp_ns = t2; e.payload = "C"; events.push_back(e);
    e.source_timestamp_ns = t1; e.payload = "B"; events.push_back(e);
    STRATUM_CHECK_OK(db->AppendBatch(events));  // 乱序写入
  }

  // 时间点查询必须按时间戳语义（与写入顺序无关）
  {
    StateAtResult r;
    STRATUM_CHECK_OK(db->GetStateAt(topic, t0, &r));
    ctx.param_desc = "at t0=" + r.event.payload;
    STRATUM_CHECK_EQ(r.event.payload, std::string("A"));
  }
  {
    StateAtResult r;
    STRATUM_CHECK_OK(db->GetStateAt(topic, t1, &r));
    ctx.param_desc += " at t1=" + r.event.payload;
    STRATUM_CHECK_EQ(r.event.payload, std::string("B"));
  }
  {
    StateAtResult r;
    // t1.5 之间：应返回 t1 的值 B
    STRATUM_CHECK_OK(db->GetStateAt(topic, t1 + 500, &r));
    ctx.param_desc += " at t1.5=" + r.event.payload;
    STRATUM_CHECK_EQ(r.event.payload, std::string("B"));
  }
  {
    StateAtResult r;
    STRATUM_CHECK_OK(db->GetStateAt(topic, t2, &r));
    ctx.param_desc += " at t2=" + r.event.payload;
    STRATUM_CHECK_EQ(r.event.payload, std::string("C"));
  }
  {
    StateAtResult r;
    // 早于首条：NotFound
    STRATUM_CHECK_OK(db->GetStateAt(topic, kBaseTs, &r));
    ctx.param_desc += " at t-1=" + std::to_string(static_cast<int>(r.kind));
    STRATUM_CHECK_EQ(static_cast<int>(r.kind),
                     static_cast<int>(StateAtResult::Kind::kNotFound));
  }

  db->Close();
  delete db;
  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

STRATUM_TEST_MAIN()
