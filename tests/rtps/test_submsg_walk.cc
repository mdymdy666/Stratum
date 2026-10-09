// 追加测试：验证 SEDP 公告的子消息能被正确遍历到 DATA
//
// 背景：Stratum↔Stratum 实验中，Writer 收到了 Reader 的 228 字节
// Subscription 包，但从未进入 DATA 分支（诊断只对 244 字节的 SPDP 包
// 打印了 "DATA: body=52"）。本测试用**完全相同**的构造路径生成该包，
// 再交给已移植的 SubmessageParser 遍历 —— 不依赖网络，可稳定复现。
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <stratum/protocol/rtps/rtps_packet_utils.h>
#include <stratum/rtps/messages/submessage_parser.h>

#include "test_framework.h"

using namespace stratum::protocol::rtps;
using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;
using namespace stratum_test;

namespace {

const uint8_t kWalkPrefix[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                                 0x54, 0x55, 0x4d, 0x00, 0x00, 0x01};

}  // namespace

STRATUM_TEST(RtpsPacket, AnnouncementSubmessageWalk) {
  packet::EndpointAnnouncement a;
  std::memcpy(a.prefix, kWalkPrefix, 12);
  a.writer_entity = packet::kEntitySedpSubWriter;
  a.endpoint_entity = packet::kEntityUserReader;
  a.topic_name = "hello_world_topic";
  a.type_name = "HelloWorld";
  a.unicast_port = 9111;
  a.sn = 1;

  const auto buf = packet::BuildSedpAnnouncement(a);
  ctx.param_desc = "packet=" + std::to_string(buf.size()) + " 字节";

  STRATUM_CHECK_TRUE(buf.size() > 20);

  SubmessageParser subm;
  CDRMessage_t msg(static_cast<uint32_t>(buf.size()) + 8);
  std::memcpy(msg.buffer, buf.data(), buf.size());
  msg.length = static_cast<uint32_t>(buf.size());
  msg.pos = 20;

  int info_ts = 0, data = 0, other = 0, rounds = 0;
  bool reached_data = false;
  uint32_t data_writer_id = 0;

  while (msg.pos + 4 <= msg.length && rounds++ < 32) {
    const uint32_t before = msg.pos;
    SubmessageHeader_t smh;
    if (!subm.ReadSubmessageHeader(&msg, &smh)) break;
    const uint32_t body = msg.pos;

    if (smh.submessageId == packet::kSubInfoTs) {
      ++info_ts;
    } else if (smh.submessageId == packet::kSubData) {
      ++data;
      reached_data = true;
      // 按与 RtpsWriter 相同的偏移取 writerId
      const size_t prefix = DataTypePrefixSize(msg.buffer + body,
                                               msg.length - body, buf[5]);
      const size_t ids = body + (prefix - 18) + 2;
      if (ids + 8 <= msg.length) {
        data_writer_id = packet::EntityVal(msg.buffer + ids + 4);
      }
    } else {
      ++other;
    }

    if (smh.is_last || body + smh.submessageLength <= before) break;
    msg.pos = body + smh.submessageLength;
  }

  ctx.param_desc += "  info_ts=" + std::to_string(info_ts) +
                    " data=" + std::to_string(data) +
                    " other=" + std::to_string(other) +
                    " writerId=0x" + std::to_string(data_writer_id);

  // 公告必须含 1 个 INFO_TS 与 1 个 DATA —— 若 data==0，
  // 即复现了"收到包却进不了 DATA 分支"的现象
  STRATUM_CHECK_EQ(info_ts, 1);
  STRATUM_CHECK_TRUE(reached_data);
  STRATUM_CHECK_EQ(data, 1);
  STRATUM_CHECK_EQ(static_cast<int>(data_writer_id),
                   static_cast<int>(packet::kEntitySedpSubWriter));
}

// 对照：SPDP 风格的包（含 INFO_DST，244 字节）也必须能走到 DATA
STRATUM_TEST(RtpsPacket, SpdpStylePacketWalk) {
  auto a = packet::EndpointAnnouncement{};
  std::memcpy(a.prefix, kWalkPrefix, 12);
  a.writer_entity = packet::kEntitySpdpWriter;
  a.endpoint_entity = packet::kEntitySpdpWriter;
  a.topic_name = "hello_world_topic";
  a.type_name = "HelloWorld";
  a.unicast_port = 9110;
  a.sn = 1;
  const auto buf = packet::BuildSedpAnnouncement(a);

  SubmessageParser subm;
  CDRMessage_t msg(static_cast<uint32_t>(buf.size()) + 8);
  std::memcpy(msg.buffer, buf.data(), buf.size());
  msg.length = static_cast<uint32_t>(buf.size());
  msg.pos = 20;

  int data = 0, rounds = 0;
  while (msg.pos + 4 <= msg.length && rounds++ < 32) {
    const uint32_t before = msg.pos;
    SubmessageHeader_t smh;
    if (!subm.ReadSubmessageHeader(&msg, &smh)) break;
    const uint32_t body = msg.pos;
    if (smh.submessageId == packet::kSubData) ++data;
    if (smh.is_last || body + smh.submessageLength <= before) break;
    msg.pos = body + smh.submessageLength;
  }
  ctx.param_desc = "packet=" + std::to_string(buf.size()) +
                   " data=" + std::to_string(data) + " rounds=" +
                   std::to_string(rounds);
  STRATUM_CHECK_EQ(data, 1);
}

STRATUM_TEST_MAIN()
