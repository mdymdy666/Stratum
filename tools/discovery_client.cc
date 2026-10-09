// M3 端到端：Stratum 发现客户端 —— 完成发现全流程并产出结构化端点信息
//
// 流程：
//   1. 发 SPDP（声明自己）→ 学到对端 metatraffic locator
//   2. 收 HEARTBEAT（SEDP writer）→ 回 ACKNACK
//   3. 收 SEDP DATA → SedpParser 解析 → 打印话题/类型/QoS
//   4. 保存真实 SEDP 向量供测试复现
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <stratum/rtps/builtin/sedp_parser.h>
#include <stratum/rtps/builtin/spdp_announcer.h>
#include <stratum/rtps/builtin/spdp_parser.h>
#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>
#include <stratum/rtps/messages/RTPSMessageCreator.hpp>
#include <stratum/rtps/messages/submessage_parser.h>

using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;

namespace {

constexpr uint32_t kPubW = 0x000003c2;
constexpr uint32_t kSubW = 0x000004c2;
constexpr uint32_t kPubR = 0x000003c7;
constexpr uint32_t kSubR = 0x000004c7;

uint32_t EntityVal(const uint8_t* e) {
  return (static_cast<uint32_t>(e[0]) << 24) |
         (static_cast<uint32_t>(e[1]) << 16) |
         (static_cast<uint32_t>(e[2]) << 8) | e[3];
}

struct Peer {
  sockaddr_in sa{};
  uint8_t guid_prefix[12] = {0};
};

}  // namespace

