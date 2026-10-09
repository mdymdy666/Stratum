// M3 起点：Stratum 发送 SPDP 后，接收 Fast DDS 主动发来的 SEDP 公告。
//
// 数据流：
//   1. Stratum 声明 metatraffic locator（如 9050）并周期发送 SPDP
//   2. Fast DDS 发现 Stratum → 向该 locator 单播它的 SEDP
//      （publications / subscriptions 公告）
//   3. 本程序打印收到的 SEDP 结构，驱动 M3 的解析器移植
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
#include <string>
#include <thread>
#include <vector>

#include <stratum/rtps/builtin/spdp_announcer.h>
#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>

using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;

namespace {

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
    case 0x000002c2: return "SEDP_TOPIC_W";
    case 0x000002c7: return "SEDP_TOPIC_R";
    default: {
      char b[24];
      std::snprintf(b, sizeof(b), "0x%08x", v);
      return b;
    }
  }
}

void DumpData(const uint8_t* d, size_t n, size_t p, uint8_t ver_minor) {
  const size_t prefix = (ver_minor >= 3) ? 20 : 18;
  const size_t ids = p + 4 + ((ver_minor >= 3) ? 2 : 0) + 2;
  if (ids + 8 > n) return;
  std::printf("      reader=%-12s writer=%-12s\n",
              EntityName(d + ids).c_str(), EntityName(d + ids + 4).c_str());

  size_t q = p + 4 + prefix + 4;   // 跳过 encapsulation
  while (q + 4 <= n) {
    const uint16_t pid = static_cast<uint16_t>(d[q]) |
                         (static_cast<uint16_t>(d[q + 1]) << 8);
    const uint16_t plen = static_cast<uint16_t>(d[q + 2]) |
                          (static_cast<uint16_t>(d[q + 3]) << 8);
    std::string hex;
    for (uint16_t i = 0; i < plen && i < 12; ++i) {
      char b[4];
      std::snprintf(b, sizeof(b), "%02x", d[q + 4 + i]);
      hex += b;
    }
    std::printf("      PID=0x%04x len=%3u val=%s\n", pid, plen, hex.c_str());
    q += 4 + ((plen + 3) & ~3u);
    if (pid == 0x0001) break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int seconds = (argc > 1) ? std::atoi(argv[1]) : 15;
  const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;
  const uint16_t meta_port = static_cast<uint16_t>(9050 + domain * 10);

  // 接收 socket
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

  // 发送器
  SpdpAnnounceConfig cfg;
  const uint8_t prefix[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                              0x54, 0x55, 0x4d, 0x00, 0x00, 0x01};
  std::memcpy(cfg.guid_prefix.value, prefix, 12);
  cfg.domain_id = domain;
  cfg.builtin_endpoints = 0x000f0c3f;
  cfg.participant_name = "stratum_recorder";
  cfg.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", meta_port));
  cfg.default_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1",
                                                        static_cast<uint16_t>(meta_port + 1)));
  SpdpAnnouncer announcer;
  std::string err;
  if (!announcer.Open(cfg, &err)) {
    std::printf("FAILED: Open: %s\n", err.c_str());
    return 1;
  }

  std::printf("domain=%u 声明 metatraffic=%u，发送 SPDP 并等待 SEDP，%d 秒...\n",
              domain, meta_port, seconds);
  std::fflush(stdout);

  std::map<std::string, int> kinds;
  std::map<std::string, std::vector<uint8_t>> first_by_writer;
  size_t rx_total = 0;

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
    ++rx_total;
    if (n < 20 || std::memcmp(buf, "RTPS", 4) != 0) continue;
    const uint8_t ver_minor = buf[5];

    size_t p = 20;
    while (p + 4 <= static_cast<size_t>(n)) {
      const uint8_t sid = buf[p];
      const uint16_t olen = static_cast<uint16_t>(buf[p + 2]) |
                            (static_cast<uint16_t>(buf[p + 3]) << 8);
      if (sid == 0x15) {
        const size_t ids = p + 4 + ((ver_minor >= 3) ? 2 : 0) + 2;
        if (ids + 8 <= static_cast<size_t>(n)) {
          const std::string w = EntityName(buf + ids + 4);
          ++kinds[w];
          if (first_by_writer.find(w) == first_by_writer.end()) {
            first_by_writer[w].assign(buf, buf + n);
          }
        }
      }
      if (olen == 0) break;
      p += 4 + olen;
    }
  }
  ::close(rx);

  std::printf("\n收到包总数 = %zu\n", rx_total);
  std::printf("\n=== DATA 类型统计 ===\n");
  for (const auto& kv : kinds) {
    std::printf("  %-12s x%d\n", kv.first.c_str(), kv.second);
  }
  std::printf("\n=== 各类型首个样本的 PID 列表 ===\n");
  for (const auto& kv : first_by_writer) {
    std::printf("\n[%s]\n", kv.first.c_str());
    const auto& v = kv.second;
    size_t p = 20;
    while (p + 4 <= v.size()) {
      const uint8_t sid = v[p];
      const uint16_t olen = static_cast<uint16_t>(v[p + 2]) |
                            (static_cast<uint16_t>(v[p + 3]) << 8);
      if (sid == 0x15) {
        DumpData(v.data(), v.size(), p, v[5]);
        break;
      }
      if (olen == 0) break;
      p += 4 + olen;
    }
  }
  return 0;
}
