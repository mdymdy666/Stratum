// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// WAL 损坏检测与恢复语义测试
//
// ===========================================================================
// 为什么必须区分"崩溃截断"与"真实损坏"
// ===========================================================================
//
// 这是 LevelDB log_reader 的核心设计（见 db/log_reader.cc），
// 也是本项目审查发现 H2 的根因：
//
//   崩溃写一半  → 正常 EOF，**不上报损坏**（这是预期内的，进程崩了而已）
//   块内真损坏  → 必须上报，且**绝不能截断文件**（否则数据永久消失）
//
// 早期实现把两者混为一谈：CRC 不匹配就 break，Replay 总是返回 OK，
// 而 Open 又用 O_TRUNC 打开 WAL —— 结果是"Open 返回 OK，数据已永久丢失"。
// 审查员实测复现：
//   Open() 返回 OK → Get(key) = NotFound → 重开后 WAL 从 40 字节变 0 字节

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "stratum/storage/lsm/lsm_engine.h"
#include "stratum/storage/lsm/memtable.h"
#include "test_framework.h"
#include "test_isolation.h"

using namespace stratum;
using namespace stratum::storage;

namespace {

// 读取整个文件
bool ReadFile(const std::string& path, std::string* out) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  struct stat st {};
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
  ::close(fd);
  return true;
}

bool WriteFileAt(const std::string& path, off_t offset, const char* data,
                 size_t n) {
  const int fd = ::open(path.c_str(), O_WRONLY);
  if (fd < 0) {
    return false;
  }
  if (::lseek(fd, offset, SEEK_SET) != offset) {
    ::close(fd);
    return false;
  }
  const ssize_t w = ::write(fd, data, n);
  ::close(fd);
  return w == static_cast<ssize_t>(n);
}

uint64_t FileSize(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(st.st_size);
}

}  // namespace

// ---------------------------------------------------------------------------
// 【H2 验证】WAL CRC 损坏时 Open 必须报错，且不得截断文件
//
// 修复前：Open 返回 OK，WAL 被 O_TRUNC 清空，数据永久丢失。
// ---------------------------------------------------------------------------
STRATUM_TEST(WalCorruption, H2_CorruptedWalIsReportedNotTruncated) {
  const std::string dir = std::string("/tmp/stratum_wal_corrupt_") + UniqueSuffix();
  const std::string wal_path = dir + "/wal.log";
  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }
  if (system(("mkdir -p " + dir).c_str()) != 0) {
    // 忽略
  }

  // --- 1) 直接用 WAL 类写入两条记录（模拟"崩溃前有未落盘数据"）---
  uint64_t size_before = 0;
  {
    lsm::WAL wal;
    STRATUM_CHECK_OK(wal.Open(wal_path, /*truncate_existing=*/true));
    STRATUM_CHECK_OK(wal.Append(1, lsm::kTypeValue, "crashkey1", "v1", true));
    STRATUM_CHECK_OK(wal.Append(2, lsm::kTypeValue, "crashkey2", "v2", true));
    size_before = wal.size_bytes();
    STRATUM_CHECK_OK(wal.Close());
  }

  ctx.param_desc = "写入后 WAL 大小=" + std::to_string(size_before);
  STRATUM_CHECK_TRUE(size_before > 16);

  // --- 2) 篡改第一条记录的 CRC（前 4 字节），模拟位翻转 ---
  //     header = CRC(4) | length(2) | type(1)
  const char garbage[4] = {'\xDE', '\xAD', '\xBE', '\xEF'};
  STRATUM_CHECK_TRUE(WriteFileAt(wal_path, 0, garbage, 4));
  ctx.param_desc += " 已篡改首条记录 CRC";

  // --- 3) Open 必须报告损坏 ---
  {
    auto engine = std::unique_ptr<LsmEngine>(new LsmEngine());
    const Status s = engine->Open(dir);

    ctx.param_desc += " Open=" + s.ToString();

    // 【核心断言 1】必须是 Corruption，而不是 OK
    STRATUM_CHECK_TRUE(s.IsCorruption());
  }

  // --- 4) 文件必须未被截断（数据保留供抢救）---
  const uint64_t size_after = FileSize(wal_path);
  ctx.param_desc += " 损坏后 WAL 大小=" + std::to_string(size_after);

  // 【核心断言 2】WAL 内容必须原样保留
  STRATUM_CHECK_EQ(size_after, size_before);

  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }
}

