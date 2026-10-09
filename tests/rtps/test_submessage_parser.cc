// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 子消息解析器验证：随机往返 + 压力 + 真实流量
//
// ===========================================================================
// 闭环设计
// ===========================================================================
//
//   编码侧：Fast DDS **移植的** RTPSMessageCreator::addSubmessageXxx
//   解码侧：Stratum 解析器（解析段同样移植自 MessageReceiver.cpp）
//
// 二者独立来源、同一协议规范 → 逐字段比对即可发现任何一侧的偏差。
// 另加真实流量用例（tests/vectors/spdp_capture.bin）作为第三方证据。

#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/common/SequenceNumber.hpp>
#include <stratum/rtps/common/Types.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>
#include <stratum/rtps/messages/RTPSMessageCreator.hpp>
#include <stratum/rtps/messages/submessage_parser.h>

#include "test_framework.h"

using namespace eprosima::fastdds::rtps;
using namespace stratum::rtps;

namespace {

// 供测试使用的编码器封装（把 CDRMessage_t 归零后再编码）
void ResetMsg(CDRMessage_t* msg) {
  msg->pos = 0;
  msg->length = 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1) HEARTBEAT 随机往返
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSubmsg, HeartbeatRandomRoundTrip) {
  constexpr uint64_t kSeed = 20261110;
  constexpr int kIterations = 20000;

  std::mt19937_64 rng(kSeed);
  SubmessageParser parser;
  int mismatches = 0;

  for (int i = 0; i < kIterations; ++i) {
    CDRMessage_t msg(512);
    ResetMsg(&msg);

    EntityId_t reader(static_cast<uint32_t>(rng() % 0xFFFF));
    EntityId_t writer(static_cast<uint32_t>(rng() % 0xFFFF));
    // firstSN 必须 >= 1（协议要求）
    SequenceNumber_t firstSN(0, static_cast<uint32_t>(1 + rng() % 1000000));
    SequenceNumber_t lastSN(0, firstSN.low + static_cast<uint32_t>(rng() % 1000));
    const uint32_t count = static_cast<uint32_t>(rng());
    const bool isFinal = (rng() & 1) != 0;
    const bool liveliness = (rng() & 1) != 0;

    if (!RTPSMessageCreator::addSubmessageHeartbeat(
            &msg, reader, writer, firstSN, lastSN, count, isFinal, liveliness)) {
      ++mismatches;
      continue;
    }

    // 解析：跳过 4 字节子消息头
    msg.pos = 0;
    SubmessageHeader_t smh;
    if (!parser.ReadSubmessageHeader(&msg, &smh)) {
      ++mismatches;
      continue;
    }
    HeartbeatInfo hb;
    if (!parser.ProcHeartbeat(&msg, &smh, &hb)) {
      ++mismatches;
      continue;
    }

    if (!(hb.reader_id == reader) || !(hb.writer_id == writer) ||
        !(hb.first_sn == firstSN) || !(hb.last_sn == lastSN) ||
        hb.count != count || hb.final_flag != isFinal ||
        hb.liveliness_flag != liveliness) {
      ++mismatches;
    }
  }

  ctx.param_desc = "seed=" + std::to_string(kSeed) +
                   " iterations=" + std::to_string(kIterations) +
                   " mismatches=" + std::to_string(mismatches);
  STRATUM_CHECK_EQ(mismatches, 0);
}

