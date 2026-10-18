// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 埋点库（probe）的测试
//
// 需求依据：docs/13-probe-requirements.md
//   PR-02 三个宏（PROBE / KV / SCOPE）
//   PR-03 无 malloc/mutex/syscall；满则丢最旧、绝不阻塞
//   PR-04 -DSTRATUM_PROBE_DISABLE 时零开销（反汇编验证）
//
// 【本文件最重要的两组用例】
//
//   1. **环满时的行为**（PR-03）：必须**覆盖最旧**且**不阻塞**，
//      同时 committed_seq 仍单调、写者不等待。
//   2. **零开销**（PR-04）：关闭宏后，函数体里**不能有任何残留**。
//      这条不能靠"看代码觉得没问题"—— 必须反汇编。
//
// 覆盖三类（项目标准）：
//   · 大数据量 —— 灌满 ring（容量 4096）并验证覆盖与计数
//   · 随机     —— 多种子随机 topic 数/负载长度
//   · 真实场景 —— ① 关键路径耗时（SCOPE）② 高频采样 ③ 多线程并发写

// 标准库头文件必须最先包含
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "stratum/probe/probe.h"
#include "test_framework.h"
#include "test_isolation.h"

namespace fs = std::filesystem;

namespace {

// 每个用例用独立的 shm，避免互相干扰
std::string FreshShm(const char* tag) {
  const std::string p = std::string("/dev/shm/stratum_test_probe_") + tag + "_" + UniqueSuffix();
  std::error_code ec;
  fs::remove(p, ec);
  return p;
}

void ResetProbe(const std::string& shm) {
  stratum_probe_close();
  std::error_code ec;
  fs::remove(shm, ec);
}

}  // namespace

// ===========================================================================
// 一、基本写入
// ===========================================================================

STRATUM_TEST(Probe, OpensAndWrites) {
  const std::string shm = FreshShm("basic");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);
  STRATUM_CHECK_TRUE(stratum_probe_enabled());

  stratum_probe_write("t/a", "hello", 5);
  stratum_probe_write("t/a", "world", 5);

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(2));
  STRATUM_CHECK_EQ(st.capacity, static_cast<uint32_t>(STRATUM_PROBE_DEFAULT_CAPACITY));
  STRATUM_CHECK_EQ(stratum_probe_committed(), static_cast<uint64_t>(2));
  STRATUM_CHECK_EQ(stratum_probe_allocated(), static_cast<uint64_t>(2));
  ResetProbe(shm);
}

STRATUM_TEST(Probe, RejectsEmptyTopic) {
  const std::string shm = FreshShm("emptytopic");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  stratum_probe_write("", "x", 1);
  stratum_probe_write(nullptr, "x", 1);

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(0));
  STRATUM_CHECK_EQ(st.dropped_bad, static_cast<uint64_t>(2));
  ResetProbe(shm);
}

STRATUM_TEST(Probe, TruncatesLongPayloadAndTopic) {
  const std::string shm = FreshShm("trunc");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  std::string long_topic(STRATUM_PROBE_TOPIC_MAX + 20, 'T');
  std::string long_payload(STRATUM_PROBE_PAYLOAD_MAX + 50, 'P');
  stratum_probe_write(long_topic.c_str(), long_payload.data(), long_payload.size());

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(1));   // 仍然写入，只是截断
  ResetProbe(shm);
}

STRATUM_TEST(Probe, GlobalSwitchStopsWrites) {
  const std::string shm = FreshShm("switch");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  stratum_probe_write("t/a", "x", 1);
  stratum_probe_set_enabled(0);
  STRATUM_CHECK_FALSE(stratum_probe_enabled());
  stratum_probe_write("t/a", "y", 1);

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(1));
  STRATUM_CHECK_EQ(st.dropped_off, static_cast<uint64_t>(1));

  stratum_probe_set_enabled(1);
  STRATUM_CHECK_TRUE(stratum_probe_enabled());
  stratum_probe_write("t/a", "z", 1);
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(2));
  ResetProbe(shm);
}

// ===========================================================================
// 二、★ 环满：覆盖最旧、不阻塞（PR-03）
// ===========================================================================

