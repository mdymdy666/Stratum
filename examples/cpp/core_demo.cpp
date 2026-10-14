// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// examples/cpp/core_demo.cpp
//
// ─────────────────────────────────────────────────────────────────────────────
// core 模块 · 简单用法演示
//
// 覆盖的 API（core/recorder.h、core/itimeline.h、core/timeline_impl_v2.h）：
//   TimelineImplV2::Open / Close   打开与关闭时间线（存储抽象的实现）
//   Recorder(timeline, backend)    录制器：把数据源源源不断写进时间线
//   Recorder::Start / Stop         启停录制
//   Recorder::GetStats             实时统计（received/recorded/dropped/queue）
//   Query::ListTopics              列出录到了哪些话题
//   Query::GetInfo                 会话总体信息
//   Player(timeline, backend)      回放器：把录制内容重新发出去
//   Player::Play / GetProgress     回放与进度
//
// 本示例用**内存后端**（common/memory_backend.h），因此不需要任何外部参与者，
// 直接就能跑。真实链路（RtpsBackend）的用法与此**完全一致**，
// 区别只在把 `MemoryBackend` 换成 `stratum::protocol::rtps::RtpsBackend`。
//
// 构建与运行见 examples/cpp/CMakeLists.txt；运行：
//   ./core_demo /tmp/core_demo_tl
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>
#include <string>
#include <vector>

#include "common/memory_backend.h"
#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"

using namespace stratum;

