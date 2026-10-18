// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// `stratum probe <shm>` 读取器的**共享内存头部校验**回归
//
// 背景（实测 bug）：
//   读取器 OpenRingReadOnly() 只校验 magic / version / record_size，
//   完全没有校验头部的 capacity 与文件实际长度是否匹配。
//   而主循环直接拿 capacity 做 `last_seq % cap` 的槽位计算：
//     · 一个被截断的 ring（写侧崩溃、磁盘/共享内存被清理）
//     · 一个手工构造/旧版本残留的小文件
//   只要头部 capacity 很大，槽位偏移就会越过 mmap 长度 —— 读取器进程
//   直接崩溃（实测 SIGSEGV），而它本该报"这个 ring 不完整"。
//
// 本用例构造一个"头部声明 capacity=1<<20、文件只有 4KB、committed=200000"
// 的假 ring。修复前：读取器越界（崩溃或读垃圾）；修复后：退出码 6（ENV）
// 并给出可读的原因。
//
// 同时保留一个**正例**：正常的 8 条小 ring 必须仍能读，且 read_ok=3，
// 防止"修越界"变成"所有 ring 都拒绝"。

// 标准库头文件必须最先包含
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "stratum/probe/probe.h"
#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

#ifndef PROBE_CLI_PATH
#define PROBE_CLI_PATH "./stratum"
#endif

const std::string kCli = PROBE_CLI_PATH;

struct CliResult {
  int code = -1;
  std::string output;
};

CliResult RunCli(const std::string& cmd) {
  CliResult r;
  std::FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
  if (p == nullptr) return r;
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), p) != nullptr) r.output += buf;
  const int st = ::pclose(p);
  r.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return r;
}

std::string ShmPath(const char* tag) {
  return std::string("/dev/shm/stratum_rd_") + tag + "_" +
         std::to_string(::getpid());
}

void Remove(const std::string& p) {
  std::error_code ec;
  fs::remove(p, ec);
}

// 用真实写侧建一个正常 ring（容量 8，写 3 条），供正例使用。
void MakeValidRing(const std::string& path) {
  Remove(path);
  stratum_probe_close();
  ::setenv("STRATUM_PROBE_CAPACITY", "8", 1);
  const int rc = stratum_probe_open(path.c_str());
  ::unsetenv("STRATUM_PROBE_CAPACITY");
  STRATUM_CHECK_EQ(rc, 0);
  for (int i = 0; i < 3; ++i) {
    stratum_probe_write("rd/x", "abc", 3);
  }
  stratum_probe_close();
}

// 构造一个头部自称 1<<20 条、实际只有 4KB 的"坏 ring"。
void MakeCorruptRing(const std::string& path) {
  Remove(path);
  stratum_ring_header h;
  std::memset(&h, 0, sizeof(h));
  h.magic = STRATUM_RING_MAGIC;
  h.version = STRATUM_RING_VERSION;
  h.capacity = 1u << 20;
  h.record_size = static_cast<uint32_t>(sizeof(stratum_probe_record));
  h.committed_seq = 200000;   // 声称有 20 万条已提交
  h.write_seq = 200000;

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  STRATUM_CHECK_TRUE(f.is_open());
  f.write(reinterpret_cast<const char*>(&h), sizeof(h));
  // 补到 4KB：远小于 capacity*sizeof(record) ≈ 444MB
  std::string pad(4096 - sizeof(h), '\0');
  f.write(pad.data(), static_cast<std::streamsize>(pad.size()));
  f.close();
}

}  // namespace

// ===========================================================================
// 一、正例：正常 ring 必须照常读（防止修复变成"一律拒绝"）
// ===========================================================================

STRATUM_TEST(ProbeCorruptRing, ValidRingStillReadable) {
  const std::string shm = ShmPath("ok");
  MakeValidRing(shm);

  const CliResult r =
      RunCli(kCli + " probe " + shm + " --once --dry-run --no-stats");
  STRATUM_CHECK_EQ(r.code, 0);
  STRATUM_CHECK_TRUE(r.output.find("\"read_ok\":3") != std::string::npos);
  std::printf("      [正例] 正常 8 条 ring：退出码 0，read_ok=3\n");
  Remove(shm);
  stratum_probe_close();
}

// ===========================================================================
// 二、★ 头部 capacity 与文件长度不符 → 必须拒绝，不能越界读
// ===========================================================================

STRATUM_TEST(ProbeCorruptRing, CorruptCapacityIsRejected) {
  const std::string shm = ShmPath("bad");
  MakeCorruptRing(shm);

  const CliResult r =
      RunCli(kCli + " probe " + shm + " --once --dry-run --no-stats");

  // 修复前：进程越界（code=-1）或读垃圾后以 0/5 退出
  STRATUM_CHECK_TRUE(r.code != 0 && r.code != -1);
  STRATUM_CHECK_EQ(r.code, 6);   // ENV：环境/数据不可用，不是 USAGE
  // 原因必须可读，且指出是长度/容量不匹配
  STRATUM_CHECK_TRUE(r.output.find("容量") != std::string::npos ||
                     r.output.find("capacity") != std::string::npos);
  STRATUM_CHECK_TRUE(r.output.find("长度") != std::string::npos ||
                     r.output.find("字节") != std::string::npos);
  std::printf("      [坏 ring] 头部 capacity=1M / 文件 4KB：退出码 6，报长度不足\n");
  Remove(shm);
  stratum_probe_close();
}

STRATUM_TEST_MAIN()
