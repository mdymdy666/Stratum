// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// RtpsReader 实现

#include <stratum/protocol/rtps/rtps_reader.h>
#include <stratum/protocol/rtps/rtps_packet_utils.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include <stratum/rtps/builtin/spdp_parser.h>
#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/core/policy/ParameterSerializer.hpp>
#include <stratum/rtps/dds/core/policy/ParameterTypes.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>
#include <stratum/rtps/messages/RTPSMessageCreator.hpp>

namespace stratum {
namespace protocol {
namespace rtps {

namespace {

// 生成符合 RTPS 9.3.1.1 的默认 GUID 前缀：
//   vendorId(2) | hostId(4) | processId(4) | counter(2)
//
// 【为什么不能用常量】此前是固定的 "STRATUM"+常量尾字节：同一台机器上
// **每次运行、每个进程拿到的 GUID 完全相同**。对端 Fast DDS 会把
// "新启动的回放端"认成"上一次那个 participant 又出现了"，沿用旧状态；
// 实测表现为"刚录完立刻回放"时最先发出的数据被当成过期数据丢弃
// （稳定 118/119），而另起一个干净会话（没有同 GUID 的前身）能到 200/200。
// 规范要求 GUID 前缀在本机唯一，hostId + processId 正是为此设计。
void FillDefaultGuidPrefix(uint8_t out[12]) {
  out[0] = 0x01;
  out[1] = 0x0f;                              // vendorId = eProsima
  const long host = ::gethostid();            // hostId
  std::memcpy(out + 2, &host, 4);
  const int32_t pid = static_cast<int32_t>(::getpid());   // processId
  std::memcpy(out + 6, &pid, 4);
  static std::atomic<uint16_t> counter{0};    // 同进程多次 Open 也不再重复
  const uint16_t c = counter.fetch_add(1, std::memory_order_relaxed);
  std::memcpy(out + 10, &c, 2);
}

}  // namespace


// 上游符号（CDRMessage 是**命名空间**，不能用 using-declaration，故整体引入）
using namespace eprosima::fastdds::rtps;

using ::stratum::rtps::AckNackInfo;
using ::stratum::rtps::DataTypePrefixSize;
using ::stratum::rtps::DefaultRecorderQos;
using ::stratum::rtps::HeartbeatInfo;
using ::stratum::rtps::MatchResult;
using ::stratum::rtps::MatchWriterToReader;
using ::stratum::rtps::SedpEndpointData;
using ::stratum::rtps::SedpKind;
using ::stratum::rtps::SedpParser;
using ::stratum::rtps::SpdpAnnounceConfig;
using ::stratum::rtps::SpdpAnnouncer;
using ::stratum::rtps::SpdpParser;
using ::stratum::rtps::SpdpParticipantData;
using ::stratum::rtps::SubmessageParser;
using eprosima::fastdds::rtps::CDRMessage_t;
using eprosima::fastdds::rtps::EntityId_t;
using eprosima::fastdds::rtps::GUID_t;
using eprosima::fastdds::rtps::GuidPrefix_t;
using eprosima::fastdds::rtps::Locator_t;
using eprosima::fastdds::rtps::SequenceNumber_t;
using eprosima::fastdds::rtps::SequenceNumberSet_t;
using eprosima::fastdds::rtps::SubmessageHeader_t;
namespace dds = eprosima::fastdds::dds;
namespace packet = ::stratum::protocol::rtps::packet;

namespace {

// RTPS 内建端点（大端数值）
constexpr uint32_t kSpdpWriter = 0x000100c2u;
constexpr uint32_t kSedpPubWriter = 0x000003c2u;
constexpr uint32_t kSedpPubReader = 0x000003c7u;
constexpr uint32_t kSedpSubWriter = 0x000004c2u;
constexpr uint32_t kSedpSubReader = 0x000004c7u;

// 我方 user reader 对外声明的 entityId。
//
// 单一来源：SEDP Subscription 的 `endpoint_entity` 与所有 ACKNACK 的
// `readerId` 都必须用它，否则对端会把 ACKNACK 视为无关报文而静默丢弃
// （详见 BuildSedpAnnouncementForReader 与 PairReaderEntity 的注释）。
// 0x00000004 = user reader with key、key 部分为 0 —— 与 Fast DDS 在
// DATA 报文里回填的 readerId 一致（可用 STRATUM_DUMP 的字节 dump 印证）。
constexpr uint32_t kUserReaderEntityId = 0x00000004u;      // NO_KEY reader
constexpr uint32_t kUserReaderWithKeyEntityId = 0x00000007u; // WITH_KEY reader

// 【topic_kind 由 entityId 决定 —— 这是 keyed 话题能否匹配的关键】
//
// Fast DDS 的 ProxyData 解析里明确按 entityId 末字节推导 topic_kind：
//   Writer（& 0x0F）: 0x03 → NO_KEY，0x02 → WITH_KEY   （WriterProxyData.cpp:1443-1450）
//   Reader（末字节）: 0x04 → NO_KEY，0x07 → WITH_KEY   （ReaderProxyData.cpp:1378-1385）
//
// 我们此前的 reader entityId 固定为 0x00000004（NO_KEY）。对 unkeyed 话题
// （hello_world）恰好一致、一切正常；一旦对端是 **keyed** writer
// （如带 @Key 的 ShapeType，writer 末字节 0x02），topic_kind 不匹配
// → 对端直接拒绝匹配，本端表现为"收到 Publication、qos 也不报错，但一条数据都没有"。
bool WriterIsKeyed(uint32_t writer_entity) {
  return (writer_entity & 0x0Fu) == 0x02u;
}

// submessage id
constexpr uint8_t kSubHeartbeat = 0x07;
constexpr uint8_t kSubInfoTs = 0x09;
constexpr uint8_t kSubInfoDst = 0x0e;
constexpr uint8_t kSubData = 0x15;

uint32_t EntityVal(const uint8_t* e) {
  return (static_cast<uint32_t>(e[0]) << 24) |
         (static_cast<uint32_t>(e[1]) << 16) |
         (static_cast<uint32_t>(e[2]) << 8) | e[3];
}

bool IsBuiltinWriter(uint32_t v) {
  return v == kSpdpWriter || v == kSedpPubWriter || v == kSedpSubWriter ||
         v == 0x000002c2u;   // SEDP topics writer
}

sockaddr_in ToSockaddr(const PeerAddress& a) {
  sockaddr_in s{};
  s.sin_family = AF_INET;
  s.sin_port = a.port;
  s.sin_addr.s_addr = a.ip;
  return s;
}

PeerAddress FromSockaddr(const sockaddr_in& s) {
  PeerAddress a;
  a.ip = s.sin_addr.s_addr;
  a.port = s.sin_port;
  return a;
}

// 组装 RTPS 头 + INFO_DST（可选）
void WriteHeaderAndDst(CDRMessage_t* msg, const uint8_t my_prefix[12],
                       const uint8_t* dst_prefix /* 12 字节或 nullptr */) {
  for (char c : {'R', 'T', 'P', 'S'}) CDRMessage::addOctet(msg, c);
  CDRMessage::addOctet(msg, 2);
  CDRMessage::addOctet(msg, 4);
  CDRMessage::addOctet(msg, 0x01);
  CDRMessage::addOctet(msg, 0x0f);
  for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
    CDRMessage::addOctet(msg, my_prefix[i]);
  }
  if (dst_prefix != nullptr) {
    CDRMessage::addOctet(msg, kSubInfoDst);
    CDRMessage::addOctet(msg, 0x01);
    CDRMessage::addUInt16(msg, 12);
    for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
      CDRMessage::addOctet(msg, dst_prefix[i]);
    }
  }
}

// 构造 SEDP Subscription 公告
//
// 参数顺序参照真实 Fast DDS 的 SEDP 公告（实测，见 tests/vectors/sedp_capture.bin）。
// 注意 PROTOCOL_VERSION / VENDORID 的长度必须是 4（实测值，规范字面是 2）——
// 写成 2 会导致 Fast DDS 静默跳过整个参数表。
std::vector<uint8_t> BuildSedpSubscription(const uint8_t my_prefix[12],
                                           const GUID_t& reader_guid,
                                           const GUID_t& participant_guid,
                                           const std::string& topic_name,
                                           const std::string& type_name,
                                           const Locator_t& unicast_locator,
                                           uint64_t sn) {
  CDRMessage_t msg(1024);
  msg.msg_endian = LITTLEEND;
  WriteHeaderAndDst(&msg, my_prefix, nullptr);

  // INFO_TS
  CDRMessage::addOctet(&msg, kSubInfoTs);
  CDRMessage::addOctet(&msg, 0x01);
  CDRMessage::addUInt16(&msg, 8);
  CDRMessage::addInt32(&msg, 0);
  CDRMessage::addUInt32(&msg, 0);

  // DATA
  CDRMessage::addOctet(&msg, kSubData);
  CDRMessage::addOctet(&msg, 0x05);   // E=1(小端) + D=1(含负载)
  const uint32_t size_pos = msg.pos;
  CDRMessage::addUInt16(&msg, 0);     // octetsToNextHeader 占位
  const uint32_t body_start = msg.pos;

  CDRMessage::addUInt16(&msg, 0);     // extraFlags
  CDRMessage::addUInt16(&msg, 16);    // octetsToInlineQos
  EntityId_t unknown(ENTITYID_UNKNOWN);
  CDRMessage::addEntityId(&msg, &unknown);
  EntityId_t sub_writer(kSedpSubWriter);
  CDRMessage::addEntityId(&msg, &sub_writer);
  SequenceNumber_t seq(static_cast<int32_t>(sn >> 32),
                       static_cast<uint32_t>(sn & 0xFFFFFFFFu));
  CDRMessage::addSequenceNumber(&msg, &seq);

  // encapsulation（小端 PL_CDR_LE）
  {
    bool ok = CDRMessage::addOctet(&msg, 0);
    ok &= CDRMessage::addOctet(&msg, static_cast<octet>(PL_CDR_LE - msg.msg_endian));
    ok &= CDRMessage::addUInt16(&msg, 0);
    if (!ok) return {};
  }

  constexpr uint16_t kGuidLen = 16;
  constexpr uint16_t kLocatorLen = 24;
  {
    dds::ParameterGuid_t p(dds::PID_ENDPOINT_GUID, kGuidLen, reader_guid);
    if (!dds::ParameterSerializer<dds::ParameterGuid_t>::add_to_cdr_message(p, &msg)) return {};
  }
  {
    dds::ParameterGuid_t p(dds::PID_PARTICIPANT_GUID, kGuidLen, participant_guid);
    if (!dds::ParameterSerializer<dds::ParameterGuid_t>::add_to_cdr_message(p, &msg)) return {};
  }
  {
    dds::ParameterString_t p(dds::PID_TOPIC_NAME, 0, topic_name);
    if (!dds::ParameterSerializer<dds::ParameterString_t>::add_to_cdr_message(p, &msg)) return {};
  }
  {
    dds::ParameterString_t p(dds::PID_TYPE_NAME, 0, type_name);
    if (!dds::ParameterSerializer<dds::ParameterString_t>::add_to_cdr_message(p, &msg)) return {};
  }
  {
    dds::ParameterLocator_t p(dds::PID_UNICAST_LOCATOR, kLocatorLen);
    p.locator = unicast_locator;
    if (!dds::ParameterSerializer<dds::ParameterLocator_t>::add_to_cdr_message(p, &msg)) return {};
  }
  {
    // 长度 4：实测真实 Fast DDS 取值（规范字面为 2）
    dds::ParameterProtocolVersion_t p(dds::PID_PROTOCOL_VERSION, 4);
    p.protocolVersion = ProtocolVersion_t{2, 4};
    if (!dds::ParameterSerializer<dds::ParameterProtocolVersion_t>::add_to_cdr_message(p, &msg)) return {};
  }
  {
    dds::ParameterVendorId_t p(dds::PID_VENDORID, 4);
    p.vendorId[0] = 0x01;
    p.vendorId[1] = 0x0f;
    if (!dds::ParameterSerializer<dds::ParameterVendorId_t>::add_to_cdr_message(p, &msg)) return {};
  }
  if (!dds::ParameterSerializer<dds::Parameter_t>::add_parameter_sentinel(&msg)) return {};

  // 回填 octetsToNextHeader —— 必须直接写 buffer：
  // CDRMessage::addUInt16 会 `length += 2` 而污染消息长度（实测导致
  // 消息尾部多 2 字节、接收方报 SubmessageHeader too short）。
  const uint16_t body_len = static_cast<uint16_t>(msg.pos - body_start);
  const octet* o = reinterpret_cast<const octet*>(&body_len);
  msg.buffer[size_pos] = o[0];
  msg.buffer[size_pos + 1] = o[1];

  return std::vector<uint8_t>(msg.buffer, msg.buffer + msg.length);
}

std::vector<uint8_t> BuildHeartbeatPacket(const uint8_t my_prefix[12],
                                          const uint8_t dst_prefix[12],
                                          uint32_t writer_id, uint32_t reader_id,
                                          int64_t first_sn, int64_t last_sn) {
  CDRMessage_t hb(256);
  hb.msg_endian = LITTLEEND;
  WriteHeaderAndDst(&hb, my_prefix, dst_prefix);
  CDRMessage::addOctet(&hb, kSubHeartbeat);
  CDRMessage::addOctet(&hb, 0x01);
  CDRMessage::addUInt16(&hb, 28);
  EntityId_t rid(reader_id);
  EntityId_t wid(writer_id);
  CDRMessage::addEntityId(&hb, &rid);
  CDRMessage::addEntityId(&hb, &wid);
  SequenceNumber_t f(static_cast<int32_t>(first_sn >> 32),
                     static_cast<uint32_t>(first_sn & 0xFFFFFFFF));
  SequenceNumber_t l(static_cast<int32_t>(last_sn >> 32),
                     static_cast<uint32_t>(last_sn & 0xFFFFFFFF));
  CDRMessage::addSequenceNumber(&hb, &f);
  CDRMessage::addSequenceNumber(&hb, &l);
  CDRMessage::addInt32(&hb, 1);
  return std::vector<uint8_t>(hb.buffer, hb.buffer + hb.length);
}


namespace {

// 用共享的 packet::BuildSedpAnnouncement 构造 Reader 侧订阅公告。
// 不在此处重复实现报文格式 —— 见 rtps_packet_utils.h 的说明。
std::vector<uint8_t> BuildSedpAnnouncementForReader(
    const uint8_t prefix[12], const GUID_t& reader_guid,
    const GUID_t& participant_guid, const std::string& topic_name,
    const std::string& type_name, uint16_t user_port, uint64_t sn,
    uint32_t requested_reliability, uint32_t requested_durability,
    const std::vector<uint32_t>& requested_representations,
    uint32_t target_writer_entity, const std::string& advertised_address) {
  packet::EndpointAnnouncement a;
  std::memcpy(a.prefix, prefix, 12);
  a.writer_entity = packet::kEntitySedpSubWriter;   // 订阅公告由 sub writer 承载
  // 【必须与 ACKNACK 用的 readerId 完全一致】
  //
  // 这里曾经写 `reader_guid.entityId.value[3] == 0 ? kEntityUserReader
  // : reader_guid.entityId.value[3]` —— `value[3]` 是**单个字节**，于是
  // 声明出去的 reader 变成了 0x00000004（丢掉 key 部分），而 ACKNACK 里
  // 仍回 0x00000104。对端的 writer 只认它学到的那个 readerId，
  // 于是**我们的 ACKNACK 被静默忽略**：history 永不释放，上游 publisher
  // 只发出第一条就停（实测 SENT=1）。
  // Fast DDS 的 DATA 报文里 readerId 正是 0x00000004，可作为交叉印证。
  // 与对端 writer 的 topic_kind 对齐（见 WriterIsKeyed 说明）。
  a.endpoint_entity = WriterIsKeyed(target_writer_entity)
                          ? kUserReaderWithKeyEntityId
                          : kUserReaderEntityId;
  a.topic_name = topic_name;
  a.type_name = type_name;
  a.unicast_port = user_port;
  // 【必须与 SPDP 声明同一个地址】此前没设置该字段，构造时会回退到
  // DetectLocalAddress()（本机探测到的真实地址）。在 NAT + UDP 中继拓扑下，
  // 真实地址（如 WSL 的 172.17.x.x）**对端根本不可达**，于是对端把样本
  // 全发到这个黑洞地址：表现为"发现成功、SEDP 正常，但一条数据都收不到"，
  // 且中继统计显示对端压根没往中继发数据（实测 to_wsl=0）。
  a.unicast_address = advertised_address;
  // 【声明必须与内部请求一致】
  //
  // 曾经无论 `config_.requested_qos` 是什么，都在 Subscription 里声明
  // RELIABLE + VOLATILE。后果：本端按 BEST_EFFORT 判定"兼容"，
  // 而对端 Fast DDS 按声明看到"RELIABLE reader"，于是
  // **BEST_EFFORT writer 判定不兼容、根本不匹配**
  // （实测：writer=best_effort 时 publisher 的 on_publication_matched 从不触发，
  //  sent=10 但 matched_now=0，本端一条也收不到）。
  // 两侧用同一份取值判定，才不会出现"我以为兼容、对端认为不兼容"。
  a.reliability = requested_reliability;
  a.durability = requested_durability;
  // 数据表示同样必须与内部请求一致（否则对端按声明判定，见结构体注释）。
  a.data_representations = requested_representations;
  a.include_qos = true;
  a.sn = sn;
  return packet::BuildSedpAnnouncement(a);
}

}  // namespace

}  // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

RtpsReader::RtpsReader() { config_.requested_qos = DefaultRecorderQos(); }

RtpsReader::~RtpsReader() { Stop(); }

Status RtpsReader::Open(const RtpsReaderConfig& config) {
  if (fd_meta_ >= 0 || fd_user_ >= 0) {
    return Status::InvalidArgument("RtpsReader 已打开");
  }
  config_ = config;
  if (config_.requested_qos.data_representations.empty()) {
    config_.requested_qos = DefaultRecorderQos();
  }

  // GUID 前缀：前 2 字节必须是 vendorId（RTPS 9.3.1.1）。
  // 实测用 "ST" 开头会被 Fast DDS 判为未知厂商而忽略。
  if (config_.guid_prefix.size() == 12) {
    std::memcpy(my_prefix_, config_.guid_prefix.data(), 12);
  } else {
    uint8_t def[12] = {0};
    FillDefaultGuidPrefix(def);
    std::memcpy(my_prefix_, def, 12);
  }

  const uint16_t meta_port =
      config_.metatraffic_port != 0
          ? config_.metatraffic_port
          : static_cast<uint16_t>(9050 + config_.domain_id * 10);
  const uint16_t user_port =
      config_.user_port != 0
          ? config_.user_port
          : static_cast<uint16_t>(meta_port + 1);

  // 组装 SPDP 公告
  const std::string adv = config_.advertised_address.empty()
                              ? packet::DetectLocalAddress()
                              : config_.advertised_address;
  advertised_address_ = adv;   // SEDP 公告也要用同一个地址
  SpdpAnnounceConfig sc;
  std::memcpy(sc.guid_prefix.value, my_prefix_, 12);
  sc.domain_id = config_.domain_id;
  sc.builtin_endpoints = 0x000f0c3f;   // 实测真实 Fast DDS 取值
  sc.participant_name = config_.participant_name;
  sc.metatraffic_unicast.push_back(
      ::stratum::rtps::MakeDefaultUdpv4Locator(adv.c_str(), meta_port));
  sc.default_unicast.push_back(
      ::stratum::rtps::MakeDefaultUdpv4Locator(adv.c_str(), user_port));
  spdp_payload_ = ::stratum::rtps::BuildSpdpAnnouncement(sc);
  if (spdp_payload_.empty()) {
    last_error_ = "组装 SPDP 公告失败";
    return Status::Internal(last_error_);
  }

  my_participant_guid_.guidPrefix = GuidPrefix_t::unknown();
  std::memcpy(my_participant_guid_.guidPrefix.value, my_prefix_, 12);
  my_participant_guid_.entityId = EntityId_t(ENTITYID_RTPSParticipant);
  my_reader_guid_ = my_participant_guid_;
  my_reader_guid_.entityId = EntityId_t(0x00000104u);   // user reader

  const Status s = OpenSockets();
  if (!s.ok()) return s;
  return Status::OK();
}

Status RtpsReader::OpenSockets() {
  // 接收 socket 的构造。除了 bind，还必须**加入 SPDP 组播组**：
  // 其他参与者的 SPDP 公告发往 239.255.0.1:7400+domain*250，
  // 不加入就收不到任何人的公告，也就无从发现对端。
  //
  // 实测症状：domain 5（无 ROS 流量、只有我方参与者）下 Reader 收到
  // 0 个包（pkt=0 spdp=0 peers=0），与同域内的另一个我方进程完全无法通信。
  // domain 0 之所以"看起来正常"，是因为 Fast DDS 会向已知参与者**单播**
  // SPDP，掩盖了这个缺陷。
  //
  // 同一缺陷也解释了回放失败：Writer 收不到 Fast DDS 的 SEDP Subscription，
  // 因而不知道对端 Reader 的数据接收地址，数据发不出去。
  auto make_rx = [](uint16_t port) -> int {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    // 【加大接收缓冲】高速接收时内核默认缓冲（~208 KB）会迅速填满并
    // 静默丢包 —— 这一点已由 /proc/net/snmp 的 RcvbufErrors 直接证实
    // （回放 300 条 8 KB 样本时 RcvbufErrors +317，而 Stratum 确实发出去了）。
    // 内核会把该值限制到 net.core.rmem_max（通常 208 KB）的 2 倍以内，
    // 所以同时建议调大 rmem_max（见 design/15-test-report.md 的说明）。
    {
      int rcvbuf = 8 * 1024 * 1024;
      // 优先用 SO_RCVBUFFORCE：有 CAP_NET_ADMIN 时它能**绕过** net.core.rmem_max
      // 把缓冲真正设到请求值；没有权限时该调用失败，再回退普通 SO_RCVBUF。
      // 背景：rmem_max 默认仅 212992（208KB），普通 SO_RCVBUF 会被静默截断，
      // 高突发下直接表现为丢包（实测多 topic 大数据量只送达 87%）。
      bool forced = false;
#ifdef SO_RCVBUFFORCE
      forced = (::setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf,
                             sizeof(rcvbuf)) == 0);
#endif
      if (!forced) {
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
      }
      // 【必须回报实际值】内核会把 SO_RCVBUF 静默截断到 net.core.rmem_max
      // （实测该值默认仅 212992 = 208KB），高突发下就成了丢包的根因，
      // 而调用方从返回值看不出来。打印实际生效值，让这类问题当场可见。
      {
        int actual = 0;
        socklen_t alen = sizeof(actual);
        if (::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &alen) == 0 &&
            actual < rcvbuf) {
          std::fprintf(stderr,
                       "[reader] 警告：SO_RCVBUF 请求 %d 字节，实际生效 %d 字节"
                       "（受 net.core.rmem_max 限制，建议 sysctl -w "
                       "net.core.rmem_max=67108864）\n", rcvbuf, actual);
          std::fflush(stderr);
        }
      }
      // 组播/单播都要：让内核即使在高突发下也有足够排队空间。
    }
    if (fd < 0) return -1;
    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
      ::close(fd);
      return -1;
    }
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = ::inet_addr("239.255.0.1");
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    return fd;
  };

  const uint16_t meta_port =
      config_.metatraffic_port != 0
          ? config_.metatraffic_port
          : static_cast<uint16_t>(9050 + config_.domain_id * 10);
  const uint16_t user_port =
      config_.user_port != 0 ? config_.user_port
                             : static_cast<uint16_t>(meta_port + 1);

  fd_meta_ = make_rx(meta_port);
  fd_user_ = make_rx(user_port);
  if (fd_meta_ < 0 || fd_user_ < 0) {
    CloseSockets();
    last_error_ = "绑定端口失败（" + std::to_string(meta_port) + "/" +
                  std::to_string(user_port) + "）";
    return Status::IOError(last_error_);
  }
  // ---------------------------------------------------------------------
  // SPDP 组播监听 socket —— 【必须】
  // ---------------------------------------------------------------------
  // SPDP 公告发往组播地址 239.255.0.1、端口 **7400 + domain*250**。
  // 它既不是我们的 metatraffic 端口，也不是 user 端口，因此必须单独
  // 绑定该端口并加入组播组，否则收不到任何新参与者的自我介绍。
  //
  // 实测症状：domain 5 下两个我方进程（Reader 9110 / Writer 9100）
  // 互相完全不可见（pkt=0 spdp=0 peers=0），因为双方都只在
  // metatraffic 端口上监听，而彼此还不知道对方的单播地址。
  //
  // domain 0 之所以"看起来正常"，是因为 Fast DDS 在收到我们的 SPDP 后
  // 会**单播**回送，掩盖了这个缺陷；而当对端也是我方实现时，
  // 没有任何一方会主动单播，发现流程彻底停摆。
  {
    const uint16_t spdp_port =
        static_cast<uint16_t>(7400 + config_.domain_id * 250);
    fd_spdp_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_spdp_ >= 0) {
      int reuse = 1;
      ::setsockopt(fd_spdp_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
      sockaddr_in sa{};
      sa.sin_family = AF_INET;
      sa.sin_port = htons(spdp_port);
      sa.sin_addr.s_addr = htonl(INADDR_ANY);
      if (::bind(fd_spdp_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        ::close(fd_spdp_);
        fd_spdp_ = -1;
      } else {
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = ::inet_addr("239.255.0.1");
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        ::setsockopt(fd_spdp_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                     sizeof(mreq));
      }
    }
  }

  fd_tx_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_tx_ < 0) {
    CloseSockets();
    last_error_ = "创建发送 socket 失败";
    return Status::IOError(last_error_);
  }
  return Status::OK();
}

