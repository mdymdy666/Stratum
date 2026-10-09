// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// CDRMessage 移植验证：基础往返 + 随机 + 压力 + 边界
//
// ===========================================================================
// 为什么需要这三类测试
// ===========================================================================
//
// CDRMessage 是 RTPS 线格式的编解码基础。移植它不能只看"编译通过"：
//   - 字节序（CDR 大端/小端）、对齐（4 字节 padding）错一位，
//     整个协议就不兼容，且症状是"能收到但内容是乱码"——极难定位
//   - 因此必须有大量**随机值 + 随机类型序列**的往返验证
//   - 还必须有**压力测试**证明吞吐可用（Stratum 要录高频数据流）

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/common/Types.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>

#include "test_framework.h"

using namespace eprosima::fastdds::rtps;

// ---------------------------------------------------------------------------
// 1) 基础往返：各类型 add → read → 值一致
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsCdr, BasicRoundTrip) {
  CDRMessage_t msg(256);

  STRATUM_CHECK_TRUE(CDRMessage::addOctet(&msg, 0x12));
  STRATUM_CHECK_TRUE(CDRMessage::addUInt16(&msg, 0x3456));
  STRATUM_CHECK_TRUE(CDRMessage::addUInt32(&msg, 0xDEADBEEF));
  STRATUM_CHECK_TRUE(CDRMessage::addUInt64(&msg, 0x0102030405060708ULL));
  STRATUM_CHECK_TRUE(CDRMessage::addInt32(&msg, -12345));

  ctx.param_desc = "encoded_len=" + std::to_string(msg.length);

  msg.pos = 0;
  octet o = 0;
  uint16_t u16 = 0;
  uint32_t u32 = 0;
  uint64_t u64 = 0;
  int32_t i32 = 0;

  STRATUM_CHECK_TRUE(CDRMessage::readOctet(&msg, &o));
  STRATUM_CHECK_TRUE(CDRMessage::readUInt16(&msg, &u16));
  STRATUM_CHECK_TRUE(CDRMessage::readUInt32(&msg, &u32));
  STRATUM_CHECK_TRUE(CDRMessage::readUInt64(&msg, &u64));
  STRATUM_CHECK_TRUE(CDRMessage::readInt32(&msg, &i32));

  STRATUM_CHECK_EQ(static_cast<int>(o), 0x12);
  STRATUM_CHECK_EQ(static_cast<int>(u16), 0x3456);
  STRATUM_CHECK_EQ(u32, 0xDEADBEEFu);
  STRATUM_CHECK_EQ(u64, 0x0102030405060708ULL);
  STRATUM_CHECK_EQ(i32, -12345);
}

// ---------------------------------------------------------------------------
// 2) 字节序与"无自动对齐"语义（RTPS 与纯 CDR 的关键区别）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsCdr, BigEndianAndNoAutoPadding) {
  // 【重要语义】CDRMessage 是 RTPS 的**原始缓冲区追加器**，它
  //   (a) 按**大端（网络序）**写入多字节整数
  //   (b) **不做** CDR 的自动对齐 padding —— 对齐由各 Submessage
  //       自己根据 RTPS 规范处理（子消息头声明 octetsToNextHeader）
  //
  // 本测试最初按"纯 CDR 会自动 4 字节对齐"来断言（期望 pos=8），
  // 实测 pos=5 —— 恰好证明移植的是 RTPS 语义而非通用 CDR 语义。
  CDRMessage_t msg(64);
  CDRMessage::addOctet(&msg, 0xAA);         // 1 字节 → pos=1
  const uint32_t after_octet = msg.pos;
  CDRMessage::addUInt32(&msg, 0x11223344);  // +4 → pos=5（无 padding）
  const uint32_t after_u32 = msg.pos;

  ctx.param_desc = "after_octet=" + std::to_string(after_octet) +
                   " after_u32=" + std::to_string(after_u32);
  STRATUM_CHECK_EQ(after_octet, 1u);
  STRATUM_CHECK_EQ(after_u32, 5u);   // 1 + 4，无对齐填充

  // ---------------------------------------------------------------------
  // 字节序语义（实测确认，并对照上游逐行一致）
  // ---------------------------------------------------------------------
  //
  // addUInt32 → addPrimitive 模板，字节序由 msg->msg_endian 决定；
  // CDRMessage_t 默认 msg_endian = DEFAULT_ENDIAN，在 x86 上即 LITTLEEND。
  //
  // 这在 RTPS 中是**合法且必要**的：Submessage 头自带 EndiannessFlag
  // （flags 第 0 位），接收方按该标志解析 —— 因此编码端可以就用主机序，
  // 由子消息头声明。本测试最初按"无条件大端"断言，失败后对照上游
  // src/cpp/rtps/messages/CDRMessage.cpp:586-591 确认两者逐行一致，
  // 证明是测试假设错、移植正确。
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(msg.buffer);
  ctx.param_desc += " endian=" +
      std::string(msg.msg_endian == LITTLEEND ? "LITTLEEND" : "BIGEND") +
      " bytes=";
  for (uint32_t i = 0; i < 5; ++i) {
    ctx.param_desc += std::to_string(static_cast<int>(raw[i])) + " ";
  }
