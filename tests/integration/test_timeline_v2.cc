// TimelineImplV2 测试：ITimeline 接口对齐验证
//
// 目标：上层（Recorder/Player/Query）零修改即可使用 v2。
// 本测试覆盖 ITimeline 契约的核心路径（对标 test_timeline_basic），
// 并按 07 标准使用大数据量。

#include <cstdint>
#include <string>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"

using namespace stratum;

namespace {
constexpr uint64_t kBaseTs = 1700000000000000000ULL;
}

STRATUM_TEST(TimelineV2, RegisterAppendQuery) {
  const std::string dir = "/tmp/stratum_tl_v2";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  TopicMetadata meta;
  meta.topic_name = "sensor/temp";
  uint32_t id1 = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(meta, &id1));
  STRATUM_CHECK_TRUE(id1 > 0);

  // 幂等
  uint32_t id2 = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(meta, &id2));
  STRATUM_CHECK_EQ(id1, id2);

  // 批量追加 1 万条
  constexpr int kCount = 10000;
  constexpr int kBatch = 1000;
  for (int base = 0; base < kCount; base += kBatch) {
    std::vector<Event> events;
    events.reserve(kBatch);
    for (int i = 0; i < kBatch; ++i) {
      Event e;
      e.topic_id = id1;
      e.seq = static_cast<uint32_t>(base + i);
      e.source_timestamp_ns =
          kBaseTs + static_cast<uint64_t>(base + i) * 1000;
      e.reception_timestamp_ns = e.source_timestamp_ns + 500;
      e.payload = "p-" + std::to_string(base + i);
      events.push_back(std::move(e));
    }
    STRATUM_CHECK_OK(tl.AppendBatch(events));
  }

  // 范围查询（迭代器接口）
  EventIteratorPtr it;
  TimeRange all;
  all.begin_ns = kBaseTs;
  all.end_ns = kBaseTs + kCount * 1000ULL;
  STRATUM_CHECK_OK(tl.QueryRange(id1, all, &it));
  int n = 0;
  std::string last;
  for (; it->Valid(); it->Next()) {
    last = it->event().payload;
    ++n;
  }
  STRATUM_CHECK_OK(it->status());
  ctx.param_desc = "count=" + std::to_string(n) + " last=" + last;
  STRATUM_CHECK_EQ(n, kCount);
  STRATUM_CHECK_EQ(last, std::string("p-9999"));

  // 双时间戳保真（B-C2 修复验证）：重新查询定位
  EventIteratorPtr it2;
  TimeRange one;
  one.begin_ns = kBaseTs + 5000 * 1000;
  one.end_ns = kBaseTs + 5001 * 1000;
  STRATUM_CHECK_OK(tl.QueryRange(id1, one, &it2));
  STRATUM_CHECK_TRUE(it2->Valid());
  ctx.param_desc += " reception=" +
                    std::to_string(it2->event().reception_timestamp_ns) +
                    " source=" + std::to_string(it2->event().source_timestamp_ns);
  STRATUM_CHECK_EQ(it2->event().reception_timestamp_ns,
                   it2->event().source_timestamp_ns + 500);

  // 定点查询
  StateAtResult r;
  STRATUM_CHECK_OK(tl.GetStateAt(id1, kBaseTs + 5000 * 1000, &r));
  STRATUM_CHECK_EQ(static_cast<int>(r.kind),
                   static_cast<int>(StateAtResult::Kind::kFound));
  STRATUM_CHECK_EQ(r.event.payload, std::string("p-5000"));

  // 早于首条：OutOfRange
  STRATUM_CHECK_OK(tl.GetStateAt(id1, kBaseTs - 1, &r));
  ctx.param_desc += " early=" + std::to_string(static_cast<int>(r.kind));
  STRATUM_CHECK_EQ(static_cast<int>(r.kind),
                   static_cast<int>(StateAtResult::Kind::kOutOfRange));

  STRATUM_CHECK_OK(tl.Close());
  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