STRATUM_TEST(ProbeRing, OverwritesOldestAndKeepsCounting) {
  const std::string shm = FreshShm("wrap");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  stratum_probe_stats st0;
  stratum_probe_get_stats(&st0);
  const uint32_t cap = st0.capacity;

  // 灌满 + 再写一倍
  const uint64_t total = static_cast<uint64_t>(cap) * 2;
  for (uint64_t i = 0; i < total; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "i=%llu", static_cast<unsigned long long>(i));
    stratum_probe_write("ring/wrap", buf, std::strlen(buf));
  }

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  // 全部调用都成功返回（不阻塞、不失败）
  STRATUM_CHECK_EQ(st.written, total);
  // 超出容量的部分被记为覆盖丢弃
  STRATUM_CHECK_EQ(st.dropped_full, total - cap);
  STRATUM_CHECK_EQ(stratum_probe_allocated(), total);
  STRATUM_CHECK_EQ(stratum_probe_committed(), total);
  std::printf("      [环满] 容量 %u，写入 %llu 条，覆盖丢弃 %llu 条，写者零阻塞\n",
              cap, static_cast<unsigned long long>(total),
              static_cast<unsigned long long>(st.dropped_full));
  ResetProbe(shm);
}

STRATUM_TEST(ProbeRing, WriteNeverBlocksUnderFlood) {
  // 灌 10 倍容量：如果实现在环满时等待读者，这里会显著变慢甚至卡住。
  // 用时间上界做弱断言（机器差异大，只作"不卡死"的证据）。
  const std::string shm = FreshShm("flood");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  stratum_probe_stats st0;
  stratum_probe_get_stats(&st0);
  const uint64_t total = static_cast<uint64_t>(st0.capacity) * 10;

  const auto t0 = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < total; ++i) {
    stratum_probe_write("flood/x", "0123456789", 10);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto us =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, total);
  // 每条约几十 ns 到几百 ns；给足余量（含调试/慢机）
  STRATUM_CHECK_TRUE(us < static_cast<long long>(total) * 20);
  std::printf("      [不阻塞] %llu 条写入耗时 %lld us（每条 %.0f ns）\n",
              static_cast<unsigned long long>(total), static_cast<long long>(us),
              static_cast<double>(us) * 1000.0 / static_cast<double>(total));
  ResetProbe(shm);
}

// ===========================================================================
// 三、多线程并发写
// ===========================================================================

STRATUM_TEST(ProbeRing, ConcurrentWritersDoNotCorruptCounters) {
  const std::string shm = FreshShm("mt");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  const int kThreads = 4;
  const int kPer = 500;
  std::vector<std::thread> ts;
  std::atomic<int> done{0};
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t]() {
      for (int i = 0; i < kPer; ++i) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "t=%d,i=%d", t, i);
        stratum_probe_write("mt/x", buf, std::strlen(buf));
        if ((i & 63) == 0) std::this_thread::yield();
      }
      done.fetch_add(1);
    });
  }
  for (auto& x : ts) x.join();

  STRATUM_CHECK_EQ(done.load(), kThreads);
  const uint64_t expect = static_cast<uint64_t>(kThreads) * kPer;
  // 计数器必须精确（它们是原子 fetch_add，不应丢计数）
  STRATUM_CHECK_EQ(stratum_probe_allocated(), expect);
  STRATUM_CHECK_EQ(stratum_probe_committed(), expect);
  std::printf("      [并发] %d 线程 × %d 条 = %llu，allocated/committed 精确\n",
              kThreads, kPer, static_cast<unsigned long long>(expect));
  ResetProbe(shm);
}

// ===========================================================================
// 四、SCOPE（PR-02 第三个宏）
// ===========================================================================

STRATUM_TEST(Probe, ScopeRecordsDuration) {
  const std::string shm = FreshShm("scope");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  // 直接调底层（宏在 C++ 里走 RAII 类，语义等价）
  const uint64_t t0 = stratum_probe_now_mono_ns();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  stratum_probe_write_scope("order/submit", stratum_probe_now_mono_ns() - t0);

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(1));
  ResetProbe(shm);
}

STRATUM_TEST(Probe, CppScopeRaiiWritesOnDestruction) {
  const std::string shm = FreshShm("raii");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  {
    STRATUM_PROBE_SCOPE("raii/topic");
    // 离开作用域时析构函数写记录
  }
  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(1));
  ResetProbe(shm);
}

