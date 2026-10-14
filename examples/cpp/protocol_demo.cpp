// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// examples/cpp/protocol_demo.cpp
//
// ─────────────────────────────────────────────────────────────────────────────
// protocol 模块 · 简单用法演示
//
// 演示**如何自己实现一个协议后端**。这是 Stratum 最重要的扩展点：
//
//      你的数据源 ──▶ ISource ──┐
//                               ├──▶ IDdsBackend ──▶ Recorder / Player
//      你的数据去向 ◀── ISink  ──┘
//
// 学完这个文件，你就能把 Stratum 接到任何数据来源上（MQTT / 串口 / 共享内存 /
// 日志回流 / 自研总线），而 Recorder 与 Player 一行都不用改。
//
// 覆盖的 API（protocol/idds_backend.h）：
//   ISource       Start(SampleCallback) / Stop() / IsRunning() / DiscoveredTopics()
//   ISink         Open() / Close() / IsOpen() / Publish() /
//                 ReadyForPublish() / WaitForAcknowledgments()
//   IDdsBackend   Name() / Version() / CreateSource() / CreateSink() / GetCapabilities()
//   BackendConfig domain_id / participant_name / topics / extra
//   ReceivedSample 各字段的含义
//   Capabilities  auto_discovery / provides_sequence / provides_type_def / supports_multicast
//
// 运行：./protocol_demo [时间线目录]
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"
#include "stratum/protocol/idds_backend.h"

using namespace stratum;
using namespace stratum::protocol;

namespace {

TimestampNs NowNs() {
  return static_cast<TimestampNs>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// ═══════════════════════════════════════════════════════════════════════════
// 第 1 步：实现 ISource（数据来源）
//
// 契约要点（这三条最容易踩）：
//   ① Start 之后，回调在**你自己的线程**上被调用 —— 不要假设它跑在调用者线程；
//   ② 回调里**不得阻塞** —— 你一阻塞就把接收路径拖慢了（Stratum 会记为延迟）；
//   ③ 回调返回 false 表示上层要求停止，应当尽快退出循环。
// ═══════════════════════════════════════════════════════════════════════════
class CounterSource final : public ISource {
 public:
  CounterSource(uint32_t n, uint32_t interval_us)
      : n_(n), interval_us_(interval_us) {}

  Status Start(SampleCallback callback) override {
    if (running_.exchange(true)) {
      return Status::AlreadyExists("CounterSource 已启动");
    }
    worker_ = std::thread([this, cb = std::move(callback)]() {
      for (uint32_t i = 0; i < n_ && running_.load(); ++i) {
        // ── 填一个 ReceivedSample ────────────────────────────────────────
        ReceivedSample s;
        // topic_name / type_name 是**必填**：录制目录靠它们组织，
        // 为空会导致后续查询按名字找不到（本项目踩过 type_name 为空导致
        //  发布公告构造失败、对端完全看不到的坑）。
        s.topic_name = "counter/value";
        s.type_name = "CounterValue";
        // 两种时间戳都要填（见 docs/06-data-trust.md）：
        //   source    —— 数据产生方的时钟，回放时序以它为准
        //   reception —— 本地收到的时刻，用于诊断本端延迟
        s.source_timestamp_ns = NowNs();
        s.reception_timestamp_ns = NowNs();
        // sequence 用于缺口检测；后端不提供时置 0（Stratum 会明确记为"无法检测"）
        s.sequence = i + 1;
        s.payload = "value=" + std::to_string(i + 1);
        if (!cb(s)) break;   // 上层要求停止
        if (interval_us_ > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(interval_us_));
        }
      }
      running_.store(false, std::memory_order_release);
    });
    return Status::OK();
  }

  Status Stop() override {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    return Status::OK();
  }

  bool IsRunning() const override { return running_.load(std::memory_order_acquire); }

  // 用于自检与质量报告：本后端"能提供"哪些话题
  std::vector<std::string> DiscoveredTopics() const override {
    return {"counter/value"};
  }

 private:
  uint32_t n_;
  uint32_t interval_us_;
  std::atomic<bool> running_{false};
  std::thread worker_;
};

// ═══════════════════════════════════════════════════════════════════════════
// 第 2 步：实现 ISink（数据去向）
//
// 契约要点：
//   ① Open() 之后才能 Publish()；
//   ② Publish() 返回 OK 只表示"已交给后端"，**不保证对端收到** ——
//      要"保证收到"就得实现 ReadyForPublish() 与 WaitForAcknowledgments()；
//   ③ ReadyForPublish() 是给上层的信号：VOLATILE 语义下，匹配完成前发出的
//      数据对端不会索要（本项目实测回放 200 条时订阅端稳定收到 199 条，
//      缺的恒为第一条，且重发相同 SN 无效）。
// ═══════════════════════════════════════════════════════════════════════════
class PrintingSink final : public ISink {
 public:
  Status Open() override {
    open_ = true;
    std::printf("    [sink] open\n");
    return Status::OK();
  }

  Status Close() override {
    open_ = false;
    std::printf("    [sink] close（共写出 %llu 条）\n",
                static_cast<unsigned long long>(written_));
    return Status::OK();
  }

  bool IsOpen() const override { return open_; }

  Status Publish(const ReceivedSample& sample) override {
    if (!open_) {
      // 明确失败，不要静默丢 —— 静默失败是本项目最大的坑源
      return Status::Internal("sink 未打开就 Publish");
    }
    ++written_;
    if (written_ <= 5) {
      std::printf("    [sink] #%u %s payload=%s\n", sample.sequence,
                  sample.topic_name.c_str(), sample.payload.c_str());
    }
    return Status::OK();
  }

