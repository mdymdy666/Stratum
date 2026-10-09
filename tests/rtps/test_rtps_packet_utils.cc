// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// RTPS 报文构造的字节级验证
//
// ===========================================================================
// 为什么这组测试价值高
// ===========================================================================
//
// 回放链路此前卡在"发布出去但 Fast DDS 收不到"。可能的原因分散在
// 报文构造的多个细节上（偏移、长度、QoS 参数、字节序）。
// 依赖真实 DDS 环境做端到端验证既慢又不稳定（同脚本多次运行，
// 录制条数在 1~199 间波动），无法定位到具体字节。
//
// 本组测试**不依赖任何网络**，直接驱动纯函数并逐字节校验，
// 再**用已移植的 SedpParser 反向解析自己构造的公告** ——
// 构造与解析互为镜像，任一侧偏离线上格式都会暴露。

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <stratum/protocol/rtps/rtps_packet_utils.h>
#include <stratum/rtps/builtin/sedp_parser.h>
#include <stratum/rtps/messages/submessage_parser.h>

#include "test_framework.h"

using namespace stratum::protocol::rtps;
using namespace stratum::rtps;
using namespace stratum_test;

namespace {

const uint8_t kPrefix[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                             0x54, 0x55, 0x4d, 0x00, 0x00, 0x02};

packet::EndpointAnnouncement MakeAnnouncement() {
  packet::EndpointAnnouncement a;
  std::memcpy(a.prefix, kPrefix, 12);
  a.writer_entity = packet::kEntitySedpPubWriter;
  a.endpoint_entity = packet::kEntityUserWriter;
  a.topic_name = "hello_world_topic";
  a.type_name = "HelloWorld";
  a.unicast_port = 9061;
  a.reliability = packet::kReliabilityReliable;
  a.durability = packet::kDurabilityVolatile;
  a.sn = 1;
  return a;
}

// 小端读 16 位
uint16_t Rd16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t Rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1) DATA 报文的字节级结构
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsPacket, DataPacketLayout) {
  packet::DataPacketParams p;
  std::memcpy(p.prefix, kPrefix, 12);
  p.writer_entity = packet::kEntityUserWriter;
  p.sn = 0x0000000700000000ULL + 3;   // 高 32 位非零，验证 SN 编解码
  p.payload = "ABCDEFGH";             // 8 字节，含 encapsulation 位置

  const auto buf = packet::BuildDataPacket(p);
  ctx.param_desc = "size=" + std::to_string(buf.size());

  STRATUM_CHECK_TRUE(buf.size() > 20);
  // RTPS 头
  STRATUM_CHECK_TRUE(std::memcmp(buf.data(), "RTPS", 4) == 0);
  STRATUM_CHECK_EQ(static_cast<int>(buf[4]), 2);    // major
  STRATUM_CHECK_EQ(static_cast<int>(buf[5]), 4);    // minor
  STRATUM_CHECK_EQ(static_cast<int>(buf[6]), 0x01); // vendorId[0]
  STRATUM_CHECK_EQ(static_cast<int>(buf[7]), 0x0f); // vendorId[1]
  STRATUM_CHECK_TRUE(std::memcmp(buf.data() + 8, kPrefix, 12) == 0);

  // 第一个子消息应为 INFO_TS（id=0x09，长度 8）
  size_t off = 20;
  STRATUM_CHECK_EQ(static_cast<int>(buf[off]), packet::kSubInfoTs);
  STRATUM_CHECK_EQ(static_cast<int>(Rd16(buf.data() + off + 2)), 8);
  off += 4 + 8;

  // 第二个子消息应为 DATA
  STRATUM_CHECK_EQ(static_cast<int>(buf[off]), packet::kSubData);
  STRATUM_CHECK_EQ(static_cast<int>(buf[off + 1]), 0x05);   // E=1, D=1
  const uint16_t body_len = Rd16(buf.data() + off + 2);
  const size_t body = off + 4;

  // 体前缀：extraFlags(2) + octetsToInlineQos(2) + readerId(4) + writerId(4) + SN(8)
  STRATUM_CHECK_EQ(static_cast<int>(Rd16(buf.data() + body)), 0);       // extraFlags
  STRATUM_CHECK_EQ(static_cast<int>(Rd16(buf.data() + body + 2)), 16);  // octetsToInlineQos
  // readerId = unknown (00 00 00 00)
  STRATUM_CHECK_EQ(static_cast<int>(buf[body + 4]), 0);
  // writerId 的 4 字节（大端序存储）
  const uint32_t wid = packet::EntityVal(buf.data() + body + 8);
  STRATUM_CHECK_EQ(static_cast<int>(wid), static_cast<int>(packet::kEntityUserWriter));
  // writerSN 低 32 位
  STRATUM_CHECK_EQ(static_cast<int>(Rd32(buf.data() + body + 16)), 3);

  // 负载应原样出现在 SN 之后
  const size_t payload_off = body + 20;
  STRATUM_CHECK_TRUE(payload_off + p.payload.size() <= buf.size());
  STRATUM_CHECK_TRUE(std::memcmp(buf.data() + payload_off, p.payload.data(),
                                 p.payload.size()) == 0);
  // octetsToNextHeader 必须等于体长度（这里就是负载长度）
  STRATUM_CHECK_EQ(static_cast<int>(body_len),
                   static_cast<int>(20 + p.payload.size()));
  // 且报文总长必须正好覆盖该子消息（回填不得污染 length）
  STRATUM_CHECK_EQ(static_cast<int>(buf.size()),
                   static_cast<int>(body + body_len));
}

