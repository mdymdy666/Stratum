// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// DATA_FRAG 接收侧健壮性注入工具
//
// 用途：验证 RtpsReader 的分片重组在"异常输入"下不会崩、不会泄漏、
//       不会把不完整的样本错误交付。三种模式：
//
//   partial   —— 只发一个样本的前 N 片（永远收不齐）→ 验证超时淘汰
//   overflow  —— 为大量不同 SN 各发 1 片 → 验证缓存上限与内存不涨
//   malformed —— 故意构造畸形字段（fragmentSize=0、sampleSize=0、
//                fragmentsInSubmessage 极大、起始号越界）→ 验证不崩
//
// 基础分片格式复用 packet::BuildDataFragPackets，保证与真实发送路径一致；
// malformed 模式才手工改字段。
//
// 用法：
//   frag_fuzz <mode> <count> [--dst=IP:PORT] [--domain=D] [--payload=B]
// 例：
//   frag_fuzz partial  20 --dst=127.0.0.1:9351 --domain=30
//   frag_fuzz overflow 500 --dst=127.0.0.1:9351 --domain=30

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "stratum/protocol/rtps/rtps_packet_utils.h"

namespace pkt = stratum::protocol::rtps::packet;

namespace {

// 与 RtpsWriter 相同的 GUID 前缀（前 12 字节）
uint8_t g_prefix[12] = {0x01, 0x0f, 0x01, 0x01, 0x7f, 0x00,
                        0x59, 0x04, 0x00, 0x00, 0x00, 0x00};

// 构造一个 DataPacketParams
pkt::DataPacketParams MakeParams(uint64_t sn, size_t payload_len) {
  pkt::DataPacketParams p;
  std::memcpy(p.prefix, g_prefix, 12);
  p.writer_entity = 0x00000103;
  p.sn = sn;
  p.payload.assign(payload_len, 'F');
  for (size_t i = 0; i < payload_len; ++i) {
    p.payload[i] = static_cast<char>(i & 0xFF);
  }
  return p;
}

// DATA_FRAG 固定字段在子消息体内的偏移（自子消息体起点）
// extraFlags(2) octetsToInlineQos(2) readerId(4) writerId(4) writerSN(8)
// fragmentStartingNum(4) fragmentsInSubmessage(2) fragmentSize(2) sampleSize(4)
constexpr size_t kBodyBase = 24;   // 跳过 RTPS 头(20) + 子消息头(4)

void PatchU16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
  b[off] = static_cast<uint8_t>(v & 0xFF);
  b[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void PatchU32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    b[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "用法: frag_fuzz <partial|overflow|malformed> <count> "
                 "[--dst=IP:PORT] [--domain=D] [--payload=B]\n");
    return 2;
  }
  const std::string mode = argv[1];
  const int count = std::atoi(argv[2]);

  std::string dst_ip = "127.0.0.1";
  uint16_t dst_port = 9351;
  uint32_t domain = 0;
  size_t payload_len = 8000;

  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--dst=", 0) == 0) {
      const std::string v = a.substr(6);
      const size_t c = v.find(':');
      dst_ip = v.substr(0, c);
      dst_port = static_cast<uint16_t>(std::atoi(v.substr(c + 1).c_str()));
    } else if (a.rfind("--domain=", 0) == 0) {
      domain = static_cast<uint32_t>(std::atoi(a.substr(9).c_str()));
    } else if (a.rfind("--payload=", 0) == 0) {
      payload_len = static_cast<size_t>(std::atoi(a.substr(10).c_str()));
    }
  }
  (void)domain;

  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    std::perror("socket");
    return 1;
  }
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(dst_port);
  if (::inet_pton(AF_INET, dst_ip.c_str(), &dst.sin_addr) != 1) {
    std::fprintf(stderr, "非法地址: %s\n", dst_ip.c_str());
    return 2;
  }

  constexpr uint16_t kFragPayload = 1416;   // 1472 - 56
  size_t sent = 0;

  for (int i = 0; i < count; ++i) {
    const uint64_t sn = 1000 + static_cast<uint64_t>(i);
    pkt::DataPacketParams p = MakeParams(sn, payload_len);
    auto frags = pkt::BuildDataFragPackets(p, kFragPayload);
    if (frags.empty()) {
      std::fprintf(stderr, "构造分片失败\n");
      return 1;
    }

    if (mode == "partial") {
      // 只发第 1 片 —— 该样本永远收不齐，用于验证超时淘汰
      ::sendto(fd, frags[0].data(), frags[0].size(), 0,
               reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
      ++sent;
    } else if (mode == "overflow") {
      // 每个 SN 只发 1 片：制造大量"不完整的重组缓冲"
      ::sendto(fd, frags[0].data(), frags[0].size(), 0,
               reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
      ++sent;
    } else if (mode == "malformed") {
      // 依次尝试几种畸形字段；每个都用独立的 SN
      std::vector<uint8_t> b = frags[0];
      switch (i % 5) {
        case 0:
          PatchU16(b, kBodyBase + 26, 0);            // fragmentSize = 0
          break;
        case 1:
          PatchU32(b, kBodyBase + 28, 0);            // sampleSize = 0
          break;
        case 2:
          PatchU16(b, kBodyBase + 24, 0xFFFF);       // fragmentsInSubmessage 极大
          break;
        case 3:
          PatchU32(b, kBodyBase + 20, 0xFFFFFF00u);  // fragmentStartingNum 越界
          break;
        default:
          PatchU32(b, kBodyBase + 28, 0xFFFFFFFFu);  // sampleSize 极大
          break;
      }
      ::sendto(fd, b.data(), b.size(), 0,
               reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
      ++sent;
    } else {
      std::fprintf(stderr, "未知模式: %s\n", mode.c_str());
      ::close(fd);
      return 2;
    }
  }

  ::close(fd);
  std::printf("[frag_fuzz] mode=%s 已发送 %zu 个报文 -> %s:%u (payload=%zu)\n",
              mode.c_str(), sent, dst_ip.c_str(),
              static_cast<unsigned>(dst_port), payload_len);
  std::fflush(stdout);
  return 0;
}