int main(int argc, char** argv) {
  const std::string dir = (argc > 1) ? argv[1] : "/tmp/core_demo_tl";

  std::printf("=== core_demo：录制 → 查询 → 回放 ===\n\n");

  // ───────────────────────────────────────────────────────────────────────
  // 步骤 1：打开时间线
  //
  // ITimeline 是存储抽象；TimelineImplV2 是基于 LSMengine 的实现。
  // 上层只依赖 ITimeline，所以换存储实现不动业务代码。
  //
  // 注意：Open 会**创建目录**。若目录里已有时间线，行为由实现决定；
  // 生产代码里应先用 Query 看一眼再决定是追加还是换目录。
  // ───────────────────────────────────────────────────────────────────────
  TimelineImplV2 timeline;
  Status s = timeline.Open(dir);
  if (!s.ok()) {
    // Status 是轻量值类型，用 ToString() 取可读信息，ok() 判断成败
    std::printf("打开时间线失败: %s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("[1] 时间线已打开: %s\n", dir.c_str());

  // ───────────────────────────────────────────────────────────────────────
  // 步骤 2：构造后端
  //
  // IDdsBackend 提供两个工厂：
  //   CreateSource() —— 录制方向的数据来源
  //   CreateSink()   —— 回放方向的数据去向
  // Recorder 和 Player 都只认这个接口，因此后端可替换。
  // ───────────────────────────────────────────────────────────────────────
  examples::MemoryBackend backend;
  std::printf("[2] 后端: %s v%s\n", backend.Name().c_str(), backend.Version().c_str());

  // ───────────────────────────────────────────────────────────────────────
  // 步骤 3：录制
  // ───────────────────────────────────────────────────────────────────────
  Recorder recorder(&timeline, &backend);

  RecorderConfig rcfg;
  rcfg.close_timeline_on_stop = false;   // 我们后面还要用同一份时间线做查询与回放
  rcfg.write_batch_size = 64;            // 批量写盘的大小：越大吞吐越高、延迟越大

  // 数据源参数经 BackendConfig::extra 传给后端。
  // 【注意】extra 是 vector<pair<string,string>>，不是 map。
  rcfg.backend.extra.push_back({"count", "50"});
  rcfg.backend.extra.push_back({"interval_us", "1000"});
  rcfg.backend.extra.push_back({"topic", "demo/temperature"});
  rcfg.backend.extra.push_back({"type_name", "DemoTemp"});
  rcfg.backend.extra.push_back({"payload_bytes", "48"});

  s = recorder.Start(rcfg);
  if (!s.ok()) {
    std::printf("启动录制失败: %s\n", s.ToString().c_str());
    timeline.Close();
    return 1;
  }

  // 等录制跑完。真实场景里这里通常是你的业务主循环。
  // GetStats() 可以随时调用，用来做监控或背压判断。
  std::printf("[3] 录制中（50 条）...\n");
  for (int i = 0; i < 100; ++i) {
    const Recorder::Stats st = recorder.GetStats();
    if (st.received >= 50) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  s = recorder.Stop();
  if (!s.ok()) {
    std::printf("  Stop 返回: %s（继续）\n", s.ToString().c_str());
  }

  // Stats 是 Recorder 的**嵌套类型**（Recorder::Stats），不是顶层名字
  const Recorder::Stats st = recorder.GetStats();
  std::printf("    received=%llu recorded=%llu dropped=%llu bytes=%llu\n",
              static_cast<unsigned long long>(st.received),
              static_cast<unsigned long long>(st.recorded),
              static_cast<unsigned long long>(st.dropped),
              static_cast<unsigned long long>(st.bytes_recorded));
  if (st.dropped > 0) {
    std::printf("    ⚠ 有丢弃：说明缓冲不足或消费跟不上（检查 overflow_policy）\n");
  }

  // ───────────────────────────────────────────────────────────────────────
  // 步骤 4：查询录到了什么
  //
  // Query 是只读视图，录制结束后（或进行中）都能用。
  // ───────────────────────────────────────────────────────────────────────
  Query query(&timeline);

  std::vector<TopicSummary> topics;
  s = query.ListTopics(&topics);
  if (!s.ok()) {
    std::printf("ListTopics 失败: %s\n", s.ToString().c_str());
    timeline.Close();
    return 1;
  }
  std::printf("[4] 录到 %zu 个话题:\n", topics.size());
  for (const auto& t : topics) {
    std::printf("    %-24s type=%-16s count=%llu\n", t.topic_name.c_str(),
                t.type_name.c_str(),
                static_cast<unsigned long long>(t.message_count));
  }

  SessionMetadata meta;
  if (query.GetInfo(&meta).ok()) {
    std::printf("    会话: 事件总数=%llu 总字节=%llu 格式版本=%u\n",
                static_cast<unsigned long long>(meta.total_events),
                static_cast<unsigned long long>(meta.total_bytes),
                meta.format_version);
  }

  if (topics.empty()) {
    std::printf("没有录到话题，跳过回放。\n");
    timeline.Close();
    return 0;
  }

  // ───────────────────────────────────────────────────────────────────────
  // 步骤 5：回放
  // ───────────────────────────────────────────────────────────────────────
  Player player(&timeline, &backend);

  PlayerConfig pcfg;
  pcfg.rate = 0.0;                  // 0 = 尽可能快；1.0 = 原速；N = N 倍速
  pcfg.discovery_wait_ms = 0;       // 内存后端没有发现过程，无需等待
  pcfg.wait_for_ack = false;
  pcfg.republish_unacked = false;   // 无确认语义时不需要收尾补救

  // 回放回调：在不持有 sink 的前提下观察每条样本。
  // 【注意】回调在回放线程上执行，**不要阻塞**，否则会拖慢回放时序。
  uint64_t seen = 0;
  pcfg.publish_callback = [&seen](const protocol::ReceivedSample& sample) {
    ++seen;
    if (seen <= 3) {
      std::printf("    [replay] seq=%-4u topic=%s\n", sample.sequence,
                  sample.topic_name.c_str());
    }
  };

  TimeRange range;   // 默认 {0,0} = 全范围
  std::printf("[5] 回放中...\n");
  s = player.Play(range, pcfg);
  if (!s.ok()) {
    std::printf("Play 失败: %s\n", s.ToString().c_str());
    timeline.Close();
    return 1;
  }

  const PlaybackProgress prog = player.GetProgress();
  std::printf("    发布=%llu 总数=%llu 进度=%.1f%%\n",
              static_cast<unsigned long long>(prog.published),
              static_cast<unsigned long long>(prog.total), prog.Percent());
  std::printf("    回调共收到 %llu 条\n",
              static_cast<unsigned long long>(seen));

  timeline.Close();
  std::printf("\n完成。下一步建议看 core_full.cpp —— 它用同一套 API 演示三个真实场景。\n");
  return 0;
}
