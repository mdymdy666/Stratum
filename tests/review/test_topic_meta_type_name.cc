// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 回归测试：话题 type_name 必须跨"关闭-重开"存活
//
// ===========================================================================
// 复现的真实缺陷
// ===========================================================================
// 症状：用 tools/stratum_rtps_replay 把录制好的 hello_world_topic（32 条）
//       回放到真实 Fast DDS 订阅端，对端**一条都收不到**，
//       而且两端都没有任何报错（Fast DDS 侧连一条日志都没有）。
//
// 根因链（每一跳都有实测证据，见 design/11-rtps-interop-handoff.md 与
// AI_COLLAB.md 续 16）：
//   1. StratumDB::ListTopics 只回填 topic_name/topic_id，
//      丢掉了元数据值里**已经存好的** type_name；
//   2. TimelineImplV2::LoadTopicsLocked → topic_meta_ 的 type_name 为空；
//   3. Player 用 t.type_name 组装 ReceivedSample → 空字符串；
//   4. RtpsWriter::Publish(topic, "") → PID_TYPE_NAME 序列化失败
//      （ParameterString_t 拒绝空串）→ BuildPublication 返回空 vector
//      → SEDP Publication **静默不发**；
//   5. Fast DDS 从未发现我方 Writer → 不回 SEDP Subscription
//      → 我方学不到对端 user locator → 用户数据全被发到 metatraffic 端口
//      → 对端 DataReader 收到 0 条。
//
// 本测试锁死第 1~3 跳（存储层 + Player 回放），不依赖任何网络；
// 第 4 跳（协议层不再静默失败）由 tests/rtps/test_publication_guard.cc 锁死。

#include <string>
#include <vector>

#include "stratum/core/recorder.h"        // Recorder / Player 都在此头文件
#include "stratum/core/timeline_impl_v2.h"
#include "mock_backend.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::testing;

namespace {

constexpr TimestampNs kBaseTs = 1700000000000000000ULL;

// 录制三条样本后关闭时间线，返回目录（模拟"录制进程退出"）。
// 关键点：**必须在新的 TimelineImplV2 实例上验证**，
// 因为只有 Open 时的 LoadTopicsLocked 才会走 StratumDB::ListTopics ——
// 这正是缺陷所在的那条路径。
void RecordAndClose(const std::string& dir) {
  TimelineImplV2 timeline;
  STRATUM_CHECK_OK(timeline.Open(dir));

  TopicMetadata meta;
  meta.topic_name = "hello_world_topic";
  meta.type_name = "HelloWorld";
  uint32_t topic_id = 0;
  STRATUM_CHECK_OK(timeline.RegisterTopic(meta, &topic_id));

  std::vector<Event> events;
  for (int i = 0; i < 3; ++i) {
    Event e;
    e.topic_id = topic_id;
    e.seq = static_cast<uint32_t>(i);
    e.source_timestamp_ns = kBaseTs + static_cast<TimestampNs>(i) * 1000000ULL;
    e.reception_timestamp_ns = e.source_timestamp_ns + 1000;
    e.payload = std::string("Hello world #") + std::to_string(i);
    events.push_back(std::move(e));
  }
  STRATUM_CHECK_OK(timeline.AppendBatch(events));
  STRATUM_CHECK_OK(timeline.Close());
}

}  // namespace

// ---------------------------------------------------------------------------
// 元数据往返：重开后 type_name 必须还在
// ---------------------------------------------------------------------------
STRATUM_TEST(TopicMetaV2, TypeNameSurvivesReopen) {
  const std::string dir = "/tmp/stratum_topicmeta_type_name";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  RecordAndClose(dir);

  TimelineImplV2 reopened;
  STRATUM_CHECK_OK(reopened.Open(dir));

  std::vector<TopicMetadata> topics;
  STRATUM_CHECK_OK(reopened.ListTopics(&topics));
  STRATUM_CHECK_EQ(topics.size(), static_cast<size_t>(1));
  ctx.param_desc = "topic=" + topics[0].topic_name +
                   " type=" + topics[0].type_name;
  STRATUM_CHECK_EQ(topics[0].topic_name, std::string("hello_world_topic"));
  // 【缺陷断言】修复前这里是空字符串。
  STRATUM_CHECK_EQ(topics[0].type_name, std::string("HelloWorld"));

  // 按名字查找走的是同一条元数据路径，必须一致
  TopicMetadata by_name;
  STRATUM_CHECK_OK(reopened.FindTopicByName("hello_world_topic", &by_name));
  STRATUM_CHECK_EQ(by_name.type_name, std::string("HelloWorld"));

  // 按 id 取元数据同样必须带 type_name
  TopicMetadata by_id;
  STRATUM_CHECK_OK(reopened.GetTopicMetadata(topics[0].topic_id, &by_id));
  STRATUM_CHECK_EQ(by_id.type_name, std::string("HelloWorld"));

  STRATUM_CHECK_OK(reopened.Close());
}

// ---------------------------------------------------------------------------
// 回放链路：Player 交给 Sink 的样本必须带 type_name
// （RtpsWriter/BuildPublication 的输入就是它）
// ---------------------------------------------------------------------------
STRATUM_TEST(TopicMetaV2, PlayerPropagatesTypeNameAfterReopen) {
  const std::string dir = "/tmp/stratum_topicmeta_type_name_play";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  RecordAndClose(dir);

  TimelineImplV2 reopened;
  STRATUM_CHECK_OK(reopened.Open(dir));

  MockBackend backend;
  Player player(&reopened, &backend);

  std::vector<protocol::ReceivedSample> published;
  PlayerConfig pcfg;
  pcfg.rate = 0;                            // 尽快回放
  pcfg.allow_original_topic_names = true;   // 回放要用原始话题名
  pcfg.publish_callback = [&published](const protocol::ReceivedSample& s) {
    published.push_back(s);
  };

  STRATUM_CHECK_OK(player.Play(TimeRange(), pcfg));
  STRATUM_CHECK_EQ(published.size(), static_cast<size_t>(3));

  for (const auto& s : published) {
    ctx.param_desc = "topic=" + s.topic_name + " type=" + s.type_name;
    STRATUM_CHECK_EQ(s.topic_name, std::string("hello_world_topic"));
    // 【缺陷断言】修复前这里是空字符串 → 后续 PID_TYPE_NAME 序列化失败。
    STRATUM_CHECK_EQ(s.type_name, std::string("HelloWorld"));
    STRATUM_CHECK_TRUE(!s.payload.empty());
  }

  STRATUM_CHECK_OK(reopened.Close());
}

STRATUM_TEST_MAIN()