void RtpsReader::CloseSockets() {
  if (fd_spdp_ >= 0) { ::close(fd_spdp_); fd_spdp_ = -1; }
  if (fd_meta_ >= 0) { ::close(fd_meta_); fd_meta_ = -1; }
  if (fd_user_ >= 0) { ::close(fd_user_); fd_user_ = -1; }
  if (fd_tx_ >= 0) { ::close(fd_tx_); fd_tx_ = -1; }
}

Status RtpsReader::Start(UserDataCallback callback) {
  if (running_.load(std::memory_order_acquire)) {
    return Status::InvalidArgument("RtpsReader 已在运行");
  }
  if (fd_meta_ < 0 || fd_user_ < 0) {
    return Status::InvalidArgument("RtpsReader 尚未 Open");
  }
  callback_ = std::move(callback);
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this]() { ReceiveLoop(); });
  return Status::OK();
}

Status RtpsReader::Stop() {
  if (!running_.load(std::memory_order_acquire)) {
    CloseSockets();
    return Status::OK();
  }
  // 先置停止标志再 join —— 反过来会因 ReceiveLoop 仍在 poll 而挂住
  stop_requested_.store(true, std::memory_order_release);
  if (thread_.joinable()) thread_.join();
  running_.store(false, std::memory_order_release);
  CloseSockets();
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 接收主循环
// ---------------------------------------------------------------------------

void RtpsReader::ReceiveLoop() {
  SubmessageParser subm;
  SpdpParser spdp;
  SedpParser sedp;

  pollfd fds[3];
  fds[0].fd = fd_spdp_;      // SPDP 组播（新参与者自我介绍）
  fds[0].events = POLLIN;
  fds[1].fd = fd_meta_;      // 单播：SEDP / HEARTBEAT / ACKNACK
  fds[1].events = POLLIN;
  fds[2].fd = fd_user_;      // 用户数据
  fds[2].events = POLLIN;

  // 公告节流：announce_interval_ms 决定多久发一次
  const auto announce_interval =
      std::chrono::milliseconds(config_.announce_interval_ms == 0
                                    ? 200
                                    : config_.announce_interval_ms);
  auto last_announce = std::chrono::steady_clock::now() -
                       announce_interval;   // 首轮立即发

  while (!stop_requested_.load(std::memory_order_acquire)) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_announce >= announce_interval) {
      AnnounceOnce();
      last_announce = now;
      // 周期性重发已发出的 SEDP Subscription。
      //
      // 【为什么必须】Subscription 只发一次时，若该 UDP 包丢失，
      // 对端 Writer 就永远学不到我方 Reader 的数据接收地址，
      // 只能把数据发往 metatraffic 端口（或干脆发不出）。
      // 实测：Reader 侧 subs=1（仅一次），而 Writer 侧 sedp_sub=0 ——
      // 即那唯一一次没有到达。
      // DDS 的发现协议本身要求这类公告可重复投递（幂等），
      // 对端按 GUID 去重，因此重发是安全且必要的。
      ReannounceSubscriptions();
    }

    if (::poll(fds, 3, 100) <= 0) continue;

    for (int idx = 0; idx < 3; ++idx) {
      if ((fds[idx].revents & POLLIN) == 0) continue;

      uint8_t buf[65536];
      sockaddr_in src{};
      socklen_t sl = sizeof(src);
      const ssize_t n = ::recvfrom(fds[idx].fd, buf, sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&src), &sl);
      if (n <= 0) continue;
      st_packets_.fetch_add(1, std::memory_order_relaxed);
      if (n < 20 || std::memcmp(buf, "RTPS", 4) != 0) continue;

      const uint8_t ver_minor = buf[5];
      const PeerAddress src_addr = FromSockaddr(src);

      CDRMessage_t msg(static_cast<uint32_t>(n) + 8);
      std::memcpy(msg.buffer, buf, n);
      msg.length = static_cast<uint32_t>(n);
      msg.pos = 20;

      while (msg.pos + 4 <= msg.length) {
        const uint32_t before = msg.pos;
        SubmessageHeader_t smh;
        if (!subm.ReadSubmessageHeader(&msg, &smh)) break;
        const uint32_t body = msg.pos;

        if (smh.submessageId == kSubInfoTs) {
          HandleInfoTimestamp(buf, static_cast<size_t>(n), body);
        } else if (smh.submessageId == kSubHeartbeat) {
          HandleHeartbeat(buf, static_cast<size_t>(n), body, smh, src_addr);
        } else if (smh.submessageId == packet::kSubDataFrag) {
          HandleDataFrag(buf, static_cast<size_t>(n), body);
        } else if (smh.submessageId == kSubData) {
          const size_t prefix = DataTypePrefixSize(msg.buffer + body,
                                                   msg.length - body, ver_minor);
          const size_t ids = body + (prefix - 18) + 2;
          if (ids + 8 <= msg.length) {
            const uint32_t wid = EntityVal(msg.buffer + ids + 4);
            // 记录已收到的 SN（含内建 writer）：ACKNACK 要据此报告真实缺口，
            // 而不是每次都把整段区间说成缺失（见 rtps_reader.h 的说明）。
            uint32_t sn_low = 0;
            bool first_time = true;   // 拿不到 SN 时按首次处理（保守：不丢数据）
            if (ids + 16 <= msg.length) {
              // writerSN 是 8 字节：高 4 字节在 ids+8、低 4 字节在 ids+12。
              // 旧实现读的是 ids+8（高位），于是**所有样本的 sequence 都是 0**，
              // 数据质量报告里的序号缺口检测随之完全失效。
              std::memcpy(&sn_low, msg.buffer + ids + 12, 4);
              first_time = RecordReceivedSn(wid, sn_low);
            }
            if (wid == kSpdpWriter) {
              HandleSpdp(buf, static_cast<size_t>(n));
            } else if (wid == kSedpPubWriter) {
              HandleSedpPublication(buf, static_cast<size_t>(n));
            } else if (!IsBuiltinWriter(wid)) {
              // 用户数据
              // STRATUM_DUMP=1：打印关键偏移与读出的 SN。
              // "eager ack 有没有触发"取决于 sn_low，必须直接看值。
              if (std::getenv("STRATUM_DUMP") != nullptr) {
                std::fprintf(stderr,
                             "[user] wid=0x%08x ids=%zu msg_len=%u sn_low=%u flags=0x%02x\n",
                             wid, ids, msg.length, sn_low, smh.flags);
                std::fprintf(stderr, "       bytes@ids-8:");
                for (int k = -8; k < 24; ++k) {
                  const long off = static_cast<long>(ids) + k;
                  if (off >= 0 && off < static_cast<long>(msg.length)) {
                    std::fprintf(stderr, " %02x", msg.buffer[off]);
                  }
                }
                std::fputc('\n', stderr);
                std::fflush(stderr);
              }
              // 【按 (writer, SN) 去重】对端会因 ACK 不及时而重传，同一份数据
              // 可能到达多次。若每次都交付，录制端会把同一份数据反复录进去
              // —— 实测压测时 2000 条被录成 58497 条（29 倍放大）。
              if (!first_time) {
                st_duplicates_.fetch_add(1, std::memory_order_relaxed);
              } else {
                st_samples_.fetch_add(1, std::memory_order_relaxed);
              }

              UserData ud;
              ud.reception_timestamp_ns =
                  static_cast<TimestampNs>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count());
              ud.source_timestamp_ns = last_info_ts_ns_;
              std::memcpy(ud.writer_guid.guidPrefix.value, buf + 8, 12);
              ud.writer_guid.entityId = EntityId_t(wid);

              // writerSN：8 字节（高 4 在 ids+8、**低 4 在 ids+12**）。
              // 读 ids+8 拿到的是恒为 0 的高位 —— 那会让所有样本 sequence=0。
              if (ids + 16 <= msg.length) {
                uint32_t low = 0;
                std::memcpy(&low, msg.buffer + ids + 12, 4);
                ud.sequence = low;
              }

              // payload 起点：跳过 prefix，若有 inlineQos 再跳过参数表；
              // 顺便**提取 PID_KEY_HASH(0x0070)** 作为实例键。
              //
              // keyed 话题的实例标识就在这里：Fast DDS 会把 key 的 16 字节哈希
              // 放进 inlineQos。此前只"跳过"参数表、不解析内容，于是
              // `ReceivedSample::instance_key` 永远是空的 —— 存储层与
              // Recorder/Player 都已贯通该字段，唯独 RTPS 层把它丢了。
              size_t p = body + prefix;
              if ((smh.flags & 0x02) != 0) {
                int guard = 0;
                while (p + 4 <= msg.length && guard++ < 256) {
                  const uint16_t pid = static_cast<uint16_t>(msg.buffer[p]) |
                                       (static_cast<uint16_t>(msg.buffer[p + 1]) << 8);
                  const uint16_t plen = static_cast<uint16_t>(msg.buffer[p + 2]) |
                                        (static_cast<uint16_t>(msg.buffer[p + 3]) << 8);
                  if (pid == 0x0070 && plen == 16 && p + 4 + 16 <= msg.length) {
                    ud.instance_key.assign(
                        reinterpret_cast<const char*>(msg.buffer + p + 4), 16);
                  }
                  p += 4 + ((plen + 3) & ~3u);
                  if (pid == 0x0001) break;
                }
              }
              if (p < msg.length) {
                ud.payload.assign(reinterpret_cast<const char*>(msg.buffer + p),
                                  msg.length - p);
              }

              // 按 writerGUID 匹配已发现的 Writer，补全话题/类型
              {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& kv : writers_) {
                  const RemoteWriter& w = kv.second;
                  if (std::memcmp(w.guid.guidPrefix.value,
                                  ud.writer_guid.guidPrefix.value, 12) == 0 &&
                      w.guid.entityId == ud.writer_guid.entityId) {
                    ud.topic_name = w.topic_name;
                    ud.type_name = w.type_name;
                    break;
                  }
                }
              }

              if (first_time && callback_ && !callback_(ud)) {
                stop_requested_.store(true, std::memory_order_release);
              }

              // 立即确认（eager ack）：对端的可靠 writer 默认 history depth=1，
              // 若等到下一次 HEARTBEAT（默认 3 秒）才确认，第 2 条数据就会因
              // history 未释放而写失败 —— 实测上游 hello_world publisher
              // **只发出第一条就停止**。真实 DDS 实现同样会即时确认。
              if (sn_low > 0) {
                SendAcknackForReceived(wid, sn_low);
              }
            }
          }
        }

        if (smh.is_last || body + smh.submessageLength <= before) break;
        msg.pos = body + smh.submessageLength;
      }
    }
  }
}

