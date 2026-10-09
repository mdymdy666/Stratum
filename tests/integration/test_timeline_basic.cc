// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// Timeline 层集成测试
//
// 验证"事件语义 → KV 语义"的映射是否正确：
//   - 话题注册与查询
//   - 批量追加与范围读取
//   - 定点查询（GetStateAt）的四种语义
//   - 质量统计（时间戳倒退、序号缺口）
//   - 保留点与保留策略

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "stratum/core/timeline_impl.h"
#include "stratum/core/types.h"
#include "stratum/storage/istorage_engine.h"
#include "stratum/storage/memory/memory_engine.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::storage;

namespace {

// 测试辅助：创建一个基于内存引擎的 TimelineImpl
std::unique_ptr<TimelineImpl> MakeTimeline() {
  auto engine = StorageEnginePtr(new MemoryEngine());
  auto timeline = std::unique_ptr<TimelineImpl>(new TimelineImpl(std::move(engine)));
  return timeline;
}

constexpr uint64_t kBaseTs = 1700000000000000000ULL;

}  // namespace

// ---------------------------------------------------------------------------
// 话题注册
// ---------------------------------------------------------------------------
STRATUM_TEST(Timeline, RegisterTopicAssignsStableIds) {
  auto tl = MakeTimeline();
  STRATUM_CHECK_OK(tl->Open("mem://test"));

  TopicMetadata meta;
  meta.topic_name = "sensor/temp";
  meta.type_name = "Temperature";

  uint32_t id1 = 0;
  STRATUM_CHECK_OK(tl->RegisterTopic(meta, &id1));
  STRATUM_CHECK_TRUE(id1 > 0);

  // 幂等：同名话题必须返回同一个 id
  uint32_t id2 = 0;
  STRATUM_CHECK_OK(tl->RegisterTopic(meta, &id2));
  STRATUM_CHECK_EQ(id1, id2);

  // 不同话题得到不同 id
  TopicMetadata meta2;
  meta2.topic_name = "sensor/humidity";
  uint32_t id3 = 0;
  STRATUM_CHECK_OK(tl->RegisterTopic(meta2, &id3));
  STRATUM_CHECK_NE(id1, id3);

  // 按名字查找
  TopicMetadata found;
  STRATUM_CHECK_OK(tl->FindTopicByName("sensor/temp", &found));
  STRATUM_CHECK_EQ(found.topic_id, id1);
  STRATUM_CHECK_EQ(found.type_name, std::string("Temperature"));

  // 不存在的名字
  STRATUM_CHECK_TRUE(tl->FindTopicByName("no/such", &found).IsNotFound());
}

STRATUM_TEST(Timeline, ListTopicsIsSortedById) {
  auto tl = MakeTimeline();
  STRATUM_CHECK_OK(tl->Open("mem://test"));

  std::vector<std::string> names = {"c/topic", "a/topic", "b/topic"};
  for (const auto& n : names) {
    TopicMetadata meta;
    meta.topic_name = n;
    STRATUM_CHECK_OK(tl->RegisterTopic(meta, nullptr));
  }

  std::vector<TopicMetadata> topics;
  STRATUM_CHECK_OK(tl->ListTopics(&topics));
  STRATUM_CHECK_EQ(topics.size(), static_cast<size_t>(3));

  // 必须按 topic_id 升序（稳定输出）
  for (size_t i = 1; i < topics.size(); ++i) {
    STRATUM_CHECK_LT(topics[i - 1].topic_id, topics[i].topic_id);
  }
}

// ---------------------------------------------------------------------------
// 追加与范围查询
// ---------------------------------------------------------------------------
STRATUM_TEST(Timeline, AppendAndQueryRange) {
  auto tl = MakeTimeline();
  STRATUM_CHECK_OK(tl->Open("mem://test"));

  TopicMetadata meta;
  meta.topic_name = "t";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(tl->RegisterTopic(meta, &topic));

  // 追加 100 条，时间戳递增
  constexpr int kCount = 100;
  std::vector<Event> events;
  events.reserve(kCount);
  for (int i = 0; i < kCount; ++i) {
    Event e;
    e.topic_id = topic;
    e.seq = static_cast<uint32_t>(i);
    e.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(i) * 1000;
    e.reception_timestamp_ns = e.source_timestamp_ns + 500;
    e.payload = "payload-" + std::to_string(i);
    events.push_back(e);
  }
  STRATUM_CHECK_OK(tl->AppendBatch(events));

  // 全范围读取
  EventIteratorPtr it;
  TimeRange all;
  STRATUM_CHECK_OK(tl->QueryRange(topic, all, &it));
  int read = 0;
  std::string last_payload;
  for (; it->Valid(); it->Next()) {
    // 必须按时间戳升序返回
    last_payload = it->event().payload;
    ++read;
  }
  STRATUM_CHECK_OK(it->status());
  STRATUM_CHECK_EQ(read, kCount);
  STRATUM_CHECK_EQ(last_payload, std::string("payload-99"));

  // 子范围：只取前 10 条
  EventIteratorPtr it2;
  TimeRange sub;
  sub.begin_ns = kBaseTs;
  sub.end_ns = kBaseTs + 10 * 1000;  // 半开区间，恰好 10 条
  STRATUM_CHECK_OK(tl->QueryRange(topic, sub, &it2));
  int sub_count = 0;
  for (; it2->Valid(); it2->Next()) {
    ++sub_count;
  }
  STRATUM_CHECK_EQ(sub_count, 10);
}

