// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// Recorder 实现：二段式录制（回调入队 + 后台写盘）

#include "stratum/core/recorder.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace stratum {

Recorder::Recorder(ITimeline* timeline, protocol::IDdsBackend* backend)
    : timeline_(timeline), backend_(backend) {}

Recorder::~Recorder() {
  // 与 Stop 走同一路径；状态机保证幂等
  Stop();
}

namespace {

TimestampNs NowNs() {
  return static_cast<TimestampNs>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

}  // namespace

Status Recorder::Start(const RecorderConfig& config) {
  if (timeline_ == nullptr) {
    return Status::InvalidArgument("timeline is null");
  }
  if (backend_ == nullptr) {
    return Status::InvalidArgument("backend is null");
  }

  // --- 状态机：抢占 kIdle → kStarting ---
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_ != RecState::kIdle) {
      return Status::AlreadyExists(
          "recorder already started (state not idle)");
    }
    state_ = RecState::kStarting;
    stop_requested_.store(false, std::memory_order_release);
  }

  config_ = config;

  // 应用保留策略（保留策略自动调度，行级对比 B-H5）。
  // 早期实现从不调用 SetRetentionPolicy，用户配置的保留策略
  // 完全不生效。
  if (timeline_ != nullptr) {
    timeline_->SetRetentionPolicy(config_.record.retention);
  }

  // --- 耗时操作在锁外：创建数据源（DDS 发现可能数百毫秒）---
  // 此期间 Stop() 会把 stop_requested_ 置位；完成后我们据此回滚，
  // 而不是像早期实现那样让 Stop "静默成功"而录制继续跑。
  // 用调用方给的 backend 配置（含 domain_id），只覆盖 topics。
  // 【缺陷修复】这里曾经是 `protocol::BackendConfig bcfg;`（默认对象），
  // 于是 domain 被静默丢弃，录制端永远跑在 domain 0。
  protocol::BackendConfig bcfg = config.backend;
  bcfg.topics = config.record.topics;
  protocol::SourcePtr source;
  Status s = backend_->CreateSource(bcfg, &source);
  if (!s.ok()) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_ = RecState::kIdle;
    return s;
  }

  // --- 原子完成启动（锁内：置 source_、建线程、启动后端）---
  // writer 线程在锁内创建，保证 Stop 永远不会"错过"一个已建线程。
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (stop_requested_.load(std::memory_order_acquire)) {
      // 启动期间被请求停止：回滚，source 随栈销毁
      state_ = RecState::kIdle;
      return Status::ShutdownInProgress("stop requested during start");
    }
    source_ = std::move(source);
    running_.store(true, std::memory_order_release);
    writer_thread_ = std::thread([this]() { WriterLoop(); });

    s = source_->Start([this](const protocol::ReceivedSample& sample) {
      return OnSample(sample);
    });
    if (!s.ok()) {
      // 回滚：移出线程（锁外 join）、释放 source_
      running_.store(false, std::memory_order_release);
      stop_requested_.store(true, std::memory_order_release);
      cv_.notify_all();
      cv_space_.notify_all();
      source_.reset();
      state_ = RecState::kIdle;
    } else {
      state_ = RecState::kRunning;
    }
  }

  if (!s.ok()) {
    // 锁外 join（writer 线程看到 stop_requested_ 会退出）
    if (writer_thread_.joinable()) {
      writer_thread_.join();
    }
  }
  return s;
}

