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
// Recorder —— DDS 样本的落盘端
// ---------------------------------------------------------------------------
/// @brief 把后端回调收到的样本写进时间线（`ITimeline`）。
///
/// @par 模块定位
/// 本类是 `stratum::core` 的**唯一写入端**。读端是 `Query`，两者最终落到同一个
/// `ITimeline` 存储上 —— 所以"写"与"读"的互斥由存储层保证，
/// 而本类自己的 `mutex_` **只保护入队队列**，与读路径无关
/// （误以为 `mutex_` 能挡住读方，是读这段代码时最容易犯的错）。
///
/// @par 保证
///  · `Start` 返回成功 ⇒ 后端已在收数据、writer 线程已运行
///  · `Stop` 返回成功 ⇒ 队列已冲刷 + `Flush()` 已调用；若
///    `config.close_timeline_on_stop` 为真，时间线也已 `Close()`
///  · **不保证**：`Stop` 期间到达的样本一定写入 —— `stop_requested_` 置位后
///    迟到样本会被丢弃（这是"尽快停下来"与"一条不漏"之间的取舍）
///  · **不保证**：`GetStats().bytes_recorded` 与实际磁盘占用一致（存储层可能压缩）
///
/// @par 上层服务（按文件点名）
///  · `src/cli/cmd_record.cc`  —— `stratum record` 的生产调用方
///  · `src/cli/cmd_probe.cc`   —— 把探针样本经 recorder 写进同一条时间线
///  · `tests/unit/test_recorder_queue.cc`、`tests/integration/test_record_*.cc`
///
/// @par 与相邻类的边界（这些事**不**在本类做）
///  · **不做** DDS 发现/匹配 —— 那是 `protocol::IDdsBackend`
///  · **不做** 序列化 —— 那是 `types::ISerializer`
///  · **不做** 完整性判定 —— 那是 `stratum verify`
///  · **不做** 保留淘汰的执行 —— 只调 `ITimeline::SetRetentionPolicy`，实际淘汰在存储层
///  · **不做** 回放 —— 那是 `Player`
///
/// @par 状态机
/// `kIdle → kStarting → kRunning → kStopping → kIdle`（四态，没有 `kClosed`）。
/// **完整转化表见 `docs/state/recorder.md`** —— 那是状态机检查的权威依据；
/// 修改任何状态切换前必须先更新它，否则那张表会变成"看起来权威其实过期"的东西。
///
/// @par 并发模型（三个执行体 + 两把锁）
/// 执行体：
///  · **调用方线程** —— `Start` / `Stop` / `Flush` / `GetStats`
///  · **后端回调线程** —— `OnSample`，**只入队、绝不 IO**（这是它能跑在 DDS 线程上的前提）
///  · **writer 线程** —— `WriterLoop` → `WriteBatch`，**唯一**写时间线的执行体
///
/// 两把锁，职责不重叠：
///  · `state_mutex_` —— 只管 `state_`
///  · `mutex_`       —— 只管 `queue_` / `queue_bytes_` / `topic_ids_`
///
/// **不要交叉持有**：代码里没有同时取两把锁的路径。若将来需要，
/// 必须固定顺序并写进 `docs/state/recorder.md`，否则会引入死锁。
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

  /// 时间线（**非拥有**指针，由调用方管理生命周期）。
  /// 只在 writer 线程里被写（`WriteBatch`），以及 `Stop` 里被 `Flush`/`Close`。
  /// 使用者：Start（SetRetentionPolicy）、WriteBatch、Stop、Flush
  ITimeline* timeline_;

  /// DDS 后端（**非拥有**指针）。
  /// 只在 `Start` 里调 `CreateSource`、以及 `~Recorder` 里用于确认后端仍在。
  /// 注意：`CreateSource` 在**锁外**调用（可能耗时数百毫秒），见 docs/state/recorder.md 时序 1。
  /// 使用者：Start
  protocol::IDdsBackend* backend_;

  /// 后端数据源的持有者（**拥有**，move-only）。
  /// 生命周期与 `state_` 绑定：`Start` 成功时移入，`Stop` 时移到局部变量再在锁外 Stop，
  /// `Start` 失败时随栈销毁。
  /// **为什么要在锁内移出**：保证 `Stop` 不会错过一个刚创建但尚未移出的 source。
  /// 使用者：Start（移入）、Stop（移出并 Stop）
  protocol::SourcePtr source_;

  /// 当前生效的配置快照（`Start` 时整体赋值，之后不再变）。
  /// **只在持锁时写**（Start 在 `kIdle -> kStarting` 之后立即赋值），
  /// 读的地方：`Stop`（`close_timeline_on_stop`）、`WriteBatch`（保留策略）。
  /// 使用者：Start、Stop、WriteBatch
  RecorderConfig config_;

  /// 是否正在对外提供"在录制"的信号（**只用于 `IsRunning()` 的快速判断**）。
  /// 与 `state_` 的区别：`state_` 是精确状态机，`running_` 是给外部看的粗粒度视图。
  /// 二者**不保证同时更新到同一瞬间**（`running_` 是原子量，`state_` 受锁保护）——
  /// 需要精确判断时用 `state_`，需要无锁读时用 `running_`。
  /// 使用者：Start/Stop（写）、IsRunning（读）
  std::atomic<bool> running_{false};

  /// "已请求停止"标志，**唯一用途**是让 `Start` 在耗时操作返回后知道要回滚。
  /// 置位者：`Stop`（锁内）；清除者：`Start` 抢占 `kIdle -> kStarting` 时。
  /// 读取者：`Start`（锁内检查）、`WriterLoop`（决定是否退出循环）。
  /// **为什么不用 `state_` 代替**：`Start` 在第 2 段持锁时 `state_` 可能已被
  /// `Stop` 改成 `kStopping`，再读 `state_` 无法区分"我自己启动失败"与"别人叫我停"。
  std::atomic<bool> stop_requested_{false};

  /// 队列锁。保护 `queue_` / `queue_bytes_` / `topic_ids_` / `last_retention_run_ns_`。
  /// **不保护** `state_`（那是 `state_mutex_` 的职责）——
  /// 两把锁从不交叉持有，所以没有死锁风险。见类注释的「并发模型」。
  /// 使用者：OnSample、WriterLoop、WriteBatch、Flush、GetStats
  mutable std::mutex mutex_;

  /// "有新数据"条件变量。`OnSample` 入队后 notify；`WriterLoop` 等它醒来批量取。
  /// 等待上限是 `config_.idle_flush_interval_ms`（默认 200ms），
  /// 所以**队列空时 writer 线程每 200ms 醒一次**做定期元数据落盘 —— 这不是 bug。
  std::condition_variable cv_;

  /// "队列有空间"条件变量。**只在后端策略为 `kBlock` 时被使用**：
  /// 队列满时 `OnSample` 在这里等，`WriterLoop` 取走数据后 notify。
  /// 【关键】`Stop` 必须先 notify 这个再移走 `source_` —— 否则后端线程
  /// 阻塞在这里时，`source->Stop()` 里的 join 会永久等待（死锁）。
  /// 见 docs/state/recorder.md 第六节时序 2。
  std::condition_variable cv_space_;

  // 上次执行保留淘汰的纳秒时间戳（节流用，B-H5）。
  uint64_t last_retention_run_ns_ = 0;

  /// 待写样本队列（FIFO）。唯一的生产者：后端回调线程（`OnSample`）；
  /// 唯一的消费者：writer 线程（`WriterLoop`）。受 `mutex_` 保护。
  /// 容量由 `RecorderConfig::record.max_queue_bytes` 约束（按 `queue_bytes_` 计）,
  /// 超限时的行为由 `record.on_full` 决定（drop-newest / drop-oldest / block）。
  std::deque<protocol::ReceivedSample> queue_;

  /// 队列当前占用的字节数**估算**（不是精确内存占用，只用于上限判断）。
  /// 【必须】入队与出队用**同一个** `SampleBytes()` 计算 ——
  /// 早期实现入队算 `payload + topic_name`、出队只减 `payload`，
  /// 导致这个值单调虚增，最终恒大于上限 → 录制中途开始丢弃全部新数据。
  /// 见 `SampleBytes` 的注释与审查记录 C3。
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

  /// 后台写盘线程。**只在 `state_mutex_` 内创建与移出，join 在锁外**。
  /// 这个"创建/移出在锁内、join 在锁外"的模式是为了：
  ///  · 锁内保证 `Stop` 不会错过一个正在创建中的线程
  ///  · 锁外 join 避免持锁等待（否则 `Stop` 会阻塞所有状态查询）
  /// 使用者：Start（创建）、Stop（移出后 join）
  std::thread writer_thread_;

  /// 话题名 → topic_id 的缓存，避免每条样本都去 `ITimeline` 查元数据。
  /// 受 `mutex_` 保护。**只增不减**（话题数是有界的，不值得做淘汰）。
  /// 竞态说明：后端可能对同一个话题并发回调（DDS 多线程接收），
  /// 所以查-插这一对必须在锁内完成，否则会重复注册同一话题。
  std::unordered_map<std::string, uint32_t> topic_ids_;

  /// 统计量（原子，可在三个执行体里并发更新）。
  ///
  /// @par 设计意图（**注意：不是已验证的不变量**）
  /// 期望的关系是：`received_ == recorded_ + dropped_ + queue_.size()`
  /// 即"收到的 = 写下去的 + 丢掉的 + 还在排队的"。
  ///
  /// 成立的前提是 `received_` 在**入队前**就 +1（`recorder.cc:196`）——
  /// 所以随后无论是入队、被 drop-newest 丢弃、还是被 drop-oldest 挤出，
  /// 都已经计入了 received。
  ///
  /// **但当前没有任何测试断言它**（`test_recorder_queue` 验的是别的性质）。
  /// 而且并发下 `GetStats()` 是**分五次独立原子读**，五次之间状态会变，
  /// 所以外部观察到的快照**可能不满足**这个等式（差值通常是 0 到队列长度之间）。
  ///
  /// 若将来要把它变成真正的断言，必须先把统计做成**一次性快照**
  /// （在同一把锁内读全部字段，或用一个 struct 原子替换）。
  /// 在那之前，请把它当作**读代码时的心智模型**，而不是可以依赖的契约。
  ///
  /// 各字段的更新者：
  ///  · `received_`  —— OnSample（入队成功时）
  ///  · `recorded_`  —— WriteBatch（真正写进时间线时）
  ///  · `dropped_`   —— OnSample（队列满且策略为 drop-* 时）
  ///  · `queue_high_water_` —— OnSample（入队后比较取大）
  ///  · `bytes_recorded_`   —— WriteBatch
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

  // 批量发布的批大小（**默认 1 = 逐条，行为与改动前完全一致**）。
  //
  // 【为什么默认 1】回放有三处已验证的时序（VOLATILE 首条要 sleep 500ms、
  // 协作式停止、publish_failed_ 的计数语义）。默认逐条让这次改动**零回归**，
  // 想省带宽时显式设 >1 即可（例如 `stratum play --batch-size 16`）。
  //
  // 【收益】一次 sendto 发 N 条：省 (N-1)×(Message Header 20 + INFO_DST 16)
  // 字节，外加 (N-1)×28 字节的 IP/UDP 头。实测（payload=24B、N=10、含 IP/UDP）
  // 1080 → 648 字节，省 40%。**这是报文级数字，真实链路还要算分片/重传/HEARTBEAT。**
  //
  // 【上限】建议 ≤ 32：批太大会让"首条延迟"变长（整批攒齐才发），
  // 也会让单包接近 MTU 而触发 IP 分片。
  uint32_t batch_size = 1;

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

