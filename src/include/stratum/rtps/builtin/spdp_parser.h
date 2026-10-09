// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// SPDP 发现解析器（M2）
//
// ===========================================================================
// 移植基础
// ===========================================================================
//
// ParameterList 的遍历在 Fast DDS 里是 header-only 模板
// （core/policy/ParameterList.hpp:readParameterListfromCDRMsg L73-150，
//  已随 53 个 IDENTICAL 文件一并移植），本文件**复用**它，不重写遍历逻辑。
//
// 本文件实现的是 Stratum 侧的"处理器"（processor 回调）+ DATA 子消息定位：
//   - DATA 子消息体的字段偏移按 RTPS 规范与 DataMsg.hpp 的序列化顺序定位
//   - 各 PID 的取值格式按 ParameterTypes.hpp 的定义
//
// 上游对应实现：rtps/builtin/data/ParticipantProxyData.cpp
//   （readFromCDRMessage，其中为每个 PID 注册处理器）
// 本实现只提取 Stratum 录制/发现所需字段，不做完整代理数据构造。

#ifndef STRATUM_RTPS_BUILTIN_SPDP_PARSER_H_
#define STRATUM_RTPS_BUILTIN_SPDP_PARSER_H_

#include <cstdint>
#include <string>
#include <vector>

#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/common/Guid.hpp>
#include <stratum/rtps/common/GuidPrefix_t.hpp>
#include <stratum/rtps/common/Locator.hpp>
#include <stratum/rtps/common/Time_t.hpp>
#include <stratum/rtps/common/VendorId_t.hpp>
#include <stratum/rtps/messages/submessage_parser.h>

namespace stratum {
namespace rtps {

// SPDP 公告中的参与者信息（Stratum 需要的子集）
struct SpdpParticipantData {
  bool has_guid = false;
  eprosima::fastdds::rtps::GUID_t guid;

  bool has_lease_duration = false;
  eprosima::fastdds::rtps::Time_t lease_duration;

  bool has_builtin_endpoints = false;
  uint32_t builtin_endpoints = 0;

  bool has_protocol_version = false;
  eprosima::fastdds::rtps::ProtocolVersion_t protocol_version;

  bool has_vendor_id = false;
  eprosima::fastdds::rtps::VendorId_t vendor_id;

  bool has_domain_id = false;
  uint32_t domain_id = 0;

  std::vector<eprosima::fastdds::rtps::Locator_t> default_unicast_locators;
  std::vector<eprosima::fastdds::rtps::Locator_t> metatraffic_unicast_locators;
  std::vector<eprosima::fastdds::rtps::Locator_t> default_multicast_locators;
  std::vector<eprosima::fastdds::rtps::Locator_t> metatraffic_multicast_locators;

  bool has_participant_name = false;
  std::string participant_name;

  // 解析统计（诊断用）
  size_t parameters_seen = 0;
  size_t parameters_unknown = 0;

  // 该公告来自哪个 RTPS 消息（源 guid prefix / vendor）
  eprosima::fastdds::rtps::GuidPrefix_t source_guid_prefix;
  eprosima::fastdds::rtps::VendorId_t source_vendor_id;
};

// 从一条完整的 RTPS 消息中解析 SPDP 公告
// 返回解析出的参与者数量（一条 SPDP 消息通常含 1 个）
class SpdpParser {
 public:
  SpdpParser() = default;

  // 解析完整 RTPS 消息（含 20 字节头）
  // 成功解析出的公告追加到 out；返回处理成功的公告数
  size_t ParseMessage(const uint8_t* data, size_t len,
                      std::vector<SpdpParticipantData>* out);

  // 解析单个 DATA 子消息的参数列表（body 指向子消息体首字节）
  //
  // protocol_minor：RTPS 次版本号。**>= 3 时 DATA 子消息体多一个
  // extraFlags(2 字节)**，固定前缀从 18 变为 20 —— 这是解析真实
  // Fast DDS（2.3）报文时的关键差异，早期实现按 18 解析导致
  // ParameterList 整体错位 2 字节而解析不出任何 PID。
  bool ParseDataSubmessage(const uint8_t* body, size_t body_len,
                           uint8_t protocol_minor, SpdpParticipantData* out);

  const ParseStats& stats() const { return stats_; }

 private:
  SubmessageParser submsg_parser_;
  ParseStats stats_;
};

}  // namespace rtps
}  // namespace stratum

#endif  // STRATUM_RTPS_BUILTIN_SPDP_PARSER_H_
