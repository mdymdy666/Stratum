// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 示例 03：回放控制（区间 / 速率 / 循环 / 进度 / 中途停止）
//
// 展示 Player 的控制面：
//   · 按时间区间回放（而不是整体）
//   · 变速（0 = 尽可能快，1.0 = 原速，N = N 倍速）
//   · 循环回放
//   · 实时读进度（PlaybackProgress）
//   · 从另一个线程请求停止（RequestStop）
//   · 话题名重映射（避免污染实时数据流）
//
// 编译：见 examples/cpp/CMakeLists.txt
// 运行：./03_playback_control <时间线目录> [domain]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"

using namespace stratum;
using namespace stratum::protocol;

namespace {

// 一个最小的内存数据汇：只统计收到了什么，不做任何网络动作。
class CountingSink final : public protocol::ISink {
 public:
  Status Open() override { open_ = true; return Status::OK(); }
  Status Close() override { open_ = false; return Status::OK(); }
  bool IsOpen() const override { return open_; }
  Status Publish(const protocol::ReceivedSample& sample) override {
    ++published_;
    last_topic_ = sample.topic_name;
    return Status::OK();
  }
  bool ReadyForPublish() const override { return open_; }
  Status WaitForAcknowledgments(uint64_t) override { return Status::OK(); }
  uint64_t published() const { return published_; }
  const std::string& last_topic() const { return last_topic_; }

 private:
  bool open_ = false;
  uint64_t published_ = 0;
  std::string last_topic_;
};

class CountingBackend final : public protocol::IDdsBackend {
 public:
  std::string Name() const override { return "counting"; }
  std::string Version() const override { return "1.0.0"; }
  Status CreateSource(const protocol::BackendConfig&,
                      protocol::SourcePtr* out) override {
    return Status::NotSupported("本示例只演示回放");
  }
  Status CreateSink(const protocol::BackendConfig&,
                    protocol::SinkPtr* out) override {
    *out = protocol::SinkPtr(new CountingSink());
    return Status::OK();
  }
  Capabilities GetCapabilities() const override {
    Capabilities c;
    c.provides_sequence = true;
    return c;
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("用法: %s <时间线目录> [domain=0]\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;

  TimelineImplV2 timeline;
  Status s = timeline.Open(dir);
  if (!s.ok()) {
    std::printf("FAILED: timeline.Open: %s\n", s.ToString().c_str());
    return 1;
  }

  Query query(&timeline);
  std::vector<TopicSummary> topics;
  if (!query.ListTopics(&topics).ok() || topics.empty()) {
    std::printf("时间线里没有话题，先用示例 01 录一段。\n");
    timeline.Close();
    return 1;
  }

  const TopicSummary& first = topics.front();
  std::printf("话题 \"%s\"：%llu 条，跨度 %.3f s\n", first.topic_name.c_str(),
              static_cast<unsigned long long>(first.message_count),
              static_cast<double>(first.last_ns - first.first_ns) / 1e9);

  // 本示例演示**控制面**，因此用一个内存数据汇：不需要真实网络与对端匹配，
  // 控制行为（区间/速率/进度/回调）与真实后端完全一致。
  // 要用真实网络回放，把这里换成 stratum::protocol::rtps::RtpsBackend 即可。
  CountingBackend backend;
  Player player(&timeline, &backend);

  protocol::BackendConfig bcfg;
  bcfg.domain_id = domain;
  bcfg.participant_name = "stratum_example_03";
  // 话题名重映射：回放时改名，避免与仍在运行的实时发布者冲突。
  // 注意：映射的 key 是录制时的原话题名。

  PlayerConfig pcfg;
  pcfg.rate = 0.0;                  // 尽可能快（示例里方便观察）
  pcfg.backend = bcfg;
  pcfg.wait_for_ack = false;        // 本示例只关心控制面，不等确认
  pcfg.loop = false;
  pcfg.topic_mapping = {{first.topic_name, first.topic_name + "_replay"}};

  // 统计回调：展示"不持有 sink 所有权也能观察回放内容"
  std::atomic<uint64_t> seen{0};
  std::atomic<uint64_t> bytes{0};
  pcfg.publish_callback = [&seen, &bytes](const protocol::ReceivedSample& smp) {
    seen.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(smp.payload.size(), std::memory_order_relaxed);
  };

  // -------------------------------------------------------------------------
  // 区间回放：只回放中间那一段（前后各跳过 20%）
  // -------------------------------------------------------------------------
  const TimestampNs span = first.last_ns - first.first_ns;
  TimeRange range;
  range.begin_ns = first.first_ns + span / 5;
  range.end_ns = first.last_ns - span / 5;
  std::printf("回放区间: %.3f s .. %.3f s（原区间 %.3f .. %.3f）\n",
              static_cast<double>(range.begin_ns - first.first_ns) / 1e9,
              static_cast<double>(range.end_ns - first.first_ns) / 1e9,
              static_cast<double>(first.first_ns) / 1e9,
              static_cast<double>(first.last_ns) / 1e9);

  // 起一个观察线程：周期性打印进度，并在若干秒后请求停止（演示 RequestStop）
  std::atomic<bool> stop_requested{false};
  std::thread watcher([&player, &stop_requested]() {
    for (int i = 0; i < 60; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      const PlaybackProgress p = player.GetProgress();
      std::printf("  [progress] %.1f%% published=%llu playing=%d\n", p.Percent(),
                  static_cast<unsigned long long>(p.published),
                  player.IsPlaying() ? 1 : 0);
      if (p.finished) break;
    }
    stop_requested.store(true, std::memory_order_release);
  });

  std::printf("开始回放（另一线程观察进度）...\n");
  s = player.Play(range, pcfg);
  watcher.join();

  if (!s.ok()) {
    std::printf("FAILED: player.Play: %s\n", s.ToString().c_str());
    timeline.Close();
    return 1;
  }

  const PlaybackProgress p = player.GetProgress();
  std::printf("\n=== 回放结束 ===\n");
  std::printf("  回调收到 %llu 条 / %llu 字节\n",
              static_cast<unsigned long long>(seen.load()),
              static_cast<unsigned long long>(bytes.load()));
  std::printf("  进度 %.1f%%（published=%llu total=%llu）\n", p.Percent(),
              static_cast<unsigned long long>(p.published),
              static_cast<unsigned long long>(p.total));

  timeline.Close();
  return 0;
}
