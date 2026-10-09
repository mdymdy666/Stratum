// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// Recorder / Player / Query —— Stratum 的对外能力
//
// ===========================================================================
// Recorder 的关键约束
// ===========================================================================
//
// DDS 后端的回调运行在**后端自己的线程**上。若在回调里直接写磁盘：
//   - 会把后端线程拖慢（自研实现中，接收线程被拖慢会导致丢包）
//   - 磁盘抖动会直接传导到业务
//
// 因此 Recorder 采用"回调入队 + 后台线程消费"的二段式：
//
//     [后端线程] 回调 → 入队（内存，极快）→ 立即返回
//                                    ↓
//     [录制线程] 出队 → 编码 → 写 Timeline（可能慢）
//
// 队列满时按配置策略处理（默认丢弃新数据 + 计数告警），
// **绝不阻塞后端线程** —— 这是"录制不影响业务"的实现基础。

#ifndef STRATUM_CORE_RECORDER_H_
#define STRATUM_CORE_RECORDER_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "stratum/core/itimeline.h"
#include "stratum/core/types.h"
#include "stratum/protocol/idds_backend.h"

namespace stratum {

// 录制器配置（在 RecordConfig 基础上补充运行时参数）
struct RecorderConfig {
  RecordConfig record;

  // 传给 DDS 后端的配置（domain_id、participant_name、extra 等）。
  //
  // 【为什么必须有这个字段】Recorder 内部会构造 BackendConfig 交给
  // backend->CreateSource()。曾经这里是一个**全新的默认对象**，
  // 于是调用方设置的 domain 被静默丢弃 —— 无论上层传什么域，
  // 录制端永远跑在 domain 0（回放端 PlayerConfig 有同样的缺陷）。
  // 实测：`stratum_rtps_replay <timeline> 3` 声称 domain=3，
  // 进程实际绑定的却是 domain 0 的 7400/9060。
  // 这类"配置在中间层被丢掉"的问题不会报错，只会让互操作永远对不上。
  protocol::BackendConfig backend;

  // 后台线程写盘的批量大小（越大吞吐越高、延迟越大）
  size_t write_batch_size = 256;

  // 后台线程在队列空时的等待上限（毫秒），用于定期刷新元数据
  uint32_t idle_flush_interval_ms = 200;

  // 是否在停止时自动关闭时间线
  bool close_timeline_on_stop = true;
};

// ---------------------------------------------------------------------------
// Recorder
// ---------------------------------------------------------------------------
class Recorder {
 public:
  Recorder(ITimeline* timeline, protocol::IDdsBackend* backend);
  ~Recorder();

  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  // 开始录制。会启动后端数据源与后台写盘线程。
  Status Start(const RecorderConfig& config);

  // 停止录制。会冲刷队列、落盘元数据、停止后端。
  // 保证：已入队的数据不丢（除非超时）。
  Status Stop();

  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  // --- 运行时状态（用于可观测性与质量报告）---

  struct Stats {
    uint64_t received = 0;      // 从后端收到的样本数
    uint64_t recorded = 0;      // 成功写入时间线的样本数
    uint64_t dropped = 0;       // 因队列满被丢弃的样本数
    uint64_t queue_depth = 0;   // 当前队列深度
    uint64_t queue_high_water = 0;
    uint64_t bytes_recorded = 0;
  };
  Stats GetStats() const;

  // 触发一次元数据落盘（不多次调用，用于优雅停止前）
  Status Flush();

 private:
  // --- 状态机 ---------------------------------------------------
  //
  // 早期实现只有 running_ 一个布尔标志，Start/Stop 是
  // "检查标志位 → 做事 → 改标志位"的裸序列（审查发现 C1，
  // 实测并发 Start 3/3 abort、并发 Stop 2/3 abort）。
  //
  // 现在参考 RocksDB DBImpl 的做法：所有状态转移在 state_mutex_
  // 保护下进行；耗时操作（CreateSource / Stop / join / Flush）
  // 在锁外执行；writer 线程的创建与移出都在锁内，join 在锁外。
  enum class RecState { kIdle, kStarting, kRunning, kStopping };
  mutable std::mutex state_mutex_;
  RecState state_ = RecState::kIdle;

