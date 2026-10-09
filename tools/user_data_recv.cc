// M5：同时监听 metatraffic 与 user 两个端口
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
#include <string>
#include <vector>

#include <stratum/rtps/builtin/spdp_announcer.h>
#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>
#include <stratum/rtps/messages/RTPSMessageCreator.hpp>
#include <stratum/rtps/messages/submessage_parser.h>

using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;

namespace {

uint32_t EntityVal(const uint8_t* e) {
  return (static_cast<uint32_t>(e[0]) << 24) |
         (static_cast<uint32_t>(e[1]) << 16) |
         (static_cast<uint32_t>(e[2]) << 8) | e[3];
}

bool IsBuiltin(uint32_t v) {
  return v == 0x000100c2u || v == 0x000100c7u ||
         v == 0x000003c2u || v == 0x000003c7u ||
         v == 0x000004c2u || v == 0x000004c7u ||
         v == 0x000002c2u || v == 0x000002c7u;
}

std::string TryParseHelloWorld(const uint8_t* payload, size_t len) {
  if (len < 8) return "(太短)";
  size_t p = 4;   // 跳过 encapsulation
  if (p + 4 > len) return "(无 index)";
  uint32_t index = 0;
  std::memcpy(&index, payload + p, 4);
  p += 4;
  if (p + 4 > len) return "index=" + std::to_string(index);
  uint32_t slen = 0;
  std::memcpy(&slen, payload + p, 4);
  p += 4;
  if (slen == 0 || slen > 4096 || p + slen > len) {
    return "index=" + std::to_string(index) +
           " (msg_len=" + std::to_string(slen) + ")";
  }
  std::string s(reinterpret_cast<const char*>(payload + p), slen);
  while (!s.empty() && s.back() == '\0') s.pop_back();
  return "index=" + std::to_string(index) + " message=\"" + s + "\"";
}

int MakeRx(uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
    ::close(fd);
    return -1;
  }
  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = ::inet_addr("239.255.0.1");
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
  return fd;
}

}  // namespace

int main(int argc, char** argv) {
  const int seconds = (argc > 1) ? std::atoi(argv[1]) : 20;
  const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;
  const uint16_t meta_port = static_cast<uint16_t>(9050 + domain * 10);
  const uint16_t user_port = static_cast<uint16_t>(meta_port + 1);

  const int rx_meta = MakeRx(meta_port);
  const int rx_user = MakeRx(user_port);
  if (rx_meta < 0 || rx_user < 0) {
    std::printf("FAILED: bind %u/%u\n", meta_port, user_port);
    return 1;
  }
  const int tx = ::socket(AF_INET, SOCK_DGRAM, 0);

  SpdpAnnounceConfig cfg;
  const uint8_t prefix[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                              0x54, 0x55, 0x4d, 0x00, 0x00, 0x01};
  std::memcpy(cfg.guid_prefix.value, prefix, 12);
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

  std::printf("监听 meta=%u user=%u，%d 秒...\n", meta_port, user_port, seconds);
  std::fflush(stdout);

  SubmessageParser subm;
  std::map<std::string, int> stats;
  size_t user_samples = 0;
  size_t acks = 0;

  pollfd fds[2];
  fds[0].fd = rx_meta;
  fds[0].events = POLLIN;
  fds[1].fd = rx_user;
  fds[1].events = POLLIN;

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(seconds);
  int announce_tick = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    if (++announce_tick % 2 == 0) announcer.AnnounceOnce(nullptr);

    const int pr = ::poll(fds, 2, 100);
    if (pr <= 0) continue;

    for (int idx = 0; idx < 2; ++idx) {
      if ((fds[idx].revents & POLLIN) == 0) continue;
      uint8_t buf[65536];
      sockaddr_in src{};
      socklen_t sl = sizeof(src);
      const ssize_t n = ::recvfrom(fds[idx].fd, buf, sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&src), &sl);
      if (n <= 0) continue;
      if (n < 20 || std::memcmp(buf, "RTPS", 4) != 0) {
        ++stats["non-RTPS"];
        continue;
      }
      const uint8_t ver_minor = buf[5];
      ++stats[idx == 0 ? "pkt_on_meta" : "pkt_on_user"];

      CDRMessage_t msg(static_cast<uint32_t>(n) + 8);
      std::memcpy(msg.buffer, buf, n);
      msg.length = static_cast<uint32_t>(n);
      msg.pos = 20;

      while (msg.pos + 4 <= msg.length) {
        const uint32_t before = msg.pos;
        SubmessageHeader_t smh;
        if (!subm.ReadSubmessageHeader(&msg, &smh)) break;
        const uint32_t body = msg.pos;

        if (smh.submessageId == 0x07) {
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
              CDRMessage::addOctet(&ack, cfg.guid_prefix.value[i]);
            }
            CDRMessage::addOctet(&ack, 0x0e);
            CDRMessage::addOctet(&ack, 0x01);
            CDRMessage::addUInt16(&ack, 12);
            for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
              CDRMessage::addOctet(&ack, buf[8 + i]);
            }
            EntityId_t rid(0x000003c7);
            if (RTPSMessageCreator::addSubmessageAcknack(
                    &ack, rid, hb.writer_id, SequenceNumberSet_t(hb.first_sn),
                    static_cast<int32_t>(acks), false)) {
              if (::sendto(tx, ack.buffer, ack.length, 0,
                           reinterpret_cast<sockaddr*>(&src), sl) > 0) {
                ++acks;
              }
            }
          }
        } else if (smh.submessageId == 0x15) {
          // 数据驱动的前缀判定（版本号不可靠）
          const size_t prefix = DataTypePrefixSize(msg.buffer + body,
                                                   msg.length - body, ver_minor);
          const size_t ids = body + (prefix - 18) + 2;
          if (ids + 8 <= msg.length) {
            const uint32_t wid = EntityVal(msg.buffer + ids + 4);
            const uint32_t rid = EntityVal(msg.buffer + ids);
            if (IsBuiltin(wid)) {
              ++stats["builtin_DATA"];
            } else {
              ++stats["user_DATA"];
              ++user_samples;
              // 【关键】payload 偏移不能写死：DATA 子消息可能带 inlineQos
              //   flags bit1 (Q) = 1 → readerId/writerId/writerSN 之后是
              //   inlineQos 参数列表，payload 在其之后。
              //   octetsToInlineQos 给出"从该字段之后到 inlineQos 起点"的偏移。
              size_t payload_pos = 0;
              {
                size_t p = body + prefix;   // inlineQos 起点
                if ((smh.flags & 0x02) != 0) {
                  // 跳过 inlineQos 参数列表（到 PID_SENTINEL 为止）
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
                payload_pos = p;
              }
              if (payload_pos > 0 && payload_pos <= msg.length) {
                std::printf("  [用户数据 #%zu] writer=0x%08x reader=0x%08x "
                            "flags=0x%02x encaps=%02x%02x payload=%zu  %s\n",
                            user_samples, wid, rid, smh.flags,
                            msg.buffer[payload_pos], msg.buffer[payload_pos + 1],
                            msg.length - payload_pos,
                            TryParseHelloWorld(msg.buffer + payload_pos,
                                               msg.length - payload_pos).c_str());
                std::fflush(stdout);
              }
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
  std::printf("  ACKNACK 发送   = %zu\n", acks);
  std::printf("  用户数据样本   = %zu\n", user_samples);
  for (const auto& kv : stats) {
    std::printf("  %-16s x%d\n", kv.first.c_str(), kv.second);
  }
  return user_samples > 0 ? 0 : 2;
}