Status Recorder::Stop() {
  std::thread to_join;
  protocol::SourcePtr source_to_stop;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    switch (state_) {
      case RecState::kIdle:
        return Status::OK();  // 幂等：未在运行

      case RecState::kStopping:
        return Status::AlreadyExists("stop already in progress");

      case RecState::kStarting:
      case RecState::kRunning: {
        state_ = RecState::kStopping;
        // 【顺序很重要】先置标志并唤醒所有等待者，再移出 source 去 Stop。
        // 后端线程此刻可能正阻塞在 cv_space_.wait（kBlock 等空间），
        // 若不先唤醒，source->Stop() 的 join 会永久等待 → 死锁。
        stop_requested_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        cv_.notify_all();
        cv_space_.notify_all();
        // 移出线程与 source（所有权转移，锁外处理）
        to_join = std::move(writer_thread_);
        source_to_stop = std::move(source_);
        break;
      }
    }
  }

  // --- 锁外：停后端、join 写盘线程 ---
  if (source_to_stop) {
    source_to_stop->Stop();
  }
  if (to_join.joinable()) {
    to_join.join();  // writer 线程看到 stop_requested_ 会冲刷队列后退出
  }

  // --- 锁外：落盘元数据 ---
  if (timeline_ != nullptr) {
    timeline_->Flush();
    if (config_.close_timeline_on_stop) {
      timeline_->Close();
    }
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_ = RecState::kIdle;
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 后端回调：只做入队，绝不做 IO
// ---------------------------------------------------------------------------
bool Recorder::OnSample(const protocol::ReceivedSample& sample) {
  if (!running_.load(std::memory_order_acquire)) {
    return false;  // 通知后端停止推送
  }

  received_.fetch_add(1, std::memory_order_relaxed);

  const uint64_t sample_bytes = SampleBytes(sample);

  {
    std::unique_lock<std::mutex> lock(mutex_);

    // 队列容量控制：按**字节**而非条数（避免大消息绕过限制，对应 EC-08）
    if (queue_bytes_ + sample_bytes > config_.record.max_buffer_bytes) {
      switch (config_.record.overflow_policy) {
        case RecordConfig::OverflowPolicy::kDropNewest:
          dropped_.fetch_add(1, std::memory_order_relaxed);
          if (timeline_ != nullptr) {
            timeline_->ReportDrop(1, sample_bytes, NowNs());
          }
          return true;  // 丢弃本条，但继续接收

        case RecordConfig::OverflowPolicy::kDropOldest:
          while (!queue_.empty() &&
                 queue_bytes_ + sample_bytes > config_.record.max_buffer_bytes) {
            const uint64_t dropped_bytes = SampleBytes(queue_.front());
            queue_bytes_ -= dropped_bytes;
            queue_.pop_front();
            dropped_.fetch_add(1, std::memory_order_relaxed);
            if (timeline_ != nullptr) {
              timeline_->ReportDrop(1, dropped_bytes, NowNs());
            }
          }
          break;

        case RecordConfig::OverflowPolicy::kBlock: {
          // 真正的阻塞等待（用户显式选择"宁可慢也不丢"时使用）。
          //
          // 代价：本函数运行在后端接收线程上，阻塞它会拖慢（甚至停住）
          // 上游接收。这正是 types.h 中该策略注释所声明的取舍。
          //
          // 早期实现这里只有 `break;` —— 既没有阻塞也没有丢弃，
          // 队列于是**无界增长**直至内存耗尽（审查发现 M1）。
          // 配置项声称"保证不丢"，实际后果却比 kDropNewest 更糟。
          //
          // 这里**不设置等待超时**：设了超时后仍会丢弃，就违背了
          // "保证不丢"的语义，配置项再次名不副实。
          // 唯一的中断条件是 Stop() —— 由 cv_space_ 唤醒。
          cv_space_.wait(lock, [&]() {
            return !running_.load(std::memory_order_acquire) ||
                   queue_bytes_ + sample_bytes <=
                       config_.record.max_buffer_bytes;
          });
          if (!running_.load(std::memory_order_acquire)) {
            return false;  // 已停止：通知后端不再推送
          }
          break;
        }
      }
    }

    queue_.push_back(sample);
    queue_bytes_ += sample_bytes;
    const uint64_t depth = queue_.size();
    if (depth > queue_high_water_.load(std::memory_order_relaxed)) {
      queue_high_water_.store(depth, std::memory_order_relaxed);
    }
  }
  cv_.notify_one();
  return true;
}

// ---------------------------------------------------------------------------
// 后台线程：出队 → 写 Timeline
// ---------------------------------------------------------------------------
void Recorder::WriterLoop() {
  std::vector<protocol::ReceivedSample> batch;
  batch.reserve(config_.write_batch_size);

  while (true) {
    batch.clear();
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, std::chrono::milliseconds(config_.idle_flush_interval_ms),
                   [this]() {
                     return !queue_.empty() || stop_requested_.load();
                   });

      bool consumed = false;
      while (!queue_.empty() && batch.size() < config_.write_batch_size) {
        batch.push_back(std::move(queue_.front()));
        // 出队必须用与入队相同的计量（见 SampleBytes 注释）
        queue_bytes_ -= SampleBytes(batch.back());
        queue_.pop_front();
        consumed = true;
      }
      if (consumed) {
        // 唤醒可能正在等空间的后端线程（kBlock 策略）
        cv_space_.notify_all();
      }

      // 退出条件：被要求停止且队列已排空
      if (stop_requested_.load() && queue_.empty()) {
        break;
      }
    }

    if (!batch.empty()) {
      WriteBatch(&batch);
    } else {
      // 空闲时定期把元数据落盘（保证崩溃时元数据不过分滞后）
      if (timeline_ != nullptr) {
        timeline_->Flush();

        // 保留策略自动调度（B-H5）：按 check_interval_ns 节流。
        // 早期实现 RunRetention 无人调用，保留策略形同虚设。
        const uint64_t interval =
            config_.record.retention.check_interval_ns;
        const uint64_t now_ns = NowNs();
        if (interval > 0 &&
            now_ns - last_retention_run_ns_ >= interval) {
          timeline_->RunRetention();
          last_retention_run_ns_ = now_ns;
        }
      }
    }
  }

  // 退出前冲刷剩余
  if (!batch.empty()) {
    WriteBatch(&batch);
  }
}

