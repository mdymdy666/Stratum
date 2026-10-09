// dump 用户数据包结构
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <stratum/rtps/builtin/spdp_announcer.h>
#include <stratum/rtps/messages/submessage_parser.h>
using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;

static uint32_t EV(const uint8_t* e) {
  return (uint32_t(e[0]) << 24) | (uint32_t(e[1]) << 16) | (uint32_t(e[2]) << 8) | e[3];
}
static bool IsBuiltin(uint32_t v) {
  return v == 0x000100c2u || v == 0x000100c7u || v == 0x000003c2u ||
         v == 0x000003c7u || v == 0x000004c2u || v == 0x000004c7u;
}
static int MakeRx(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  int r = 1; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &r, sizeof(r));
  sockaddr_in a{};
  a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd, (sockaddr*)&a, sizeof(a)) < 0) return -1;
  return fd;
}
int main(int argc, char** argv) {
  const uint16_t mp = 9050, up = 9051;
  int rm = MakeRx(mp), ru = MakeRx(up);
  if (rm < 0 || ru < 0) { std::printf("bind fail\n"); return 1; }
  SpdpAnnounceConfig cfg;
  const uint8_t pre[12] = {0x01,0x0f,0x53,0x54,0x52,0x41,0x54,0x55,0x4d,0,0,1};
  std::memcpy(cfg.guid_prefix.value, pre, 12);
  cfg.builtin_endpoints = 0x000f0c3f;
  cfg.participant_name = "stratum";
  cfg.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", mp));
  cfg.default_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", up));
  SpdpAnnouncer ann; std::string e;
  if (!ann.Open(cfg, &e)) { std::printf("open fail\n"); return 1; }
  SubmessageParser sp;
  pollfd fds[2]; fds[0].fd = rm; fds[0].events = POLLIN; fds[1].fd = ru; fds[1].events = POLLIN;
  bool dumped = false;
  auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  int tick = 0;
  while (std::chrono::steady_clock::now() < dl && !dumped) {
    if (++tick % 2 == 0) ann.AnnounceOnce(nullptr);
    if (::poll(fds, 2, 100) <= 0) continue;
    for (int i = 0; i < 2; ++i) {
      if (!(fds[i].revents & POLLIN)) continue;
      uint8_t buf[65536]; ssize_t n = ::recv(fds[i].fd, buf, sizeof(buf), 0);
      if (n < 20 || std::memcmp(buf, "RTPS", 4) != 0) continue;
      const uint8_t vm = buf[5];
      CDRMessage_t msg((uint32_t)n + 8);
      std::memcpy(msg.buffer, buf, n); msg.length = (uint32_t)n; msg.pos = 20;
      while (msg.pos + 4 <= msg.length) {
        uint32_t before = msg.pos;
        SubmessageHeader_t smh;
        if (!sp.ReadSubmessageHeader(&msg, &smh)) break;
        uint32_t body = msg.pos;
        if (smh.submessageId == 0x15) {
          size_t ids = body + ((vm >= 3) ? 2 : 0) + 2;
          if (ids + 8 <= msg.length) {
            uint32_t wid = EV(msg.buffer + ids + 4);
            if (!IsBuiltin(wid)) {
              std::printf("包长=%zd ver=2.%u flags=0x%02x octetsToNext=%u\n",
                          n, vm, smh.flags, smh.submessageLength);
              std::printf("DATA 体起点=%u，前 48 字节：\n  ", body);
              for (uint32_t k = body; k < body + 48 && k < msg.length; ++k) {
                std::printf("%02x ", msg.buffer[k]);
                if ((k - body + 1) % 16 == 0) std::printf("\n  ");
              }
              std::printf("\n");
              std::printf("  [off] 解读：\n");
              std::printf("    body+0  extraFlags      = %02x %02x\n", msg.buffer[body], msg.buffer[body+1]);
              std::printf("    body+2  octetsToInlineQos= %u\n",
                          (unsigned)(msg.buffer[body+2] | (msg.buffer[body+3] << 8)));
              std::printf("    body+4  readerId        = %02x %02x %02x %02x\n",
                          msg.buffer[body+4], msg.buffer[body+5], msg.buffer[body+6], msg.buffer[body+7]);
              std::printf("    body+8  writerId        = %02x %02x %02x %02x\n",
                          msg.buffer[body+8], msg.buffer[body+9], msg.buffer[body+10], msg.buffer[body+11]);
              std::printf("    body+12 writerSN        = %02x %02x %02x %02x %02x %02x %02x %02x\n",
                          msg.buffer[body+12], msg.buffer[body+13], msg.buffer[body+14], msg.buffer[body+15],
                          msg.buffer[body+16], msg.buffer[body+17], msg.buffer[body+18], msg.buffer[body+19]);
              std::printf("    body+20 疑似 encaps     = %02x %02x %02x %02x\n",
                          msg.buffer[body+20], msg.buffer[body+21], msg.buffer[body+22], msg.buffer[body+23]);
              std::printf("    body+24 疑似 payload    = %02x %02x %02x %02x %02x %02x %02x %02x\n",
                          msg.buffer[body+24], msg.buffer[body+25], msg.buffer[body+26], msg.buffer[body+27],
                          msg.buffer[body+28], msg.buffer[body+29], msg.buffer[body+30], msg.buffer[body+31]);
              dumped = true;
            }
          }
        }
        if (smh.is_last || body + smh.submessageLength <= before) break;
        msg.pos = body + smh.submessageLength;
      }
    }
  }
  if (!dumped) std::printf("未捕获到用户数据包\n");
  return dumped ? 0 : 2;
}