void RtpsReader::AnnounceOnce() {
  if (fd_tx_ < 0 || spdp_payload_.empty()) return;
  const uint16_t port = static_cast<uint16_t>(
      7400 + config_.domain_id * 250);
  // 【跨主机发现】同 writer：NAT 下组播不通，需向单播对端列表发 SPDP。
  std::vector<std::string> targets = {"239.255.0.1", "127.0.0.1"};
  for (const auto& ip : config_.initial_peers) {
    if (!ip.empty()) targets.push_back(ip);
  }
  for (const auto& ip : targets) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = ::inet_addr(ip.c_str());
    ::sendto(fd_tx_, spdp_payload_.data(), spdp_payload_.size(), 0,
             reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
  }
}

// ---------------------------------------------------------------------------
// 子消息处理
// ---------------------------------------------------------------------------

void RtpsReader::HandleInfoTimestamp(const uint8_t* packet, size_t len,
                                     uint32_t body) {
  if (body + 8 > len) return;
  int32_t sec = 0;
  uint32_t frac = 0;
  std::memcpy(&sec, packet + body, 4);
  std::memcpy(&frac, packet + body + 4, 4);
  // RTPS 时间戳：seconds + fraction(1/2^32 秒)
  const uint64_t ns =
      static_cast<uint64_t>(sec) * 1000000000ULL +
      (static_cast<uint64_t>(frac) * 1000000000ULL >> 32);
  last_info_ts_ns_ = static_cast<TimestampNs>(ns);
}