// ---------------------------------------------------------------------------
// 2) ACKNACK 随机往返（含 SequenceNumberSet 位图）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSubmsg, AckNackRandomRoundTrip) {
  constexpr uint64_t kSeed = 20261111;
  constexpr int kIterations = 20000;

  std::mt19937_64 rng(kSeed);
  SubmessageParser parser;
  int mismatches = 0;
  uint64_t total_bits = 0;

  for (int i = 0; i < kIterations; ++i) {
    CDRMessage_t msg(1024);
    ResetMsg(&msg);

    EntityId_t reader(static_cast<uint32_t>(rng() % 0xFFFF));
    EntityId_t writer(static_cast<uint32_t>(rng() % 0xFFFF));

    // 构造随机 SequenceNumberSet（用 BitmapRange 的公开 API）
    const uint32_t base = static_cast<uint32_t>(1 + rng() % 100000);
    SequenceNumberSet_t snSet(SequenceNumber_t(0, base));
    const uint32_t nbits = static_cast<uint32_t>(rng() % 128);
    std::vector<uint32_t> set_seqs;
    for (uint32_t b = 0; b < nbits; ++b) {
      if (rng() & 1) {
        const uint32_t seq = base + b;
        snSet.add(SequenceNumber_t(0, seq));
        set_seqs.push_back(seq);
        ++total_bits;
      }
    }

    const int32_t count = static_cast<int32_t>(rng());
    const bool finalFlag = (rng() & 1) != 0;

    if (!RTPSMessageCreator::addSubmessageAcknack(&msg, reader, writer, snSet,
                                                  count, finalFlag)) {
      ++mismatches;
      continue;
    }

    msg.pos = 0;
    SubmessageHeader_t smh;
    if (!parser.ReadSubmessageHeader(&msg, &smh)) {
      ++mismatches;
      continue;
    }
    AckNackInfo ack;
    if (!parser.ProcAcknack(&msg, &smh, &ack)) {
      ++mismatches;
      continue;
    }

    if (!(ack.reader_id == reader) || !(ack.writer_id == writer) ||
        ack.count != static_cast<uint32_t>(count) ||
        ack.final_flag != finalFlag) {
      ++mismatches;
      continue;
    }
    // 逐序号比对（BitmapRange 无公开 bitmap 访问）
    if (!(ack.sn_set.base() == snSet.base())) {
      ++mismatches;
      continue;
    }
    bool ok_set = true;
    for (uint32_t seq : set_seqs) {
      if (!ack.sn_set.is_set(SequenceNumber_t(0, seq))) {
        ok_set = false;
        break;
      }
    }
    if (!ok_set) {
      ++mismatches;
    }
  }

  ctx.param_desc = "iterations=" + std::to_string(kIterations) +
                   " set_bits=" + std::to_string(total_bits) +
                   " mismatches=" + std::to_string(mismatches);
  STRATUM_CHECK_EQ(mismatches, 0);
}

// ---------------------------------------------------------------------------
// 3) GAP 随机往返
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSubmsg, GapRandomRoundTrip) {
  constexpr int kIterations = 20000;
  std::mt19937_64 rng(20261112);
  SubmessageParser parser;
  int mismatches = 0;

  for (int i = 0; i < kIterations; ++i) {
    CDRMessage_t msg(512);
    ResetMsg(&msg);

    EntityId_t reader(static_cast<uint32_t>(rng() % 0xFFFF));
    EntityId_t writer(static_cast<uint32_t>(rng() % 0xFFFF));
    SequenceNumber_t gapStart(0, static_cast<uint32_t>(1 + rng() % 100000));
    SequenceNumberSet_t gapList(SequenceNumber_t(0, gapStart.low + 1));

    if (!RTPSMessageCreator::addSubmessageGap(&msg, gapStart, gapList, reader,
                                              writer)) {
      ++mismatches;
      continue;
    }

    msg.pos = 0;
    SubmessageHeader_t smh;
    if (!parser.ReadSubmessageHeader(&msg, &smh)) {
      ++mismatches;
      continue;
    }
    GapInfo gap;
    if (!parser.ProcGap(&msg, &smh, &gap)) {
      ++mismatches;
      continue;
    }
    if (!(gap.reader_id == reader) || !(gap.writer_id == writer) ||
        !(gap.gap_start == gapStart)) {
      ++mismatches;
    }
  }

  ctx.param_desc = "iterations=" + std::to_string(kIterations) +
                   " mismatches=" + std::to_string(mismatches);
  STRATUM_CHECK_EQ(mismatches, 0);
}

