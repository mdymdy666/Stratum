// 文件：tests/manual/test_data_frag.cc
// 用途：验证 DATA_FRAG 分片报文的字节级格式（对照 RTPS 2.3 §9.6.3.6）
//
// 为什么单独做这个测试：上一轮把分片直接接进 Publish，端到端表现为
// "匹配建立又被取消、已发布样本=0"，但无法判断是分片构造错了还是接线错了。
// 这个测试只回答一个问题：**分片报文本身是否合规**。
//
// 编译（不接入 ctest，便于快速迭代）：
//   g++ -std=c++17 -o /tmp/t_frag tests/manual/test_data_frag.cc \
//       -I src/include -I build/generated \
//       -I /UserData/LeaningLevelDB/LSMengine/include \
//       -I /opt/ros/jazzy/include/fastcdr \
//       build/src/libstratum_rtps_protocol.a ... -lfastcdr -lpthread
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "stratum/protocol/rtps/rtps_packet_utils.h"

using namespace stratum::protocol::rtps::packet;

static int g_fail = 0;
static void check(bool ok, const char* what) {
  std::printf("  %-58s %s\n", what, ok ? "OK" : "FAIL");
  if (!ok) ++g_fail;
}

int main() {
  const size_t payload_len = 8000;
  const uint16_t frag_size = 1416;   // 1472(MTU上限) - 56(DATA_FRAG固定开销)

  DataPacketParams p;
  for (int i = 0; i < 12; ++i) p.prefix[i] = static_cast<uint8_t>(0x10 + i);
  p.writer_entity = 0x00000103;
  p.sn = 42;
  p.payload.assign(payload_len, 'A');
  for (size_t i = 0; i < payload_len; ++i) {
    p.payload[i] = static_cast<char>(i & 0xFF);
  }

  auto frags = BuildDataFragPackets(p, frag_size);
  const size_t expect_n = (payload_len + frag_size - 1) / frag_size;

  std::printf("=== DATA_FRAG 构造检查（payload=%zu, fragmentSize=%u）===\n",
              payload_len, frag_size);
  std::printf("  期望片数 = %zu，实际 = %zu\n", expect_n, frags.size());
  check(frags.size() == expect_n, "片数正确");

  if (frags.empty()) {
    std::printf("  分片为空 —— 后续检查无法进行\n");
    return 1;
  }

  // 逐片检查
  for (size_t i = 0; i < frags.size(); ++i) {
    const auto& b = frags[i];
    const uint8_t* q = b.data();
    const size_t n = b.size();

    // RTPS 头 20 字节：'RTPS' + version + vendor + guidPrefix
    check(std::memcmp(q, "RTPS", 4) == 0, "RTPS 魔数");
    check(std::memcmp(q + 8, p.prefix, 12) == 0, "guidPrefix 一致");

    // 子消息头：id=0x16, flags, octetsToNextHeader（16位）
    check(q[20] == 0x16, "子消息 id = DATA_FRAG(0x16)");
    const uint16_t o2n = static_cast<uint16_t>(q[22] | (q[23] << 8));   // 小端
    check(static_cast<size_t>(o2n) == n - 24, "octetsToNextHeader = 子消息体长度");

    // 体：extraFlags(2) octetsToInlineQos(2) readerId(4) writerId(4) writerSN(8)
    //     fragmentStartingNum(4) fragmentsInSubmessage(2) fragmentSize(2) sampleSize(4)
    const uint8_t* body = q + 24;
    const uint16_t o2iq = static_cast<uint16_t>(body[2] | (body[3] << 8));  // 小端
    check(o2iq == 28, "octetsToInlineQos = 28");

    // EntityId 在 RTPS 报文中是**大端**（规范规定 value[3] 先写）
    uint32_t wid = (static_cast<uint32_t>(body[8]) << 24) |
                   (static_cast<uint32_t>(body[9]) << 16) |
                   (static_cast<uint32_t>(body[10]) << 8) |
                   static_cast<uint32_t>(body[11]);
    check(wid == 0x00000103, "writerId 正确（大端）");

    uint64_t sn = 0;
    {
      uint32_t hi = 0, lo = 0;
      std::memcpy(&hi, body + 12, 4);
      std::memcpy(&lo, body + 16, 4);
      sn = (static_cast<uint64_t>(hi) << 32) | lo;
    }
    check(sn == 42, "writerSN 正确（所有分片共用）");

    uint32_t start = 0;
    uint16_t in_sub = 0, fsz = 0;
    uint32_t sample = 0;
    std::memcpy(&start, body + 20, 4);
    std::memcpy(&in_sub, body + 24, 2);
    std::memcpy(&fsz, body + 26, 2);
    std::memcpy(&sample, body + 28, 4);
    check(start == i + 1, "fragmentStartingNum 从 1 递增");
    check(in_sub == 1, "fragmentsInSubmessage = 1");
    check(fsz == frag_size, "fragmentSize = 请求值");
    check(sample == payload_len, "sampleSize = 完整样本长度");

    // 负载内容
    const size_t off = i * frag_size;
    const size_t len = n - 24 - 32;   // 减去子消息头(4)与固定字段(32)? 见下
    (void)len;
    const uint8_t* pl = body + 32;    // extraFlags..sampleSize 共 32 字节
    const size_t pl_len = static_cast<size_t>(o2n) - 32;
    const size_t want = (off + frag_size <= payload_len) ? frag_size
                                                         : payload_len - off;
    check(pl_len == want, "本片负载长度正确");
    bool content_ok = true;
    for (size_t k = 0; k < pl_len && k < want; ++k) {
      if (pl[k] != static_cast<uint8_t>((off + k) & 0xFF)) { content_ok = false; break; }
    }
    check(content_ok, "本片负载内容与源一致（按偏移）");

    if (i == 0 || i + 1 == frags.size()) {
      std::printf("    [片 %zu] 报文 %zu 字节，负载 %zu 字节，起始 %u\n",
                  i + 1, n, pl_len, start);
    }
  }

  std::printf("\n结果：%s（失败项 %d）\n", g_fail == 0 ? "PASS" : "FAIL", g_fail);
  return g_fail == 0 ? 0 : 2;
}
