// 交叉验证消费者：抓取真实 Fast DDS 的 SPDP 公告包，
// 用 Stratum **移植的**解码器解析，验证线格式兼容性。
//
// 这是独立于 round-trip 的第二道验证：
//   round-trip 只能证明"自洽"；本程序证明"能读懂真实 Fast DDS 的字节"。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/common/Types.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>

using namespace eprosima::fastdds::rtps;

namespace {

// RTPS 消息头（20 字节）
struct RtpsHeader {
  char magic[4];
  uint8_t version_major;
  uint8_t version_minor;
  uint8_t vendor[2];
  uint8_t guid_prefix[12];
};

int failures = 0;
int checks = 0;

void check(bool cond, const std::string& what) {
  ++checks;
  if (!cond) {
    ++failures;
    std::printf("  [FAIL] %s\n", what.c_str());
  } else {
    std::printf("  [ok]   %s\n", what.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int duration_sec = (argc > 1) ? std::atoi(argv[1]) : 6;
  const char* out_path = (argc > 2) ? argv[2] : "/tmp/stratum_spdp_capture.bin";

  // 加入 SPDP 组播组（域 0：239.255.0.1:7400）
  int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    std::printf("FAILED: socket\n");
    return 1;
  }
  int reuse = 1;
  ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(7400);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::printf("FAILED: bind 7400 (需要 root 或端口空闲)\n");
    return 1;
  }
  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = ::inet_addr("239.255.0.1");
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  if (::setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    std::printf("WARN: 加入组播失败（继续，可能收到单播）\n");
  }

  timeval tv{};
  tv.tv_sec = 1;
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  std::printf("监听 SPDP 239.255.0.1:7400，持续 %d 秒...\n", duration_sec);
  std::fflush(stdout);

  std::vector<std::vector<uint8_t>> packets;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(duration_sec);
  while (std::chrono::steady_clock::now() < deadline) {
    uint8_t buf[65536];
    const ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
    if (n > 0) {
      packets.emplace_back(buf, buf + n);
    }
  }
  ::close(sock);

  std::printf("\n收到 %zu 个包\n", packets.size());
  if (packets.empty()) {
    std::printf("FAILED: 未收到任何 RTPS 包\n");
    return 2;
  }

  // 保存原始字节作为**固定测试向量**
  {
    std::ofstream out(out_path, std::ios::binary);
    const uint32_t count = static_cast<uint32_t>(packets.size());
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& p : packets) {
      const uint32_t len = static_cast<uint32_t>(p.size());
      out.write(reinterpret_cast<const char*>(&len), sizeof(len));
      out.write(reinterpret_cast<const char*>(p.data()), len);
    }
    std::printf("已保存测试向量: %s\n", out_path);
  }

  // ---------------------------------------------------------------------
  // 用移植的解码器解析真实包
  // ---------------------------------------------------------------------
  std::printf("\n=== 用 Stratum 移植解码器解析真实 RTPS 包 ===\n");

  int spdp_data_found = 0;
  int header_ok = 0;

  for (size_t i = 0; i < packets.size(); ++i) {
    const auto& raw = packets[i];
    if (raw.size() < sizeof(RtpsHeader)) continue;

    // --- RTPS 头 ---
    RtpsHeader hdr;
    std::memcpy(&hdr, raw.data(), sizeof(hdr));
    const bool magic_ok = (std::memcmp(hdr.magic, "RTPS", 4) == 0);
    if (!magic_ok) continue;
    ++header_ok;

    if (i == 0) {
      char name[5] = {hdr.magic[0], hdr.magic[1], hdr.magic[2], hdr.magic[3], 0};
      std::printf("报文 #0: 魔数=%s 版本=%u.%u vendor=%02x%02x guidPrefix=",
                  name, hdr.version_major, hdr.version_minor,
                  hdr.vendor[0], hdr.vendor[1]);
      for (int k = 0; k < 12; ++k) std::printf("%02x", hdr.guid_prefix[k]);
      std::printf("\n");

      check(magic_ok, "魔数 == \"RTPS\"");
      check(hdr.version_major == 2, "主版本 == 2");
      check(hdr.vendor[0] == 0x01 && hdr.vendor[1] == 0x0f,
            "vendorId == eProsima (01 0f)");
    }

    // --- 子消息遍历（用移植的 CDRMessage 读取器）---
    CDRMessage_t msg(static_cast<uint32_t>(raw.size()));
    std::memcpy(msg.buffer, raw.data(), raw.size());
    msg.length = static_cast<uint32_t>(raw.size());
    msg.pos = sizeof(RtpsHeader);

    int submsg_count = 0;
    while (msg.pos + 4 <= msg.length) {
      octet submsg_id = 0;
      octet flags = 0;
      uint16_t octets_to_next = 0;
      if (!CDRMessage::readOctet(&msg, &submsg_id)) break;
      if (!CDRMessage::readOctet(&msg, &flags)) break;
      if (!CDRMessage::readUInt16(&msg, &octets_to_next)) break;

      // 步进到下一个子消息
      const uint32_t next_pos = msg.pos + octets_to_next;
      ++submsg_count;

      // DATA 子消息（0x15）→ SPDP 的参与者公告
      if (submsg_id == 0x15) {
        ++spdp_data_found;
        if (spdp_data_found == 1 && i == 0) {
          std::printf("  发现 DATA 子消息 (id=0x%02x flags=0x%02x"
                      " octetsToNext=%u)\n",
                      static_cast<unsigned>(submsg_id),
                      static_cast<unsigned>(flags), octets_to_next);
        }
      }

      if (next_pos <= msg.pos && octets_to_next == 0) {
        // octetsToNextHeader==0 在 DATA 中表示"直到消息末尾"
        break;
      }
      if (next_pos > msg.length) break;
      msg.pos = next_pos;
    }

    if (i == 0) {
      std::printf("  子消息个数 = %d\n", submsg_count);
      check(submsg_count > 0, "至少解析出 1 个子消息");
    }
  }

  std::printf("\n含有效 RTPS 头的包: %d/%zu\n", header_ok, packets.size());

  // 全局断言
  check(header_ok > 0, "存在有效的 RTPS 报文");
  check(spdp_data_found > 0, "解析出 SPDP 的 DATA 子消息");

  std::printf("\n检查项 %d，失败 %d\n", checks, failures);
  return failures == 0 ? 0 : 3;
}
