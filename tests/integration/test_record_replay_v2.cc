// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 端到端测试：Recorder/Player/Query 跑在 TimelineImplV2（LSMengine 后端）
//
// ===========================================================================
// 目的：证明"上层零修改"承诺
// ===========================================================================
//
// test_record_replay.cc 里的 DoRecord/DoPlay 接受 ITimeline* 接口。
// 本测试把 TimelineImpl（v1 MemoryEngine）换成 TimelineImplV2（LSMengine），
// **上层代码一行不改**，验证端到端行为一致。
//
// 数据量（07 标准）：端到端 ≥ 20,000 条。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"
#include "mock_backend.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::testing;

namespace {

constexpr uint64_t kBaseTs = 1700000000000000000ULL;

std::vector<protocol::ReceivedSample> MakeSamples(int count_per_topic) {
  std::vector<protocol::ReceivedSample> samples;
  const char* topics[] = {"sensor/temp", "sensor/pressure", "control/cmd"};
  for (int t = 0; t < 3; ++t) {
    for (int i = 0; i < count_per_topic; ++i) {
      protocol::ReceivedSample s;
      s.topic_name = topics[t];
      s.type_name = "TestType";
      s.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(i) * 1000000ULL +
                              static_cast<uint64_t>(t) * 1000ULL;
      s.reception_timestamp_ns = s.source_timestamp_ns + 500;
      s.sequence = static_cast<uint32_t>(i);
      s.payload = std::string("payload-") + topics[t] + "-" + std::to_string(i);
      samples.push_back(std::move(s));
    }
  }
  std::sort(samples.begin(), samples.end(),
            [](const protocol::ReceivedSample& a,
               const protocol::ReceivedSample& b) {
              return a.source_timestamp_ns < b.source_timestamp_ns;
            });
  return samples;
}

Status DoRecord(ITimeline* timeline, protocol::IDdsBackend* backend,
                const std::vector<protocol::ReceivedSample>& samples,
                Recorder::Stats* stats_out) {
  Recorder recorder(timeline, backend);
  RecorderConfig rcfg;
  rcfg.record.max_buffer_bytes = 64 * 1024 * 1024;
  rcfg.write_batch_size = 256;
  rcfg.close_timeline_on_stop = false;

  Status s = recorder.Start(rcfg);
  if (!s.ok()) return s;

  MockSource* source = MockBackend::source_slot();
  if (source == nullptr) return Status::Internal("mock source not registered");

  // 分批推送并等待送达（受控速率，见 test_recorder_queue 的教训）
  constexpr size_t kPushBatch = 200;
  for (size_t i = 0; i < samples.size(); i += kPushBatch) {
    const size_t end = std::min(i + kPushBatch, samples.size());
    for (size_t j = i; j < end; ++j) {
      source->Push(samples[j]);
    }
    int waited = 0;
    while (waited < 10000) {
      if (recorder.GetStats().received >= end) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      waited += 2;
    }
  }

  s = recorder.Stop();
  if (stats_out != nullptr) *stats_out = recorder.GetStats();
  return s;
}

Status DoPlay(ITimeline* timeline, protocol::IDdsBackend* backend,
              const TimeRange& range, std::vector<protocol::ReceivedSample>* out) {
  Player player(timeline, backend);
  PlayerConfig pcfg;
  pcfg.rate = 0;
  pcfg.allow_original_topic_names = true;
  pcfg.publish_callback = [out](const protocol::ReceivedSample& s) {
    out->push_back(s);
  };
  return player.Play(range, pcfg);
}

void SortSamples(std::vector<protocol::ReceivedSample>* v) {
  std::sort(v->begin(), v->end(),
            [](const protocol::ReceivedSample& a,
               const protocol::ReceivedSample& b) {
              if (a.source_timestamp_ns != b.source_timestamp_ns) {
                return a.source_timestamp_ns < b.source_timestamp_ns;
              }
              return a.payload < b.payload;
            });
}

}  // namespace

