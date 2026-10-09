// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// StratumDB：基于 LSMengine（LevelDB 架构）的时间轴 DB 层
//
// ===========================================================================
// 架构定位（用户决策：不重写底层，直接基于 LSMengine 开发 DB 层）
// ===========================================================================
//
//   StratumDB（本文件）      —— 时间轴业务语义
//     └─ LSMengine::DB       —— 完整 LevelDB 架构（SSTable/VersionSet/
//                               MANIFEST/WAL/跳表 MemTable/compaction/snapshot）
//
// 本层只做三件事：
//   1. TimeKey 编码（20 字节定长，topic|ts|seq_in_ts 大端）作为 user key
//   2. 时间轴语义：话题注册 / 批量追加 / 时间范围查询 / 时间点查询 / 保留
//   3. seq_in_ts 反转编码（~seq_in_ts），使"同一时间戳的最新条目"
//      恰好排在 Seek 命中的第一位（ref-01 精读报告建议）
//
// ===========================================================================
// 关键编码决策
// ===========================================================================
//
// seq_in_ts 按位取反后存入 key：
//   seq 大 → stored 小 → 排前
//   时间点查询 Seek(topic, T, stored=0) 命中的第一条
//   = T 时刻 seq 最大的条目，无需扫描同 ts 的所有版本
//
// 时间点查询（方案 B，ref-01 建议）：
//   时间点由 key 空间表达（Seek 定位），LevelDB Snapshot 仅保证并发一致性。
//   保留点 = LevelDB Snapshot（后续可扩展为 MANIFEST 持久化）。

#ifndef STRATUM_STORAGE_LSM_V2_STRATUM_DB_H_
#define STRATUM_STORAGE_LSM_V2_STRATUM_DB_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "LSMengine/db.h"
#include "stratum/core/itimeline.h"
#include "stratum/core/status.h"
#include "stratum/core/types.h"
#include "stratum/storage/lsm_v2/time_key_comparator.h"

namespace stratum {
namespace storage {
namespace lsm_v2 {

// ---------------------------------------------------------------------------
// 保留点：LevelDB Snapshot 的 RAII 封装
// ---------------------------------------------------------------------------
class RetentionPoint {
 public:
  // 【生命周期契约】RetentionPoint 持有底层 DB 的裸指针，
  // 析构时调用 ReleaseSnapshot。因此**必须保证**：
  // 所有 RetentionPoint 都在其所属 StratumDB 销毁**之前**析构。
  // 违反契约 = use-after-free（开发中实测 SIGSEGV，gdb 定位
  // 在 ReleaseSnapshot）。

  RetentionPoint(const LSMengine::Snapshot* snap, LSMengine::DB* db)
      : snap_(snap), db_(db) {}
  ~RetentionPoint() {
    if (snap_ != nullptr && db_ != nullptr) {
      db_->ReleaseSnapshot(snap_);
    }
  }
  RetentionPoint(const RetentionPoint&) = delete;
  RetentionPoint& operator=(const RetentionPoint&) = delete;

  const LSMengine::Snapshot* raw() const { return snap_; }

 private:
  const LSMengine::Snapshot* snap_;
  LSMengine::DB* db_;
};

// ---------------------------------------------------------------------------
// StratumDB
// ---------------------------------------------------------------------------
class StratumDB {
 public:
  StratumDB();
  ~StratumDB();

  StratumDB(const StratumDB&) = delete;
  StratumDB& operator=(const StratumDB&) = delete;

  static Status Open(const std::string& path, StratumDB** out);
  Status Close();

  // --- 时间轴 API ---

  // 注册话题（幂等：同名返回同一 id）。
  Status RegisterTopic(const TopicMetadata& meta, uint32_t* topic_id);

  // 批量追加事件（时间戳可乱序；同一时间戳内 seq_in_ts 自动分配）。
  //
  // sync 语义（对齐 LevelDB WriteOptions.sync）：
  //   false = 不 fsync（WAL 在 OS 缓存，进程崩溃丢最后一批；性能优先）
  //   true  = 每条批量写后 fsync（进程崩溃不丢已提交数据）
  Status AppendBatch(const std::vector<Event>& events, bool sync = false);

