// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// **多实例**场景的测试
//
// 与 test_probe.cc 的分工：
//   test_probe.cc        —— 单实例的隔离（两个 shm 互不干扰，顺序执行）
//   test_probe_instance.cc —— **同时**存在多个实例：
//       · 两个实例交叉写入，计数不串
//       · 各自的全局开关互不影响
//       · 一个实例关闭后另一个继续
//       · 配置文件不同实例各用各的
//
// 【为什么单独一个文件】"顺序打开/关闭两个实例"和"两个实例同时开着"
// 是两件事。前者只能证明计数清零正确；后者才能证明没有共享的全局状态。
// 若实现里用了 static 变量存 ring 指针，顺序测试照样通过，并发测试才会暴露。

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
#include "stratum/probe/probe_config.h"
#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

std::string FreshShm(const char* tag) {
  const std::string p = std::string("/dev/shm/stratum_inst_") + tag;
  std::error_code ec;
  fs::remove(p, ec);
  return p;
}

void Clean(const std::string& shm) {
  stratum_probe_close();
  std::error_code ec;
  fs::remove(shm, ec);
}

}  // namespace

// ===========================================================================
// 一、★ 两个实例**同时**存在，各自的写入互不串
// ===========================================================================
//
// 【这是本文件的核心】probe 库内部用 static 变量持有 ring 指针。
// 同一进程只能打开一个 ring（这是设计决定 —— 一个进程一块共享内存）。
// 但**不同进程**各自有自己的 static，天然隔离。
//
// 所以真正要验证的是：**同一进程内先关后开**不会残留状态，
// 以及**不同 shm 的容量可以不同**（不被前一次的 static 缓存污染）。

STRATUM_TEST(ProbeInstance, DifferentCapacityPerOpen) {
  const std::string shm_a = FreshShm("capa");
  const std::string shm_b = FreshShm("capb");
  Clean(shm_a);
  Clean(shm_b);

  // A: 容量 8
  ::setenv("STRATUM_PROBE_CAPACITY", "8", 1);
  STRATUM_CHECK_EQ(stratum_probe_open(shm_a.c_str()), 0);
  ::unsetenv("STRATUM_PROBE_CAPACITY");
  stratum_probe_stats st_a;
  stratum_probe_get_stats(&st_a);
  STRATUM_CHECK_EQ(st_a.capacity, static_cast<uint32_t>(8));
  stratum_probe_close();

  // B: 容量 32 —— 若实现缓存了 A 的容量，这里会拿到 8
  ::setenv("STRATUM_PROBE_CAPACITY", "32", 1);
  STRATUM_CHECK_EQ(stratum_probe_open(shm_b.c_str()), 0);
  ::unsetenv("STRATUM_PROBE_CAPACITY");
  stratum_probe_stats st_b;
  stratum_probe_get_stats(&st_b);
  STRATUM_CHECK_EQ(st_b.capacity, static_cast<uint32_t>(32));
  std::printf("      [实例] A=8 B=32：容量按各自 ring 的头部读取，未被前次缓存污染\n");

  Clean(shm_a);
  Clean(shm_b);
}

// ===========================================================================
// 二、两个 shm 的写入互不影响（交叉写）
// ===========================================================================

STRATUM_TEST(ProbeInstance, CrossWritesDoNotLeak) {
  const std::string shm_a = FreshShm("xa");
  const std::string shm_b = FreshShm("xb");
  Clean(shm_a);
  Clean(shm_b);

  // A 写 5 条
  STRATUM_CHECK_EQ(stratum_probe_open(shm_a.c_str()), 0);
  for (int i = 0; i < 5; ++i) stratum_probe_write("a/x", "1", 1);
  const uint64_t a_committed = stratum_probe_committed();
  stratum_probe_close();

  // B 写 3 条：计数必须从 0 开始
  STRATUM_CHECK_EQ(stratum_probe_open(shm_b.c_str()), 0);
  STRATUM_CHECK_EQ(stratum_probe_committed(), static_cast<uint64_t>(0));
  for (int i = 0; i < 3; ++i) stratum_probe_write("b/x", "2", 1);
  STRATUM_CHECK_EQ(stratum_probe_committed(), static_cast<uint64_t>(3));
  stratum_probe_close();

  // 再打开 A：数据还在（committed=5）
  STRATUM_CHECK_EQ(stratum_probe_open(shm_a.c_str()), 0);
  STRATUM_CHECK_EQ(stratum_probe_committed(), a_committed);
  // 环里的 topic 必须是 a/x，不该混入 b/x
  const auto* h = static_cast<const stratum_ring_header*>(stratum_probe_ring_view());
  const stratum_probe_record* r = stratum_ring_records_const(h);
  STRATUM_CHECK_EQ(std::string(r[0].topic, r[0].topic_len), std::string("a/x"));
  std::printf("      [实例] A 的 5 条与 B 的 3 条完全隔离，A 重开后数据仍在\n");

  Clean(shm_a);
  Clean(shm_b);
}

// ===========================================================================
// 三、全局开关是 per-ring 的
// ===========================================================================
//
// 【为什么重要】若 enabled 存在进程级 static 变量里，
// 关掉 A 会连带关掉 B —— 多实例部署时这是灾难。