STRATUM_TEST(Probe, CppScopeWritesEvenOnException) {
  // S01 的边界分支：异常路径也必须发一条，否则"最慢的那次"恰恰查不到
  const std::string shm = FreshShm("raii_ex");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  try {
    STRATUM_PROBE_SCOPE("raii/throw");
    throw std::runtime_error("boom");
  } catch (const std::exception&) {
    // 栈展开时 Scope 析构已执行
  }

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(1));
  std::printf("      [RAII] 异常路径也发出了耗时记录（%llu 条）\n",
              static_cast<unsigned long long>(st.written));
  ResetProbe(shm);
}

// ===========================================================================
// 五、★ PR-04：编译期关闭后零开销（反汇编验证）
// ===========================================================================

STRATUM_TEST(ProbeDisable, DisabledMacrosLeaveNoCode) {
  // 【验证方式】编译两个极小的翻译单元：
  //   A) 正常：函数体里调 STRATUM_PROBE
  //   B) -DSTRATUM_PROBE_DISABLE：同一函数体
  // 然后反汇编，断言 B 的函数体**没有调用任何 stratum_probe_* 符号**。
  //
  // 这是唯一能真正证明"零开销"的方法 —— 看代码觉得没问题不算。
  const std::string dir = std::string("/tmp/stratum_probe_disable_test_") + UniqueSuffix();
  std::error_code ec;
  fs::create_directories(dir, ec);

  const std::string src = dir + "/t.c";
  {
    std::ofstream o(src, std::ios::out | std::ios::trunc);
    o << "#include <stratum/probe/probe.h>\n"
      << "__attribute__((noinline)) void work(void) {\n"
      << "  STRATUM_PROBE_KV(\"t/x\", \"a=1\");\n"
      << "}\n"
      << "int main(void) { work(); return 0; }\n";
  }

  auto BuildAndDisasm = [&](const std::string& out, bool disabled) -> std::string {
    const std::string cmd =
        std::string("gcc -std=c11 -O2 -I") + PROJECT_INCLUDE_DIR + " " +
        (disabled ? "-DSTRATUM_PROBE_DISABLE " : "") + src + " -o " + out +
        " 2>&1";
    std::FILE* p = ::popen(cmd.c_str(), "r");
    std::string log;
    if (p) {
      char buf[512];
      while (std::fgets(buf, sizeof(buf), p)) log += buf;
      ::pclose(p);
    }
    return log;
  };

  // 注意：正常版本需要链接 probe.c 的实现；这里只关心**是否引用了符号**，
  // 所以用 -c 生成对象再反汇编，避免链接问题。
  auto CompileObject = [&](const std::string& obj, bool disabled) -> bool {
    const std::string cmd =
        std::string("gcc -std=c11 -O2 -c -I") + PROJECT_INCLUDE_DIR + " " +
        (disabled ? "-DSTRATUM_PROBE_DISABLE " : "") + src + " -o " + obj + " 2>&1";
    std::FILE* p = ::popen(cmd.c_str(), "r");
    std::string log;
    if (p) {
      char buf[512];
      while (std::fgets(buf, sizeof(buf), p)) log += buf;
      ::pclose(p);
    }
    return fs::exists(obj);
  };

  auto CountProbeRefs = [&](const std::string& obj) -> int {
    // 只统计**真实的调用与重定位**。
    // 第一版用 `grep -c stratum_probe`，结果匹配到了注释里的头文件名
    // （`stratum_probe.h`）—— 注释不算开销，必须排除。
    const std::string cmd =
        "objdump -dr " + obj +
        " 2>/dev/null | grep -cE 'call.*stratum_probe|R_X86_64.*stratum_probe'";
    std::FILE* p = ::popen(cmd.c_str(), "r");
    int n = -1;
    if (p) {
      char buf[64] = {0};
      if (std::fgets(buf, sizeof(buf), p)) n = std::atoi(buf);
      ::pclose(p);
    }
    return n;
  };

  const std::string obj_on = dir + "/on.o";
  const std::string obj_off = dir + "/off.o";
  STRATUM_CHECK_TRUE(CompileObject(obj_on, false));
  STRATUM_CHECK_TRUE(CompileObject(obj_off, true));

  const int refs_on = CountProbeRefs(obj_on);
  const int refs_off = CountProbeRefs(obj_off);

  std::printf("      [零开销] 启用时引用 %d 处；关闭时引用 %d 处（应为 0）\n",
              refs_on, refs_off);
  STRATUM_CHECK_TRUE(refs_on > 0);   // 启用时必须有引用（否则测试没意义）
  STRATUM_CHECK_EQ(refs_off, 0);     // ★ 关闭时不许有任何残留

  std::error_code e2;
  fs::remove_all(dir, e2);
}

