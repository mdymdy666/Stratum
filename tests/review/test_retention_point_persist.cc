// P4 验证：保留点跨重启持久化
//
// 场景：录制中创建保留点 → 保留策略淘汰 → 关闭 → 重开
//       → 恢复的保留点必须继续保护"其之前的数据"不被淘汰。
//
// 修复前：保留点只在内存 → 重启后丢失 → RunRetention 把保留点
// 之前的数据全部删掉 → 时间点查询 NotFound。

#include <cstdint>
#include <string>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"

using namespace stratum;

namespace {
constexpr uint64_t kBaseTs = 1700000000000000000ULL;
}

STRATUM_TEST(P4, RetentionPointSurvivesRestartAndProtects) {
  const std::string dir = "/tmp/stratum_p4";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  constexpr int kCount = 3000;           // 3000 条跨 300 秒
  const TimestampNs kPointTs = kBaseTs + 150000000000ULL;  // 保留点：150 秒处

  // --- 第一阶段：写入 + 创建保留点 + 执行保留策略 ---
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));

    TopicMetadata meta;
    meta.topic_name = "p4/data";
    uint32_t topic = 0;
    STRATUM_CHECK_OK(tl.RegisterTopic(meta, &topic));

    std::vector<Event> events;
    events.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
      Event e;
      e.topic_id = topic;
      e.seq = static_cast<uint32_t>(i);
      e.source_timestamp_ns =
          kBaseTs + static_cast<uint64_t>(i) * 100000000ULL;  // 每 0.1 秒
      e.payload = "data-" + std::to_string(i);
      events.push_back(std::move(e));
    }
    STRATUM_CHECK_OK(tl.AppendBatch(events));

    // 保留点：150 秒处
    uint64_t point_id = 0;
    STRATUM_CHECK_OK(tl.CreateRetentionPoint(kPointTs, &point_id));
    STRATUM_CHECK_TRUE(point_id > 0);

    // 保留策略：只保留最近 100 秒 → 理论上删到 200 秒处，
    // 但保留点（150 秒）保护其之前的数据 → 实际只删到 150 秒处
    RetentionPolicy policy;
    policy.retain_duration_ns = 100000000000ULL;  // 100 秒
    STRATUM_CHECK_OK(tl.SetRetentionPolicy(policy));
    STRATUM_CHECK_OK(tl.RunRetention());

    // 验证：150 秒之前的数据还在（保留点保护）
    uint64_t before_point = 0;
    TimeRange r;
    r.begin_ns = kBaseTs;
    r.end_ns = kPointTs;
    STRATUM_CHECK_OK(tl.CountRange(topic, r, &before_point));
    ctx.param_desc = "第一阶段 保留点前条数=" + std::to_string(before_point);
    STRATUM_CHECK_TRUE(before_point > 0);

    STRATUM_CHECK_OK(tl.Close());
  }

  // --- 第二阶段：重开 → 恢复的保留点必须继续保护 ---
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));

    TopicMetadata found;
    STRATUM_CHECK_OK(tl.FindTopicByName("p4/data", &found));

    // 重开后立即执行保留策略（模拟后台调度）
    RetentionPolicy policy;
    policy.retain_duration_ns = 100000000000ULL;
    STRATUM_CHECK_OK(tl.SetRetentionPolicy(policy));
    STRATUM_CHECK_OK(tl.RunRetention());

    // 【核心断言】保留点之前的数据仍在（修复前会被删光）
    uint64_t before_point = 0;
    TimeRange r;
    r.begin_ns = kBaseTs;
    r.end_ns = kPointTs;
    STRATUM_CHECK_OK(tl.CountRange(found.topic_id, r, &before_point));
    ctx.param_desc += " 重开后再淘汰 保留点前条数=" +
                      std::to_string(before_point);
    STRATUM_CHECK_TRUE(before_point > 0);

    // 【核心断言 2】保留点时刻的定点查询能命中
    StateAtResult state;
    STRATUM_CHECK_OK(tl.GetStateAt(found.topic_id, kPointTs - 1, &state));
    ctx.param_desc += " 定点=" + std::to_string(static_cast<int>(state.kind));
    STRATUM_CHECK_EQ(static_cast<int>(state.kind),
                     static_cast<int>(StateAtResult::Kind::kFound));

    STRATUM_CHECK_OK(tl.Close());
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

STRATUM_TEST(P4, ReleaseRetentionPointPersists) {
  const std::string dir = "/tmp/stratum_p4b";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));

    uint64_t p1 = 0;
    uint64_t p2 = 0;
    STRATUM_CHECK_OK(tl.CreateRetentionPoint(kBaseTs + 1000, &p1));
    STRATUM_CHECK_OK(tl.CreateRetentionPoint(kBaseTs + 2000, &p2));
    // 释放 p1
    STRATUM_CHECK_OK(tl.ReleaseRetentionPoint(p1));
    STRATUM_CHECK_OK(tl.Close());
  }

  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));

    // 【核心断言】重开后 p1 已释放（NotFound），p2 仍存在
    const Status s1 = tl.ReleaseRetentionPoint(1);
    const Status s2 = tl.ReleaseRetentionPoint(2);
    ctx.param_desc = "p1(已释放)=" + s1.ToString() + " p2=" + s2.ToString();
    // 注意：p1 若在则 release 成功；修复后 p1 已在重启前释放，应 NotFound
    STRATUM_CHECK_TRUE(s1.IsNotFound());
    STRATUM_CHECK_OK(s2);

    STRATUM_CHECK_OK(tl.Close());
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

STRATUM_TEST_MAIN()
