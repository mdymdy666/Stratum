// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 内存存储引擎实现

#include "stratum/storage/memory/memory_engine.h"

#include <algorithm>
#include <limits>
#include <mutex>

namespace stratum {
namespace storage {

namespace {
// 表示"最新视图"的哨兵序号
constexpr uint64_t kLatestSequence = std::numeric_limits<uint64_t>::max();
}  // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
MemoryEngine::~MemoryEngine() = default;

Status MemoryEngine::Open(const std::string& /*path*/) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (opened_) {
    return Status::AlreadyExists("memory engine already open");
  }
  map_.clear();
  active_snapshot_sequences_.clear();
  last_sequence_ = 0;
  oldest_needed_sequence_ = 0;
  opened_ = true;
  return Status::OK();
}

Status MemoryEngine::Close() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (!opened_) {
    return Status::OK();
  }
  map_.clear();
  active_snapshot_sequences_.clear();
  opened_ = false;
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 内部：可见性查找
//
// 版本链按 sequence 降序。返回第一个 sequence <= snapshot_seq 的版本。
// ---------------------------------------------------------------------------
const MemoryEngine::ValueVersion* MemoryEngine::FindVisible(
    const Entry& entry, uint64_t snapshot_seq) {
  // 版本链按 sequence 降序（versions[0] 最新）。
  // 目标：找到"对给定快照可见"的版本。
  //
  // 语义要点：快照的 sequence = 创建快照那一刻已分配的最后一个序号。
  // 因此 sequence <= snapshot_seq 的版本才对该快照可见。
  //
  // 若整条链都不可见，返回 nullptr 表示"该 key 在快照时刻尚不存在"。
  for (const ValueVersion& v : entry.versions) {
    if (v.sequence <= snapshot_seq) {
      return &v;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// 内部：写入
// ---------------------------------------------------------------------------
void MemoryEngine::PutLocked(uint64_t seq, const std::string& key,
                             const std::string& value, bool is_deletion) {
  Entry& entry = map_[key];

  ValueVersion v;
  v.sequence = seq;
  v.value = value;
  v.is_deletion = is_deletion;

  // 头插，保持降序
  entry.versions.insert(entry.versions.begin(), std::move(v));

  // 空间回收：如果版本数超过 2 且没有快照需要更旧的版本，裁剪尾部。
  // 保留 2 个的作用：即使没有快照，也便于调试与最近一次覆盖的可见性。
  const uint64_t keep_from = oldest_needed_sequence_;
  while (entry.versions.size() > 2) {
    const ValueVersion& last_ver = entry.versions.back();
    (void)last_ver;
    const ValueVersion& second_last = entry.versions[entry.versions.size() - 2];
    // 若倒数第二个版本已经 <= keep_from，则最后一个永远不会被任何快照看到
    if (keep_from == 0 ? true : second_last.sequence <= keep_from) {
      entry.versions.pop_back();
    } else {
      break;
    }
  }

  bytes_written_.fetch_add(key.size() + value.size(),
                           std::memory_order_relaxed);
  writes_total_.fetch_add(1, std::memory_order_relaxed);
}

void MemoryEngine::RecomputeOldestNeededLocked() {
  if (active_snapshot_sequences_.empty()) {
    oldest_needed_sequence_ = 0;
  } else {
    oldest_needed_sequence_ = *active_snapshot_sequences_.begin();
  }
}

// ---------------------------------------------------------------------------
// 写入接口
// ---------------------------------------------------------------------------
Status MemoryEngine::Put(const WriteOptions& /*options*/,
                         const std::string& key, const std::string& value) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("engine not open");
  }
  PutLocked(NextSequence(), key, value, false);
  return Status::OK();
}

Status MemoryEngine::PutBatch(const WriteOptions& /*options*/,
                              const std::vector<std::string>& keys,
                              const std::vector<std::string>& values) {
  if (keys.size() != values.size()) {
    return Status::InvalidArgument("keys/values size mismatch");
  }
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("engine not open");
  }
  // 整批在同一把锁内完成，保证原子性
  for (size_t i = 0; i < keys.size(); ++i) {
    PutLocked(NextSequence(), keys[i], values[i], false);
  }
  return Status::OK();
}

Status MemoryEngine::Delete(const WriteOptions& /*options*/,
                            const std::string& key) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("engine not open");
  }
  // 记录一个删除标记版本（不直接从 map 移除，否则旧快照会读到错误结果）
  PutLocked(NextSequence(), key, std::string(), true);
  return Status::OK();
}

