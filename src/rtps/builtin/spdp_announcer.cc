// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// SPDP 主动探测实现
//
// ===========================================================================
// 状态：**未完成，暂未纳入构建**（src/CMakeLists.txt 中已注释）
// ===========================================================================
//
// 已完成：字段顺序照抄上游 write_to_cdr_message、各 ParameterXxx_t 填充、
//        DATA 子消息与 RTPS 头组装、UDP 组播发送器。
// 未收敛：PL_CDR_LE 常量来源、Parameter_t sentinel 的模板参数、
//        SerializedPayload 的宏冲突。
//
// 解析侧（spdp_parser）**已完成并通过真实流量验证**，本文件是发送侧。
// 恢复构建前请先解决上述三处。


#include <stratum/rtps/builtin/spdp_announcer.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/core/policy/ParameterList.hpp>
#include <stratum/rtps/core/policy/ParameterSerializer.hpp>
#include <stratum/rtps/dds/core/policy/ParameterTypes.hpp>
#include <stratum/rtps/log/Log.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>

namespace stratum {
namespace rtps {

using namespace eprosima::fastdds::rtps;
namespace dds = eprosima::fastdds::dds;

namespace {

// SPDP 默认端口基址与域增益（RTPS 规范 9.6.1.1）
constexpr uint16_t kSpdpWellKnownPort = 7400;
constexpr uint16_t kDomainGain = 250;
// 域 0 的 SPDP 组播地址（RTPS 规范：239.255.0.1）
constexpr const char* kSpdpMulticastAddr = "239.255.0.1";
// 元流量 unicast locator 的 kind 取值
constexpr int32_t kLocatorKindUdpv4 = LOCATOR_KIND_UDPv4;

// 与上游 ParticipantProxyData 一致的固定长度常量
constexpr uint16_t kParamProtocolLength = 4;   // 实测真实包为 4
constexpr uint16_t kParamVendorLength = 4;     // 实测真实包为 4
constexpr uint16_t kParamProductVersionLength = 4;
constexpr uint16_t kParamGuidLength = 16;
constexpr uint16_t kParamNetworkConfigSetLength = 4;
constexpr uint16_t kParamBuiltinEndpointSetLength = 4;
constexpr uint16_t kParamLocatorLength = 24;

// RTPS 消息头（20 字节）
void WriteRtpsHeader(CDRMessage_t* msg, const GuidPrefix_t& prefix,
                     const VendorId_t& vendor, const ProtocolVersion_t& ver) {
  CDRMessage::addOctet(msg, 'R');
  CDRMessage::addOctet(msg, 'T');
  CDRMessage::addOctet(msg, 'P');
  CDRMessage::addOctet(msg, 'S');
  CDRMessage::addOctet(msg, ver.m_major);
  CDRMessage::addOctet(msg, ver.m_minor);
  CDRMessage::addOctet(msg, vendor[0]);
  CDRMessage::addOctet(msg, vendor[1]);
  for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
    CDRMessage::addOctet(msg, prefix.value[i]);
  }
}

// locator 的 UDPv4 地址填充（IPv4 放在 address 的最后 4 字节）
Locator_t MakeUdpv4Locator(const char* ip, uint16_t port) {
  Locator_t loc;
  loc.kind = kLocatorKindUdpv4;
  loc.port = port;
  const uint32_t a = ::inet_addr(ip);
  std::memcpy(&loc.address[12], &a, 4);
  return loc;
}

}  // namespace

std::vector<uint8_t> BuildSpdpAnnouncement(const SpdpAnnounceConfig& cfg) {
  CDRMessage_t msg(4096);
  msg.msg_endian = LITTLEEND;

  // --- RTPS 头 ---
  WriteRtpsHeader(&msg, cfg.guid_prefix, cfg.vendor_id, cfg.protocol_version);

  // --- INFO_TS 子消息（时间戳）---
  // 真实 Fast DDS 的 SPDP 消息含三个子消息：INFO_TS + INFO_DST + DATA。
  // 首轮 M2 验收失败即因只发了 DATA：Fast DDS 日志报
  //   "No Reader accepts this message (directed to: 0.0.0.1)"
  // —— 缺少 INFO_DST 时目标 prefix 未定义，接收侧拒绝投递。
  {
    CDRMessage::addOctet(&msg, INFO_TS);       // 0x09
    CDRMessage::addOctet(&msg, 0x01);          // bit0=1 → little endian
    CDRMessage::addUInt16(&msg, 8);            // 时间戳 8 字节
    // RTPS 时间戳 = seconds(int32) + fraction(uint32)，此处用 0（无效时间戳）
    CDRMessage::addInt32(&msg, 0);
    CDRMessage::addUInt32(&msg, 0);
  }

  // --- INFO_DST 子消息（目标 GUID prefix）---
  // SPDP 是组播广播：目标设为 unknown（全 0），表示"所有参与者"
  {
    CDRMessage::addOctet(&msg, INFO_DST);      // 0x0e
    CDRMessage::addOctet(&msg, 0x01);          // little endian
    CDRMessage::addUInt16(&msg, GuidPrefix_t::size);   // 12
    GuidPrefix_t unknown_prefix = GuidPrefix_t::unknown();
    for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
      CDRMessage::addOctet(&msg, unknown_prefix.value[i]);
    }
  }