STRATUM_TEST(Timeline, SameTimestampDoesNotOverwrite) {
  auto tl = MakeTimeline();
  STRATUM_CHECK_OK(tl->Open("mem://test"));

  TopicMetadata meta;
  meta.topic_name = "t";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(tl->RegisterTopic(meta, &topic));

  // 同一个时间戳写 50 条
  constexpr int kCount = 50;
  std::vector<Event> events;
  for (int i = 0; i < kCount; ++i) {
    Event e;
    e.topic_id = topic;
    e.seq = static_cast<uint32_t>(i);
    e.source_timestamp_ns = kBaseTs;  // 完全相同
    e.payload = "same-ts-" + std::to_string(i);
    events.push_back(e);
  }
  STRATUM_CHECK_OK(tl->AppendBatch(events));

  EventIteratorPtr it;
  TimeRange all;
  STRATUM_CHECK_OK(tl->QueryRange(topic, all, &it));
  int read = 0;
  for (; it->Valid(); it->Next()) {
    ++read;
  }
  // 关键：一条都不能少
  STRATUM_CHECK_EQ(read, kCount);
}

STRATUM_TEST(Timeline, CountRangeMatchesIteration) {
  auto tl = MakeTimeline();
  STRATUM_CHECK_OK(tl->Open("mem://test"));

  TopicMetadata meta;
  meta.topic_name = "t";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(tl->RegisterTopic(meta, &topic));

  for (int i = 0; i < 37; ++i) {
    Event e;
    e.topic_id = topic;
    e.seq = static_cast<uint32_t>(i);
    e.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(i);
    e.payload = "x";
    STRATUM_CHECK_OK(tl->Append(e));
  }

  uint64_t count = 0;
  TimeRange all;
  STRATUM_CHECK_OK(tl->CountRange(topic, all, &count));
  STRATUM_CHECK_EQ(count, static_cast<uint64_t>(37));
}

// ---------------------------------------------------------------------------
// 定点查询
// ---------------------------------------------------------------------------
STRATUM_TEST(Timeline, GetStateAtReturnsLatestBeforeTime) {
  auto tl = MakeTimeline();
  STRATUM_CHECK_OK(tl->Open("mem://test"));

  TopicMetadata meta;
  meta.topic_name = "t";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(tl->RegisterTopic(meta, &topic));

  // t0 -> "v0", t1 -> "v1", t2 -> "v2"
  for (int i = 0; i < 3; ++i) {
    Event e;
    e.topic_id = topic;
    e.seq = static_cast<uint32_t>(i);
    e.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(i) * 100;
    e.payload = "v" + std::to_string(i);
    STRATUM_CHECK_OK(tl->Append(e));
  }

  // 恰好在 t1 时刻：应返回 v1
  StateAtResult r;
  STRATUM_CHECK_OK(tl->GetStateAt(topic, kBaseTs + 100, &r));
  STRATUM_CHECK_EQ(static_cast<int>(r.kind), static_cast<int>(StateAtResult::Kind::kFound));
  STRATUM_CHECK_EQ(r.event.payload, std::string("v1"));

  // t1 与 t2 之间：仍应返回 v1
  STRATUM_CHECK_OK(tl->GetStateAt(topic, kBaseTs + 150, &r));
  STRATUM_CHECK_EQ(r.event.payload, std::string("v1"));

  // 晚于最后一条：返回 v2
  STRATUM_CHECK_OK(tl->GetStateAt(topic, kBaseTs + 10000, &r));
  STRATUM_CHECK_EQ(r.event.payload, std::string("v2"));

  // 早于第一条：必须报 out_of_range，而不是"猜"一个值
  STRATUM_CHECK_OK(tl->GetStateAt(topic, kBaseTs - 1, &r));
  STRATUM_CHECK_EQ(static_cast<int>(r.kind),
                   static_cast<int>(StateAtResult::Kind::kOutOfRange));

  // 不存在的话题：not found
  STRATUM_CHECK_OK(tl->GetStateAt(9999, kBaseTs, &r));
  STRATUM_CHECK_EQ(static_cast<int>(r.kind),
                   static_cast<int>(StateAtResult::Kind::kNotFound));
}