// ---------------------------------------------------------------------------
// 2) SEDP 公告的字节级结构
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsPacket, AnnouncementLayout) {
  const auto a = MakeAnnouncement();
  const auto buf = packet::BuildSedpAnnouncement(a);
  ctx.param_desc = "size=" + std::to_string(buf.size());

  STRATUM_CHECK_TRUE(buf.size() > 20);
  STRATUM_CHECK_TRUE(std::memcmp(buf.data(), "RTPS", 4) == 0);

  size_t off = 20;
  // INFO_TS
  STRATUM_CHECK_EQ(static_cast<int>(buf[off]), packet::kSubInfoTs);
  off += 4 + 8;
  // DATA
  STRATUM_CHECK_EQ(static_cast<int>(buf[off]), packet::kSubData);
  const uint16_t body_len = Rd16(buf.data() + off + 2);
  const size_t body = off + 4;
  STRATUM_CHECK_EQ(static_cast<int>(Rd16(buf.data() + body + 2)), 16);

  // writerId 应为 SEDP publications writer
  const uint32_t wid = packet::EntityVal(buf.data() + body + 8);
  STRATUM_CHECK_EQ(static_cast<int>(wid),
                   static_cast<int>(packet::kEntitySedpPubWriter));

  // encapsulation 应为 PL_CDR_LE（00 03）
  const size_t enc = body + 20;
  STRATUM_CHECK_EQ(static_cast<int>(buf[enc]), 0x00);
  STRATUM_CHECK_EQ(static_cast<int>(buf[enc + 1]), 0x03);

  // 参数表：必须包含全部关键 PID，且以 SENTINEL 结尾
  bool has_endpoint_guid = false, has_participant_guid = false;
  bool has_topic = false, has_type = false, has_locator = false;
  bool has_reliability = false, has_durability = false;
  bool has_protocol = false, has_vendor = false;
  bool sentinel_last = false;

  size_t p = enc + 4;
  size_t guard = 0;
  while (p + 4 <= buf.size() && guard++ < 64) {
    const uint16_t pid = Rd16(buf.data() + p);
    const uint16_t plen = Rd16(buf.data() + p + 2);
    if (pid == packet::kPidEndpointGuid) has_endpoint_guid = true;
    else if (pid == packet::kPidParticipantGuid) has_participant_guid = true;
    else if (pid == packet::kPidTopicName) has_topic = true;
    else if (pid == packet::kPidTypeName) has_type = true;
    else if (pid == packet::kPidUnicastLocator) has_locator = true;
    else if (pid == packet::kPidReliability) {
      has_reliability = true;
      // 值应为 RELIABLE(2)
      STRATUM_CHECK_EQ(static_cast<int>(Rd32(buf.data() + p + 4)),
                       static_cast<int>(packet::kReliabilityReliable));
    } else if (pid == packet::kPidDurability) has_durability = true;
    else if (pid == packet::kPidProtocolVersion) {
      has_protocol = true;
      // 长度必须是 4（实测值；写成 2 会被 Fast DDS 静默跳过）
      STRATUM_CHECK_EQ(static_cast<int>(plen), 4);
    } else if (pid == packet::kPidVendorId) {
      has_vendor = true;
      STRATUM_CHECK_EQ(static_cast<int>(plen), 4);
    } else if (pid == packet::kPidSentinel) {
      sentinel_last = true;
      break;
    }
    p += 4 + ((plen + 3) & ~3u);
  }

  STRATUM_CHECK_TRUE(has_endpoint_guid);
  STRATUM_CHECK_TRUE(has_participant_guid);
  STRATUM_CHECK_TRUE(has_topic);
  STRATUM_CHECK_TRUE(has_type);
  STRATUM_CHECK_TRUE(has_locator);
  STRATUM_CHECK_TRUE(has_reliability);
  STRATUM_CHECK_TRUE(has_durability);
  STRATUM_CHECK_TRUE(has_protocol);
  STRATUM_CHECK_TRUE(has_vendor);
  STRATUM_CHECK_TRUE(sentinel_last);
}

