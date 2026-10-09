// M5 端到端：Stratum 作为 RTPS Reader 订阅并接收真实用户数据
//
// 完整链路：
//   1. 发 SPDP → 被 Fast DDS 发现
//   2. 收 SEDP Publication → 得知远端有哪些话题/类型
//   3. **为我方订阅这些话题，构造并发送 SEDP Subscription 公告**
//      （经我方的 SEDP subscriptions writer，EntityId 0x000004c2）
//   4. 发 HEARTBEAT 宣告有订阅数据，处理对端 ACKNACK
//   5. Fast DDS 完成 QoS 匹配后，向我们的 default unicast locator 推送用户数据
//   6. 解析并打印用户数据
//
// 组件全部复用已移植代码：
//   - ParameterList / ParameterSerializer（构造与解析参数表）
//   - RTPSMessageCreator（ACKNACK / HEARTBEAT 子消息）
//   - SubmessageParser（解析收到的子消息）
//   - SedpParser / SpdpParser（发现）
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <stratum/rtps/builtin/qos_matcher.h>
#include <stratum/rtps/builtin/sedp_parser.h>
#include <stratum/rtps/builtin/spdp_announcer.h>
#include <stratum/rtps/builtin/spdp_parser.h>
#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/core/policy/ParameterSerializer.hpp>
#include <stratum/rtps/dds/core/policy/ParameterTypes.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>
#include <stratum/rtps/messages/RTPSMessageCreator.hpp>
#include <stratum/rtps/messages/submessage_parser.h>

using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;
namespace dds = eprosima::fastdds::dds;

namespace {

constexpr uint32_t kSedpPubW = 0x000003c2;
constexpr uint32_t kSedpSubW = 0x000004c2;
constexpr uint32_t kSedpPubR = 0x000003c7;
constexpr uint32_t kSedpSubR = 0x000004c7;
constexpr uint32_t kSpdpW = 0x000100c2;

uint32_t EV(const uint8_t* e) {
  return (static_cast<uint32_t>(e[0]) << 24) |
         (static_cast<uint32_t>(e[1]) << 16) |
         (static_cast<uint32_t>(e[2]) << 8) | e[3];
}

// ---------------------------------------------------------------------------
// 构造 SEDP Subscription 公告
// ---------------------------------------------------------------------------
// 参数顺序与内容参照真实 Fast DDS 的 SEDP 公告（见 tests/vectors/sedp_capture.bin
// 与 tools/sedp_sniff.cc 的实测输出）。
std::vector<uint8_t> BuildSedpSubscription(
    const GuidPrefix_t& my_prefix, const GUID_t& reader_guid,
    const GUID_t& participant_guid, const std::string& topic_name,
    const std::string& type_name, const Locator_t& unicast_locator,
    uint64_t sn) {
  CDRMessage_t msg(1024);
  msg.msg_endian = LITTLEEND;

  // --- RTPS 头 ---
  for (char c : {'R', 'T', 'P', 'S'}) CDRMessage::addOctet(&msg, c);
  CDRMessage::addOctet(&msg, 2);
  CDRMessage::addOctet(&msg, 4);
  CDRMessage::addOctet(&msg, 0x01);
  CDRMessage::addOctet(&msg, 0x0f);
  for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
    CDRMessage::addOctet(&msg, my_prefix.value[i]);
  }

  // --- INFO_TS ---
  CDRMessage::addOctet(&msg, 0x09);
  CDRMessage::addOctet(&msg, 0x01);
  CDRMessage::addUInt16(&msg, 8);
  CDRMessage::addInt32(&msg, 0);
  CDRMessage::addUInt32(&msg, 0);

  // --- INFO_DST（广播：目标未知）---
  CDRMessage::addOctet(&msg, 0x0e);
  CDRMessage::addOctet(&msg, 0x01);
  CDRMessage::addUInt16(&msg, 12);
  for (int i = 0; i < 12; ++i) CDRMessage::addOctet(&msg, 0);

  // --- DATA 子消息 ---
  CDRMessage::addOctet(&msg, 0x15);
  CDRMessage::addOctet(&msg, 0x05);   // E=1(小端) + D=1(有数据)
  const uint32_t size_pos = msg.pos;
  CDRMessage::addUInt16(&msg, 0);     // octetsToNextHeader 占位
  const uint32_t body_start = msg.pos;