Status MemoryEngine::DeleteRange(const WriteOptions& /*options*/,
                                 const std::string& begin,
                                 const std::string& end) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("engine not open");
  }

  auto it = begin.empty() ? map_.begin() : map_.lower_bound(begin);
  size_t touched = 0;
  while (it != map_.end()) {
    if (!end.empty() && it->first >= end) {
      break;
    }
    // 为每个被覆盖的 key 写一个删除标记
    const std::string key_copy = it->first;
    ++it;  // 先自增，PutLocked 可能使迭代器失效（map 插入不影响其他元素，但为稳妥起见）
    PutLocked(NextSequence(), key_copy, std::string(), true);
    ++touched;
  }

  if (touched == 0) {
    return Status::NotFound("no keys in range");
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 读取接口
// ---------------------------------------------------------------------------
Status MemoryEngine::Get(const ReadOptions& options, const std::string& key,
                         std::string* value) {
  if (value == nullptr) {
    return Status::InvalidArgument("value is null");
  }

  const uint64_t snapshot_seq =
      options.snapshot ? options.snapshot->sequence() : kLatestSequence;

  std::shared_lock<std::shared_mutex> lock(mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("engine not open");
  }

  reads_total_.fetch_add(1, std::memory_order_relaxed);

  auto it = map_.find(key);
  if (it == map_.end()) {
    return Status::NotFound("key not found");
  }

  const ValueVersion* v = FindVisible(it->second, snapshot_seq);
  if (v == nullptr) {
    // 该 key 在当前快照下尚不存在（写于快照之后）
    return Status::NotFound("key not visible at snapshot");
  }
  if (v->is_deletion) {
    return Status::NotFound("key deleted");
  }

  *value = v->value;
  bytes_read_.fetch_add(key.size() + v->value.size(),
                        std::memory_order_relaxed);
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 迭代器
// ---------------------------------------------------------------------------
std::unique_ptr<IStorageEngine::Iterator> MemoryEngine::NewIterator(
    const ReadOptions& options, const std::string& begin,
    const std::string& end) {
  const uint64_t snapshot_seq =
      options.snapshot ? options.snapshot->sequence() : kLatestSequence;

  // 【C3 修复】锁内物化快照。
  //
  // 早期实现把 &map_ 裸指针交给迭代器后立即释放锁：
  // 并发 Put 的红黑树 rebalance 与 versions.insert（PutLocked:84）
  // 会与迭代器的遍历形成数据竞争（TSAN 实证：FindVisible:63 读 vs
  // PutLocked:84 写，后果含红黑树遍历失控），违反
  // "迭代期间写入不影响已创建的迭代器"契约。
  //
  // 现在在锁内把可见条目**值拷贝**成物化快照，迭代器只操作自己的
  // 副本，语义与 LsmIterator 一致。代价是一次拷贝（参考实现可接受；
  // 追求零拷贝需 SuperVersion 引用计数方案，见 review-02 C3）。
  std::vector<std::pair<std::string, std::string>> entries;
  {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& kv : map_) {
      const std::string& uk = kv.first;
      if (!begin.empty() && uk < begin) continue;
      if (!end.empty() && uk >= end) break;
      const ValueVersion* v = FindVisible(kv.second, snapshot_seq);
      if (v == nullptr || v->is_deletion) continue;
      entries.emplace_back(uk, v->value);
    }
  }
  return std::unique_ptr<IStorageEngine::Iterator>(
      new MemoryIterator(std::move(entries)));
}

MemoryIterator::MemoryIterator(
    std::vector<std::pair<std::string, std::string>> entries)
    : entries_(std::move(entries)) {}

bool MemoryIterator::Valid() const {
  return valid_ && pos_ < entries_.size();
}

void MemoryIterator::Next() {
  if (Valid()) ++pos_;
}

void MemoryIterator::Prev() {
  // 【越过 first 必须变无效】否则 `while (Valid()) Prev();` 死循环 ——
  // pos_ == 0 是"指向第一个元素"的合法位置，不能再减，
  // 但只看 pos_ < size 的话 Valid() 会一直 true。
  // 这条由契约测试 StoreContract.IteratorSeekAndPrev 抓到（Memory 与 LSM 同款）。
  if (!Valid()) return;
  if (pos_ > 0) {
    --pos_;
  } else {
    valid_ = false;   // 已在 first，再往回走 → 越界
  }
}

void MemoryIterator::Seek(const std::string& target) {
  pos_ = 0;
  valid_ = true;
  while (Valid() && entries_[pos_].first < target) ++pos_;
}

void MemoryIterator::SeekForPrev(const std::string& target) {
  pos_ = 0;
  valid_ = true;
  size_t best = entries_.size();
  while (pos_ < entries_.size() && entries_[pos_].first <= target) {
    best = pos_;
    ++pos_;
  }
  pos_ = best;
  // best == size 表示"没有任何 key <= target" → 定位失败
  valid_ = (best < entries_.size());
}

void MemoryIterator::SeekToFirst() {
  pos_ = 0;
  valid_ = true;
}

void MemoryIterator::SeekToLast() {
  pos_ = entries_.empty() ? 0 : entries_.size() - 1;
  valid_ = !entries_.empty();
}

std::string MemoryIterator::key() const {
  return Valid() ? entries_[pos_].first : std::string();
}

std::string MemoryIterator::value() const {
  return Valid() ? entries_[pos_].second : std::string();
}

// ---------------------------------------------------------------------------
// 快照
// ---------------------------------------------------------------------------
SnapshotPtr MemoryEngine::GetSnapshot() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  const uint64_t seq = last_sequence_;
  active_snapshot_sequences_.insert(seq);
  RecomputeOldestNeededLocked();
  live_snapshots_.fetch_add(1, std::memory_order_relaxed);
  return std::make_shared<MemorySnapshot>(seq);
}

