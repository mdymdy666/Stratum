// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// LSM 存储引擎
//
// ===========================================================================
// 架构
// ===========================================================================
//
//   ┌──────────────┐   PutBatch    ┌─────────┐
//   │  IStorageEngine │ ──────────▶ │   WAL   │  ← 先持久化（崩溃可恢复）
//   └──────────────┘               └────┬────┘
//                                       │
//                                       ▼
//                                  ┌──────────┐
//                                  │ MemTable │  ← 内存有序表（可见）
//                                  └────┬─────┘
//                         满了 → 转 Immutable、后台刷盘
//                                       │
//                                       ▼
//                               ┌───────────────┐
//                               │ SSTable (L0)  │  ← 不可变有序文件
//                               └───────┬───────┘
//                          L0 超阈值 → Compaction 下沉 L1, L2 ...
//
// ===========================================================================
// 针对 Stratum 时间轴的优化
// ===========================================================================
//
// 时间轴的 key 是 (topic, timestamp, seq) 且**单调递增**写入，
// 因此：
//   1. 写入总落在 key 空间尾部 —— LSM 最优情况（顺序写）
//   2. Compaction 几乎不需要丢弃旧版本（每个 key 唯一）
//   3. **范围扫描是主要读模式** —— 因此 SSTable 保留全部内容并支持顺序遍历
//
// ===========================================================================
// 首版取舍（明确记录，避免后续误解）
// ===========================================================================
//
//  - SSTable 采用"追加记录 + 读时建索引"的简单格式，未做块级前缀压缩。
//    理由：先保证正确性与可恢复性；块编码是纯优化，可后续替换而不影响接口。
//  - Compaction 采用"全量合并下一层"的简单策略，未做 key 范围轮转。
//    理由：时间轴顺序写入下，分层轮转的收益有限。

#ifndef STRATUM_STORAGE_LSM_ENGINE_H_
#define STRATUM_STORAGE_LSM_ENGINE_H_

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "stratum/storage/istorage_engine.h"
#include "stratum/storage/lsm/memtable.h"

namespace stratum {
namespace storage {

// LSM 引擎的快照
class LsmSnapshot : public Snapshot {
 public:
  explicit LsmSnapshot(uint64_t sequence) : sequence_(sequence) {}
  uint64_t sequence() const override { return sequence_; }

 private:
  uint64_t sequence_;
};

// ---------------------------------------------------------------------------
// 一个不可变的 SSTable 文件
//
// 磁盘格式：
//   [magic 4B][version 4B]
//   然后重复：[klen 4B][key][vlen 4B][value]
//   结尾：[klen = 0xFFFFFFFF][count 8B]
//
// 读时一次性载入内存建立有序索引。对时间轴场景（单文件几百 MB）
// 可接受；更大规模时应改为块索引 + 延迟加载。
// ---------------------------------------------------------------------------
class SSTable {
 public:
  SSTable() = default;

  // 从内存条目写成一个文件
  static Status Write(const std::string& path,
                      const std::vector<lsm::MemTable::Entry>& entries,
                      uint64_t* file_size_out);

  // 打开并载入有序索引
  Status Open(const std::string& path);
  void Close();

  // 点查（返回对 snapshot 可见的最新版本）
  bool Get(const std::string& user_key, uint64_t snapshot,
           std::string* out_value, bool* out_is_deletion) const;

  // 范围收集
  void CollectRange(const std::string& begin, const std::string& end,
                    uint64_t snapshot, std::vector<lsm::MemTable::Entry>* out) const;

  bool IsOpen() const { return opened_; }
  const std::string& smallest_key() const { return smallest_key_; }
  const std::string& largest_key() const { return largest_key_; }
  uint64_t file_size() const { return file_size_; }
  uint64_t entry_count() const { return entries_.size(); }

  // 文件路径。compaction 需要它来删除被合并掉的旧文件（见 C2）。
  const std::string& path() const { return path_; }

  // 本文件内最大的 sequence。用于重启后恢复 sequence 水位（见 C1）。
  uint64_t max_sequence() const { return max_sequence_; }

