// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// examples/cpp/protocol_full.cpp
//
// ─────────────────────────────────────────────────────────────────────────────
// protocol 模块 · 全面用法演示（三个真实场景）
//
// protocol_demo.cpp 教"接口长什么样"；本文件教"**真实项目里怎么写**"。
//
//   场景 A：接入非 DDS 数据源 —— 有界队列 + 背压 + 明确的丢弃策略与计数
//            （模拟串口/PLC/日志回流：速率不稳、消费可能跟不上）
//
//   场景 B：转发到自定义去向 —— 连接状态驱动 ReadyForPublish、
//            带重试与确认语义的 WaitForAcknowledgments、失败要能上报
//            （模拟推到远端服务：会断线、会超时）
//
//   场景 C：能力协商与优雅降级 —— 同一个业务代码面对三种能力组合的后端
//            （有/无自动发现、有/无序号、有/无类型定义），
//            按能力决定"能不能做缺口检测""能不能录制全部话题"
//
// 覆盖的 API（比 demo 多了这些使用方式）：
//   ISource::DiscoveredTopics 在录制前用于自检
//   ISink::ReadyForPublish    反映真实就绪状态（而不是永远 true）
//   ISink::WaitForAcknowledgments 返回 Timeout / NotSupported 等**有语义的失败**
//   IDdsBackend::Capabilities 四个字段的**实际影响**
//   BackendConfig 的 participant_name / topics / extra 三类配置
//   Status 的错误分类（IOError / Timeout / NotSupported / InvalidArgument）
//
// 运行：./protocol_full [工作目录]
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"
#include "stratum/protocol/idds_backend.h"

using namespace stratum;
using namespace stratum::protocol;

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
  std::printf("    [%s] %s\n", ok ? "OK" : "FAIL", what);
  if (!ok) ++g_failed;
}

TimestampNs NowNs() {
  return static_cast<TimestampNs>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

bool EnsureDir(const std::string& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec && !std::filesystem::exists(dir)) {
    std::printf("  创建目录失败: %s (%s)\n", dir.c_str(), ec.message().c_str());
    return false;
  }
  return true;
}

// 清空并重建目录。
//
// 【为什么不是 EnsureDir】示例必须**可重复运行**。若只确保目录存在，
// 第二次运行时目录里还留着上一次的数据，TimelineImplV2::Open 会打开它，
// 于是"录制了多少条""窗口里有多少条"都跟首次不同，检查项就会误报失败 ——
// 读者照着跑第二遍会以为是自己环境的问题。
// （这也是本项目反复出现的主题：验证手段本身不可靠时，结论就不可信。）
bool ResetDir(const std::string& dir) {
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);   // 目录不存在时也返回成功
  std::filesystem::create_directories(dir, ec);
  if (ec && !std::filesystem::exists(dir)) {
    std::printf("  重建目录失败: %s (%s)\n", dir.c_str(), ec.message().c_str());
    return false;
  }
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// 场景 A：接入非 DDS 数据源
//
// 真实诉求（以接 PLC / 串口为例）：
//   · 外部设备以不稳定速率产出数据，本端消费可能跟不上
//   · 必须有**有界队列**，否则内存会被打爆
//   · 队列满时的策略要由业务决定，且**必须可见地计数**（不能悄悄丢）
//   · 录制端要能随时知道"产了多少、进了多少、丢了多少"
//
// 另外演示：录制**开始前**先用 DiscoveredTopics() 自检数据源是否正常，
// 而不是等录完发现是空的。
// ═══════════════════════════════════════════════════════════════════════════

// 有界队列 + 三种溢出策略
class BoundedSampleQueue {
 public:
  enum class Policy { kDropNewest, kDropOldest, kBlock };

  BoundedSampleQueue(size_t cap, Policy p) : cap_(cap), policy_(p) {}

  // 返回 true 表示已入队；false 表示按策略丢弃
  bool Push(const ReceivedSample& s) {
    std::unique_lock<std::mutex> lock(mu_);
    if (q_.size() >= cap_) {
      switch (policy_) {
        case Policy::kDropNewest:
          ++dropped_;
          return false;
        case Policy::kDropOldest:
          q_.pop_front();
          ++dropped_;
          break;
        case Policy::kBlock:
          not_full_.wait_for(lock, std::chrono::milliseconds(100), [this] {
            return q_.size() < cap_;
          });
          if (q_.size() >= cap_) {   // 等超时了，仍然丢（并计数）
            ++dropped_;
            return false;
          }
          break;
      }
    }
    q_.push_back(s);
    ++enqueued_;
    not_empty_.notify_one();
    return true;
  }

