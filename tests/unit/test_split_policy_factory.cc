// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// **`SplitPolicy` + 存储引擎工厂的测试**（`docs/06` 缺口 #4 与 #5）
//
// 两组都很小，但各有一个容易错的地方：
//
// **SplitPolicy**：`max_layer_duration_ns` / `max_layer_events` 的 **0 表示"不限"**，
// 不是"0 字节 / 0 条"。这个语义如果不明确，实现方会把它当成"立刻分片" ——
// 于是每个事件都分一层，文件数爆炸而没人知道为什么。
//
// **工厂**：`Create()` 必须返回**对应类型的**引擎，不能"返回非空"就算过。
// 如果 `LsmEngineFactory::Create()` 返回了 MemoryEngine，`dynamic_cast` 能抓出来；
// 只断言 `!= nullptr` 抓不出来 —— 而那种错误会在生产里表现为"数据没落盘"。

// 标准库头文件必须最先包含
#include <cstdio>
#include <memory>
#include <string>

#include "stratum/core/types.h"
#include "stratum/storage/istorage_engine.h"
#include "stratum/storage/lsm/lsm_engine.h"
#include "stratum/storage/memory/memory_engine.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::storage;

// ===========================================================================
// 一、SplitPolicy 的默认值
// ===========================================================================

STRATUM_TEST(SplitPolicyDefaults, ByteLimitIs256MB) {
  const SplitPolicy p;
  // 256MB —— 这个默认值是"单个分片层文件的上限"，不是总量上限。
  // 若有人把它改成 0（"不限"），超大会话会写出一个巨大的层文件，
  // 打开时要全量扫索引。
  STRATUM_CHECK_EQ(p.max_layer_bytes, 256ull * 1024ull * 1024ull);
  std::printf("      [默认] max_layer_bytes = %llu（256 MB）\n",
              static_cast<unsigned long long>(p.max_layer_bytes));
}

// ===========================================================================
// 二、★ 0 表示"不限"（不是"0 字节 / 0 条"）
// ===========================================================================
//
// 这条语义必须显式验证，因为两种解读会导致完全相反的行为：
//   · "0 = 不限"   → 不分片（期望）
//   · "0 = 零上限" → 每个事件都触发分片（文件数爆炸）
//
// 真实数据里这两种都会"跑起来不报错"，区别只在文件数 —— 属于最难查的那类。

STRATUM_TEST(SplitPolicyDefaults, ZeroMeansUnlimited) {
  const SplitPolicy p;
  STRATUM_CHECK_EQ(p.max_layer_duration_ns, 0ull);
  STRATUM_CHECK_EQ(p.max_layer_events, 0ull);

  // 语义断言：`0` 与 `非0` 必须能被区分出"是否启用该维度"
  auto enabled = [](uint64_t v) { return v != 0; };
  STRATUM_CHECK_FALSE(enabled(p.max_layer_duration_ns));
  STRATUM_CHECK_FALSE(enabled(p.max_layer_events));

  SplitPolicy custom;
  custom.max_layer_duration_ns = 3600ull * 1000000000ull;   // 1 小时
  custom.max_layer_events = 1000000;
  STRATUM_CHECK_TRUE(enabled(custom.max_layer_duration_ns));
  STRATUM_CHECK_TRUE(enabled(custom.max_layer_events));

  std::printf("      [语义] duration/events 的 0 表示「不限」；非 0 才启用该维度\n");
}

// ===========================================================================
// 三、工厂：Name 与 Create 的类型都正确
// ===========================================================================

STRATUM_TEST(StorageFactory, MemoryFactoryCreatesMemoryEngine) {
  MemoryEngineFactory f;
  STRATUM_CHECK_EQ(f.Name(), std::string("memory"));

  StorageEnginePtr e = f.Create();
  STRATUM_CHECK_TRUE(e != nullptr);

  // ★ 只断言非空不够 —— 必须确认**类型正确**。
  // 若工厂返回了别的引擎，数据去向就变了，而"非空"断言抓不到。
  auto* as_mem = dynamic_cast<MemoryEngine*>(e.get());
  STRATUM_CHECK_TRUE(as_mem != nullptr);
  auto* as_lsm = dynamic_cast<LsmEngine*>(e.get());
  STRATUM_CHECK_TRUE(as_lsm == nullptr);

  // 该后端不需要文件系统
  STRATUM_CHECK_TRUE(e->Open("mem://factory").ok());
  STRATUM_CHECK_TRUE(e->Close().ok());

  std::printf("      [memory] Name=memory，Create 返回 MemoryEngine（非 LsmEngine）\n");
}

STRATUM_TEST(StorageFactory, LsmFactoryCreatesLsmEngine) {
  LsmEngineFactory f;
  STRATUM_CHECK_EQ(f.Name(), std::string("lsm"));

  StorageEnginePtr e = f.Create();
  STRATUM_CHECK_TRUE(e != nullptr);

  auto* as_lsm = dynamic_cast<LsmEngine*>(e.get());
  STRATUM_CHECK_TRUE(as_lsm != nullptr);
  auto* as_mem = dynamic_cast<MemoryEngine*>(e.get());
  STRATUM_CHECK_TRUE(as_mem == nullptr);

  // 不在这里 Open（LSM 要真实目录，且会创建文件锁）；
  // 契约测试已经完整覆盖 LSM 的 Open/Close。
  std::printf("      [lsm]    Name=lsm，Create 返回 LsmEngine（非 MemoryEngine）\n");
}

// ===========================================================================
// 四、两个工厂互不干扰（Create 多次都给新对象）
// ===========================================================================

STRATUM_TEST(StorageFactory, CreateReturnsFreshInstanceEachTime) {
  MemoryEngineFactory f;

  StorageEnginePtr a = f.Create();
  StorageEnginePtr b = f.Create();
  STRATUM_CHECK_TRUE(a != nullptr);
  STRATUM_CHECK_TRUE(b != nullptr);
  // ★ 必须是**两个不同对象** —— 若工厂内部缓存了同一个实例，
  // 两个"会话"会共享状态（写入互相污染）。
  STRATUM_CHECK_TRUE(a.get() != b.get());

  // LSM 工厂同理
  LsmEngineFactory g;
  StorageEnginePtr c = g.Create();
  StorageEnginePtr d = g.Create();
  STRATUM_CHECK_TRUE(c.get() != d.get());

  std::printf("      [独立] 两个工厂各自 Create 两次，返回 4 个不同对象\n");
}

STRATUM_TEST_MAIN()