void RtpsReader::HandleSpdp(const uint8_t* packet, size_t len) {
  st_spdp_.fetch_add(1, std::memory_order_relaxed);
  SpdpParser parser;
  std::vector<SpdpParticipantData> pds;
  parser.ParseMessage(packet, len, &pds);

  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& pd : pds) {
  // 跳过**自己**发出的公告。
  //
  // 组播会把本机发出的包回环给自己，因此若不按 GUID 前缀过滤，
  // 就会把自己当成对端：peers_ 里只有一个"自己"，
  // 于是 SEDP 公告与会话数据全部发给自己，永远无法与真正的对端通信。
  //
  // 实测症状：domain 5 下 Reader 的 spdp 计数高达 490（自己 + 对端各半），
  // 但 peers=1 且 sedp_pub=0 —— 说明唯一的 peer 就是自己。
    if (std::memcmp(pd.guid.guidPrefix.value, my_prefix_, 12) == 0) {
      continue;
    }
    for (const Locator_t& loc : pd.metatraffic_unicast_locators) {
      Peer p;
      p.addr.port = htons(static_cast<uint16_t>(loc.port));
      std::memcpy(&p.addr.ip, &loc.address[12], 4);
      std::memcpy(p.guid_prefix, pd.guid.guidPrefix.value, 12);
      bool dup = false;
      for (const auto& e : peers_) {
        if (e.addr.port == p.addr.port && e.addr.ip == p.addr.ip) dup = true;
      }
      if (!dup) peers_.push_back(p);
    }
  }
}

