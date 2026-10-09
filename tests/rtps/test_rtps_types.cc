// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// M1 验证：移植的 RTPS 基础类型字节级正确性
//
// 这些类型是 RTPS 线格式的基础。移植正确性必须用**固定字节向量**验证，
// 而不是只看编译通过。重点验证大端（网络序）语义 —— EntityId_t 的
// 构造函数会做字节序反转，这是线格式兼容的关键细节。

#include <cstdint>
#include <cstring>
#include <string>

#include <stratum/rtps/common/EntityId_t.hpp>
#include <stratum/rtps/common/Guid.hpp>
#include <stratum/rtps/common/GuidPrefix_t.hpp>
#include <stratum/rtps/common/Locator.hpp>
#include <stratum/rtps/common/SequenceNumber.hpp>
#include <stratum/rtps/common/Time_t.hpp>
#include <stratum/rtps/common/Types.hpp>
#include <stratum/rtps/common/VendorId_t.hpp>

#include "test_framework.h"

using namespace eprosima::fastdds::rtps;

// ---------------------------------------------------------------------------
// GuidPrefix_t：12 字节（RTPS 规范）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsPort, GuidPrefixIsTwelveBytes) {
  ctx.param_desc = "size=" + std::to_string(GuidPrefix_t::size);
  STRATUM_CHECK_EQ(GuidPrefix_t::size, 12u);

  GuidPrefix_t prefix;
  // 默认构造应为全 0
  bool all_zero = true;
  for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
    if (prefix.value[i] != 0) all_zero = false;
  }
  STRATUM_CHECK_TRUE(all_zero);

  // 逐字节赋值后读回
  for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
    prefix.value[i] = static_cast<octet>(0xA0 + i);
  }
  bool ok = true;
  for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
    if (prefix.value[i] != static_cast<octet>(0xA0 + i)) ok = false;
  }
  STRATUM_CHECK_TRUE(ok);
}

// ---------------------------------------------------------------------------
// EntityId_t：4 字节，**大端线格式**（构造时自动 reverse）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsPort, EntityIdIsBigEndianOnWire) {
  constexpr unsigned kSize = EntityId_t::size;
  STRATUM_CHECK_EQ(kSize, 4u);

  // 参与者内建端点 0x000001c1 → 线格式（大端）应为 00 00 01 c1
  EntityId_t participant(ENTITYID_RTPSParticipant);
  ctx.param_desc = "participant bytes=" +
                   std::to_string(static_cast<int>(participant.value[0])) + " " +
                   std::to_string(static_cast<int>(participant.value[1])) + " " +
                   std::to_string(static_cast<int>(participant.value[2])) + " " +
                   std::to_string(static_cast<int>(participant.value[3]));
  STRATUM_CHECK_EQ(static_cast<int>(participant.value[0]), 0x00);
  STRATUM_CHECK_EQ(static_cast<int>(participant.value[1]), 0x00);
  STRATUM_CHECK_EQ(static_cast<int>(participant.value[2]), 0x01);
  STRATUM_CHECK_EQ(static_cast<int>(participant.value[3]), 0xc1);

  // SEDP 内建发布 Writer 0x000003c2 → 00 00 03 c2
  EntityId_t sedp_w(ENTITYID_SEDP_BUILTIN_PUBLICATIONS_WRITER);
  STRATUM_CHECK_EQ(static_cast<int>(sedp_w.value[2]), 0x03);
  STRATUM_CHECK_EQ(static_cast<int>(sedp_w.value[3]), 0xc2);

  // 未知实体全 0
  EntityId_t unknown(ENTITYID_UNKNOWN);
  bool zero = true;
  for (unsigned i = 0; i < kSize; ++i) {
    if (unknown.value[i] != 0) zero = false;
  }
  STRATUM_CHECK_TRUE(zero);

  // 读/写端点判定（低字节 0x02=writer with key / 0x03 / 0x07=reader）
  STRATUM_CHECK_TRUE(sedp_w.is_writer());
  EntityId_t sedp_r(ENTITYID_SEDP_BUILTIN_PUBLICATIONS_READER);
  STRATUM_CHECK_TRUE(sedp_r.is_reader());
}

// ---------------------------------------------------------------------------
// GUID_t = GuidPrefix(12) + EntityId(4) = 16 字节
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsPort, GuidIsSixteenBytes) {
  GuidPrefix_t prefix;
  for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
    prefix.value[i] = static_cast<octet>(i + 1);
  }
  EntityId_t entity(ENTITYID_RTPSParticipant);
  GUID_t guid(prefix, entity);

  ctx.param_desc = "prefix[0]=" +
                   std::to_string(static_cast<int>(guid.guidPrefix.value[0])) +
                   " entity[3]=" +
                   std::to_string(static_cast<int>(guid.entityId.value[3]));
  STRATUM_CHECK_EQ(guid.guidPrefix.value[0], static_cast<octet>(1));
  STRATUM_CHECK_EQ(guid.entityId.value[3], static_cast<octet>(0xc1));

  // GUID 相等性
  GUID_t same(prefix, entity);
  STRATUM_CHECK_TRUE(guid == same);

  // 修改 prefix 后应不等
  GuidPrefix_t other = prefix;
  other.value[0] = 0xFF;
  GUID_t diff(other, entity);
  STRATUM_CHECK_TRUE(guid != diff);
}

