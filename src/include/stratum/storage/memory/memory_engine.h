// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 内存存储引擎
//
// ===========================================================================
// 定位
// ===========================================================================
//
// 这是 IStorageEngine 的**参考实现**，用途有三：
//   1. 单元测试的快速后端（无磁盘 IO，毫秒级）
//   2. 上层逻辑（Timeline/Recorder/Query）的行为基准
//   3. 验证"插件式"确实成立 —— 换上它，上层代码一行不改
//
// 它**不用于生产**：数据全在内存，进程退出即丢失。
//
// ===========================================================================
// 实现要点
// ===========================================================================
//
//  - 有序性：std::map<std::string, ...> 保证 key 字节序，满足范围扫描。
//  - 快照：每个 key 维护"版本链"（按 sequence 降序）。快照读时，
//    在版本链上取第一个 sequence <= snapshot.sequence 的版本。
//  - 并发：读写锁（shared_mutex）。读多写少场景下性能好。
//  - 空间回收：写入时顺带裁剪版本链（只保留快照需要的最旧版本之后的部分）。

#ifndef STRATUM_STORAGE_MEMORY_ENGINE_H_
#define STRATUM_STORAGE_MEMORY_ENGINE_H_

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include "stratum/storage/istorage_engine.h"

namespace stratum {
namespace storage {

class MemoryEngine : public IStorageEngine {
 public:
  MemoryEngine() = default;
  ~MemoryEngine() override;

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

  std::string Name() const override { return "memory"; }
  std::string Version() const override { return "1.0.0"; }

  Capabilities GetCapabilities() const override;

  // 测试辅助：当前保留的版本总数（用于验证空间回收是否生效）
  size_t VersionCountForTesting() const;

 private:
  friend class MemoryIterator;

  struct ValueVersion {
    uint64_t sequence = 0;
    std::string value;
    bool is_deletion = false;
  };

  // 版本链按 sequence 降序：versions[0] 最新
  struct Entry {
    std::vector<ValueVersion> versions;
  };

  using Map = std::map<std::string, Entry>;

  // 在版本链上查找对指定快照可见的版本。
  // snapshot_seq == UINT64_MAX 表示"最新视图"。
  static const ValueVersion* FindVisible(const Entry& entry, uint64_t snapshot_seq);

  uint64_t NextSequence() { return ++last_sequence_; }

  // 调用者须持有写锁
  void PutLocked(uint64_t seq, const std::string& key, const std::string& value,
                 bool is_deletion);

  // 重算 oldest_needed_sequence_（调用者须持有写锁）
  void RecomputeOldestNeededLocked();

  mutable std::shared_mutex mutex_;
  Map map_;
  uint64_t last_sequence_ = 0;
  uint64_t oldest_needed_sequence_ = 0;
  bool opened_ = false;

  // 活跃快照的 sequence 集合（用于重算 oldest_needed_sequence_）
  std::multiset<uint64_t> active_snapshot_sequences_;

  mutable std::atomic<uint64_t> writes_total_{0};
  mutable std::atomic<uint64_t> reads_total_{0};
  mutable std::atomic<uint64_t> bytes_written_{0};
  mutable std::atomic<uint64_t> bytes_read_{0};
  mutable std::atomic<uint64_t> live_snapshots_{0};
};

// 内存引擎的快照
class MemorySnapshot : public Snapshot {
 public:
  explicit MemorySnapshot(uint64_t sequence) : sequence_(sequence) {}
  uint64_t sequence() const override { return sequence_; }

 private:
  uint64_t sequence_;
};

// 内存引擎的迭代器
class MemoryIterator : public IStorageEngine::Iterator {
 public:
  // 【C3 修复】物化迭代器：持有 NewIterator 锁内拷贝的快照，
  // 此后不接触引擎的任何共享状态 —— 并发写/Close 均安全。
  explicit MemoryIterator(
      std::vector<std::pair<std::string, std::string>> entries);

  bool Valid() const override;
  void Next() override;
  void Prev() override;
  void Seek(const std::string& target) override;
  void SeekForPrev(const std::string& target) override;
  void SeekToFirst() override;
  void SeekToLast() override;
  std::string key() const override;
  std::string value() const override;
  Status status() const override { return status_; }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
  size_t pos_ = 0;

  // 迭代器是否定位在有效元素上。
  //
  // 【为什么需要它】只靠 `pos_ < entries_.size()` 无法表达"越过 first"：
  // `Prev()` 在 `pos_ == 0` 时无路可退，但 `pos_ = 0` 本身是合法位置
  // （指向第一个元素）。于是"往回走到头"与"停在第一个"无法区分 ——
  // 而 `while (Valid()) Prev();` 就会**死循环**。
  // 这是存储契约测试（StoreContract.IteratorSeekAndPrevBothEngines）暴露的：
  // MemoryIterator 的反向遍历无法终止。
  bool valid_ = true;

  Status status_;
};

// 内存引擎的工厂（用于插件注册）
class MemoryEngineFactory : public IStorageEngineFactory {
 public:
  std::string Name() const override { return "memory"; }
  StorageEnginePtr Create() override {
    return StorageEnginePtr(new MemoryEngine());
  }
};

}  // namespace storage
}  // namespace stratum

#endif  // STRATUM_STORAGE_MEMORY_ENGINE_H_