int main(int argc, char** argv) {
  const int seconds = (argc > 1) ? std::atoi(argv[1]) : 20;
  const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;
  const char* vec_out = (argc > 3) ? argv[3] : nullptr;
  const uint16_t meta_port = static_cast<uint16_t>(9050 + domain * 10);

  const int rx = ::socket(AF_INET, SOCK_DGRAM, 0);
  int reuse = 1;
  ::setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(meta_port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(rx, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
    std::printf("FAILED: bind %u\n", meta_port);
    return 1;
  }
  timeval tv{};
  tv.tv_sec = 1;
  ::setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  const int tx = ::socket(AF_INET, SOCK_DGRAM, 0);

  SpdpAnnounceConfig cfg;
  const uint8_t prefix[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                              0x54, 0x55, 0x4d, 0x00, 0x00, 0x01};
  std::memcpy(cfg.guid_prefix.value, prefix, 12);
  cfg.domain_id = domain;
  cfg.builtin_endpoints = 0x000f0c3f;
  cfg.participant_name = "stratum_recorder";
  cfg.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", meta_port));
  cfg.default_unicast.push_back(MakeDefaultUdpv4Locator(
      "127.0.0.1", static_cast<uint16_t>(meta_port + 1)));
  SpdpAnnouncer announcer;
  std::string err;
  if (!announcer.Open(cfg, &err)) {
    std::printf("FAILED: Open: %s\n", err.c_str());
    return 1;
  }

  std::printf("发现客户端启动：domain=%u 端口=%u 时长=%d 秒\n",
              domain, meta_port, seconds);
  std::fflush(stdout);

  SubmessageParser subm;
  SpdpParser spdp;
  SedpParser sedp;
  std::vector<Peer> peers;
  std::vector<std::vector<uint8_t>> sedp_vectors;
  std::set<std::string> seen_endpoints;   // 去重：话题+类型+端点GUID
  size_t acks = 0;

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    announcer.AnnounceOnce(nullptr);

    uint8_t buf[65536];
    sockaddr_in src{};
    socklen_t sl = sizeof(src);
    const ssize_t n = ::recvfrom(rx, buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&src), &sl);
    if (n <= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(90));
      continue;
    }
    if (n < 20 || std::memcmp(buf, "RTPS", 4) != 0) continue;
    const uint8_t ver_minor = buf[5];

    CDRMessage_t msg(static_cast<uint32_t>(n) + 8);
    std::memcpy(msg.buffer, buf, n);
    msg.length = static_cast<uint32_t>(n);
    msg.pos = 20;

    while (msg.pos + 4 <= msg.length) {
      const uint32_t before = msg.pos;
      SubmessageHeader_t smh;
      if (!subm.ReadSubmessageHeader(&msg, &smh)) break;
      const uint32_t body = msg.pos;

      if (smh.submessageId == 0x07) {          // HEARTBEAT → 回 ACKNACK
        msg.pos = body;
        HeartbeatInfo hb;
        if (subm.ProcHeartbeat(&msg, &smh, &hb)) {
          const uint32_t wid = EntityVal(hb.writer_id.value);
          if ((wid == kPubW || wid == kSubW) && !peers.empty()) {
            const uint32_t rval = (wid == kPubW) ? kPubR : kSubR;
            for (const Peer& peer : peers) {
              CDRMessage_t ack(512);
              ack.msg_endian = LITTLEEND;
              for (char c : {'R', 'T', 'P', 'S'}) CDRMessage::addOctet(&ack, c);
              CDRMessage::addOctet(&ack, 2);
              CDRMessage::addOctet(&ack, 4);
              CDRMessage::addOctet(&ack, 0x01);
              CDRMessage::addOctet(&ack, 0x0f);
              for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
                CDRMessage::addOctet(&ack, cfg.guid_prefix.value[i]);
              }
              CDRMessage::addOctet(&ack, 0x0e);
              CDRMessage::addOctet(&ack, 0x01);
              CDRMessage::addUInt16(&ack, 12);
              for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
                CDRMessage::addOctet(&ack, peer.guid_prefix[i]);
              }
              EntityId_t rid(rval);
              EntityId_t wid_e(wid);
              SequenceNumberSet_t sns(hb.first_sn);
              const uint32_t span = (hb.last_sn >= hb.first_sn)
                  ? (hb.last_sn.low - hb.first_sn.low + 1) : 1;
              const uint32_t nbits = span > 256 ? 256 : span;
              for (uint32_t k = 0; k < nbits; ++k) {
                sns.add(SequenceNumber_t(0, hb.first_sn.low + k));
              }
              if (RTPSMessageCreator::addSubmessageAcknack(
                      &ack, rid, wid_e, sns, static_cast<int32_t>(acks), false)) {
                ::sendto(tx, ack.buffer, ack.length, 0,
                         reinterpret_cast<const sockaddr*>(&peer.sa),
                         sizeof(peer.sa));
                ++acks;
              }
            }
          }
        }
      } else if (smh.submessageId == 0x15) {   // DATA
        const size_t ids = body + ((ver_minor >= 3) ? 2 : 0) + 2;
        if (ids + 8 <= msg.length) {
          const uint32_t wid = EntityVal(msg.buffer + ids + 4);
          if (wid == 0x000100c2u) {            // SPDP：学习对端地址
            std::vector<SpdpParticipantData> pds;
            spdp.ParseMessage(buf, static_cast<size_t>(n), &pds);
            for (const auto& pd : pds) {
              for (const Locator_t& loc : pd.metatraffic_unicast_locators) {
                bool dup = false;
                for (const auto& e : peers) {
                  if (e.sa.sin_port == htons(static_cast<uint16_t>(loc.port))) {
                    dup = true;
                  }
                }
                if (dup) continue;
                Peer pa;
                pa.sa.sin_family = AF_INET;
                pa.sa.sin_port = htons(static_cast<uint16_t>(loc.port));
                std::memcpy(&pa.sa.sin_addr.s_addr, &loc.address[12], 4);
                std::memcpy(pa.guid_prefix, pd.guid.guidPrefix.value, 12);
                peers.push_back(pa);
                char ip[32] = {0};
                ::inet_ntop(AF_INET, &pa.sa.sin_addr, ip, sizeof(ip));
                std::printf("  [发现] 对端 metatraffic %s:%u\n", ip,
                            static_cast<unsigned>(loc.port));
              }
            }
          } else if (wid == kPubW || wid == kSubW) {   // SEDP
            if (sedp_vectors.size() < 64) {
              sedp_vectors.emplace_back(buf, buf + n);
            }
            std::vector<SedpEndpointData> eds;
            sedp.ParseMessage(buf, static_cast<size_t>(n), &eds);
            for (const auto& ed : eds) {
              char key[256];
              std::snprintf(key, sizeof(key), "%s|%s|%02x%02x%02x%02x",
                            ed.topic_name.c_str(), ed.type_name.c_str(),
                            ed.endpoint_guid.guidPrefix.value[0],
                            ed.endpoint_guid.guidPrefix.value[1],
                            ed.endpoint_guid.guidPrefix.value[2],
                            ed.endpoint_guid.guidPrefix.value[3]);
              if (seen_endpoints.count(key) != 0) continue;
              seen_endpoints.insert(key);

              std::printf("\n[端点 %s] 话题=\"%s\" 类型=\"%s\"\n",
                          ed.kind == SedpKind::kPublication ? "Writer" : "Reader",
                          ed.topic_name.c_str(), ed.type_name.c_str());
              std::printf("    endpointGUID=%02x%02x%02x%02x entity=%02x%02x%02x%02x\n",
                          ed.endpoint_guid.guidPrefix.value[0],
                          ed.endpoint_guid.guidPrefix.value[1],
                          ed.endpoint_guid.guidPrefix.value[2],
                          ed.endpoint_guid.guidPrefix.value[3],
                          ed.endpoint_guid.entityId.value[0],
                          ed.endpoint_guid.entityId.value[1],
                          ed.endpoint_guid.entityId.value[2],
                          ed.endpoint_guid.entityId.value[3]);
              std::printf("    QoS: reliability=%u durability=%u ownership=%u "
                          "dataRep=%u deadline=%d.%u\n",
                          ed.reliability_kind, ed.durability_kind,
                          ed.ownership_kind, ed.data_representation,
                          ed.deadline.seconds(), ed.deadline.fraction());
              std::printf("    locators: unicast=%zu multicast=%zu, "
                          "params=%zu unknown=%zu\n",
                          ed.unicast_locators.size(), ed.multicast_locators.size(),
                          ed.parameters_seen, ed.parameters_unknown);
              std::fflush(stdout);
            }
          }
        }
      }

      if (smh.is_last || body + smh.submessageLength <= before) break;
      msg.pos = body + smh.submessageLength;
    }
  }
  ::close(rx);
  ::close(tx);

  std::printf("\n=== 汇总 ===\n");
  std::printf("  对端地址       = %zu\n", peers.size());
  std::printf("  ACKNACK 已发送 = %zu\n", acks);
  std::printf("  SEDP 向量保存  = %zu\n", sedp_vectors.size());
  std::printf("  去重端点       = %zu\n", seen_endpoints.size());

  if (vec_out != nullptr && !sedp_vectors.empty()) {
    std::ofstream out(vec_out, std::ios::binary);
    const uint32_t count = static_cast<uint32_t>(sedp_vectors.size());
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& v : sedp_vectors) {
      const uint32_t len = static_cast<uint32_t>(v.size());
      out.write(reinterpret_cast<const char*>(&len), sizeof(len));
      out.write(reinterpret_cast<const char*>(v.data()), len);
    }
    std::printf("  已写入 %s\n", vec_out);
  }
  return 0;
}
