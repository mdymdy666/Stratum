// DATA 子消息字节级对比：Stratum 生成 vs 真实 Fast DDS
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <stratum/rtps/builtin/spdp_announcer.h>

using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;

namespace {

void HexDump(const uint8_t* d, size_t n, const char* label, size_t maxb = 48) {
  std::printf("--- %s (%zu 字节) ---\n  ", label, n);
  for (size_t i = 0; i < n && i < maxb; ++i) {
    std::printf("%02x ", d[i]);
    if ((i + 1) % 16 == 0) std::printf("\n  ");
  }
  std::printf("\n");
}

// 定位并打印 DATA 子消息的固定字段（前缀 18 字节）
void ShowDataFields(const uint8_t* d, size_t n, const char* label) {
  std::printf("--- %s ---\n", label);
  if (n < 24) { std::printf("  太短\n"); return; }
  size_t p = 20;   // 跳过 RTPS 头
  while (p + 4 <= n) {
    const uint8_t id = d[p];
    const uint8_t flags = d[p + 1];
    const uint16_t olen = static_cast<uint16_t>(d[p + 2]) |
                          (static_cast<uint16_t>(d[p + 3]) << 8);
    std::printf("  submsg id=0x%02x flags=0x%02x len=%u\n", id, flags, olen);
    if (id == 0x15) {
      const size_t q = p + 4;
      if (q + 18 > n) { std::printf("  DATA 太短\n"); return; }
      std::printf("    octetsToInlineQos=%u\n",
                  static_cast<unsigned>(d[q]) | (static_cast<unsigned>(d[q + 1]) << 8));
      std::printf("    readerId = %02x %02x %02x %02x\n",
                  d[q + 2], d[q + 3], d[q + 4], d[q + 5]);
      std::printf("    writerId = %02x %02x %02x %02x\n",
                  d[q + 6], d[q + 7], d[q + 8], d[q + 9]);
      std::printf("    writerSN = %02x %02x %02x %02x %02x %02x %02x %02x\n",
                  d[q + 10], d[q + 11], d[q + 12], d[q + 13],
                  d[q + 14], d[q + 15], d[q + 16], d[q + 17]);
      return;
    }
    if (olen == 0) return;
    p += 4 + olen;
  }
}

}  // namespace

int main(int argc, char** argv) {
  SpdpAnnounceConfig cfg;
  const uint8_t prefix[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                              0x54, 0x55, 0x4d, 0x00, 0x00, 0x01};
  std::memcpy(cfg.guid_prefix.value, prefix, 12);
  cfg.builtin_endpoints = 0x3f;
  cfg.participant_name = "stratum_recorder";
  cfg.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", 7410));
  cfg.default_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", 7411));

  const auto mine = BuildSpdpAnnouncement(cfg);
  ShowDataFields(mine.data(), mine.size(), "Stratum 生成的公告");
  HexDump(mine.data(), mine.size(), "Stratum 头部+前 48 字节");

  const char* path = (argc > 1) ? argv[1] : "tests/vectors/spdp_capture.bin";
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::printf("\n(向量文件不可读: %s)\n", path);
    return 0;
  }
  uint32_t count = 0;
  in.read(reinterpret_cast<char*>(&count), sizeof(count));
  std::printf("\n向量文件含 %u 个包\n", count);
  if (count == 0) return 0;
  uint32_t len = 0;
  in.read(reinterpret_cast<char*>(&len), sizeof(len));
  std::vector<uint8_t> buf(len);
  in.read(reinterpret_cast<char*>(buf.data()), len);
  ShowDataFields(buf.data(), buf.size(), "真实 Fast DDS 公告");
  HexDump(buf.data(), buf.size(), "真实 Fast DDS 头部+前 48 字节");
  return 0;
}
