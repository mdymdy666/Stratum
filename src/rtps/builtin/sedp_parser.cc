// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// SEDP 解析器实现

#include <stratum/rtps/builtin/sedp_parser.h>

#include <cstring>

#include <stratum/rtps/core/policy/ParameterList.hpp>
#include <stratum/rtps/dds/core/policy/ParameterTypes.hpp>
#include <stratum/rtps/log/Log.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>
#include <stratum/rtps/messages/submessage_parser.h>

namespace stratum {
namespace rtps {

using namespace eprosima::fastdds::rtps;
namespace dds = eprosima::fastdds::dds;

namespace {

// DATA 子消息体的固定前缀（与 spdp_parser 相同规则）
constexpr size_t kPrefixShort = 18;   // RTPS < 2.3：无 extraFlags
constexpr size_t kPrefixLong = 20;    // RTPS >= 2.3：含 extraFlags

// 从参数值里读一个 uint32 的 kind（QoS 策略的第一个字段）
bool ReadKind(CDRMessage_t* submsg, uint32_t* out) {
  return CDRMessage::readUInt32(submsg, out);
}

// 读 Duration_t（seconds:int32 + fraction:uint32）
bool ReadDuration(CDRMessage_t* submsg, Time_t* out) {
  int32_t sec = 0;
  uint32_t frac = 0;
  if (!CDRMessage::readInt32(submsg, &sec)) return false;
  if (!CDRMessage::readUInt32(submsg, &frac)) return false;
  *out = Time_t(sec, frac);
  return true;
}

}  // namespace

bool SedpParser::ParseDataSubmessage(const uint8_t* body, size_t body_len,
                                     uint32_t writer_id, uint8_t protocol_minor,
                                     SedpEndpointData* out) {
  if (body == nullptr || out == nullptr) return false;

  const size_t prefix = DataTypePrefixSize(body, body_len, protocol_minor);
  if (body_len < prefix + 4) return false;

  // 判定类别
  if (writer_id == 0x000003c2u) {
    out->kind = SedpKind::kPublication;
  } else if (writer_id == 0x000004c2u) {
    out->kind = SedpKind::kSubscription;
  } else {
    out->kind = SedpKind::kUnknown;
  }

  CDRMessage_t msg(static_cast<uint32_t>(body_len) + 8);
  if (body_len > msg.max_size) return false;
  std::memcpy(msg.buffer, body, body_len);
  msg.length = static_cast<uint32_t>(body_len);
  msg.pos = static_cast<uint32_t>(prefix);
  msg.msg_endian = LITTLEEND;

  // 复用移植的 ParameterList 模板遍历（含 encapsulation 处理）
  uint32_t qos_size = 0;
  const bool ok = dds::ParameterList::readParameterListfromCDRMsg(
      msg,
      [out](CDRMessage_t* submsg, dds::ParameterId_t pid, uint16_t plength) -> bool {
        ++out->parameters_seen;
        switch (pid) {
          case dds::PID_ENDPOINT_GUID: {
            if (plength < 16) break;
            if (CDRMessage::readData(submsg, out->endpoint_guid.guidPrefix.value,
                                     GuidPrefix_t::size) &&
                CDRMessage::readEntityId(submsg, &out->endpoint_guid.entityId)) {
              out->has_endpoint_guid = true;
            }
            break;
          }
          case dds::PID_PARTICIPANT_GUID: {
            if (plength < 16) break;
            if (CDRMessage::readData(submsg, out->participant_guid.guidPrefix.value,
                                     GuidPrefix_t::size) &&
                CDRMessage::readEntityId(submsg, &out->participant_guid.entityId)) {
              out->has_participant_guid = true;
            }
            break;
          }
          case dds::PID_TOPIC_NAME: {
            // ParameterString_t：uint32 长度（含结尾 '\0'）+ 字符
            uint32_t len = 0;
            if (!CDRMessage::readUInt32(submsg, &len)) break;
            if (len == 0 || len > plength) break;
            std::string s(len, '\0');
            if (!CDRMessage::readData(submsg,
                                      reinterpret_cast<octet*>(&s[0]), len)) {
              break;
            }
            while (!s.empty() && s.back() == '\0') s.pop_back();
            out->topic_name = s;
            out->has_topic_name = true;
            break;
          }
          case dds::PID_TYPE_NAME: {
            uint32_t len = 0;
            if (!CDRMessage::readUInt32(submsg, &len)) break;
            if (len == 0 || len > plength) break;
            std::string s(len, '\0');
            if (!CDRMessage::readData(submsg,
                                      reinterpret_cast<octet*>(&s[0]), len)) {
              break;
            }
            while (!s.empty() && s.back() == '\0') s.pop_back();
            out->type_name = s;
            out->has_type_name = true;
            break;
          }
          case dds::PID_UNICAST_LOCATOR: {
            Locator_t loc;
            if (CDRMessage::read_locator(submsg, &loc)) {
              out->unicast_locators.push_back(loc);
            }
            break;
          }
          case dds::PID_MULTICAST_LOCATOR: {
            Locator_t loc;
            if (CDRMessage::read_locator(submsg, &loc)) {
              out->multicast_locators.push_back(loc);
            }
            break;
          }
          case dds::PID_RELIABILITY: {
            uint32_t k = 0;
            if (ReadKind(submsg, &k)) {
              out->reliability_kind = k;
              out->has_reliability = true;
            }
            break;
          }
          case dds::PID_DURABILITY: {
            uint32_t k = 0;
            if (ReadKind(submsg, &k)) {
              out->durability_kind = k;
              out->has_durability = true;
            }
            break;
          }
          case dds::PID_OWNERSHIP: {
            uint32_t k = 0;
            if (ReadKind(submsg, &k)) {
              out->ownership_kind = k;
              out->has_ownership = true;
            }
            break;
          }
          case dds::PID_DESTINATION_ORDER: {
            uint32_t k = 0;
            if (ReadKind(submsg, &k)) {
              out->destination_order_kind = k;
              out->has_destination_order = true;
            }
            break;
          }
          case dds::PID_LIVELINESS: {
            // 结构：kind(uint32) + lease_duration(seconds int32 + fraction uint32)
            uint32_t k = 0;
            if (ReadKind(submsg, &k)) {
              out->liveliness_kind = k;
              out->has_liveliness = true;
              Time_t lease;
              if (ReadDuration(submsg, &lease)) {
                out->liveliness_lease = lease;
              }
            }
            break;
          }
          case dds::PID_DATA_REPRESENTATION: {
            uint32_t k = 0;
            if (ReadKind(submsg, &k)) {
              out->data_representation = k;
              out->has_data_representation = true;
            }
            break;
          }
          case dds::PID_DEADLINE: {
            Time_t dur;
            if (ReadDuration(submsg, &dur)) {
              out->deadline = dur;
              out->has_deadline = true;
            }
            break;
          }
          case dds::PID_LATENCY_BUDGET: {
            Time_t dur;
            if (ReadDuration(submsg, &dur)) {
              out->latency_budget = dur;
              out->has_latency_budget = true;
            }
            break;
          }
          case dds::PID_LIFESPAN: {
            Time_t dur;
            if (ReadDuration(submsg, &dur)) {
              out->lifespan = dur;
              out->has_lifespan = true;
            }
            break;
          }
          case dds::PID_TYPE_MAX_SIZE_SERIALIZED: {
            uint32_t v = 0;
            if (ReadKind(submsg, &v)) {
              out->type_max_size_serialized = v;
              out->has_type_max_size = true;
            }
            break;
          }
          case dds::PID_PROTOCOL_VERSION: {
            octet maj = 0, min = 0;
            if (CDRMessage::readOctet(submsg, &maj) &&
                CDRMessage::readOctet(submsg, &min)) {
              out->protocol_major = maj;
              out->protocol_minor = min;
              out->has_protocol_version = true;
            }
            break;
          }
          case dds::PID_VENDORID: {
            if (CDRMessage::readData(submsg, out->vendor_id, 2)) {
              out->has_vendor_id = true;
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

  out->parse_ok = ok;
  return ok;
}

size_t SedpParser::ParseMessage(const uint8_t* data, size_t len,
                                std::vector<SedpEndpointData>* out) {
  if (data == nullptr || out == nullptr || len < 24) return 0;
  if (std::memcmp(data, "RTPS", 4) != 0) return 0;

  const uint8_t ver_minor = data[5];
  size_t found = 0;
  size_t p = 20;

  while (p + 4 <= len) {
    const uint8_t sid = data[p];
    const uint16_t olen = static_cast<uint16_t>(data[p + 2]) |
                          (static_cast<uint16_t>(data[p + 3]) << 8);
    if (sid == 0x15) {   // DATA
      const size_t body = p + 4;
      const size_t prefix = DataTypePrefixSize(data + body, olen, ver_minor);
      if (body + prefix + 8 <= len) {
        // writerId 位于 prefix 内的固定偏移
        const size_t ids = body + (prefix - 18) + 2;
        const uint32_t wid = (static_cast<uint32_t>(data[ids + 4]) << 24) |
                             (data[ids + 5] << 16) |
                             (data[ids + 6] << 8) | data[ids + 7];
        if (wid == 0x000003c2u || wid == 0x000004c2u) {
          SedpEndpointData ed;
          if (ParseDataSubmessage(data + body, olen, wid, ver_minor, &ed)) {
            out->push_back(ed);
            ++found;
          }
        }
      }
    }
    if (olen == 0) break;
    p += 4 + olen;
  }
  return found;
}

}  // namespace rtps
}  // namespace stratum
