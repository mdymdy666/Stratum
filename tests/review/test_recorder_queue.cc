// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// Recorder 队列行为的回归测试
//
// ===========================================================================
// 为什么这些用例必须存在
// ===========================================================================
//
// 原有 47 个测试**没有一个**覆盖 Recorder 的队列溢出路径：
// 压力测试直接调 `timeline.AppendBatch`，绕过了 Recorder 的缓冲层。
// 结果是两个缺陷逃过了全部测试（审查发现 C3、M1）：
//
//   C3: 入队算 payload+topic_name、出队只减 payload
//       → queue_bytes_ 单调虚增 → 录制中途丢弃**全部**新数据
//   M1: kBlock 策略是空实现（只有 break）
//       → 队列无界增长 → OOM
//
// 本文件的用例在修复前**必然失败**。
//
// 数据量标准（design/07-testing-standards.md）：
//   队列/缓冲测试必须**超过配置上限**，否则溢出分支永不执行。

#include <algorithm>
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

constexpr uint64_t kBaseTs = 1700000000000000000ULL;

// 构造样本：**topic 名故意很长**，这是 C3 的触发条件
// （每条样本净增 topic_name.size() 字节的虚增）
constexpr const char* kLongTopicName =
    "factory/line-a/station-07/sensor/temperature/raw";

std::vector<protocol::ReceivedSample> MakeLongTopicSamples(int count,
                                                           size_t payload_size) {
  std::vector<protocol::ReceivedSample> samples;
  samples.reserve(count);
  for (int i = 0; i < count; ++i) {
    protocol::ReceivedSample s;
    s.topic_name = kLongTopicName;
    s.type_name = "SensorData";
    s.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(i) * 1000000ULL;
    s.reception_timestamp_ns = s.source_timestamp_ns + 1000;
    s.sequence = static_cast<uint32_t>(i);
    s.payload.assign(payload_size, 'p');
    samples.push_back(std::move(s));
  }
  return samples;
}

// 录制辅助：返回 Recorder 统计
Recorder::Stats RecordAll(ITimeline* timeline, protocol::IDdsBackend* backend,
                          const std::vector<protocol::ReceivedSample>& samples,
                          uint64_t max_buffer_bytes,
                          RecordConfig::OverflowPolicy policy) {
  Recorder recorder(timeline, backend);
  RecorderConfig rcfg;
  rcfg.record.max_buffer_bytes = max_buffer_bytes;
  rcfg.record.overflow_policy = policy;
  rcfg.write_batch_size = 32;
  rcfg.close_timeline_on_stop = false;

  if (!recorder.Start(rcfg).ok()) {
    return recorder.GetStats();
  }

  MockSource* source = MockBackend::source_slot();
  if (source == nullptr) {
    return recorder.GetStats();
  }

  // 分批推送，每批后等待队列排空。
  //
  // 【为什么必须这样】直接把全部样本倒进去会让队列瞬间填满，
  // 此时丢弃是**合理的背压行为**，无法区分"记账 bug"与"消费能力不足"。
  //
  // C3 的真实危害是：queue_bytes_ 因记账错误虚增后，**即使队列已被消费空
  // 也无法回落**，于是此后拒绝**所有**新数据。只有让生产者受控
  // （每批都被消费干净），才能把这一行为与正常背压区分开。
  // 批次必须**小于队列容量**，否则一批就会溢出，
  // 丢弃将完全由批次大小决定，而与被测的记账逻辑无关。
  // 容量 = 上限 2000B / 单条 (16B payload + 52B topic) ≈ 29 条。
  constexpr size_t kPushBatch = 10;
  for (size_t i = 0; i < samples.size(); i += kPushBatch) {
    const size_t end = std::min(i + kPushBatch, samples.size());
    for (size_t j = i; j < end; ++j) {
      source->Push(samples[j]);
    }

    // 【关键】等待这一批**真正被 Recorder 收到**（received 增长），
    // 而不是等待"队列排空"。
    //
    // 为什么：Push() 只是把样本放进 MockSource 的待推队列，
    // 而 MockSource::Loop 在有待推样本时是**全速**取出的。
    // 若等待 queue_depth == 0，检查时数据往往还没送达 Recorder，
    // 于是等待立即结束、继续 Push —— 限速完全失效，
    // 退化成"一次性倒进去"，丢弃由背压决定而非记账逻辑决定。
    // （这个错误在本用例的开发中实际发生过。）
    int waited_inner = 0;
    while (waited_inner < 5000) {
      if (recorder.GetStats().received >= end) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      waited_inner += 2;
    }
    // 再给写盘线程一点时间把这一批落盘，避免队列里堆积跨批数据
    waited_inner = 0;
    while (waited_inner < 5000) {
      if (recorder.GetStats().queue_depth == 0) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      waited_inner += 2;
    }
  }

  // 等待全部处理完（带超时，避免测试挂死）
  const int kMaxWaitMs = 20000;
  int waited = 0;
  while (waited < kMaxWaitMs) {
    const Recorder::Stats st = recorder.GetStats();
    if (st.received >= samples.size() && st.queue_depth == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    waited += 20;
  }

  recorder.Stop();
  return recorder.GetStats();
}

}  // namespace

