// 完全复刻生产序列，复现 pos/length 差 2
#include <cstdio>
#include <cstring>
#include <string>

#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/core/policy/ParameterSerializer.hpp>
#include <stratum/rtps/dds/core/policy/ParameterTypes.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>

using namespace eprosima::fastdds::rtps;
namespace dds = eprosima::fastdds::dds;

namespace {
CDRMessage_t* g = nullptr;
int step = 0;
void R(const char* what) {
  std::printf("  [%2d] %-36s pos=%4u length=%4u %s\n", step++, what, g->pos, g->length,
              (g->pos == g->length) ? "" : "  <<< 不一致");
}
}  // namespace

int main() {
  CDRMessage_t msg(4096);
  msg.msg_endian = LITTLEEND;
  g = &msg;

  // 完整 RTPS 头（20 字节）
  R("start");
  for (int i = 0; i < 20; ++i) CDRMessage::addOctet(&msg, 0);
  R("RTPS 头(20)");

  // INFO_TS
  CDRMessage::addOctet(&msg, 0x09);
  CDRMessage::addOctet(&msg, 0x01);
  CDRMessage::addUInt16(&msg, 8);
  CDRMessage::addInt32(&msg, 0);
  CDRMessage::addUInt32(&msg, 0);
  R("INFO_TS");

  // INFO_DST
  CDRMessage::addOctet(&msg, 0x0e);
  CDRMessage::addOctet(&msg, 0x01);
  CDRMessage::addUInt16(&msg, 12);
  for (int i = 0; i < 12; ++i) CDRMessage::addOctet(&msg, 0);
  R("INFO_DST");

  // DATA
  CDRMessage::addOctet(&msg, 0x15);
  CDRMessage::addOctet(&msg, 0x05);
  const uint32_t size_pos = msg.pos;
  CDRMessage::addUInt16(&msg, 0);
  const uint32_t body_start = msg.pos;
  R("DATA 头 + 占位");

  CDRMessage::addUInt16(&msg, 0);
  CDRMessage::addUInt16(&msg, 16);
  R("extraFlags+octetsToInlineQos");

  EntityId_t u(ENTITYID_UNKNOWN);
  EntityId_t w(ENTITYID_SPDP_BUILTIN_RTPSParticipant_WRITER);
  CDRMessage::addEntityId(&msg, &u);
  CDRMessage::addEntityId(&msg, &w);
  R("readerId+writerId");

  SequenceNumber_t sn(0, 1);
  CDRMessage::addSequenceNumber(&msg, &sn);
  R("writerSN");

  CDRMessage::addOctet(&msg, 0);
  CDRMessage::addOctet(&msg, 3);
  CDRMessage::addUInt16(&msg, 0);
  R("encapsulation");

  {
    dds::ParameterProtocolVersion_t p(dds::PID_PROTOCOL_VERSION, 2);
    p.protocolVersion = ProtocolVersion_t{2, 4};
    dds::ParameterSerializer<dds::ParameterProtocolVersion_t>::add_to_cdr_message(p, &msg);
    R("PID_PROTOCOL_VERSION");
  }
  {
    dds::ParameterVendorId_t p(dds::PID_VENDORID, 2);
    p.vendorId[0] = 1; p.vendorId[1] = 0x0f;
    dds::ParameterSerializer<dds::ParameterVendorId_t>::add_to_cdr_message(p, &msg);
    R("PID_VENDORID");
  }
  {
    dds::ParameterProductVersion_t p(dds::PID_PRODUCT_VERSION, 4);
    p.version.major = 3; p.version.minor = 6; p.version.patch = 2; p.version.tweak = 0;
    dds::ParameterSerializer<dds::ParameterProductVersion_t>::add_to_cdr_message(p, &msg);
    R("PID_PRODUCT_VERSION");
  }
  {
    dds::ParameterDomainId_t p(dds::PID_DOMAIN_ID, 4);
    p.domain_id = 5;
    dds::ParameterSerializer<dds::ParameterDomainId_t>::add_to_cdr_message(p, &msg);
    R("PID_DOMAIN_ID");
  }
  {
    GUID_t gg;
    dds::ParameterGuid_t p(dds::PID_PARTICIPANT_GUID, 16, gg);
    dds::ParameterSerializer<dds::ParameterGuid_t>::add_to_cdr_message(p, &msg);
    R("PID_PARTICIPANT_GUID");
  }
  {
    dds::ParameterNetworkConfigSet_t p(dds::PID_NETWORK_CONFIGURATION_SET, 4);
    p.netconfigSet = 0;
    dds::ParameterSerializer<dds::ParameterNetworkConfigSet_t>::add_to_cdr_message(p, &msg);
    R("PID_NETWORK_CONFIGURATION_SET");
  }
  {
    Locator_t loc;
    dds::ParameterLocator_t p(dds::PID_METATRAFFIC_UNICAST_LOCATOR, 24);
    p.locator = loc;
    dds::ParameterSerializer<dds::ParameterLocator_t>::add_to_cdr_message(p, &msg);
    R("PID_METATRAFFIC_UNICAST_LOCATOR");
  }
  {
    Locator_t loc;
    dds::ParameterLocator_t p(dds::PID_DEFAULT_UNICAST_LOCATOR, 24);
    p.locator = loc;
    dds::ParameterSerializer<dds::ParameterLocator_t>::add_to_cdr_message(p, &msg);
    R("PID_DEFAULT_UNICAST_LOCATOR");
  }
  {
    dds::ParameterBuiltinEndpointSet_t p(dds::PID_BUILTIN_ENDPOINT_SET, 4);
    p.endpointSet = 0x3f;
    dds::ParameterSerializer<dds::ParameterBuiltinEndpointSet_t>::add_to_cdr_message(p, &msg);
    R("PID_BUILTIN_ENDPOINT_SET");
  }
  {
    dds::ParameterTime_t p(dds::PID_PARTICIPANT_LEASE_DURATION, 8);
    p.time = Time_t(20, 0);
    dds::ParameterSerializer<dds::ParameterTime_t>::add_to_cdr_message(p, &msg);
    R("PID_PARTICIPANT_LEASE_DURATION");
  }
  {
    dds::ParameterString_t p(dds::PID_ENTITY_NAME, 0, std::string("stratum_recorder"));
    dds::ParameterSerializer<dds::ParameterString_t>::add_to_cdr_message(p, &msg);
    R("PID_ENTITY_NAME");
  }
  dds::ParameterSerializer<dds::Parameter_t>::add_parameter_sentinel(&msg);
  R("PID_SENTINEL");

  std::printf("\n回填前: pos=%u length=%u\n", msg.pos, msg.length);

  const uint16_t body_len = static_cast<uint16_t>(msg.pos - body_start);
  std::printf("body_len = %u\n", body_len);
  const uint32_t save = msg.pos;
  msg.pos = size_pos;
  CDRMessage::addUInt16(&msg, body_len);
  std::printf("回填后: pos=%u length=%u\n", msg.pos, msg.length);
  msg.pos = save;
  std::printf("恢复后: pos=%u length=%u\n", msg.pos, msg.length);
  return 0;
}