// ---------------------------------------------------------------------------
// SequenceNumber_t：高 32 位优先，递增语义
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsPort, SequenceNumberOrdering) {
  SequenceNumber_t a(1, 100);
  SequenceNumber_t b(1, 200);
  SequenceNumber_t c(2, 0);

  ctx.param_desc = "a=" + std::to_string(a.to64long()) +
                   " b=" + std::to_string(b.to64long()) +
                   " c=" + std::to_string(c.to64long());

  STRATUM_CHECK_TRUE(a < b);
  STRATUM_CHECK_TRUE(b < c);  // 高位优先
  STRATUM_CHECK_TRUE(a <= a);
  STRATUM_CHECK_TRUE(c > b);
  STRATUM_CHECK_TRUE(a != b);

  // 自增
  ++a;
  STRATUM_CHECK_EQ(a.low, static_cast<uint32_t>(101));
}

// ---------------------------------------------------------------------------
// Time_t：秒 + 纳秒（RTPS 时间戳）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsPort, TimeValueAccessors) {
  // 【重要语义】RTPS 的时间戳线格式是 (int32 seconds, uint32 fraction)，
  // 其中 fraction 的单位是 1/2^32 秒 —— **不是纳秒**。
  // 本测试最初把第二参数当纳秒写，断言失败（5678 → nanosec 1322），
  // 恰好证明了换算关系正确：5678 * 1e9 / 2^32 ≈ 1322。
  Time_t t(1234, 5678);
  ctx.param_desc = "sec=" + std::to_string(t.seconds()) +
                   " frac=" + std::to_string(t.fraction()) +
                   " nsec=" + std::to_string(t.nanosec());
  STRATUM_CHECK_EQ(t.seconds(), 1234);
  STRATUM_CHECK_EQ(t.fraction(), 5678u);

  // fraction → nanosec 的换算（RTPS 规范：nsec = frac * 1e9 / 2^32）
  const uint64_t expected_ns =
      (static_cast<uint64_t>(5678) * 1000000000ULL) >> 32;
  STRATUM_CHECK_EQ(t.nanosec(), static_cast<uint32_t>(expected_ns));
  ctx.param_desc += " 换算期望=" + std::to_string(expected_ns);

  // 默认构造为零
  Time_t zero;
  STRATUM_CHECK_EQ(zero.seconds(), 0);
  STRATUM_CHECK_EQ(zero.fraction(), 0u);
  STRATUM_CHECK_EQ(zero.nanosec(), 0u);
}

// ---------------------------------------------------------------------------
// Locator_t：端点定位（kind + port + 16 字节地址）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsPort, LocatorFields) {
  Locator_t loc;
  loc.kind = LOCATOR_KIND_UDPv4;
  loc.port = 7400;
  loc.address[12] = 192;
  loc.address[13] = 168;
  loc.address[14] = 1;
  loc.address[15] = 100;

  ctx.param_desc = "kind=" + std::to_string(loc.kind) +
                   " port=" + std::to_string(loc.port);
  STRATUM_CHECK_EQ(loc.kind, static_cast<int32_t>(LOCATOR_KIND_UDPv4));
  STRATUM_CHECK_EQ(loc.port, static_cast<uint32_t>(7400));
  STRATUM_CHECK_EQ(static_cast<int>(loc.address[14]), 1);
  STRATUM_CHECK_EQ(static_cast<int>(loc.address[15]), 100);
  STRATUM_CHECK_EQ(static_cast<int>(loc.address[0]), 0);
}

// ---------------------------------------------------------------------------
// VendorId_t：厂商标识（决定对端行为差异）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsPort, VendorIdIsEprosima) {
  ctx.param_desc = "vendor=" +
                   std::to_string(static_cast<int>(c_VendorId_eProsima[0])) + "," +
                   std::to_string(static_cast<int>(c_VendorId_eProsima[1]));
  // eProsima 的 RTPS 分配的 vendor id
  STRATUM_CHECK_EQ(static_cast<int>(c_VendorId_eProsima[0]), 0x01);
  STRATUM_CHECK_EQ(static_cast<int>(c_VendorId_eProsima[1]), 0x0f);
}

STRATUM_TEST_MAIN()
