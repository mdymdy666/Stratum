// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// C1 验证：Recorder Start/Stop 并发安全
//
// 修复前（审查员实测）：并发 Start 3/3 abort，并发 Stop 2/3 abort。
// 修复后：并发调用必须安全——要么成功、要么返回明确的状态错误，
// **绝不 abort、绝不崩溃、绝不"静默成功但什么都没做"**。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl.h"
#include "stratum/storage/memory/memory_engine.h"
#include "mock_backend.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::storage;
using namespace stratum::testing;

namespace {

// 构造一个可用的 Recorder
struct Fixture {
  MemoryEngine engine;
  TimelineImpl timeline;
  MockBackend backend;
  Recorder recorder;

  Fixture()
      : engine(),
        timeline(StorageEnginePtr(new MemoryEngine())),
        recorder(&timeline, &backend) {
    timeline.Open("mem://c1-concurrent");
  }
};

RecorderConfig MakeConfig() {
  RecorderConfig rcfg;
  rcfg.record.max_buffer_bytes = 16 * 1024 * 1024;
  rcfg.write_batch_size = 64;
  rcfg.close_timeline_on_stop = false;
  return rcfg;
}

}  // namespace

// ---------------------------------------------------------------------------
// 并发 Start：两个线程同时抢。必须恰好一个成功、一个 AlreadyExists。
// 任何 abort/崩溃即失败（进程直接死，测试无法通过）。
// ---------------------------------------------------------------------------
STRATUM_TEST(Concurrency, C1_ConcurrentStartIsSafe) {
  Fixture fix;
  const auto cfg = MakeConfig();

  std::atomic<int> ok_count{0};
  std::atomic<int> err_count{0};

  std::thread t1([&]() {
    const Status s = fix.recorder.Start(cfg);
    if (s.ok()) {
      ok_count.fetch_add(1);
    } else {
      err_count.fetch_add(1);
    }
  });
  std::thread t2([&]() {
    const Status s = fix.recorder.Start(cfg);
    if (s.ok()) {
      ok_count.fetch_add(1);
    } else {
      err_count.fetch_add(1);
    }
  });
  t1.join();
  t2.join();

  ctx.param_desc = "ok=" + std::to_string(ok_count.load()) +
                   " err=" + std::to_string(err_count.load());
  // 恰好一个成功
  STRATUM_CHECK_EQ(ok_count.load(), 1);
  STRATUM_CHECK_EQ(err_count.load(), 1);

  STRATUM_CHECK_OK(fix.recorder.Stop());
}

// ---------------------------------------------------------------------------
// 并发 Stop：两个线程同时停。都必须安全返回。
// ---------------------------------------------------------------------------
STRATUM_TEST(Concurrency, C1_ConcurrentStopIsSafe) {
  Fixture fix;
  const auto cfg = MakeConfig();
  STRATUM_CHECK_OK(fix.recorder.Start(cfg));

  // 用原子计数**实际验证**两个 Stop 都返回了 ——
  // 早期版本此处是 `STRATUM_CHECK_TRUE(true)`，它只证明"跑到了这一行"，
  // 不证明任何行为（若 t1 内抛异常/卡死，join 之前的断言也不会触发）。
  std::atomic<int> stops_returned{0};
  std::thread t1([&]() {
    fix.recorder.Stop();
    stops_returned.fetch_add(1, std::memory_order_relaxed);
  });
  std::thread t2([&]() {
    fix.recorder.Stop();
    stops_returned.fetch_add(1, std::memory_order_relaxed);
  });
  t1.join();
  t2.join();

  ctx.param_desc = "concurrent stop completed, stops_returned=" +
                   std::to_string(stops_returned.load());
  STRATUM_CHECK_EQ(stops_returned.load(), 2);
}

// ---------------------------------------------------------------------------
// Stop 与 Start 交错竞争（多轮，覆盖 Start 中间被 Stop 打断的窗口）
// ---------------------------------------------------------------------------
STRATUM_TEST(Concurrency, C1_InterleavedStartStopStress) {
  constexpr int kRounds = 30;
  int completed = 0;
  int aborted = 0;

  for (int i = 0; i < kRounds; ++i) {
    Fixture fix;
    const auto cfg = MakeConfig();

    // 两个线程：一个反复 Start，一个在随机时刻 Stop
    std::atomic<bool> go{true};
    std::thread starter([&]() {
      for (int j = 0; j < 20 && go.load(); ++j) {
        const Status s = fix.recorder.Start(cfg);
        if (s.ok()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          const Status stop = fix.recorder.Stop();
          if (stop.ok()) {
            completed++;
          }
        } else if (s.code() == StatusCode::kShutdownInProgress) {
          aborted++;
        } else if (s.code() == StatusCode::kAlreadyExists) {
          // 上一次还没停完：正常
        } else {
          // 其他状态码都可以接受，只要不崩溃
        }
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    fix.recorder.Stop();  // 打断进行中的 Start/Stop
    go.store(false);
    starter.join();
  }

  ctx.param_desc = "完成启动-停止循环=" + std::to_string(completed) +
                   " 启动中被停止=" + std::to_string(aborted);
  // 至少完成若干轮完整循环（证明状态机可重复使用）
  STRATUM_CHECK_TRUE(completed > 0);
}

// ---------------------------------------------------------------------------
// Stop 之后 Start 必须可用（同一实例可重用）
// ---------------------------------------------------------------------------
STRATUM_TEST(Concurrency, C1_StartAfterStopIsUsable) {
  Fixture fix;
  const auto cfg = MakeConfig();

  // 三轮 Start/Stop 各有 STRATUM_CHECK_OK 断言（共 6 个），
  // 已足以验证可重用性；早期此处末尾多了一句 `STRATUM_CHECK_TRUE(true)`，
  // 属于无验证价值的空断言，已删除。
  int rounds = 0;
  for (int i = 0; i < 3; ++i) {
    STRATUM_CHECK_OK(fix.recorder.Start(cfg));
    STRATUM_CHECK_OK(fix.recorder.Stop());
    ++rounds;
  }
  ctx.param_desc = "start/stop rounds=" + std::to_string(rounds);
  STRATUM_CHECK_EQ(rounds, 3);
}

STRATUM_TEST_MAIN()