STRATUM_TEST(TimelineV2, SameTimestampAndQualityStats) {
  const std::string dir = "/tmp/stratum_tl_v2b";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  TopicMetadata meta;
  meta.topic_name = "same/ts";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(meta, &topic));

  // 同时间戳 50 条
  std::vector<Event> events;
  for (int i = 0; i < 50; ++i) {
    Event e;
    e.topic_id = topic;
    e.seq = static_cast<uint32_t>(i);
    e.source_timestamp_ns = kBaseTs;
    e.payload = "same-" + std::to_string(i);
    events.push_back(std::move(e));
  }
  STRATUM_CHECK_OK(tl.AppendBatch(events));

  uint64_t n = 0;
  TimeRange ts_range;
  ts_range.begin_ns = kBaseTs;
  ts_range.end_ns = kBaseTs + 1;
  STRATUM_CHECK_OK(tl.CountRange(topic, ts_range, &n));
  ctx.param_desc = "same_ts=" + std::to_string(n);
  STRATUM_CHECK_EQ(n, static_cast<uint64_t>(50));

  // 质量统计：时间戳倒退
  Event back;
  back.topic_id = topic;
  back.seq = 100;
  back.source_timestamp_ns = kBaseTs - 100;  // 倒退
  back.payload = "back";
  STRATUM_CHECK_OK(tl.Append(back));

  const QualityStats q = tl.GetQualityStats();
  ctx.param_desc += " regressions=" + std::to_string(q.timestamp_regressions);
  STRATUM_CHECK_EQ(q.timestamp_regressions, static_cast<uint64_t>(1));

  STRATUM_CHECK_OK(tl.Close());
  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

STRATUM_TEST(TimelineV2, RestartRestoresTopicsAndStats) {
  const std::string dir = "/tmp/stratum_tl_v2c";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    TopicMetadata meta;
    meta.topic_name = "persist/x";
    uint32_t topic = 0;
    STRATUM_CHECK_OK(tl.RegisterTopic(meta, &topic));

    std::vector<Event> events;
    for (int i = 0; i < 2000; ++i) {
      Event e;
      e.topic_id = topic;
      e.seq = static_cast<uint32_t>(i);
      e.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(i) * 1000000ULL;
      e.payload.assign(32, 'z');
      events.push_back(std::move(e));
    }
    STRATUM_CHECK_OK(tl.AppendBatch(events));
    STRATUM_CHECK_OK(tl.Close());
  }

  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));

    std::vector<TopicMetadata> topics;
    STRATUM_CHECK_OK(tl.ListTopics(&topics));
    STRATUM_CHECK_EQ(topics.size(), static_cast<size_t>(1));
    ctx.param_desc = "topics=" + std::to_string(topics.size()) +
                     " count=" + std::to_string(topics[0].message_count);
    // 统计在 Close 时持久化，重开恢复
    STRATUM_CHECK_EQ(topics[0].message_count, static_cast<uint64_t>(2000));

    TopicMetadata found;
    STRATUM_CHECK_OK(tl.FindTopicByName("persist/x", &found));
    STRATUM_CHECK_EQ(found.topic_id, topics[0].topic_id);

    STRATUM_CHECK_OK(tl.Close());
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

STRATUM_TEST(TimelineV2, RetentionPointAndPolicy) {
  const std::string dir = "/tmp/stratum_tl_v2d";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  TopicMetadata meta;
  meta.topic_name = "retention/t";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(meta, &topic));

  // 1000 条跨 100 秒
  std::vector<Event> events;
  for (int i = 0; i < 1000; ++i) {
    Event e;
    e.topic_id = topic;
    e.seq = static_cast<uint32_t>(i);
    e.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(i) * 100000000ULL;
    e.payload.assign(64, 'r');
    events.push_back(std::move(e));
  }
  STRATUM_CHECK_OK(tl.AppendBatch(events));

  // 保留点
  uint64_t point_id = 0;
  STRATUM_CHECK_OK(tl.CreateRetentionPoint(kBaseTs + 50000000000ULL, &point_id));
  STRATUM_CHECK_TRUE(point_id > 0);

  // 保留策略：只保留最近 10 秒 → 应删到第 900 条（90 秒处）
  RetentionPolicy policy;
  policy.retain_duration_ns = 10000000000ULL;
  STRATUM_CHECK_OK(tl.SetRetentionPolicy(policy));
  const Status rs = tl.RunRetention();

  uint64_t after = 0;
  TimeRange all;
  STRATUM_CHECK_OK(tl.CountRange(topic, all, &after));
  ctx.param_desc = "retention=" + rs.ToString() + " after=" +
                   std::to_string(after);
  STRATUM_CHECK_TRUE(after < 1000);

  STRATUM_CHECK_OK(tl.ReleaseRetentionPoint(point_id));
  STRATUM_CHECK_TRUE(tl.ReleaseRetentionPoint(point_id).IsNotFound());

  STRATUM_CHECK_OK(tl.Close());
  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

STRATUM_TEST_MAIN()
