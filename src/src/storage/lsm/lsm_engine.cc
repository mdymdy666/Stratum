// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// LSM 引擎实现（SSTable + 引擎主体）

#include "stratum/storage/lsm/lsm_engine.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <set>

namespace stratum {
namespace storage {

namespace {

constexpr uint32_t kSstMagic = 0x53535431u;  // "SST1"
constexpr uint32_t kSstVersion = 1;

void PutFixed32LE(std::string* dst, uint32_t v) {
  char b[4];
  b[0] = static_cast<char>(v & 0xFF);
  b[1] = static_cast<char>((v >> 8) & 0xFF);
  b[2] = static_cast<char>((v >> 16) & 0xFF);
  b[3] = static_cast<char>((v >> 24) & 0xFF);
  dst->append(b, 4);
}

uint32_t GetFixed32LE(const char* p) {
  return static_cast<uint32_t>(static_cast<uint8_t>(p[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return false;
  }
  out->resize(static_cast<size_t>(st.st_size));
  size_t got = 0;
  while (got < out->size()) {
    const ssize_t n = ::read(fd, &(*out)[got], out->size() - got);
    if (n <= 0) break;
    got += static_cast<size_t>(n);
  }
  out->resize(got);
  ::close(fd);
  return true;
}

bool WriteWholeFile(const std::string& path, const std::string& data) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return false;
  }
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n <= 0) {
      ::close(fd);
      return false;
    }
    written += static_cast<size_t>(n);
  }
  ::fsync(fd);
  ::close(fd);
  return true;
}

// 目录内已有的 .sst 文件号（用于恢复时载入）
std::vector<uint64_t> ListTableFiles(const std::string& dir) {
  std::vector<uint64_t> numbers;
  // 用 opendir 遍历，避免依赖 std::filesystem 的异常行为
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) {
    return numbers;
  }
  while (struct dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    const auto pos = name.rfind(".sst");
    if (pos == std::string::npos || pos == 0) {
      continue;
    }
    const std::string num = name.substr(0, pos);
    if (num.find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    numbers.push_back(std::stoull(num));
  }
  ::closedir(d);
  std::sort(numbers.begin(), numbers.end());
  return numbers;
}

}  // namespace

// ---------------------------------------------------------------------------
// SSTable
// ---------------------------------------------------------------------------
Status SSTable::Write(const std::string& path,
                      const std::vector<lsm::MemTable::Entry>& entries,
                      uint64_t* file_size_out) {
  std::string buf;
  PutFixed32LE(&buf, kSstMagic);
  PutFixed32LE(&buf, kSstVersion);

  for (const auto& e : entries) {
    const std::string ikey =
        lsm::MakeInternalKey(e.user_key, e.sequence, e.type);
    PutFixed32LE(&buf, static_cast<uint32_t>(ikey.size()));
    buf.append(ikey);
    PutFixed32LE(&buf, static_cast<uint32_t>(e.value.size()));
    buf.append(e.value);
  }

  // 结束标记
  PutFixed32LE(&buf, 0xFFFFFFFFu);
  PutFixed32LE(&buf, 0);
  PutFixed32LE(&buf, static_cast<uint32_t>(entries.size()));

  if (!WriteWholeFile(path, buf)) {
    return Status::IOError("cannot write SSTable: " + path);
  }
  if (file_size_out != nullptr) {
    *file_size_out = buf.size();
  }
  return Status::OK();
}