  // 本去向没有发现过程，随时可发
  bool ReadyForPublish() const override { return open_; }

  // 本去向没有确认语义；真实网络后端应在这里等 ACKNACK，
  // 不支持时返回 NotSupported（Stratum 会据此调整行为，而不是当成成功）。
  Status WaitForAcknowledgments(uint64_t /*timeout_ms*/) override {
    return Status::NotSupported("本后端不做确认");
  }

 private:
  bool open_ = false;
  uint64_t written_ = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
// 第 3 步：实现 IDdsBackend（把两者组装起来）
//
// 它只是个工厂 + 能力广告牌。Recorder 录制的方向调 CreateSource，
// Player 回放的方向调 CreateSink。
// ═══════════════════════════════════════════════════════════════════════════
class CounterBackend final : public IDdsBackend {
 public:
  std::string Name() const override { return "counter-demo"; }
  std::string Version() const override { return "1.0.0"; }

  Status CreateSource(const BackendConfig& cfg, SourcePtr* out) override {
    // BackendConfig::extra 是 vector<pair<string,string>>（不是 map），
    // 用于传后端特有参数。用遍历查找，别写 extra["k"]。
    uint32_t n = 10;
    uint32_t interval_us = 1000;
    for (const auto& kv : cfg.extra) {
      if (kv.first == "count") n = static_cast<uint32_t>(std::atoi(kv.second.c_str()));
      else if (kv.first == "interval_us") {
        interval_us = static_cast<uint32_t>(std::atoi(kv.second.c_str()));
      }
    }
    *out = SourcePtr(new CounterSource(n, interval_us));
    return Status::OK();
  }

  Status CreateSink(const BackendConfig& /*cfg*/, SinkPtr* out) override {
    *out = SinkPtr(new PrintingSink());
    return Status::OK();
  }

  // 能力广告牌：上层会据此决定"要不要抱怨没有序号"这类事
  Capabilities GetCapabilities() const override {
    Capabilities c;
    c.auto_discovery = false;      // 话题固定，不能"录制全部"
    c.provides_sequence = true;    // 我们填了 sequence → 可以做缺口检测
    c.provides_type_def = false;   // 没有类型定义
    c.supports_multicast = false;
    return c;
  }
};

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = (argc > 1) ? argv[1] : "/tmp/protocol_demo_tl";

  std::printf("=== protocol_demo：自己实现一个协议后端 ===\n\n");

  TimelineImplV2 timeline;
  Status s = timeline.Open(dir);
  if (!s.ok()) {
    std::printf("打开时间线失败: %s\n", s.ToString().c_str());
    return 1;
  }

  // ── 用我们的后端替代 RtpsBackend，其余代码与真实链路完全一致 ─────────
  CounterBackend backend;
  const auto caps = backend.GetCapabilities();
  std::printf("[1] 后端 %s v%s\n", backend.Name().c_str(), backend.Version().c_str());
  std::printf("    能力: auto_discovery=%d provides_sequence=%d type_def=%d\n",
              caps.auto_discovery ? 1 : 0, caps.provides_sequence ? 1 : 0,
              caps.provides_type_def ? 1 : 0);

  // ── 录制：Recorder 会调 backend.CreateSource() ────────────────────────
  Recorder recorder(&timeline, &backend);
  RecorderConfig rcfg;
  rcfg.close_timeline_on_stop = false;
  rcfg.backend.participant_name = "protocol_demo";   // 便于运维识别
  rcfg.backend.extra.push_back({"count", "30"});
  rcfg.backend.extra.push_back({"interval_us", "1000"});

  s = recorder.Start(rcfg);
  if (!s.ok()) {
    std::printf("Start 失败: %s\n", s.ToString().c_str());
    timeline.Close();
    return 1;
  }

  std::printf("[2] 录制中（30 条）...\n");
  for (int i = 0; i < 200; ++i) {
    if (recorder.GetStats().received >= 30) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  recorder.Stop();
  const Recorder::Stats st = recorder.GetStats();
  std::printf("    received=%llu recorded=%llu dropped=%llu\n",
              static_cast<unsigned long long>(st.received),
              static_cast<unsigned long long>(st.recorded),
              static_cast<unsigned long long>(st.dropped));

  // ── 回放：Player 会调 backend.CreateSink() ────────────────────────────
  Player player(&timeline, &backend);
  PlayerConfig pcfg;
  pcfg.rate = 0.0;
  pcfg.discovery_wait_ms = 0;
  pcfg.wait_for_ack = false;       // 本后端 WaitForAcknowledgments 返回 NotSupported
  pcfg.republish_unacked = false;

  std::printf("[3] 回放中...\n");
  TimeRange range;
  s = player.Play(range, pcfg);
  if (!s.ok()) {
    std::printf("Play 失败: %s\n", s.ToString().c_str());
    timeline.Close();
    return 1;
  }

  const PlaybackProgress p = player.GetProgress();
  std::printf("    发布=%llu 进度=%.1f%%\n",
              static_cast<unsigned long long>(p.published), p.Percent());

  timeline.Close();
  std::printf("\n完成。下一步看 protocol_full.cpp —— 三个真实场景：\n");
  std::printf("  · 接非 DDS 数据源（带背压与丢弃策略）\n");
  std::printf("  · 转发到自定义去向（带确认与重试语义）\n");
  std::printf("  · 能力协商与优雅降级\n");
  return 0;
}