  CDRMessage::addUInt16(&msg, 0);     // extraFlags（RTPS 2.3+）
  CDRMessage::addUInt16(&msg, 16);    // octetsToInlineQos
  EntityId_t unknown(ENTITYID_UNKNOWN);
  CDRMessage::addEntityId(&msg, &unknown);            // readerId = unknown（广播）
  EntityId_t sub_writer(kSedpSubW);
  CDRMessage::addEntityId(&msg, &sub_writer);         // writerId = SEDP_SUB_W
  SequenceNumber_t seq(static_cast<int32_t>(sn >> 32),
                       static_cast<uint32_t>(sn & 0xFFFFFFFFu));
  CDRMessage::addSequenceNumber(&msg, &seq);

  // encapsulation（小端 PL_CDR_LE）
  {
    bool ok = CDRMessage::addOctet(&msg, 0);
    ok &= CDRMessage::addOctet(&msg, static_cast<octet>(PL_CDR_LE - msg.msg_endian));
    ok &= CDRMessage::addUInt16(&msg, 0);
    if (!ok) return {};
  }

  constexpr uint16_t kGuidLen = 16;
  constexpr uint16_t kLocatorLen = 24;

  // PID_ENDPOINT_GUID —— 本 Reader 的 GUID
  {
    dds::ParameterGuid_t p(dds::PID_ENDPOINT_GUID, kGuidLen, reader_guid);
    if (!dds::ParameterSerializer<dds::ParameterGuid_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }
  // PID_PARTICIPANT_GUID
  {
    dds::ParameterGuid_t p(dds::PID_PARTICIPANT_GUID, kGuidLen, participant_guid);
    if (!dds::ParameterSerializer<dds::ParameterGuid_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }
  // PID_TOPIC_NAME
  {
    dds::ParameterString_t p(dds::PID_TOPIC_NAME, 0, topic_name);
    if (!dds::ParameterSerializer<dds::ParameterString_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }
  // PID_TYPE_NAME
  {
    dds::ParameterString_t p(dds::PID_TYPE_NAME, 0, type_name);
    if (!dds::ParameterSerializer<dds::ParameterString_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }
  // PID_UNICAST_LOCATOR —— 我方接收地址
  {
    dds::ParameterLocator_t p(dds::PID_UNICAST_LOCATOR, kLocatorLen);
    p.locator = unicast_locator;
    if (!dds::ParameterSerializer<dds::ParameterLocator_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }
  // PID_PROTOCOL_VERSION（实测真实包长度为 4）
  {
    dds::ParameterProtocolVersion_t p(dds::PID_PROTOCOL_VERSION, 4);
    p.protocolVersion = ProtocolVersion_t{2, 4};
    if (!dds::ParameterSerializer<dds::ParameterProtocolVersion_t>::add_to_cdr_message(
            p, &msg)) {
      return {};
    }
  }
  // PID_VENDORID（同上，长度 4）
  {
    dds::ParameterVendorId_t p(dds::PID_VENDORID, 4);
    p.vendorId[0] = 0x01;
    p.vendorId[1] = 0x0f;
    if (!dds::ParameterSerializer<dds::ParameterVendorId_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }
  // PID_SENTINEL
  if (!dds::ParameterSerializer<dds::Parameter_t>::add_parameter_sentinel(&msg)) {
    return {};
  }

  // 回填 octetsToNextHeader（直接写 buffer，不能用 addUInt16 —— 见 spdp_announcer 注释）
  const uint16_t body_len = static_cast<uint16_t>(msg.pos - body_start);
  const octet* o = reinterpret_cast<const octet*>(&body_len);
  msg.buffer[size_pos] = o[0];
  msg.buffer[size_pos + 1] = o[1];

  return std::vector<uint8_t>(msg.buffer, msg.buffer + msg.length);
}

// 构造 HEARTBEAT（宣告我方 SEDP subscriptions writer 有数据）
std::vector<uint8_t> BuildHeartbeat(const GuidPrefix_t& my_prefix,
                                    const uint8_t remote_prefix[12],
                                    const GUID_t& writer_guid,
                                    const GUID_t& reader_guid,
                                    int64_t first_sn, int64_t last_sn) {
  CDRMessage_t hb(256);
  hb.msg_endian = LITTLEEND;
  for (char c : {'R', 'T', 'P', 'S'}) CDRMessage::addOctet(&hb, c);
  CDRMessage::addOctet(&hb, 2);
  CDRMessage::addOctet(&hb, 4);
  CDRMessage::addOctet(&hb, 0x01);
  CDRMessage::addOctet(&hb, 0x0f);
  for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
    CDRMessage::addOctet(&hb, my_prefix.value[i]);
  }
  // INFO_DST
  CDRMessage::addOctet(&hb, 0x0e);
  CDRMessage::addOctet(&hb, 0x01);
  CDRMessage::addUInt16(&hb, 12);
  for (int i = 0; i < 12; ++i) CDRMessage::addOctet(&hb, remote_prefix[i]);
  // HEARTBEAT
  CDRMessage::addOctet(&hb, 0x07);
  CDRMessage::addOctet(&hb, 0x01);
  CDRMessage::addUInt16(&hb, 28);
  CDRMessage::addEntityId(&hb, &reader_guid.entityId);
  CDRMessage::addEntityId(&hb, &writer_guid.entityId);
  SequenceNumber_t f(static_cast<int32_t>(first_sn >> 32),
                     static_cast<uint32_t>(first_sn & 0xFFFFFFFF));
  SequenceNumber_t l(static_cast<int32_t>(last_sn >> 32),
                     static_cast<uint32_t>(last_sn & 0xFFFFFFFF));
  CDRMessage::addSequenceNumber(&hb, &f);
  CDRMessage::addSequenceNumber(&hb, &l);
  CDRMessage::addInt32(&hb, 1);
  return std::vector<uint8_t>(hb.buffer, hb.buffer + hb.length);
}

std::string TryParseHelloWorld(const uint8_t* p, size_t len) {
  if (len < 12) return "(太短)";
  size_t i = 4;
  uint32_t index = 0;
  std::memcpy(&index, p + i, 4);
  i += 4;
  uint32_t slen = 0;
  std::memcpy(&slen, p + i, 4);
  i += 4;
  if (slen == 0 || slen > 4096 || i + slen > len) {
    return "index=" + std::to_string(index);
  }
  std::string s(reinterpret_cast<const char*>(p + i), slen);
  while (!s.empty() && s.back() == '\0') s.pop_back();
  return "index=" + std::to_string(index) + " message=\"" + s + "\"";
}

}  // namespace

int main(int argc, char** argv) {
  const int seconds = (argc > 1) ? std::atoi(argv[1]) : 30;
  const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;
  const uint16_t meta_port = static_cast<uint16_t>(9050 + domain * 10);
  const uint16_t user_port = static_cast<uint16_t>(meta_port + 1);

  auto make_rx = [](uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    int r = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &r, sizeof(r));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
      ::close(fd);
      return -1;
    }
    return fd;
  };
  const int rx_meta = make_rx(meta_port);
  const int rx_user = make_rx(user_port);
  if (rx_meta < 0 || rx_user < 0) {
    std::printf("FAILED: bind %u/%u\n", meta_port, user_port);
    return 1;
  }
  const int tx = ::socket(AF_INET, SOCK_DGRAM, 0);

  GuidPrefix_t my_prefix;
  const uint8_t pre[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                           0x54, 0x55, 0x4d, 0x00, 0x00, 0x01};
  std::memcpy(my_prefix.value, pre, 12);

  SpdpAnnounceConfig cfg;
  cfg.guid_prefix = my_prefix;
  cfg.domain_id = domain;
  cfg.builtin_endpoints = 0x000f0c3f;
  cfg.participant_name = "stratum_recorder";
  cfg.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", meta_port));
  cfg.default_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", user_port));
  SpdpAnnouncer announcer;
  std::string err;
  if (!announcer.Open(cfg, &err)) {
    std::printf("FAILED: Open: %s\n", err.c_str());
    return 1;
  }

  std::printf("端到端录制器：meta=%u user=%u 时长=%d 秒\n",
              meta_port, user_port, seconds);
  std::fflush(stdout);

  SubmessageParser subm;
  SpdpParser spdp;
  SedpParser sedp;
  const QosRequest my_qos = DefaultRecorderQos();

  struct Peer { sockaddr_in sa{}; uint8_t prefix[12] = {0}; };
  std::vector<Peer> peers;
  std::set<std::string> subscribed;      // 已订阅的 topic|type
  std::map<std::string, int> stats;
  uint64_t sub_sn = 0;
  size_t user_samples = 0;
  size_t acks = 0;

  pollfd fds[2];
  fds[0].fd = rx_meta; fds[0].events = POLLIN;
  fds[1].fd = rx_user; fds[1].events = POLLIN;

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(seconds);
  int tick = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    if (++tick % 2 == 0) announcer.AnnounceOnce(nullptr);

    if (::poll(fds, 2, 100) <= 0) continue;

    for (int idx = 0; idx < 2; ++idx) {
      if ((fds[idx].revents & POLLIN) == 0) continue;
      uint8_t buf[65536];
      sockaddr_in src{};
      socklen_t sl = sizeof(src);
      const ssize_t n = ::recvfrom(fds[idx].fd, buf, sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&src), &sl);
      if (n <= 0 || n < 20 || std::memcmp(buf, "RTPS", 4) != 0) continue;
      const uint8_t ver_minor = buf[5];
      ++stats[idx == 0 ? "pkt_meta" : "pkt_user"];

      CDRMessage_t msg(static_cast<uint32_t>(n) + 8);
      std::memcpy(msg.buffer, buf, n);
      msg.length = static_cast<uint32_t>(n);
      msg.pos = 20;

      while (msg.pos + 4 <= msg.length) {
        const uint32_t before = msg.pos;
        SubmessageHeader_t smh;
        if (!subm.ReadSubmessageHeader(&msg, &smh)) break;
        const uint32_t body = msg.pos;

        if (smh.submessageId == 0x07) {        // HEARTBEAT → ACKNACK
          msg.pos = body;
          HeartbeatInfo hb;
          if (subm.ProcHeartbeat(&msg, &smh, &hb)) {
            ++stats["HEARTBEAT"];
            CDRMessage_t ack(512);
            ack.msg_endian = LITTLEEND;
            for (char c : {'R', 'T', 'P', 'S'}) CDRMessage::addOctet(&ack, c);
            CDRMessage::addOctet(&ack, 2);
            CDRMessage::addOctet(&ack, 4);
            CDRMessage::addOctet(&ack, 0x01);
            CDRMessage::addOctet(&ack, 0x0f);
            for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
              CDRMessage::addOctet(&ack, my_prefix.value[i]);
            }
            CDRMessage::addOctet(&ack, 0x0e);
            CDRMessage::addOctet(&ack, 0x01);
            CDRMessage::addUInt16(&ack, 12);
            for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
              CDRMessage::addOctet(&ack, buf[8 + i]);
            }
            const uint32_t wid = EV(hb.writer_id.value);
            const uint32_t rid = (wid == kSedpPubW) ? kSedpPubR
                                : (wid == kSedpSubW) ? kSedpSubR
                                : kSedpPubR;
            EntityId_t reader_id(rid);
            // 【关键】SNSet 必须把缺失的 SN 位全部置 1，
            // 否则 Fast DDS 认为 reader 已拥有全部数据而不重传 ——
            // 实测只设 base 时完全收不到 SEDP DATA（0 个），
            // 补上位图后立即收到（参考 tools/discovery_client.cc 的成功写法）。
            SequenceNumberSet_t sns(hb.first_sn);
            {
              const uint32_t span = (hb.last_sn >= hb.first_sn)
                  ? (hb.last_sn.low - hb.first_sn.low + 1) : 1;
              const uint32_t nbits = span > 256 ? 256 : span;
              for (uint32_t k = 0; k < nbits; ++k) {
                sns.add(SequenceNumber_t(0, hb.first_sn.low + k));
              }
            }
            if (RTPSMessageCreator::addSubmessageAcknack(
                    &ack, reader_id, hb.writer_id, sns,
                    static_cast<int32_t>(acks), false)) {
              // 【关键】ACKNACK 必须发到对端在 SPDP 里声明的 metatraffic
              // unicast locator，而不是收到包的源地址 —— 后者可能是组播
              // 地址（SPDP 组播端口），Fast DDS 不会在那里处理 ACKNACK。
              // 这与 M3 阶段遇到的是同一个坑（当时也用源地址，收不到 SEDP）。
              if (peers.empty()) {
                // 尚未学到对端地址时，退回源地址（此时多半也无效）
                if (::sendto(tx, ack.buffer, ack.length, 0,
                             reinterpret_cast<sockaddr*>(&src), sl) > 0) {
                  ++acks;
                }
              } else {
                for (const Peer& peer : peers) {
                  // INFO_DST 需指向该 peer 的 prefix —— 重建一次 ack
                  CDRMessage_t a2(512);
                  a2.msg_endian = LITTLEEND;
                  for (char c : {'R', 'T', 'P', 'S'}) CDRMessage::addOctet(&a2, c);
                  CDRMessage::addOctet(&a2, 2);
                  CDRMessage::addOctet(&a2, 4);
                  CDRMessage::addOctet(&a2, 0x01);
                  CDRMessage::addOctet(&a2, 0x0f);
                  for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
                    CDRMessage::addOctet(&a2, my_prefix.value[i]);
                  }
                  CDRMessage::addOctet(&a2, 0x0e);
                  CDRMessage::addOctet(&a2, 0x01);
                  CDRMessage::addUInt16(&a2, 12);
                  for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
                    CDRMessage::addOctet(&a2, peer.prefix[i]);
                  }
                  if (RTPSMessageCreator::addSubmessageAcknack(
                          &a2, reader_id, hb.writer_id, sns,
                          static_cast<int32_t>(acks), false)) {
                    if (::sendto(tx, a2.buffer, a2.length, 0,
                                 reinterpret_cast<const sockaddr*>(&peer.sa),
                                 sizeof(peer.sa)) > 0) {
                      ++acks;
                    }
                  }
                }
              }
            }
          }
        } else if (smh.submessageId == 0x15) {   // DATA
          const size_t prefix = DataTypePrefixSize(msg.buffer + body,
                                                   msg.length - body, ver_minor);
          const size_t ids = body + (prefix - 18) + 2;
          if (ids + 8 > msg.length) {
            if (smh.is_last || body + smh.submessageLength <= before) break;
            msg.pos = body + smh.submessageLength;
            continue;
          }
          const uint32_t wid = EV(msg.buffer + ids + 4);

          if (wid == kSpdpW) {
            ++stats["SPDP"];
            std::vector<SpdpParticipantData> pds;
            spdp.ParseMessage(buf, static_cast<size_t>(n), &pds);
            for (const auto& pd : pds) {
              for (const Locator_t& loc : pd.metatraffic_unicast_locators) {
                bool dup = false;
                for (const auto& e : peers) {
                  if (e.sa.sin_port == htons(static_cast<uint16_t>(loc.port))) dup = true;
                }
                if (dup) continue;
                Peer pa;
                pa.sa.sin_family = AF_INET;
                pa.sa.sin_port = htons(static_cast<uint16_t>(loc.port));
                std::memcpy(&pa.sa.sin_addr.s_addr, &loc.address[12], 4);
                std::memcpy(pa.prefix, pd.guid.guidPrefix.value, 12);
                peers.push_back(pa);
                char ip[32] = {0};
                ::inet_ntop(AF_INET, &pa.sa.sin_addr, ip, sizeof(ip));
                std::printf("  [发现对端] %s:%u\n", ip,
                            static_cast<unsigned>(loc.port));
              }
            }
          } else if (wid == kSedpPubW) {
            ++stats["SEDP_PUB"];
            // 收到远端 Writer 公告 → 若 QoS 兼容则订阅该话题
            std::vector<SedpEndpointData> eds;
            sedp.ParseMessage(buf, static_cast<size_t>(n), &eds);
            for (const auto& ed : eds) {
              const MatchResult mr = MatchWriterToReader(ed, my_qos);
              std::printf("  [远端 Writer] 话题=\"%s\" 类型=\"%s\" → %s\n",
                          ed.topic_name.c_str(), ed.type_name.c_str(),
                          mr.matched ? "QoS 兼容，准备订阅" : "QoS 不兼容");
              if (!mr.matched) {
                if (!mr.details.empty()) {
                  std::printf("      原因: %s\n", mr.details[0].c_str());
                }
                continue;
              }
              const std::string key = ed.topic_name + "|" + ed.type_name;
              if (subscribed.count(key) != 0) continue;
              if (peers.empty()) continue;

              // 构造并发送我方 SEDP Subscription
              GUID_t reader_guid;
              reader_guid.guidPrefix = my_prefix;
              // 用一个 user reader 的 EntityId（kind=0x04 reader with key）
              reader_guid.entityId = EntityId_t(0x00000104u);
              GUID_t part_guid;
              part_guid.guidPrefix = my_prefix;
              part_guid.entityId = EntityId_t(ENTITYID_RTPSParticipant);

              const auto payload = BuildSedpSubscription(
                  my_prefix, reader_guid, part_guid, ed.topic_name, ed.type_name,
                  MakeDefaultUdpv4Locator("127.0.0.1", user_port), ++sub_sn);
              if (payload.empty()) {
                std::printf("      ** 构造 Subscription 失败 **\n");
                continue;
              }
              for (const Peer& peer : peers) {
                ::sendto(tx, payload.data(), payload.size(), 0,
                         reinterpret_cast<const sockaddr*>(&peer.sa), sizeof(peer.sa));
                // 同一条数据再发一次 HEARTBEAT 宣告
                GUID_t sub_writer_guid;
                sub_writer_guid.guidPrefix = my_prefix;
                sub_writer_guid.entityId = EntityId_t(kSedpSubW);
                const auto hbp = BuildHeartbeat(
                    my_prefix, peer.prefix, sub_writer_guid, reader_guid,
                    static_cast<int64_t>(sub_sn), static_cast<int64_t>(sub_sn));
                ::sendto(tx, hbp.data(), hbp.size(), 0,
                         reinterpret_cast<const sockaddr*>(&peer.sa), sizeof(peer.sa));
              }
              subscribed.insert(key);
              std::printf("      → 已发送 SEDP Subscription（sn=%llu，话题=\"%s\"）\n",
                          static_cast<unsigned long long>(sub_sn),
                          ed.topic_name.c_str());
              std::fflush(stdout);
            }
          } else if (wid == kSedpSubW) {
            ++stats["SEDP_SUB"];
          } else {
            // 用户数据
            ++stats["user_DATA"];
            ++user_samples;
            size_t p = body + prefix;
            if ((smh.flags & 0x02) != 0) {
              int guard = 0;
              while (p + 4 <= msg.length && guard++ < 256) {
                const uint16_t pid = static_cast<uint16_t>(msg.buffer[p]) |
                                     (static_cast<uint16_t>(msg.buffer[p + 1]) << 8);
                const uint16_t plen = static_cast<uint16_t>(msg.buffer[p + 2]) |
                                      (static_cast<uint16_t>(msg.buffer[p + 3]) << 8);
                p += 4 + ((plen + 3) & ~3u);
                if (pid == 0x0001) break;
              }
            }
            if (p + 4 <= msg.length) {
              std::printf("  [★ 用户数据 #%zu] writer=0x%08x %s\n",
                          user_samples, wid,
                          TryParseHelloWorld(msg.buffer + p, msg.length - p).c_str());
              std::fflush(stdout);
            }
          }
        }

        if (smh.is_last || body + smh.submessageLength <= before) break;
        msg.pos = body + smh.submessageLength;
      }
    }
  }
  ::close(rx_meta);
  ::close(rx_user);
  ::close(tx);

  std::printf("\n=== 汇总 ===\n");
  std::printf("  对端地址       = %zu\n", peers.size());
  std::printf("  已订阅话题     = %zu\n", subscribed.size());
  std::printf("  ACKNACK 发送   = %zu\n", acks);
  std::printf("  用户数据样本   = %zu\n", user_samples);
  for (const auto& kv : stats) {
    std::printf("  %-14s x%d\n", kv.first.c_str(), kv.second);
  }
  return user_samples > 0 ? 0 : 2;
}
