// SEDP 抓包分析器：抓取真实 DDS 流量，按 writerId 区分 SPDP / SEDP，
// 并打印每个 DATA 子消息的 PID 列表。用于驱动 M3（SEDP + QoS）的移植。
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
#include <vector>

#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>

using namespace eprosima::fastdds::rtps;

namespace {

// 内建 writer 的 EntityId（大端字节）
std::string EntityName(const uint8_t* e) {
  const uint32_t v = (static_cast<uint32_t>(e[0]) << 24) |
                     (static_cast<uint32_t>(e[1]) << 16) |
                     (static_cast<uint32_t>(e[2]) << 8) | e[3];
  switch (v) {
    case 0x000100c2: return "SPDP_W";
    case 0x000100c7: return "SPDP_R";
    case 0x000003c2: return "SEDP_PUB_W";     // 端点发现：发布公告
    case 0x000003c7: return "SEDP_PUB_R";
    case 0x000004c2: return "SEDP_SUB_W";     // 端点发现：订阅公告
    case 0x000004c7: return "SEDP_SUB_R";
    case 0x000002c2: return "SEDP_TOPIC_W";
    case 0x000002c7: return "SEDP_TOPIC_R";
    case 0x00000000: return "UNKNOWN";
    default: {
      char b[32];
      std::snprintf(b, sizeof(b), "0x%08x", v);
      return b;
    }
  }
}

// 从 DATA 子消息提取 readerId/writerId/参数表 PID 列表
struct DataInfo {
  std::string reader;
  std::string writer;
  std::vector<std::pair<uint16_t, uint16_t>> pids;   // (pid, len)
  std::vector<std::string> pid_values;               // 前 8 字节 hex
};

bool ParseData(const uint8_t* d, size_t n, size_t data_pos, uint8_t ver_minor,
               DataInfo* out) {
  const size_t prefix = (ver_minor >= 3) ? 20 : 18;
  size_t p = data_pos + 4 + prefix;
  if (p + 4 > n) return false;

  // readerId/writerId 在 prefix 内：extraFlags(2)+octetsToInlineQos(2)
  const size_t ids = data_pos + 4 + ((ver_minor >= 3) ? 2 : 0) + 2;
  if (ids + 8 > n) return false;
  out->reader = EntityName(d + ids);
  out->writer = EntityName(d + ids + 4);

  // 参数表
  while (p + 4 <= n) {
    const uint16_t pid = static_cast<uint16_t>(d[p]) |
                         (static_cast<uint16_t>(d[p + 1]) << 8);
    const uint16_t plen = static_cast<uint16_t>(d[p + 2]) |
                          (static_cast<uint16_t>(d[p + 3]) << 8);
    std::string hex;
    for (uint16_t i = 0; i < plen && i < 8; ++i) {
      char b[4];
      std::snprintf(b, sizeof(b), "%02x", d[p + 4 + i]);
      hex += b;
    }
    out->pids.emplace_back(pid, plen);
    out->pid_values.push_back(hex);
    p += 4 + ((plen + 3) & ~3u);
    if (pid == 0x0001) break;
    if (out->pids.size() > 60) break;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const int seconds = (argc > 1) ? std::atoi(argv[1]) : 8;
  const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;
  const char* vec_out = (argc > 3) ? argv[3] : nullptr;
  const uint16_t port = static_cast<uint16_t>(7400 + domain * 250);

  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { std::printf("FAILED: socket\n"); return 1; }
  int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
    std::printf("FAILED: bind %u\n", port);
    return 1;
  }
  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = ::inet_addr("239.255.0.1");
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
  timeval tv{};
  tv.tv_sec = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  std::printf("抓取 domain %u 的 %u 端口，%d 秒...\n", domain, port, seconds);
  std::fflush(stdout);

  std::map<std::string, int> kinds;
  std::map<std::string, DataInfo> samples;   // 每类 DATA 保留一个样本
  std::vector<std::vector<uint8_t>> captured;
  size_t total = 0;

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    uint8_t buf[65536];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) continue;
    ++total;
    captured.emplace_back(buf, buf + n);

    if (n < 20 || std::memcmp(buf, "RTPS", 4) != 0) continue;
    const uint8_t ver_minor = buf[5];

    size_t p = 20;
    while (p + 4 <= static_cast<size_t>(n)) {
      const uint8_t sid = buf[p];
      const uint16_t olen = static_cast<uint16_t>(buf[p + 2]) |
                            (static_cast<uint16_t>(buf[p + 3]) << 8);
      if (sid == 0x15) {
        DataInfo info;
        if (ParseData(buf, static_cast<size_t>(n), p, ver_minor, &info)) {
          ++kinds[info.writer];
          if (samples.find(info.writer) == samples.end()) {
            samples[info.writer] = info;
          }
        }
      }
      if (olen == 0) break;
      p += 4 + olen;
    }
  }
  ::close(fd);

  std::printf("\n抓包总数 = %zu\n", total);
  std::printf("\n=== 按 DATA 的 writerId 分类 ===\n");
  for (const auto& kv : kinds) {
    std::printf("  %-14s x%d\n", kv.first.c_str(), kv.second);
  }

  std::printf("\n=== 每类 DATA 的 PID 列表（首个样本）===\n");
  for (const auto& kv : samples) {
    std::printf("\n[%s]  reader=%s\n", kv.first.c_str(), kv.second.reader.c_str());
    for (size_t i = 0; i < kv.second.pids.size(); ++i) {
      std::printf("    PID=0x%04x len=%3u val=%s\n",
                  kv.second.pids[i].first, kv.second.pids[i].second,
                  kv.second.pid_values[i].c_str());
    }
  }

  if (vec_out != nullptr && !captured.empty()) {
    std::ofstream out(vec_out, std::ios::binary);
    const uint32_t count = static_cast<uint32_t>(captured.size());
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& v : captured) {
      const uint32_t len = static_cast<uint32_t>(v.size());
      out.write(reinterpret_cast<const char*>(&len), sizeof(len));
      out.write(reinterpret_cast<const char*>(v.data()), len);
    }
    std::printf("\n已保存 %u 个包到 %s\n", count, vec_out);
  }
  return 0;
}
