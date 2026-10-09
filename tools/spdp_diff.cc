// 对比真实 Fast DDS 与 Stratum 的 SPDP 参数列表（PID 与值）
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <stratum/rtps/builtin/spdp_announcer.h>
#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/core/policy/ParameterList.hpp>
#include <stratum/rtps/dds/core/policy/ParameterTypes.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>

using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;
namespace dds = eprosima::fastdds::dds;

namespace {

// 解析一条 RTPS 消息，收集 PID → 值(hex) 映射
void CollectPids(const uint8_t* d, size_t n, std::map<uint16_t, std::string>* out,
                 const char* label) {
  std::printf("=== %s (%zu 字节) ===\n", label, n);
  if (n < 24) return;

  CDRMessage_t msg(static_cast<uint32_t>(n) + 8);
  std::memcpy(msg.buffer, d, n);
  msg.length = static_cast<uint32_t>(n);
  msg.pos = 20;
  msg.msg_endian = LITTLEEND;

  const uint8_t ver_minor = d[5];
  const size_t prefix = (ver_minor >= 3) ? 20 : 18;

  while (msg.pos + 4 <= msg.length) {
    const uint8_t id = msg.buffer[msg.pos];
    const uint16_t olen = static_cast<uint16_t>(msg.buffer[msg.pos + 2]) |
                          (static_cast<uint16_t>(msg.buffer[msg.pos + 3]) << 8);
    if (id != 0x15) {
      if (olen == 0) break;
      msg.pos += 4 + olen;
      continue;
    }
    size_t p = msg.pos + 4 + prefix;
    if (p + 4 > msg.length) return;
    std::printf("  encapsulation = %02x %02x\n", msg.buffer[p], msg.buffer[p + 1]);
    p += 4;
    while (p + 4 <= msg.length) {
      const uint16_t pid = static_cast<uint16_t>(msg.buffer[p]) |
                           (static_cast<uint16_t>(msg.buffer[p + 1]) << 8);
      const uint16_t plen = static_cast<uint16_t>(msg.buffer[p + 2]) |
                            (static_cast<uint16_t>(msg.buffer[p + 3]) << 8);
      std::string hex;
      for (uint16_t i = 0; i < plen && i < 8; ++i) {
        char b[4];
        std::snprintf(b, sizeof(b), "%02x", msg.buffer[p + 4 + i]);
        hex += b;
      }
      std::printf("  PID=0x%04x len=%3u val=%s\n", pid, plen, hex.c_str());
      (*out)[pid] = hex;
      p += 4 + ((plen + 3) & ~3u);
      if (pid == 0x0001) break;
    }
    break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::map<uint16_t, std::string> real, mine;

  // 真实 Fast DDS
  const char* path = (argc > 1) ? argv[1] : "/tmp/vec.bin";
  std::ifstream in(path, std::ios::binary);
  if (in) {
    uint32_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (count > 0) {
      uint32_t len = 0;
      in.read(reinterpret_cast<char*>(&len), sizeof(len));
      std::vector<uint8_t> buf(len);
      in.read(reinterpret_cast<char*>(buf.data()), len);
      CollectPids(buf.data(), buf.size(), &real, "真实 Fast DDS 公告");
    }
  } else {
    std::printf("(真实包文件不可读: %s)\n", path);
  }

  std::printf("\n");
  SpdpAnnounceConfig cfg;
  const uint8_t prefix[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                              0x54, 0x55, 0x4d, 0x00, 0x00, 0x01};
  std::memcpy(cfg.guid_prefix.value, prefix, 12);
  cfg.domain_id = 0;
  cfg.builtin_endpoints = 0x3f;
  cfg.participant_name = "stratum_recorder";
  cfg.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", 7410));
  cfg.default_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", 7411));
  const auto m = BuildSpdpAnnouncement(cfg);
  CollectPids(m.data(), m.size(), &mine, "Stratum 公告");

  std::printf("\n=== 差异分析 ===\n");
  if (!real.empty()) {
    std::printf("[真实包有、Stratum 没有 的 PID]\n");
    bool any = false;
    for (const auto& kv : real) {
      if (mine.find(kv.first) == mine.end()) {
        std::printf("  PID=0x%04x (真实值 %s)\n", kv.first, kv.second.c_str());
        any = true;
      }
    }
    if (!any) std::printf("  （无）\n");

    std::printf("[两边都有但值不同 的 PID]\n");
    any = false;
    for (const auto& kv : real) {
      auto it = mine.find(kv.first);
      if (it != mine.end() && it->second != kv.second) {
        std::printf("  PID=0x%04x 真实=%s 我方=%s\n", kv.first,
                    kv.second.c_str(), it->second.c_str());
        any = true;
      }
    }
    if (!any) std::printf("  （无）\n");
  }
  return 0;
}
