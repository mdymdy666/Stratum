// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 内存模拟 DDS 后端（测试支撑）
//
// 这不是"测试脚手架"，而是架构的一部分：它证明 IDdsBackend 抽象可实现，
// 并让录制/回放逻辑在零网络条件下被完整验证。
//
// 生命周期说明（本文件的关键点）：
//   CreateSource/CreateSink 返回的 unique_ptr 所有权**转移给调用方**
//   （Recorder / Player）。因此测试不能在 Play/Stop 返回后访问这些指针。
//
//   测试观察行为的两条正确途径：
//     - 录制侧：MockBackend::source_slot()（登记刚创建的 source，
//       仅在对应 Recorder 存活期间有效），用于注入数据与查询 delivered
//     - 回放侧：PlayerConfig::publish_callback，在回放过程中收集样本，
//       不依赖 sink 指针存活

#ifndef STRATUM_TESTING_MOCK_BACKEND_H_
#define STRATUM_TESTING_MOCK_BACKEND_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "stratum/protocol/idds_backend.h"

namespace stratum {
namespace testing {

// ---------------------------------------------------------------------------
// 模拟数据源
// ---------------------------------------------------------------------------
class MockSource : public protocol::ISource {
 public:
  MockSource() = default;
  ~MockSource() override { Stop(); }

  MockSource(const MockSource&) = delete;
  MockSource& operator=(const MockSource&) = delete;

  void Push(protocol::ReceivedSample sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(std::move(sample));
    cv_.notify_all();
  }

  Status Start(protocol::SampleCallback callback) override {
    if (running_.load()) {
      return Status::AlreadyExists("source already running");
    }
    callback_ = std::move(callback);
    running_.store(true);
    thread_ = std::thread([this]() { Loop(); });
    return Status::OK();
  }

  Status Stop() override {
    if (!running_.load()) {
      return Status::OK();
    }
    running_.store(false);
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
    return Status::OK();
  }

  bool IsRunning() const override { return running_.load(); }

  std::vector<std::string> DiscoveredTopics() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (const auto& s : pending_) {
      result.push_back(s.topic_name);
    }
    return result;
  }

  uint64_t delivered() const { return delivered_.load(); }

 private:
  void Loop() {
    while (running_.load()) {
      protocol::ReceivedSample sample;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(1), [this]() {
          return !pending_.empty() || !running_.load();
        });
        if (!running_.load()) {
          break;
        }
        if (pending_.empty()) {
          continue;
        }
        sample = std::move(pending_.front());
        pending_.pop_front();
      }
      if (callback_) {
        if (!callback_(sample)) {
          break;
        }
        delivered_.fetch_add(1);
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<protocol::ReceivedSample> pending_;
  protocol::SampleCallback callback_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::atomic<uint64_t> delivered_{0};
};

// ---------------------------------------------------------------------------
// 模拟数据汇
// ---------------------------------------------------------------------------
class MockSink : public protocol::ISink {
 public:
  Status Open() override {
    open_.store(true);
    return Status::OK();
  }

  Status Close() override {
    open_.store(false);
    return Status::OK();
  }

  bool IsOpen() const override { return open_.load(); }

  Status Publish(const protocol::ReceivedSample& sample) override {
    if (!open_.load()) {
      return Status::ShutdownInProgress("sink not open");
    }
    count_.fetch_add(1);
    (void)sample;
    return Status::OK();
  }

  Status WaitForAcknowledgments(uint64_t /*timeout_ms*/) override {
    return Status::OK();
  }

  uint64_t count() const { return count_.load(); }

 private:
  std::atomic<bool> open_{false};
  std::atomic<uint64_t> count_{0};
};

// ---------------------------------------------------------------------------
// 模拟后端
// ---------------------------------------------------------------------------
class MockBackend : public protocol::IDdsBackend {
 public:
  MockBackend() = default;
  ~MockBackend() override = default;

  std::string Name() const override { return "mock"; }
  std::string Version() const override { return "1.0"; }

  Status CreateSource(const protocol::BackendConfig& config,
                      protocol::SourcePtr* out) override {
    // 记录收到的后端配置：Recorder 曾在这里丢掉调用方的 domain，
    // 必须让测试能断言"配置真的传到了后端"。
    last_source_domain() = config.domain_id;
    last_source_topic_count() = config.topics.size();
    auto* raw = new MockSource();
    source_slot() = raw;  // 登记供测试注入（所有权随即转移给 Recorder）
    *out = protocol::SourcePtr(raw);
    return Status::OK();
  }

  Status CreateSink(const protocol::BackendConfig& config,
                    protocol::SinkPtr* out) override {
    last_sink_domain() = config.domain_id;
    auto* raw = new MockSink();
    sink_slot() = raw;  // 登记供测试读取计数（所有权随即转移给 Player）
    *out = protocol::SinkPtr(raw);
    return Status::OK();
  }

  Capabilities GetCapabilities() const override {
    Capabilities caps;
    caps.auto_discovery = true;
    caps.provides_sequence = true;
    caps.provides_type_def = true;
    return caps;
  }

  // 测试用静态槽位。指针在对应对象销毁后即失效，
  // 只在 Recorder 存活期间（或 Player::Play 返回前）使用。
  static MockSource*& source_slot() {
    static MockSource* slot = nullptr;
    return slot;
  }
  static MockSink*& sink_slot() {
    static MockSink* slot = nullptr;
    return slot;
  }

  // 最近一次 CreateSource / CreateSink 收到的后端配置（供"配置传递"回归测试）
  static uint32_t& last_source_domain() {
    static uint32_t value = 0xFFFFFFFFu;
    return value;
  }
  static uint32_t& last_sink_domain() {
    static uint32_t value = 0xFFFFFFFFu;
    return value;
  }
  static size_t& last_source_topic_count() {
    static size_t value = 0;
    return value;
  }
};

}  // namespace testing
}  // namespace stratum

#endif  // STRATUM_TESTING_MOCK_BACKEND_H_
