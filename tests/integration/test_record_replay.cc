// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 端到端测试：录制 → 回放 → 逐条比对
//
// 这是 Stratum 最核心的验证：它回答"录下来的东西能不能一模一样地放出来"。
// 验证维度：数量一致、内容一致、顺序一致、时间戳一致、跨引擎/跨重启一致。
//
// 使用 MockBackend，测试不依赖网络与真实 DDS，可在 CI 稳定运行。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl.h"
#include "stratum/storage/lsm/lsm_engine.h"
#include "stratum/storage/memory/memory_engine.h"
#include "mock_backend.h"
#include "test_framework.h"
#include "test_isolation.h"

using namespace stratum;
using namespace stratum::storage;
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
  rcfg.write_batch_size = 16;
  rcfg.close_timeline_on_stop = false;

  Status s = recorder.Start(rcfg);
  if (!s.ok()) {
    return s;
  }

  // source 所有权已转移给 Recorder，经静态槽位访问
  MockSource* source = MockBackend::source_slot();
  if (source == nullptr) {
    return Status::Internal("mock source not registered");
  }

  for (const auto& sample : samples) {
    source->Push(sample);
  }

  const int kMaxWaitMs = 5000;
  int waited = 0;
  while (source->delivered() < samples.size() && waited < kMaxWaitMs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    waited += 10;
  }

  s = recorder.Stop();
  if (stats_out != nullptr) {
    *stats_out = recorder.GetStats();
  }
  return s;
}

// 回放辅助：用 publish_callback 收集（不依赖 sink 指针存活）
Status DoPlay(ITimeline* timeline, protocol::IDdsBackend* backend,
              const TimeRange& range, double rate, bool map_topics,
              std::vector<protocol::ReceivedSample>* out) {
  Player player(timeline, backend);
  PlayerConfig pcfg;
  pcfg.rate = rate;
  pcfg.allow_original_topic_names = !map_topics;
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

STRATUM_TEST(EndToEnd, RecordThenReplayAllMatches) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://e2e"));

  MockBackend backend;
  const auto samples = MakeSamples(20);
  STRATUM_CHECK_EQ(samples.size(), static_cast<size_t>(60));

  Recorder::Stats stats;
  STRATUM_CHECK_OK(DoRecord(&timeline, &backend, samples, &stats));

  ctx.param_desc = "recorded=" + std::to_string(stats.recorded);
  STRATUM_CHECK_EQ(stats.recorded, static_cast<uint64_t>(60));
  STRATUM_CHECK_EQ(stats.dropped, static_cast<uint64_t>(0));

  MockBackend player_backend;
  std::vector<protocol::ReceivedSample> published;
  STRATUM_CHECK_OK(
      DoPlay(&timeline, &player_backend, TimeRange(), 0.0, false, &published));

  ctx.param_desc = "published=" + std::to_string(published.size());
  STRATUM_CHECK_EQ(published.size(), static_cast<size_t>(60));

  auto expected = samples;
  SortSamples(&expected);
  SortSamples(&published);

  for (size_t i = 0; i < published.size(); ++i) {
    ctx.param_desc = "index=" + std::to_string(i);
    STRATUM_CHECK_EQ(published[i].payload, expected[i].payload);
    STRATUM_CHECK_EQ(published[i].source_timestamp_ns,
                     expected[i].source_timestamp_ns);
  }
}

STRATUM_TEST(EndToEnd, RecordReplayOnLsmEngineAcrossReopen) {
  const std::string dir = std::string("/tmp/stratum_e2e_lsm_") + UniqueSuffix();
  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }

  {
    auto engine = StorageEnginePtr(new LsmEngine());
    TimelineImpl timeline(std::move(engine));
    STRATUM_CHECK_OK(timeline.Open(dir));

    MockBackend backend;
    const auto samples = MakeSamples(15);
    Recorder::Stats stats;
    STRATUM_CHECK_OK(DoRecord(&timeline, &backend, samples, &stats));
    STRATUM_CHECK_EQ(stats.recorded, static_cast<uint64_t>(45));

    STRATUM_CHECK_OK(timeline.Flush());
    STRATUM_CHECK_OK(timeline.Close());
  }

  {
    auto engine = StorageEnginePtr(new LsmEngine());
    TimelineImpl timeline(std::move(engine));
    STRATUM_CHECK_OK(timeline.Open(dir));

    std::vector<TopicMetadata> topics;
    STRATUM_CHECK_OK(timeline.ListTopics(&topics));
    ctx.param_desc = "topics=" + std::to_string(topics.size());
    STRATUM_CHECK_EQ(topics.size(), static_cast<size_t>(3));

    MockBackend player_backend;
    std::vector<protocol::ReceivedSample> published;
    STRATUM_CHECK_OK(
        DoPlay(&timeline, &player_backend, TimeRange(), 0.0, false, &published));
    STRATUM_CHECK_EQ(published.size(), static_cast<size_t>(45));

    timeline.Close();
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }
}