  // --- DATA 子消息 ---
  // flags: bit0=1 (little endian) + bit2=1 (D, 含负载) → 0x05
  constexpr uint8_t kDataFlags = 0x05;
  CDRMessage::addOctet(&msg, DATA);
  CDRMessage::addOctet(&msg, kDataFlags);
  const uint32_t size_pos = msg.pos;
  CDRMessage::addUInt16(&msg, 0);          // octetsToNextHeader 占位
  const uint32_t body_start = msg.pos;

  // extraFlags(2) + octetsToInlineQos(2)
  //
  // 【关键】RTPS 2.3 起 DATA 子消息体首部多了 extraFlags 字段。
  // 本实现声明协议版本 2.4（>= 2.3），**必须**写上这 2 字节，
  // 否则接收方按 20 字节前缀解析会整体错位 —— 实测 Fast DDS 的
  // 反应是 willAReaderAcceptMsgDirectedTo 报
  //   "No Reader accepts this message (directed to: 0.0.0.1)"
  // （它从错位位置读出的 readerId 已不是 ENTITYID_UNKNOWN）。
  CDRMessage::addUInt16(&msg, 0);    // extraFlags
  // octetsToInlineQos：从其自身之后到 inlineQos 起点的字节数
  // = readerId(4) + writerId(4) + writerSN(8) = 16
  CDRMessage::addUInt16(&msg, 16);

  // readerId = ENTITYID_UNKNOWN（SPDP 是广播公告）
  EntityId_t unknown(ENTITYID_UNKNOWN);
  CDRMessage::addEntityId(&msg, &unknown);
  // writerId = SPDP 内建 Writer
  EntityId_t spdp_writer(ENTITYID_SPDP_BUILTIN_RTPSParticipant_WRITER);
  CDRMessage::addEntityId(&msg, &spdp_writer);
  // writerSN = 1
  SequenceNumber_t sn(0, 1);
  CDRMessage::addSequenceNumber(&msg, &sn);

  // --- serializedPayload: encapsulation + ParameterList ---
  // ParameterList::writeEncapsulationToCDRMsg 的**逐行内联**
  // （上游 src/cpp/fastdds/core/policy/ParameterList.cpp:33-40）。
  //
  // 不整体移植 ParameterList.cpp 的原因：该文件的其余三个函数
  // （updateCacheChangeFromInlineQos / read_guid_from_cdr_msg /
  //  readInstanceHandleFromCDRMsg）会引入 QosPolicies → attributes →
  // transport → flowcontrol 的**发散依赖链**（实测补齐 19 个文件后仍未收敛），
  // 而 SPDP 组网只需这 6 行。
  {
    bool valid = CDRMessage::addOctet(&msg, 0);
    valid &= CDRMessage::addOctet(
        &msg, static_cast<octet>(PL_CDR_LE - msg.msg_endian));
    valid &= CDRMessage::addUInt16(&msg, 0);
    if (!valid) {
      return {};
    }
  }