Status SSTable::Open(const std::string& path) {
  Close();

  std::string data;
  if (!ReadWholeFile(path, &data)) {
    return Status::IOError("cannot read SSTable: " + path);
  }
  if (data.size() < 12) {
    return Status::Corruption("SSTable too small: " + path);
  }
  if (GetFixed32LE(data.data()) != kSstMagic) {
    return Status::Corruption("SSTable bad magic: " + path);
  }
  if (GetFixed32LE(data.data() + 4) != kSstVersion) {
    return Status::Corruption("SSTable unsupported version: " + path);
  }

  size_t pos = 8;
  while (pos + 4 <= data.size()) {
    const uint32_t klen = GetFixed32LE(data.data() + pos);
    pos += 4;
    if (klen == 0xFFFFFFFFu) {
      break;  // 结束标记
    }
    if (pos + klen + 4 > data.size()) {
      return Status::Corruption("SSTable truncated key: " + path);
    }
    const std::string ikey(data, pos, klen);
    pos += klen;
    const uint32_t vlen = GetFixed32LE(data.data() + pos);
    pos += 4;
    if (pos + vlen > data.size()) {
      return Status::Corruption("SSTable truncated value: " + path);
    }
    const std::string value(data, pos, vlen);
    pos += vlen;

    lsm::MemTable::Entry e;
    uint64_t seq = 0;
    lsm::ValueType type = lsm::kTypeValue;
    if (!lsm::ParseInternalKey(ikey, &e.user_key, &seq, &type)) {
      return Status::Corruption("SSTable bad internal key: " + path);
    }
    e.sequence = seq;
    e.type = type;
    e.value = value;
    entries_.push_back(std::move(e));
  }

  // 按 user_key 升序、同 key 按 sequence 降序排列
  std::sort(entries_.begin(), entries_.end(),
            [](const lsm::MemTable::Entry& a, const lsm::MemTable::Entry& b) {
              if (a.user_key != b.user_key) return a.user_key < b.user_key;
              return a.sequence > b.sequence;
            });

  if (!entries_.empty()) {
    smallest_key_ = entries_.front().user_key;
    largest_key_ = entries_.back().user_key;
  }
  // 记录最大 sequence：重启后据此恢复水位，避免 sequence 回绕
  // 导致旧版本覆盖新版本（见审查发现 C1）。
  for (const auto& e : entries_) {
    if (e.sequence > max_sequence_) {
      max_sequence_ = e.sequence;
    }
  }
  path_ = path;
  file_size_ = data.size();
  opened_ = true;
  return Status::OK();
}

void SSTable::Close() {
  entries_.clear();
  smallest_key_.clear();
  largest_key_.clear();
  opened_ = false;
}

bool SSTable::Get(const std::string& user_key, uint64_t snapshot,
                  std::string* out_value, bool* out_is_deletion) const {
  // 二分定位到该 user_key 的第一个（最新）版本
  auto it = std::lower_bound(
      entries_.begin(), entries_.end(), user_key,
      [](const lsm::MemTable::Entry& e, const std::string& k) { return e.user_key < k; });

  for (; it != entries_.end() && it->user_key == user_key; ++it) {
    if (it->sequence <= snapshot) {
      if (out_value != nullptr) *out_value = it->value;
      if (out_is_deletion != nullptr) {
        *out_is_deletion = (it->type == lsm::kTypeDeletion);
      }
      return true;
    }
  }
  return false;
}

void SSTable::CollectRange(const std::string& begin, const std::string& end,
                           uint64_t snapshot,
                           std::vector<lsm::MemTable::Entry>* out) const {
  if (out == nullptr) return;
  auto it = begin.empty()
                ? entries_.begin()
                : std::lower_bound(entries_.begin(), entries_.end(), begin,
                                   [](const lsm::MemTable::Entry& e,
                                      const std::string& k) {
                                     return e.user_key < k;
                                   });
  for (; it != entries_.end(); ++it) {
    if (!end.empty() && it->user_key >= end) break;
    if (it->sequence > snapshot) continue;
    out->push_back(*it);
  }
}

// ---------------------------------------------------------------------------
// LsmEngine 生命周期
// ---------------------------------------------------------------------------
LsmEngine::~LsmEngine() { Close(); }

Status LsmEngine::Open(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (opened_) {
    return Status::AlreadyExists("lsm engine already open");
  }
  db_path_ = path;

  // 建目录（不存在则创建）
  ::mkdir(path.c_str(), 0755);

  Status s = RecoverLocked();
  if (!s.ok()) {
    return s;
  }

  s = wal_.Open(WalPath(), /*truncate_existing=*/imm_ == nullptr && mem_->Empty());
  if (!s.ok()) {
    return s;
  }

  opened_ = true;
  return Status::OK();
}

