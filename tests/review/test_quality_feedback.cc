// N-1 验证：Recorder 丢弃必须汇入质量统计
//
// 修复前：dropped 只加 Recorder 自己的原子量，QualityStats::dropped_messages
// 恒为 0 → 丢 96% 数据后质量报告仍输出"可信度高"（审查员实测）。
// 修复后：丢弃通过 ITimeline::ReportDrop 汇入，质量报告应报 kBad。

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl.h"
#include "stratum/core/timeline_impl_v2.h"
#include "stratum/storage/memory/memory_engine.h"
#include "mock_backend.h"
#include "test_framework.h"
#include "test_isolation.h"

using namespace stratum;
using namespace stratum::storage;
using namespace stratum::testing;

namespace {
constexpr uint64_t kBaseTs = 1700000000000000000ULL;
constexpr const char* kLongTopic =
    "factory/line-a/station-07/sensor/temperature/raw";
}

// 灌入大量数据（必然丢弃），检查统计与报告
void RunDropScenario(ITimeline* timeline, protocol::IDdsBackend* backend,
                     uint64_t* out_dropped, QualityStats* out_quality,
                     std::string* out_setup_error) {
  Recorder recorder(timeline, backend);
  RecorderConfig rcfg;
  rcfg.record.max_buffer_bytes = 2000;  // 极小：必然溢出
  rcfg.record.overflow_policy = RecordConfig::OverflowPolicy::kDropNewest;
  rcfg.write_batch_size = 32;
  rcfg.close_timeline_on_stop = false;

  // 【不可静默返回】早期版本在 Start 失败或 MockSource 未注册时直接 return，
  // 结果是调用方拿着未写入的统计继续断言（失败信息指向错误的根因）。
  // 这里用 out 参数把失败显式传递出去，调用方据此断言。
  Status start_status = recorder.Start(rcfg);
  if (!start_status.ok()) {
    *out_setup_error = "recorder.Start 失败: " + start_status.ToString();
    return;
  }

  MockSource* source = MockBackend::source_slot();
  if (source == nullptr) {
    *out_setup_error = "MockSource 未注册（MockBackend::source_slot() 返回空）";
    return;
  }

  // 5000 条：远超 2000 字节上限
  for (int i = 0; i < 5000; ++i) {
    protocol::ReceivedSample s;
    s.topic_name = kLongTopic;
    s.type_name = "SensorData";
    s.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(i) * 1000000ULL;
    s.reception_timestamp_ns = s.source_timestamp_ns + 1000;
    s.sequence = static_cast<uint32_t>(i);
    s.payload.assign(16, 'p');
    source->Push(s);
  }

  // 等全部处理完
  int waited = 0;
  while (waited < 20000) {
    const Recorder::Stats st = recorder.GetStats();
    if (st.received >= 5000 && st.queue_depth == 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    waited += 10;
  }

  const Recorder::Stats st = recorder.GetStats();
  *out_dropped = st.dropped;
  *out_quality = timeline->GetQualityStats();
  recorder.Stop();
}

STRATUM_TEST(N1, DropsFeedQualityStatsV1) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://n1-v1"));

  MockBackend backend;
  uint64_t dropped = 0;
  QualityStats q;
  std::string setup_err;
  RunDropScenario(&timeline, &backend, &dropped, &q, &setup_err);
  STRATUM_CHECK_EQ(setup_err, std::string());

  ctx.param_desc = "dropped=" + std::to_string(dropped) +
                   " quality.dropped=" + std::to_string(q.dropped_messages);

  // 前提：确实发生了丢弃（否则测试无效）
  STRATUM_CHECK_TRUE(dropped > 100);
  // 【核心断言】丢弃必须汇入质量统计
  STRATUM_CHECK_EQ(q.dropped_messages, dropped);
  STRATUM_CHECK_TRUE(q.dropped_bytes > 0);
  STRATUM_CHECK_TRUE(q.HasIntegrityIssue());

  // 质量报告必须报 kBad
  QualityReporter reporter(&timeline);
  QualityReport report;
  STRATUM_CHECK_OK(reporter.Generate(0, &report));
  ctx.param_desc += " level=" + std::to_string(static_cast<int>(report.level));
  STRATUM_CHECK_EQ(static_cast<int>(report.level),
                   static_cast<int>(QualityReport::Level::kBad));
}

STRATUM_TEST(N1, DropsFeedQualityStatsV2) {
  const std::string dir = std::string("/tmp/stratum_n1_v2_") + UniqueSuffix();
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  TimelineImplV2 timeline;
  STRATUM_CHECK_OK(timeline.Open(dir));

  MockBackend backend;
  uint64_t dropped = 0;
  QualityStats q;
  std::string setup_err;
  RunDropScenario(&timeline, &backend, &dropped, &q, &setup_err);
  STRATUM_CHECK_EQ(setup_err, std::string());

  ctx.param_desc = "v2 dropped=" + std::to_string(dropped) +
                   " quality.dropped=" + std::to_string(q.dropped_messages);

  STRATUM_CHECK_TRUE(dropped > 100);
  STRATUM_CHECK_EQ(q.dropped_messages, dropped);

  timeline.Close();
  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

STRATUM_TEST_MAIN()
