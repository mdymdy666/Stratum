// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// LSM 引擎的存储骨架：MemTable / WAL / SSTable
//
// ===========================================================================
// 分层
// ===========================================================================
//
//   写入：PutBatch → WAL 追加（持久化）→ MemTable（可见）
//   读取：MemTable → Immutable MemTable → SSTable（按层，新的优先）
//   刷盘：MemTable 满 → 转 Immutable → 后台落成 SSTable（L0）
//   合并：L0 文件数超阈值 → Compaction → 下沉到 L1，以此类推
//
// ===========================================================================
// 与 Stratum 时间轴的适配
// ===========================================================================
//
// 时间轴的写入是 **append-only**（TimeKey 含时间戳与去重序号，几乎不重复），
// 这带来两个后果：
//   1. 写入总是追加到 key 空间尾部 → LSM 的最优情况
//   2. Compaction 变成"搬运"而非"合并"（没有旧版本可丢）
//
// 因此本引擎在 compaction 上做了针对性处理：见 version_set.h。

#ifndef STRATUM_STORAGE_LSM_MEMTABLE_H_
#define STRATUM_STORAGE_LSM_MEMTABLE_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "stratum/core/status.h"

namespace stratum {
namespace storage {
namespace lsm {

// ---------------------------------------------------------------------------
// 内部键
//
// 布局（与 TimeKey 不同，这是 LSM 内部的键）：
//   user_key + 8 字节大端 tag
//   tag = (sequence << 8) | type
//
// 为什么把 sequence 放在**尾部大端**：
//   这样同一 user_key 的不同版本在字节序上**降序**排列（序号大的在前），
//   读取时从前往后遇到的第一个可见版本就是最新版本 —— 无需反向扫描。
// ---------------------------------------------------------------------------
enum ValueType : uint8_t {
  kTypeDeletion = 0x0,
  kTypeValue = 0x1,
};

constexpr uint64_t kMaxSequenceNumber = (1ULL << 56) - 1;

std::string MakeInternalKey(const std::string& user_key, uint64_t sequence,
                            ValueType type);

// 解析 internal key。失败返回 false。
bool ParseInternalKey(const std::string& internal_key, std::string* user_key,
                      uint64_t* sequence, ValueType* type);

// 从 internal key 提取 user key（不校验）
std::string ExtractUserKey(const std::string& internal_key);

// internal key 比较：先比 user_key，再按 sequence **降序**
int CompareInternalKey(const std::string& a, const std::string& b);

// ---------------------------------------------------------------------------
// MemTable
//
// 内存中的有序表。每个条目带 sequence，支持 MVCC 快照读。
//
// 线程安全：外部串行化（调用者持锁）。这与跳表的"单写多读"约定一致。
// 这里用 std::map 而非跳表作为首版实现 —— 先保证正确性，
// 跳表（已实现）作为后续的性能优化选项。
// ---------------------------------------------------------------------------
class MemTable {
 public:
  MemTable() = default;

  // 写入一条。sequence 必须单调递增。
  void Add(uint64_t sequence, ValueType type, const std::string& user_key,
           const std::string& value);

  // 精确查找（返回对 snapshot 可见的最新版本）
  bool Get(const std::string& user_key, uint64_t snapshot,
           std::string* out_value, bool* out_is_deletion) const;

  // 有序遍历：[begin, end) 范围内的所有条目
  struct Entry {
    std::string user_key;
    uint64_t sequence;
    ValueType type;
    std::string value;
  };
  void CollectRange(const std::string& begin, const std::string& end,
                    uint64_t snapshot, std::vector<Entry>* out) const;

  size_t ApproximateSize() const { return approximate_size_; }
  size_t Count() const { return map_.size(); }
  bool Empty() const { return map_.empty(); }

  // 最小/最大 user key（用于 SSTable 元数据）
  bool GetFirstKey(std::string* user_key) const;
  bool GetLastKey(std::string* user_key) const;

 private:
  // key = internal key，value = 值
  std::map<std::string, std::string> map_;
  size_t approximate_size_ = 0;
};

// ---------------------------------------------------------------------------
// WAL（Write-Ahead Log）
//
// 格式（简化但完整）：
//   [4 字节长度][4 字节 CRC32][payload]
//   payload = [8 字节 sequence][1 字节 type][4 字节 key_len][key][4 字节 val_len][value]
//
// 崩溃恢复：读取到不完整/校验失败的记录即停止（之前的记录仍然有效）。
// ---------------------------------------------------------------------------
// WAL 采用 **LevelDB 的 log 格式**（见参考实现 db/log_format.h、
// log_writer.cc、log_reader.cc），而不是自创简化格式。
//
// 为什么必须这样：自创格式时漏掉的那些设计点，每一条都对应一个真实故障：
//
//   1. **记录按 32KB block 切分**，用 FULL/FIRST/MIDDLE/LAST 标记片段。
//      不切分则超大记录（如 256KB 的 payload）无法安全写入/恢复。
//   2. **Header = CRC(4) + length(2) + type(1)**。把 type 纳入 CRC 覆盖范围，
//      才能发现"片段类型被篡改"（否则重组的记录会静默错位）。
//   3. **CRC 做 Mask 变换**，避免 CRC 与长度字段产生相关性。
//   4. **严格区分"崩溃写一半"与"真损坏"**：
//        - 文件末尾的截断记录 → 视为正常 EOF（writer 崩溃），**不上报损坏**
//        - block 内部长度越界 / CRC 不匹配 → 视为损坏，**必须上报**
//      混为一谈的后果就是本项目的 H2 缺陷：Open 返回 OK，而数据已永久丢失。
//   5. **CRC 失败时丢弃整个 block 剩余部分**，因为 length 字段本身可能已损坏；
//      若继续相信它，可能把真实记录的片段误认成有效记录。
class WAL {
 public:
  WAL() = default;
  ~WAL();

  Status Open(const std::string& path, bool truncate_existing);
  Status Close();

  // 追加一条记录（内部按需切分为多个物理记录）
  Status Append(uint64_t sequence, ValueType type, const std::string& key,
                const std::string& value, bool sync);

  // 重放所有记录。
  //
  // 返回值语义（对齐 LevelDB）：
  //   - OK           : 全部可读记录已重放（可能因崩溃截断而提前结束，属正常）
  //   - kCorruption  : 检测到真实损坏，*truncated_at_out 给出损坏起始偏移
  using ReplayFn = std::function<void(uint64_t sequence, ValueType type,
                                      const std::string& key,
                                      const std::string& value)>;
  Status Replay(const ReplayFn& fn, uint64_t* corrupted_at_out = nullptr);

  // 显式落盘（kBatch 语义下在 Flush/Close 时调用）
  Status Sync();

  // 清除内容（compaction 后调用，因为数据已落到 SSTable）
  Status Reset();

  uint64_t size_bytes() const { return size_bytes_; }
  bool IsOpen() const { return fd_ >= 0; }

 private:
  // 写出一个物理记录（对齐 LevelDB 的 Writer::EmitPhysicalRecord）
  Status EmitPhysicalRecord(int type, const char* ptr, size_t length);

  int fd_ = -1;
  std::string path_;
  uint64_t size_bytes_ = 0;
  uint64_t block_offset_ = 0;  // 当前 block 内已写入的字节数
};

}  // namespace lsm
}  // namespace storage
}  // namespace stratum

#endif  // STRATUM_STORAGE_LSM_MEMTABLE_H_
