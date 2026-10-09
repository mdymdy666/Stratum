// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// Stratum 存储引擎插件接口（SPI）
//
// 设计要点：
//  1. 这是 Stratum 与存储后端之间的**唯一**契约。任何后端（自研 LSM、LSMengine、
//     内存实现、测试桩）只要实现本接口即可接入。
//  2. 接口刻意保持"窄"：只暴露 LSM 语义必需的操作，不泄漏任何后端特有概念
//     （不提 level、SSTable、memtable）。
//  3. **时间点读（Snapshot）是一等公民**：它不是可选优化，而是 Stratum
//     "定点查询"能力的基石。

#ifndef STRATUM_STORAGE_ISTORAGE_ENGINE_H_
#define STRATUM_STORAGE_ISTORAGE_ENGINE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "stratum/core/status.h"

namespace stratum {
namespace storage {

// ---------------------------------------------------------------------------
// 快照句柄
//
// 语义：快照代表"创建这一刻的数据库一致视图"。
//
// 生死规则（实现者必须遵守）：
//   - 只要快照未被 Release，其视图内的**历史版本数据不得被回收**。
//   - 这是 Stratum 实现"定点查询"的基础：持有一个旧快照，
//     就能读回那个时刻的数据。
//   - 快照是**内存对象**，不跨进程、不跨重启。若要跨重启保留，
//     调用方需自行持久化"保留点"，重启后重建。
// ---------------------------------------------------------------------------
class Snapshot {
 public:
  virtual ~Snapshot() = default;

  // 该快照对应的逻辑序号。仅用于诊断与调试，不保证跨后端可比。
  virtual uint64_t sequence() const = 0;
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

// ---------------------------------------------------------------------------
// 读取选项
// ---------------------------------------------------------------------------
struct ReadOptions {
  // 为空则在"最新"视图上读；否则在指定快照视图上读。
  SnapshotPtr snapshot;

  // 是否验证校验和。默认 true；压测时可关闭以获得更真实的吞吐数字。
  bool verify_checksums = true;

  // 是否填充块缓存。全表扫描场景可置 false 以避免污染缓存。
  bool fill_cache = true;
};

// ---------------------------------------------------------------------------
// 写入选项
// ---------------------------------------------------------------------------
enum class SyncPolicy : uint8_t {
  // 不主动 fsync：最快，进程崩溃可能丢最后一批，断电丢更多。
  kNone = 0,
  // 批量 fsync：平衡档（推荐默认）。
  kBatch = 1,
  // 每次写都 fsync：最安全，吞吐可能下降一个数量级。
  kAlways = 2,
};

struct WriteOptions {
  SyncPolicy sync = SyncPolicy::kBatch;

  // 若为 true，写入后立即可见；否则可能延迟到下次刷盘。
  // 注意：这不影响"是否持久"，持久性由 sync 决定。
  bool immediate_visibility = true;
};

// ---------------------------------------------------------------------------
// 存储引擎统计（用于质量报告与性能观测）
// ---------------------------------------------------------------------------
struct EngineStats {
  uint64_t total_keys = 0;          // 当前可见的 key 总数（近似值即可）
  uint64_t total_bytes = 0;         // 占用磁盘字节数
  uint64_t live_snapshots = 0;      // 当前存活快照数
  uint64_t writes_total = 0;        // 累计写入次数
  uint64_t reads_total = 0;         // 累计读取次数
  uint64_t compactions_total = 0;   // 累计 compaction 次数
  uint64_t bytes_written = 0;       // 累计写入字节（含 compaction 重写）
  uint64_t bytes_read = 0;          // 累计读取字节

  // 写放大 = bytes_written / 用户实际写入字节。
  // 这是时间序列负载在 LSM 上的关键指标。
  double WriteAmplification(uint64_t user_bytes) const {
    return user_bytes == 0 ? 0.0
                           : static_cast<double>(bytes_written) /
                                 static_cast<double>(user_bytes);
  }
};

// ---------------------------------------------------------------------------
// 存储引擎抽象
//
// 线程安全约定：
//   - 所有方法必须可被多个线程并发调用。
//   - 实现方负责内部同步；调用方不承担责任。
//
// 错误约定：
//   - 不存在"抛异常表示失败"的路径；一律通过 Status 返回。
//   - 只有内存分配失败（std::bad_alloc）允许抛出。
// ---------------------------------------------------------------------------
class IStorageEngine {
 public:
  virtual ~IStorageEngine() = default;

  // --- 生命周期 -----------------------------------------------------------

  // 打开（或创建）一个数据库。
  // 失败时返回非 OK 的 Status。
  virtual Status Open(const std::string& path) = 0;

  // 关闭并释放资源。关闭后除析构外不应再调用其他方法。
  virtual Status Close() = 0;