void MemoryEngine::ReleaseSnapshot(const SnapshotPtr& snapshot) {
  if (!snapshot) return;
  std::unique_lock<std::shared_mutex> lock(mutex_);
  auto it = active_snapshot_sequences_.find(snapshot->sequence());
  if (it != active_snapshot_sequences_.end()) {
    active_snapshot_sequences_.erase(it);
    RecomputeOldestNeededLocked();
    live_snapshots_.fetch_sub(1, std::memory_order_relaxed);
  }
}

// ---------------------------------------------------------------------------
// 运维
// ---------------------------------------------------------------------------
Status MemoryEngine::Flush() {
  // 内存引擎无持久化需求，刷盘是空操作。
  return Status::OK();
}

Status MemoryEngine::CompactRange(const std::string& /*begin*/,
                                  const std::string& /*end*/) {
  // 内存引擎无 compaction 概念；提供一个"压缩空间"的实现：
  // 在没有快照的情况下，把所有版本链裁剪到只剩最新版本。
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (!active_snapshot_sequences_.empty()) {
    return Status::OK();  // 有快照时不能裁剪
  }
  for (auto& kv : map_) {
    if (kv.second.versions.size() > 1) {
      kv.second.versions.resize(1);
    }
  }
  return Status::OK();
}

size_t MemoryEngine::VersionCountForTesting() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  size_t total = 0;
  for (const auto& kv : map_) {
    total += kv.second.versions.size();
  }
  return total;
}

EngineStats MemoryEngine::GetStats() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  EngineStats stats;
  stats.writes_total = writes_total_.load(std::memory_order_relaxed);
  stats.reads_total = reads_total_.load(std::memory_order_relaxed);
  stats.bytes_written = bytes_written_.load(std::memory_order_relaxed);
  stats.bytes_read = bytes_read_.load(std::memory_order_relaxed);
  stats.live_snapshots = live_snapshots_.load(std::memory_order_relaxed);
  stats.total_keys = map_.size();
  for (const auto& kv : map_) {
    stats.total_bytes += kv.first.size();
    if (!kv.second.versions.empty()) {
      stats.total_bytes += kv.second.versions[0].value.size();
    }
  }
  return stats;
}

IStorageEngine::Capabilities MemoryEngine::GetCapabilities() const {
  Capabilities caps;
  caps.range_delete = true;
  caps.snapshots = true;
  caps.ordered_iteration = true;
  caps.merge_operator = false;
  return caps;
}

}  // namespace storage
}  // namespace stratum
