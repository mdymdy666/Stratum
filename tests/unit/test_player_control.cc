// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// **`Player` 的控制面测试**（`docs/06` 缺口 #1）
//
// 覆盖三件事 —— 它们都是"不看代码就不知道对不对"的语义：
//   1. `Percent()` 的边界（纯函数）
//   2. `GetProgress` 的单调性（cursor / published 只增不减）
//   3. `RequestStop` 的**协作式停止**（跨线程，最容易出错）
//
// 【为什么值得单独一个文件】
// `Play` 是**阻塞**的，`RequestStop` 必须从另一个线程调。
// "会不会卡死、会不会停不下来、进度会不会倒退"只在并发时序下暴露 ——
// 而 `docs/06` 记录这组接口此前**零测试覆盖**。
//
// 【受控环境】用 mock backend + NullSink（不需要 DDS 对端），
// 所以测试快、稳定、无外部依赖。

// 标准库头文件必须最先包含
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"

namespace fs = std::filesystem;
using namespace stratum;

namespace {

std::string TempDir(const char* tag) {
  const char* home = std::getenv("HOME");
  const std::string d = std::string(home ? home : ".") + "/stratum_pc_" + tag;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

// 造 n 条样本、跨度 span_ns（同一 topic，时间均匀）
bool MakeSession(const std::string& dir, int n, uint64_t span_ns,
                 std::string* err) {
  TimelineImplV2 t;
  if (!t.Open(dir).ok()) {
    if (err) *err = "Open 失败";
    return false;
  }
  TopicMetadata m;
  m.topic_name = "pc/t";
  m.type_name = "T";
  uint32_t id = 0;
  if (!t.RegisterTopic(m, &id).ok()) {
    t.Close();
    if (err) *err = "RegisterTopic 失败";
    return false;
  }
  std::vector<Event> ev;
  ev.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    Event e;
    e.topic_id = id;
    e.seq = static_cast<uint32_t>(i + 1);
    const double f = (n > 1) ? static_cast<double>(i) / (n - 1) : 0.0;
    e.source_timestamp_ns = 1700000000000000000ull +
                            static_cast<TimestampNs>(f * static_cast<double>(span_ns));
    e.reception_timestamp_ns = e.source_timestamp_ns + 100;
    e.payload = "x";
    ev.push_back(std::move(e));
  }
  const bool ok = t.AppendBatch(ev).ok();
  t.Close();
  if (!ok && err) *err = "AppendBatch 失败";
  return ok;
}

// 永不上报对端就绪（让我们能精确控制时序）
class NullSink : public protocol::ISink {
 public:
  Status Open() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }
  bool IsOpen() const override { return true; }
  Status Publish(const protocol::ReceivedSample&) override {
    published_.fetch_add(1, std::memory_order_relaxed);
    return Status::OK();
  }
  Status PublishBatch(const std::vector<protocol::ReceivedSample>& v) override {
    published_.fetch_add(v.size(), std::memory_order_relaxed);
    return Status::OK();
  }
  Status WaitForAcknowledgments(uint64_t) override { return Status::OK(); }
  bool ReadyForPublish() const override { return true; }

  uint64_t Published() const {
    return published_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<uint64_t> published_{0};
};

// mock backend：CreateSink 返回一个 NullSink（唯一用途）
class NullBackend : public protocol::IDdsBackend {
 public:
  std::string Name() const override { return "null"; }
  std::string Version() const override { return "test"; }

  Status CreateSource(const protocol::BackendConfig&,
                      protocol::SourcePtr* out) override {
    if (out) out->reset();
    return Status::NotSupported("测试不提供 source");
  }

  Status CreateSink(const protocol::BackendConfig&,
                    protocol::SinkPtr* out) override {
    if (out == nullptr) return Status::InvalidArgument("out 为空");
    out->reset(new NullSink());
    return Status::OK();
  }

  // 控制面测试不关心后端能力；全 false 表示"什么都不提供"。
  // （漏了这个纯虚函数会让 NullBackend 成为抽象类，编译期才报错。）
  Capabilities GetCapabilities() const override { return Capabilities{}; }
};

}  // namespace

// ===========================================================================
// 一、`Percent()` 的边界
// ===========================================================================

STRATUM_TEST(PlayerControl, PercentHandlesEdges) {
  PlaybackProgress p;
  p.begin_ns = 100;
  p.end_ns = 100;      // end == begin → 0，不能除零
  p.cursor_ns = 100;
  STRATUM_CHECK_TRUE(p.Percent() == 0.0);

  p.end_ns = 50;       // end < begin（异常数据）
  STRATUM_CHECK_TRUE(p.Percent() == 0.0);

  p.begin_ns = 0;
  p.end_ns = 100;
  p.cursor_ns = 50;
  STRATUM_CHECK_TRUE(p.Percent() > 49.9 && p.Percent() < 50.1);

  p.cursor_ns = 200;   // 越过终点 → 100，不能超 100
  STRATUM_CHECK_TRUE(p.Percent() == 100.0);

  std::printf("      [边界] end<=begin→0；一半→50%%；越过终点→100（不超 100）\n");
}

// ===========================================================================
// 二、★ `RequestStop` 的协作式停止（核心用例）
// ===========================================================================
//
// 造 100 条、跨度 12 秒，rate=1 时回放约 12 秒。
// 300ms 后从另一线程 RequestStop，验证：
//   · Play 提前返回（远早于 12 秒）
//   · IsPlaying 转 false
//   · 实际发布的条数 < 100（真被中断，不是跑完了）

