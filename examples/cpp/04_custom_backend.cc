// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 示例 04：自定义后端（框架扩展点）
//
// Stratum 的协议层与存储层都是**接口**，换实现不需要改上层：
//
//      Recorder ──▶ IDdsBackend ──▶ ISource ──▶ 你的数据来源
//      Player   ──▶ IDdsBackend ──▶ ISink   ──▶ 你的数据去向
//
// 本示例实现一个**内存后端**：录制时按固定间隔合成样本（模拟传感器），
// 回放时把样本打印出来。它有完整的发现语义（无），因此不需要等待匹配。
//
// 这对以下场景很有用：
//   · 单测：不依赖真实网络即可验证 Recorder/Player 的行为
//   · 自定义协议：接非 DDS 的数据源（MQTT / 串口 / 共享内存）
//   · 数据迁移：把已有格式导入 Stratum
//
// 编译：见 examples/cpp/CMakeLists.txt
// 运行：./04_custom_backend <时间线目录> [样本数]

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

// ---------------------------------------------------------------------------
// 数据源：合成样本
//
// 契约要点（见 idds_backend.h）：
//   · Start(callback) 之后，回调在**后端自己的线程**上被调用；
//   · 回调中**不得阻塞**，否则会拖慢接收；
//   · 回调返回 false 表示"不要再推了"（本实现据此退出）。
// ---------------------------------------------------------------------------
class SyntheticSource final : public protocol::ISource {
 public:
  SyntheticSource(uint32_t count, uint32_t interval_ms)
      : count_(count), interval_ms_(interval_ms) {}

  Status Start(protocol::SampleCallback callback) override {
    if (running_.exchange(true)) {
      return Status::Internal("已经启动");
    }
    thread_ = std::thread([this, cb = std::move(callback)]() {
      for (uint32_t i = 0; i < count_ && running_.load(); ++i) {
        protocol::ReceivedSample s;
        s.topic_name = "synthetic/temperature";
        s.type_name = "SyntheticTemp";
        s.source_timestamp_ns = NowNs();
        s.reception_timestamp_ns = NowNs();
        s.sequence = i + 1;
        // 负载用可辨识内容，便于回放时肉眼确认
        char buf[64];
        std::snprintf(buf, sizeof(buf), "sample-%u", i + 1);
        s.payload.assign(buf);
        if (!cb(s)) break;   // 上层要求停止
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
      }
      running_.store(false);
    });
    return Status::OK();
  }

  Status Stop() override {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    return Status::OK();
  }

  bool IsRunning() const override { return running_.load(); }

  std::vector<std::string> DiscoveredTopics() const override {
    return {"synthetic/temperature"};
  }

 private:
  static TimestampNs NowNs() {
    return static_cast<TimestampNs>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
  }

  uint32_t count_;
  uint32_t interval_ms_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

// ---------------------------------------------------------------------------
// 数据汇：把回放的样本打印出来
//
// 契约要点：
//   · Open() 后才可以 Publish()；
//   · Publish() 返回 OK 只表示"已交给后端"，不保证对端收到；
//   · ReadyForPublish() 用于告知上层"现在发才有意义"
//     （真实网络后端要等发现完成；本实现没有发现过程，直接 true）。
// ---------------------------------------------------------------------------
class PrintingSink final : public protocol::ISink {
 public:
  Status Open() override {
    open_ = true;
    std::printf("  [sink] 已打开\n");
    return Status::OK();
  }
  Status Close() override {
    open_ = false;
    std::printf("  [sink] 已关闭（共发布 %llu 条）\n",
                static_cast<unsigned long long>(published_));
    return Status::OK();
  }
  bool IsOpen() const override { return open_; }

  Status Publish(const protocol::ReceivedSample& sample) override {
    if (!open_) return Status::Internal("sink 未打开");
    ++published_;
    std::printf("  [sink] #%u topic=%s payload=%s\n", sample.sequence,
                sample.topic_name.c_str(), sample.payload.c_str());
    return Status::OK();
  }

  bool ReadyForPublish() const override { return open_; }

  // WaitForAcknowledgments 是 ISink 的纯虚函数。
  // 本后端没有确认语义（写出去就算完），因此直接返回成功。
  // 真实网络后端应在这里等待对端 ACKNACK，或返回 NotSupported。
  Status WaitForAcknowledgments(uint64_t /*timeout_ms*/) override {
    return Status::OK();
  }

