// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// examples/cpp/common/memory_backend.h
//
// 示例共用的**内存后端**：不依赖网络，可在任何机器上直接跑。
//
// 为什么示例需要它：
//   真实链路（RtpsBackend）需要另一个 DDS 参与者配合，否则回放会报
//   "尚无已发现的对端 Reader"、published=0，示例就演示不了 API 本身。
//   内存后端把"数据从哪来、到哪去"替换掉，让示例聚焦在**被演示的 API** 上。
//
// 它同时也是 `IDdsBackend` 扩展点的最小完整实现，可作为自定义后端的起点。
//
// 用法：
//   #include "common/memory_backend.h"
//   MemoryBackend backend;
//   Recorder recorder(&timeline, &backend);   // 其余代码与真实后端完全一致

#ifndef STRATUM_EXAMPLES_MEMORY_BACKEND_H_
#define STRATUM_EXAMPLES_MEMORY_BACKEND_H_

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "stratum/protocol/idds_backend.h"

namespace stratum {
namespace examples {

// 当前时间（纳秒）。示例里多处需要，统一放这里。
inline TimestampNs NowNs() {
  return static_cast<TimestampNs>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// ---------------------------------------------------------------------------
// 数据源：按固定间隔合成样本
//
// 契约（见 protocol/idds_backend.h）：
//   · Start(callback) 之后，回调在**后端自己的线程**上被调用；
//   · 回调中**不得阻塞**，否则会拖慢接收；
//   · 回调返回 false 表示"不要再推了"。
// ---------------------------------------------------------------------------
class SyntheticSource final : public protocol::ISource {
 public:
  struct Options {
    uint32_t count = 20;            // 合成多少条
    uint32_t interval_us = 2000;    // 条间隔（微秒）
    std::string topic = "demo/signal";
    std::string type = "DemoSignal";
    uint32_t payload_bytes = 32;    // 每条负载大小
    bool dispose_at_end = false;    // 结束时是否补一条 dispose
  };

  explicit SyntheticSource(Options o) : opt_(std::move(o)) {}

  Status Start(protocol::SampleCallback callback) override {
    if (running_.exchange(true)) {
      return Status::Internal("SyntheticSource 已经启动");
    }
    thread_ = std::thread([this, cb = std::move(callback)]() {
      for (uint32_t i = 0; i < opt_.count && running_.load(); ++i) {
        protocol::ReceivedSample s;
        s.topic_name = opt_.topic;
        s.type_name = opt_.type;
        s.source_timestamp_ns = NowNs();
        s.reception_timestamp_ns = s.source_timestamp_ns;
        s.sequence = i + 1;
        // 负载：填可辨识内容，便于回放时肉眼确认
        std::string body = "sample-" + std::to_string(i + 1);
        if (opt_.payload_bytes > body.size()) {
          body.append(opt_.payload_bytes - body.size(), '.');
        }
        s.payload = body;
        if (opt_.dispose_at_end && i + 1 == opt_.count) {
          s.is_dispose = true;
        }
        if (!cb(s)) break;   // 上层要求停止
        if (opt_.interval_us > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(opt_.interval_us));
        }
      }
      running_.store(false, std::memory_order_release);
    });
    return Status::OK();
  }

  Status Stop() override {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    return Status::OK();
  }

  bool IsRunning() const override { return running_.load(std::memory_order_acquire); }

  std::vector<std::string> DiscoveredTopics() const override {
    return {opt_.topic};
  }

 private:
  Options opt_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

// ---------------------------------------------------------------------------
// 数据汇：把回放的样本收集到内存（可选同时打印）
// ---------------------------------------------------------------------------
class CollectingSink final : public protocol::ISink {
 public:
  explicit CollectingSink(bool verbose = false) : verbose_(verbose) {}

  Status Open() override {
    open_ = true;
    if (verbose_) std::printf("    [sink] open\n");
    return Status::OK();
  }
  Status Close() override {
    open_ = false;
    if (verbose_) {
      std::printf("    [sink] close（共收到 %llu 条）\n",
                  static_cast<unsigned long long>(count_));
    }
    return Status::OK();
  }
  bool IsOpen() const override { return open_; }

  Status Publish(const protocol::ReceivedSample& s) override {
    if (!open_) return Status::Internal("sink 未打开");
    std::lock_guard<std::mutex> lock(mu_);
    ++count_;
    total_bytes_ += s.payload.size();
    if (verbose_ && count_ <= 5) {
      std::printf("    [sink] #%u %s bytes=%zu\n", s.sequence, s.topic_name.c_str(),
                  s.payload.size());
    }
    samples_.push_back(s);
    return Status::OK();
  }

  // 没有发现过程，随时可发
  bool ReadyForPublish() const override { return open_; }
  Status WaitForAcknowledgments(uint64_t) override { return Status::OK(); }

  uint64_t count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return count_;
  }
  uint64_t total_bytes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return total_bytes_;
  }
  std::vector<protocol::ReceivedSample> samples() const {
    std::lock_guard<std::mutex> lock(mu_);
    return samples_;
  }

