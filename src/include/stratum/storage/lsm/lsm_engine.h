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
/// @brief 快照实现 —— 架构图里没有它，但它是"时间点读"的锚。
///
/// @par 在整体中的位置
/// `IStorageEngine::Snapshot` 的具体实现。持有一个 LSM 的逻辑序号（sequence），
/// 据此决定"哪些版本可见"。**只要它未释放，比它新的版本删除操作不得回收旧数据。**
///
/// @par 与 `StratumDB::RetentionPoint` 的区别（容易混）
///  · `LsmSnapshot` 是**内存对象**，只在进程内有效，不跨重启
///  · `RetentionPoint`（`lsm_v2/stratum_db.h`）是**持久化的保留点**，
///    记录"某个时刻的数据必须留住"，重启后仍生效
///
///   两者配合：`RetentionPoint` 保证数据不被淘汰，`LsmSnapshot` 保证读到一致视图。
///
/// @par 使用者
/// `TimelineImplV2::CreateRetentionPoint`（间接）、`GetStateAt` 的读路径
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
/// @brief 不可变有序文件 —— 架构图里 "SSTable (L0) → Compaction → L1/L2" 那一层。
///
/// @par 格式取舍（文件头"首版取舍"里提过，这里给结论）
/// 采用「追加记录 + **读时建索引**」：
///  · 优点：写入极简单，不需要在写入时维护块索引结构
///  · 代价：**每次打开都要扫全文件建索引**（所以大文件打开慢）
///  · 为什么可以接受：Stratum 的 SSTable 数量少、单文件不大；
///    真成瓶颈时再改"写入时落索引"
///
/// @par 针对时间轴的优化
/// 因为 key 单调递增写入，SSTable 内记录**天然有序** —— 范围扫描可以顺序读，
/// 不需要跳表或二分定位。
///
/// @par 边界
///  · **不做** 压缩（首版无块级前缀压缩）
///  · **不做** 删除标记的物理清除（`DeleteRange` 只写 tombstone，
///    真正的回收由 compaction 完成）
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
/// @brief LSM 存储引擎 —— 架构图里 WAL + MemTable + SSTable + Compaction 的整体。
///
/// @par 在整体中的位置
/// `IStorageEngine` 的生产实现（另一个实现是 `memory_engine.h`，只用于测试）。
/// **完整架构见本文件头部**（那里有 ASCII 图和针对时间轴的优化说明，不在此重复）。
///
/// @par 上层服务
///  · `src/src/storage/lsm_v2/stratum_db.cc` —— 唯一的实际使用者
///    （`StratumDB` 在其上实现时间轴语义，`TimelineImplV2` 再在其上）
///  · `tests/unit/test_storage_contract.cc` —— 契约测试
///  · `tests/integration/test_lsm_*.cc` —— 持久化与恢复
///
/// @par 三个必须理解的约束
/// **① 写入路径固定为 WAL → MemTable**：
///   `PutBatch` 先写 WAL 再进 MemTable。所以"返回成功"意味着**崩溃可恢复**，
///   但不意味着已落 SSTable（那要等 MemTable 满或显式 Flush）。
///
/// **② `DeleteRange` 只写 tombstone**：
///   返回的"删除数量"是估算值，不是真的删掉了那么多条 —— 物理回收靠 compaction。
///   所以磁盘空间**不会**因为 `DeleteRange` 立刻下降。
///
/// **③ Compaction 在后台线程**：
///   会与读写并发。它持自己的锁，不会阻塞 `Put` 太久，但会造成**写入延迟抖动**。
///   这是 LSM 的固有代价，不是本实现的缺陷。
///
/// @par 边界
///  · **不做** 时间轴语义（TimeKey 编码、话题注册）—— 那是 `StratumDB`
///  · **不做** 跨进程锁以外的并发控制 —— 单进程内由本类的锁负责
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
/// @brief LSM 后端的工厂。名字是 `"lsm"`（配置里写 `storage.engine = lsm`）。
///
/// @par 为什么需要它
/// 让"用哪个存储后端"成为**运行时配置**。`cmd_config` 用它的 `ConfigSchema()`
/// 生成配置帮助；`validate` 用它校验后端特有配置项。
///
/// @par 与 `LsmEngine` 的关系
/// `Create()` 只 new 一个 `LsmEngine`（不碰磁盘）；真正的 `Open(path)` 由调用方做。
/// 这个分离让"校验配置"与"真去开库"成为两件可分开的事 —— 见
/// `istorage_engine.h` 里 `IStorageEngineFactory` 的说明。
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