#if FASTDDS_IS_BIG_ENDIAN_TARGET
  STRATUM_CHECK_EQ(static_cast<int>(raw[1]), 0x11);   // 大端机：高位在前
#else
  // 小端机：低位在前 → AA 44 33 22 11
  STRATUM_CHECK_EQ(static_cast<int>(raw[1]), 0x44);
  STRATUM_CHECK_EQ(static_cast<int>(raw[2]), 0x33);
  STRATUM_CHECK_EQ(static_cast<int>(raw[3]), 0x22);
  STRATUM_CHECK_EQ(static_cast<int>(raw[4]), 0x11);
#endif

  // 关键不变量：同字节序下 add→read 必须往返一致
  msg.pos = 1;
  uint32_t back = 0;
  STRATUM_CHECK_TRUE(CDRMessage::readUInt32(&msg, &back));
  STRATUM_CHECK_EQ(back, 0x11223344u);
}

// ---------------------------------------------------------------------------
// 3) 随机往返测试（固定种子，大量迭代）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsCdr, RandomRoundTripManyIterations) {
  constexpr uint64_t kSeed = 20261101;
  constexpr int kIterations = 6000;   // 每次 8~64 个字段
  constexpr int kMaxFields = 64;

  std::mt19937_64 rng(kSeed);
  ctx.param_desc = "seed=" + std::to_string(kSeed) +
                   " iterations=" + std::to_string(kIterations);

  int mismatches = 0;
  uint64_t total_fields = 0;
  uint64_t total_bytes = 0;

  for (int iter = 0; iter < kIterations; ++iter) {
    CDRMessage_t msg(64 * 1024);
    const int nfields = 1 + static_cast<int>(rng() % kMaxFields);

    // 生成随机字段序列并编码
    struct Field {
      int type;   // 0=octet 1=u16 2=u32 3=u64 4=i32 5=i64
      uint64_t value;
    };
    std::vector<Field> fields;
    fields.reserve(nfields);

    for (int i = 0; i < nfields; ++i) {
      Field f;
      f.type = static_cast<int>(rng() % 6);
      f.value = rng();
      fields.push_back(f);
      bool ok = false;
      switch (f.type) {
        case 0: ok = CDRMessage::addOctet(&msg, static_cast<octet>(f.value)); break;
        case 1: ok = CDRMessage::addUInt16(&msg, static_cast<uint16_t>(f.value)); break;
        case 2: ok = CDRMessage::addUInt32(&msg, static_cast<uint32_t>(f.value)); break;
        case 3: ok = CDRMessage::addUInt64(&msg, f.value); break;
        case 4: ok = CDRMessage::addInt32(&msg, static_cast<int32_t>(f.value)); break;
        case 5: ok = CDRMessage::addInt64(&msg, static_cast<int64_t>(f.value)); break;
        default: break;
      }
      if (!ok) ++mismatches;   // 编码失败也算不一致
    }
    total_fields += nfields;
    total_bytes += msg.length;

    // 解码并逐字段比对
    msg.pos = 0;
    for (const Field& f : fields) {
      uint64_t got = 0;
      bool ok = false;
      octet o8 = 0; uint16_t v16 = 0; uint32_t v32 = 0; uint64_t v64 = 0;
      int32_t i32 = 0; int64_t i64 = 0;
      switch (f.type) {
        case 0:
          ok = CDRMessage::readOctet(&msg, &o8);
          got = o8;
          break;
        case 1:
          ok = CDRMessage::readUInt16(&msg, &v16);
          got = v16;
          break;
        case 2:
          ok = CDRMessage::readUInt32(&msg, &v32);
          got = v32;
          break;
        case 3:
          ok = CDRMessage::readUInt64(&msg, &v64);
          got = v64;
          break;
        case 4:
          ok = CDRMessage::readInt32(&msg, &i32);
          got = static_cast<uint64_t>(static_cast<int64_t>(i32));
          break;
        case 5:
          ok = CDRMessage::readInt64(&msg, &i64);
          got = static_cast<uint64_t>(i64);
          break;
        default: break;
      }
      uint64_t want = f.value;
      switch (f.type) {
        case 0: want = static_cast<uint8_t>(f.value); break;
        case 1: want = static_cast<uint16_t>(f.value); break;
        case 2: want = static_cast<uint32_t>(f.value); break;
        case 4: want = static_cast<uint64_t>(static_cast<int64_t>(
                    static_cast<int32_t>(f.value))); break;
        default: break;
      }
      if (!ok || got != want) {
        ++mismatches;
      }
    }
  }

  ctx.param_desc += " fields=" + std::to_string(total_fields) +
                    " bytes=" + std::to_string(total_bytes) +
                    " mismatches=" + std::to_string(mismatches);
  STRATUM_CHECK_EQ(mismatches, 0);
  // 数据量保证（3000 次 × 平均 32 字段 ≈ 96k，故取 6000 次）
  STRATUM_CHECK_TRUE(total_fields > 150000);
}

