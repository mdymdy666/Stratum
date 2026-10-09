// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// SEDP（端点发现）解析器
//
// ===========================================================================
// 移植基础
// ===========================================================================
//
// 参数表遍历**复用**已移植的 ParameterList::readParameterListfromCDRMsg
// （core/policy/ParameterList.hpp:73-150），与 SPDP 解析器同一套机制。
//
// 上游对应实现：rtps/builtin/data/WriterProxyData.cpp /
//              ReaderProxyData.cpp 的 readFromCDRMessage（各约 2000 行）
// 本实现只提取 Stratum 录制所需字段（话题/类型/GUID/locator/QoS），
// 不做完整代理数据构造 —— 与 SPDP 解析器同策略。
//
// 数据来源区分：
//   DATA 的 writerId == ENTITYID_SEDP_BUILTIN_PUBLICATIONS_WRITER
//       → 内容是 Publication（远端存在 DataWriter，Stratum 可作为 Reader 订阅）
//   DATA 的 writerId == ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_WRITER
//       → 内容是 Subscription（远端存在 DataReader）

#ifndef STRATUM_RTPS_BUILTIN_SEDP_PARSER_H_
#define STRATUM_RTPS_BUILTIN_SEDP_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <stratum/rtps/common/Guid.hpp>
#include <stratum/rtps/common/Locator.hpp>
#include <stratum/rtps/common/Time_t.hpp>
#include <stratum/rtps/common/Types.hpp>

namespace stratum {
namespace rtps {

// SEDP 公告的类别
enum class SedpKind : uint8_t {
  kUnknown = 0,
  kPublication = 1,    // 远端 DataWriter（我们可订阅）
  kSubscription = 2,   // 远端 DataReader
};

// 一个端点的关键信息（Stratum 录制所需子集）
struct SedpEndpointData {
  SedpKind kind = SedpKind::kUnknown;

  // 端点与参与者标识
  bool has_endpoint_guid = false;
  eprosima::fastdds::rtps::GUID_t endpoint_guid;
  bool has_participant_guid = false;
  eprosima::fastdds::rtps::GUID_t participant_guid;

  // 话题与类型（发现的核心产出）
  bool has_topic_name = false;
  std::string topic_name;
  bool has_type_name = false;
  std::string type_name;

  // 数据地址
  std::vector<eprosima::fastdds::rtps::Locator_t> unicast_locators;
  std::vector<eprosima::fastdds::rtps::Locator_t> multicast_locators;

  // ---- QoS（Stratum 作为 Reader 需要判定兼容性的策略）----
  bool has_reliability = false;
  uint32_t reliability_kind = 0;    // 1=BEST_EFFORT, 2=RELIABLE
  bool has_durability = false;
  uint32_t durability_kind = 0;     // 0=VOLATILE, 1=TRANSIENT_LOCAL, 2=TRANSIENT, 3=PERSISTENT
  bool has_ownership = false;
  uint32_t ownership_kind = 0;      // 0=SHARED, 1=EXCLUSIVE
  bool has_destination_order = false;
  uint32_t destination_order_kind = 0;
  bool has_liveliness = false;
  uint32_t liveliness_kind = 0;
  // LIVELINESS 策略的第二部分是 lease_duration（PID 0x001b 共 12 字节）
  eprosima::fastdds::rtps::Time_t liveliness_lease;
  bool has_data_representation = false;
  uint32_t data_representation = 0;  // 0=XCDR1, 1=XCDR2

  bool has_deadline = false;
  eprosima::fastdds::rtps::Time_t deadline;
  bool has_latency_budget = false;
  eprosima::fastdds::rtps::Time_t latency_budget;
  bool has_lifespan = false;
  eprosima::fastdds::rtps::Time_t lifespan;

  bool has_type_max_size = false;
  uint32_t type_max_size_serialized = 0;

  // 协议版本 / 厂商（用于区分对端实现差异）
  bool has_protocol_version = false;
  uint8_t protocol_major = 0;
  uint8_t protocol_minor = 0;
  bool has_vendor_id = false;
  uint8_t vendor_id[2] = {0, 0};

  // 解析统计
  size_t parameters_seen = 0;
  size_t parameters_unknown = 0;
  bool parse_ok = false;
};

// 从一条 RTPS 消息（DATA 子消息体）解析 SEDP 公告
class SedpParser {
 public:
  SedpParser() = default;

  // 输入：RTPS 消息原始字节 + 该消息中 DATA 子消息的位置信息
  //   body       —— DATA 子消息体首字节（子消息头之后）
  //   body_len   —— 子消息体长度
  //   writer_id  —— DATA 的 writerId（用于判定 Publication / Subscription）
  //   protocol_minor —— RTPS 次版本（>=3 时 DATA 前缀含 extraFlags）
  bool ParseDataSubmessage(const uint8_t* body, size_t body_len,
                           uint32_t writer_id, uint8_t protocol_minor,
                           SedpEndpointData* out);

  // 从完整 RTPS 消息里定位并解析所有 SEDP DATA
  size_t ParseMessage(const uint8_t* data, size_t len,
                      std::vector<SedpEndpointData>* out);
};

}  // namespace rtps
}  // namespace stratum

#endif  // STRATUM_RTPS_BUILTIN_SEDP_PARSER_H_