void Recorder::WriteBatch(std::vector<protocol::ReceivedSample>* batch) {
  if (batch == nullptr || batch->empty() || timeline_ == nullptr) {
    return;
  }

  std::vector<Event> events;
  events.reserve(batch->size());

  for (const auto& s : *batch) {
    // --- 话题注册（带缓存，避免每条样本都查元数据）---
    uint32_t topic_id = 0;
    auto it = topic_ids_.find(s.topic_name);
    if (it != topic_ids_.end()) {
      topic_id = it->second;
    } else {
      TopicMetadata meta;
      meta.topic_name = s.topic_name;
      meta.type_name = s.type_name;
      meta.type_definition = s.type_definition;
      if (timeline_->RegisterTopic(meta, &topic_id).ok()) {
        topic_ids_[s.topic_name] = topic_id;
      } else {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
    }

    Event e;
    e.topic_id = topic_id;
    e.seq = s.sequence;
    // 源时间戳缺失时退化为接收时间戳（保证时间轴可用）
    e.source_timestamp_ns = s.source_timestamp_ns != kInvalidTimestamp
                                ? s.source_timestamp_ns
                                : s.reception_timestamp_ns;
    e.reception_timestamp_ns = s.reception_timestamp_ns != kInvalidTimestamp
                                   ? s.reception_timestamp_ns
                                   : NowNs();
    e.payload = s.payload;
    e.instance_key = s.instance_key;
    events.push_back(std::move(e));

    bytes_recorded_.fetch_add(s.payload.size(), std::memory_order_relaxed);
  }

  if (!events.empty()) {
    Status s = timeline_->AppendBatch(events);
    if (s.ok()) {
      recorded_.fetch_add(events.size(), std::memory_order_relaxed);
    } else {
      dropped_.fetch_add(events.size(), std::memory_order_relaxed);
      if (timeline_ != nullptr) {
        uint64_t dropped_bytes = 0;
        for (const auto& e : events) {
          dropped_bytes += e.payload.size();
        }
        timeline_->ReportDrop(events.size(), dropped_bytes, NowNs());
      }
    }
  }
}

Status Recorder::Flush() {
  if (timeline_ == nullptr) {
    return Status::InvalidArgument("timeline is null");
  }
  return timeline_->Flush();
}

Recorder::Stats Recorder::GetStats() const {
  Stats stats;
  stats.received = received_.load(std::memory_order_relaxed);
  stats.recorded = recorded_.load(std::memory_order_relaxed);
  stats.dropped = dropped_.load(std::memory_order_relaxed);
  stats.queue_high_water = queue_high_water_.load(std::memory_order_relaxed);
  stats.bytes_recorded = bytes_recorded_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stats.queue_depth = queue_.size();
  }
  return stats;
}

}  // namespace stratum