  // 以下字段顺序照抄 ParticipantProxyData::write_to_cdr_message
  // (ParticipantProxyData.cpp:273-380)
  {
    // length 用 4 —— 实测真实 Fast DDS 的 PID_PROTOCOL_VERSION 长度是 4
    // （值 2 字节 + 2 字节对齐填充），而非规范字面的 2。
    dds::ParameterProtocolVersion_t p(dds::PID_PROTOCOL_VERSION, kParamProtocolLength);
    p.protocolVersion = cfg.protocol_version;
    if (!dds::ParameterSerializer<dds::ParameterProtocolVersion_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }
  {
    // 同上：真实包中 PID_VENDORID 长度为 4
    dds::ParameterVendorId_t p(dds::PID_VENDORID, kParamVendorLength);
    p.vendorId[0] = cfg.vendor_id[0];
    p.vendorId[1] = cfg.vendor_id[1];
    if (!dds::ParameterSerializer<dds::ParameterVendorId_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }
  {
    dds::ParameterProductVersion_t p(dds::PID_PRODUCT_VERSION, kParamProductVersionLength);
    p.version.major = 3;
    p.version.minor = 6;
    p.version.patch = 2;
    p.version.tweak = 0;
    if (!dds::ParameterSerializer<dds::ParameterProductVersion_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }
  {
    dds::ParameterDomainId_t p(dds::PID_DOMAIN_ID, 4);
    p.domain_id = cfg.domain_id;
    if (!dds::ParameterSerializer<dds::ParameterDomainId_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }
  {
    GUID_t guid(cfg.guid_prefix, EntityId_t(ENTITYID_RTPSParticipant));
    dds::ParameterGuid_t p(dds::PID_PARTICIPANT_GUID, kParamGuidLength, guid);
    if (!dds::ParameterSerializer<dds::ParameterGuid_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }
  {
    dds::ParameterNetworkConfigSet_t p(dds::PID_NETWORK_CONFIGURATION_SET,
                                       kParamNetworkConfigSetLength);
    // 真实包该值为 1（IPv4 配置集），非 0
    p.netconfigSet = 1;
    if (!dds::ParameterSerializer<dds::ParameterNetworkConfigSet_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }

  // locators：metatraffic 与 default（unicast / multicast）
  auto write_locator = [&msg](const Locator_t& loc) -> bool {
    dds::ParameterLocator_t p(dds::PID_METATRAFFIC_UNICAST_LOCATOR, kParamLocatorLength);
    p.locator = loc;
    return dds::ParameterSerializer<dds::ParameterLocator_t>::add_to_cdr_message(p, &msg);
  };
  for (const Locator_t& loc : cfg.metatraffic_unicast) {
    if (!write_locator(loc)) return {};
  }
  for (const Locator_t& loc : cfg.default_unicast) {
    // default 用对应 PID 重写一次（保持与上游相同的两种 PID）
    dds::ParameterLocator_t p(dds::PID_DEFAULT_UNICAST_LOCATOR, kParamLocatorLength);
    p.locator = loc;
    if (!dds::ParameterSerializer<dds::ParameterLocator_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }

  {
    dds::ParameterBuiltinEndpointSet_t p(dds::PID_BUILTIN_ENDPOINT_SET,
                                         kParamBuiltinEndpointSetLength);
    p.endpointSet = cfg.builtin_endpoints;
    if (!dds::ParameterSerializer<dds::ParameterBuiltinEndpointSet_t>::add_to_cdr_message(
            p, &msg)) {
      return {};
    }
  }
  {
    dds::ParameterTime_t p(dds::PID_PARTICIPANT_LEASE_DURATION, 8);
    p.time = cfg.lease_duration;
    if (!dds::ParameterSerializer<dds::ParameterTime_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }
  if (!cfg.participant_name.empty()) {
    dds::ParameterString_t p(dds::PID_ENTITY_NAME, 0, cfg.participant_name);
    if (!dds::ParameterSerializer<dds::ParameterString_t>::add_to_cdr_message(p, &msg)) {
      return {};
    }
  }
  // PID_SENTINEL：上游由 ParameterSerializer<Parameter_t>::add_parameter_sentinel
  // 写入（ParameterSerializer.hpp:136-147），而非某个 ParameterXxx_t 结构。
  // 上游没有 ParameterSentinel 类型 —— 这是先前编译失败的第二个根因。
  if (!dds::ParameterSerializer<dds::Parameter_t>::add_parameter_sentinel(&msg)) {
    return {};
  }

  // --- 回填 octetsToNextHeader ---
  //
  // 【关键】必须**直接写 buffer**，不能用 CDRMessage::addUInt16 倒回 pos 写：
  // 后者的实现会 `msg->length += 2`（而非取 max(pos,length)），
  // 于是 length 从 244 变成 246，而 pos 恢复为 244 —— 消息尾部多出
  // 2 字节垃圾。接收方遍历子消息走到 244 时发现只剩 2 字节，
  // 报 "SubmessageHeader too short"，整条 SPDP 公告被丢弃。
  //
  // 上游的同类回填也是直接写 buffer
  // （见 messages/submessages/AckNackMsg.hpp、HeartbeatMsg.hpp 中
  //  `msg->buffer[submessage_size_pos] = *(o);` 的写法）。
  const uint16_t body_len = static_cast<uint16_t>(msg.pos - body_start);
  const octet* o = reinterpret_cast<const octet*>(&body_len);
  if (msg.msg_endian == DEFAULT_ENDIAN) {
    msg.buffer[size_pos] = o[0];
    msg.buffer[size_pos + 1] = o[1];
  } else {
    msg.buffer[size_pos] = o[1];
    msg.buffer[size_pos + 1] = o[0];
  }

  std::vector<uint8_t> out(msg.length);
  std::memcpy(out.data(), msg.buffer, msg.length);
  return out;
}

// ---------------------------------------------------------------------------
// UDP 发送器
// ---------------------------------------------------------------------------

SpdpAnnouncer::~SpdpAnnouncer() { Close(); }

bool SpdpAnnouncer::Open(const SpdpAnnounceConfig& cfg, std::string* error) {
  cfg_ = cfg;
  payload_ = BuildSpdpAnnouncement(cfg_);
  if (payload_.empty()) {
    last_error_ = "组装 SPDP 公告失败";
    if (error) *error = last_error_;
    return false;
  }

  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) {
    last_error_ = "socket() 失败";
    if (error) *error = last_error_;
    return false;
  }

  // 组播回环：让本机其他参与者（含 Fast DDS）能收到
  unsigned char loop = 1;
  ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
  unsigned char ttl = 1;
  ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

  if (error) error->clear();
  return true;
}

size_t SpdpAnnouncer::AnnounceOnce(std::string* error) {
  if (fd_ < 0 || payload_.empty()) {
    last_error_ = "未 Open";
    if (error) *error = last_error_;
    return 0;
  }

  const uint16_t port = static_cast<uint16_t>(
      kSpdpWellKnownPort + cfg_.domain_id * kDomainGain);

  // 组播 + 单播回环**双发**。
  //
  // 只发组播在本环境不可靠：WSL 有多张网卡（eth0 172.17.x.x /
  // 10.255.255.254 / lo），发送端未指定 IP_MULTICAST_IF 时由内核选路，
  // 可能与接收端加入组播组所用的接口不一致 —— 实测 domain 5 下
  // Fast DDS 的 PDP 层完全无活动（收不到），而它在 0.0.0.0:8650
  // 已绑定监听。
  //
  // 单播到 127.0.0.1 对同机参与者是可靠路径（RTPS 也支持通过
  // 初始对等点列表做单播发现）。两路都发，接收端去重即可。
  size_t total = 0;
  const char* targets[2] = {kSpdpMulticastAddr, "127.0.0.1"};
  for (const char* ip : targets) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = ::inet_addr(ip);
    const ssize_t n = ::sendto(fd_, payload_.data(), payload_.size(), 0,
                               reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    if (n > 0) {
      total += static_cast<size_t>(n);
    }
  }

  if (total == 0) {
    last_error_ = "sendto() 全部失败（组播与单播）";
    if (error) *error = last_error_;
    return 0;
  }
  ++sent_packets_;
  sent_bytes_ += total;
  if (error) error->clear();
  return total;
}

size_t SpdpAnnouncer::AnnounceFor(int seconds, int interval_ms) {
  size_t total = 0;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    total += AnnounceOnce(nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
  return total;
}

void SpdpAnnouncer::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

// 供测试使用：提供一个构造默认 locator 的便捷函数
Locator_t MakeDefaultUdpv4Locator(const char* ip, uint16_t port) {
  return MakeUdpv4Locator(ip, port);
}

}  // namespace rtps
}  // namespace stratum