// ---------------------------------------------------------------------------
// 端到端：21,000 条录制 → 回放 → 逐条比对（v2 后端）
// ---------------------------------------------------------------------------
STRATUM_TEST(EndToEndV2, RecordThenReplayAllMatches) {
  const std::string dir = "/tmp/stratum_e2e_v2";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  TimelineImplV2 timeline;
  STRATUM_CHECK_OK(timeline.Open(dir));

  MockBackend backend;
  const auto samples = MakeSamples(7000);  // 3 × 7000 = 21000 条
  STRATUM_CHECK_EQ(samples.size(), static_cast<size_t>(21000));

  Recorder::Stats stats;
  STRATUM_CHECK_OK(DoRecord(&timeline, &backend, samples, &stats));

  ctx.param_desc = "recorded=" + std::to_string(stats.recorded) +
                   " dropped=" + std::to_string(stats.dropped);
  STRATUM_CHECK_EQ(stats.recorded, static_cast<uint64_t>(21000));
  STRATUM_CHECK_EQ(stats.dropped, static_cast<uint64_t>(0));

  // 回放
  MockBackend player_backend;
  std::vector<protocol::ReceivedSample> published;
  STRATUM_CHECK_OK(DoPlay(&timeline, &player_backend, TimeRange(), &published));

  ctx.param_desc += " published=" + std::to_string(published.size());
  STRATUM_CHECK_EQ(published.size(), static_cast<size_t>(21000));

  auto expected = samples;
  SortSamples(&expected);
  SortSamples(&published);
  int mismatches = 0;
  for (size_t i = 0; i < published.size(); ++i) {
    if (published[i].payload != expected[i].payload ||
        published[i].source_timestamp_ns != expected[i].source_timestamp_ns) {
      ++mismatches;
    }
  }
  ctx.param_desc += " mismatches=" + std::to_string(mismatches);
  STRATUM_CHECK_EQ(mismatches, 0);

  timeline.Close();
  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

// ---------------------------------------------------------------------------
// 跨重启：录制 → 关闭 → 重开 → 回放逐条比对（LSMengine 持久化）
// ---------------------------------------------------------------------------
STRATUM_TEST(EndToEndV2, RecordReplayAcrossRestart) {
  const std::string dir = "/tmp/stratum_e2e_v2b";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  const auto samples = MakeSamples(5000);  // 15000 条

  {
    TimelineImplV2 timeline;
    STRATUM_CHECK_OK(timeline.Open(dir));
    MockBackend backend;
    Recorder::Stats stats;
    STRATUM_CHECK_OK(DoRecord(&timeline, &backend, samples, &stats));
    STRATUM_CHECK_EQ(stats.recorded, static_cast<uint64_t>(15000));
    STRATUM_CHECK_OK(timeline.Close());
  }

  {
    TimelineImplV2 timeline;
    STRATUM_CHECK_OK(timeline.Open(dir));

    MockBackend player_backend;
    std::vector<protocol::ReceivedSample> published;
    STRATUM_CHECK_OK(DoPlay(&timeline, &player_backend, TimeRange(), &published));

    ctx.param_desc = "重启后回放条数=" + std::to_string(published.size());
    STRATUM_CHECK_EQ(published.size(), static_cast<size_t>(15000));

    auto expected = samples;
    SortSamples(&expected);
    SortSamples(&published);
    int mismatches = 0;
    for (size_t i = 0; i < published.size(); ++i) {
      if (published[i].payload != expected[i].payload) ++mismatches;
    }
    ctx.param_desc += " 不一致=" + std::to_string(mismatches);
    STRATUM_CHECK_EQ(mismatches, 0);

    timeline.Close();
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

// ---------------------------------------------------------------------------
// Query 接口跑在 v2 上
// ---------------------------------------------------------------------------
STRATUM_TEST(EndToEndV2, QueryWorksOnV2) {
  const std::string dir = "/tmp/stratum_e2e_v2c";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  TimelineImplV2 timeline;
  STRATUM_CHECK_OK(timeline.Open(dir));

  MockBackend backend;
  const auto samples = MakeSamples(2000);
  Recorder::Stats stats;
  STRATUM_CHECK_OK(DoRecord(&timeline, &backend, samples, &stats));

  Query query(&timeline);
  std::vector<TopicSummary> topics;
  STRATUM_CHECK_OK(query.ListTopics(&topics));
  ctx.param_desc = "topics=" + std::to_string(topics.size());
  STRATUM_CHECK_EQ(topics.size(), static_cast<size_t>(3));

  TopicMetadata meta;
  STRATUM_CHECK_OK(query.FindTopic("sensor/temp", &meta));
  std::vector<Event> events;
  STRATUM_CHECK_OK(query.ExportRange(meta.topic_id, TimeRange(), &events));
  ctx.param_desc += " exported=" + std::to_string(events.size());
  STRATUM_CHECK_EQ(events.size(), static_cast<size_t>(2000));

  timeline.Close();
  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

STRATUM_TEST_MAIN()
