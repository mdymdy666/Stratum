// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// TimelineImplV2：基于 StratumDB（LSMengine 架构）的 ITimeline 实现
//
// 这是 v2 接入上层的适配层：让 Recorder/Player/Query 无感知地跑在
// LSMengine 之上，兑现"插件式架构"的承诺（现有上层代码零修改）。

#ifndef STRATUM_CORE_TIMELINE_IMPL_V2_H_
#define STRATUM_CORE_TIMELINE_IMPL_V2_H_

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "stratum/core/itimeline.h"
#include "stratum/storage/lsm_v2/stratum_db.h"

namespace stratum {

// 物化迭代器：把 StratumDB 的范围查询结果包装为 IEventIterator。
class TimelineV2Iterator : public IEventIterator {
 public:
  explicit TimelineV2Iterator(std::vector<Event> events, Status status)
      : events_(std::move(events)), status_(std::move(status)) {}

  bool Valid() const override { return pos_ < events_.size(); }
  void Next() override {
    if (Valid()) ++pos_;
  }
  const Event& event() const override { return events_[pos_]; }
  Status status() const override { return status_; }

 private:
  std::vector<Event> events_;
  size_t pos_ = 0;
  Status status_;
};

/// @brief `ITimeline` 的 V2 实现：基于 LSM（`StratumDB`）的持久化时间线。
///
/// @par 模块定位
/// 本类是**存储层之上、业务层之下**的那一层：它把 `ITimeline` 的语义
/// （话题注册、事件追加、区间查询、保留淘汰、质量统计）翻译成对
/// `storage::lsm_v2::StratumDB` 的键值操作。
///
/// 它是**读与写的汇合点** —— `Recorder` 写它、`Query` 读它，
/// 两者共享同一个实例（因此也共享同一把 `mutex_`）。
///
/// @par 与 V1 的区别（`timeline_impl.h`）
///  · V1 用单一 LSM 引擎 + 自定义 WAL；V2 改用 `StratumDB`（多级 LSM + 列族）
///  · V2 才有**保留点快照**（`retention_points_`）与**质量统计**（`quality_`）
///  · V1 仍在仓库里因为部分测试与工具还在用；**新代码一律用 V2**
///
/// @par 上层服务（按文件点名）
///  · `src/src/core/recorder.cc` —— `Recorder` 通过 `Append`/`AppendBatch` 写入（**唯一写方**）
///  · `src/cli/cmd_query.cc`   —— `query` 的读路径（经 `Query` 包装）
///  · `src/cli/cmd_verify.cc`  —— 读 `quality_` 与 `ListPrunedRanges` 做完整性判定
///  · `src/cli/cmd_diff.cc` / `cmd_merge.cc` —— 只读遍历
///  · `src/cli/cmd_prune.cc`   —— `DeleteRange` 与保留策略
///
/// @par 与相邻类的边界（这些事**不**在本类做）
///  · **不做** DDS 交互 —— 那是 `Recorder` + `protocol::IDdsBackend`
///  · **不做** 序列化/反序列化 —— 事件进来时已是 `Event` 结构，序列化在 `types/`
///  · **不做** 完整性"结论" —— 只提供 `QualityStats` 与裁剪区间等**事实**，
///    "这算不算有问题"由 `verify` 判断（见 `docs/14` 的「不下结论」原则）
///  · **不做** 跨会话合并 —— 那是 `cmd_merge.cc`
///
/// @par 并发模型（**一条必须遵守的纪律**）
/// 只有一把锁 `mutex_`，它保护**全部**可变状态（`db_` 之外的成员 + `db_` 自身的使用）。
///
/// **铁律：每个公开方法的 `if (!opened_) return ...` 检查必须在 `lock_guard` 之后。**
/// 原因：`opened_` 本身受 `mutex_` 保护。若在锁外读它，会与并发的 `Open`/`Close`
/// 形成数据竞争（TSan 会报，且实际可能读到"已打开"然后进到函数里发现 `db_` 为空）。
///
/// 正确形状：
/// ```cpp
/// Status TimelineImplV2::Foo(...) {
///     std::lock_guard<std::mutex> lock(mutex_);   // ← 先持锁
///     if (!opened_) return Status::Error("未打开"); // ← 再检查
///     ...
/// }
/// ```
///
/// 这条纪律在整个项目里出现过多次（`Recorder`、`Query`、`Player` 同理），
/// 因为"先检查再持锁"看起来更自然 —— 但它错了。
class TimelineImplV2 : public ITimeline {
 public:
  TimelineImplV2() = default;
  ~TimelineImplV2() override;