  // 后端回调：入队
  bool OnSample(const protocol::ReceivedSample& sample);
  // 后台线程主循环
  void WriterLoop();
  // 处理一批样本（调用者不持锁）
  void WriteBatch(std::vector<protocol::ReceivedSample>* batch);

  ITimeline* timeline_;
  protocol::IDdsBackend* backend_;
  protocol::SourcePtr source_;

  RecorderConfig config_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};

  mutable std::mutex mutex_;
  std::condition_variable cv_;        // 通知写盘线程：有新数据
  std::condition_variable cv_space_;  // 通知后端线程：队列有空间（kBlock 用）

  // 上次执行保留淘汰的纳秒时间戳（节流用，B-H5）。
  uint64_t last_retention_run_ns_ = 0;

  std::deque<protocol::ReceivedSample> queue_;
  size_t queue_bytes_ = 0;

  // 单条样本占用的字节数。
  //
  // 【重要】入队与出队必须使用同一个函数计算，否则计数会漂移。
  // 早期实现在入队时算 `payload + topic_name`、出队时只减 `payload`，
  // 导致 queue_bytes_ 随每条样本单调虚增 topic_name.size() 字节，
  // 最终恒大于上限 —— 录制中途开始丢弃**全部**新数据（审查发现 C3）。
  static uint64_t SampleBytes(const protocol::ReceivedSample& s) {
    return static_cast<uint64_t>(s.payload.size() + s.topic_name.size());
  }

  std::thread writer_thread_;

  // 话题名 → topic_id 的缓存（避免每条样本都查元数据）
  std::unordered_map<std::string, uint32_t> topic_ids_;

  // 统计（原子，可在后端线程与录制线程并发更新）
  std::atomic<uint64_t> received_{0};
  std::atomic<uint64_t> recorded_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> queue_high_water_{0};
  std::atomic<uint64_t> bytes_recorded_{0};
};

// ---------------------------------------------------------------------------
// Player（回放器）
// ---------------------------------------------------------------------------
struct PlayerConfig {
  // 回放速率：1.0 = 原速，10.0 = 十倍速，0 = 尽可能快
  double rate = 1.0;

  // 传给 DDS 后端的配置（domain_id、participant_name、extra 等）。
  // 见 RecorderConfig::backend 的说明：这个字段缺失曾导致
  // "回放总在 domain 0" 这一长期误判。
  protocol::BackendConfig backend;

  // 话题名重映射（避免污染实时数据流）
  // 例如 {"sensor/temp", "sensor/temp_replay"}
  std::vector<std::pair<std::string, std::string>> topic_mapping;

  // 强制使用原名（危险：会与实时发布者冲突）
  bool allow_original_topic_names = false;

  // 打开数据汇后、开始回放前的等待时间（毫秒）。
  //
  // 【为什么需要】回放器以 RTPS Writer 身份发布数据，而对端需要先
  // 通过 SPDP/SEDP 交换完成匹配才会接收。若 Open 后立即发数据，
  // 对端尚未建立 Writer 代理，数据会被丢弃
  // （实测：等待 0ms 时对端收到 0 条；等待数秒后正常收到）。
  //
  // 默认 0 表示不等待（适用于内存/模拟后端，它们没有发现过程）。
  uint64_t discovery_wait_ms = 0;

  // 是否在回放结束后等待确认
  bool wait_for_ack = false;
  uint64_t ack_timeout_ms = 5000;

  // 收尾补救：把未被确认的样本用**新 SN** 重新发布。
  //
  // 背景：VOLATILE durability 下 reader 不索要匹配完成前发出的数据，
  // 回放首条会永久丢失（实测稳定 199/200，缺的恒为首条），且重发相同 SN 无效。
  // 开启后内容完整性由 199/200 提升到 200/200。
  bool republish_unacked = true;
  int republish_rounds = 2;

  // 是否循环回放
  bool loop = false;

  // 每条样本发布后的回调（在回放线程上调用）。
  //
  // 用途：让调用方在**不持有 sink 所有权**的前提下观察回放内容
  // （测试收集、实时统计、进度展示）。
  // 注意：回调中不得阻塞，否则会拖慢回放时序。
  std::function<void(const protocol::ReceivedSample&)> publish_callback;

};

