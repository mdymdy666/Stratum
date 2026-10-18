// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 回归测试：后端配置（domain 等）必须从上层配置传到 DDS 后端
//
// ===========================================================================
// 复现的真实缺陷
// ===========================================================================
// Recorder::Start 与 Player::Play 曾经各自构造一个**默认的**
// protocol::BackendConfig 交给 backend->CreateSource/CreateSink，
// 于是调用方设置的 domain_id 被静默丢弃：
//
//   tools/stratum_rtps_replay <timeline> 3   # 声称 domain=3
//   实际进程绑定：7400 / 9060                # domain 0 的端口
//
// 后果：任何在非 0 域上等待的订阅端都收不到数据，而**两端都没有任何报错**。
// 这个缺陷直接导致了"Stratum 与 Fast DDS 在非 0 domain 上完全发现不到彼此"，
// 并一度被误判为 WSL 网络环境问题。
//
// 本测试锁死"配置必须传到后端"这一契约（用 MockBackend 记录收到的 config）。

#include <cstdlib>
#include <string>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"
#include "mock_backend.h"
#include "test_framework.h"
#include "test_isolation.h"

using namespace stratum;
using namespace stratum::testing;

namespace {
constexpr TimestampNs kBaseTs = 1700000000000000000ULL;
}

// ---------------------------------------------------------------------------
// 录制端：RecorderConfig::backend 必须原样交给 CreateSource
// ---------------------------------------------------------------------------
STRATUM_TEST(BackendConfigPropagation, RecorderForwardsConfigToBackend) {
  const std::string dir = std::string("/tmp/stratum_backend_cfg_rec_") + UniqueSuffix();
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  TimelineImplV2 timeline;
  STRATUM_CHECK_OK(timeline.Open(dir));

  MockBackend backend;
  Recorder recorder(&timeline, &backend);

  RecorderConfig rcfg;
  rcfg.close_timeline_on_stop = false;
  rcfg.backend.domain_id = 42;
  rcfg.backend.participant_name = "cfg_test_recorder";
  rcfg.record.topics = {"hello_world_topic"};

  STRATUM_CHECK_OK(recorder.Start(rcfg));

  ctx.param_desc = "last_source_domain=" +
                   std::to_string(MockBackend::last_source_domain());
  // 【缺陷断言】修复前这里是 0（默认构造的 BackendConfig）
  STRATUM_CHECK_EQ(MockBackend::last_source_domain(), static_cast<uint32_t>(42));
  // topics 也必须继续传递（原有行为不能被这次修复弄丢）
  STRATUM_CHECK_EQ(MockBackend::last_source_topic_count(), static_cast<size_t>(1));

  STRATUM_CHECK_OK(recorder.Stop());
  STRATUM_CHECK_OK(timeline.Close());
}

// ---------------------------------------------------------------------------
// 回放端：PlayerConfig::backend 必须原样交给 CreateSink
// ---------------------------------------------------------------------------
STRATUM_TEST(BackendConfigPropagation, PlayerForwardsConfigToBackend) {
  const std::string dir = std::string("/tmp/stratum_backend_cfg_play_") + UniqueSuffix();
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  // 先写一条数据，保证 Play 有内容可走完整流程
  {
    TimelineImplV2 writer_timeline;
    STRATUM_CHECK_OK(writer_timeline.Open(dir));

    TopicMetadata meta;
    meta.topic_name = "hello_world_topic";
    meta.type_name = "HelloWorld";
    uint32_t topic_id = 0;
    STRATUM_CHECK_OK(writer_timeline.RegisterTopic(meta, &topic_id));

    std::vector<Event> events;
    Event e;
    e.topic_id = topic_id;
    e.seq = 0;
    e.source_timestamp_ns = kBaseTs;
    e.reception_timestamp_ns = kBaseTs + 1000;
    e.payload = "payload-bytes";
    events.push_back(std::move(e));
    STRATUM_CHECK_OK(writer_timeline.AppendBatch(events));
    STRATUM_CHECK_OK(writer_timeline.Close());
  }

  TimelineImplV2 timeline;
  STRATUM_CHECK_OK(timeline.Open(dir));

  MockBackend backend;
  Player player(&timeline, &backend);

  size_t published = 0;
  PlayerConfig pcfg;
  pcfg.rate = 0;                            // 尽快回放
  pcfg.allow_original_topic_names = true;
  pcfg.backend.domain_id = 7;
  pcfg.backend.participant_name = "cfg_test_player";
  pcfg.publish_callback = [&published](const protocol::ReceivedSample&) {
    ++published;
  };

  STRATUM_CHECK_OK(player.Play(TimeRange{}, pcfg));
  STRATUM_CHECK_EQ(published, static_cast<size_t>(1));

  ctx.param_desc = "last_sink_domain=" +
                   std::to_string(MockBackend::last_sink_domain());
  // 【缺陷断言】修复前这里是 0，回放永远落在 domain 0
  STRATUM_CHECK_EQ(MockBackend::last_sink_domain(), static_cast<uint32_t>(7));

  STRATUM_CHECK_OK(timeline.Close());
}

// ---------------------------------------------------------------------------
// 默认行为不变：不显式设置时 domain 仍为 0（不引入意外副作用）
// ---------------------------------------------------------------------------
STRATUM_TEST(BackendConfigPropagation, DefaultsRemainZero) {
  const std::string dir = std::string("/tmp/stratum_backend_cfg_default_") + UniqueSuffix();
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  TimelineImplV2 timeline;
  STRATUM_CHECK_OK(timeline.Open(dir));

  MockBackend backend;
  Recorder recorder(&timeline, &backend);

  RecorderConfig rcfg;
  rcfg.close_timeline_on_stop = false;
  STRATUM_CHECK_OK(recorder.Start(rcfg));
  STRATUM_CHECK_EQ(MockBackend::last_source_domain(), static_cast<uint32_t>(0));
  STRATUM_CHECK_OK(recorder.Stop());
  STRATUM_CHECK_OK(timeline.Close());
}

STRATUM_TEST_MAIN()
