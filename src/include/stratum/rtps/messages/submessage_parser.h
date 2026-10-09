// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// RTPS 子消息解析器
//
// ===========================================================================
// 移植说明（ref-05 分类：裁剪修改后移植）
// ===========================================================================
//
// 解析逻辑**逐行移植**自 Fast DDS：
//   src/cpp/rtps/messages/MessageReceiver.cpp
//     - checkRTPSHeader        (L600-636)
//     - readSubmessageHeader   (L638-678)
//     - proc_Submsg_Heartbeat  (L1082-1149) 的解析段
//     - proc_Submsg_Acknack    (L1151-1207) 的解析段
//     - proc_Submsg_Gap        (L1210-1255) 的解析段
//     - proc_Submsg_InfoTS/InfoDST/InfoSRC (L1258-1344)
//
// **剥离**（属 Fast DDS 的 Reader/Writer 体系，Stratum 不需要）：
//   guard(mtx_) / findAllReaders() / associated_writers_ /
//   reader->process_*_msg() / HAVE_SECURITY 分支
//
// **保留的协议语义**（不可省）：
//   flags bit0 决定 msg_endian；Heartbeat 的 firstSN/lastSN 校验；
//   Gap 的 gapStart 校验；octetsToNextHeader 越界检查与最后子消息判定。

#ifndef STRATUM_RTPS_MESSAGES_SUBMESSAGE_PARSER_H_
#define STRATUM_RTPS_MESSAGES_SUBMESSAGE_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/common/EntityId_t.hpp>
#include <stratum/rtps/common/Guid.hpp>
#include <stratum/rtps/common/SequenceNumber.hpp>
#include <stratum/rtps/common/Time_t.hpp>
#include <stratum/rtps/common/Types.hpp>
#include <stratum/rtps/common/VendorId_t.hpp>
#include <stratum/rtps/log/Log.hpp>
#include <stratum/rtps/messages/RTPS_messages.hpp>

namespace stratum {
namespace rtps {

// 上游类型别名（避免在头文件里 using namespace）
using RtpsProtocolVersion = eprosima::fastdds::rtps::ProtocolVersion_t;
using RtpsVendorId = eprosima::fastdds::rtps::VendorId_t;
using RtpsGuidPrefix = eprosima::fastdds::rtps::GuidPrefix_t;
using RtpsEntityId = eprosima::fastdds::rtps::EntityId_t;
using RtpsSequenceNumber = eprosima::fastdds::rtps::SequenceNumber_t;
using RtpsSequenceNumberSet = eprosima::fastdds::rtps::SequenceNumberSet_t;
using RtpsSubmessageHeader = eprosima::fastdds::rtps::SubmessageHeader_t;
using RtpsTime = eprosima::fastdds::rtps::Time_t;
using RtpsCdrMessage = eprosima::fastdds::rtps::CDRMessage_t;
using RtpsEndianness = eprosima::fastdds::rtps::Endianness_t;

// RTPS 消息头解析结果（对应 Fast DDS checkRTPSHeader 写入的接收器状态）
struct RtpsHeaderInfo {
  RtpsProtocolVersion source_version;
  RtpsVendorId source_vendor_id;
  RtpsGuidPrefix source_guid_prefix;
  bool have_timestamp = false;
};

struct HeartbeatInfo {
  RtpsEntityId reader_id;
  RtpsEntityId writer_id;
  RtpsSequenceNumber first_sn;
  RtpsSequenceNumber last_sn;
  uint32_t count = 0;
  bool final_flag = false;
  bool liveliness_flag = false;
};

struct AckNackInfo {
  RtpsEntityId reader_id;
  RtpsEntityId writer_id;
  RtpsSequenceNumberSet sn_set;
  uint32_t count = 0;
  bool final_flag = false;
};

struct GapInfo {
  RtpsEntityId reader_id;
  RtpsEntityId writer_id;
  RtpsSequenceNumber gap_start;
  RtpsSequenceNumberSet gap_list;
};

struct InfoTimestampInfo {
  RtpsTime timestamp;
  bool invalidate = false;
};

struct InfoDestinationInfo {
  RtpsGuidPrefix guid_prefix;
};

struct InfoSourceInfo {
  RtpsProtocolVersion version;
  RtpsVendorId vendor_id;
  RtpsGuidPrefix guid_prefix;
};

struct ParseStats {
  size_t total = 0;
  size_t heartbeat = 0;
  size_t acknack = 0;
  size_t gap = 0;
  size_t info_ts = 0;
  size_t info_dst = 0;
  size_t info_src = 0;
  size_t data = 0;
  size_t pad = 0;
  size_t unknown = 0;
  size_t malformed = 0;
};

struct SubmessageEnvelope {
  RtpsSubmessageHeader header;
  RtpsEndianness endian;
  size_t body_offset = 0;
  size_t body_length = 0;
  size_t next_offset = 0;
};


// ---------------------------------------------------------------------------
// DATA 子消息的固定前缀长度
// ---------------------------------------------------------------------------
//
// RTPS 2.3 起 DATA 子消息体首部多了 `extraFlags`(2 字节)，故前缀为
// 20 字节（extraFlags + octetsToInlineQos + readerId + writerId + writerSN），
// 2.2 及更早为 18 字节。
//
// **但版本号不可靠**：实测同一环境中存在
//   - Fast DDS 3.6 的 SPDP 报文：版本号 2.3，含 extraFlags ✓
//   - rmw_fastrtps 的用户数据报文：版本号 **2.2**，却**同样含 extraFlags**
// 若只看版本号，后者会被按 18 字节解析 → readerId/writerId 整体错位 2 字节，
// 表现为"把 SPDP 公告当成用户数据"这类误判。
//
// 因此改为**数据驱动判定**：`octetsToInlineQos` 的标准值是 16
// （readerId 4 + writerId 4 + writerSN 8），据此确定 extraFlags 是否存在。
size_t DataTypePrefixSize(const uint8_t* body, size_t body_len, uint8_t ver_minor);

class SubmessageParser {
 public:
  SubmessageParser() = default;