// 回放进度
struct PlaybackProgress {
  TimestampNs cursor_ns = 0;      // 当前时间游标
  TimestampNs begin_ns = 0;       // 回放起点
  TimestampNs end_ns = 0;         // 回放终点
  uint64_t published = 0;         // 已发布样本数
  uint64_t total = 0;             // 总样本数（估计）
  bool finished = false;

  double Percent() const {
    if (end_ns <= begin_ns) return 0.0;
    const double done = static_cast<double>(cursor_ns - begin_ns);
    const double full = static_cast<double>(end_ns - begin_ns);
    return done >= full ? 100.0 : (done * 100.0 / full);
  }
};

class Player {
 public:
  Player(ITimeline* timeline, protocol::IDdsBackend* backend);
  ~Player();

  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;

  // 回放指定时间范围（end_ns = 0 表示到末尾）。
  // 阻塞直到回放完成（或 loop=true 且被 Stop）。
  Status Play(const TimeRange& range, const PlayerConfig& config);

  // 从另一个线程请求停止
  void RequestStop();
  bool IsPlaying() const { return playing_.load(std::memory_order_acquire); }

  PlaybackProgress GetProgress() const;

 private:
  ITimeline* timeline_;
  protocol::IDdsBackend* backend_;

  std::atomic<uint64_t> republished_{0};   // 收尾补救重发条数（用新 SN）
  std::atomic<uint64_t> publish_failed_{0};  // 发布失败条数（必须可见）

  std::atomic<bool> playing_{false};
  std::atomic<bool> stop_requested_{false};

  // 回放起点（steady_clock），用于速率控制。
  // 每轮 loop 开始时重置，避免上一轮的累积误差影响下一轮。
  std::chrono::steady_clock::time_point play_start_{};

  mutable std::mutex progress_mutex_;
  PlaybackProgress progress_;
};

// ---------------------------------------------------------------------------
// Query（查询接口）
// ---------------------------------------------------------------------------
struct TopicSummary {
  std::string topic_name;
  std::string type_name;
  uint64_t message_count = 0;
  uint64_t byte_count = 0;
  TimestampNs first_ns = kInvalidTimestamp;
  TimestampNs last_ns = kInvalidTimestamp;
  bool active = false;
};

class Query {
 public:
  explicit Query(ITimeline* timeline) : timeline_(timeline) {}

  // 列出全部话题的摘要
  Status ListTopics(std::vector<TopicSummary>* out) const;

  // 录制总体信息
  Status GetInfo(SessionMetadata* out) const;

  // 时间范围查询（把结果转成人类可读或结构化输出）
  Status ExportRange(uint32_t topic_id, const TimeRange& range,
                     std::vector<Event>* out) const;

  // 定点查询
  Status GetStateAt(uint32_t topic_id, TimestampNs as_of_ns,
                    StateAtResult* out) const;

  // 按名字找话题
  Status FindTopic(const std::string& name, TopicMetadata* out) const;

 private:
  ITimeline* timeline_;
};

// ---------------------------------------------------------------------------
// 质量报告
// ---------------------------------------------------------------------------
struct QualityReport {
  // 总体评估
  enum class Level { kGood, kWarning, kBad };
  Level level = Level::kGood;

  // 逐话题的完整性
  struct TopicQuality {
    std::string topic_name;
    uint64_t message_count = 0;
    uint64_t gaps = 0;              // 时间轴空洞数
    uint64_t missing = 0;           // 估计缺失的消息数
    bool truncated_at_start = false;  // 起始处可能不完整（发现延迟）
  };
  std::vector<TopicQuality> topics;

  QualityStats stats;

  // 人类可读的报告（含"使用建议"）
  std::string ToText() const;

  // 机器可读（JSON）
  std::string ToJson() const;
};

class QualityReporter {
 public:
  explicit QualityReporter(ITimeline* timeline) : timeline_(timeline) {}

  // 生成报告。gap_threshold_ns 用于判断"空洞"（0 表示用默认值 1 秒）
  Status Generate(uint64_t gap_threshold_ns, QualityReport* out) const;

 private:
  ITimeline* timeline_;
};

}  // namespace stratum

#endif  // STRATUM_CORE_RECORDER_H_