  TimelineImplV2(const TimelineImplV2&) = delete;
  TimelineImplV2& operator=(const TimelineImplV2&) = delete;

  Status Open(const std::string& path) override;
  Status Close() override;

  Status RegisterTopic(const TopicMetadata& metadata,
                       uint32_t* topic_id_out) override;
  Status Append(const Event& event) override;
  Status AppendBatch(const std::vector<Event>& events) override;

  Status GetSessionMetadata(SessionMetadata* out) const override;
  Status GetTopicMetadata(uint32_t topic_id, TopicMetadata* out) const override;
  Status ListTopics(std::vector<TopicMetadata>* out) const override;
  Status FindTopicByName(const std::string& name,
                         TopicMetadata* out) const override;

  Status QueryRange(uint32_t topic_id, const TimeRange& range,
                    EventIteratorPtr* out) override;
  Status GetStateAt(uint32_t topic_id, TimestampNs as_of_ns,
                    StateAtResult* out) override;
  Status CountRange(uint32_t topic_id, const TimeRange& range,
                    uint64_t* count_out) override;
  Status DeleteRange(uint32_t topic_id, const TimeRange& range,
                     uint64_t* deleted_events = nullptr,
                     uint64_t* deleted_bytes = nullptr) override;
  Status ListPrunedRanges(uint32_t topic_id,
                         std::vector<PrunedRange>* out) override;
  Status SetTopicTypeDefinition(const std::string& topic_name,
                               const std::string& definition) override;
  Status GetTopicTypeDefinition(const std::string& topic_name,
                               std::string* definition) override;
  Status RecordControlEvent(const ControlEvent& event) override;
  Status ListControlEvents(std::vector<ControlEvent>* out) override;

  Status CreateRetentionPoint(TimestampNs at_ns,
                              uint64_t* point_id_out) override;
  Status ReleaseRetentionPoint(uint64_t point_id) override;

  Status SetRetentionPolicy(const RetentionPolicy& policy) override;
  RetentionPolicy GetRetentionPolicy() const override;
  Status RunRetention() override;

  Status Flush() override;
  QualityStats GetQualityStats() const override;

  Status ReportDrop(uint64_t count, uint64_t bytes,
                    TimestampNs at_ns) override;

 private:
  // 追加时更新质量统计（时间戳倒退/序号缺口/延迟），复用 v1 语义
  void UpdateQualityOnAppend(const Event& e);
  // 加载话题元数据（注册信息 + 统计）
  Status LoadTopicsLocked();
  // 读取会话级元数据（created/closed/total_*）。
  // 与 PersistSessionLocked 配对；缺失它会导致重开后会话时间戳归零。
  Status LoadSessionLocked();
  // 持久化话题统计（first/last/count/bytes）
  Status PersistTopicStatsLocked(uint32_t topic_id, const TopicMetadata& meta);
  // 持久化会话元数据
  Status PersistSessionLocked();
  // 持久化/恢复保留点列表（P4：跨重启）
  Status PersistRetentionPointsLocked();
  Status LoadRetentionPointsLocked();

  /// 底层 LSM 数据库（**拥有**）。`Open` 时创建、`Close` 时销毁。
  /// 受 `mutex_` 保护 —— 注意这里保护的是"指针本身"（可能被 Close 置空），
  /// 不是"数据库内部状态"（那由 StratumDB 自己的锁负责）。
  /// 使用者：几乎所有方法
  std::unique_ptr<storage::lsm_v2::StratumDB> db_;

  /// 会话目录路径（`Open` 时记下，用于错误消息与日志；之后不再变）。
  std::string path_;

  /// 是否已 `Open` 成功。
  /// **只在持有 `mutex_` 时读写** —— 见类注释的「并发模型」铁律：
  /// 每个公开方法的 `if (!opened_)` 必须在 `lock_guard` **之后**。
  /// 使用者：Open/Close（写）、其余所有公开方法（读）
  bool opened_ = false;

  /// 全局锁。保护本类**全部**可变状态：`db_` / `opened_` / 两张 topic 表 /
  /// 保留点 / 质量统计 / 会话统计。
  ///
  /// 【为什么只有一把】本类的操作都很快（键值读写，真正的 IO 在 StratumDB 内部），
  /// 拆细粒度锁的收益抵不上复杂度与死锁风险。
  /// 代价：并发读也会串行 —— 若将来出现读热点，再考虑改用 `shared_mutex`
  /// （那时**所有**写方法必须改成独占锁，且 `opened_` 检查的纪律不变）。
  mutable std::mutex mutex_;