// ---------------------------------------------------------------------------
// 4) 压力测试：多子消息混合流的结构解析吞吐
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSubmsg, StressManySubmessages) {
  constexpr int kMessages = 20000;
  constexpr int kSubPerMessage = 8;

  std::mt19937_64 rng(20261113);
  SubmessageParser parser;
  size_t total_submessages = 0;
  size_t malformed = 0;

  for (int m = 0; m < kMessages; ++m) {
    CDRMessage_t msg(4096);
    ResetMsg(&msg);

    for (int s = 0; s < kSubPerMessage; ++s) {
      const int kind = static_cast<int>(rng() % 3);
      EntityId_t reader(static_cast<uint32_t>(rng() % 0xFFFF));
      EntityId_t writer(static_cast<uint32_t>(rng() % 0xFFFF));
      bool ok = false;
      if (kind == 0) {
        SequenceNumber_t f(0, static_cast<uint32_t>(1 + rng() % 100000));
        SequenceNumber_t l(0, f.low + 1);
        ok = RTPSMessageCreator::addSubmessageHeartbeat(
            &msg, reader, writer, f, l, static_cast<uint32_t>(rng()),
            (rng() & 1) != 0, false);
      } else if (kind == 1) {
        SequenceNumberSet_t snSet(
            SequenceNumber_t(0, static_cast<uint32_t>(1 + rng() % 1000)));
        for (uint32_t b = 0; b < (rng() % 32); ++b) {
          if (rng() & 1) {
            snSet.add(SequenceNumber_t(0, snSet.base().low + b));
          }
        }
        ok = RTPSMessageCreator::addSubmessageAcknack(
            &msg, reader, writer, snSet, static_cast<int32_t>(rng()), false);
      } else {
        SequenceNumber_t gs(0, static_cast<uint32_t>(1 + rng() % 100000));
        SequenceNumberSet_t gl(SequenceNumber_t(0, gs.low + 1));
        ok = RTPSMessageCreator::addSubmessageGap(&msg, gs, gl, reader, writer);
      }
      if (!ok) ++malformed;
    }

    // 解析整条消息
    msg.pos = 0;
    std::vector<SubmessageEnvelope> envs;
    ParseStats stats;
    const size_t n = parser.ParseMessage(&msg, &envs, &stats);
    total_submessages += n;
    malformed += stats.malformed;
  }

  ctx.param_desc = "messages=" + std::to_string(kMessages) +
                   " submessages=" + std::to_string(total_submessages) +
                   " malformed=" + std::to_string(malformed);
  STRATUM_CHECK_EQ(malformed, 0);
  STRATUM_CHECK_EQ(total_submessages,
                   static_cast<size_t>(kMessages) * kSubPerMessage);
}

// ---------------------------------------------------------------------------
// 5) 畸形/截断输入必须安全失败（不越界、不 crash）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSubmsg, MalformedInputFailsSafely) {
  SubmessageParser parser;
  int safe = 0;
  int cases = 0;

  // (a) 空消息
  {
    CDRMessage_t msg(16);
    ResetMsg(&msg);
    msg.length = 0;
    SubmessageHeader_t smh;
    ++cases;
    if (!parser.ReadSubmessageHeader(&msg, &smh)) ++safe;
  }
  // (b) 只有 3 字节（头不完整）
  {
    CDRMessage_t msg(16);
    ResetMsg(&msg);
    msg.buffer[0] = 0x07;
    msg.buffer[1] = 0x01;
    msg.buffer[2] = 0x00;
    msg.length = 3;
    SubmessageHeader_t smh;
    ++cases;
    if (!parser.ReadSubmessageHeader(&msg, &smh)) ++safe;
  }
  // (c) 长度字段越界：声称 60000 字节但实际只有 20
  {
    CDRMessage_t msg(64);
    ResetMsg(&msg);
    msg.buffer[0] = 0x07;   // HEARTBEAT
    msg.buffer[1] = 0x01;   // little endian
    msg.buffer[2] = 0x60;   // 0x0060
    msg.buffer[3] = 0xEA;   // 0xEA60 = 60000
    msg.length = 20;
    SubmessageHeader_t smh;
    ++cases;
    if (!parser.ReadSubmessageHeader(&msg, &smh)) ++safe;
  }
  // (d) 随机字节流（大量）
  {
    std::mt19937_64 rng(20261114);
    for (int i = 0; i < 5000; ++i) {
      const size_t len = 4 + (rng() % 200);
      CDRMessage_t msg(static_cast<uint32_t>(len + 8));
      ResetMsg(&msg);
      for (size_t k = 0; k < len; ++k) {
        msg.buffer[k] = static_cast<octet>(rng());
      }
      msg.length = static_cast<uint32_t>(len);
      ++cases;
      // 只要不崩溃即算安全（返回值不保证）
      SubmessageHeader_t smh;
      if (!parser.ReadSubmessageHeader(&msg, &smh)) {
        ++safe;
      } else {
        std::vector<SubmessageEnvelope> envs;
        ParseStats stats;
        parser.ParseMessage(&msg, &envs, &stats);
        ++safe;   // 未崩溃
      }
    }
  }

  ctx.param_desc = "cases=" + std::to_string(cases) +
                   " safely_handled=" + std::to_string(safe);
  STRATUM_CHECK_EQ(safe, cases);
}

