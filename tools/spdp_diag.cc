// SPDP 公告诊断：对比 Stratum 自己的公告与真实 Fast DDS 公告的 PID 结构
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <stratum/rtps/builtin/spdp_announcer.h>
#include <stratum/rtps/builtin/spdp_parser.h>
#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>

using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;

namespace {

// 打印一条 RTPS 消息的 PID 序列（用于结构对比）
void DumpPids(const uint8_t* data, size_t len, const char* label) {
  std::printf("--- %s (%zu 字节) ---\n", label, len);
  if (len < 20) { std::printf("  太短\n"); return; }
  std::printf("  header: magic=%.4s ver=%u.%u vendor=%02x%02x\n", data, data[4], data[5],
              data[6], data[7]);

  CDRMessage_t msg(static_cast<uint32_t>(len) + 8);
  std::memcpy(msg.buffer, data, len);
  msg.length = static_cast<uint32_t>(len);
  msg.pos = 20;
  msg.msg_endian = LITTLEEND;

  // 子消息遍历
  while (msg.pos + 4 <= msg.length) {
    const uint8_t id = msg.buffer[msg.pos];
    const uint8_t flags = msg.buffer[msg.pos + 1];
    const uint16_t olen = static_cast<uint16_t>(msg.buffer[msg.pos + 2]) |
                          (static_cast<uint16_t>(msg.buffer[msg.pos + 3]) << 8);
    std::printf("  submsg id=0x%02x flags=0x%02x len=%u\n", id, flags, olen);
    if (id != 0x15) {           // 只看 DATA
      if (olen == 0) break;
      msg.pos += 4 + olen;
      continue;
    }
    // DATA：跳过 16 字节 inlineQos 前缀
    // DATA 子消息体前缀 = octetsToInlineQos(2) + readerId(4)
    //                     + writerId(4) + writerSN(8) = 18
    uint32_t p = msg.pos + 4 + 18;
    if (p + 4 > msg.length) break;
    // encapsulation
    std::printf("    encap=%02x%02x\n", msg.buffer[p], msg.buffer[p + 1]);
    p += 4;
    // PID 列表
    int n = 0;
    while (p + 4 <= msg.length && n < 40) {
      const uint16_t pid = static_cast<uint16_t>(msg.buffer[p]) |
                           (static_cast<uint16_t>(msg.buffer[p + 1]) << 8);
      const uint16_t plen = static_cast<uint16_t>(msg.buffer[p + 2]) |
                            (static_cast<uint16_t>(msg.buffer[p + 3]) << 8);
      std::printf("    PID=0x%04x len=%u\n", pid, plen);
      p += 4 + ((plen + 3) & ~3u);
      ++n;
      if (pid == 0x0001) break;   // SENTINEL
    }
    break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  // 1) Stratum 生成的公告
  SpdpAnnounceConfig cfg;
  const uint8_t prefix[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                              0x54, 0x55, 0x4d, 0x00, 0x00, 0x01};
  std::memcpy(cfg.guid_prefix.value, prefix, 12);
  cfg.builtin_endpoints = 0x3f;
  cfg.participant_name = "stratum_recorder";
  cfg.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", 7410));
  cfg.default_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", 7411));

  const auto mine = BuildSpdpAnnouncement(cfg);
  DumpPids(mine.data(), mine.size(), "Stratum 生成的公告");

  // 自洽验证：用我们自己的解析器解析
  {
    SpdpParser parser;
    std::vector<SpdpParticipantData> out;
    const size_t n = parser.ParseMessage(mine.data(), mine.size(), &out);
    std::printf("\n自解析: participants=%zu", n);
    if (n > 0) {
      std::printf(" guid=%02x%02x... vendor=%02x%02x lease=%d builtin=0x%x locators=%zu\n",
                  out[0].guid.guidPrefix.value[0], out[0].guid.guidPrefix.value[1],
                  out[0].vendor_id[0], out[0].vendor_id[1],
                  out[0].lease_duration.seconds(), out[0].builtin_endpoints,
                  out[0].metatraffic_unicast_locators.size() +
                  out[0].default_unicast_locators.size());
    } else {
      std::printf("  ** 自解析失败 **\n");
    }
  }

  // 2) 真实 Fast DDS 公告
  const char* path = (argc > 1) ? argv[1] : "tests/vectors/spdp_capture.bin";
  std::ifstream in(path, std::ios::binary);
  if (in) {
    uint32_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (count > 0) {
      uint32_t len = 0;
      in.read(reinterpret_cast<char*>(&len), sizeof(len));
      std::vector<uint8_t> buf(len);
      in.read(reinterpret_cast<char*>(buf.data()), len);
      std::printf("\n");
      DumpPids(buf.data(), buf.size(), "真实 Fast DDS 公告");
    }
  } else {
    std::printf("\n(向量文件 %s 不可读)\n", path);
  }
  return 0;
}