Status LsmEngine::RecoverLocked() {
  mem_.reset(new lsm::MemTable());
  imm_.reset();

  // 1) 载入已有 SSTable 文件（按其序号顺序放入 L0）
  const std::vector<uint64_t> files = ListTableFiles(db_path_);
  uint64_t sst_max_seq = 0;  // 所有已落盘数据中的最大 sequence
  for (uint64_t number : files) {
    auto table = std::make_shared<SSTable>();
    Status s = table->Open(TablePath(number));
    if (!s.ok()) {
      continue;  // 单个文件损坏不应阻断启动（对应 EC-13 降级要求）
    }
    levels_[0].push_back(table);
    sst_max_seq = std::max(sst_max_seq, table->max_sequence());
    next_file_number_ = std::max(next_file_number_, number + 1);
  }

  // 【关键】先把 sequence 水位提升到 SSTable 中的最大值。
  //
  // 早期实现只从 WAL 推导水位，而数据落盘后 WAL 会被 Reset，
  // 导致重启后 last_sequence_ 回绕为 0 —— 此后新写入的数据
  // sequence 反而小于 SSTable 中的旧数据，被判定为「旧版本」而丢弃，
  // 表现为「新写入被静默覆盖成旧值」甚至「已删除数据复活」。
  // （审查发现 C1，已由实测复现确认）
  //
  // 注意：必须在 WAL 重放**之前**执行，这样即使 WAL 打不开
  // （损坏/权限问题）水位仍然是安全的。
  last_sequence_.store(
      std::max(last_sequence_.load(), sst_max_seq));

  // 2) 从 WAL 重放（覆盖 SSTable 中的同 key 更新）
  //
  // 【重要】损坏检测与"是否截断 WAL"必须分开决策。
  //
  // 早期实现无条件把 Replay 的返回值当作"读到哪里算哪里"，且
  // Open() 会用 O_TRUNC 打开 WAL —— 只要第一条记录 CRC 不匹配，
  // Replay 就返回 0 条（mem_ 为空）→ 后续 wal_.Open(..., true) 直接
  // 把整个 WAL 截断 → **未落盘数据永久消失，而 Open 返回 OK**。
  // 这正是审查发现 H2 实测到的行为。
  //
  // 现在的规则（对齐 LevelDB 的 log_reader 语义）：
  //   - Replay 返回 OK         → 正常（可能因崩溃截断而提前结束，属正常）
  //   - Replay 返回 kCorruption → **立即失败**，不截断文件，交上层处置
  lsm::WAL replay_wal;
  Status s = replay_wal.Open(WalPath(), false);
  if (s.ok()) {
    uint64_t max_seq = last_sequence_.load();
    uint64_t corrupted_at = 0;
    s = replay_wal.Replay(
        [&](uint64_t seq, lsm::ValueType type, const std::string& key,
            const std::string& value) {
          mem_->Add(seq, type, key, value);
          max_seq = std::max(max_seq, seq);
        },
        &corrupted_at);
    replay_wal.Close();
    if (s.IsCorruption()) {
      // 不截断、不清空：把 WAL 原样保留，让上层（或人工）有机会抢救。
      // 注意此时 last_sequence_ 已在上面从 SSTable 恢复过，是安全的。
      return Status::Corruption(
          "WAL corrupted at offset " + std::to_string(corrupted_at) +
          " — refusing to truncate (data preserved for recovery)");
    }
    if (!s.ok()) {
      return s;
    }
    last_sequence_.store(max_seq);
  }

  return Status::OK();
}

Status LsmEngine::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return Status::OK();
  }

  // 关闭前把内存表落盘并显式 fsync，保证数据不丢
  Status s = FlushMemTableLocked(/*sync=*/true);
  wal_.Sync();
  wal_.Close();

  for (int i = 0; i < kNumLevels; ++i) {
    levels_[i].clear();
  }
  mem_.reset();
  imm_.reset();
  opened_ = false;
  return s;
}

// ---------------------------------------------------------------------------
// 写入路径
// ---------------------------------------------------------------------------
Status LsmEngine::Put(const WriteOptions& options, const std::string& key,
                      const std::string& value) {
  std::vector<std::string> keys{key};
  std::vector<std::string> values{value};
  return PutBatch(options, keys, values);
}