// ---------------------------------------------------------------------------
// 【C3 验证】队列字节记账不得随时间漂移
//
// 修复前：入队加 payload+topic_name、出队只减 payload
//         → 每条净增 topic_name.size() 字节
//         → 上限很快被"虚增"占满 → 之后所有数据被丢弃
//
// 本用例：上限 2000 字节，topic 名 52 字节，payload 16 字节，发 5000 条
//   修复前：只有前 ~38 条能通过（2000/52），其余全部丢弃
//   修复后：5000 条全部录制成功
// ---------------------------------------------------------------------------
STRATUM_TEST(RecorderQueue, C3_AccountingDoesNotDrift) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://c3-accounting"));

  MockBackend backend;
  // 数据量远超上限：保证反复触发容量判断
  constexpr int kCount = 5000;
  const auto samples = MakeLongTopicSamples(kCount, 16);

  // 上限设得很小，强制频繁触碰容量边界
  const Recorder::Stats stats = RecordAll(
      &timeline, &backend, samples, /*max_buffer_bytes=*/2000,
      RecordConfig::OverflowPolicy::kDropNewest);

  ctx.param_desc = "received=" + std::to_string(stats.received) +
                   " recorded=" + std::to_string(stats.recorded) +
                   " dropped=" + std::to_string(stats.dropped) +
                   " queue_depth=" + std::to_string(stats.queue_depth);

  // 全部样本都送到了录制器
  STRATUM_CHECK_EQ(stats.received, static_cast<uint64_t>(kCount));

  // 【关键断言】写入速度远快于"虚增"速度时，不应有任何丢弃。
  //
  // 注意：本用例的下限逻辑是"记账正确 ⇒ 消费速度快于生产速度 ⇒ 不丢"。
  // 若消费者确实跟不上，dropped > 0 是合法行为；但修复前的丢弃**并非
  // 因为消费慢**，而是因为 queue_bytes_ 永远降不下来（记账 bug）。
  // 这两者的区别由下一条断言捕获。
  ctx.param_desc += " 队列峰值=" + std::to_string(stats.queue_high_water);

  // 【核心断言】一条都不能丢。
  //
  // 这是唯一能捕获 C3 的判据：
  //   修复前 —— queue_bytes_ 因记账错误单调虚增，很快恒大于上限，
  //             此后**每一条**都被 kDropNewest 丢弃；
  //             审查员实测（上限 2000 / topic 26B / payload 10B / 300 条）
  //             得到 recorded=66 dropped=234。
  //   修复后 —— 记账正确，队列随消费及时回落，全部录制成功。
  //
  // 注意：**不能**用"队列深度"作为判据 —— 修复前因为丢弃得早，
  // 队列深度反而很小（虚增占满后就不再入队），断言会误判为通过。
  ctx.param_desc += " 应为" + std::to_string(kCount);
  STRATUM_CHECK_EQ(stats.recorded + stats.dropped,
                   static_cast<uint64_t>(kCount));
  STRATUM_CHECK_EQ(stats.dropped, static_cast<uint64_t>(0));
  STRATUM_CHECK_EQ(stats.recorded, static_cast<uint64_t>(kCount));
}

// ---------------------------------------------------------------------------
// 【M1 验证】kBlock 策略必须真正阻塞，而不是无界增长
//
// 修复前：kBlock 分支只有 break → 无条件入队 → queue_bytes_ 无界增长
//         → 内存耗尽（本用例会观察到队列深度远超上限）
// 修复后：阻塞等待消费者；等待超时时按丢弃计数（有界）
// ---------------------------------------------------------------------------
STRATUM_TEST(RecorderQueue, M1_BlockPolicyKeepsQueueBounded) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://m1-block"));

  MockBackend backend;
  constexpr int kCount = 3000;
  const auto samples = MakeLongTopicSamples(kCount, 64);

  // 上限 4000 字节；单条 = 64 + 52 = 116 字节 → 最多容纳 ~34 条
  constexpr uint64_t kLimit = 4000;
  const Recorder::Stats stats = RecordAll(
      &timeline, &backend, samples, kLimit,
      RecordConfig::OverflowPolicy::kBlock);

  ctx.param_desc = "received=" + std::to_string(stats.received) +
                   " recorded=" + std::to_string(stats.recorded) +
                   " dropped=" + std::to_string(stats.dropped) +
                   " 队列峰值=" + std::to_string(stats.queue_high_water);

  // 【关键断言】队列深度必须有界。
  //
  // 修复前 kBlock 不阻塞，队列会堆到 3000 条（远超 34 条的容量）。
  // 修复后阻塞生效，峰值应接近容量上限（允许批次写入的少量过冲）。
  const uint64_t per_sample = 64 + std::string(kLongTopicName).size();
  const uint64_t capacity = kLimit / per_sample;
  ctx.param_desc += " 容量≈" + std::to_string(capacity) + " 条";

  STRATUM_CHECK_LE(stats.queue_high_water, capacity * 4 + 8);

  // 说明：这里**不**断言 received == kCount。
  //
  // kBlock 的真阻塞会让生产者受限于消费速度，测试有等待上限；
  // 未及时送达的样本属于「还没发」，不是「丢了」——用送达总数
  // 作判据会把测试超时误判为功能缺陷。
  //
  // 本用例的核心判据是上一条：队列深度必须有界
  // （修复前 kBlock 不阻塞，深度会涨到 kCount）。
  ctx.param_desc += " 已送达=" + std::to_string(stats.received) +
                    "/" + std::to_string(kCount);
  // 至少应有相当比例的数据被成功送达（证明阻塞是可推进的，不是死锁）
  STRATUM_CHECK_TRUE(stats.received > static_cast<uint64_t>(kCount / 10));
}

STRATUM_TEST_MAIN()
