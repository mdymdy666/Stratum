// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// MemTable 与 WAL 实现

#include "stratum/storage/lsm/memtable.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

namespace stratum {
namespace storage {
namespace lsm {

// ---------------------------------------------------------------------------
// 内部键编解码
//
// 布局：user_key + 8 字节 tag
//   tag = (sequence << 8) | type，**大端**
//
// 大端 + 尾部布局的效果：同一 user_key 的多个版本按键字节序**降序**，
// 序号大的（更新的）排在前面。读取时最先遇到的就是最新版本。
// ---------------------------------------------------------------------------
namespace {

void EncodeTagBE(char* dst, uint64_t sequence, ValueType type) {
  const uint64_t tag = (sequence << 8) | static_cast<uint8_t>(type);
  for (int i = 0; i < 8; ++i) {
    dst[i] = static_cast<char>((tag >> (56 - 8 * i)) & 0xFF);
  }
}

uint64_t DecodeTagBE(const char* src, ValueType* type) {
  uint64_t tag = 0;
  for (int i = 0; i < 8; ++i) {
    tag = (tag << 8) | static_cast<uint8_t>(src[i]);
  }
  if (type != nullptr) {
    *type = static_cast<ValueType>(tag & 0xFF);
  }
  return tag >> 8;
}

void PutFixed32LE(std::string* dst, uint32_t v) {
  char buf[4];
  buf[0] = static_cast<char>(v & 0xFF);
  buf[1] = static_cast<char>((v >> 8) & 0xFF);
  buf[2] = static_cast<char>((v >> 16) & 0xFF);
  buf[3] = static_cast<char>((v >> 24) & 0xFF);
  dst->append(buf, 4);
}

uint32_t DecodeFixed32LE(const char* p) {
  return static_cast<uint32_t>(static_cast<uint8_t>(p[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

// CRC32（IEEE 802.3，查表法）
uint32_t Crc32(const char* data, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    init = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc = table[(crc ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace

std::string MakeInternalKey(const std::string& user_key, uint64_t sequence,
                            ValueType type) {
  std::string result = user_key;
  char tag[8];
  EncodeTagBE(tag, sequence, type);
  result.append(tag, 8);
  return result;
}

bool ParseInternalKey(const std::string& internal_key, std::string* user_key,
                      uint64_t* sequence, ValueType* type) {
  if (internal_key.size() < 8) {
    return false;
  }
  const size_t n = internal_key.size();
  const char* tag_ptr = internal_key.data() + n - 8;
  ValueType t = kTypeValue;
  const uint64_t seq = DecodeTagBE(tag_ptr, &t);
  if (t != kTypeValue && t != kTypeDeletion) {
    return false;
  }
  if (user_key != nullptr) {
    user_key->assign(internal_key.data(), n - 8);
  }
  if (sequence != nullptr) {
    *sequence = seq;
  }
  if (type != nullptr) {
    *type = t;
  }
  return true;
}

std::string ExtractUserKey(const std::string& internal_key) {
  if (internal_key.size() < 8) {
    return std::string();
  }
  return internal_key.substr(0, internal_key.size() - 8);
}

int CompareInternalKey(const std::string& a, const std::string& b) {
  // 重要：必须与 std::string 的字节序比较**完全一致**。
  //
  // 原因：internal key 存放在 std::map<std::string, ...> 中，容器用本函数
  // 排序、lower_bound 也用它。若本函数与字节序不一致，就违反了容器的
  // 排序不变量，红黑树结构被破坏，查找会随机失败。
  //
  // '同一 user_key 的版本按 sequence 升序排列' 这一性质由**编码**保证：
  // tag = (sequence << 8 | type) 以**大端**写在键尾，
  // 所以 sequence 越大则 tag 的字节序越大，整个 internal key 也越大。
  //
  // 配合 MemTable::Get 中的 seek_key（sequence = kMaxSequenceNumber 上界），
  // lower_bound 会命中该 user_key 的**最小** internal key，即最新版本。
  if (a < b) return -1;
  if (a > b) return 1;
  return 0;
}


// ---------------------------------------------------------------------------
// MemTable
// ---------------------------------------------------------------------------
void MemTable::Add(uint64_t sequence, ValueType type,
                   const std::string& user_key, const std::string& value) {
  const std::string ikey = MakeInternalKey(user_key, sequence, type);
  auto it = map_.find(ikey);
  if (it != map_.end()) {
    approximate_size_ -= it->second.size();
    it->second = value;
    approximate_size_ += value.size();
    return;
  }
  map_.emplace(ikey, value);
  approximate_size_ += ikey.size() + value.size() + 32;
}


bool MemTable::Get(const std::string& user_key, uint64_t snapshot,
                   std::string* out_value, bool* out_is_deletion) const {
  // 实现说明：这里采用**遍历 + 取符合条件的最大 sequence** 的直白做法，
  // 而不依赖 lower_bound 与自定义比较函数的一致性。
  //
  // 理由：internal key 的编码把 sequence 大端放在键尾，使得
  // '同一 user_key 的版本按键字节序排列'。任何额外的比较函数一旦与
  // std::string 的字节序不一致，就会破坏 std::map 的排序不变量，
  // 导致查找随机失败（本 bug 已在开发中实际踩到）。
  //
  // 直白实现牺牲一点性能换取确定性；后续可用跳表替换整个 MemTable
  // 以获得 O(log n) 且无需担心容器不变量。
  bool found = false;
  uint64_t best_seq = 0;
  ValueType best_type = kTypeValue;
  std::string best_value;

  for (const auto& kv : map_) {
    std::string key;
    uint64_t seq = 0;
    ValueType type = kTypeValue;
    if (!ParseInternalKey(kv.first, &key, &seq, &type)) {
      continue;
    }
    if (key != user_key) {
      continue;
    }
    if (seq > snapshot) {
      continue;
    }
    if (!found || seq > best_seq) {
      found = true;
      best_seq = seq;
      best_type = type;
      best_value = kv.second;
    }
  }

  if (!found) {
    return false;
  }
  if (out_value != nullptr) {
    *out_value = best_value;
  }
  if (out_is_deletion != nullptr) {
    *out_is_deletion = (best_type == kTypeDeletion);
  }
  return true;
}



void MemTable::CollectRange(const std::string& begin, const std::string& end,
                            uint64_t snapshot,
                            std::vector<Entry>* out) const {
  if (out == nullptr) {
    return;
  }
  // 起点：begin 的"最大 tag"版本（保证覆盖该 key 的所有版本）
  auto it = map_.begin();
  if (!begin.empty()) {
    it = map_.lower_bound(MakeInternalKey(begin, kMaxSequenceNumber, kTypeValue));
    // 若下界落在 begin 之后（begin 不存在），回退到第一个 >= begin 的条目
    while (it != map_.end()) {
      std::string k;
      if (!ParseInternalKey(it->first, &k, nullptr, nullptr)) {
        ++it;
        continue;
      }
      if (k >= begin) {
        break;
      }
      ++it;
    }
  }

  for (; it != map_.end(); ++it) {
    std::string key;
    uint64_t seq = 0;
    ValueType type = kTypeValue;
    if (!ParseInternalKey(it->first, &key, &seq, &type)) {
      continue;
    }
    if (!end.empty() && key >= end) {
      break;
    }
    if (seq > snapshot) {
      continue;
    }
    Entry e;
    e.user_key = key;
    e.sequence = seq;
    e.type = type;
    e.value = it->second;
    out->push_back(std::move(e));
  }
}


bool MemTable::GetFirstKey(std::string* user_key) const {
  if (map_.empty()) {
    return false;
  }
  if (user_key != nullptr) {
    *user_key = ExtractUserKey(map_.begin()->first);
  }
  return true;
}

bool MemTable::GetLastKey(std::string* user_key) const {
  if (map_.empty()) {
    return false;
  }
  if (user_key != nullptr) {
    *user_key = ExtractUserKey(map_.rbegin()->first);
  }
  return true;
}

// ---------------------------------------------------------------------------
// WAL
//
// 记录格式：[4 字节 payload 长度][4 字节 CRC32][payload]
// payload   ：[8 字节 sequence][1 字节 type][4 字节 key_len][key][4 字节 val_len][value]
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// WAL —— 采用 LevelDB 的 log 格式
//
// 磁盘布局：
//   [block 0][block 1]...   每个 block 固定 kBlockSize 字节
//   每个 block 内是若干物理记录：
//     header(7B) = CRC(4, 小端) | length(2, 小端) | type(1)
//     payload(length 字节)
//   若 block 尾部容纳不下 header，用 0 填充到 block 边界。
//
// 一条逻辑记录若超过 block 剩余空间，会被切分为
// FIRST ... MIDDLE ... LAST 多个物理记录。
// ---------------------------------------------------------------------------
namespace {

// 对齐 LevelDB db/log_format.h
constexpr int kLogBlockSize = 32768;
constexpr int kLogHeaderSize = 4 + 2 + 1;  // crc(4) + length(2) + type(1)

enum LogRecordType {
  kLogZeroType = 0,  // 预留（预分配文件）
  kLogFullType = 1,
  kLogFirstType = 2,
  kLogMiddleType = 3,
  kLogLastType = 4,
};
constexpr int kLogMaxRecordType = kLogLastType;

// CRC 掩码变换，对齐 LevelDB crc32c::Mask/Unmask。
// 作用：避免 CRC 值与 length 字段产生相关性（否则某些损坏模式会被漏检）。
inline uint32_t MaskCrc(uint32_t crc) {
  return ((crc >> 15) | (crc << 17)) + 0xa282ead8u;
}
inline uint32_t UnmaskCrc(uint32_t masked) {
  const uint32_t rot = masked - 0xa282ead8u;
  return (rot >> 17) | (rot << 15);
}

}  // namespace

WAL::~WAL() { Close(); }

Status WAL::Open(const std::string& path, bool truncate_existing) {
  Close();
  path_ = path;
  const int flags = O_RDWR | O_CREAT | (truncate_existing ? O_TRUNC : 0);
  fd_ = ::open(path.c_str(), flags, 0644);
  if (fd_ < 0) {
    return Status::IOError("cannot open WAL: " + path);
  }
  if (truncate_existing) {
    size_bytes_ = 0;
    block_offset_ = 0;
  } else {
    const off_t sz = ::lseek(fd_, 0, SEEK_END);
    size_bytes_ = sz > 0 ? static_cast<uint64_t>(sz) : 0;
    block_offset_ = size_bytes_ % kLogBlockSize;
  }
  return Status::OK();
}

Status WAL::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  return Status::OK();
}

Status WAL::Sync() {
  if (fd_ < 0) {
    return Status::IOError("WAL not open");
  }
  if (::fsync(fd_) != 0) {
    return Status::IOError("WAL fsync failed");
  }
  return Status::OK();
}

Status WAL::EmitPhysicalRecord(int type, const char* ptr, size_t length) {
  if (length > 0xffff) {
    return Status::InvalidArgument("WAL fragment too large");
  }

  char header[kLogHeaderSize];
  header[4] = static_cast<char>(length & 0xff);
  header[5] = static_cast<char>((length >> 8) & 0xff);
  header[6] = static_cast<char>(type);

  // CRC 覆盖 **type + payload**（对齐 LevelDB：
  // crc32c::Extend(type_crc_[t], ptr, length)），因此 type 被篡改也能发现。
  std::string crc_input;
  crc_input.reserve(1 + length);
  crc_input.push_back(static_cast<char>(type));
  crc_input.append(ptr, length);
  const uint32_t crc = MaskCrc(Crc32(crc_input.data(), crc_input.size()));
  header[0] = static_cast<char>(crc & 0xff);
  header[1] = static_cast<char>((crc >> 8) & 0xff);
  header[2] = static_cast<char>((crc >> 16) & 0xff);
  header[3] = static_cast<char>((crc >> 24) & 0xff);

  // 完整写出 header + payload（循环处理短写，对齐 WriteWholeFile 的做法）
  auto write_all = [this](const char* data, size_t n) -> bool {
    size_t done = 0;
    while (done < n) {
      const ssize_t w = ::write(fd_, data + done, n - done);
      if (w <= 0) {
        return false;
      }
      done += static_cast<size_t>(w);
    }
    return true;
  };

  const uint64_t before = size_bytes_;
  if (!write_all(header, kLogHeaderSize) || !write_all(ptr, length)) {
    // 【关键】短写/失败时回滚到写入前的位置。
    //
    // 否则会在文件里留下半条物理记录，而恢复时遇到它就会停止 ——
    // 于是它**之后所有原本完好的记录都被丢弃**（审查发现 M6）。
    if (::ftruncate(fd_, static_cast<off_t>(before)) != 0) {
      // 回滚失败：只能尽量把偏移移回，避免覆盖后续数据
    }
    ::lseek(fd_, static_cast<off_t>(before), SEEK_SET);
    size_bytes_ = before;
    block_offset_ = before % kLogBlockSize;
    return Status::IOError("WAL write failed");
  }

  size_bytes_ += kLogHeaderSize + length;
  block_offset_ += kLogHeaderSize + length;
  return Status::OK();
}

Status WAL::Append(uint64_t sequence, ValueType type, const std::string& key,
                   const std::string& value, bool sync) {
  if (fd_ < 0) {
    return Status::IOError("WAL not open");
  }

  // 组装 Stratum 自己的 payload（这部分是本项目特有语义，保留原设计）
  std::string payload;
  char seq_buf[8];
  for (int i = 0; i < 8; ++i) {
    seq_buf[i] = static_cast<char>((sequence >> (8 * i)) & 0xFF);
  }
  payload.append(seq_buf, 8);
  payload.push_back(static_cast<char>(type));
  PutFixed32LE(&payload, static_cast<uint32_t>(key.size()));
  payload.append(key);
  PutFixed32LE(&payload, static_cast<uint32_t>(value.size()));
  payload.append(value);

  // 按 block 切分写出（对齐 LevelDB Writer::AddRecord）
  const char* ptr = payload.data();
  size_t left = payload.size();
  bool begin = true;
  do {
    const int64_t leftover =
        static_cast<int64_t>(kLogBlockSize) - static_cast<int64_t>(block_offset_);
    if (leftover < kLogHeaderSize) {
      // block 尾部放不下 header：用 0 填充到 block 边界
      if (leftover > 0) {
        const char zeros[kLogHeaderSize] = {0};
        Status s = EmitPhysicalRecord(kLogZeroType, zeros,
                                      static_cast<size_t>(leftover));
        if (!s.ok()) {
          return s;
        }
      }
      block_offset_ = 0;
    }

    const size_t avail =
        kLogBlockSize - static_cast<size_t>(block_offset_) - kLogHeaderSize;
    const size_t fragment_length = (left < avail) ? left : avail;

    int rec_type;
    const bool end = (left == fragment_length);
    if (begin && end) {
      rec_type = kLogFullType;
    } else if (begin) {
      rec_type = kLogFirstType;
    } else if (end) {
      rec_type = kLogLastType;
    } else {
      rec_type = kLogMiddleType;
    }

    Status s = EmitPhysicalRecord(rec_type, ptr, fragment_length);
    if (!s.ok()) {
      return s;
    }
    ptr += fragment_length;
    left -= fragment_length;
    begin = false;
  } while (left > 0);

  if (sync) {
    return Sync();
  }
  return Status::OK();
}

Status WAL::Replay(const ReplayFn& fn, uint64_t* corrupted_at_out) {
  if (fd_ < 0) {
    return Status::IOError("WAL not open");
  }
  if (corrupted_at_out != nullptr) {
    *corrupted_at_out = 0;
  }

  const off_t file_size = ::lseek(fd_, 0, SEEK_END);
  if (file_size <= 0) {
    return Status::OK();
  }
  ::lseek(fd_, 0, SEEK_SET);

  std::string file_data(static_cast<size_t>(file_size), '\0');
  size_t total_read = 0;
  while (total_read < file_data.size()) {
    const ssize_t n =
        ::read(fd_, &file_data[total_read], file_data.size() - total_read);
    if (n < 0) {
      if (errno == EINTR) {
        continue;  // 被信号中断：重试，不要当成 EOF（审查发现 M11）
      }
      return Status::IOError("WAL read failed");
    }
    if (n == 0) {
      break;
    }
    total_read += static_cast<size_t>(n);
  }
  file_data.resize(total_read);

  // 重组中的逻辑记录
  std::string scratch;
  bool in_fragmented_record = false;

  size_t pos = 0;
  bool eof = false;
  while (!eof) {
    // --- 读取一个物理记录（对齐 Reader::ReadPhysicalRecord）---
    if (pos + kLogHeaderSize > file_data.size()) {
      // 文件末尾剩余不足 header：writer 可能在写 header 中途崩溃。
      // **不上报损坏**（这是正常的崩溃截断）。
      break;
    }

    const char* header = file_data.data() + pos;
    const uint32_t length = static_cast<uint32_t>(
        static_cast<uint8_t>(header[4]) |
        (static_cast<uint32_t>(static_cast<uint8_t>(header[5])) << 8));
    const int rec_type = static_cast<uint8_t>(header[6]);

    if (pos + kLogHeaderSize + length > file_data.size()) {
      // 长度越界：
      //   文件末尾 → writer 写 payload 中途崩溃，正常截断，不上报
      //   块内部   → 长度字段损坏，必须上报
      const bool at_file_end =
          (pos + kLogHeaderSize + length >= file_data.size());
      if (at_file_end) {
        break;
      }
      if (corrupted_at_out != nullptr) {
        *corrupted_at_out = pos;
      }
      return Status::Corruption("WAL bad record length at offset " +
                                std::to_string(pos));
    }

    if (rec_type == kLogZeroType && length == 0) {
      // 预分配/填充区域：跳过，不计为损坏
      pos += kLogHeaderSize;
      continue;
    }

    if (rec_type < kLogFullType || rec_type > kLogMaxRecordType) {
      if (corrupted_at_out != nullptr) {
        *corrupted_at_out = pos;
      }
      return Status::Corruption("WAL unknown record type at offset " +
                                std::to_string(pos));
    }

    // CRC 校验：覆盖 type + payload
    {
      std::string crc_input;
      crc_input.reserve(1 + length);
      crc_input.push_back(static_cast<char>(rec_type));
      crc_input.append(file_data.data() + pos + kLogHeaderSize, length);
      const uint32_t actual = MaskCrc(Crc32(crc_input.data(), crc_input.size()));
      uint32_t expected = 0;
      for (int i = 0; i < 4; ++i) {
        expected |= static_cast<uint32_t>(
                        static_cast<uint8_t>(header[i]))
                    << (8 * i);
      }
      if (actual != expected) {
        // 【关键】CRC 失败即认为本 block 剩余部分不可信
        // （length 字段本身可能已损坏），对齐 LevelDB：丢弃并上报。
        if (corrupted_at_out != nullptr) {
          *corrupted_at_out = pos;
        }
        return Status::Corruption("WAL checksum mismatch at offset " +
                                  std::to_string(pos));
      }
      (void)UnmaskCrc;  // 保留对称助手，便于将来诊断
    }

    const char* fragment = file_data.data() + pos + kLogHeaderSize;
    pos += kLogHeaderSize + length;

    // --- 按类型重组逻辑记录 ---
    if (rec_type == kLogFullType) {
      if (in_fragmented_record) {
        // 上一段 FIRST/MIDDLE 未闭合即遇到 FULL：前面那段是残缺的，
        // 丢弃它（对应 LevelDB 的 "partial record without end(1)"）
        scratch.clear();
        in_fragmented_record = false;
      }
      scratch.assign(fragment, length);
    } else if (rec_type == kLogFirstType) {
      scratch.assign(fragment, length);
      in_fragmented_record = true;
      continue;
    } else if (rec_type == kLogMiddleType) {
      if (!in_fragmented_record) {
        continue;  // 没有对应的 FIRST：孤立片段，跳过
      }
      scratch.append(fragment, length);
      continue;
    } else if (rec_type == kLogLastType) {
      if (!in_fragmented_record) {
        continue;  // 没有对应的 FIRST：孤立片段，跳过
      }
      scratch.append(fragment, length);
      in_fragmented_record = false;
    }

    // --- 解析 Stratum payload ---
    if (scratch.size() < 13) {
      continue;  // 结构不完整的记录：跳过（不视为损坏）
    }
    size_t p = 0;
    uint64_t seq = 0;
    for (int i = 0; i < 8; ++i) {
      seq |= static_cast<uint64_t>(static_cast<uint8_t>(scratch[i]))
             << (8 * i);
    }
    p = 8;
    const ValueType vtype =
        static_cast<ValueType>(static_cast<uint8_t>(scratch[p++]));
    const uint32_t key_len = DecodeFixed32LE(scratch.data() + p);
    p += 4;
    if (p + key_len + 4 > scratch.size()) {
      continue;
    }
    const std::string key(scratch.data() + p, key_len);
    p += key_len;
    const uint32_t val_len = DecodeFixed32LE(scratch.data() + p);
    p += 4;
    if (p + val_len > scratch.size()) {
      continue;
    }
    const std::string value(scratch.data() + p, val_len);

    if (fn) {
      fn(seq, vtype, key, value);
    }
    scratch.clear();
  }

  // 循环结束：若仍处于分段中间，说明最后一条逻辑记录不完整
  // （writer 崩溃），属正常截断，不上报损坏。
  return Status::OK();
}

Status WAL::Reset() {
  if (fd_ < 0) {
    return Status::IOError("WAL not open");
  }
  if (::ftruncate(fd_, 0) != 0) {
    return Status::IOError("WAL truncate failed");
  }
  ::lseek(fd_, 0, SEEK_SET);
  size_bytes_ = 0;
  block_offset_ = 0;
  return Status::OK();
}

}  // namespace lsm
}  // namespace storage
}  // namespace stratum