Status LsmEngine::PutBatch(const WriteOptions& options,
                           const std::vector<std::string>& keys,
                           const std::vector<std::string>& values) {
  if (keys.size() != values.size()) {
    return Status::InvalidArgument("keys/values size mismatch");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("engine not open");
  }

  // 只有 kAlways 才逐条 fsync。
  //
  // 早期实现把 kBatch 也当作逐条 fsync，导致默认配置下每条写入都落盘，
  // 吞吐下降约两个数量级（实测 5 万条耗时 66 秒）。
  // kBatch 的语义应为"由引擎决定批量提交时机"，即依赖 WAL 的顺序写
  // 与 Close/Flush 时的显式 fsync 来保证持久性。
  const bool sync = (options.sync == SyncPolicy::kAlways);

  // 1) 先写 WAL（持久化在前，保证崩溃可恢复）
  for (size_t i = 0; i < keys.size(); ++i) {
    const uint64_t seq = NextSequence();
    Status s = wal_.Append(seq, lsm::kTypeValue, keys[i], values[i], sync);
    if (!s.ok()) {
      return s;
    }
    // 2) 再写 MemTable（可见）
    mem_->Add(seq, lsm::kTypeValue, keys[i], values[i]);
    bytes_written_.fetch_add(keys[i].size() + values[i].size(),
                             std::memory_order_relaxed);
  }
  writes_total_.fetch_add(keys.size(), std::memory_order_relaxed);

  // 3) MemTable 满则刷盘
  if (mem_->ApproximateSize() >= flush_threshold_) {
    Status s = FlushMemTableLocked(sync);
    if (!s.ok()) {
      return s;
    }
  }
  return Status::OK();
}

Status LsmEngine::Delete(const WriteOptions& options, const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("engine not open");
  }
  const bool sync = (options.sync != SyncPolicy::kNone);
  const uint64_t seq = NextSequence();
  Status s = wal_.Append(seq, lsm::kTypeDeletion, key, std::string(), sync);
  if (!s.ok()) {
    return s;
  }
  mem_->Add(seq, lsm::kTypeDeletion, key, std::string());
  return Status::OK();
}

Status LsmEngine::DeleteRange(const WriteOptions& /*options*/,
                              const std::string& begin,
                              const std::string& end) {
  // 收集范围内需要删除的 user key。
  //
  // 注意：这里**自己做边界过滤**，不依赖 InternalCollectRange
  // （后者为支持迭代器已改为"不做边界过滤、只收集"）。
  // 边界逻辑分散在不同调用点时必须各自负责，否则会出现误删。
  std::set<std::string> victims;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) {
      return Status::ShutdownInProgress("engine not open");
    }

    std::vector<lsm::MemTable::Entry> all;
    InternalCollectRange(std::string(), std::string(), lsm::kMaxSequenceNumber,
                         &all);
    for (const auto& e : all) {
      if (!begin.empty() && e.user_key < begin) continue;
      if (!end.empty() && e.user_key >= end) continue;
      if (e.type == lsm::kTypeDeletion) continue;  // 已删除的不重复处理
      victims.insert(e.user_key);
    }
  }

  if (victims.empty()) {
    return Status::NotFound("no keys in range");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& key : victims) {
    const uint64_t seq = NextSequence();
    Status s = wal_.Append(seq, lsm::kTypeDeletion, key, std::string(), true);
    if (!s.ok()) {
      return s;
    }
    mem_->Add(seq, lsm::kTypeDeletion, key, std::string());
    writes_total_.fetch_add(1, std::memory_order_relaxed);
  }
  return Status::OK();
}


// ---------------------------------------------------------------------------
// 刷盘与 Compaction
// ---------------------------------------------------------------------------
Status LsmEngine::FlushMemTableLocked(bool /*sync*/) {
  if (mem_->Empty()) {
    return Status::OK();
  }

  // 收集当前内存表的全部条目
  std::vector<lsm::MemTable::Entry> entries;
  mem_->CollectRange(std::string(), std::string(), lsm::kMaxSequenceNumber, &entries);

  if (entries.empty()) {
    mem_.reset(new lsm::MemTable());
    return Status::OK();
  }

  const uint64_t number = next_file_number_++;
  uint64_t file_size = 0;
  Status s = SSTable::Write(TablePath(number), entries, &file_size);
  if (!s.ok()) {
    return s;
  }

  auto table = std::make_shared<SSTable>();
  s = table->Open(TablePath(number));
  if (!s.ok()) {
    return s;
  }
  levels_[0].push_back(table);

  // 数据已落盘，WAL 可以清空
  s = wal_.Reset();
  if (!s.ok()) {
    return s;
  }

  mem_.reset(new lsm::MemTable());

  // L0 文件过多则触发 compaction
  if (levels_[0].size() >= kL0CompactionTrigger) {
    return CompactLocked();
  }
  return Status::OK();
}