// ---------------------------------------------------------------------------
// 【H2 验证 · 反面】崩溃截断（文件尾部不完整）不应被当作损坏
//
// 这是 LevelDB 的关键区分：writer 写一半就崩了是**正常**情况，
// 不能因此让数据库打不开。
// ---------------------------------------------------------------------------
STRATUM_TEST(WalCorruption, TruncatedTailIsNormalEofNotCorruption) {
  const std::string dir = std::string("/tmp/stratum_wal_truncate_") + UniqueSuffix();
  const std::string wal_path = dir + "/wal.log";
  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }
  if (system(("mkdir -p " + dir).c_str()) != 0) {
    // 忽略
  }

  uint64_t full_size = 0;
  {
    lsm::WAL wal;
    STRATUM_CHECK_OK(wal.Open(wal_path, true));
    STRATUM_CHECK_OK(wal.Append(1, lsm::kTypeValue, "k1", "v1", true));
    STRATUM_CHECK_OK(wal.Append(2, lsm::kTypeValue, "k2", "v2", true));
    STRATUM_CHECK_OK(wal.Append(3, lsm::kTypeValue, "k3", "v3", true));
    full_size = wal.size_bytes();
    STRATUM_CHECK_OK(wal.Close());
  }

  // 模拟"写第 4 条时崩溃"：把文件截断到中途
  const uint64_t truncated = full_size * 2 / 3;
  STRATUM_CHECK_TRUE(::truncate(wal_path.c_str(),
                                static_cast<off_t>(truncated)) == 0);
  ctx.param_desc = "原始=" + std::to_string(full_size) +
                   " 截断到=" + std::to_string(truncated);

  // Open 必须成功（崩溃截断是正常情况）
  {
    auto engine = std::unique_ptr<LsmEngine>(new LsmEngine());
    const Status s = engine->Open(dir);
    ctx.param_desc += " Open=" + s.ToString();
    STRATUM_CHECK_OK(s);

    // 前两条完整记录应当被恢复
    ReadOptions ro;
    std::string v;
    const Status s1 = engine->Get(ro, "k1", &v);
    ctx.param_desc += " k1=" + s1.ToString();
    STRATUM_CHECK_OK(s1);
    STRATUM_CHECK_EQ(v, std::string("v1"));

    engine->Close();
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }
}

// ---------------------------------------------------------------------------
// 【WAL 格式验证】跨 block 的大记录（>32KB）必须能正确写入与恢复
//
// 这是 LevelDB 用 FULL/FIRST/MIDDLE/LAST 分片的原因。
// 早期自创格式没有分片概念，大 payload 无法安全处理。
// ---------------------------------------------------------------------------
STRATUM_TEST(WalCorruption, LargeRecordSpanningBlocksSurvivesReplay) {
  const std::string dir = std::string("/tmp/stratum_wal_large_") + UniqueSuffix();
  const std::string wal_path = dir + "/wal.log";
  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }
  if (system(("mkdir -p " + dir).c_str()) != 0) {
    // 忽略
  }

  // 200KB 负载：必然跨越多个 32KB block
  const std::string big(200 * 1024, 'B');
  constexpr uint64_t kSeq = 42;

  {
    lsm::WAL wal;
    STRATUM_CHECK_OK(wal.Open(wal_path, true));
    STRATUM_CHECK_OK(wal.Append(kSeq, lsm::kTypeValue, "bigkey", big, true));
    STRATUM_CHECK_OK(wal.Close());
  }

  // 重放：必须完整还原
  {
    lsm::WAL wal;
    STRATUM_CHECK_OK(wal.Open(wal_path, false));
    int count = 0;
    bool matched = false;
    uint64_t corrupted = 0;
    const Status s = wal.Replay(
        [&](uint64_t seq, lsm::ValueType /*type*/, const std::string& key,
            const std::string& value) {
          ++count;
          if (seq == kSeq && key == "bigkey" && value == big) {
            matched = true;
          }
        },
        &corrupted);
    STRATUM_CHECK_OK(s);
    ctx.param_desc = "重放记录数=" + std::to_string(count) +
                     " 内容匹配=" + (matched ? "是" : "否");
    STRATUM_CHECK_EQ(count, 1);
    STRATUM_CHECK_TRUE(matched);
    wal.Close();
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }
}

STRATUM_TEST_MAIN()
