// M2 验收：Stratum 发送 SPDP 公告
//
// 与 tools/xcheck_listener.cc 配合运行：
//   listener（真实 Fast DDS）应报告发现 Stratum 的参与者。

#include <cstdio>
#include <cstring>
#include <string>

#include <stratum/rtps/builtin/spdp_announcer.h>

using namespace stratum::rtps;

namespace {

// Fast DDS 的内建端点集取值（来自 RTPS 规范 / BuiltinEndpointSet_t）
// 这里只置 SPDP + SEDP 相关位，表示"我是参与者并具备发现能力"
// 取真实 Fast DDS 公告中的实测值 0x000f0c3f：
//   低 6 位 = PARTICIPANT/PUBLICATIONS/SUBSCRIPTIONS 的 announcer+detector
//   bit 10,11 = 安全端点
//   bit 16-19 = TypeLookup 服务
constexpr uint32_t kBuiltinEndpoints = 0x000f0c3f;

}  // namespace

int main(int argc, char** argv) {
  const int seconds = (argc > 1) ? std::atoi(argv[1]) : 8;
  const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;

  SpdpAnnounceConfig cfg;
  // GUID prefix 的布局由 RTPS 规范 9.3.1.1 规定：
  //   vendorId(2) | hostId(4) | processId(4) | counter(2)
  // **前 2 字节必须等于本参与者的 vendorId**，否则对端会把它当成
  // 未知厂商实现而拒绝（M2 首轮验收即因此失败：曾用 "ST" 开头）。
  // 这里沿用 eProsima 的 vendor id 以表示与本实现的兼容性，
  // 后 10 字节用 "STRATUM" 的 ASCII 便于在抓包中辨认。
  const uint8_t prefix[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                              0x54, 0x55, 0x4d, 0x00, 0x00, 0x01};
  std::memcpy(cfg.guid_prefix.value, prefix, sizeof(prefix));
  cfg.domain_id = domain;
  cfg.builtin_endpoints = kBuiltinEndpoints;
  cfg.lease_duration = eprosima::fastdds::rtps::Time_t(20, 0);
  cfg.participant_name = "stratum_recorder";

  // 元流量单播 locator：Stratum 监听端口（域 0 → 7410，避开 Fast DDS 默认）
  const uint16_t meta_port = static_cast<uint16_t>(7410 + domain * 250);
  cfg.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", meta_port));
  cfg.default_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", meta_port + 1));

  SpdpAnnouncer announcer;
  std::string err;
  if (!announcer.Open(cfg, &err)) {
    std::printf("FAILED: Open: %s\n", err.c_str());
    return 1;
  }

  std::printf("Stratum SPDP 发送中：domain=%u guidPrefix=5354524154554d0000000001"
              " 持续 %d 秒...\n", domain, seconds);
  std::fflush(stdout);

  const size_t sent = announcer.AnnounceFor(seconds, 100);

  std::printf("完成：packets=%zu bytes=%zu\n",
              announcer.sent_packets(), announcer.sent_bytes());
  if (sent == 0) {
    std::printf("FAILED: 未发出任何包\n");
    return 2;
  }
  return 0;
}
