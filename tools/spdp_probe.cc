// M2 决定性实验：Stratum 在公告里声明一个**自定义 metatraffic 端口**并监听它。
// 若 Fast DDS 解析并记录了 Stratum，它会向该端口发送 SPDP/SEDP。
//
// 端口选择：不能与 Fast DDS 自己的 metatraffic 端口（7410 + domain*250）冲突，
// 否则 bind 失败。这里用 9000 + domain*10。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <stratum/rtps/builtin/spdp_announcer.h>

using namespace stratum::rtps;

namespace {

bool LooksLikeRtps(const uint8_t* d, size_t n) {
  return n >= 20 && d[0] == 'R' && d[1] == 'T' && d[2] == 'P' && d[3] == 'S';
}

// 打印子消息 id 序列，判断回复类型（DATA=0x15 / INFO_TS=0x09 / ACKNACK=0x06 ...）
std::string SubmsgIds(const uint8_t* d, size_t n) {
  std::string out;
  size_t p = 20;
  int guard = 0;
  while (p + 4 <= n && guard++ < 16) {
    const uint8_t id = d[p];
    const uint16_t olen = static_cast<uint16_t>(d[p + 2]) |
                          (static_cast<uint16_t>(d[p + 3]) << 8);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02x ", id);
    out += buf;
    if (olen == 0) break;
    p += 4 + olen;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const int seconds = (argc > 1) ? std::atoi(argv[1]) : 10;
  const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 5;

  // 自定义 metatraffic 端口：刻意避开 Fast DDS 的 7410 + domain*250
  const uint16_t meta_port = static_cast<uint16_t>(9000 + domain * 10);

  const int rx = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (rx < 0) {
    std::printf("FAILED: socket\n");
    return 1;
  }
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
  // 同时加入 domain 对应的 SPDP 组播组，便于观察对方的组播公告
  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = ::inet_addr("239.255.0.1");
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  ::setsockopt(rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
  timeval tv{};
  tv.tv_sec = 1;
  ::setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  SpdpAnnounceConfig cfg;
  const uint8_t prefix[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                              0x54, 0x55, 0x4d, 0x00, 0x00, 0x01};
  std::memcpy(cfg.guid_prefix.value, prefix, 12);
  cfg.domain_id = domain;
  cfg.builtin_endpoints = 0x3f;
  cfg.participant_name = "stratum_recorder";
  cfg.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", meta_port));
  cfg.default_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", meta_port + 1));

  SpdpAnnouncer announcer;
  std::string err;
  if (!announcer.Open(cfg, &err)) {
    std::printf("FAILED: Open: %s\n", err.c_str());
    return 1;
  }

  std::printf("domain=%u 声明 metatraffic=%u，发送并监听 %d 秒...\n",
              domain, meta_port, seconds);
  std::fflush(stdout);

  size_t rx_total = 0;
  size_t rx_rtps = 0;
  size_t rx_spdp_port = 0;   // 来自 SPDP 组播端口（对方公告）
  bool printed_first = false;

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

    ++rx_total;
    const uint16_t src_port = ntohs(src.sin_port);
    char src_ip[32] = {0};
    ::inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));
    if (src_port == static_cast<uint16_t>(7400 + domain * 250)) {
      ++rx_spdp_port;
    }

    if (LooksLikeRtps(buf, static_cast<size_t>(n))) {
      ++rx_rtps;
      if (!printed_first) {
        printed_first = true;
        std::printf("  [RTPS 包] 来自 %s:%u len=%zd ver=%u.%u vendor=%02x%02x "
                    "submsg=[%s]\n",
                    src_ip, src_port, n, buf[4], buf[5], buf[6], buf[7],
                    SubmsgIds(buf, static_cast<size_t>(n)).c_str());
        std::fflush(stdout);
      }
    } else if (!printed_first) {
      printed_first = true;
      std::printf("  [非 RTPS 包] 来自 %s:%u len=%zd\n", src_ip, src_port, n);
      std::fflush(stdout);
    }
  }
  ::close(rx);

  std::printf("\n结果：\n");
  std::printf("  Stratum 发送         = %zu\n", announcer.sent_packets());
  std::printf("  收到包总数           = %zu\n", rx_total);
  std::printf("  其中 RTPS 包         = %zu\n", rx_rtps);
  std::printf("  来自 SPDP 组播端口   = %zu\n", rx_spdp_port);
  std::printf("  判定：%s\n",
              rx_rtps > 0 ? "Fast DDS 有回应 → 它已看到 Stratum"
                          : "无任何回应 → Fast DDS 未接受该参与者");
  return 0;
}