void RtpsReader::HandleSedpPublication(const uint8_t* packet, size_t len) {
  st_sedp_pub_.fetch_add(1, std::memory_order_relaxed);
  SedpParser parser;
  std::vector<SedpEndpointData> eds;
  parser.ParseMessage(packet, len, &eds);

  for (const auto& ed : eds) {
    // 话题过滤
    if (!config_.topic_filter.empty() &&
        config_.topic_filter.count(ed.topic_name) == 0) {
      continue;
    }
    // QoS 兼容判定（规则移植自上游 EDP::valid_matching）
    const MatchResult mr =
        MatchWriterToReader(ed, config_.requested_qos);
    if (!mr.matched) {
      st_qos_rejected_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    const std::string key = ed.topic_name + "|" + ed.type_name;
    RemoteWriter rw;
    rw.topic_name = ed.topic_name;
    rw.type_name = ed.type_name;
    rw.guid = ed.endpoint_guid;
    rw.endpoint = ed;

    // STRATUM_DUMP=1：打印解析出的 writer 及其 user locator。
    // ACKNACK 必须发到这些 locator（而不是 SPDP 的 metatraffic 端口），
    // 因此"有没有解析到 locator"是录制方向能否持续的关键证据。
    if (std::getenv("STRATUM_DUMP") != nullptr) {
      std::fprintf(stderr, "[edp] writer=0x%08x topic=\"%s\" type=\"%s\" locators=%zu:",
                   EntityVal(ed.endpoint_guid.entityId.value),
                   ed.topic_name.c_str(), ed.type_name.c_str(),
                   ed.unicast_locators.size());
      for (const Locator_t& loc : ed.unicast_locators) {
        std::fprintf(stderr, " %u.%u.%u.%u:%u", loc.address[12], loc.address[13],
                     loc.address[14], loc.address[15],
                     static_cast<unsigned>(loc.port));
      }
      std::putc(10, stderr);
      std::fflush(stderr);
    }

    bool need_subscribe = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = writers_.find(key);
      if (it == writers_.end()) {
        rw.subscribed = false;
        writers_[key] = rw;
        need_subscribe = true;
      } else if (!it->second.subscribed) {
        need_subscribe = true;
      } else {
        continue;   // 已订阅
      }
    }
    if (need_subscribe) {
      PublishSubscription(rw);
    }
  }
}