Status LsmEngine::CompactLocked() {
  // 首版策略：把 L0 的全部文件与 L1 全部文件合并成新的 L1。
  //
  // 为什么可以对时间轴简化：
  //   时间轴 key 唯一，合并时不需要按 sequence 丢弃旧版本（除 tombstone 外），
  //   只需归并排序 + 去重（保留最大 sequence）。
  std::vector<std::shared_ptr<SSTable>> inputs = levels_[0];
  inputs.insert(inputs.end(), levels_[1].begin(), levels_[1].end());
  if (inputs.empty()) {
    return Status::OK();
  }

  // 归并收集
  std::vector<lsm::MemTable::Entry> merged;
  for (const auto& t : inputs) {
    t->CollectRange(std::string(), std::string(), lsm::kMaxSequenceNumber, &merged);
  }

  // 按 (user_key 升序, sequence 降序) 排序后去重
  std::sort(merged.begin(), merged.end(),
            [](const lsm::MemTable::Entry& a, const lsm::MemTable::Entry& b) {
              if (a.user_key != b.user_key) return a.user_key < b.user_key;
              return a.sequence > b.sequence;
            });
  merged.erase(std::unique(merged.begin(), merged.end(),
                           [](const lsm::MemTable::Entry& a,
                              const lsm::MemTable::Entry& b) {
                             return a.user_key == b.user_key;
                           }),
               merged.end());

  const uint64_t number = next_file_number_++;
  uint64_t file_size = 0;
  Status s = SSTable::Write(TablePath(number), merged, &file_size);
  if (!s.ok()) {
    return s;
  }

  auto table = std::make_shared<SSTable>();
  s = table->Open(TablePath(number));
  if (!s.ok()) {
    return s;
  }

  // 收集被合并掉的输入文件路径。
  //
  // 早期实现这里是个空循环（注释写「删除旧文件」但什么都没做），
  // 导致旧 SSTable 永久留在磁盘上：既泄漏空间，又会在重启时被
  // RecoverLocked 全量重载为 L0，使 L0 文件数随 compaction 次数
  // 单调增长并立刻触发下一轮 compaction，形成正反馈。
  // （审查发现 C2，已由实测复现确认：合并后目录残留 1.sst/2.sst/3.sst）
  std::vector<std::string> obsolete;
  obsolete.reserve(inputs.size());
  for (const auto& t : inputs) {
    if (!t->path().empty()) {
      obsolete.push_back(t->path());
    }
  }
  levels_[0].clear();
  levels_[1].clear();
  levels_[1].push_back(table);

  // 新文件已就绪，删除旧文件。
  // 若删除失败也只是空间浪费，不影响正确性，故不返回错误。
  for (const auto& path : obsolete) {
    if (::unlink(path.c_str()) != 0) {
      // 忽略：文件可能已被移除
    }
  }

  compactions_.fetch_add(1, std::memory_order_relaxed);
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 读取路径
// ---------------------------------------------------------------------------
bool LsmEngine::InternalGet(const std::string& user_key, uint64_t snapshot,
                            std::string* out_value,
                            bool* out_is_deletion) const {
  // 查找顺序：MemTable（最新）→ Immutable → L0（新的优先）→ L1 → L2
  uint64_t seq = 0;
  bool del = false;
  if (mem_ != nullptr && mem_->Get(user_key, snapshot, out_value, &del)) {
    if (out_is_deletion != nullptr) *out_is_deletion = del;
    return !del;
  }
  if (imm_ != nullptr && imm_->Get(user_key, snapshot, out_value, &del)) {
    if (out_is_deletion != nullptr) *out_is_deletion = del;
    return !del;
  }

  for (int level = 0; level < kNumLevels; ++level) {
    // L0 内文件可能有重叠：按文件号从新到旧查
    const auto& files = levels_[level];
    if (level == 0) {
      for (auto it = files.rbegin(); it != files.rend(); ++it) {
        if ((*it)->Get(user_key, snapshot, out_value, &del)) {
          if (out_is_deletion != nullptr) *out_is_deletion = del;
          return !del;
        }
      }
    } else {
      for (const auto& t : files) {
        if (user_key < t->smallest_key() || user_key > t->largest_key()) {
          continue;  // 范围不相交，跳过
        }
        if (t->Get(user_key, snapshot, out_value, &del)) {
          if (out_is_deletion != nullptr) *out_is_deletion = del;
          return !del;
        }
      }
    }
  }
  (void)seq;
  return false;
}

void LsmEngine::InternalCollectRange(const std::string& begin,
                                     const std::string& end, uint64_t snapshot,
                                     std::vector<lsm::MemTable::Entry>* out) const {
  // 说明：这里**不做边界过滤**，把各层的可见条目全部收集起来，
  // 边界过滤统一在 LsmIterator 构造时进行。
  //
  // 理由：边界逻辑分散在各层（MemTable / SSTable）时极易出现不一致
  // （开发中确实踩到：全范围扫描返回 0 条、子范围少 1 条）。
  // 集中到一处后，正确性只依赖一个实现。
  (void)begin;
  (void)end;
  if (out == nullptr) return;
  if (mem_ != nullptr) {
    mem_->CollectRange(std::string(), std::string(), snapshot, out);
  }
  if (imm_ != nullptr) {
    imm_->CollectRange(std::string(), std::string(), snapshot, out);
  }
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& t : levels_[level]) {
      t->CollectRange(std::string(), std::string(), snapshot, out);
    }
  }
}