 private:
  bool verbose_ = false;
  bool open_ = false;
  mutable std::mutex mu_;
  uint64_t count_ = 0;
  uint64_t total_bytes_ = 0;
  std::vector<protocol::ReceivedSample> samples_;
};

// ---------------------------------------------------------------------------
// 后端：把上面两个组合起来
//
// 参数经 BackendConfig::extra 传入（它是 vector<pair<string,string>>，不是 map）。
// ---------------------------------------------------------------------------
class MemoryBackend final : public protocol::IDdsBackend {
 public:
  // 让示例能拿到 sink 以检查结果（真实后端不需要这个）
  std::shared_ptr<CollectingSink> last_sink() const { return last_sink_; }
  std::shared_ptr<SyntheticSource> last_source() const { return last_source_; }

  std::string Name() const override { return "memory"; }
  std::string Version() const override { return "1.0.0"; }

  Status CreateSource(const protocol::BackendConfig& cfg,
                      protocol::SourcePtr* out) override {
    SyntheticSource::Options o;
    o.count = GetU32(cfg, "count", o.count);
    o.interval_us = GetU32(cfg, "interval_us", o.interval_us);
    o.payload_bytes = GetU32(cfg, "payload_bytes", o.payload_bytes);
    if (const std::string* v = Find(cfg, "topic")) o.topic = *v;
    if (const std::string* v = Find(cfg, "type_name")) o.type = *v;
    if (const std::string* v = Find(cfg, "dispose_at_end")) {
      o.dispose_at_end = (*v == "1" || *v == "true");
    }
    auto sp = std::make_shared<SyntheticSource>(o);
    last_source_ = sp;
    *out = protocol::SourcePtr(new SharedSource(sp));
    return Status::OK();
  }

  Status CreateSink(const protocol::BackendConfig& cfg,
                    protocol::SinkPtr* out) override {
    bool verbose = false;
    if (const std::string* v = Find(cfg, "verbose")) {
      verbose = (*v == "1" || *v == "true");
    }
    auto sp = std::make_shared<CollectingSink>(verbose);
    last_sink_ = sp;
    *out = protocol::SinkPtr(new SharedSink(sp));
    return Status::OK();
  }

  Capabilities GetCapabilities() const override {
    Capabilities c;
    c.auto_discovery = false;       // 话题由配置决定
    c.provides_sequence = true;     // 我们填了 sequence，缺口检测可用
    c.provides_type_def = false;
    c.supports_multicast = false;
    return c;
  }

 private:
  static const std::string* Find(const protocol::BackendConfig& cfg,
                                 const std::string& key) {
    for (const auto& kv : cfg.extra) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
  static uint32_t GetU32(const protocol::BackendConfig& cfg, const std::string& key,
                         uint32_t def) {
    const std::string* v = Find(cfg, key);
    return v == nullptr ? def : static_cast<uint32_t>(std::atoi(v->c_str()));
  }

  // 把 shared_ptr 包成 ISource/ISink（unique_ptr 接口）
  class SharedSource final : public protocol::ISource {
   public:
    explicit SharedSource(std::shared_ptr<SyntheticSource> p) : p_(std::move(p)) {}
    Status Start(protocol::SampleCallback cb) override { return p_->Start(std::move(cb)); }
    Status Stop() override { return p_->Stop(); }
    bool IsRunning() const override { return p_->IsRunning(); }
    std::vector<std::string> DiscoveredTopics() const override {
      return p_->DiscoveredTopics();
    }

   private:
    std::shared_ptr<SyntheticSource> p_;
  };
  class SharedSink final : public protocol::ISink {
   public:
    explicit SharedSink(std::shared_ptr<CollectingSink> p) : p_(std::move(p)) {}
    Status Open() override { return p_->Open(); }
    Status Close() override { return p_->Close(); }
    bool IsOpen() const override { return p_->IsOpen(); }
    Status Publish(const protocol::ReceivedSample& s) override { return p_->Publish(s); }
    bool ReadyForPublish() const override { return p_->ReadyForPublish(); }
    Status WaitForAcknowledgments(uint64_t t) override {
      return p_->WaitForAcknowledgments(t);
    }

   private:
    std::shared_ptr<CollectingSink> p_;
  };

  std::shared_ptr<CollectingSink> last_sink_;
  std::shared_ptr<SyntheticSource> last_source_;
};

}  // namespace examples
}  // namespace stratum

#endif  // STRATUM_EXAMPLES_MEMORY_BACKEND_H_