// ---------------------------------------------------------------------------
// 6) 真实 Fast DDS 流量（tests/vectors/spdp_capture.bin）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSubmsg, ParseRealFastDdsCapture) {
  const char* path = "tests/vectors/spdp_capture.bin";
  std::ifstream in(path, std::ios::binary);
  // 【不可静默跳过】向量缺失即真实流量验证失效，必须失败。
  if (!in) {
    ctx.param_desc = std::string("**向量文件缺失**: ") + path;
    STRATUM_CHECK_TRUE(false);
    return;
  }

  uint32_t count = 0;
  in.read(reinterpret_cast<char*>(&count), sizeof(count));
  if (count == 0 || count > 10000) {
    STRATUM_CHECK_TRUE(false);
    return;
  }

  SubmessageParser parser;
  size_t packets_ok = 0;
  size_t total_submessages = 0;
  size_t malformed = 0;
  size_t data_submessages = 0;
  int guid_checked = 0;

  for (uint32_t i = 0; i < count; ++i) {
    uint32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (len == 0 || len > 65535) break;
    std::vector<uint8_t> buf(len);
    in.read(reinterpret_cast<char*>(buf.data()), len);

    CDRMessage_t msg(len + 8);
    std::memcpy(msg.buffer, buf.data(), len);
    msg.length = len;
    msg.pos = 0;

    RtpsHeaderInfo hdr;
    if (!parser.CheckRtpsHeader(&msg, &hdr)) {
      ++malformed;
      continue;
    }
    ++packets_ok;
    if (guid_checked == 0) {
      // 真实包：glibc 验证魔数与 vendor 已由 CheckRtpsHeader 完成
      guid_checked = 1;
      ctx.param_desc = "首个包 version=" +
                       std::to_string(hdr.source_version.m_major) + "." +
                       std::to_string(hdr.source_version.m_minor) +
                       " vendor=" +
                       std::to_string(static_cast<int>(hdr.source_vendor_id[0])) +
                       std::to_string(static_cast<int>(hdr.source_vendor_id[1]));
    }

    std::vector<SubmessageEnvelope> envs;
    ParseStats stats;
    parser.ParseMessage(&msg, &envs, &stats);
    total_submessages += stats.total;
    malformed += stats.malformed;
    data_submessages += stats.data;
  }

  ctx.param_desc += " packets=" + std::to_string(count) +
                    " ok=" + std::to_string(packets_ok) +
                    " submsg=" + std::to_string(total_submessages) +
                    " data=" + std::to_string(data_submessages) +
                    " malformed=" + std::to_string(malformed);

  STRATUM_CHECK_EQ(packets_ok, static_cast<size_t>(count));
  STRATUM_CHECK_TRUE(data_submessages > 0);   // SPDP 公告必含 DATA
  STRATUM_CHECK_EQ(malformed, 0);
}

STRATUM_TEST_MAIN()
