// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 示例 01：录制与回放（最小完整流程）
//
// 这是 Stratum 作为**库**被嵌入时的最短路径，展示三件事：
//   1. 组装：Timeline（存储）+ Backend（协议）→ Recorder / Player
//   2. 录制：Start(config) → 采样 → Stop()，并可随时读 GetStats()
//   3. 回放：Play(range, config)，回调里可观察每条样本
//
// 编译：见 examples/cpp/CMakeLists.txt
// 运行：./01_record_and_replay <时间线目录> [domain] [录制秒数]
//
// 关键点（都是踩过坑的地方，注释里写明原因）：
//   · RecorderConfig::backend 必须显式赋值，否则 domain 会被内部默认值覆盖
//   · PlayerConfig::discovery_wait_ms 必须留出发现时间，否则对端收不到
//   · PlayerConfig::republish_unacked 默认开启，负责补回首条（VOLATILE 语义）

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"
#include "stratum/protocol/rtps/rtps_backend.h"

using namespace stratum;
using namespace stratum::protocol;
using namespace stratum::protocol::rtps;

namespace {

void PrintStats(const Recorder::Stats& st) {
  std::printf("    received=%llu recorded=%llu dropped=%llu bytes=%llu queue=%llu\n",
              static_cast<unsigned long long>(st.received),
              static_cast<unsigned long long>(st.recorded),
              static_cast<unsigned long long>(st.dropped),
              static_cast<unsigned long long>(st.bytes_recorded),
              static_cast<unsigned long long>(st.queue_depth));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("用法: %s <时间线目录> [domain=0] [录制秒数=5]\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;
  const int seconds = (argc > 3) ? std::atoi(argv[3]) : 5;

  // -------------------------------------------------------------------------
  // 1) 打开时间线（存储层）
  //
  // ITimeline 是存储抽象；TimelineImplV2 是当前基于 LSMengine 的实现。
  // 上层代码只依赖 ITimeline，因此可以替换存储后端而不改业务代码。
  // -------------------------------------------------------------------------
  TimelineImplV2 timeline;
  Status s = timeline.Open(dir);
  if (!s.ok()) {
    std::printf("FAILED: timeline.Open(%s): %s\n", dir.c_str(),
                s.ToString().c_str());
    return 1;
  }
  std::printf("时间线已打开: %s\n", dir.c_str());

  // -------------------------------------------------------------------------
  // 2) 协议后端
  //
  // IDdsBackend 是协议抽象：CreateSource 供录制、CreateSink 供回放。
  // RtpsBackend 是自带的 RTPS 实现；换成别的 DDS 实现只需替换这一行。
  // -------------------------------------------------------------------------
  stratum::protocol::rtps::RtpsBackend backend;

  // -------------------------------------------------------------------------
  // 3) 录制
  // -------------------------------------------------------------------------
  Recorder recorder(&timeline, &backend);

  RecorderConfig rcfg;
  rcfg.record.max_buffer_bytes = 32ULL * 1024 * 1024;   // 缓冲上限
  rcfg.record.overflow_policy = RecordConfig::OverflowPolicy::kDropNewest;
  rcfg.record.durability = RecordConfig::DurabilityLevel::kBalanced;
  // rcfg.record.topics 留空 = 自动订阅全部发现到的话题
  rcfg.write_batch_size = 128;
  rcfg.close_timeline_on_stop = false;   // 后面还要用它做查询与回放

  protocol::BackendConfig bcfg;
  bcfg.domain_id = domain;
  bcfg.participant_name = "stratum_example_01";
  // 【必须显式传入】否则 Recorder 内部会用一个默认构造的 BackendConfig，
  // 调用方设置的 domain 被静默丢弃 —— 表现为"无论传什么域，都跑在 domain 0"。
  rcfg.backend = bcfg;

  s = recorder.Start(rcfg);
  if (!s.ok()) {
    std::printf("FAILED: recorder.Start: %s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("录制中（domain=%u，%d 秒）...\n", domain, seconds);
  for (int i = 0; i < seconds; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::printf("  [%2ds]", i + 1);
    PrintStats(recorder.GetStats());
  }

  s = recorder.Stop();
  if (!s.ok()) {
    std::printf("WARN: recorder.Stop: %s\n", s.ToString().c_str());
  }

  const Recorder::Stats final_stats = recorder.GetStats();
  std::printf("\n=== 录制结果 ===\n");
  PrintStats(final_stats);
  if (final_stats.recorded == 0) {
    std::printf("提示：没录到数据。检查是否有发布者、domain 是否一致。\n");
  }

  // -------------------------------------------------------------------------
  // 4) 查询录到了什么
  //
  // Query 是只读视图，可在录制结束后（或录制过程中）使用。
  // -------------------------------------------------------------------------
  Query query(&timeline);
  std::vector<TopicSummary> topics;
  s = query.ListTopics(&topics);
  if (!s.ok()) {
    std::printf("FAILED: query.ListTopics: %s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("\n=== 录到 %zu 个话题 ===\n", topics.size());
  for (const auto& t : topics) {
    std::printf("  %-32s type=%-20s count=%llu bytes=%llu\n", t.topic_name.c_str(),
                t.type_name.c_str(),
                static_cast<unsigned long long>(t.message_count),
                static_cast<unsigned long long>(t.byte_count));
  }
  if (topics.empty()) {
    std::printf("（无话题，跳过回放）\n");
    timeline.Close();
    return 0;
  }

  // -------------------------------------------------------------------------
  // 5) 回放
  //
  // Player 用同一份 timeline 读数据，经 backend 的 Sink 发布出去。
  // -------------------------------------------------------------------------
  Player player(&timeline, &backend);

  PlayerConfig pcfg;
  pcfg.rate = 1.0;                 // 1.0 = 原速；0 = 尽可能快
  pcfg.backend = bcfg;             // 同 Recorder：必须显式传，否则 domain 丢失
  pcfg.discovery_wait_ms = 3000;   // 【关键】等对端完成匹配，否则首包会丢
  pcfg.wait_for_ack = true;
  pcfg.republish_unacked = true;   // 用新 SN 补回首条（VOLATILE 下必需）
  pcfg.publish_callback = [](const protocol::ReceivedSample& sample) {
    // 回调在回放线程上执行：**不要阻塞**，否则会拖慢回放时序。
    std::printf("  [replay] topic=%-24s seq=%u bytes=%zu\n",
                sample.topic_name.c_str(), sample.sequence,
                sample.payload.size());
  };

  TimeRange range;   // 默认 {0, 0} = 全范围
  std::printf("\n=== 开始回放 ===\n");
  s = player.Play(range, pcfg);
  if (!s.ok()) {
    std::printf("FAILED: player.Play: %s\n", s.ToString().c_str());
    timeline.Close();
    return 1;
  }

  const PlaybackProgress prog = player.GetProgress();
  std::printf("\n=== 回放结束 ===\n");
  std::printf("  已发布=%llu 总数=%llu 进度=%.1f%% finished=%d\n",
              static_cast<unsigned long long>(prog.published),
              static_cast<unsigned long long>(prog.total), prog.Percent(),
              prog.finished ? 1 : 0);

  timeline.Close();
  return 0;
}