STRATUM_TEST(PlayerControl, RequestStopInterruptsPlay) {
  const std::string dir = TempDir("stop");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 100, 12000000000ull, &err));

  auto timeline = std::make_unique<TimelineImplV2>();
  STRATUM_CHECK_TRUE(timeline->Open(dir).ok());

  NullBackend backend;
  Player player(timeline.get(), &backend);

  PlayerConfig cfg;
  cfg.rate = 1.0;                    // 原速 → 约 12 秒
  cfg.discovery_wait_ms = 0;         // NullSink 天然就绪
  cfg.wait_for_ack = false;
  cfg.batch_size = 1;

  std::atomic<bool> play_done{false};
  std::atomic<int64_t> elapsed_ms{0};
  const auto t0 = std::chrono::steady_clock::now();

  std::thread runner([&]() {
    const Status st = player.Play(TimeRange{}, cfg);
    (void)st;
    play_done.store(true, std::memory_order_release);
  });

  // 等它真正开始（IsPlaying 转 true）
  bool started = false;
  for (int i = 0; i < 100 && !started; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    started = player.IsPlaying();
  }
  STRATUM_CHECK_TRUE(started);

  // 让它跑 300ms 再请求停止
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const PlaybackProgress mid = player.GetProgress();
  STRATUM_CHECK_TRUE(mid.published > 0);   // 确实在发
  STRATUM_CHECK_TRUE(mid.published < 100); // 但没跑完

  player.RequestStop();

  // 等 Play 返回（最多 5 秒；超过说明停不下来）
  for (int i = 0; i < 250 && !play_done.load(std::memory_order_acquire); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  elapsed_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count());

  STRATUM_CHECK_TRUE(play_done.load(std::memory_order_acquire));   // ★ 停下来了
  STRATUM_CHECK_FALSE(player.IsPlaying());                          // ★ 状态已转
  STRATUM_CHECK_LT(elapsed_ms.load(), 6000);                        // ★ 远早于 12 秒

  const PlaybackProgress fin = player.GetProgress();
  STRATUM_CHECK_TRUE(fin.published < 100);                          // ★ 真被中断

  runner.join();
  timeline->Close();

  std::printf("      [停止] 请求后 %lld ms 返回；发布 %llu/%llu 条（<100 说明真中断）\n",
              static_cast<long long>(elapsed_ms.load()),
              static_cast<unsigned long long>(fin.published),
              static_cast<unsigned long long>(fin.total));
}

// ===========================================================================
// 三、`GetProgress` 的单调性
// ===========================================================================
//
// 回放过程中连续采样，验证 cursor_ns 与 published **只增不减**。
// 非单调意味着进度会"倒退"，而 CLI 的百分比会跳来跳去 —— 用户会以为卡了。

STRATUM_TEST(PlayerControl, ProgressIsMonotonic) {
  const std::string dir = TempDir("mono");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 60, 6000000000ull, &err));  // 6 秒

  auto timeline = std::make_unique<TimelineImplV2>();
  STRATUM_CHECK_TRUE(timeline->Open(dir).ok());

  NullBackend backend;
  Player player(timeline.get(), &backend);

  PlayerConfig cfg;
  cfg.rate = 1.0;
  cfg.discovery_wait_ms = 0;
  cfg.wait_for_ack = false;
  cfg.batch_size = 1;

  std::atomic<bool> play_done{false};
  std::thread runner([&]() {
    player.Play(TimeRange{}, cfg);
    play_done.store(true, std::memory_order_release);
  });

  bool started = false;
  for (int i = 0; i < 100 && !started; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    started = player.IsPlaying();
  }

  TimestampNs last_cursor = 0;
  uint64_t last_pub = 0;
  bool monotonic = true;
  int samples = 0;
  for (int i = 0; i < 50; ++i) {   // 最多 1 秒采样
    const PlaybackProgress p = player.GetProgress();
    if (p.cursor_ns < last_cursor || p.published < last_pub) {
      monotonic = false;
      break;
    }
    last_cursor = p.cursor_ns;
    last_pub = p.published;
    ++samples;
    if (play_done.load(std::memory_order_acquire)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  player.RequestStop();
  for (int i = 0; i < 250 && !play_done.load(std::memory_order_acquire); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  runner.join();
  timeline->Close();

  STRATUM_CHECK_TRUE(monotonic);
  STRATUM_CHECK_GT(samples, 3);
  std::printf("      [单调] 采样 %d 次，cursor/published 均未倒退\n", samples);
}

// ===========================================================================
// 四、未开始回放时 `IsPlaying` 为 false、进度为空
// ===========================================================================

STRATUM_TEST(PlayerControl, IdleStateIsClean) {
  const std::string dir = TempDir("idle");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 10, 100000000ull, &err));

  auto timeline = std::make_unique<TimelineImplV2>();
  STRATUM_CHECK_TRUE(timeline->Open(dir).ok());

  NullBackend backend;
  Player player(timeline.get(), &backend);

  // 从未 Play → 不应是"正在回放"
  STRATUM_CHECK_FALSE(player.IsPlaying());
  const PlaybackProgress p = player.GetProgress();
  STRATUM_CHECK_EQ(p.published, static_cast<uint64_t>(0));

  // 未回放时 RequestStop 不应崩、不应改变状态
  player.RequestStop();
  STRATUM_CHECK_FALSE(player.IsPlaying());

  timeline->Close();
  std::printf("      [空闲] 未 Play 时 IsPlaying=false、published=0、RequestStop 安全\n");
}

STRATUM_TEST_MAIN()