  /// 话题名 → topic_id 的映射。与 `topic_meta_` 是**同一批数据的两个索引**，
  /// 必须同时更新（`RegisterTopic` 与 `LoadTopicsLocked` 是唯一改它们的地方）。
  std::unordered_map<std::string, uint32_t> topic_ids_;

  /// topic_id → 完整元数据（含类型名、条数、字节数、首末时间、active 标志）。
  /// 这是 `ListTopics` 的数据源，也是 `GetTopicMetadata` 的唯一来源。
  /// **不变量**：`topic_ids_` 与 `topic_meta_` 的键集合语义一一对应
  /// （前者按名字索引、后者按 id 索引）。
  std::unordered_map<uint32_t, TopicMetadata> topic_meta_;

  /// 保留点 id 分配器。**只增不减**（删掉的 id 不回收，避免 id 复用导致
  /// `ReleaseRetentionPoint` 误删新快照）。
  /// 使用者：CreateRetentionPoint
  uint64_t next_retention_point_id_ = 1;

  /// 保留点：id → (创建时刻, 底层 LSM 快照)。
  ///
  /// 【为什么需要它】保留策略（`RunRetention`）要删旧数据，但**不能删掉
  /// 正被某个查询引用的快照**。保留点就是这个"引用的锚"：
  ///  · `CreateRetentionPoint` 建一个快照，之后的删除会跳过它
  ///  · `ReleaseRetentionPoint` 释放，之后才可能被淘汰
  ///
  /// 用 `std::map` 而非 `unordered_map`：遍历时要按 id 有序（日志与测试依赖）。
  std::map<uint64_t,
           std::pair<TimestampNs,
                     std::unique_ptr<storage::lsm_v2::RetentionPoint>>>
      retention_points_;

  /// 当前保留策略（`SetRetentionPolicy` 写入，`RunRetention` 读取）。
  /// 注意：**本类不自动调度** —— 何时调用 `RunRetention` 由上层决定
  /// （`Recorder::Start` 会设置策略，但执行时机由存储层/显式调用驱动）。
  RetentionPolicy retention_;

  /// 质量统计（丢弃数、缺口数、缺失数）。
  /// **这是"事实"，不是"结论"** —— `HasIntegrityIssue()` 只是一个聚合判断，
  /// 真正的完整性报告由 `stratum verify` 生成（见 docs/14 的「不下结论」原则）。
  QualityStats quality_;

  /// 每个话题上一条事件的 source 时间戳，用于检测**时间戳回退**。
  /// **只用于统计，不阻止写入** —— 回退会被计入 `quality_`，
  /// 因为"时间戳非单调"本身是数据事实，不该由存储层替业务决定丢弃。
  std::unordered_map<uint32_t, TimestampNs> last_source_ts_;

  /// 每个话题上一条事件的 seq，用于检测**序号缺口**（同一发布者的丢包）。
  /// 与 `last_source_ts_` 一样，只统计不阻止。
  std::unordered_map<uint32_t, uint32_t> last_seq_;

  /// 会话级事件总数（所有话题之和）。**由 `UpdateQualityOnAppend` 增量维护**，
  /// 不从 `topic_meta_` 重算 —— 所以若 `topic_meta_` 被外部改坏，两者会不一致。
  /// 使用者：GetSessionMetadata、PersistSessionLocked
  uint64_t total_events_ = 0;

  /// 会话级字节总数（所有 payload 之和，**不含元数据开销**）。
  /// 所以它不等于磁盘占用 —— 报告"数据量"时用它，估算"磁盘"时不能用它。
  uint64_t total_bytes_ = 0;

  /// 会话最早事件时刻。`kInvalidTimestamp`（=0）表示**尚未有事件**，
  /// **不是**"时间 0 点" —— 两者在代码里必须区分，否则会算出荒谬的跨度。
  TimestampNs session_start_ns_ = kInvalidTimestamp;

  /// 会话最晚事件时刻。语义同上。
  /// 【不变量】若有事件，则 `session_end_ns_ >= session_start_ns_`；
  /// 若两者都是 `kInvalidTimestamp`，说明会话为空（此时跨度按 0 处理）。
  TimestampNs session_end_ns_ = kInvalidTimestamp;
};

}  // namespace stratum

#endif  // STRATUM_CORE_TIMELINE_IMPL_V2_H_