void RtpsReader::PublishSubscription(const RemoteWriter& writer) {
  std::vector<Peer> peers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peers = peers_;
  }
  if (peers.empty()) return;

  const uint16_t user_port =
      config_.user_port != 0
          ? config_.user_port
          : static_cast<uint16_t>(9051 + config_.domain_id * 10);

  const uint64_t sn = ++subscription_sn_;
  const auto payload = BuildSedpAnnouncementForReader(
      my_prefix_, my_reader_guid_, my_participant_guid_, writer.topic_name,
      writer.type_name, user_port, sn,
      config_.requested_qos.reliability_kind,
      config_.requested_qos.durability_kind,
      config_.requested_qos.data_representations,
      EntityVal(writer.guid.entityId.value),
      advertised_address_);
  if (payload.empty()) return;

  for (const Peer& peer : peers) {
    const sockaddr_in sa = ToSockaddr(peer.addr);
    const int sent = ::sendto(fd_tx_, payload.data(), payload.size(), 0,
                              reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
    // 同一条数据再宣告一次 HEARTBEAT，促使对端确认
    const auto hb = BuildHeartbeatPacket(my_prefix_, peer.guid_prefix,
                                         kSedpSubWriter, 0x000004c7u,
                                         static_cast<int64_t>(sn),
                                         static_cast<int64_t>(sn));
    ::sendto(fd_tx_, hb.data(), hb.size(), 0,
             reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
  }
  st_subs_.fetch_add(1, std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(mutex_);
  const std::string key = writer.topic_name + "|" + writer.type_name;
  auto it = writers_.find(key);
  if (it != writers_.end()) it->second.subscribed = true;
}


// 重发所有已订阅 Writer 的 SEDP Subscription（幂等）。
// 见 ReceiveLoop 中调用处的说明：这是对 UDP 丢包的必需防护。
void RtpsReader::ReannounceSubscriptions() {
  std::vector<RemoteWriter> targets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : writers_) {
      if (kv.second.subscribed) targets.push_back(kv.second);
    }
  }
  if (targets.empty()) return;

  std::vector<Peer> peers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peers = peers_;
  }
  if (peers.empty()) return;

  const uint16_t user_port =
      config_.user_port != 0
          ? config_.user_port
          : static_cast<uint16_t>(9051 + config_.domain_id * 10);

  for (const RemoteWriter& w : targets) {
    const uint64_t sn = ++subscription_sn_;
    const auto payload = BuildSedpAnnouncementForReader(
        my_prefix_, my_reader_guid_, my_participant_guid_, w.topic_name,
        w.type_name, user_port, sn,
        config_.requested_qos.reliability_kind,
        config_.requested_qos.durability_kind,
        config_.requested_qos.data_representations,
        EntityVal(w.guid.entityId.value),
        advertised_address_);
    if (payload.empty()) continue;
    for (const Peer& peer : peers) {
      const sockaddr_in sa = ToSockaddr(peer.addr);
      ::sendto(fd_tx_, payload.data(), payload.size(), 0,
               reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
    }
    st_subs_.fetch_add(1, std::memory_order_relaxed);
  }
}

void RtpsReader::HandleHeartbeat(const uint8_t* packet, size_t len,
                                 uint32_t body,
                                 const SubmessageHeader_t& smh,
                                 const PeerAddress& src) {
  st_heartbeats_.fetch_add(1, std::memory_order_relaxed);

  // 【关键】用已移植并经真实流量验证的 ProcHeartbeat 解析，
  // 而不是手工按偏移 memcpy —— 后者在重构时曾导致 ACKNACK 完全无效
  // （对端不推送任何 SEDP，sedp_pub 恒为 0），而同一份逻辑用
  // ProcHeartbeat 时正常（tools/e2e_recorder.cc 实测 199 条用户数据）。
  CDRMessage_t msg(static_cast<uint32_t>(len) + 8);
  std::memcpy(msg.buffer, packet, len);
  msg.length = static_cast<uint32_t>(len);
  msg.pos = body;

  SubmessageParser subm;
  HeartbeatInfo hb;
  if (!subm.ProcHeartbeat(&msg, &smh, &hb)) return;

  const uint32_t writer_id = EntityVal(hb.writer_id.value);
  // ACKNACK 的 readerId 必须与心跳来源的 writer **配对**，否则对端不认这个
  // ACKNACK，数据永远不会被确认。
  //
  // 【曾经的缺陷】这里把任何非 SEDP writer 的心跳都映射成 kSedpPubReader：
  //   · 用户数据的心跳来自 user writer（0x00000103），
  //     却被回成 SEDP publications reader（0x000003c7）；
  //   · 真实 publisher（Fast DDS hello_world）因此把 ACKNACK 视为无关报文，
  //     第 1 条数据永远不确认 → writer history 占满 → **只发 1 条就停**。
  // 映射规则：内建 writer 用对应的内建 reader；
  // 用户 writer（末字节 0x03 = with-key writer）对应的 reader 末字节为 0x04。
  uint32_t reader_id = kSedpPubReader;
  if (writer_id == kSedpPubWriter) {
    reader_id = kSedpPubReader;
  } else if (writer_id == kSedpSubWriter) {
    reader_id = kSedpSubReader;
  } else if (!IsBuiltinWriter(writer_id)) {
    reader_id = (writer_id & 0xFFFFFF00u) | 0x04u;   // user reader with key
  }

  const int64_t first_sn = hb.first_sn.to64long();
  const int64_t last_sn = hb.last_sn.to64long();

  std::vector<Peer> peers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peers = peers_;
  }

  // ACKNACK 必须发到**学到的对端 metatraffic locator**，而不是收到包的源地址：
  // 源地址可能是 SPDP 组播地址，Fast DDS 不会在那里处理 ACKNACK。
  // （这个坑在 M3 与 M5 各踩过一次，勿再简化。）
  if (!peers.empty()) {
    for (const Peer& peer : peers) {
      SendAcknack(peer, writer_id, reader_id, first_sn, last_sn);
    }
  } else {
    Peer p;
    p.addr = src;
    std::memcpy(p.guid_prefix, packet + 8, 12);
    SendAcknack(p, writer_id, reader_id, first_sn, last_sn);
  }
}

