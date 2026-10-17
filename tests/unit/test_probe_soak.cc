// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// probe 的 **soak（持续性）测试**
//
// 与其它 probe 测试的分工：
//   其余套件        —— 短时间内的正确性（毫秒到秒级）
//   test_probe_soak —— **持续写入下的稳定性**：
//       · 计数是否单调、是否漂移
//       · 内存是否增长（ring 是预分配的，RSS 不该涨）
//       · 跨多圈覆盖后计数器是否仍然精确
//
// 【为什么需要它】
// 之前的测试最长只跑 14 秒。而埋点库的定位是"长期常开"，
// 它要连续运行几天。短测试发现不了的问题包括：
//   · 计数器缓慢漂移（每次多算/少算一条，一万次后就很明显）
//   · 隐式内存分配（某条路径上漏了预分配，RSS 缓慢上涨）
//   · 覆盖多圈后序号比对失效
//
// 这里用 20 秒压缩验证这些趋势 —— 不是为了替代真正的数小时 soak，
// 而是给"趋势不对"提供一个能在 CI 里跑的检查点。

// 标准库头文件必须最先包含
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "stratum/probe/probe.h"
#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

std::string FreshShm(const char* tag) {
  const std::string p = std::string("/dev/shm/stratum_soak_") + tag;
  std::error_code ec;
  fs::remove(p, ec);
  return p;
}

// 读本进程的常驻内存（KB）。读不到返回 0。
long ReadVmRssKb() {
  std::FILE* f = std::fopen("/proc/self/status", "r");
  if (f == nullptr) return 0;
  char line[256];
  long kb = 0;
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    if (std::strncmp(line, "VmRSS:", 6) == 0) {
      kb = std::atol(line + 6);
      break;
    }
  }
  std::fclose(f);
  return kb;
}

}  // namespace

// ===========================================================================
// 一、★ 持续写入 20 秒：计数精确、内存不涨
// ===========================================================================
//
// 【判据的选择】
// 不拿"写入速率"当门槛（机器差异大，无意义）。
// 只断言三件确定的事：
//   1. allocated == 各线程实际写入数之和（**精确相等**，不是"差不多"）
//   2. 每个阶段采样到的 written 单调不减
//   3. RSS 增长在合理范围内（比如 < 8 MB）—— ring 是预分配的

STRATUM_TEST(ProbeSoak, TwentySecondsSteadyWrite) {
  const std::string shm = FreshShm("steady");
  stratum_probe_close();
  std::error_code ec;
  fs::remove(shm, ec);

  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  const int kThreads = 2;
  std::atomic<bool> stop{false};
  std::vector<std::thread> ts;
  // 每个线程自己数写了多少 —— 用来与 allocated 精确对账
  std::vector<uint64_t> per_thread(kThreads, 0);

  const auto t0 = std::chrono::steady_clock::now();
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t]() {
      uint64_t n = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "soak-%d-%llu", t,
                      static_cast<unsigned long long>(n));
        stratum_probe_write("soak/x", buf, std::strlen(buf));
        ++n;
        if ((n & 0xFF) == 0) std::this_thread::yield();
      }
      per_thread[static_cast<size_t>(t)] = n;
    });
  }

  // 采样：每秒一次，检查 written 单调不减
  uint64_t prev_written = 0;
  const long rss_before = ReadVmRssKb();
  bool monotonic = true;
  std::vector<std::pair<int, uint64_t>> samples;
  for (int sec = 0; sec < 20; ++sec) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    stratum_probe_stats st;
    stratum_probe_get_stats(&st);
    if (st.written < prev_written) monotonic = false;
    prev_written = st.written;
    samples.emplace_back(sec + 1, st.written);
  }
  const long rss_after = ReadVmRssKb();

  stop.store(true, std::memory_order_relaxed);
  for (auto& x : ts) x.join();

  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count();

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);

  // 1) 计数精确对账
  uint64_t expect = 0;
  for (auto n : per_thread) expect += n;
  STRATUM_CHECK_EQ(st.written, expect);
  STRATUM_CHECK_EQ(stratum_probe_allocated(), expect);
  STRATUM_CHECK_EQ(stratum_probe_committed(), expect);

  // 2) 单调性
  STRATUM_CHECK_TRUE(monotonic);

  // 3) 内存：ring 预分配，RSS 不该显著增长
  const long rss_delta = (rss_after > 0 && rss_before > 0) ? (rss_after - rss_before) : 0;
  STRATUM_CHECK_TRUE(rss_delta < 8 * 1024);   // < 8 MB

  const double per_sec = static_cast<double>(expect) * 1000.0 /
                         static_cast<double>(elapsed > 0 ? elapsed : 1);
  std::printf("      [soak] %d 线程 × 20 秒：写入 %llu 条（%.0f 条/秒），"
              "计数精确，RSS 变化 %ld KB\n",
              kThreads, static_cast<unsigned long long>(expect), per_sec, rss_delta);
  std::printf("      [soak] 采样轨迹（每秒累计）：");
  for (size_t i = 0; i < samples.size(); i += 5) {
    std::printf("%llu ", static_cast<unsigned long long>(samples[i].second));
  }
  std::printf("...  \n");

  stratum_probe_close();
  fs::remove(shm, ec);
}

// ===========================================================================
// 二、跨多圈覆盖后计数仍精确
// ===========================================================================
//
// 【为什么单独一条】容量 1024 的环，写 10000 条要绕近 10 圈。
// 每绕一圈都会覆盖旧记录。若 dropped_full 的计算或 committed 的推进
// 有任何 off-by-one，绕多圈后会放大成明显偏差。

STRATUM_TEST(ProbeSoak, ManyWrapsKeepCountsExact) {
  const std::string shm = FreshShm("wraps");
  stratum_probe_close();
  std::error_code ec;
  fs::remove(shm, ec);
  ::setenv("STRATUM_PROBE_CAPACITY", "1024", 1);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);
  ::unsetenv("STRATUM_PROBE_CAPACITY");

  const uint64_t kTotal = 10000;   // 绕近 10 圈
  for (uint64_t i = 1; i <= kTotal; ++i) {
    stratum_probe_write("wrap/x", "v", 1);
    // 中途抽查：每 1000 条核对一次
    if ((i % 1000) == 0) {
      STRATUM_CHECK_EQ(stratum_probe_allocated(), i);
      STRATUM_CHECK_EQ(stratum_probe_committed(), i);
    }
  }

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, kTotal);
  // 覆盖丢弃 = 总量 - 容量（精确，不是估算）
  STRATUM_CHECK_EQ(st.dropped_full, kTotal - 1024);
  std::printf("      [多圈] 容量 1024 写 10000 条（约 10 圈）："
              "allocated/committed 每 1000 条核对一次全部精确，覆盖丢弃 %llu\n",
              static_cast<unsigned long long>(st.dropped_full));

  stratum_probe_close();
  fs::remove(shm, ec);
}

STRATUM_TEST_MAIN()
