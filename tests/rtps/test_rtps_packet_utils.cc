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
#include <cstdio>
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

// ---------------------------------------------------------------------------
// 批量 DATA（一个 Message 装多个 DATA 子消息）
//
// 这组测试的**核心目的**不是"批量能跑"，而是证明三件事：
//   ① N=1 时与单发逐字节相同 → 批量没有引入第二个实现
//   ② 子消息序列正确（数量、id、每条的 INFO_TS 与 writerId/SN）
//   ③ 两道边界真的挡住了畸形报文（不是"构造了但发出去坏了"）
// ---------------------------------------------------------------------------

namespace {

// 扫描一个 RTPS Message 里的子消息序列，按 octetsToNextHeader 跳。
// 返回 (id, 体长度) 列表；遇到长度越界立即停止（说明报文本身坏了）。
struct SubMsg {
  uint8_t id;
  uint16_t body_len;
  size_t off;   // 子消息头在报文中的偏移
};

std::vector<SubMsg> ScanSubmessages(const std::vector<uint8_t>& buf) {
  std::vector<SubMsg> out;
  size_t off = 20;   // 跳过 RTPS Message Header
  while (off + 4 <= buf.size()) {
    const uint8_t id = buf[off];
    const uint16_t len = Rd16(buf.data() + off + 2);
    out.push_back({id, len, off});
    const size_t next = off + 4 + len;
    if (next <= off || next > buf.size()) break;   // 防越界/死循环
    off = next;
  }
  return out;
}

size_t CountId(const std::vector<SubMsg>& v, uint8_t id) {
  size_t n = 0;
  for (const auto& s : v) if (s.id == id) ++n;
  return n;
}

// 从 DATA 子消息体里读 writerId（大端存储的 4 字节，用 EntityVal 解）
uint32_t WriterIdOf(const std::vector<uint8_t>& buf, const SubMsg& s) {
  const size_t body = s.off + 4;
  return packet::EntityVal(buf.data() + body + 8);
}

// 从 INFO_TS 子消息体里读 (sec, frac)
void TimeTsOf(const std::vector<uint8_t>& buf, const SubMsg& s,
              uint32_t* sec, uint32_t* frac) {
  const size_t body = s.off + 4;
  *sec = Rd32(buf.data() + body);
  *frac = Rd32(buf.data() + body + 4);
}

}  // namespace

// ① 最强证据：N=1 时逐字节等于单发
STRATUM_TEST(RtpsPacketBatch, SingleSampleMatchesSinglePacketByteForByte) {
  packet::DataPacketParams p;
  std::memcpy(p.prefix, kPrefix, 12);
  p.writer_entity = packet::kEntityUserWriter;
  p.sn = 0x0000000700000000ULL + 42;
  p.payload = "ABCDEFGH";
  p.source_ts_ns = 1700000000123456789LL;

  const auto single = packet::BuildDataPacket(p);
  const auto batch = packet::BuildDataPacketBatch({p});

  ctx.param_desc = "single=" + std::to_string(single.size()) +
                   " batch1=" + std::to_string(batch.size());

  // ★ 逐字节相同 —— 这是"批量没有另起一套实现"的铁证
  STRATUM_CHECK_EQ(batch.size(), single.size());
  STRATUM_CHECK_TRUE(std::memcmp(batch.data(), single.data(), single.size()) == 0);
}

// ② 空批次 → 空（调用方应把它当 no-op，而不是发一个只有 Header 的空包）
STRATUM_TEST(RtpsPacketBatch, EmptyBatchProducesNothing) {
  const auto buf = packet::BuildDataPacketBatch({});
  ctx.param_desc = "size=" + std::to_string(buf.size());
  STRATUM_CHECK_TRUE(buf.empty());
}