  // --- 基本读写 -----------------------------------------------------------

  virtual Status Put(const WriteOptions& options, const std::string& key,
                     const std::string& value) = 0;

  // 批量原子写：整批要么全部生效，要么全部不生效。
  // keys/values 必须等长。
  virtual Status PutBatch(const WriteOptions& options,
                          const std::vector<std::string>& keys,
                          const std::vector<std::string>& values) = 0;

  // 点查。不存在时返回 Status::NotFound()。
  virtual Status Get(const ReadOptions& options, const std::string& key,
                     std::string* value) = 0;

  virtual Status Delete(const WriteOptions& options,
                        const std::string& key) = 0;

  // 按 key 区间批量删除（**Stratum 环形保留的核心操作**）。
  // 删除 [begin, end) 区间内所有 key。
  // 返回删除的 key 数量近似值（后端可返回上限估计）。
  virtual Status DeleteRange(const WriteOptions& options,
                             const std::string& begin,
                             const std::string& end) = 0;

  // --- 有序遍历（时间轴范围查询的基础）------------------------------------

  // 有序迭代器。用于范围扫描。
  //
  // 语义：
  //   - 迭代器创建后捕获一个一致视图（若指定快照则用快照视图）。
  //   - 迭代期间对数据库的写入不影响已创建的迭代器。
  //   - 必须在销毁前 Close()，或依赖析构自动关闭。
  class Iterator {
   public:
    virtual ~Iterator() = default;

    virtual bool Valid() const = 0;
    virtual void Next() = 0;
    virtual void Prev() = 0;

    // 定位到第一个 >= target 的位置。
    virtual void Seek(const std::string& target) = 0;

    // 定位到第一个 >= target 的位置；若无则定位到最后一个。
    virtual void SeekForPrev(const std::string& target) = 0;

    virtual void SeekToFirst() = 0;
    virtual void SeekToLast() = 0;

    virtual std::string key() const = 0;
    virtual std::string value() const = 0;
    virtual Status status() const = 0;
  };

  // 返回 [begin, end) 范围内的有序迭代器。空字符串表示无边界。
  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options,
                                                 const std::string& begin,
                                                 const std::string& end) = 0;

  // --- 快照（定点查询的基石）----------------------------------------------

  // 创建一个覆盖当前全部写入的快照。
  virtual SnapshotPtr GetSnapshot() = 0;

  // 释放快照。释放后其保护的历史版本可被回收。
  // 传入已释放或不属于本引擎的快照是未定义行为。
  virtual void ReleaseSnapshot(const SnapshotPtr& snapshot) = 0;

  // --- 运维与观测 ---------------------------------------------------------

  // 强制把内存中的数据刷到持久存储。
  virtual Status Flush() = 0;

  // 主动触发一次 compaction（用于压测与运维）。
  // 若为阻塞实现，应在合理时间内返回。
  virtual Status CompactRange(const std::string& begin,
                              const std::string& end) = 0;

  virtual EngineStats GetStats() const = 0;

  // 引擎名称与版本，用于诊断。
  virtual std::string Name() const = 0;
  virtual std::string Version() const = 0;

  // --- 能力协商 -----------------------------------------------------------

  // 后端能力位。调用方可据此调整策略（例如不支持范围删除时退化为逐条删）。
  struct Capabilities {
    bool range_delete = false;       // 支持 DeleteRange
    bool snapshots = false;          // 支持快照
    bool ordered_iteration = false;  // 支持有序遍历（几乎所有实现都应支持）
    bool merge_operator = false;     // 支持自定义合并（预留）
  };

  virtual Capabilities GetCapabilities() const = 0;
};

using StorageEnginePtr = std::unique_ptr<IStorageEngine>;

// ---------------------------------------------------------------------------
// 引擎工厂插件接口
//
// Stratum 通过"注册表 + 工厂"实现插件式存储后端。
// 新增后端只需：
//   1. 实现 IStorageEngine
//   2. 实现 IStorageEngineFactory
//   3. 用 STRATUM_REGISTER_STORAGE_ENGINE 宏注册
// ---------------------------------------------------------------------------
class IStorageEngineFactory {
 public:
  virtual ~IStorageEngineFactory() = default;

  // 工厂名（如 "lsm"、"memory"、"lsmengine"）。用于配置中选择后端。
  virtual std::string Name() const = 0;

  // 创建实例。不在此处打开数据库。
  virtual StorageEnginePtr Create() = 0;

  // 该后端支持的配置项描述（用于生成帮助与校验配置）。
  // 格式：每项一行 "name:type:default:description"。
  virtual std::vector<std::string> ConfigSchema() const { return {}; }
};

}  // namespace storage
}  // namespace stratum

#endif  // STRATUM_STORAGE_ISTORAGE_ENGINE_H_