 private:
  bool open_ = false;
  uint64_t published_ = 0;
};

// ---------------------------------------------------------------------------
// 后端：把上面两个组合起来
// ---------------------------------------------------------------------------
class SyntheticBackend final : public protocol::IDdsBackend {
 public:
  std::string Name() const override { return "synthetic"; }
  std::string Version() const override { return "1.0.0"; }

  Status CreateSource(const protocol::BackendConfig& config,
                      protocol::SourcePtr* out) override {
    // 从 extra 里读参数，这样调用方不用改后端签名就能传自定义配置
    uint32_t count = 20;
    uint32_t interval = 50;
    // extra 是 vector<pair<string,string>>（不是 map），按序遍历即可
    for (const auto& kv : config.extra) {
      if (kv.first == "count") count = static_cast<uint32_t>(std::atoi(kv.second.c_str()));
      else if (kv.first == "interval_ms") interval = static_cast<uint32_t>(std::atoi(kv.second.c_str()));
    }

    *out = SourcePtr(new SyntheticSource(count, interval));
    return Status::OK();
  }

  Status CreateSink(const protocol::BackendConfig& /*config*/,
                    protocol::SinkPtr* out) override {
    *out = SinkPtr(new PrintingSink());
    return Status::OK();
  }

  Capabilities GetCapabilities() const override {
    Capabilities c;
    c.auto_discovery = false;      // 话题是固定的
    c.provides_sequence = true;    // 我们填了 sequence，缺口检测可用
    c.provides_type_def = false;   // 没有类型定义
    c.supports_multicast = false;
    return c;
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("用法: %s <时间线目录> [样本数=20]\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const int count = (argc > 2) ? std::atoi(argv[2]) : 20;

  TimelineImplV2 timeline;
  Status s = timeline.Open(dir);
  if (!s.ok()) {
    std::printf("FAILED: timeline.Open(%s): %s\n", dir.c_str(),
                s.ToString().c_str());
    return 1;
  }

  // 关键：这里传的是我们自己的后端，而不是 RtpsBackend。
  // 上层（Recorder / Player）完全不知道底下换成了什么。
  SyntheticBackend backend;
  std::printf("后端: %s v%s\n", backend.Name().c_str(), backend.Version().c_str());
  const auto caps = backend.GetCapabilities();
  std::printf("能力: auto_discovery=%d sequence=%d type_def=%d multicast=%d\n",
              caps.auto_discovery ? 1 : 0, caps.provides_sequence ? 1 : 0,
              caps.provides_type_def ? 1 : 0, caps.supports_multicast ? 1 : 0);

  // ---- 录制 ----
  Recorder recorder(&timeline, &backend);
  RecorderConfig rcfg;
  rcfg.close_timeline_on_stop = false;
  rcfg.backend.domain_id = 0;             // 本后端不用，但保持字段语义完整
  rcfg.backend.extra.push_back({"count", std::to_string(count)});
  rcfg.backend.extra.push_back({"interval_ms", "20"});

  s = recorder.Start(rcfg);
  if (!s.ok()) {
    std::printf("FAILED: recorder.Start: %s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("\n录制中（合成 %d 条）...\n", count);
  while (recorder.GetStats().recorded < static_cast<uint64_t>(count)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto st = recorder.GetStats();
    if (st.received >= static_cast<uint64_t>(count)) break;
  }
  recorder.Stop();
  const auto st = recorder.GetStats();
  std::printf("录制完成: received=%llu recorded=%llu\n",
              static_cast<unsigned long long>(st.received),
              static_cast<unsigned long long>(st.recorded));

  // ---- 查询 ----
  Query query(&timeline);
  std::vector<TopicSummary> topics;
  query.ListTopics(&topics);
  std::printf("\n录到 %zu 个话题:\n", topics.size());
  for (const auto& t : topics) {
    std::printf("  %s  count=%llu\n", t.topic_name.c_str(),
                static_cast<unsigned long long>(t.message_count));
  }

  // ---- 回放 ----
  Player player(&timeline, &backend);
  PlayerConfig pcfg;
  pcfg.rate = 0.0;             // 尽快
  pcfg.discovery_wait_ms = 0;  // 本后端没有发现过程
  pcfg.wait_for_ack = false;
  pcfg.republish_unacked = false;

  std::printf("\n=== 回放（经自定义 Sink）===\n");
  TimeRange range;
  s = player.Play(range, pcfg);
  if (!s.ok()) {
    std::printf("FAILED: player.Play: %s\n", s.ToString().c_str());
    timeline.Close();
    return 1;
  }

  timeline.Close();
  std::printf("\n完成。自定义后端无需改动 Recorder/Player 任何代码。\n");
  return 0;
}