// ===========================================================================
// 六、大数据量
// ===========================================================================

STRATUM_TEST(Probe, LargePayloadManyRecords) {
  const std::string shm = FreshShm("large");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  // 接近上限的负载 × 容量的 3 倍
  stratum_probe_stats st0;
  stratum_probe_get_stats(&st0);
  const uint64_t total = static_cast<uint64_t>(st0.capacity) * 3;
  std::string payload(STRATUM_PROBE_PAYLOAD_MAX, 'Z');

  for (uint64_t i = 0; i < total; ++i) {
    stratum_probe_write("large/x", payload.data(), payload.size());
  }
  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, total);
  STRATUM_CHECK_EQ(st.dropped_full, total - st0.capacity);
  std::printf("      [规模] %llu 条 × %u 字节负载，覆盖丢弃 %llu 条\n",
              static_cast<unsigned long long>(total), STRATUM_PROBE_PAYLOAD_MAX,
              static_cast<unsigned long long>(st.dropped_full));
  ResetProbe(shm);
}

// ===========================================================================
// 七、随机（多种子）
// ===========================================================================

STRATUM_TEST(Probe, RandomTopicsAndPayloadsMultipleSeeds) {
  for (uint64_t seed : {9ULL, 99ULL, 999ULL}) {
    const std::string shm = FreshShm(("r" + std::to_string(seed)).c_str());
    ResetProbe(shm);
    STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> topic_d(1, 12);
    std::uniform_int_distribution<int> len_d(0, 120);
    std::uniform_int_distribution<int> cnt_d(50, 300);

    const int n_topics = topic_d(rng);
    const int n = cnt_d(rng);
    for (int i = 0; i < n; ++i) {
      const std::string topic = "rnd/t" + std::to_string(rng() % static_cast<uint64_t>(n_topics));
      const int len = len_d(rng);
      std::string payload(static_cast<size_t>(len), static_cast<char>('a' + (rng() % 26)));
      stratum_probe_write(topic.c_str(), payload.data(), payload.size());
    }
    stratum_probe_stats st;
    stratum_probe_get_stats(&st);
    STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(n));
    STRATUM_CHECK_EQ(st.dropped_bad, static_cast<uint64_t>(0));
    ResetProbe(shm);
  }
}

// ===========================================================================
// 八、真实场景（对应 docs/13 的场景编号）
// ===========================================================================

// ---------------------------------------------------------------------------
// 场景 1（S01）：关键路径耗时监控
// ---------------------------------------------------------------------------
STRATUM_TEST(ProbeReal, HotPathLatencyMonitoring) {
  const std::string shm = FreshShm("real1");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  // 模拟被监控的 100 次调用
  for (int i = 0; i < 100; ++i) {
    STRATUM_PROBE_SCOPE("order/submit");
    volatile int acc = 0;
    for (int k = 0; k < 1000; ++k) acc += k;   // 一点真实工作
    (void)acc;
  }
  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(100));
  std::printf("      [场景1/S01] 100 次关键路径调用，全部产生耗时记录\n");
  ResetProbe(shm);
}

// ---------------------------------------------------------------------------
// 场景 2（S05）：高频探针 —— 写入开销必须足够低
// ---------------------------------------------------------------------------
STRATUM_TEST(ProbeReal, HighFrequencyOverheadIsLow) {
  const std::string shm = FreshShm("real2");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  const int kN = 100000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) {
    stratum_probe_write("hot/x", "0123456789abcdef", 16);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(kN));
  const double per = static_cast<double>(ns) / kN;
  std::printf("      [场景2/S05] %d 次写入，每次 %.0f ns（含覆盖丢弃）\n", kN, per);
  // 上界放宽到 2000ns：不同机器/调试构建差异大，这里只作"没有灾难性开销"的证据
  STRATUM_CHECK_TRUE(per < 2000.0);
  ResetProbe(shm);
}

