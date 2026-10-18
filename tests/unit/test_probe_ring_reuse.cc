// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// probe ring **复用**时的容量一致性回归
//
// 背景（实测 bug）：
//   stratum_probe_open() 复用已存在的共享内存段时，会沿用头部记录的
//   capacity（段创建者的容量），但 mmap 的长度用的是**本次请求**的容量。
//   若本次请求的容量更小（例如读取器/业务用不同的 STRATUM_PROBE_CAPACITY，
//   或默认值从 4096 变成更小的值），g_records 就会指向一块比头部声明
//   容量更小的映射，第 10 条起 `seq % capacity` 算出的槽位直接越过映射
//   末尾 —— 写入触发 SIGSEGV（实测 exit 139）。
//
// 本用例先以容量 1024 建段，再用容量 2 重开同一路径并写 200 条。
// 修复前该用例在 10 条左右崩溃（进程被信号杀死，ctest 失败）。

// 标准库头文件必须最先包含
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "stratum/probe/probe.h"
#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

std::string ReuseShm() {
  return "/dev/shm/stratum_reuse_cap_" + std::to_string(::getpid());
}

void Cleanup(const std::string& path) {
  stratum_probe_close();
  std::error_code ec;
  fs::remove(path, ec);
}

}  // namespace

// ===========================================================================
// 一、★ 复用段时，映射长度必须按**头部**容量算，而不是按本次请求容量
// ===========================================================================

STRATUM_TEST(ProbeRingReuse, ReopenWithSmallerCapacityDoesNotOverrun) {
  const std::string shm = ReuseShm();
  Cleanup(shm);

  // 1) 以容量 1024 建段，写 3 条作为"历史数据"
  ::setenv("STRATUM_PROBE_CAPACITY", "1024", 1);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);
  ::unsetenv("STRATUM_PROBE_CAPACITY");
  for (int i = 0; i < 3; ++i) stratum_probe_write("reuse/x", "a", 1);
  STRATUM_CHECK_EQ(stratum_probe_committed(), static_cast<uint64_t>(3));
  stratum_probe_close();

  // 2) 用更小的容量 2 重开同一段：段已存在，头部的 1024 是权威值
  ::setenv("STRATUM_PROBE_CAPACITY", "2", 1);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);
  ::unsetenv("STRATUM_PROBE_CAPACITY");

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.capacity, static_cast<uint32_t>(1024));
  STRATUM_CHECK_EQ(stratum_probe_committed(), static_cast<uint64_t>(3));

  // 3) 再写 200 条 —— 修复前第 10 条左右就越界（SIGSEGV）
  for (int i = 0; i < 200; ++i) stratum_probe_write("reuse/y", "b", 1);

  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(stratum_probe_committed(), static_cast<uint64_t>(203));
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(203));
  STRATUM_CHECK_EQ(st.dropped_full, static_cast<uint64_t>(0));

  // 4) 越界若没崩而只是写坏，这里会读到错位的数据：抽查最后一条
  const auto* h = static_cast<const stratum_ring_header*>(
      stratum_probe_ring_view());
  const stratum_probe_record* recs = stratum_ring_records_const(h);
  const uint32_t slot = static_cast<uint32_t>(202u % st.capacity);
  STRATUM_CHECK_EQ(recs[slot].seq, static_cast<uint32_t>(202));
  STRATUM_CHECK_EQ(std::string(recs[slot].topic, recs[slot].topic_len),
                   std::string("reuse/y"));

  std::printf("      [复用] 1024 段用容量 2 重开：写 203 条无越界，历史 3 条仍在\n");
  Cleanup(shm);
}

STRATUM_TEST_MAIN()