// 记录一个已收到的 DATA 序列号。
//
// 内存受控：只保留每个 writer 最近的 kKeepWindow 个 SN。ACKNACK 只关心
// "当前还缺哪些"，过老的 SN 已经没有意义（对端也不会再重传那么早的数据）。
bool RtpsReader::RecordReceivedSn(uint32_t writer_entity, uint64_t sn) {
  constexpr uint64_t kKeepWindow = 4096;
  std::lock_guard<std::mutex> lock(mutex_);
  auto& set = received_sn_[writer_entity];
  const bool first_time = set.insert(sn).second;
  if (set.size() > kKeepWindow) {
    const uint64_t oldest = sn > kKeepWindow ? (sn - kKeepWindow) : 0;
    set.erase(set.begin(), set.lower_bound(oldest));
  }
  return first_time;
}

uint32_t RtpsReader::PairReaderEntity(uint32_t writer_entity) {
  if (writer_entity == kSedpPubWriter) {
    return kSedpPubReader;
  }
  if (writer_entity == kSedpSubWriter) {
    return kSedpSubReader;
  }
  if (IsBuiltinWriter(writer_entity)) {
    return kSedpPubReader;
  }
  // 用户 writer → **我们向对端声明的那个 user reader entityId**。
  // 必须与 SEDP Subscription 里 `endpoint_entity` 一致，否则对端不认这个
  // ACKNACK（见 BuildSedpAnnouncementForReader 的注释）。
  return WriterIsKeyed(writer_entity) ? kUserReaderWithKeyEntityId
                                      : kUserReaderEntityId;
}

void RtpsReader::SendAcknackForReceived(uint32_t writer_entity, uint64_t sn) {
  const uint32_t reader_id = PairReaderEntity(writer_entity);

  // base = sn + 1、位图为空 ⇒ "sn 及之前全部收到"的纯确认。
  const int64_t base = static_cast<int64_t>(sn + 1);
  const int64_t hi = static_cast<int64_t>(sn);

  // 【关键】ACKNACK 必须发到该 writer **自己的 user locator**
  // （它公告在 SEDP Publication 里，已随 RemoteWriter.endpoint 保存），
  // 而不是 SPDP 的 metatraffic locator。
  //
  // 对端的 DataWriter 在 user 端口上等确认；把 ACKNACK 发到 metatraffic
  // 端口等于没发 → writer history 永不释放 → 实测上游 hello_world
  // publisher **只发出第一条就停止**（有另一个正常订阅端陪跑时才继续）。
  Peer ack_peer;
  bool have_writer_locator = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : writers_) {
      const RemoteWriter& w = kv.second;
      if (EntityVal(w.guid.entityId.value) != writer_entity) {
        continue;
      }
      // 优先选**具体地址**：对端常把 0.0.0.0 也列进 locator 列表，
      // 它只在本机（内核当作 localhost）可用，跨机时必须用通告的具体 IP。
      for (const Locator_t& loc : w.endpoint.unicast_locators) {
        const bool any_addr = (loc.address[12] | loc.address[13] |
                               loc.address[14] | loc.address[15]) == 0;
        if (any_addr && have_writer_locator) {
          continue;   // 已有具体地址，不再被 0.0.0.0 覆盖
        }
        ack_peer.addr.port = htons(static_cast<uint16_t>(loc.port));
        std::memcpy(&ack_peer.addr.ip, &loc.address[12], 4);
        std::memcpy(ack_peer.guid_prefix, w.guid.guidPrefix.value, 12);
        have_writer_locator = true;
        if (!any_addr) {
          break;      // 具体地址已拿到，收工
        }
      }
      break;
    }
  }
  if (have_writer_locator) {
    SendAcknack(ack_peer, writer_entity, reader_id, base, hi);
    return;
  }

  // 回退：还没学到该 writer 的 user locator 时，发到学到的 metatraffic locator。
  std::vector<Peer> peers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peers = peers_;
  }
  for (const Peer& peer : peers) {
    SendAcknack(peer, writer_entity, reader_id, base, hi);
  }
}

void RtpsReader::SendAcknack(const Peer& peer, uint32_t writer_id,
                             uint32_t reader_id, int64_t first_sn,
                             int64_t last_sn) {
  CDRMessage_t ack(512);
  ack.msg_endian = LITTLEEND;
  WriteHeaderAndDst(&ack, my_prefix_, peer.guid_prefix);

  EntityId_t rid(reader_id);
  EntityId_t wid(writer_id);

  // 【按真实收到的 SN 报告缺口】
  //
  // ACKNACK 的语义是"我缺哪些"：
  //   base_sn = 所有小于它的 SN 都已收到；位图标记 >= base_sn 的缺失 SN。
  //
  // 旧实现把 HEARTBEAT 宣告的 [firstSN, lastSN] **整段标记为缺失**，
  // 这等于宣称"我一条都没收到"。后果：
  //   · 对端反复重传，并持续打印 `Inconsistent acknack received`；
  //   · 真实 publisher（Fast DDS hello_world）在这种 ACKNACK 下
  //     **只发第一条就停止**，录制方向完全不可用（实测 samples 停在 1）。
  // 现在的做法：从 firstSN 起找第一个缺口作为 base，位图只标记真正的缺口；
  // 若整段都收到了，base = lastSN + 1 且位图为空 —— 这就是一个纯确认。
  std::set<uint64_t> received;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = received_sn_.find(writer_id);
    if (it != received_sn_.end()) {
      received = it->second;
    }
  }

  const uint64_t lo = (first_sn > 0) ? static_cast<uint64_t>(first_sn) : 1ULL;
  const uint64_t hi = (last_sn >= first_sn) ? static_cast<uint64_t>(last_sn) : lo;
  uint64_t base = lo;
  while (base <= hi && received.count(base) > 0) {
    ++base;
  }

  SequenceNumberSet_t sns(SequenceNumber_t(0, static_cast<uint32_t>(base)));
  constexpr uint64_t kBitmapLimit = 256;   // BitmapRange 为 256 位
  uint64_t marks = 0;
  for (uint64_t sn = base; sn <= hi && marks < kBitmapLimit; ++sn) {
    if (received.count(sn) == 0) {
      sns.add(SequenceNumber_t(0, static_cast<uint32_t>(sn)));
      ++marks;
    }
  }

  if (!RTPSMessageCreator::addSubmessageAcknack(
          &ack, rid, wid, sns,
          static_cast<int32_t>(st_acks_.load(std::memory_order_relaxed)), false)) {
    return;
  }
  const sockaddr_in sa = ToSockaddr(peer.addr);
  // STRATUM_DUMP=1 时打印 ACKNACK 的**目标**与内容摘要：
  // "对端到底在哪等确认"必须看得见，否则只能靠猜（本轮就因此绕了远路）。
  if (std::getenv("STRATUM_DUMP") != nullptr) {
    char dip[32] = {0};
    ::inet_ntop(AF_INET, &sa.sin_addr, dip, sizeof(dip));
    std::fprintf(stderr,
                 "[ack] -> %s:%u reader=0x%08x writer=0x%08x base=%llu marks=%llu bytes=%u\n",
                 dip, static_cast<unsigned>(ntohs(sa.sin_port)), reader_id,
                 writer_id, static_cast<unsigned long long>(base),
                 static_cast<unsigned long long>(marks), ack.length);
    std::fflush(stderr);
  }
  if (::sendto(fd_tx_, ack.buffer, ack.length, 0,
               reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) > 0) {
    st_acks_.fetch_add(1, std::memory_order_relaxed);
  }
}