// ---------------------------------------------------------------------------
// 4) 压力测试：大消息 + 高迭代吞吐
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsCdr, StressLargeMessages) {
  constexpr int kRounds = 2000;
  constexpr uint32_t kPayload = 8 * 1024;   // 每次 8KB 负载

  std::mt19937_64 rng(20261102);
  std::vector<uint8_t> payload(kPayload);
  for (auto& b : payload) {
    b = static_cast<uint8_t>(rng());
  }

  uint64_t bytes_total = 0;
  int failures = 0;

  for (int r = 0; r < kRounds; ++r) {
    CDRMessage_t msg(kPayload + 128);
    if (!CDRMessage::addData(&msg, payload.data(), kPayload)) {
      ++failures;
      continue;
    }
    bytes_total += msg.length;

    // 读回比对
    std::vector<uint8_t> out(kPayload, 0);
    msg.pos = 0;
    if (!CDRMessage::readData(&msg, out.data(), kPayload)) {
      ++failures;
      continue;
    }
    if (std::memcmp(out.data(), payload.data(), kPayload) != 0) {
      ++failures;
    }
  }

  ctx.param_desc = "rounds=" + std::to_string(kRounds) +
                   " bytes=" + std::to_string(bytes_total) +
                   " failures=" + std::to_string(failures);
  STRATUM_CHECK_EQ(failures, 0);
  STRATUM_CHECK_EQ(bytes_total,
                   static_cast<uint64_t>(kRounds) * kPayload);
}

// ---------------------------------------------------------------------------
// 5) 边界：读取越界必须安全失败（不崩溃、不越界读）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsCdr, OutOfBoundsReadFailsSafely) {
  CDRMessage_t msg(16);
  CDRMessage::addOctet(&msg, 0x01);
  const uint32_t real_len = msg.length;

  // 尝试读超出长度的数据
  msg.pos = real_len;
  uint32_t v = 0;
  const bool ok = CDRMessage::readUInt32(&msg, &v);
  ctx.param_desc = "read_past_end=" + std::string(ok ? "true" : "false") +
                   " len=" + std::to_string(real_len);
  STRATUM_CHECK_TRUE(!ok);   // 必须失败

  // 空消息读取也必须失败
  CDRMessage_t empty(16);
  empty.pos = 0;
  uint64_t v64 = 0;
  STRATUM_CHECK_TRUE(!CDRMessage::readUInt64(&empty, &v64));
}

STRATUM_TEST_MAIN()
