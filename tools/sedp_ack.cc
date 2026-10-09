// M3 修正版：从 SPDP 学到对端 metatraffic locator，向该地址回 ACKNACK
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <stratum/rtps/builtin/spdp_announcer.h>
#include <stratum/rtps/builtin/spdp_parser.h>
#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>
#include <stratum/rtps/messages/RTPSMessageCreator.hpp>
#include <stratum/rtps/messages/submessage_parser.h>

using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;

namespace {

constexpr uint32_t kEntityPubW = 0x000003c2;
constexpr uint32_t kEntitySubW = 0x000004c2;
constexpr uint32_t kEntityPubR = 0x000003c7;
constexpr uint32_t kEntitySubR = 0x000004c7;

uint32_t EntityVal(const uint8_t* e) {
  return (static_cast<uint32_t>(e[0]) << 24) |
         (static_cast<uint32_t>(e[1]) << 16) |
         (static_cast<uint32_t>(e[2]) << 8) | e[3];
}

std::string EntityName(const uint8_t* e) {
  switch (EntityVal(e)) {
    case 0x000100c2: return "SPDP_W";
    case 0x000003c2: return "SEDP_PUB_W";
    case 0x000003c7: return "SEDP_PUB_R";
    case 0x000004c2: return "SEDP_SUB_W";
    case 0x000004c7: return "SEDP_SUB_R";
    default: {
      char b[24];
      std::snprintf(b, sizeof(b), "0x%08x", EntityVal(e));
      return b;
    }
  }
}

struct PeerAddr {
  bool valid = false;
  sockaddr_in sa{};
  uint8_t guid_prefix[12] = {0};
};

}  // namespace

int main(int argc, char** argv) {
  const int seconds = (argc > 1) ? std::atoi(argv[1]) : 25;
  const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;
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

  std::printf("监听 %u，%d 秒...\n", meta_port, seconds);
  std::fflush(stdout);

  SubmessageParser parser;
  SpdpParser spdp;
  std::vector<PeerAddr> peers;
  std::map<std::string, int> stats;
  size_t acks = 0, sedp_data = 0;

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
      if (!parser.ReadSubmessageHeader(&msg, &smh)) break;
      const uint32_t body = msg.pos;

      if (smh.submessageId == 0x07) {          // HEARTBEAT
        ++stats["HEARTBEAT"];
        msg.pos = body;
        HeartbeatInfo hb;
        if (parser.ProcHeartbeat(&msg, &smh, &hb)) {
          const uint32_t wid = EntityVal(hb.writer_id.value);
          if ((wid == kEntityPubW || wid == kEntitySubW) && !peers.empty()) {
            const uint32_t rval = (wid == kEntityPubW) ? kEntityPubR : kEntitySubR;
            for (const PeerAddr& peer : peers) {
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
              CDRMessage::addOctet(&ack, 0x0e);    // INFO_DST
              CDRMessage::addOctet(&ack, 0x01);
              CDRMessage::addUInt16(&ack, 12);
              for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
                CDRMessage::addOctet(&ack, peer.guid_prefix[i]);
              }
              EntityId_t rid(rval);
              EntityId_t wid_e(wid);
              // ACKNACK：base = firstSN，位图全部置 1（表示 firstSN 起全缺）
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
            ++stats["acked_" + EntityName(hb.writer_id.value)];
          }
        }
      } else if (smh.submessageId == 0x15) {   // DATA
        const size_t ids = body + ((ver_minor >= 3) ? 2 : 0) + 2;
        if (ids + 8 <= msg.length) {
          const std::string w = EntityName(msg.buffer + ids + 4);
          ++stats["DATA_" + w];

          if (w == "SPDP_W") {
            // 从 SPDP 里学习对端的 metatraffic unicast locator
            std::vector<SpdpParticipantData> pds;
            spdp.ParseMessage(buf, static_cast<size_t>(n), &pds);
            for (const auto& pd : pds) {
              for (const Locator_t& loc : pd.metatraffic_unicast_locators) {
                PeerAddr pa;
                pa.sa.sin_family = AF_INET;
                pa.sa.sin_port = htons(static_cast<uint16_t>(loc.port));
                std::memcpy(&pa.sa.sin_addr.s_addr, &loc.address[12], 4);
                std::memcpy(pa.guid_prefix, pd.guid.guidPrefix.value, 12);
                pa.valid = true;
                bool dup = false;
                for (const auto& e : peers) {
                  if (e.sa.sin_port == pa.sa.sin_port) dup = true;
                }
                if (!dup) {
                  peers.push_back(pa);
                  char ip[32] = {0};
                  ::inet_ntop(AF_INET, &pa.sa.sin_addr, ip, sizeof(ip));
                  std::printf("  学到对端 metatraffic: %s:%u\n", ip,
                              static_cast<unsigned>(loc.port));
                  std::fflush(stdout);
                }
              }
            }
          } else if (w == "SEDP_PUB_W" || w == "SEDP_SUB_W") {
            if (sedp_data == 0) {
              std::printf("\n*** 收到 SEDP 公告！writer=%s ***\n", w.c_str());
              size_t q = body + 20 + 4;
              while (q + 4 <= msg.length) {
                const uint16_t pid = static_cast<uint16_t>(msg.buffer[q]) |
                                     (static_cast<uint16_t>(msg.buffer[q + 1]) << 8);
                const uint16_t plen = static_cast<uint16_t>(msg.buffer[q + 2]) |
                                      (static_cast<uint16_t>(msg.buffer[q + 3]) << 8);
                std::string hex;
                for (uint16_t i = 0; i < plen && i < 16; ++i) {
                  char b[4];
                  std::snprintf(b, sizeof(b), "%02x", msg.buffer[q + 4 + i]);
                  hex += b;
                }
                std::printf("    PID=0x%04x len=%3u val=%s\n", pid, plen, hex.c_str());
                q += 4 + ((plen + 3) & ~3u);
                if (pid == 0x0001) break;
              }
              std::fflush(stdout);
            }
            ++sedp_data;
          }
        }
      }

      if (smh.is_last || body + smh.submessageLength <= before) break;
      msg.pos = body + smh.submessageLength;
    }
  }
  ::close(rx);
  ::close(tx);

  std::printf("\n=== 统计 ===\n");
  std::printf("  学到对端地址   = %zu\n", peers.size());
  std::printf("  ACKNACK 已发送 = %zu\n", acks);
  std::printf("  SEDP 公告收到  = %zu\n", sedp_data);
  for (const auto& kv : stats) {
    std::printf("  %-22s x%d\n", kv.first.c_str(), kv.second);
  }
  return 0;
}