  // 对应 checkRTPSHeader (MessageReceiver.cpp:600)
  bool CheckRtpsHeader(RtpsCdrMessage* msg, RtpsHeaderInfo* info);

  // 对应 readSubmessageHeader (MessageReceiver.cpp:638)
  bool ReadSubmessageHeader(RtpsCdrMessage* msg, RtpsSubmessageHeader* smh) const;

  // 对应 proc_Submsg_Heartbeat/Acknack/Gap 的解析段
  bool ProcHeartbeat(RtpsCdrMessage* msg, const RtpsSubmessageHeader* smh,
                     HeartbeatInfo* out) const;
  bool ProcAcknack(RtpsCdrMessage* msg, const RtpsSubmessageHeader* smh,
                   AckNackInfo* out) const;
  bool ProcGap(RtpsCdrMessage* msg, const RtpsSubmessageHeader* smh,
               GapInfo* out) const;

  // 对应 proc_Submsg_InfoTS / InfoDST / InfoSRC
  bool ProcInfoTimestamp(RtpsCdrMessage* msg, const RtpsSubmessageHeader* smh,
                         InfoTimestampInfo* out);
  bool ProcInfoDestination(RtpsCdrMessage* msg, const RtpsSubmessageHeader* smh,
                           InfoDestinationInfo* out);
  bool ProcInfoSource(RtpsCdrMessage* msg, const RtpsSubmessageHeader* smh,
                      InfoSourceInfo* out);

  // 接收器状态访问
  const RtpsGuidPrefix& source_guid_prefix() const { return source_guid_prefix_; }
  const RtpsVendorId& source_vendor_id() const { return source_vendor_id_; }
  const RtpsProtocolVersion& source_version() const { return source_version_; }
  bool have_timestamp() const { return have_timestamp_; }

  // 结构遍历（Stratum 附加；骨架对应 processCDRMsg L345-598 的 while 循环）
  size_t ParseMessage(RtpsCdrMessage* msg, std::vector<SubmessageEnvelope>* out,
                      ParseStats* stats);

 private:
  // 对应 Fast DDS MessageReceiver 的接收器状态成员
  RtpsGuidPrefix source_guid_prefix_;
  RtpsVendorId source_vendor_id_;
  RtpsProtocolVersion source_version_;
  bool have_timestamp_ = false;
};

}  // namespace rtps
}  // namespace stratum

#endif  // STRATUM_RTPS_MESSAGES_SUBMESSAGE_PARSER_H_