// ③ N=3，不同 writer/sn/source_ts → 3 个 DATA + 3 个 INFO_TS
STRATUM_TEST(RtpsPacketBatch, ThreeSamplesProduceThreeDataSubmessages) {
  std::vector<packet::DataPacketParams> samples;
  for (int i = 0; i < 3; ++i) {
    packet::DataPacketParams p;
    std::memcpy(p.prefix, kPrefix, 12);
    // 故意用不同 writer_entity：证明跨 topic（不同 writer）能合进一个 Message
    p.writer_entity = packet::kEntityUserWriter + static_cast<uint32_t>(i);
    p.sn = 100 + i;
    p.payload = std::string(4 + i, 'A' + i);
    p.source_ts_ns = 1700000000000000000LL + i * 1000000LL;
    samples.push_back(p);
  }

  const auto buf = packet::BuildDataPacketBatch(samples);
  const auto subs = ScanSubmessages(buf);
  const size_t n_ts = CountId(subs, packet::kSubInfoTs);
  const size_t n_data = CountId(subs, packet::kSubData);

  ctx.param_desc = "size=" + std::to_string(buf.size()) +
                   " info_ts=" + std::to_string(n_ts) +
                   " data=" + std::to_string(n_data);

  STRATUM_CHECK_TRUE(std::memcmp(buf.data(), "RTPS", 4) == 0);
  STRATUM_CHECK_EQ(n_ts, static_cast<size_t>(3));
  STRATUM_CHECK_EQ(n_data, static_cast<size_t>(3));

  // 每个 DATA 的 writerId 必须是各自的（证明没有串用同一个 writer）
  std::vector<uint32_t> wids;
  for (const auto& s : subs) {
    if (s.id == packet::kSubData) wids.push_back(WriterIdOf(buf, s));
  }
  STRATUM_CHECK_EQ(wids.size(), static_cast<size_t>(3));
  for (size_t i = 0; i < wids.size(); ++i) {
    STRATUM_CHECK_EQ(static_cast<int>(wids[i]),
                     static_cast<int>(packet::kEntityUserWriter + i));
  }
}

// ④ 每条 INFO_TS 的值必须是各自的 source_ts（不能共用第一个）
STRATUM_TEST(RtpsPacketBatch, EachInfoTsCarriesItsOwnTimestamp) {
  const int64_t kBase = 1700000000000000000LL;
  std::vector<packet::DataPacketParams> samples;
  for (int i = 0; i < 3; ++i) {
    packet::DataPacketParams p;
    std::memcpy(p.prefix, kPrefix, 12);
    p.sn = static_cast<uint64_t>(i) + 1;
    p.payload = "X";
    p.source_ts_ns = kBase + static_cast<int64_t>(i) * 5000000000LL;  // 差 5 秒
    samples.push_back(p);
  }

  const auto buf = packet::BuildDataPacketBatch(samples);
  const auto subs = ScanSubmessages(buf);

  std::vector<uint32_t> secs;
  for (const auto& s : subs) {
    if (s.id != packet::kSubInfoTs) continue;
    uint32_t sec = 0, frac = 0;
    TimeTsOf(buf, s, &sec, &frac);
    secs.push_back(sec);
  }
  ctx.param_desc = "info_ts_count=" + std::to_string(secs.size());
  STRATUM_CHECK_EQ(secs.size(), static_cast<size_t>(3));

  // 三个秒数必须互不相同且递增（间隔 5 秒 → 秒数差 5）
  if (secs.size() == 3) {
    STRATUM_CHECK_EQ(static_cast<int>(secs[1] - secs[0]), 5);
    STRATUM_CHECK_EQ(static_cast<int>(secs[2] - secs[1]), 5);
  }
}

// ⑤ 单条 payload 超上限 → 整批返回空（不发畸形报文）
//
// 【诚实标注】这条用例实际是被**总长检查**（60000）拦下的，不是单条检查（65535）：
//   因为 65535 > 60000，任何"单条超界"的批次必然先撞总长上限。
// 所以实现里那道单条检查目前是**防御性代码**（把总长提到 64KiB+ 时才会走到）。
// 不这样标注的话，后人会误以为"单条检查有测试覆盖"。
//
// 本用例真正验证的是：**整批拒绝，而不是跳过坏的那条继续发** ——
// 后者会让调用方以为全部发成功（静默丢数据）。
STRATUM_TEST(RtpsPacketBatch, OversizedSingleSampleRejectsWholeBatch) {
  std::vector<packet::DataPacketParams> samples;
  {
    packet::DataPacketParams p;
    std::memcpy(p.prefix, kPrefix, 12);
    p.sn = 1;
    p.payload = "ok";
    samples.push_back(p);
  }
  {
    packet::DataPacketParams p;
    std::memcpy(p.prefix, kPrefix, 12);
    p.sn = 2;
    // 65535 - 20 = 65515 是 DATA 子消息体的上限；这里再多 1 字节
    p.payload = std::string(65516, 'Z');
    samples.push_back(p);
  }

  const auto buf = packet::BuildDataPacketBatch(samples);
  ctx.param_desc = "size=" + std::to_string(buf.size());
  // 必须整批拒绝：不能"跳过坏的那条、发出去剩下的"——
  // 那会让调用方以为全部发成功（静默丢数据）。
  STRATUM_CHECK_TRUE(buf.empty());
}