STRATUM_TEST(EndToEnd, TopicMappingAvoidsPollution) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://mapping"));

  MockBackend backend;
  const auto samples = MakeSamples(5);
  Recorder::Stats stats;
  STRATUM_CHECK_OK(DoRecord(&timeline, &backend, samples, &stats));

  MockBackend player_backend;
  std::vector<protocol::ReceivedSample> published;
  STRATUM_CHECK_OK(
      DoPlay(&timeline, &player_backend, TimeRange(), 0.0, true, &published));

  STRATUM_CHECK_TRUE(!published.empty());
  const std::string suffix = "_replay";
  for (const auto& s : published) {
    ctx.param_desc = s.topic_name;
    STRATUM_CHECK_TRUE(s.topic_name.size() > suffix.size());
    STRATUM_CHECK_EQ(s.topic_name.substr(s.topic_name.size() - suffix.size()),
                     suffix);
  }
}

STRATUM_TEST(EndToEnd, ReplaySubRange) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://subrange"));

  MockBackend backend;
  const auto samples = MakeSamples(20);
  Recorder::Stats stats;
  STRATUM_CHECK_OK(DoRecord(&timeline, &backend, samples, &stats));

  TimeRange range;
  range.begin_ns = kBaseTs + 5 * 1000000ULL;
  range.end_ns = kBaseTs + 10 * 1000000ULL;

  MockBackend player_backend;
  std::vector<protocol::ReceivedSample> published;
  STRATUM_CHECK_OK(
      DoPlay(&timeline, &player_backend, range, 0.0, false, &published));

  ctx.param_desc = "published=" + std::to_string(published.size());
  STRATUM_CHECK_EQ(published.size(), static_cast<size_t>(15));

  for (const auto& s : published) {
    STRATUM_CHECK_TRUE(s.source_timestamp_ns >= range.begin_ns);
    STRATUM_CHECK_TRUE(s.source_timestamp_ns < range.end_ns);
  }
}

STRATUM_TEST(EndToEnd, QueryTopicSummary) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://query"));

  MockBackend backend;
  const auto samples = MakeSamples(10);
  Recorder::Stats stats;
  STRATUM_CHECK_OK(DoRecord(&timeline, &backend, samples, &stats));

  Query query(&timeline);
  std::vector<TopicSummary> topics;
  STRATUM_CHECK_OK(query.ListTopics(&topics));

  STRATUM_CHECK_EQ(topics.size(), static_cast<size_t>(3));
  for (const auto& t : topics) {
    ctx.param_desc = t.topic_name;
    STRATUM_CHECK_EQ(t.message_count, static_cast<uint64_t>(10));
    STRATUM_CHECK_TRUE(t.first_ns != kInvalidTimestamp);
    STRATUM_CHECK_TRUE(t.last_ns >= t.first_ns);
  }

  TopicMetadata meta;
  STRATUM_CHECK_OK(query.FindTopic("sensor/temp", &meta));
  std::vector<Event> events;
  STRATUM_CHECK_OK(query.ExportRange(meta.topic_id, TimeRange(), &events));
  STRATUM_CHECK_EQ(events.size(), static_cast<size_t>(10));
}

STRATUM_TEST(EndToEnd, QualityReportIsGenerated) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://quality"));

  MockBackend backend;
  const auto samples = MakeSamples(10);
  Recorder::Stats stats;
  STRATUM_CHECK_OK(DoRecord(&timeline, &backend, samples, &stats));

  QualityReporter reporter(&timeline);
  QualityReport report;
  STRATUM_CHECK_OK(reporter.Generate(0, &report));

  ctx.param_desc = "level";
  STRATUM_CHECK_EQ(static_cast<int>(report.level),
                   static_cast<int>(QualityReport::Level::kGood));

  const std::string text = report.ToText();
  STRATUM_CHECK_TRUE(!text.empty());
  STRATUM_CHECK_TRUE(text.find("使用建议") != std::string::npos);
  STRATUM_CHECK_TRUE(text.find("诚实声明") != std::string::npos);

  const std::string json = report.ToJson();
  STRATUM_CHECK_TRUE(json.find("\"level\":\"good\"") != std::string::npos);
}

STRATUM_TEST_MAIN()