  // 时间范围查询 [begin_ns, end_ns)，按时间戳升序。
  // at_point 非空时用该保留点的快照视图查询（时间回溯）。
  Status QueryRange(uint32_t topic_id, TimestampNs begin_ns,
                    TimestampNs end_ns, std::vector<Event>* out,
                    const RetentionPoint* at_point = nullptr);

  // 时间点查询："as_of_ns 时刻（或之前）该话题的最新值"。
  // 通过 Seek + Prev 实现，O(log n)，不再全量扫描。
  // at_point 非空时用该保留点的快照视图查询。
  Status GetStateAt(uint32_t topic_id, TimestampNs as_of_ns,
                    StateAtResult* out,
                    const RetentionPoint* at_point = nullptr);

  Status CountRange(uint32_t topic_id, TimestampNs begin_ns,
                    TimestampNs end_ns, uint64_t* count,
                    const RetentionPoint* at_point = nullptr);

  // 删除 [begin_ns, end_ns) 内的全部事件（保留策略的基础）。
  Status DeleteRange(uint32_t topic_id, TimestampNs begin_ns,
                     TimestampNs end_ns);

  // 创建保留点（LevelDB 快照）。
  Status CreateRetentionPoint(std::unique_ptr<RetentionPoint>* out);

  // 落盘（memtable → SSTable）。
  Status Flush();

  // 触发 compaction（透传 LevelDB）。
  Status Compact();

  // --- 元数据 ---
  Status ListTopics(std::vector<TopicMetadata>* out);

  // --- 通用元数据读写（上层持久化统计用）---

  // 写入元数据 key（持久化，sync）。
  Status MetaPut(const std::string& key, const std::string& value);
  // 读取元数据 key。不存在返回 kNotFound。
  Status MetaGet(const std::string& key, std::string* value);

  // --- 统计 ---
  struct Stats {
    uint64_t total_keys = 0;
    uint64_t total_bytes = 0;
    uint64_t writes = 0;
  };
  Stats GetStats();

 private:
  // topic 名 → id 的注册（写元数据 key + 内存缓存）
  Status RegisterTopicLocked(const TopicMetadata& meta, uint32_t* topic_id);
  // 加载已有话题（Open 时）
  Status LoadTopics();

  // 键构造：事件 key 与元数据 key
  static std::string EventKey(uint32_t topic_id, TimestampNs ts_ns,
                              uint32_t seq_in_ts);
  static std::string MetaKey(const std::string& name);
  static bool DecodeEventKey(const std::string& key, uint32_t* topic_id,
                             TimestampNs* ts_ns, uint32_t* seq_in_ts);

  // 同 (topic, ts) 内的 seq_in_ts 分配（内存计数，写盘时随事件持久化）
  uint32_t NextSeqInTs(uint32_t topic_id, TimestampNs ts_ns);

  // 【B-C2 修复】value 编码：事件的全部语义字段都必须落盘。
  //
  // 早期实现 value 只存 payload，丢弃了 Event.seq / reception_timestamp_ns
  // / instance_key —— 双时间戳（P0 需求 FR-R-3）、序号缺口检测、
  // 延迟统计的数据源全部永久丢失（行级对比发现 B-C2，Critical）。
  //
  // 布局（全大端）：[8B reception][4B seq][4B iklen][iklen B ik][payload]
  static std::string EncodeEventValue(const Event& e);
  static bool DecodeEventValue(const std::string& v, Event* out);

  LSMengine::DB* db_ = nullptr;
  TimeKeyComparator comparator_;
  std::string path_;
  bool opened_ = false;

  std::mutex mutex_;  // 话题注册与 seq 分配的串行化（写路径单写者）
  std::unordered_map<std::string, uint32_t> topic_ids_;
  std::unordered_map<uint32_t, std::string> topic_names_;  // id → name（反向索引）
  uint32_t next_topic_id_ = 1;
  // per (topic, ts) 的 seq_in_ts 计数器：仅需记住"最后时间戳"
  std::unordered_map<uint32_t, std::pair<TimestampNs, uint32_t>> seq_in_ts_state_;

  std::atomic<uint64_t> writes_{0};
  std::atomic<uint64_t> bytes_written_{0};
};

}  // namespace lsm_v2
}  // namespace storage
}  // namespace stratum

#endif  // STRATUM_STORAGE_LSM_V2_STRATUM_DB_H_