// ---------------------------------------------------------------------------
// 质量统计
// ---------------------------------------------------------------------------
STRATUM_TEST(Timeline, QualityStatsDetectTimestampRegression) {
  auto tl = MakeTimeline();
  STRATUM_CHECK_OK(tl->Open("mem://test"));

  TopicMetadata meta;
  meta.topic_name = "t";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(tl->RegisterTopic(meta, &topic));

  // 正常递增，然后倒退
  for (uint64_t ts : {kBaseTs, kBaseTs + 100, kBaseTs + 50, kBaseTs + 200}) {
    Event e;
    e.topic_id = topic;
    e.source_timestamp_ns = ts;
    e.payload = "x";
    STRATUM_CHECK_OK(tl->Append(e));
  }

  const QualityStats q = tl->GetQualityStats();
  STRATUM_CHECK_EQ(q.timestamp_regressions, static_cast<uint64_t>(1));
  STRATUM_CHECK_TRUE(q.HasIntegrityIssue());
}

STRATUM_TEST(Timeline, QualityStatsDetectSequenceGap) {
  auto tl = MakeTimeline();
  STRATUM_CHECK_OK(tl->Open("mem://test"));

  TopicMetadata meta;
  meta.topic_name = "t";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(tl->RegisterTopic(meta, &topic));

  // seq: 0, 1, 5（缺 2,3,4）, 6
  for (uint32_t seq : {0u, 1u, 5u, 6u}) {
    Event e;
    e.topic_id = topic;
    e.seq = seq;
    e.source_timestamp_ns = kBaseTs + seq;
    e.payload = "x";
    STRATUM_CHECK_OK(tl->Append(e));
  }

  const QualityStats q = tl->GetQualityStats();
  STRATUM_CHECK_EQ(q.sequence_gaps, static_cast<uint64_t>(1));
  STRATUM_CHECK_EQ(q.missing_messages, static_cast<uint64_t>(3));
}

STRATUM_TEST(Timeline, QualityStatsTrackLatency) {
  auto tl = MakeTimeline();
  STRATUM_CHECK_OK(tl->Open("mem://test"));

  TopicMetadata meta;
  meta.topic_name = "t";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(tl->RegisterTopic(meta, &topic));

  for (int i = 0; i < 10; ++i) {
    Event e;
    e.topic_id = topic;
    e.seq = static_cast<uint32_t>(i);
    e.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(i) * 1000;
    e.reception_timestamp_ns = e.source_timestamp_ns + 200;  // 固定 200ns 延迟
    e.payload = "x";
    STRATUM_CHECK_OK(tl->Append(e));
  }

  const QualityStats q = tl->GetQualityStats();
  STRATUM_CHECK_EQ(q.latency_samples, static_cast<uint64_t>(10));
  STRATUM_CHECK_EQ(q.min_latency_ns, static_cast<int64_t>(200));
  STRATUM_CHECK_EQ(q.max_latency_ns, static_cast<int64_t>(200));
  STRATUM_CHECK_EQ(q.AverageLatencyNs(), 200.0);
}

// ---------------------------------------------------------------------------
// 保留点
// ---------------------------------------------------------------------------
STRATUM_TEST(Timeline, RetentionPointLifecycle) {
  auto tl = MakeTimeline();
  STRATUM_CHECK_OK(tl->Open("mem://test"));

  TopicMetadata meta;
  meta.topic_name = "t";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(tl->RegisterTopic(meta, &topic));

  STRATUM_CHECK_EQ(tl->RetentionPointCount(), static_cast<size_t>(0));

  uint64_t point = 0;
  STRATUM_CHECK_OK(tl->CreateRetentionPoint(kBaseTs, &point));
  STRATUM_CHECK_TRUE(point > 0);
  STRATUM_CHECK_EQ(tl->RetentionPointCount(), static_cast<size_t>(1));

  // 重复释放必须报错
  STRATUM_CHECK_OK(tl->ReleaseRetentionPoint(point));
  STRATUM_CHECK_EQ(tl->RetentionPointCount(), static_cast<size_t>(0));
  STRATUM_CHECK_TRUE(tl->ReleaseRetentionPoint(point).IsNotFound());
}

STRATUM_TEST_MAIN()