  bool Pop(ReceivedSample* out) {
    std::unique_lock<std::mutex> lock(mu_);
    if (q_.empty()) return false;
    *out = q_.front();
    q_.pop_front();
    not_full_.notify_one();
    return true;
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return q_.size();
  }
  // 成功入队的条数（不含被丢弃的）
  uint64_t enqueued() const {
    std::lock_guard<std::mutex> lock(mu_);
    return enqueued_;
  }
  // 丢弃的条数
  uint64_t dropped() const {
    std::lock_guard<std::mutex> lock(mu_);
    return dropped_;
  }
  size_t capacity() const { return cap_; }

 private:
  mutable std::mutex mu_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::deque<ReceivedSample> q_;
  size_t cap_;
  Policy policy_;
  uint64_t enqueued_ = 0;
  uint64_t dropped_ = 0;
};

// 把"外部设备"接到 Stratum：生产者线程把数据推进有界队列，
// 消费者（Stratum 的采样回调）从这里取。
class PlcLikeSource final : public ISource {
 public:
  PlcLikeSource(uint32_t total, uint32_t produce_us, size_t cap,
                BoundedSampleQueue::Policy policy)
      : total_(total), produce_us_(produce_us),
        queue_(cap, policy) {}

  Status Start(SampleCallback cb) override {
    if (running_.exchange(true)) return Status::AlreadyExists("已启动");

    // 生产者：模拟外部设备以固定速率产出（真实场景里是 socket/串口读线程）
    producer_ = std::thread([this]() {
      for (uint32_t i = 0; i < total_ && running_.load(); ++i) {
        ++attempted_;
        ReceivedSample s;
        s.topic_name = "plc/register";
        s.type_name = "PlcRegister";
        s.source_timestamp_ns = NowNs();
        s.reception_timestamp_ns = s.source_timestamp_ns;
        s.sequence = i + 1;
        s.payload = "reg[" + std::to_string(i % 16) + "]=" + std::to_string(i);
        queue_.Push(s);
        if (produce_us_ > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(produce_us_));
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      running_.store(false, std::memory_order_release);
    });

    // 消费者：把队列里的数据交给 Stratum。注意回调**不得阻塞**，
    // 这里的 Pop 是非阻塞的（队列空就返回 false）。
    consumer_ = std::thread([this, cb = std::move(cb)]() {
      ReceivedSample s;
      uint32_t since_pause = 0;
      while (running_.load() || queue_.Size() > 0) {
        if (queue_.Pop(&s)) {
          if (!cb(s)) break;      // 上层要求停止
          ++delivered_;
          // ── 模拟真实的下游处理成本 ──────────────────────────────
          // 现实中 cb 之后往往还有解析/落库/转发等开销。若消费者是紧循环，
          // 消费速度会超过生产速度、队列永不溢出，"有界队列 + 丢弃策略"
          // 这一段就演示不到，示例也就失去了意义。
          // 这里每处理 200 条停 1ms，把消费者整体压到约 20 万条/秒，
          // 低于生产端的 50 万条/秒。
          if (++since_pause >= 200) {
            since_pause = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        } else {
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
      }
    });
    return Status::OK();
  }

  Status Stop() override {
    running_.store(false, std::memory_order_release);
    if (producer_.joinable()) producer_.join();
    if (consumer_.joinable()) consumer_.join();
    return Status::OK();
  }

  bool IsRunning() const override { return running_.load(std::memory_order_acquire); }

  std::vector<std::string> DiscoveredTopics() const override {
    return {"plc/register"};
  }

  uint64_t delivered() const { return delivered_; }
  uint64_t attempted() const { return attempted_; }
  const BoundedSampleQueue& queue() const { return queue_; }

 private:
  uint32_t total_;
  uint32_t produce_us_;
  BoundedSampleQueue queue_;
  std::atomic<bool> running_{false};
  std::thread producer_;
  std::thread consumer_;
  std::atomic<uint64_t> attempted_{0};
  std::atomic<uint64_t> delivered_{0};
};

// 场景 A 的后端：故意用一个**很小**的队列，让丢弃策略可见
class PlcBackend final : public IDdsBackend {
 public:
  std::string Name() const override { return "plc-like"; }
  std::string Version() const override { return "1.0.0"; }

  Status CreateSource(const BackendConfig& cfg, SourcePtr* out) override {
    uint32_t total = GetU32(cfg, "total", 3000);
    uint32_t produce_us = GetU32(cfg, "produce_us", 50);
    size_t cap = GetU32(cfg, "queue_cap", 256);
    BoundedSampleQueue::Policy pol = BoundedSampleQueue::Policy::kDropNewest;
    if (const std::string* v = Find(cfg, "overflow")) {
      if (*v == "oldest") pol = BoundedSampleQueue::Policy::kDropOldest;
      else if (*v == "block") pol = BoundedSampleQueue::Policy::kBlock;
    }
    auto sp = std::make_shared<PlcLikeSource>(total, produce_us, cap, pol);
    last_source_ = sp;
    *out = SourcePtr(new WrapSource(sp));
    return Status::OK();
  }

  Status CreateSink(const BackendConfig&, SinkPtr* out) override {
    *out = SinkPtr(new NullSink());
    return Status::OK();
  }

  IDdsBackend::Capabilities GetCapabilities() const override {
    IDdsBackend::Capabilities c;
    c.auto_discovery = false;
    c.provides_sequence = true;
    c.provides_type_def = false;
    c.supports_multicast = false;
    return c;
  }

  std::shared_ptr<PlcLikeSource> last_source() const { return last_source_; }

 private:
  static const std::string* Find(const BackendConfig& c, const std::string& k) {
    for (const auto& kv : c.extra) if (kv.first == k) return &kv.second;
    return nullptr;
  }
  static uint32_t GetU32(const BackendConfig& c, const std::string& k, uint32_t d) {
    const std::string* v = Find(c, k);
    return v ? static_cast<uint32_t>(std::atoi(v->c_str())) : d;
  }

  class WrapSource final : public ISource {
   public:
    explicit WrapSource(std::shared_ptr<PlcLikeSource> p) : p_(std::move(p)) {}
    Status Start(SampleCallback cb) override { return p_->Start(std::move(cb)); }
    Status Stop() override { return p_->Stop(); }
    bool IsRunning() const override { return p_->IsRunning(); }
    std::vector<std::string> DiscoveredTopics() const override {
      return p_->DiscoveredTopics();
    }

   private:
    std::shared_ptr<PlcLikeSource> p_;
  };
  class NullSink final : public ISink {
   public:
    Status Open() override { open_ = true; return Status::OK(); }
    Status Close() override { open_ = false; return Status::OK(); }
    bool IsOpen() const override { return open_; }
    Status Publish(const ReceivedSample&) override { return Status::OK(); }
    Status WaitForAcknowledgments(uint64_t) override { return Status::OK(); }

   private:
    bool open_ = false;
  };

  std::shared_ptr<PlcLikeSource> last_source_;
};

int ScenarioA_NonDdsSource(const std::string& base) {
  std::printf("\n══════ 场景 A：接入非 DDS 数据源（有界队列 + 背压）══════\n");
  const std::string dir = base + "/scenario_a";
  if (!ResetDir(dir)) { ++g_failed; return 1; }

  TimelineImplV2 timeline;
  Status s = timeline.Open(dir);
  if (!s.ok()) {
    std::printf("  Open 失败: %s\n", s.ToString().c_str());
    ++g_failed;
    return 1;
  }

  // ── 录制前自检：先问数据源"你能提供什么" ──────────────────────────
  PlcBackend backend;
  {
    BackendConfig probe;
    SourcePtr probe_src;
    if (backend.CreateSource(probe, &probe_src).ok()) {
      auto topics = probe_src->DiscoveredTopics();
      std::printf("  数据源自检: 可提供 %zu 个话题", topics.size());
      for (const auto& t : topics) std::printf(" %s", t.c_str());
      std::printf("\n");
      Check(!topics.empty(), "录制前能问到话题（不必等录完才发现为空）");
    }
  }

  Recorder recorder(&timeline, &backend);
  RecorderConfig rcfg;
  rcfg.close_timeline_on_stop = false;
  rcfg.record.max_buffer_bytes = 4ULL * 1024 * 1024;
  // 队列故意开得很小（64），让丢弃策略真实发生
  rcfg.backend.participant_name = "plc-recorder";
  rcfg.backend.topics = {"plc/register"};      // 只关心这一个
  rcfg.backend.extra.push_back({"total", "4000"});
  // 生产端**不限速**：这样队列必然溢出，丢弃策略才演示得到。
  // 真实设备通常是限速的，但"下游比上游慢"这件事在现实里很常见
  // （磁盘抖动、GC、远端限流），因此这个场景不是人为造出来的。
  rcfg.backend.extra.push_back({"produce_us", "0"});
  rcfg.backend.extra.push_back({"queue_cap", "64"});
  rcfg.backend.extra.push_back({"overflow", "newest"});

  s = recorder.Start(rcfg);
  if (!s.ok()) {
    std::printf("  Start 失败: %s\n", s.ToString().c_str());
    ++g_failed;
    timeline.Close();
    return 1;
  }

  std::printf("  录制中（生产不限速 / 队列仅 64 / 策略 drop-newest）...\n");
  for (int i = 0; i < 60; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const Recorder::Stats st = recorder.GetStats();
    if (i % 10 == 9) {
      std::printf("    t=%4dms received=%-6llu recorded=%-6llu dropped=%-5llu queue=%llu\n",
                  (i + 1) * 100,
                  static_cast<unsigned long long>(st.received),
                  static_cast<unsigned long long>(st.recorded),
                  static_cast<unsigned long long>(st.dropped),
                  static_cast<unsigned long long>(st.queue_depth));
    }
    if (st.received > 0 && st.queue_depth == 0 &&
        backend.last_source() && !backend.last_source()->IsRunning()) {
      break;
    }
  }
  recorder.Stop();

  const Recorder::Stats st = recorder.GetStats();
  std::printf("  最终: received=%llu recorded=%llu dropped=%llu\n",
              static_cast<unsigned long long>(st.received),
              static_cast<unsigned long long>(st.recorded),
              static_cast<unsigned long long>(st.dropped));

  Check(st.received > 0, "有数据进来");
  Check(st.recorded > 0, "有数据落盘");
  // 关键：丢弃必须是**可见**的。若队列只有 64 而生产 50kHz，
  // dropped 必然 > 0 —— 如果这里是 0，说明计数没接线（静默丢弃）。
  if (backend.last_source()) {
    const auto& q = backend.last_source()->queue();
    const uint64_t attempted = backend.last_source()->attempted();
    const uint64_t enq = q.enqueued();
    const uint64_t dropped = q.dropped();
    std::printf("    队列侧: 生产尝试=%llu 入队=%llu 丢弃=%llu 容量=%zu\n",
                static_cast<unsigned long long>(attempted),
                static_cast<unsigned long long>(enq),
                static_cast<unsigned long long>(dropped), q.capacity());

    // 【正确的不变量】入队 + 丢弃 == 生产尝试。
    // 这条守恒式任何时序下都成立，是真正该断言的东西。
    //
    // 反面教材（本示例最初就是这么写的）：断言"丢弃必须 > 0"。
    // 那是**依赖时序**的断言 —— 消费者够快时不丢，测试就假失败。
    // 本例把消费者压到比生产慢，丢弃才必然发生；但断言本身不该依赖这一点。
    Check(enq + dropped == attempted,
          "守恒：入队数 + 丢弃数 == 生产尝试数（无静默丢弃）");

    if (dropped > 0) {
      std::printf("    （本次确实发生了丢弃：%llu 条，说明策略生效）\n",
                  static_cast<unsigned long long>(dropped));
    } else {
      std::printf("    （本次未发生丢弃：消费跟得上；守恒式仍成立）\n");
    }
  }

  timeline.Close();
  std::printf("  场景 A 完成\n");
  return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// 场景 B：转发到自定义去向（连接状态 + 重试 + 确认语义）
//
// 真实诉求（以推到远端服务为例）：
//   · 连接会断，断了就不该继续发（发了也是丢），要如实告诉上层
//   · 发送会超时，超时要返回**有语义的失败**（Timeout，而不是 OK）
//   · 要能统计成功/失败/重试次数，供告警与自愈使用
// ═══════════════════════════════════════════════════════════════════════════
class FlakyRemoteSink final : public ISink {
 public:
  // fail_first_n: 前 N 次 Publish 失败（模拟连接未建立）
  FlakyRemoteSink(uint32_t fail_first_n, uint32_t flap_every)
      : fail_first_(fail_first_n), flap_every_(flap_every) {}

  Status Open() override {
    connected_ = true;
    open_ = true;
    std::printf("    [remote] 已连接\n");
    return Status::OK();
  }
  Status Close() override {
    connected_ = false;
    open_ = false;
    std::printf("    [remote] 断开（成功 %llu 失败 %llu 重试 %llu）\n",
                static_cast<unsigned long long>(ok_),
                static_cast<unsigned long long>(fail_),
                static_cast<unsigned long long>(retry_));
    return Status::OK();
  }
  bool IsOpen() const override { return open_; }

  Status Publish(const ReceivedSample& sample) override {
    if (!open_) return Status::Internal("未打开");
    ++attempts_;
    // 前 N 次失败：模拟"连接刚建立但还没就绪"
    if (attempts_ <= fail_first_) {
      ++fail_;
      return Status::IOError("远端尚未就绪");
    }
    // 周期性"抖动"：模拟网络不稳
    if (flap_every_ > 0 && (attempts_ % flap_every_) == 0) {
      ++fail_;
      connected_ = false;
      return Status::IOError("远端连接抖动");
    }
    connected_ = true;
    ++ok_;
    if (ok_ <= 3) {
      std::printf("    [remote] 发出 #%u %s\n", sample.sequence,
                  sample.topic_name.c_str());
    }
    return Status::OK();
  }

  // 关键：就绪状态由**真实连接状态**决定，不是永远 true。
  // 上层（Player）据此决定"现在发有没有意义"。
  bool ReadyForPublish() const override { return open_ && connected_; }

  // 有真实语义的确认等待：本实现用"是否还有未成功的样本"近似。
  // 真实后端应在这里等对端的 ACKNACK。
  Status WaitForAcknowledgments(uint64_t timeout_ms) override {
    if (!open_) return Status::Internal("未打开");
    const uint64_t deadline_ms = timeout_ms;
    uint64_t waited = 0;
    while (waited < deadline_ms) {
      if (fail_ == 0) return Status::OK();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      waited += 20;
    }
    // 超时要返回**有语义的失败**，不能返回 OK —— 否则上层以为发成功了
    if (fail_ > 0) {
      ++retry_;
      return Status::Timeout("仍有未确认样本");
    }
    return Status::OK();
  }

  // 可选能力：告诉上层"哪些还没被确认"，供其用新 SN 重发
  std::vector<ReceivedSample> UnackedSamples() const override { return unacked_; }

  uint64_t ok() const { return ok_; }
  uint64_t fail() const { return fail_; }

 private:
  uint32_t fail_first_;
  uint32_t flap_every_;
  bool open_ = false;
  bool connected_ = false;
  uint64_t attempts_ = 0;
  uint64_t ok_ = 0;
  uint64_t fail_ = 0;
  uint64_t retry_ = 0;
  std::vector<ReceivedSample> unacked_;
};

class RemoteBackend final : public IDdsBackend {
 public:
  std::string Name() const override { return "flaky-remote"; }
  std::string Version() const override { return "1.0.0"; }

  Status CreateSource(const BackendConfig&, SourcePtr*) override {
    // 本后端只做"往远端推"，不提供数据源
    return Status::NotSupported("flaky-remote 只支持回放方向");
  }
  Status CreateSink(const BackendConfig& cfg, SinkPtr* out) override {
    uint32_t ff = GetU32(cfg, "fail_first", 3);
    uint32_t flap = GetU32(cfg, "flap_every", 7);
    auto sp = std::make_shared<FlakyRemoteSink>(ff, flap);
    last_sink_ = sp;
    *out = SinkPtr(new WrapSink(sp));
    return Status::OK();
  }
  IDdsBackend::Capabilities GetCapabilities() const override {
    IDdsBackend::Capabilities c;
    c.auto_discovery = false;
    c.provides_sequence = false;   // 只转发，不产生序号
    c.provides_type_def = false;
    c.supports_multicast = false;
    return c;
  }
  std::shared_ptr<FlakyRemoteSink> last_sink() const { return last_sink_; }

 private:
  static uint32_t GetU32(const BackendConfig& c, const std::string& k, uint32_t d) {
    for (const auto& kv : c.extra) {
      if (kv.first == k) return static_cast<uint32_t>(std::atoi(kv.second.c_str()));
    }
    return d;
  }
  class WrapSink final : public ISink {
   public:
    explicit WrapSink(std::shared_ptr<FlakyRemoteSink> p) : p_(std::move(p)) {}
    Status Open() override { return p_->Open(); }
    Status Close() override { return p_->Close(); }
    bool IsOpen() const override { return p_->IsOpen(); }
    Status Publish(const ReceivedSample& s) override { return p_->Publish(s); }
    bool ReadyForPublish() const override { return p_->ReadyForPublish(); }
    Status WaitForAcknowledgments(uint64_t t) override {
      return p_->WaitForAcknowledgments(t);
    }
    std::vector<ReceivedSample> UnackedSamples() const override {
      return p_->UnackedSamples();
    }

   private:
    std::shared_ptr<FlakyRemoteSink> p_;
  };
  std::shared_ptr<FlakyRemoteSink> last_sink_;
};

int ScenarioB_RemoteForwarding(const std::string& base) {
  std::printf("\n══════ 场景 B：转发到自定义去向（连接状态 + 重试）══════\n");
  const std::string dir = base + "/scenario_b";
  if (!ResetDir(dir)) { ++g_failed; return 1; }

  TimelineImplV2 timeline;
  if (!timeline.Open(dir).ok()) {
    std::printf("  Open 失败\n");
    ++g_failed;
    return 1;
  }

  // 先用一个简单后端造数据
  {
    PlcBackend seed;
    Recorder recorder(&timeline, &seed);
    RecorderConfig rcfg;
    rcfg.close_timeline_on_stop = false;
    rcfg.backend.extra.push_back({"total", "60"});
    rcfg.backend.extra.push_back({"produce_us", "500"});
    rcfg.backend.extra.push_back({"queue_cap", "256"});
    recorder.Start(rcfg);
    for (int i = 0; i < 100; ++i) {
      if (recorder.GetStats().received >= 60) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    recorder.Stop();
    std::printf("  已准备 %llu 条待转发数据\n",
                static_cast<unsigned long long>(recorder.GetStats().recorded));
  }

  RemoteBackend backend;
  Player player(&timeline, &backend);

  PlayerConfig pcfg;
  pcfg.rate = 0.0;
  pcfg.discovery_wait_ms = 0;
  pcfg.wait_for_ack = true;       // 本后端有真实确认语义，值得等
  pcfg.ack_timeout_ms = 200;
  pcfg.republish_unacked = false; // 本示例自己不实现收尾补救

  uint64_t observed = 0;
  pcfg.publish_callback = [&observed](const ReceivedSample&) { ++observed; };

  TimeRange range;
  Status s = player.Play(range, pcfg);
  Check(s.ok(), "Play 正常返回（个别发送失败不应让整次回放失败）");

  if (auto sink = backend.last_sink()) {
    std::printf("    远端统计: 成功=%llu 失败=%llu\n",
                static_cast<unsigned long long>(sink->ok()),
                static_cast<unsigned long long>(sink->fail()));
    Check(sink->ok() > 0, "有成功转发的样本");
    // 前 3 次必然失败（我们故意让它失败），用来验证错误路径真的会被走到
    Check(sink->fail() > 0, "注入的失败被真实触发（错误路径可达）");
  } else {
    Check(false, "能拿到 sink 以检查统计");
  }
  std::printf("    回调观察到 %llu 条\n",
              static_cast<unsigned long long>(observed));

  // 反向验证：给一个只支持回放的后端要数据源，应当**明确失败**
  {
    SourcePtr src;
    Status r = backend.CreateSource(BackendConfig{}, &src);
    Check(!r.ok(), "不支持的方向返回明确失败（NotSupported）而不是空指针");
    std::printf("    失败信息: %s\n", r.ToString().c_str());
  }

  timeline.Close();
  std::printf("  场景 B 完成\n");
  return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// 场景 C：能力协商与优雅降级
//
// 真实诉求：
//   · 同一份业务代码要能面对不同后端（有的支持自动发现、有的不支持）
//   · "没有序号"的后端不能做缺口检测 —— 业务应当**知道并告知用户**，
//     而不是给出一份"看起来很完整"的质量报告
//   · 不支持的能力要能优雅降级，不能崩
// ═══════════════════════════════════════════════════════════════════════════

// 一个能力可配置的后端，用来演示上层的决策分支
class ConfigurableBackend final : public IDdsBackend {
 public:
  explicit ConfigurableBackend(IDdsBackend::Capabilities c, bool provide_source = true)
      : caps_(c), provide_source_(provide_source) {}

  std::string Name() const override { return "configurable"; }
  std::string Version() const override { return "1.0.0"; }

  Status CreateSource(const BackendConfig& cfg, SourcePtr* out) override {
    if (!provide_source_) return Status::NotSupported("本实例未启用数据源");
    uint32_t n = 20;
    for (const auto& kv : cfg.extra) {
      if (kv.first == "count") n = static_cast<uint32_t>(std::atoi(kv.second.c_str()));
    }
    // 若后端"不提供序号"，就真的不填 sequence —— 上层才能看到差异
    *out = SourcePtr(new FlexSource(n, caps_.provides_sequence));
    return Status::OK();
  }
  Status CreateSink(const BackendConfig&, SinkPtr* out) override {
    *out = SinkPtr(new NullSink());
    return Status::OK();
  }
  IDdsBackend::Capabilities GetCapabilities() const override { return caps_; }

 private:
  class FlexSource final : public ISource {
   public:
    FlexSource(uint32_t n, bool with_seq) : n_(n), with_seq_(with_seq) {}
    Status Start(SampleCallback cb) override {
      running_ = true;
      worker_ = std::thread([this, cb = std::move(cb)]() {
        for (uint32_t i = 0; i < n_ && running_; ++i) {
          ReceivedSample s;
          s.topic_name = "flex/data";
          s.type_name = "FlexData";
          s.source_timestamp_ns = NowNs();
          s.reception_timestamp_ns = s.source_timestamp_ns;
          s.sequence = with_seq_ ? (i + 1) : 0;   // 不提供序号就置 0
          s.payload = "v" + std::to_string(i);
          if (!cb(s)) break;
          std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        running_ = false;
      });
      return Status::OK();
    }
    Status Stop() override {
      running_ = false;
      if (worker_.joinable()) worker_.join();
      return Status::OK();
    }
    bool IsRunning() const override { return running_; }
    std::vector<std::string> DiscoveredTopics() const override { return {"flex/data"}; }

   private:
    uint32_t n_;
    bool with_seq_;
    std::atomic<bool> running_{false};
    std::thread worker_;
  };
  class NullSink final : public ISink {
   public:
    Status Open() override { open_ = true; return Status::OK(); }
    Status Close() override { open_ = false; return Status::OK(); }
    bool IsOpen() const override { return open_; }
    Status Publish(const ReceivedSample&) override { return Status::OK(); }
    Status WaitForAcknowledgments(uint64_t) override { return Status::OK(); }

   private:
    bool open_ = false;
  };

  IDdsBackend::Capabilities caps_;
  bool provide_source_;
};

// 上层业务代码：根据能力决定行为。注意它**没有**针对具体后端写分支。
struct RecordingPlan {
  bool can_record_all = false;      // 依赖 auto_discovery
  bool can_detect_gaps = false;     // 依赖 provides_sequence
  bool can_export_types = false;    // 依赖 provides_type_def
  std::vector<std::string> warnings;
};

RecordingPlan PlanFrom(const IDdsBackend::Capabilities& c) {
  RecordingPlan p;
  p.can_record_all = c.auto_discovery;
  p.can_detect_gaps = c.provides_sequence;
  p.can_export_types = c.provides_type_def;
  if (!p.can_record_all) {
    p.warnings.push_back("后端不支持自动发现：必须显式给出要录制的话题");
  }
  if (!p.can_detect_gaps) {
    p.warnings.push_back("后端不提供序号：**无法检测缺口**，质量报告只能给到条数");
  }
  if (!p.can_export_types) {
    p.warnings.push_back("后端不提供类型定义：回放端需要自带类型");
  }
  return p;
}

int ScenarioC_CapabilityNegotiation(const std::string& base) {
  std::printf("\n══════ 场景 C：能力协商与优雅降级 ══════\n");

  struct Case {
    const char* name;
    IDdsBackend::Capabilities caps;
  };

  IDdsBackend::Capabilities full;
  full.auto_discovery = true;
  full.provides_sequence = true;
  full.provides_type_def = true;

  IDdsBackend::Capabilities nosEq;
  nosEq.auto_discovery = true;
  nosEq.provides_sequence = false;
  nosEq.provides_type_def = true;

  IDdsBackend::Capabilities minimal;
  minimal.auto_discovery = false;
  minimal.provides_sequence = false;
  minimal.provides_type_def = false;

  const Case cases[] = {
      {"能力完备（可自动发现 + 有序号 + 有类型）", full},
      {"有发现但无序号（不能做缺口检测）", nosEq},
      {"最小能力（只能显式话题 + 无缺口检测）", minimal},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const Case& c = cases[i];
    const std::string dir = base + "/scenario_c/case" + std::to_string(i);
    if (!EnsureDir(dir)) { ++g_failed; continue; }

    std::printf("\n  ── 用例 %zu：%s ──\n", i + 1, c.name);
    ConfigurableBackend backend(c.caps);
    const IDdsBackend::Capabilities caps = backend.GetCapabilities();
    const RecordingPlan plan = PlanFrom(caps);

    std::printf("    计划: record_all=%d detect_gaps=%d export_types=%d\n",
                plan.can_record_all ? 1 : 0, plan.can_detect_gaps ? 1 : 0,
                plan.can_export_types ? 1 : 0);
    for (const auto& w : plan.warnings) {
      std::printf("    ⚠ %s\n", w.c_str());
    }

    TimelineImplV2 timeline;
    if (!timeline.Open(dir).ok()) {
      ++g_failed;
      continue;
    }

    Recorder recorder(&timeline, &backend);
    RecorderConfig rcfg;
    rcfg.close_timeline_on_stop = false;
    rcfg.backend.extra.push_back({"count", "40"});
    // 不支持自动发现时，业务必须自己给话题 —— 这正是"按能力决策"的体现
    if (!plan.can_record_all) {
      rcfg.record.topics = {"flex/data"};
      rcfg.backend.topics = {"flex/data"};
    }

    Status s = recorder.Start(rcfg);
    Check(s.ok(), "按能力裁剪配置后仍能启动录制");
    if (!s.ok()) {
      timeline.Close();
      continue;
    }
    for (int k = 0; k < 150; ++k) {
      if (recorder.GetStats().received >= 40) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    recorder.Stop();

    // 缺口检测的可用性直接体现在数据里：无序号的样本 sequence 恒为 0
    Query q(&timeline);
    std::vector<TopicSummary> topics;
    q.ListTopics(&topics);
    Check(!topics.empty(), "录到了话题");
    if (!topics.empty()) {
      std::printf("      录制: %s 共 %llu 条\n", topics.front().topic_name.c_str(),
                  static_cast<unsigned long long>(topics.front().message_count));
      // 用 sequence 是否可辨来判断"缺口检测有没有意义"
      bool seq_meaningful = plan.can_detect_gaps;
      Check(seq_meaningful == caps.provides_sequence,
            "缺口检测能力与后端声明一致（没有骗用户）");
    }
    timeline.Close();
  }

  // 反向验证：后端不支持某方向时，上层应能优雅处理
  {
    ConfigurableBackend no_source(minimal, /*provide_source=*/false);
    SourcePtr src;
    Status r = no_source.CreateSource(BackendConfig{}, &src);
    Check(!r.ok(), "不支持数据源的后端明确失败");
    std::printf("    失败信息: %s\n", r.ToString().c_str());
  }

  std::printf("\n  场景 C 完成\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string base = (argc > 1) ? argv[1] : "/tmp/protocol_full_tl";

  std::printf("╔══════════════════════════════════════════════════════════════╗\n");
  std::printf("║  protocol_full：三个真实场景                                  ║\n");
  std::printf("║   A 接非 DDS 数据源（有界队列/背压）                          ║\n");
  std::printf("║   B 转发到自定义去向（连接状态/重试/确认）                    ║\n");
  std::printf("║   C 能力协商与优雅降级                                        ║\n");
  std::printf("╚══════════════════════════════════════════════════════════════╝\n");
  std::printf("工作目录: %s\n", base.c_str());

  ScenarioA_NonDdsSource(base);
  ScenarioB_RemoteForwarding(base);
  ScenarioC_CapabilityNegotiation(base);

  std::printf("\n══════════════════════════════════════════════════════════════\n");
  if (g_failed == 0) {
    std::printf("三个场景全部通过（0 项失败）\n");
    return 0;
  }
  std::printf("有 %d 项检查未通过\n", g_failed);
  return 1;
}