 private:
  // 按 user_key 排序的条目（同 key 多版本按 sequence 降序）
  std::vector<lsm::MemTable::Entry> entries_;
  std::string path_;
  std::string smallest_key_;
  std::string largest_key_;
  uint64_t file_size_ = 0;
  uint64_t max_sequence_ = 0;
  bool opened_ = false;
};

// ---------------------------------------------------------------------------
// LSM 引擎
//
// 线程安全：所有公开方法可并发调用（内部有读写锁 + 后台线程）。
// ---------------------------------------------------------------------------
class LsmEngine : public IStorageEngine {
 public:
  LsmEngine() = default;
  ~LsmEngine() override;

  Status Open(const std::string& path) override;
  Status Close() override;

  Status Put(const WriteOptions& options, const std::string& key,
             const std::string& value) override;
  Status PutBatch(const WriteOptions& options,
                  const std::vector<std::string>& keys,
                  const std::vector<std::string>& values) override;
  Status Get(const ReadOptions& options, const std::string& key,
             std::string* value) override;
  Status Delete(const WriteOptions& options, const std::string& key) override;
  Status DeleteRange(const WriteOptions& options, const std::string& begin,
                     const std::string& end) override;

  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options,
                                        const std::string& begin,
                                        const std::string& end) override;

  SnapshotPtr GetSnapshot() override;
  void ReleaseSnapshot(const SnapshotPtr& snapshot) override;

  Status Flush() override;
  Status CompactRange(const std::string& begin,
                      const std::string& end) override;
  EngineStats GetStats() const override;

  std::string Name() const override { return "lsm"; }
  std::string Version() const override { return "0.1.0"; }
  Capabilities GetCapabilities() const override;

  // --- 诊断 / 测试辅助 ---
  size_t MemTableCount() const;
  size_t LevelFileCount(int level) const;
  uint64_t CurrentSequence() const { return last_sequence_.load(); }

  // 供测试降低刷盘阈值
  void SetMemTableFlushThreshold(size_t bytes) { flush_threshold_ = bytes; }

 private:
  friend class LsmIterator;

  // 内部：合并所有层的数据（供读路径使用）
  bool InternalGet(const std::string& user_key, uint64_t snapshot,
                   std::string* out_value, bool* out_is_deletion) const;
  void InternalCollectRange(const std::string& begin, const std::string& end,
                            uint64_t snapshot,
                            std::vector<lsm::MemTable::Entry>* out) const;

  // 把当前 MemTable 刷成 SSTable（调用者持写锁）
  Status FlushMemTableLocked(bool sync);
  // 执行一次 compaction（调用者持写锁）
  Status CompactLocked();

  // 文件路径工具
  std::string TablePath(uint64_t number) const {
    return db_path_ + "/" + std::to_string(number) + ".sst";
  }
  std::string WalPath() const { return db_path_ + "/wal.log"; }

  // 恢复：从 WAL 重放 + 载入已有 SSTable
  Status RecoverLocked();

  uint64_t NextSequence() { return ++last_sequence_; }

  mutable std::mutex mutex_;  // 简化：读写共用一把锁（首版正确性优先）

  std::string db_path_;
  bool opened_ = false;

  lsm::WAL wal_;
  std::unique_ptr<lsm::MemTable> mem_;
  std::unique_ptr<lsm::MemTable> imm_;  // 待刷盘的不可变表
  size_t flush_threshold_ = 4 * 1024 * 1024;  // 4MB

  // 分层文件（level 0 可有重叠；level>=1 应无重叠）
  std::vector<std::shared_ptr<SSTable>> levels_[3];

  std::atomic<uint64_t> last_sequence_{0};
  uint64_t next_file_number_ = 1;

  // 快照管理
  std::multiset<uint64_t> snapshots_;
  std::atomic<uint64_t> live_snapshots_{0};

  // 统计
  mutable std::atomic<uint64_t> writes_total_{0};
  mutable std::atomic<uint64_t> reads_total_{0};
  mutable std::atomic<uint64_t> bytes_written_{0};
  mutable std::atomic<uint64_t> bytes_read_{0};
  mutable std::atomic<uint64_t> compactions_{0};

  static constexpr size_t kL0CompactionTrigger = 4;
  static constexpr int kNumLevels = 3;
};

// 引擎工厂（用于插件注册）
class LsmEngineFactory : public IStorageEngineFactory {
 public:
  std::string Name() const override { return "lsm"; }
  StorageEnginePtr Create() override {
    return StorageEnginePtr(new LsmEngine());
  }
};

}  // namespace storage
}  // namespace stratum

#endif  // STRATUM_STORAGE_LSM_ENGINE_H_