Status LsmEngine::Get(const ReadOptions& options, const std::string& key,
                      std::string* value) {
  if (value == nullptr) {
    return Status::InvalidArgument("value is null");
  }
  const uint64_t snapshot =
      options.snapshot ? options.snapshot->sequence() : lsm::kMaxSequenceNumber;

  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("engine not open");
  }
  reads_total_.fetch_add(1, std::memory_order_relaxed);

  std::string found;
  bool del = false;
  if (!InternalGet(key, snapshot, &found, &del)) {
    return Status::NotFound("key not found");
  }
  *value = found;
  bytes_read_.fetch_add(key.size() + found.size(), std::memory_order_relaxed);
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 迭代器
//
// 实现方式：把范围内的全部条目收集到内存后排序去重，再顺序返回。
// 对时间轴范围查询（结果集通常有限）可接受；
// 超大范围扫描应改为多路归并迭代器（记录为优化方向）。
// ---------------------------------------------------------------------------
class LsmIterator : public IStorageEngine::Iterator {
 public:
  LsmIterator(std::vector<lsm::MemTable::Entry> entries, std::string begin,
              std::string end)
      : begin_(std::move(begin)), end_(std::move(end)) {
    // 统一处理：排序 -> 去重 -> 边界过滤
    //
    // 排序规则：user_key 升序；同 key 按 sequence 降序（新的在前）
    std::sort(entries.begin(), entries.end(),
              [](const lsm::MemTable::Entry& a, const lsm::MemTable::Entry& b) {
                if (a.user_key != b.user_key) return a.user_key < b.user_key;
                return a.sequence > b.sequence;
              });

    // 去重 + 边界过滤 + 处理 tombstone
    std::string current_key;
    bool has_current = false;
    bool current_is_deletion = false;

    for (auto& e : entries) {
      if (has_current && e.user_key == current_key) {
        continue;  // 同一 key 只保留最新版本（已按 sequence 降序）
      }
      current_key = e.user_key;
      has_current = true;
      current_is_deletion = (e.type == lsm::kTypeDeletion);

      // 边界过滤（半开区间 [begin_, end_)）
      if (!begin_.empty() && e.user_key < begin_) {
        continue;
      }
      if (!end_.empty() && e.user_key >= end_) {
        continue;  // 注意：不能 break，因为后续 key 可能更大，但这里已排序，可以 break
      }
      if (current_is_deletion) {
        continue;  // tombstone 不作为可见条目返回
      }
      entries_.push_back(std::move(e));
    }
    pos_ = 0;
  }

  bool Valid() const override { return pos_ < entries_.size(); }

  void Next() override {
    if (Valid()) ++pos_;
  }