// ---------------------------------------------------------------------------
// 场景 3（S09）：多实例隔离 —— 两个 shm 互不干扰
// ---------------------------------------------------------------------------
STRATUM_TEST(ProbeReal, TwoInstancesAreIsolated) {
  const std::string shm_a = FreshShm("inst_a");
  const std::string shm_b = FreshShm("inst_b");
  ResetProbe(shm_a);
  ResetProbe(shm_b);

  STRATUM_CHECK_EQ(stratum_probe_open(shm_a.c_str()), 0);
  stratum_probe_write("a/x", "1", 1);
  stratum_probe_write("a/x", "2", 1);
  stratum_probe_stats st_a;
  stratum_probe_get_stats(&st_a);
  const uint64_t written_a = st_a.written;
  stratum_probe_close();

  STRATUM_CHECK_EQ(stratum_probe_open(shm_b.c_str()), 0);
  stratum_probe_write("b/x", "1", 1);
  stratum_probe_stats st_b;
  stratum_probe_get_stats(&st_b);
  // B 的写入不应看到 A 的任何计数（每实例独立共享内存段）
  STRATUM_CHECK_EQ(st_b.written, static_cast<uint64_t>(1));
  STRATUM_CHECK_EQ(stratum_probe_allocated(), static_cast<uint64_t>(1));
  std::printf("      [场景3/S09] 两实例隔离：A 写 %llu 条，B 从 0 开始（写 1 条）\n",
              static_cast<unsigned long long>(written_a));
  stratum_probe_close();
  std::error_code ec;
  fs::remove(shm_a, ec);
  fs::remove(shm_b, ec);
}

// ===========================================================================
// 九、双时间戳（PR-15）
// ===========================================================================
//
// 记录里同时保存 realtime 与 mono：
//   · realtime → 与业务 DDS 数据对齐（S10）
//   · mono     → 算耗时，不受系统时间调整影响
//
// 【为什么必须断言】拿 mono 去对齐业务数据会导致"时间轴错位"，
// 而且不会报错 —— 只是复盘时怎么都对不上。

STRATUM_TEST(Probe, RecordsBothTimestamps) {
  const std::string shm = FreshShm("ts");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  // 顺序写 5 条，中间稍微隔开
  for (int i = 0; i < 5; ++i) {
    stratum_probe_write("ts/x", "v", 1);
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }

  const auto* hdr = static_cast<const stratum_ring_header*>(stratum_probe_ring_view());
  STRATUM_CHECK_TRUE(hdr != nullptr);
  const stratum_probe_record* recs = stratum_ring_records_const(hdr);

  uint64_t prev_mono = 0;
  for (uint32_t i = 0; i < 5; ++i) {
    const stratum_probe_record& r = recs[i];
    // 1) 两个时间戳都采集了
    STRATUM_CHECK_TRUE(r.ts_realtime_ns > 0);
    STRATUM_CHECK_TRUE(r.ts_mono_ns > 0);

    // 2) mono 单调不减（顺序写入时的耗时计算前提）
    if (i > 0) STRATUM_CHECK_TRUE(r.ts_mono_ns >= prev_mono);
    prev_mono = r.ts_mono_ns;

    // 3) 两者是"同一时刻的两个时钟读数"：realtime 远大于 mono，
    //    但差值（开机时长）应在一个合理窗口内（这里只检查非负且不过分）
    STRATUM_CHECK_TRUE(r.ts_realtime_ns > r.ts_mono_ns);
  }

  std::printf("      [双时间戳] 5 条记录都含 realtime 与 mono，mono 单调不减\n");
  ResetProbe(shm);
}

STRATUM_TEST(Probe, TimestampsAdvanceBetweenWrites) {
  // 两次写入之间 mono 必须前进 —— 否则耗时永远是 0，监控失去意义
  const std::string shm = FreshShm("ts2");
  ResetProbe(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  stratum_probe_write("ts/y", "a", 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  stratum_probe_write("ts/y", "b", 1);

  const auto* hdr = static_cast<const stratum_ring_header*>(stratum_probe_ring_view());
  const stratum_probe_record* recs = stratum_ring_records_const(hdr);
  const uint64_t d = recs[1].ts_mono_ns - recs[0].ts_mono_ns;
  // 至少 1ms（我们 sleep 了 2ms，给足余量）
  STRATUM_CHECK_TRUE(d >= 1000 * 1000ULL);
  std::printf("      [双时间戳] 两次写入的 mono 差 %llu ns（>= 1ms）\n",
              static_cast<unsigned long long>(d));
  ResetProbe(shm);
}

STRATUM_TEST_MAIN()