// ⑥ 总长超 60000 → 返回空
STRATUM_TEST(RtpsPacketBatch, OversizedTotalRejectsBatch) {
  std::vector<packet::DataPacketParams> samples;
  // 40 条 × 1600 字节 ≈ 64000 > 60000
  for (int i = 0; i < 40; ++i) {
    packet::DataPacketParams p;
    std::memcpy(p.prefix, kPrefix, 12);
    p.sn = static_cast<uint64_t>(i) + 1;
    p.payload = std::string(1600, 'Y');
    samples.push_back(p);
  }
  const auto buf = packet::BuildDataPacketBatch(samples);
  ctx.param_desc = "samples=40 size=" + std::to_string(buf.size());
  STRATUM_CHECK_TRUE(buf.empty());
}

// ⑦ 带宽收益的直接证据：批量报文 < N × 单发报文
STRATUM_TEST(RtpsPacketBatch, BatchIsSmallerThanN_TimesSingle) {
  const int kN = 10;
  const std::string kPayload(24, 'P');   // HelloWorld 量级

  size_t single_total = 0;
  std::vector<packet::DataPacketParams> samples;
  for (int i = 0; i < kN; ++i) {
    packet::DataPacketParams p;
    std::memcpy(p.prefix, kPrefix, 12);
    p.writer_entity = packet::kEntityUserWriter;
    p.sn = static_cast<uint64_t>(i) + 1;
    p.payload = kPayload;
    p.source_ts_ns = 1700000000000000000LL + i * 1000000LL;
    samples.push_back(p);
    single_total += packet::BuildDataPacket(p).size();
  }
  const auto batch = packet::BuildDataPacketBatch(samples);

  const size_t saved = single_total - batch.size();
  const double pct = single_total ? (100.0 * static_cast<double>(saved) /
                                     static_cast<double>(single_total)) : 0.0;
  ctx.param_desc = "single_" + std::to_string(kN) + "=" + std::to_string(single_total) +
                   " batch=" + std::to_string(batch.size()) +
                   " saved=" + std::to_string(saved) +
                   " (" + std::to_string(static_cast<int>(pct)) + "%)";

  // 【实测数字打到 stdout】测试框架只在失败时打印 param_desc，
  // 而这条用例是 PASS 的 —— 所以数字必须显式 printf，否则拿不到"测出来的值"。
  // 含 IP(20)+UDP(8) 的线上字节：单发 N 个包 vs 批量 1 个包。
  const size_t single_wire = single_total + kN * 28;
  const size_t batch_wire = batch.size() + 28;
  std::printf(
      "      [实测] payload=24B N=%d | RTPS: 单发=%zu 批量=%zu 省=%zu(%d%%) | "
      "含IP/UDP: %zu vs %zu 省=%zu(%d%%) | sendto 次数 %d vs 1\n",
      kN, single_total, batch.size(), saved,
      static_cast<int>(pct), single_wire, batch_wire, single_wire - batch_wire,
      static_cast<int>(100.0 * static_cast<double>(single_wire - batch_wire) /
                       static_cast<double>(single_wire)),
      kN);

  // 批量必须更小；按结构它应省掉 (N-1) × (Message Header 20 + INFO_DST 16)
  STRATUM_CHECK_LT(batch.size(), single_total);
  // 省下的量级：至少 (N-1)*20（保守下界，只算 Message Header）
  STRATUM_CHECK_LE(static_cast<size_t>((kN - 1) * 20), saved);
}

STRATUM_TEST_MAIN()