// ---------------------------------------------------------------------------
// 3) 反向解析：用 SedpParser 解析自己构造的公告
//
// 构造与解析互为镜像。若构造端在偏移/长度/字节序上有偏差，
// 这里解析出的 topic/type/QoS 就会错 —— 这是最强的自验证。
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsPacket, AnnouncementRoundTrip) {
  const auto a = MakeAnnouncement();
  const auto buf = packet::BuildSedpAnnouncement(a);

  SedpParser parser;
  std::vector<SedpEndpointData> out;
  const size_t n = parser.ParseMessage(buf.data(), buf.size(), &out);

  ctx.param_desc = "endpoints=" + std::to_string(n);
  STRATUM_CHECK_EQ(n, static_cast<size_t>(1));
  if (out.empty()) return;

  const SedpEndpointData& e = out[0];
  STRATUM_CHECK_TRUE(e.has_topic_name);
  STRATUM_CHECK_EQ(e.topic_name, std::string("hello_world_topic"));
  STRATUM_CHECK_TRUE(e.has_type_name);
  STRATUM_CHECK_EQ(e.type_name, std::string("HelloWorld"));
  STRATUM_CHECK_TRUE(e.has_endpoint_guid);
  STRATUM_CHECK_TRUE(std::memcmp(e.endpoint_guid.guidPrefix.value, kPrefix, 12) == 0);
  STRATUM_CHECK_TRUE(e.has_participant_guid);
  STRATUM_CHECK_TRUE(e.has_reliability);
  STRATUM_CHECK_EQ(static_cast<int>(e.reliability_kind),
                   static_cast<int>(packet::kReliabilityReliable));
  STRATUM_CHECK_TRUE(e.has_durability);
  STRATUM_CHECK_EQ(static_cast<int>(e.durability_kind),
                   static_cast<int>(packet::kDurabilityVolatile));
  STRATUM_CHECK_TRUE(!e.unicast_locators.empty());
  STRATUM_CHECK_EQ(static_cast<int>(e.unicast_locators[0].port), 9061);
}