/// @brief 回放器：把时间线里的数据按时间轴重新发布到 DDS。
///
/// @par 模块定位
/// 与 `Recorder` 互为逆操作。`Recorder` 消费 DDS 写时间线，`Player` 读时间线写 DDS。
///
/// @par 上层服务（按文件点名）
///  · `src/cli/cmd_play.cc` —— `stratum play` 的唯一调用方
///  · `tests/integration/test_record_replay*.cc` —— 端到端回环验证
///
/// @par 与相邻类的边界
///  · **不做** DDS 发现 —— 那是 `protocol::IDdsBackend`（但 `PlayerConfig::discovery_wait_ms`
///    暴露了"等发现完成"这个必要动作，见该字段的注释）
///  · **不做** 话题改名 —— `PlayerConfig::topic_mapping` 只是把配置传给后端
///  · **不做** 时间轴合并 —— 那是 `cmd_merge.cc`
///
/// @par 已实现与未实现
/// 基础回放（速率、范围、映射、等待发现）已实现。
/// **`--inject` / `--loop-count` 等 6 个选项未实现**，会明确报"暂不支持"（不静默）。
/// 它们需要 core 层改动，见 `docs/02-open-issues.md`。
///
/// @par 两个必须可见的计数
/// `republished_` 与 `publish_failed_` 是原子量且**对外暴露**，因为：
///  · 回放"看起来成功"但实际丢了数据，是最难查的一类问题
///  · 项目纪律是"不静默失败" —— 发不出去的条数必须能被读出来
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
  /// 数据来源（**非拥有**）。回放期间只读。
  ITimeline* timeline_;

  /// DDS 后端（**非拥有**）。`Play` 时调 `Open`/`Send`/`Close`。
  /// 注意 `PlayerConfig::discovery_wait_ms` 的作用：Open 之后必须等 SPDP/SEDP
  /// 匹配完成才能发数据，否则对端会把样本当成"未知 writer"丢掉（实测收到 0 条）。
  protocol::IDdsBackend* backend_;

  /// 收尾补救时重发的条数（用**新 SN** 重发未被 ack 的样本）。
  /// 为什么用新 SN：DDS 的可靠性机制不会为同一个 SN 重试无限次，
  /// 且对端可能已把旧 SN 记为"已收" —— 只有新 SN 才会真正重传。
  /// 使用者：Play（累加）、GetProgress（读）、CLI 输出
  std::atomic<uint64_t> republished_{0};

  /// 发布失败条数。**必须可见**（项目纪律：不静默失败）。
  /// 与 `republished_` 的区别：这是"补发后仍然失败"，即真正丢了。
  std::atomic<uint64_t> publish_failed_{0};

  /// 是否正在回放。`RequestStop` 会把它置 false，`Play` 的循环检查它。
  /// 与 `Player` 无 `state_` 状态机 —— 因为回放是一次性的阻塞调用，
  /// 不存在 `Recorder` 那种"启动中被打断"的中间态。
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

