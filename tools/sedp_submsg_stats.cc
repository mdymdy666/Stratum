// 分析 Fast DDS 向 Stratum 发送的**所有**子消息类型
// （不只 DATA：可能是 HEARTBEAT/ACKNACK 等可靠传输控制消息）
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
#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>

using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;

namespace {

const char* SubmsgName(uint8_t id) {
  switch (id) {
    case 0x01: return "PAD";
    case 0x06: return "ACKNACK";
    case 0x07: return "HEARTBEAT";
    case 0x08: return "GAP";
    case 0x09: return "INFO_TS";
    case 0x0c: return "INFO_SRC";
    case 0x0d: return "INFO_REPLY_IP4";
    case 0x0e: return "INFO_DST";
    case 0x0f: return "INFO_REPLY";
    case 0x12: return "NACK_FRAG";
    case 0x13: return "HEARTBEAT_FRAG";
    case 0x15: return "DATA";
    case 0x16: return "DATA_FRAG";
    default: return "?";
  }
}

std::string EntityName(const uint8_t* e) {
  const uint32_t v = (static_cast<uint32_t>(e[0]) << 24) |
                     (static_cast<uint32_t>(e[1]) << 16) |
                     (static_cast<uint32_t>(e[2]) << 8) | e[3];
  switch (v) {
    case 0x000100c2: return "SPDP_W";
    case 0x000100c7: return "SPDP_R";
    case 0x000003c2: return "SEDP_PUB_W";
    case 0x000003c7: return "SEDP_PUB_R";
    case 0x000004c2: return "SEDP_SUB_W";
    case 0x000004c7: return "SEDP_SUB_R";
    default: {
      char b[24];
      std::snprintf(b, sizeof(b), "0x%08x", v);
      return b;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int seconds = (argc > 1) ? std::atoi(argv[1]) : 15;
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

  std::map<std::string, int> submsg_kinds;      // 子消息类型 → 次数
  std::map<std::string, std::string> hb_detail; // HEARTBEAT 详情
  size_t packets = 0;

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    announcer.AnnounceOnce(nullptr);

    uint8_t buf[65536];
    const ssize_t n = ::recv(rx, buf, sizeof(buf), 0);
    if (n <= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(90));
      continue;
    }
    ++packets;
    if (n < 20 || std::memcmp(buf, "RTPS", 4) != 0) continue;

    size_t p = 20;
    while (p + 4 <= static_cast<size_t>(n)) {
      const uint8_t sid = buf[p];
      const uint16_t olen = static_cast<uint16_t>(buf[p + 2]) |
                            (static_cast<uint16_t>(buf[p + 3]) << 8);
      std::string key = SubmsgName(sid);
      ++submsg_kinds[key];

      // HEARTBEAT 的 writerId（位于体偏移 4）
      if (sid == 0x07 && p + 4 + 8 <= static_cast<size_t>(n)) {
        const size_t base = p + 4 + 4;   // readerId(4) 之后是 writerId
        if (base + 4 <= static_cast<size_t>(n)) {
          hb_detail[key] = EntityName(buf + base);
        }
      }
      if (olen == 0) break;
      p += 4 + olen;
    }
  }
  ::close(rx);

  std::printf("\n收到包总数 = %zu\n", packets);
  std::printf("\n=== 子消息类型统计 ===\n");
  for (const auto& kv : submsg_kinds) {
    std::printf("  %-14s x%d", kv.first.c_str(), kv.second);
    auto it = hb_detail.find(kv.first);
    if (it != hb_detail.end()) {
      std::printf("   (writer=%s)", it->second.c_str());
    }
    std::printf("\n");
  }
  return 0;
}
