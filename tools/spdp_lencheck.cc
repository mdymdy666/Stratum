// SPDP 公告长度自洽性检查
// 遍历所有子消息，验证 4 + octetsToNextHeader 是否恰好走到消息末尾。
// 若不能恰好走完，Fast DDS 会在 readSubmessageHeader 报
// "SubmessageHeader too short"。
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <stratum/rtps/builtin/spdp_announcer.h>

using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;

int main(int argc, char** argv) {
  SpdpAnnounceConfig cfg;
  const uint8_t prefix[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                              0x54, 0x55, 0x4d, 0x00, 0x00, 0x01};
  std::memcpy(cfg.guid_prefix.value, prefix, 12);
  cfg.domain_id = 5;
  cfg.builtin_endpoints = 0x3f;
  cfg.participant_name = "stratum_recorder";
  cfg.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", 9050));
  cfg.default_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", 9051));

  const auto m = BuildSpdpAnnouncement(cfg);
  std::printf("总长度 = %zu\n\n", m.size());

  size_t p = 20;   // 跳过 RTPS 头
  int idx = 0;
  bool ok = true;
  while (p < m.size()) {
    if (p + 4 > m.size()) {
      std::printf("  [错误] 位置 %zu 处剩余 %zu 字节 —— 不足子消息头（%s）\n",
                  p, m.size() - p, "SubmessageHeader too short");
      ok = false;
      break;
    }
    const uint8_t id = m[p];
    const uint8_t flags = m[p + 1];
    const uint16_t olen = static_cast<uint16_t>(m[p + 2]) |
                          (static_cast<uint16_t>(m[p + 3]) << 8);
    const size_t next = p + 4 + olen;
    std::printf("  子消息 #%d: id=0x%02x flags=0x%02x octetsToNext=%u "
                "→ 体[%zu,%zu) 下一个=%zu%s\n",
                idx, id, flags, olen, p + 4, next, next,
                next > m.size() ? "  ** 越界 **" : "");
    if (next > m.size()) {
      ok = false;
      break;
    }
    if (olen == 0 && id != 0x09 && id != 0x01) {
      std::printf("      （octetsToNext=0 → 视为最后一个子消息）\n");
      break;
    }
    p = next;
    ++idx;
  }

  std::printf("\n遍历结束位置 = %zu / 总长 %zu\n", p, m.size());
  std::printf("判定：%s\n", ok && p == m.size()
                ? "自洽 ✓（恰好走到末尾）"
                : (p == m.size() ? "走到末尾但中间有错" : "** 不自洽 —— 会触发 SubmessageHeader too short **"));

  // 保存字节供 Python 侧复核
  if (argc > 1) {
    std::ofstream out(argv[1], std::ios::binary);
    out.write(reinterpret_cast<const char*>(m.data()),
              static_cast<std::streamsize>(m.size()));
    std::printf("已保存: %s\n", argv[1]);
  }
  return ok && p == m.size() ? 0 : 1;
}
