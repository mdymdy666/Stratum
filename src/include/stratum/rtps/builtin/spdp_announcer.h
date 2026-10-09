// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// SPDP 主动探测：组装并发送 Stratum 自己的参与者公告
//
// ===========================================================================
// 移植说明
// ===========================================================================
//
// 报文字段顺序**照抄**上游 ParticipantProxyData::write_to_cdr_message
// (src/cpp/rtps/builtin/data/ParticipantProxyData.cpp:273 起)：
//
//   writeEncapsulationToCDRMsg → PID_PROTOCOL_VERSION → PID_VENDORID →
//   PID_PRODUCT_VERSION → PID_DOMAIN_ID → PID_PARTICIPANT_GUID →
//   PID_NETWORK_CONFIGURATION_SET → (locators) → PID_BUILTIN_ENDPOINT_SET →
//   PID_PARTICIPANT_LEASE_DURATION → PID_SENTINEL
//
// 序列化全部复用已移植的 ParameterSerializer<>::add_to_cdr_message
// 与 ParameterList::writeEncapsulationToCDRMsg —— 本文件只负责：
//   1. 按上游顺序填充各 ParameterXxx_t 结构
//   2. 在其外包上 DATA 子消息与 RTPS 头（复用已移植的 CDRMessage 原语）
//   3. UDP 组播发送

#ifndef STRATUM_RTPS_BUILTIN_SPDP_ANNOUNCER_H_
#define STRATUM_RTPS_BUILTIN_SPDP_ANNOUNCER_H_

#include <cstdint>
#include <string>
#include <vector>

#include <stratum/rtps/common/Guid.hpp>
#include <stratum/rtps/common/GuidPrefix_t.hpp>
#include <stratum/rtps/common/Locator.hpp>
#include <stratum/rtps/common/Time_t.hpp>
#include <stratum/rtps/common/VendorId_t.hpp>

namespace stratum {
namespace rtps {

// Stratum 参与者的公告配置
struct SpdpAnnounceConfig {
  eprosima::fastdds::rtps::GuidPrefix_t guid_prefix;                 // 本参与者 GUID 前缀
  uint32_t domain_id = 0;
  eprosima::fastdds::rtps::VendorId_t vendor_id{{0x01, 0x0f}};
  eprosima::fastdds::rtps::ProtocolVersion_t protocol_version{2, 4};
  eprosima::fastdds::rtps::Time_t lease_duration{20, 0};
  uint32_t builtin_endpoints = 0;           // BuiltinEndpointSet
  std::string participant_name;

  // 元流量单播 locator（Stratum 监听 ACKNACK/Heartbeat 的地址）
  std::vector<eprosima::fastdds::rtps::Locator_t> metatraffic_unicast;
  // 用户流量单播 locator
  std::vector<eprosima::fastdds::rtps::Locator_t> default_unicast;
};

// 构造一个 UDPv4 locator（测试与配置便利函数）
eprosima::fastdds::rtps::Locator_t MakeDefaultUdpv4Locator(const char* ip,
                                                          uint16_t port);

// 把 SPDP 公告组装成完整的 RTPS 消息字节流
std::vector<uint8_t> BuildSpdpAnnouncement(const SpdpAnnounceConfig& cfg);

// UDP 组播发送器（SPDP 默认地址 239.255.0.1:7400 + domain*250）
class SpdpAnnouncer {
 public:
  SpdpAnnouncer() = default;
  ~SpdpAnnouncer();

  SpdpAnnouncer(const SpdpAnnouncer&) = delete;
  SpdpAnnouncer& operator=(const SpdpAnnouncer&) = delete;

  // 打开 UDP socket 并加入组播组（同时开启组播回环以便本机 Fast DDS 收到）
  bool Open(const SpdpAnnounceConfig& cfg, std::string* error);

  // 发送一次公告；返回实际发送字节数（0 表示失败）
  size_t AnnounceOnce(std::string* error);

  // 周期发送（阻塞 seconds 秒，每 interval_ms 发一次）
  size_t AnnounceFor(int seconds, int interval_ms);

  size_t sent_packets() const { return sent_packets_; }
  size_t sent_bytes() const { return sent_bytes_; }
  const std::string& last_error() const { return last_error_; }

  void Close();

 private:
  int fd_ = -1;
  SpdpAnnounceConfig cfg_;
  std::vector<uint8_t> payload_;
  size_t sent_packets_ = 0;
  size_t sent_bytes_ = 0;
  std::string last_error_;
};

}  // namespace rtps
}  // namespace stratum

#endif  // STRATUM_RTPS_BUILTIN_SPDP_ANNOUNCER_H_