/// @brief 只读查询包装：把 `ITimeline` 的底层接口翻译成 CLI 需要的形状。
///
/// @par 模块定位
/// 它是一个**极薄的适配层**（25 行）—— 不缓存、不索引、不做任何判断。
/// 存在的理由是把"topic_id 的查法"与"输出的构造"从 CLI 里剥离出来，
/// 让 `cmd_query.cc` / `cmd_verify.cc` / `cmd_diff.cc` / `cmd_merge.cc` 共用同一套读取逻辑。
///
/// @par 上层服务（按文件点名）
///  · `src/cli/cmd_query.cc`  —— 全部读取都经本类
///  · `src/cli/cmd_verify.cc` —— `ListTopics` / `GetInfo`
///  · `src/cli/cmd_diff.cc`   —— `ListTopics`（拿条数与首末时间）
///  · `src/cli/cmd_merge.cc`  —— `FindTopic`（`TopicSummary` 没有 topic_id，必须回到这里拿）
///
/// @par 与相邻类的边界
///  · **不做** 缓存 —— 每次调用都直达存储层（多调用一次就多一次 IO，
///    调用方自己在循环外缓存，例如 `cmd_diff` 的两遍打开）
///  · **不做** 完整性判定 —— 只返回事实（`SessionMetadata` 里的质量字段），
///    "算不算有问题"由 `verify` 给结论
///  · **不做** 并发保护 —— 线程安全由 `ITimeline` 实现（`TimelineImplV2`）负责
///
/// @par 为什么 `FindTopic` 必须存在
/// `ListTopics` 返回的是 `TopicSummary`，它**故意不含 `topic_id`**
/// （摘要只给"用户看得懂的东西"）。任何要按 id 查询的路径都必须
/// 先 `FindTopic(名字)` 拿 `TopicMetadata`，再 `ExportRange(id, ...)`。
/// 忘了这一步会导致"用错 id 查到别人的数据"—— 而且不会报错。
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
  /// 被查询的时间线（**非拥有**，生命周期由调用方保证）。
  /// 不做空指针检查 —— 构造方（各 CLI 命令）在构造前已确认时间线已打开。
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
