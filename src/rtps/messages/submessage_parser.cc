// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// RTPS 子消息解析器实现
//
// 解析逻辑逐行移植自 Fast DDS 的 src/cpp/rtps/messages/MessageReceiver.cpp。
// 每段都标注了对应的上游行号，便于逐行对照复核。

#include <stratum/rtps/messages/submessage_parser.h>

#include <stratum/rtps/log/Log.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>

#include <cstring>

namespace stratum {
namespace rtps {

// ---------------------------------------------------------------------------
// DATA 子消息前缀长度的数据驱动判定（见头文件中的说明）
// ---------------------------------------------------------------------------
size_t DataTypePrefixSize(const uint8_t* body, size_t body_len,
                          uint8_t ver_minor) {
  if (body != nullptr && body_len >= 4) {
    const uint16_t with_extra = static_cast<uint16_t>(body[2]) |
                                (static_cast<uint16_t>(body[3]) << 8);
    const uint16_t without_extra = static_cast<uint16_t>(body[0]) |
                                   (static_cast<uint16_t>(body[1]) << 8);
    if (with_extra == 16) return 20;
    if (without_extra == 16) return 18;
  }
  return (ver_minor >= 3) ? 20 : 18;   // 兜底：按版本号
}


// 移植代码原样保留上游符号名（CDRMessage 等），整体引入其命名空间。
// 头文件侧不使用 using namespace，改用 Rtps* 别名以避免污染。
using namespace eprosima::fastdds::rtps;


// ---------------------------------------------------------------------------
// 对应 MessageReceiver::checkRTPSHeader (MessageReceiver.cpp:600-636)
//
// 移植差异：
//   - 上游把结果写入成员 source_version_ / source_vendor_id_ /
//     source_guid_prefix_ / have_timestamp_；本实现额外回填 info 出参。
//   - 去掉 EPROSIMA_LOG_*（已由 log/Log.hpp 桩置空，保留原调用以便对照）。
// ---------------------------------------------------------------------------
bool SubmessageParser::CheckRtpsHeader(CDRMessage_t* msg, RtpsHeaderInfo* info) {
  // check and proccess the RTPS Header
  if (msg->buffer[0] != 'R' || msg->buffer[1] != 'T' ||
      msg->buffer[2] != 'P' || msg->buffer[3] != 'S') {
    EPROSIMA_LOG_INFO(RTPS_MSG_IN,
                      IDSTRING "Msg received with no RTPS in header, ignoring...");
    return false;
  }

  msg->pos += 4;

  // CHECK AND SET protocol version
  if (msg->buffer[msg->pos] == c_ProtocolVersion.m_major) {
    source_version_.m_major = msg->buffer[msg->pos];
    msg->pos++;
    source_version_.m_minor = msg->buffer[msg->pos];
    msg->pos++;
  } else {
    EPROSIMA_LOG_WARNING(RTPS_MSG_IN, IDSTRING "Major RTPS Version not supported");
    return false;
  }

  // Set source vendor id
  source_vendor_id_[0] = msg->buffer[msg->pos];
  msg->pos++;
  source_vendor_id_[1] = msg->buffer[msg->pos];
  msg->pos++;
  // set source guid prefix
  CDRMessage::readData(msg, source_guid_prefix_.value, GuidPrefix_t::size);
  have_timestamp_ = false;

  if (info != nullptr) {
    info->source_version = source_version_;
    info->source_vendor_id = source_vendor_id_;
    info->source_guid_prefix = source_guid_prefix_;
    info->have_timestamp = have_timestamp_;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 对应 MessageReceiver::readSubmessageHeader (MessageReceiver.cpp:638-678)
// ---------------------------------------------------------------------------
bool SubmessageParser::ReadSubmessageHeader(CDRMessage_t* msg,
                                            SubmessageHeader_t* smh) const {
  if (msg->length - msg->pos < 4) {
    EPROSIMA_LOG_WARNING(RTPS_MSG_IN, IDSTRING "SubmessageHeader too short");
    return false;
  }

  smh->submessageId = msg->buffer[msg->pos];
  msg->pos++;
  smh->flags = msg->buffer[msg->pos];
  msg->pos++;

  // Set endianness of message
  msg->msg_endian = (smh->flags & BIT(0)) != 0 ? LITTLEEND : BIGEND;
  uint16_t length{0};
  CDRMessage::readUInt16(msg, &length);
  if (msg->pos + length > msg->length) {
    EPROSIMA_LOG_WARNING(RTPS_MSG_IN,
                         IDSTRING "SubMsg of invalid length ("
                             << length << ") with current msg position/length ("
                             << msg->pos << "/" << msg->length << ")");
    return false;
  }

  if ((length == 0) && (smh->submessageId != INFO_TS) &&
      (smh->submessageId != PAD)) {
    // THIS IS THE LAST SUBMESSAGE
    smh->submessageLength = msg->length - msg->pos;
    smh->is_last = true;
  } else {
    smh->submessageLength = length;
    smh->is_last = false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// 对应 MessageReceiver::proc_Submsg_Heartbeat (MessageReceiver.cpp:1082-1149)
//
// 移植差异：保留全部解析与协议校验；用 out 出参替代 findAllReaders 派发
// （上游末尾调用 reader->process_heartbeat_msg(...)，本实现不派发）。
// ---------------------------------------------------------------------------
bool SubmessageParser::ProcHeartbeat(CDRMessage_t* msg,
                                     const SubmessageHeader_t* smh,
                                     HeartbeatInfo* out) const {
  bool endiannessFlag{(smh->flags & BIT(0)) != 0};
  bool finalFlag{(smh->flags & BIT(1)) != 0};
  bool livelinessFlag{(smh->flags & BIT(2)) != 0};
  // Assign message endianness
  if (endiannessFlag) {
    msg->msg_endian = LITTLEEND;
  } else {
    msg->msg_endian = BIGEND;
  }

  GUID_t readerGUID;
  GUID_t writerGUID;
  readerGUID.guidPrefix = GuidPrefix_t::unknown();
  CDRMessage::readEntityId(msg, &readerGUID.entityId);
  writerGUID.guidPrefix = source_guid_prefix_;
  CDRMessage::readEntityId(msg, &writerGUID.entityId);
  SequenceNumber_t firstSN;
  SequenceNumber_t lastSN;
  CDRMessage::readSequenceNumber(msg, &firstSN);
  CDRMessage::readSequenceNumber(msg, &lastSN);

  SequenceNumber_t zeroSN;
  if (firstSN <= zeroSN) {
    EPROSIMA_LOG_WARNING(RTPS_MSG_IN,
                         IDSTRING "Invalid Heartbeat received (" << firstSN
                                                                << " <= 0), ignoring");
    return false;
  }
  if (lastSN < firstSN && lastSN != firstSN - 1) {
    EPROSIMA_LOG_WARNING(RTPS_MSG_IN,
                         IDSTRING "Invalid Heartbeat received (" << firstSN
                                                                << ") - (" << lastSN
                                                                << "), ignoring");
    return false;
  }
  uint32_t HBCount{0};
  if (!CDRMessage::readUInt32(msg, &HBCount)) {
    EPROSIMA_LOG_WARNING(RTPS_MSG_IN,
                         IDSTRING "Unable to read heartbeat count from heartbeat message");
    return false;
  }

  // 上游此处调用 findAllReaders(...) 并 reader->process_heartbeat_msg(...)。
  // Stratum 只做解析，结果交给上层（Recorder/Player）。
  if (out != nullptr) {
    out->reader_id = readerGUID.entityId;
    out->writer_id = writerGUID.entityId;
    out->first_sn = firstSN;
    out->last_sn = lastSN;
    out->count = HBCount;
    out->final_flag = finalFlag;
    out->liveliness_flag = livelinessFlag;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 对应 MessageReceiver::proc_Submsg_Acknack (MessageReceiver.cpp:1151-1207)
//
// 移植差异：保留解析；用 out 出参替代 associated_writers_ 遍历派发。
// ---------------------------------------------------------------------------
bool SubmessageParser::ProcAcknack(CDRMessage_t* msg,
                                   const SubmessageHeader_t* smh,
                                   AckNackInfo* out) const {
  bool endiannessFlag{(smh->flags & BIT(0)) != 0};
  bool finalFlag{(smh->flags & BIT(1)) != 0};
  // Assign message endianness
  if (endiannessFlag) {
    msg->msg_endian = LITTLEEND;
  } else {
    msg->msg_endian = BIGEND;
  }
  GUID_t readerGUID;
  GUID_t writerGUID;
  readerGUID.guidPrefix = source_guid_prefix_;
  CDRMessage::readEntityId(msg, &readerGUID.entityId);
  writerGUID.guidPrefix = GuidPrefix_t::unknown();
  CDRMessage::readEntityId(msg, &writerGUID.entityId);

  SequenceNumberSet_t SNSet = CDRMessage::readSequenceNumberSet(msg);
  uint32_t Ackcount{0};
  if (!CDRMessage::readUInt32(msg, &Ackcount)) {
    EPROSIMA_LOG_WARNING(RTPS_MSG_IN, IDSTRING "Unable to read ackcount from message");
    return false;
  }

  // 上游此处遍历 associated_writers_ 调用 it->process_acknack(...)。
  if (out != nullptr) {
    out->reader_id = readerGUID.entityId;
    out->writer_id = writerGUID.entityId;
    out->sn_set = SNSet;
    out->count = Ackcount;
    out->final_flag = finalFlag;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 对应 MessageReceiver::proc_Submsg_Gap (MessageReceiver.cpp:1210-1255)
// ---------------------------------------------------------------------------
bool SubmessageParser::ProcGap(CDRMessage_t* msg, const SubmessageHeader_t* smh,
                               GapInfo* out) const {
  bool endiannessFlag{(smh->flags & BIT(0)) != 0};
  // Assign message endianness
  if (endiannessFlag) {
    msg->msg_endian = LITTLEEND;
  } else {
    msg->msg_endian = BIGEND;
  }

  GUID_t writerGUID;
  GUID_t readerGUID;
  readerGUID.guidPrefix = GuidPrefix_t::unknown();
  CDRMessage::readEntityId(msg, &readerGUID.entityId);
  writerGUID.guidPrefix = source_guid_prefix_;
  CDRMessage::readEntityId(msg, &writerGUID.entityId);
  SequenceNumber_t gapStart;
  CDRMessage::readSequenceNumber(msg, &gapStart);
  SequenceNumberSet_t gapList = CDRMessage::readSequenceNumberSet(msg);
  if (gapStart <= SequenceNumber_t(0, 0)) {
    return false;
  }

  // 上游此处调用 findAllReaders(...) 并 reader->process_gap_msg(...)。
  if (out != nullptr) {
    out->reader_id = readerGUID.entityId;
    out->writer_id = writerGUID.entityId;
    out->gap_start = gapStart;
    out->gap_list = gapList;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 对应 MessageReceiver::proc_Submsg_InfoTS (MessageReceiver.cpp:1258-1286)
// ---------------------------------------------------------------------------
bool SubmessageParser::ProcInfoTimestamp(CDRMessage_t* msg,
                                         const SubmessageHeader_t* smh,
                                         InfoTimestampInfo* out) {
  bool endiannessFlag{(smh->flags & BIT(0)) != 0};
  bool invalidateFlag{(smh->flags & BIT(1)) != 0};
  if (endiannessFlag) {
    msg->msg_endian = LITTLEEND;
  } else {
    msg->msg_endian = BIGEND;
  }

  Time_t timestamp;
  if (!invalidateFlag) {
    if (!CDRMessage::readTimestamp(msg, &timestamp)) {
      return false;
    }
  } else {
    timestamp = c_RTPSTimeInfinite;
  }
  have_timestamp_ = true;

  if (out != nullptr) {
    out->timestamp = timestamp;
    out->invalidate = invalidateFlag;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 对应 MessageReceiver::proc_Submsg_InfoDST (MessageReceiver.cpp:1288-1313)
// ---------------------------------------------------------------------------
bool SubmessageParser::ProcInfoDestination(CDRMessage_t* msg,
                                           const SubmessageHeader_t* smh,
                                           InfoDestinationInfo* out) {
  bool endiannessFlag{(smh->flags & BIT(0)) != 0};
  if (endiannessFlag) {
    msg->msg_endian = LITTLEEND;
  } else {
    msg->msg_endian = BIGEND;
  }

  GuidPrefix_t guidP;
  if (!CDRMessage::readData(msg, guidP.value, GuidPrefix_t::size)) {
    return false;
  }
  if (guidP != GuidPrefix_t::unknown()) {
    // 上游更新 dest_guid_prefix_（派发目标）
  }
  if (out != nullptr) {
    out->guid_prefix = guidP;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 对应 MessageReceiver::proc_Submsg_InfoSRC (MessageReceiver.cpp:1315-1344)
// ---------------------------------------------------------------------------
bool SubmessageParser::ProcInfoSource(CDRMessage_t* msg,
                                      const SubmessageHeader_t* smh,
                                      InfoSourceInfo* out) {
  bool endiannessFlag{(smh->flags & BIT(0)) != 0};
  if (endiannessFlag) {
    msg->msg_endian = LITTLEEND;
  } else {
    msg->msg_endian = BIGEND;
  }

  ProtocolVersion_t pv;
  if (!CDRMessage::readOctet(msg, &pv.m_major)) {
    return false;
  }
  if (!CDRMessage::readOctet(msg, &pv.m_minor)) {
    return false;
  }
  VendorId_t vendorId;
  if (!CDRMessage::readData(msg, vendorId.data(), 2)) {
    return false;
  }
  GuidPrefix_t guidP;
  if (!CDRMessage::readData(msg, guidP.value, GuidPrefix_t::size)) {
    return false;
  }

  source_version_ = pv;
  source_vendor_id_ = vendorId;
  source_guid_prefix_ = guidP;

  if (out != nullptr) {
    out->version = pv;
    out->vendor_id = vendorId;
    out->guid_prefix = guidP;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 结构遍历（Stratum 附加，无上游直接对应；用上游的 readSubmessageHeader
// 做步进，对应 MessageReceiver::processCDRMsg L345-598 的 while 循环骨架）
// ---------------------------------------------------------------------------
size_t SubmessageParser::ParseMessage(CDRMessage_t* msg,
                                      std::vector<SubmessageEnvelope>* out,
                                      ParseStats* stats) {
  size_t parsed = 0;
  while (msg->pos < msg->length) {
    const size_t header_start = msg->pos;
    SubmessageHeader_t smh;
    if (!ReadSubmessageHeader(msg, &smh)) {
      if (stats != nullptr) {
        ++stats->malformed;
      }
      break;
    }

    SubmessageEnvelope env;
    env.header = smh;
    env.endian = msg->msg_endian;
    env.body_offset = msg->pos;
    env.body_length = smh.submessageLength;
    env.next_offset = msg->pos + smh.submessageLength;

    if (stats != nullptr) {
      ++stats->total;
      switch (smh.submessageId) {
        case eprosima::fastdds::rtps::HEARTBEAT: ++stats->heartbeat; break;
        case eprosima::fastdds::rtps::ACKNACK: ++stats->acknack; break;
        case eprosima::fastdds::rtps::GAP: ++stats->gap; break;
        case eprosima::fastdds::rtps::INFO_TS: ++stats->info_ts; break;
        case eprosima::fastdds::rtps::INFO_DST: ++stats->info_dst; break;
        case eprosima::fastdds::rtps::INFO_SRC: ++stats->info_src; break;
        case eprosima::fastdds::rtps::DATA: ++stats->data; break;
        case eprosima::fastdds::rtps::PAD: ++stats->pad; break;
        default: ++stats->unknown; break;
      }
    }

    if (out != nullptr) {
      out->push_back(env);
    }
    ++parsed;

    // 步进（对应上游 processCDRMsg 的 pos 推进）
    if (smh.is_last) {
      break;
    }
    if (env.next_offset <= header_start) {
      // 防御：长度异常导致不前进
      if (stats != nullptr) {
        ++stats->malformed;
      }
      break;
    }
    msg->pos = static_cast<uint32_t>(env.next_offset);
    if (msg->pos > msg->length) {
      if (stats != nullptr) {
        ++stats->malformed;
      }
      break;
    }
  }
  return parsed;
}

}  // namespace rtps
}  // namespace stratum