  void Prev() override {
    if (pos_ > 0) --pos_;
  }

  void Seek(const std::string& target) override {
    pos_ = 0;
    while (pos_ < entries_.size() && entries_[pos_].user_key < target) {
      ++pos_;
    }
  }

  void SeekForPrev(const std::string& target) override {
    pos_ = 0;
    size_t best = entries_.size();
    while (pos_ < entries_.size() && entries_[pos_].user_key <= target) {
      best = pos_;
      ++pos_;
    }
    pos_ = (best == entries_.size()) ? entries_.size() : best;
  }

  void SeekToFirst() override { pos_ = 0; }
  void SeekToLast() override {
    pos_ = entries_.empty() ? 0 : entries_.size() - 1;
  }

  std::string key() const override {
    return Valid() ? entries_[pos_].user_key : std::string();
  }
  std::string value() const override {
    return Valid() ? entries_[pos_].value : std::string();
  }
  Status status() const override { return Status::OK(); }

 private:
  std::vector<lsm::MemTable::Entry> entries_;
  std::string begin_;
  std::string end_;
  size_t pos_ = 0;
};


std::unique_ptr<IStorageEngine::Iterator> LsmEngine::NewIterator(
    const ReadOptions& options, const std::string& begin,
    const std::string& end) {
  const uint64_t snapshot =
      options.snapshot ? options.snapshot->sequence() : lsm::kMaxSequenceNumber;

  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<lsm::MemTable::Entry> entries;
  InternalCollectRange(begin, end, snapshot, &entries);
  return std::unique_ptr<IStorageEngine::Iterator>(
      new LsmIterator(std::move(entries), begin, end));
}

// ---------------------------------------------------------------------------
// 快照
// ---------------------------------------------------------------------------
SnapshotPtr LsmEngine::GetSnapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t seq = last_sequence_.load();
  snapshots_.insert(seq);
  live_snapshots_.fetch_add(1, std::memory_order_relaxed);
  return std::make_shared<LsmSnapshot>(seq);
}

void LsmEngine::ReleaseSnapshot(const SnapshotPtr& snapshot) {
  if (!snapshot) return;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = snapshots_.find(snapshot->sequence());
  if (it != snapshots_.end()) {
    snapshots_.erase(it);
    live_snapshots_.fetch_sub(1, std::memory_order_relaxed);
  }
}

// ---------------------------------------------------------------------------
// 运维
// ---------------------------------------------------------------------------
Status LsmEngine::Flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("engine not open");
  }
  Status s = FlushMemTableLocked(/*sync=*/true);
  if (!s.ok()) {
    return s;
  }
  return wal_.Sync();
}

Status LsmEngine::CompactRange(const std::string& /*begin*/,
                               const std::string& /*end*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!opened_) {
    return Status::ShutdownInProgress("engine not open");
  }
  return CompactLocked();
}

EngineStats LsmEngine::GetStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  EngineStats stats;
  stats.writes_total = writes_total_.load(std::memory_order_relaxed);
  stats.reads_total = reads_total_.load(std::memory_order_relaxed);
  stats.bytes_written = bytes_written_.load(std::memory_order_relaxed);
  stats.bytes_read = bytes_read_.load(std::memory_order_relaxed);
  stats.live_snapshots = live_snapshots_.load(std::memory_order_relaxed);
  stats.compactions_total = compactions_.load(std::memory_order_relaxed);

  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& t : levels_[level]) {
      stats.total_keys += t->entry_count();
      stats.total_bytes += t->file_size();
    }
  }
  if (mem_ != nullptr) {
    stats.total_keys += mem_->Count();
    stats.total_bytes += mem_->ApproximateSize();
  }
  return stats;
}

IStorageEngine::Capabilities LsmEngine::GetCapabilities() const {
  Capabilities caps;
  caps.range_delete = true;
  caps.snapshots = true;
  caps.ordered_iteration = true;
  caps.merge_operator = false;
  return caps;
}

size_t LsmEngine::MemTableCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mem_ != nullptr ? mem_->Count() : 0;
}

size_t LsmEngine::LevelFileCount(int level) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (level < 0 || level >= kNumLevels) return 0;
  return levels_[level].size();
}

}  // namespace storage
}  // namespace stratum
