// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// ACKNACK 位图判定单测（遗留问题 O10）
//
// 背景：这段判定原先内嵌在 RtpsWriter::HandleAcknack 的 lambda 里，
// 只能靠端到端现象间接观察 —— 本项目曾因此误判（看到一个计数器为 0
// 就以为"位图路径从未走过"，实际上只是统计口径不对）。
// 现在判定被抽成纯函数 AckClassifySn，可以逐位验证。
//
// 三条互斥路径：
//   ① BaseAcked     sn < base_sn          → reader 已确认收到
//   ② BitmapHit     位图覆盖该 SN          → 按位判定
//   ③ BeyondBitmap  位图未覆盖 / 字节不足   → 保守视为缺失
//
// 特别要盯住**位序**：RTPS 位图是大端位序，第 i 位取 byte[i/8] 的 bit (7 - i%8)。
// 写反了会让"缺失"和"已收到"整体颠倒，而端到端看起来"只是重传变多"，极难发现。

#include <cstdint>
#include <vector>

#include "stratum/protocol/rtps/rtps_writer.h"
#include "test_framework.h"

using stratum::protocol::rtps::AckClassifySn;
using stratum::protocol::rtps::AckPath;

// ---------------------------------------------------------------------------
// ① base 路径
// ---------------------------------------------------------------------------
STRATUM_TEST(AckBitmap, BaseAckedPath) {
  const uint8_t bm[1] = {0xFF};
  bool miss = true;

  // sn < base_sn → 已确认收到，missing 出参必须被清零
  STRATUM_CHECK_TRUE(AckClassifySn(9, 10, 8, bm, sizeof(bm), &miss) ==
                     AckPath::BaseAcked);
  STRATUM_CHECK_TRUE(!miss);

  // sn == base_sn 时不走该路径（边界）
  STRATUM_CHECK_TRUE(AckClassifySn(10, 10, 8, bm, sizeof(bm), &miss) ==
                     AckPath::BitmapHit);
}

// ---------------------------------------------------------------------------
// ② 位图命中：重点验证位序
// ---------------------------------------------------------------------------
STRATUM_TEST(AckBitmap, BitmapBitOrderIsBigEndian) {
  // base_sn = 100，位图首字节 = 0b1000_0001
  //   第 0 位（SN=100）→ bit7 = 1 → 缺失
  //   第 7 位（SN=107）→ bit0 = 1 → 缺失
  //   第 1..6 位     → 0      → 已收到
  const uint8_t bm[1] = {0x81};
  bool miss = false;

  STRATUM_CHECK_TRUE(AckClassifySn(100, 100, 8, bm, sizeof(bm), &miss) ==
                     AckPath::BitmapHit);
  STRATUM_CHECK_TRUE(miss);          // bit7 == 1

  STRATUM_CHECK_TRUE(AckClassifySn(101, 100, 8, bm, sizeof(bm), &miss) ==
                     AckPath::BitmapHit);
  STRATUM_CHECK_TRUE(!miss);         // bit6 == 0

  STRATUM_CHECK_TRUE(AckClassifySn(106, 100, 8, bm, sizeof(bm), &miss) ==
                     AckPath::BitmapHit);
  STRATUM_CHECK_TRUE(!miss);         // bit1 == 0

  STRATUM_CHECK_TRUE(AckClassifySn(107, 100, 8, bm, sizeof(bm), &miss) ==
                     AckPath::BitmapHit);
  STRATUM_CHECK_TRUE(miss);          // bit0 == 1
}

STRATUM_TEST(AckBitmap, BitmapSpansMultipleBytes) {
  // 两字节：第一字节全 0，第二字节 0x01（即第 15 位为 1）
  const uint8_t bm[2] = {0x00, 0x01};
  bool miss = true;

  // 第 0..7 位都在第一字节，全 0 → 不缺失
  for (uint32_t i = 0; i < 8; ++i) {
    STRATUM_CHECK_TRUE(AckClassifySn(200 + i, 200, 16, bm, sizeof(bm), &miss) ==
                       AckPath::BitmapHit);
    STRATUM_CHECK_TRUE(!miss);
  }
  // 第 8..14 位在第二字节高位，仍为 0
  for (uint32_t i = 8; i < 15; ++i) {
    STRATUM_CHECK_TRUE(AckClassifySn(200 + i, 200, 16, bm, sizeof(bm), &miss) ==
                       AckPath::BitmapHit);
    STRATUM_CHECK_TRUE(!miss);
  }
  // 第 15 位 → byte[1] 的 bit0 = 1 → 缺失
  STRATUM_CHECK_TRUE(AckClassifySn(215, 200, 16, bm, sizeof(bm), &miss) ==
                     AckPath::BitmapHit);
  STRATUM_CHECK_TRUE(miss);
}

// ---------------------------------------------------------------------------
// ③ 位图外 / 字节不足 / 空指针
// ---------------------------------------------------------------------------
STRATUM_TEST(AckBitmap, BeyondBitmapCases) {
  const uint8_t bm[1] = {0x00};
  bool miss = false;

  // idx >= num_bits → 位图未覆盖
  STRATUM_CHECK_TRUE(AckClassifySn(108, 100, 8, bm, sizeof(bm), &miss) ==
                     AckPath::BeyondBitmap);

  // num_bits = 0：任何 SN 都在位图外
  STRATUM_CHECK_TRUE(AckClassifySn(100, 100, 0, bm, sizeof(bm), &miss) ==
                     AckPath::BeyondBitmap);

  // num_bits 声称有 16 位，但只给了 1 字节。
  // idx=8 起落在第二字节 —— 字节不足时必须**保守判为位图外**（即视为缺失），
  // 绝不能因为"位图声明了 16 位"就去读越界内存。
  // （这条最初被我写成期望 BitmapHit，是测试期望错了、实现是对的。）
  STRATUM_CHECK_TRUE(AckClassifySn(107, 100, 16, bm, 1, &miss) ==
                     AckPath::BitmapHit);    // 第 7 位仍在第一字节内
  STRATUM_CHECK_TRUE(AckClassifySn(108, 100, 16, bm, 1, &miss) ==
                     AckPath::BeyondBitmap); // 第 8 位需读第二字节 → 字节不足
  STRATUM_CHECK_TRUE(AckClassifySn(116, 100, 16, bm, 1, &miss) ==
                     AckPath::BeyondBitmap);

  // bitmap == nullptr 且需要读位 → 保守判为位图外
  STRATUM_CHECK_TRUE(AckClassifySn(100, 100, 8, nullptr, 0, &miss) ==
                     AckPath::BeyondBitmap);
}

// ---------------------------------------------------------------------------
// 边界：base_sn = 0 与 missing 出参可为空
// ---------------------------------------------------------------------------
STRATUM_TEST(AckBitmap, EdgeCases) {
  const uint8_t bm[1] = {0x80};
  bool miss = false;

  // base_sn = 0 时第 0 位应可判定
  STRATUM_CHECK_TRUE(AckClassifySn(0, 0, 8, bm, sizeof(bm), &miss) ==
                     AckPath::BitmapHit);
  STRATUM_CHECK_TRUE(miss);

  // missing 传 nullptr 时不得崩溃
  STRATUM_CHECK_TRUE(AckClassifySn(0, 0, 8, bm, sizeof(bm), nullptr) ==
                     AckPath::BitmapHit);
  STRATUM_CHECK_TRUE(AckClassifySn(0, 1, 8, bm, sizeof(bm), nullptr) ==
                     AckPath::BaseAcked);
}

STRATUM_TEST_MAIN()
