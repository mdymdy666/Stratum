// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// SPDP 发现解析器实现

#include <stratum/rtps/builtin/spdp_parser.h>

#include <cstring>

#include <stratum/rtps/core/policy/ParameterList.hpp>
#include <stratum/rtps/dds/core/policy/ParameterTypes.hpp>
#include <stratum/rtps/log/Log.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>
#include <stratum/rtps/messages/submessage_parser.h>

namespace stratum {
namespace rtps {

using namespace eprosima::fastdds::rtps;
using eprosima::fastdds::dds::ParameterList;
using eprosima::fastdds::dds::ParameterId_t;
using eprosima::fastdds::dds::PID_SENTINEL;

namespace {

// RTPS DATA 子消息的固定字段长度（RTPS 规范 9.4.5.2 / DataMsg.hpp 的序列化顺序）：
//   octetsToInlineQos(2) + readerId(4) + writerId(4) + writerSN(8) = 18
// 注意：当 flags 的 bit0 == 0（大端）时，RTPS 2.3+ 还会有一个 extraFlags(2)，
// 此时前缀为 20 字节。由调用方按 flags 判断。
constexpr size_t kDataPrefixLittleEndian = 18;
constexpr size_t kDataPrefixBigEndian = 20;

// octetsToInlineQos 的标准值（RTPSMESSAGE_OCTETSTOINLINEQOS_DATASUBMSG）
constexpr uint16_t kOctetsToInlineQos = 16;

}  // namespace

bool SpdpParser::ParseDataSubmessage(const uint8_t* body, size_t body_len,
                                     uint8_t protocol_minor,
                                     SpdpParticipantData* out) {
  // 前缀长度由 DataTypePrefixSize 数据驱动判定（见 submessage_parser.h）：
  // 版本号不可靠，实测存在"版本号 2.2 却含 extraFlags"的实现。
  const size_t kFixedPrefix =
      DataTypePrefixSize(body, body_len, protocol_minor);

  if (body == nullptr || out == nullptr || body_len < kFixedPrefix + 4) {
    return false;
  }

  CDRMessage_t msg(static_cast<uint32_t>(body_len) + 8);
  if (body_len > msg.max_size) {
    return false;
  }
  std::memcpy(msg.buffer, body, body_len);
  msg.length = static_cast<uint32_t>(body_len);
  msg.pos = 0;
  // SPDP 的 DATA 由 Fast DDS 以主机序（小端）编码并置 E 位；
  // 这里先按小端解析前缀，随后由 ParameterList 的 encapsulation 覆盖字节序。
  msg.msg_endian = LITTLEEND;

  // 前缀长度直接按协议版本确定（见上）。
  const size_t prefix = kFixedPrefix;
  if (prefix > body_len) {
    return false;
  }
  // readerId(4) + writerId(4) + writerSN(8)
  msg.pos = static_cast<uint32_t>(prefix);

  // 其后为 serializedPayload：encapsulation(4) + ParameterList
  if (msg.pos + 4 > msg.length) {
    return false;
  }

  // 复用移植的 ParameterList 模板遍历（use_encapsulation=true 让它自行
  // 读取 encapsulation 并据此设定字节序）
  uint32_t qos_size = 0;
  const bool ok = ParameterList::readParameterListfromCDRMsg(
      msg,
      [out](CDRMessage_t* submsg, ParameterId_t pid, uint16_t plength) -> bool {
        ++out->parameters_seen;
        switch (pid) {
          case eprosima::fastdds::dds::PID_PARTICIPANT_GUID: {
            if (plength < 16) return true;
            if (!CDRMessage::readData(submsg, out->guid.guidPrefix.value,
                                      GuidPrefix_t::size)) {
              return true;
            }
            if (!CDRMessage::readEntityId(submsg, &out->guid.entityId)) {
              return true;
            }
            out->has_guid = true;
            break;
          }
          case eprosima::fastdds::dds::PID_PARTICIPANT_LEASE_DURATION: {
            int32_t sec = 0;
            uint32_t frac = 0;
            if (CDRMessage::readInt32(submsg, &sec) &&
                CDRMessage::readUInt32(submsg, &frac)) {
              out->lease_duration = Time_t(sec, frac);
              out->has_lease_duration = true;
            }
            break;
          }
          case eprosima::fastdds::dds::PID_BUILTIN_ENDPOINT_SET: {
            uint32_t v = 0;
            if (CDRMessage::readUInt32(submsg, &v)) {
              out->builtin_endpoints = v;
              out->has_builtin_endpoints = true;
            }
            break;
          }
          case eprosima::fastdds::dds::PID_PROTOCOL_VERSION: {
            ProtocolVersion_t pv;
            if (CDRMessage::readOctet(submsg, &pv.m_major) &&
                CDRMessage::readOctet(submsg, &pv.m_minor)) {
              out->protocol_version = pv;
              out->has_protocol_version = true;
            }
            break;
          }
          case eprosima::fastdds::dds::PID_VENDORID: {
            VendorId_t vid;
            if (CDRMessage::readData(submsg, vid.data(), 2)) {
              out->vendor_id = vid;
              out->has_vendor_id = true;
            }
            break;
          }
          case eprosima::fastdds::dds::PID_DOMAIN_ID: {
            uint32_t v = 0;
            if (CDRMessage::readUInt32(submsg, &v)) {
              out->domain_id = v;
              out->has_domain_id = true;
            }
            break;
          }
          case eprosima::fastdds::dds::PID_DEFAULT_UNICAST_LOCATOR:
          case eprosima::fastdds::dds::PID_METATRAFFIC_UNICAST_LOCATOR:
          case eprosima::fastdds::dds::PID_DEFAULT_MULTICAST_LOCATOR:
          case eprosima::fastdds::dds::PID_METATRAFFIC_MULTICAST_LOCATOR: {
            Locator_t loc;
            if (!CDRMessage::read_locator(submsg, &loc)) {
              break;
            }
            switch (pid) {
              case eprosima::fastdds::dds::PID_DEFAULT_UNICAST_LOCATOR:
                out->default_unicast_locators.push_back(loc);
                break;
              case eprosima::fastdds::dds::PID_METATRAFFIC_UNICAST_LOCATOR:
                out->metatraffic_unicast_locators.push_back(loc);
                break;
              case eprosima::fastdds::dds::PID_DEFAULT_MULTICAST_LOCATOR:
                out->default_multicast_locators.push_back(loc);
                break;
              default:
                out->metatraffic_multicast_locators.push_back(loc);
                break;
            }
            break;
          }
          default:
            ++out->parameters_unknown;
            break;
        }
        return true;
      },
      /*use_encapsulation=*/true, qos_size);

  return ok && out->has_guid;
}

size_t SpdpParser::ParseMessage(const uint8_t* data, size_t len,
                                std::vector<SpdpParticipantData>* out) {
  if (data == nullptr || len < 20 || out == nullptr) {
    return 0;
  }

  CDRMessage_t msg(static_cast<uint32_t>(len) + 8);
  if (len > msg.max_size) {
    return 0;
  }
  std::memcpy(msg.buffer, data, len);
  msg.length = static_cast<uint32_t>(len);
  msg.pos = 0;
  msg.msg_endian = LITTLEEND;

  RtpsHeaderInfo hdr;
  if (!submsg_parser_.CheckRtpsHeader(&msg, &hdr)) {
    return 0;
  }

  size_t found = 0;
  // 遍历子消息；DATA 子消息里承载 SPDP 公告
  while (msg.pos < msg.length) {
    const uint32_t before = msg.pos;
    SubmessageHeader_t smh;
    if (!submsg_parser_.ReadSubmessageHeader(&msg, &smh)) {
      ++stats_.malformed;
      break;
    }
    ++stats_.total;

    const uint32_t body_start = msg.pos;
    const uint32_t body_len = smh.submessageLength;

    if (smh.submessageId == DATA && body_len > 0) {
      ++stats_.data;
      SpdpParticipantData pd;
      pd.source_guid_prefix = hdr.source_guid_prefix;
      pd.source_vendor_id = hdr.source_vendor_id;

      // DATA 子消息的 flags：bit2 = D（含负载）。SPDP 的公告一定有负载。
      if ((smh.flags & 0x04) != 0 &&
          body_start + body_len <= msg.length &&
          ParseDataSubmessage(msg.buffer + body_start, body_len,
                              hdr.source_version.m_minor, &pd)) {
        out->push_back(pd);
        ++found;
      }
    } else {
      switch (smh.submessageId) {
        case HEARTBEAT: ++stats_.heartbeat; break;
        case ACKNACK: ++stats_.acknack; break;
        case GAP: ++stats_.gap; break;
        case INFO_TS: ++stats_.info_ts; break;
        case INFO_DST: ++stats_.info_dst; break;
        case INFO_SRC: ++stats_.info_src; break;
        default: ++stats_.unknown; break;
      }
    }

    // 步进
    if (smh.is_last) {
      break;
    }
    const uint32_t next = body_start + body_len;
    if (next <= before) {
      ++stats_.malformed;
      break;
    }
    msg.pos = next;
    if (msg.pos > msg.length) {
      ++stats_.malformed;
      break;
    }
  }

  return found;
}

}  // namespace rtps
}  // namespace stratum