void RtpsReader::SendHeartbeat(const Peer& peer, uint32_t writer_id,
                               int64_t first_sn, int64_t last_sn) {
  const auto hb = BuildHeartbeatPacket(my_prefix_, peer.guid_prefix, writer_id,
                                       kSedpSubReader, first_sn, last_sn);
  const sockaddr_in sa = ToSockaddr(peer.addr);
  ::sendto(fd_tx_, hb.data(), hb.size(), 0,
           reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

std::vector<std::string> RtpsReader::DiscoveredTopics() const {
  std::vector<std::string> out;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& kv : writers_) out.push_back(kv.second.topic_name);
  return out;
}

size_t RtpsReader::SubscribedWriterCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t n = 0;
  for (const auto& kv : writers_) {
    if (kv.second.subscribed) ++n;
  }
  return n;
}

RtpsReader::Stats RtpsReader::GetStats() const {
  Stats s;
  s.packets_total = st_packets_.load(std::memory_order_relaxed);
  s.spdp_received = st_spdp_.load(std::memory_order_relaxed);
  s.sedp_publications = st_sedp_pub_.load(std::memory_order_relaxed);
  s.heartbeats = st_heartbeats_.load(std::memory_order_relaxed);
  s.acknacks_sent = st_acks_.load(std::memory_order_relaxed);
  s.subscriptions_sent = st_subs_.load(std::memory_order_relaxed);
  s.user_samples = st_samples_.load(std::memory_order_relaxed);
  s.duplicates = st_duplicates_.load(std::memory_order_relaxed);
  s.qos_rejected = st_qos_rejected_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    s.peers_learned = peers_.size();
  }
  return s;
}



// ---------------------------------------------------------------------------
// DATA_FRAG 重组
//
// 子消息体布局（RTPS 2.3 §9.6.3.6），自 body 起：
//   extraFlags(2) octetsToInlineQos(2) readerId(4) writerId(4) writerSN(8)
//   fragmentStartingNum(4) fragmentsInSubmessage(2) fragmentSize(2) sampleSize(4)
//   serializedPayload
// 固定字段共 32 字节（不含子消息头）。
// ---------------------------------------------------------------------------
void RtpsReader::SweepFragBufs(uint64_t now_ms) {
  for (auto it = frag_bufs_.begin(); it != frag_bufs_.end();) {
    if (now_ms - it->second.first_seen_ms > 30000) {   // 30 秒未收齐则丢弃
      frag_bufs_bytes_ -= it->second.data.size();
      it = frag_bufs_.erase(it);
    } else {
      ++it;
    }
  }
}

void RtpsReader::HandleDataFrag(const uint8_t* buf, size_t n, uint32_t body) {
  const size_t fixed = 32;
  if (body + fixed > n) return;

  const uint8_t* b = buf + body;
  uint32_t wid = 0, start = 0, sample_size = 0;
  uint64_t sn = 0;
  uint16_t in_sub = 0, fsz = 0;
  wid = EntityVal(b + 8);   // 与 DATA 路径一致（处理 EntityId 的字节序）
  {
    uint32_t hi = 0, lo = 0;
    std::memcpy(&hi, b + 12, 4);
    std::memcpy(&lo, b + 16, 4);
    sn = (static_cast<uint64_t>(hi) << 32) | lo;
  }
  std::memcpy(&start, b + 20, 4);
  std::memcpy(&in_sub, b + 24, 2);
  std::memcpy(&fsz, b + 26, 2);
  std::memcpy(&sample_size, b + 28, 4);

  if (fsz == 0 || sample_size == 0 || in_sub == 0) return;
  const size_t pl_off = body + fixed;
  if (pl_off > n) return;

  const uint64_t now_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
  SweepFragBufs(now_ms);

  // 只处理我们关心的用户数据 writer（内建 writer 不分片）
  if (IsBuiltinWriter(wid)) return;

  char key[64];
  std::snprintf(key, sizeof(key), "%.12s|%08x|%llu",
                reinterpret_cast<const char*>(buf + 8), wid,
                static_cast<unsigned long long>(sn));

  FragBuf& fb = frag_bufs_[key];
  if (fb.data.empty()) {
    fb.sample_size = sample_size;
    fb.frag_size = fsz;
    fb.data.assign(sample_size, 0);
    const uint32_t nf = (sample_size + fsz - 1) / fsz;
    fb.have.assign(nf, false);
    fb.got_frags = 0;
    fb.first_seen_ms = now_ms;
    fb.source_ts_ns = last_info_ts_ns_;
    frag_bufs_bytes_ += sample_size;
  }

  // 本次子消息可能含多个连续分片
  const size_t avail = n - pl_off;
  for (uint16_t k = 0; k < in_sub; ++k) {
    const uint32_t fnum = start + k;              // 从 1 起
    const size_t off = static_cast<size_t>(fnum - 1) * fsz;
    if (off >= sample_size) break;
    const size_t len = (off + fsz <= sample_size) ? fsz : (sample_size - off);
    if (k * fsz + len > avail) break;
    if (fnum - 1 < fb.have.size() && !fb.have[fnum - 1]) {
      std::memcpy(fb.data.data() + off, buf + pl_off + k * fsz, len);
      fb.have[fnum - 1] = true;
      ++fb.got_frags;
    }
  }

  if (fb.got_frags < fb.have.size()) return;   // 还没收齐

  // ---- 收齐：走与 DATA 相同的交付路径 ----
  UserData ud;
  ud.reception_timestamp_ns = static_cast<TimestampNs>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  ud.source_timestamp_ns = fb.source_ts_ns;
  std::memcpy(ud.writer_guid.guidPrefix.value, buf + 8, 12);
  ud.writer_guid.entityId = EntityId_t(wid);
  ud.sequence = static_cast<uint32_t>(sn & 0xFFFFFFFFu);
  ud.payload.assign(reinterpret_cast<const char*>(fb.data.data()), fb.data.size());

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : writers_) {
      const RemoteWriter& w = kv.second;
      if (std::memcmp(w.guid.guidPrefix.value,
                      ud.writer_guid.guidPrefix.value, 12) == 0 &&
          w.guid.entityId == ud.writer_guid.entityId) {
        ud.topic_name = w.topic_name;
        ud.type_name = w.type_name;
        break;
      }
    }
  }

  // 【去重】重传的分片会再次收齐，必须用 SN 去重，否则同一样本会重复交付
  // （实测：发 20 条 8000B 却收到 56 条）。这与 DATA 路径用的是同一个机制。
  const bool first_time =
      RecordReceivedSn(wid, static_cast<uint32_t>(sn & 0xFFFFFFFFu));

  frag_bufs_bytes_ -= fb.data.size();
  frag_bufs_.erase(key);

  if (!first_time) {
    return;   // 已交付过，静默丢弃重复
  }

  st_samples_.fetch_add(1, std::memory_order_relaxed);
  if (callback_) {
    callback_(ud);
  }
}

}  // namespace stratum

}  // namespace rtps
}  // namespace protocol