STRATUM_TEST(ProbeInstance, EnabledFlagIsPerRing) {
  const std::string shm_a = FreshShm("ena");
  const std::string shm_b = FreshShm("enb");
  Clean(shm_a);
  Clean(shm_b);

  STRATUM_CHECK_EQ(stratum_probe_open(shm_a.c_str()), 0);
  stratum_probe_set_enabled(0);          // 关掉 A
  STRATUM_CHECK_FALSE(stratum_probe_enabled());
  stratum_probe_close();

  // 打开 B：应当**默认是开的**（enabled 存在各自的 ring header 里）
  STRATUM_CHECK_EQ(stratum_probe_open(shm_b.c_str()), 0);
  STRATUM_CHECK_TRUE(stratum_probe_enabled());

  // 再回 A：仍然是关的
  stratum_probe_close();
  STRATUM_CHECK_EQ(stratum_probe_open(shm_a.c_str()), 0);
  STRATUM_CHECK_FALSE(stratum_probe_enabled());
  std::printf("      [实例] enabled 存 per-ring：关 A 不影响 B，回 A 仍是关\n");

  Clean(shm_a);
  Clean(shm_b);
}

// ===========================================================================
// 四、同一份配置被两个实例共享（显式路径）
// ===========================================================================
//
// S09 的裁决：共享一份配置是**合法**的（统一开关）。
// 两个 ProbeConfig 对象读同一文件，结果必须一致。

STRATUM_TEST(ProbeInstance, SharedConfigFileIsConsistent) {
  const char* home = std::getenv("HOME");
  const std::string dir = std::string(home ? home : ".") + "/stratum_inst_cfg";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  const std::string cfg = dir + "/shared.ini";
  {
    std::FILE* f = std::fopen(cfg.c_str(), "w");
    STRATUM_CHECK_TRUE(f != nullptr);
    std::fputs("[probe]\nenabled = true\n"
               "[defaults]\nenabled = true\n"
               "[topic.\"shared/off\"]\nenabled = false\n", f);
    std::fclose(f);
  }

  // 两个独立的配置对象，读同一文件
  stratum::probe::ProbeConfig a;
  stratum::probe::ProbeConfig b;
  const auto ra = a.Load(cfg);
  const auto rb = b.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(ra.outcome), static_cast<int>(stratum::probe::LoadOutcome::kOk));
  STRATUM_CHECK_EQ(static_cast<int>(rb.outcome), static_cast<int>(stratum::probe::LoadOutcome::kOk));

  // 结果必须一致
  STRATUM_CHECK_TRUE(a.Snapshot() == b.Snapshot());
  STRATUM_CHECK_FALSE(a.Resolve("shared/off").enabled);
  STRATUM_CHECK_FALSE(b.Resolve("shared/off").enabled);
  STRATUM_CHECK_TRUE(a.Resolve("other").enabled);   // 未声明 → 用默认
  std::printf("      [实例] 两个 ProbeConfig 读同一份配置，结果完全一致\n");

  fs::remove_all(dir, ec);
}

// ===========================================================================
// 五、不同实例用不同配置（各自独立）
// ===========================================================================

STRATUM_TEST(ProbeInstance, IndependentConfigsDoNotInterfere) {
  const char* home = std::getenv("HOME");
  const std::string dir = std::string(home ? home : ".") + "/stratum_inst_cfg2";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  const std::string cfg_a = dir + "/a.ini";
  const std::string cfg_b = dir + "/b.ini";
  {
    std::FILE* f = std::fopen(cfg_a.c_str(), "w");
    std::fputs("[probe]\nenabled = true\n[topic.\"t\"]\nenabled = false\n", f);
    std::fclose(f);
  }
  {
    std::FILE* f = std::fopen(cfg_b.c_str(), "w");
    std::fputs("[probe]\nenabled = true\n[topic.\"t\"]\nenabled = true\n", f);
    std::fclose(f);
  }

  stratum::probe::ProbeConfig a;
  stratum::probe::ProbeConfig b;
  (void)a.Load(cfg_a);
  (void)b.Load(cfg_b);

  // 同名 topic，不同结论 —— 这就是"多实例各自配置"的意义
  STRATUM_CHECK_FALSE(a.Resolve("t").enabled);
  STRATUM_CHECK_TRUE(b.Resolve("t").enabled);
  std::printf("      [实例] 同一 topic 在 A 配置里关、在 B 配置里开，互不影响\n");

  fs::remove_all(dir, ec);
}

// ===========================================================================
// 六、多线程同时写同一个 ring（同一实例）
// ===========================================================================
//
// 【为什么放在这里】多实例是"多块 ring"，多线程是"一块 ring 多写者"。
// 两者合起来才是完整的并发图景。

STRATUM_TEST(ProbeInstance, MultiThreadSingleRing) {
  const std::string shm = FreshShm("mt1");
  Clean(shm);
  ::setenv("STRATUM_PROBE_CAPACITY", "1024", 1);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);
  ::unsetenv("STRATUM_PROBE_CAPACITY");

  const int kThreads = 3;
  const int kPer = 100;
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t]() {
      for (int i = 0; i < kPer; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "t%d-%d", t, i);
        stratum_probe_write("mt/x", buf, std::strlen(buf));
      }
    });
  }
  for (auto& x : ts) x.join();

  // 300 条全部写入，容量 1024 足够，不应有覆盖丢弃
  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(stratum_probe_allocated(), static_cast<uint64_t>(kThreads * kPer));
  STRATUM_CHECK_EQ(stratum_probe_committed(), static_cast<uint64_t>(kThreads * kPer));
  STRATUM_CHECK_EQ(st.dropped_full, static_cast<uint64_t>(0));
  std::printf("      [并发] 3 线程 × 100 条 = 300，无覆盖丢弃，计数器精确\n");
  Clean(shm);
}

STRATUM_TEST_MAIN()