// ---------------------------------------------------------------------------
// 4) QoS 声明开关的影响
//
// 验证"不声明 QoS"与"声明 QoS"的差异，锁住这个曾经导致
// 对端收到 0 条数据的回归点。
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsPacket, QosDeclarationMatters) {
  auto a = MakeAnnouncement();

  a.include_qos = false;
  const auto without = packet::BuildSedpAnnouncement(a);
  SedpParser parser;
  std::vector<SedpEndpointData> out1;
  parser.ParseMessage(without.data(), without.size(), &out1);
  STRATUM_CHECK_EQ(out1.size(), static_cast<size_t>(1));
  if (!out1.empty()) {
    // 未声明 → 解析端看不到 RELIABILITY
    STRATUM_CHECK_TRUE(!out1[0].has_reliability);
  }

  a.include_qos = true;
  const auto with = packet::BuildSedpAnnouncement(a);
  std::vector<SedpEndpointData> out2;
  parser.ParseMessage(with.data(), with.size(), &out2);
  STRATUM_CHECK_EQ(out2.size(), static_cast<size_t>(1));
  if (!out2.empty()) {
    STRATUM_CHECK_TRUE(out2[0].has_reliability);
    STRATUM_CHECK_EQ(static_cast<int>(out2[0].reliability_kind), 2);
  }

  ctx.param_desc = "without_qos=" + std::to_string(without.size()) +
                   " with_qos=" + std::to_string(with.size());
  // 声明 QoS 后报文必然更长
  STRATUM_CHECK_TRUE(with.size() > without.size());
}

// ---------------------------------------------------------------------------
// 5) 边界与健壮性
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsPacket, DegenerateInputs) {
  int cases = 0, ok = 0;

  {
    auto a = MakeAnnouncement();
    a.topic_name.clear();
    ++cases;
    if (packet::BuildSedpAnnouncement(a).empty()) ++ok;   // 空话题必须失败
  }
  {
    auto a = MakeAnnouncement();
    a.type_name.clear();
    ++cases;
    if (packet::BuildSedpAnnouncement(a).empty()) ++ok;
  }
  {
    // 超长话题名（含 4 字节对齐）
    auto a = MakeAnnouncement();
    a.topic_name = std::string(255, 'x');
    ++cases;
    const auto b = packet::BuildSedpAnnouncement(a);
    SedpParser p;
    std::vector<SedpEndpointData> out;
    p.ParseMessage(b.data(), b.size(), &out);
    if (out.size() == 1 && out[0].topic_name == a.topic_name) ++ok;
  }
  {
    // 空负载的 DATA 报文
    packet::DataPacketParams d;
    std::memcpy(d.prefix, kPrefix, 12);
    d.sn = 1;
    ++cases;
    const auto b = packet::BuildDataPacket(d);
    if (!b.empty() && std::memcmp(b.data(), "RTPS", 4) == 0) ++ok;
  }
  {
    // 大负载（64 KiB 量级）
    packet::DataPacketParams d;
    std::memcpy(d.prefix, kPrefix, 12);
    d.sn = 42;
    d.payload = std::string(8192, 'Z');
    ++cases;
    const auto b = packet::BuildDataPacket(d);
    if (b.size() > 8192) ++ok;
  }

  ctx.param_desc = "cases=" + std::to_string(cases) + " ok=" + std::to_string(ok);
  STRATUM_CHECK_EQ(ok, cases);
}

// ---------------------------------------------------------------------------
// 6) 压力：大量构造 + 反向解析
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsPacket, BuildParseStress) {
  constexpr int kIterations = 5000;
  SedpParser parser;
  size_t parsed_ok = 0;

  for (int i = 0; i < kIterations; ++i) {
    auto a = MakeAnnouncement();
    a.topic_name = "topic_" + std::to_string(i % 97);
    a.type_name = "Type_" + std::to_string(i % 13);
    a.sn = static_cast<uint64_t>(i) + 1;
    const auto buf = packet::BuildSedpAnnouncement(a);
    std::vector<SedpEndpointData> out;
    if (parser.ParseMessage(buf.data(), buf.size(), &out) == 1 &&
        !out.empty() && out[0].topic_name == a.topic_name &&
        out[0].type_name == a.type_name) {
      ++parsed_ok;
    }

    packet::DataPacketParams d;
    std::memcpy(d.prefix, kPrefix, 12);
    d.sn = static_cast<uint64_t>(i) + 1;
    d.payload = std::string(64 + (i % 256), 'Q');
    const auto dp = packet::BuildDataPacket(d);
    if (dp.size() < 20) --parsed_ok;   // 构造失败应被发现
  }

  ctx.param_desc = "iterations=" + std::to_string(kIterations) +
                   " round_trip_ok=" + std::to_string(parsed_ok);
  STRATUM_CHECK_EQ(parsed_ok, static_cast<size_t>(kIterations));
}

STRATUM_TEST_MAIN()
