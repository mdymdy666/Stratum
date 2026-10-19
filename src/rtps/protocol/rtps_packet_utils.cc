// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// RTPS 报文构造工具实现

#include <stratum/protocol/rtps/rtps_packet_utils.h>

#include <cstdio>
#include <arpa/inet.h>
#include <cstring>
#include <unistd.h>

#include <stratum/rtps/builtin/spdp_announcer.h>
#include <stratum/rtps/core/policy/ParameterSerializer.hpp>
#include <stratum/rtps/dds/core/policy/ParameterTypes.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>
#include <stratum/rtps/messages/RTPSMessageCreator.hpp>

namespace stratum {
namespace protocol {
namespace rtps {
namespace packet {

// CDRMessage 是**命名空间**而非类型，不能用 using-declaration，故整体引入
using namespace eprosima::fastdds::rtps;
namespace dds = eprosima::fastdds::dds;

uint32_t EntityVal(const uint8_t* e) {
  return (static_cast<uint32_t>(e[0]) << 24) |
         (static_cast<uint32_t>(e[1]) << 16) |
         (static_cast<uint32_t>(e[2]) << 8) | e[3];
}

bool IsBuiltinWriter(uint32_t v) {
  return v == kEntitySpdpWriter || v == kEntitySedpPubWriter ||
         v == kEntitySedpSubWriter || v == kEntitySedpTopicsWriter;
}

bool IsBuiltinEntity(uint32_t v) {
  return IsBuiltinWriter(v) || v == kEntitySpdpReader ||
         v == kEntitySedpPubReader || v == kEntitySedpSubReader ||
         v == 0x000002c7u;
}

void WriteHeaderAndDst(CDRMessage_t* msg, const uint8_t my_prefix[12],
                       const uint8_t* dst_prefix) {
  for (char c : {'R', 'T', 'P', 'S'}) CDRMessage::addOctet(msg, c);
  CDRMessage::addOctet(msg, 2);
  CDRMessage::addOctet(msg, 4);
  CDRMessage::addOctet(msg, 0x01);   // vendorId[0]（eProsima）
  CDRMessage::addOctet(msg, 0x0f);   // vendorId[1]
  for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
    CDRMessage::addOctet(msg, my_prefix[i]);
  }
  if (dst_prefix != nullptr) {
    CDRMessage::addOctet(msg, kSubInfoDst);
    CDRMessage::addOctet(msg, 0x01);   // little endian
    CDRMessage::addUInt16(msg, 12);
    for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
      CDRMessage::addOctet(msg, dst_prefix[i]);
    }
  }
}

std::vector<uint8_t> BuildHeartbeat(const uint8_t my_prefix[12],
                                    const uint8_t* dst_prefix,
                                    uint32_t writer_id, uint32_t reader_id,
                                    int64_t first_sn, int64_t last_sn) {
  CDRMessage_t hb(256);
  hb.msg_endian = LITTLEEND;
  WriteHeaderAndDst(&hb, my_prefix, dst_prefix);

  CDRMessage::addOctet(&hb, kSubHeartbeat);
  CDRMessage::addOctet(&hb, 0x01);   // E=1（小端）
  CDRMessage::addUInt16(&hb, 28);    // 体长度 = 4+4+8+8+4
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

bool AppendAcknack(CDRMessage_t* msg, uint32_t reader_id, uint32_t writer_id,
                   int64_t base_sn, uint32_t sn_span, int32_t count) {
  // SNSet 必须把缺失的 SN 位**全部**置 1：
  // 只设 base 时对端认为 reader 已拥有全部数据而不重传（实测收到数据为 0）。
  const uint32_t lo = static_cast<uint32_t>(base_sn & 0xFFFFFFFF);
  SequenceNumberSet_t sns(SequenceNumber_t(0, lo));
  uint32_t span = sn_span == 0 ? 1 : sn_span;
  if (span > 256) span = 256;   // BitmapRange 上限 256 位
  for (uint32_t k = 0; k < span; ++k) {
    sns.add(SequenceNumber_t(0, lo + k));
  }
  EntityId_t rid(reader_id);
  EntityId_t wid(writer_id);
  return RTPSMessageCreator::addSubmessageAcknack(msg, rid, wid, sns, count,
                                                  false);
}

void BackfillSubmessageLength(CDRMessage_t* msg, uint32_t size_pos,
                              uint32_t body_start) {
  const uint16_t body_len = static_cast<uint16_t>(msg->pos - body_start);
  const octet* o = reinterpret_cast<const octet*>(&body_len);
  if (msg->msg_endian == DEFAULT_ENDIAN) {
    msg->buffer[size_pos] = o[0];
    msg->buffer[size_pos + 1] = o[1];
  } else {
    msg->buffer[size_pos] = o[1];
    msg->buffer[size_pos + 1] = o[0];
  }
}

// ---------------------------------------------------------------------------
// SEDP 公告
// ---------------------------------------------------------------------------

namespace {

// 写一个 QoS 参数（PID + length + value），值长度须为 4 的倍数
void WriteU32Param(CDRMessage_t* msg, uint16_t pid, uint32_t value) {
  CDRMessage::addUInt16(msg, pid);
  CDRMessage::addUInt16(msg, 4);
  CDRMessage::addUInt32(msg, value);
}

}  // namespace

std::string DetectLocalAddress() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return "127.0.0.1";
  }
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(9);
  ::inet_pton(AF_INET, "1.1.1.1", &dst.sin_addr);
  std::string out = "127.0.0.1";
  // connect 一个 UDP socket 不会发包，只让内核按路由表选定出口地址。
  if (::connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
    sockaddr_in self{};
    socklen_t sl = sizeof(self);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&self), &sl) == 0) {
      char buf[INET_ADDRSTRLEN] = {0};
      if (::inet_ntop(AF_INET, &self.sin_addr, buf, sizeof(buf)) != nullptr) {
        out = buf;
      }
    }
  }
  ::close(fd);
  return out;
}

std::vector<uint8_t> BuildSedpAnnouncement(const EndpointAnnouncement& a) {
  if (a.topic_name.empty() || a.type_name.empty()) return {};

  // 同样按名称长度动态分配：真实话题名可达数百字节
  const uint32_t cap = static_cast<uint32_t>(a.topic_name.size() +
                                             a.type_name.size()) + 512;
  CDRMessage_t msg(cap);
  msg.msg_endian = LITTLEEND;
  WriteHeaderAndDst(&msg, a.prefix, nullptr);

  // INFO_TS
  CDRMessage::addOctet(&msg, kSubInfoTs);
  CDRMessage::addOctet(&msg, 0x01);
  CDRMessage::addUInt16(&msg, 8);
  CDRMessage::addInt32(&msg, 0);
  CDRMessage::addUInt32(&msg, 0);

  // DATA
  CDRMessage::addOctet(&msg, kSubData);
  CDRMessage::addOctet(&msg, 0x05);   // E=1（小端）+ D=1（含负载）
  const uint32_t size_pos = msg.pos;
  CDRMessage::addUInt16(&msg, 0);     // octetsToNextHeader 占位
  const uint32_t body_start = msg.pos;

  CDRMessage::addUInt16(&msg, 0);     // extraFlags（RTPS 2.3+）
  CDRMessage::addUInt16(&msg, 16);    // octetsToInlineQos（标准值）
  EntityId_t unknown(ENTITYID_UNKNOWN);
  CDRMessage::addEntityId(&msg, &unknown);          // readerId = unknown（广播）
  EntityId_t sedp_writer(a.writer_entity);
  CDRMessage::addEntityId(&msg, &sedp_writer);
  SequenceNumber_t seq(static_cast<int32_t>(a.sn >> 32),
                       static_cast<uint32_t>(a.sn & 0xFFFFFFFF));
  CDRMessage::addSequenceNumber(&msg, &seq);

  // encapsulation（小端 PL_CDR_LE）
  {
    bool ok = CDRMessage::addOctet(&msg, 0);
    ok &= CDRMessage::addOctet(&msg,
                               static_cast<octet>(PL_CDR_LE - msg.msg_endian));
    ok &= CDRMessage::addUInt16(&msg, 0);
    if (!ok) return {};
  }

  GUID_t endpoint_guid;
  std::memcpy(endpoint_guid.guidPrefix.value, a.prefix, 12);
  endpoint_guid.entityId = EntityId_t(a.endpoint_entity);
  GUID_t participant_guid;
  std::memcpy(participant_guid.guidPrefix.value, a.prefix, 12);
  participant_guid.entityId = EntityId_t(ENTITYID_RTPSParticipant);

  {
    dds::ParameterGuid_t p(dds::PID_ENDPOINT_GUID, 16, endpoint_guid);
    if (!dds::ParameterSerializer<dds::ParameterGuid_t>::add_to_cdr_message(p, &msg)) return {};
  }
  {
    dds::ParameterGuid_t p(dds::PID_PARTICIPANT_GUID, 16, participant_guid);
    if (!dds::ParameterSerializer<dds::ParameterGuid_t>::add_to_cdr_message(p, &msg)) return {};
  }
  {
    dds::ParameterString_t p(dds::PID_TOPIC_NAME, 0, a.topic_name);
    if (!dds::ParameterSerializer<dds::ParameterString_t>::add_to_cdr_message(p, &msg)) return {};
  }
  {
    dds::ParameterString_t p(dds::PID_TYPE_NAME, 0, a.type_name);
    if (!dds::ParameterSerializer<dds::ParameterString_t>::add_to_cdr_message(p, &msg)) return {};
  }

  // --- PID_KEY_HASH (0x0070) ---
  // 对 **unkeyed** 话题，Fast DDS 约定此处填**端点自身的 GUID**：
  //   WriterProxyData.cpp: "WriterProxyData always contains the writer's GUID
  //                         as a key hash"，并写 `4 + 16` 字节。
  // 对端据此推导 topic_kind（GUID ⇒ NO_KEY）。缺失该 PID 时对端拿不到
  // 实例标识，`valid_matching` 会因 topic_kind 判定失败而**拒绝匹配**
  // —— 这正是 Stratum→Fast DDS 方向收不到数据的原因。
  {
    CDRMessage::addUInt16(&msg, kPidKeyHash);
    CDRMessage::addUInt16(&msg, 16);
    CDRMessage::addData(&msg, endpoint_guid.guidPrefix.value,
                        GuidPrefix_t::size);
    CDRMessage::addEntityId(&msg, &endpoint_guid.entityId);
  }

  // --- PID_TYPE_MAX_SIZE_SERIALIZED (0x0060) ---
  // 对端用它做样本尺寸预分配；0 表示未知。
  {
    CDRMessage::addUInt16(&msg, kPidTypeMaxSizeSerialized);
    CDRMessage::addUInt16(&msg, 4);
    CDRMessage::addUInt32(&msg, 0);
  }
  {
    dds::ParameterLocator_t p(dds::PID_UNICAST_LOCATOR, 24);
    // 声明**对外可达**的地址（空则自动探测）：跨主机通信的前提，
    // 见 DetectLocalAddress 的说明。
    const std::string addr =
        a.unicast_address.empty() ? DetectLocalAddress() : a.unicast_address;
    p.locator = ::stratum::rtps::MakeDefaultUdpv4Locator(addr.c_str(),
                                                         a.unicast_port);
    if (!dds::ParameterSerializer<dds::ParameterLocator_t>::add_to_cdr_message(p, &msg)) return {};
  }

  // --- QoS ---
  // 必须显式声明 RELIABILITY：Fast DDS 的 DataReader 默认请求 RELIABLE，
  // 若 Writer 侧被推断为 BEST_EFFORT，QoS 判定为不兼容，**对端不会接收数据**。
  // （实测：不声明时回放 110 条，Fast DDS subscriber 收到 0 条。）
  if (a.include_qos) {
    CDRMessage::addUInt16(&msg, kPidReliability);
    CDRMessage::addUInt16(&msg, 12);          // kind(4) + max_blocking_time(8)
    CDRMessage::addUInt32(&msg, a.reliability);
    CDRMessage::addInt32(&msg, 0);            // max_blocking_time.seconds
    CDRMessage::addUInt32(&msg, 100000000);   // 0.1 秒（fraction）

    WriteU32Param(&msg, kPidDurability, a.durability);

    // --- PID_DATA_REPRESENTATION (0x0073) ---
    // 值 = sequence<DataRepresentationId>：uint32 元素个数 + N × int32。
    // 不发这个 PID 会让需要 XCDR2 的对端判定不兼容（见结构体注释）。
    if (!a.data_representations.empty()) {
      // 【元素宽度是 2 字节】DataRepresentationId 在 Fast DDS 里定义为
      // `enum DataRepresentationId : int16_t`（QosPolicies.hpp:2071）。
      // 按 4 字节写会让对端解析失败 —— 表现为"这个 PID 像没发一样"，
      // 行为与修复前完全一致（实测统计数字逐项相同，很容易误判为"没生效"）。
      CDRMessage::addUInt16(&msg, kPidDataRepresentation);
      CDRMessage::addUInt16(
          &msg, static_cast<uint16_t>(4 + 2 * a.data_representations.size()));
      CDRMessage::addUInt32(&msg,
                            static_cast<uint32_t>(a.data_representations.size()));
      for (uint32_t v : a.data_representations) {
        CDRMessage::addUInt16(&msg, static_cast<uint16_t>(v));
      }
    }
    // DATA_REPRESENTATION 声明为 {XCDR1, XCDR2} 两者。
    //
    // 【为什么】该策略是**序列**而非单值：Reader 与 Writer 需就数据的
    // 序列化表示达成一致。此前的实现只声明 XCDR1(0)，而真实 Fast DDS
    // 节点（ROS 2 等）通常请求 XCDR2(1) —— 表示集合无交集时
    // Fast DDS 判定为不兼容，**Reader 不会接收任何数据**。
    // 实测：只声明 XCDR1 时，回放 32 条数据，Fast DDS subscriber 收到 0 条。
    // 声明两者后可与任意对端协商。
    CDRMessage::addUInt16(&msg, kPidDataRepresentation);
    CDRMessage::addUInt16(&msg, 8);          // 两个 uint32
    CDRMessage::addUInt32(&msg, 0);          // XCDR_DATA_REPRESENTATION
    CDRMessage::addUInt32(&msg, 1);          // XCDR2_DATA_REPRESENTATION
  }

  {
    // 长度 4：实测真实 Fast DDS 取值（规范字面是 2）。
    // 写成 2 会导致 Fast DDS 静默跳过整个参数表。
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

  BackfillSubmessageLength(&msg, size_pos, body_start);
  return std::vector<uint8_t>(msg.buffer, msg.buffer + msg.length);
}

// ---------------------------------------------------------------------------
// 用户数据 DATA
// ---------------------------------------------------------------------------

std::vector<uint8_t> BuildDataPacket(const DataPacketParams& p) {
  // 【必须按负载大小分配】早期实现固定 2048 字节，
  // 超过该尺寸的样本会被截断或构造失败 —— 单元测试
  // RtpsPacket.DegenerateInputs 用 8 KiB 负载立即暴露了这一点。
  // 回放真实话题时单条样本超过 2 KiB 很常见。
  // 【>64 KiB 必须显式失败，不得发坏包】
  //
  // RTPS 的 SubmessageHeader.octetsToNextHeader 是 16 位（上限 65535），
  // 而 DATA 子消息体 = extraFlags(2)+octetsToInlineQos(2)+readerId(4)
  // +writerId(4)+SN(8)+payload = 20 + payload。
  // 超过上限时长度字段会溢出，构造出畸形报文，对端静默丢弃 ——
  // 表现为"回放端显示已发布、订阅端一条都收不到"（本项目已踩过一次同类坑：
  // 固定 2048 缓冲导致 8 KiB 负载发出 56 字节空报文）。
  // 真正的支持需要 DATA_FRAG 分片（未实现），因此这里**明确报错**。
  constexpr size_t kMaxDataSubmessageBody = 65535;
  constexpr size_t kDataSubmessageOverhead = 20;
  if (p.payload.size() + kDataSubmessageOverhead > kMaxDataSubmessageBody) {
    std::fprintf(stderr,
                 "[packet] 样本过大：payload=%zu 字节，DATA 子消息体将达 %zu "
                 "字节，超过 16 位长度上限 %zu —— 需要 DATA_FRAG 分片（未实现）。"
                 "已拒绝构造，不会发出畸形报文。\n",
                 p.payload.size(), p.payload.size() + kDataSubmessageOverhead,
                 kMaxDataSubmessageBody);
    std::fflush(stderr);
    return {};
  }

  const uint32_t cap = static_cast<uint32_t>(p.payload.size()) + 512;
  CDRMessage_t msg(cap);
  msg.msg_endian = LITTLEEND;
  WriteHeaderAndDst(&msg, p.prefix, nullptr);

  CDRMessage::addOctet(&msg, kSubInfoTs);
  CDRMessage::addOctet(&msg, 0x01);
  CDRMessage::addUInt16(&msg, 8);
  {
    // RTPS 9.4.5.4：Time_t = seconds(int32) + fraction(uint32, 单位 1/2^32 秒)。
    // 源时间戳无效时写 0（对端按"无时间戳"处理）。
    int32_t ts_sec = 0;
    uint32_t ts_frac = 0;
    if (p.source_ts_ns > 0) {
      ts_sec = static_cast<int32_t>(p.source_ts_ns / 1000000000LL);
      const uint64_t rem = static_cast<uint64_t>(p.source_ts_ns % 1000000000LL);
      ts_frac = static_cast<uint32_t>((rem << 32) / 1000000000ULL);
    }
    CDRMessage::addInt32(&msg, ts_sec);
    CDRMessage::addUInt32(&msg, ts_frac);
  }

  CDRMessage::addOctet(&msg, kSubData);
  CDRMessage::addOctet(&msg, 0x05);
  const uint32_t size_pos = msg.pos;
  CDRMessage::addUInt16(&msg, 0);
  const uint32_t body_start = msg.pos;
  CDRMessage::addUInt16(&msg, 0);    // extraFlags
  CDRMessage::addUInt16(&msg, 16);   // octetsToInlineQos
  EntityId_t unknown(ENTITYID_UNKNOWN);
  CDRMessage::addEntityId(&msg, &unknown);
  EntityId_t w(p.writer_entity);
  CDRMessage::addEntityId(&msg, &w);
  SequenceNumber_t seq(static_cast<int32_t>(p.sn >> 32),
                       static_cast<uint32_t>(p.sn & 0xFFFFFFFF));
  CDRMessage::addSequenceNumber(&msg, &seq);

  if (!p.payload.empty()) {
    // 【不能忽略返回值】容量不足时 addData 返回 false 且不写入、不推进 length，
    // 于是我们会发出一份"只有头部、没有负载"的 DATA：
    //   sendto 成功、回放端显示"已发布"、订阅端一条都收不到。
    // 实测 8 KiB 负载正是如此（报文仅 56 字节）。
    if (!CDRMessage::addData(&msg,
                             reinterpret_cast<const octet*>(p.payload.data()),
                             static_cast<uint32_t>(p.payload.size()))) {
      std::fprintf(stderr,
                   "[packet] addData 失败: payload=%zu cap=%u max_size=%u pos=%u length=%u\n",
                   p.payload.size(), cap, msg.max_size, msg.pos, msg.length);
      std::fflush(stderr);
      return {};
    }
  }
  BackfillSubmessageLength(&msg, size_pos, body_start);
  return std::vector<uint8_t>(msg.buffer, msg.buffer + msg.length);
}

// ---------------------------------------------------------------------------
// 批量 DATA（见头文件里的收益测算与约束说明）
//
// 与 BuildDataPacket 的**唯一**结构差异：
//   Header + INFO_DST 只写一次，然后循环追加 N 个 (INFO_TS + DATA)。
// 其余子消息布局逐字节相同 —— 对端是按子消息解析的，不关心一个报文里有几条。
// ---------------------------------------------------------------------------
std::vector<uint8_t> BuildDataPacketBatch(
    const std::vector<DataPacketParams>& samples) {
  if (samples.empty()) return {};

  // --- 先算总容量：Header 余量 + 每条 (INFO_TS 12 + DATA 24 + payload) ---
  size_t need = 512;
  for (const auto& s : samples) need += s.payload.size() + 36;
  if (need > 60000) {
    // UDP 单包实际可用 ~64KiB，留余量。超了让调用方拆批 ——
    // 不在这里截断（截断会发出"少了几条"的报文，而调用方以为全发了）。
    std::fprintf(stderr,
                 "[packet] 批量过大：%zu 条需要 %zu 字节（上限 60000）。"
                 "调用方应拆成更小的批。\n",
                 samples.size(), need);
    std::fflush(stderr);
    return {};
  }

  const uint32_t cap = static_cast<uint32_t>(need);
  CDRMessage_t msg(cap);
  msg.msg_endian = LITTLEEND;
  // ★ 收益来源：这一段只写一次，而单发是每条一次。
  WriteHeaderAndDst(&msg, samples[0].prefix, nullptr);

  for (const auto& p : samples) {
    // 单条子消息的 octetsToNextHeader 是 16 位 —— 逐条检查。
    //
    // 【当前是防御性代码】总长上限 60000 < 单条上限 65535，所以任何单条超界的
    // 批次在上面的总长检查就被拒了，走不到这里。保留它的理由：若将来把总长
    // 提到 64KiB+（例如走 TCP/共享内存传输），这条会成为唯一防线。
    // 测试里已诚实标注这一点（见 test_rtps_packet_utils.cc 的 OversizedSingleSample...）。
    constexpr size_t kMaxDataSubmessageBody = 65535;
    if (p.payload.size() + 20 > kMaxDataSubmessageBody) {
      std::fprintf(stderr,
                   "[packet] 批量内单条过大：payload=%zu 字节，需要 DATA_FRAG"
                   "（批量接口不支持分片）。整批放弃，不发畸形报文。\n",
                   p.payload.size());
      std::fflush(stderr);
      return {};
    }

    // --- INFO_TS（每条自己的源时间戳，不能共用）---
    CDRMessage::addOctet(&msg, kSubInfoTs);
    CDRMessage::addOctet(&msg, 0x01);
    CDRMessage::addUInt16(&msg, 8);
    {
      int32_t ts_sec = 0;
      uint32_t ts_frac = 0;
      if (p.source_ts_ns > 0) {
        ts_sec = static_cast<int32_t>(p.source_ts_ns / 1000000000LL);
        const uint64_t rem = static_cast<uint64_t>(p.source_ts_ns % 1000000000LL);
        ts_frac = static_cast<uint32_t>((rem << 32) / 1000000000ULL);
      }
      CDRMessage::addInt32(&msg, ts_sec);
      CDRMessage::addUInt32(&msg, ts_frac);
    }

    // --- DATA（每条自己的 writerId 与 SN）---
    CDRMessage::addOctet(&msg, kSubData);
    CDRMessage::addOctet(&msg, 0x05);
    const uint32_t size_pos = msg.pos;
    CDRMessage::addUInt16(&msg, 0);
    const uint32_t body_start = msg.pos;
    CDRMessage::addUInt16(&msg, 0);    // extraFlags
    CDRMessage::addUInt16(&msg, 16);   // octetsToInlineQos（DATA=16）
    EntityId_t unknown(ENTITYID_UNKNOWN);
    CDRMessage::addEntityId(&msg, &unknown);
    EntityId_t w(p.writer_entity);
    CDRMessage::addEntityId(&msg, &w);
    SequenceNumber_t seq(static_cast<int32_t>(p.sn >> 32),
                         static_cast<uint32_t>(p.sn & 0xFFFFFFFF));
    CDRMessage::addSequenceNumber(&msg, &seq);

    if (!p.payload.empty()) {
      // 【不能忽略返回值】容量不足时 addData 返回 false 且不写入，
      // 会发出"只有头没有负载"的 DATA（本项目在 8 KiB 负载上踩过）。
      if (!CDRMessage::addData(&msg,
                               reinterpret_cast<const octet*>(p.payload.data()),
                               static_cast<uint32_t>(p.payload.size()))) {
        std::fprintf(stderr,
                     "[packet] 批量 addData 失败: payload=%zu cap=%u pos=%u\n",
                     p.payload.size(), cap, msg.pos);
        std::fflush(stderr);
        return {};
      }
    }
    BackfillSubmessageLength(&msg, size_pos, body_start);
  }
  return std::vector<uint8_t>(msg.buffer, msg.buffer + msg.length);
}

// ---------------------------------------------------------------------------
// DATA_FRAG（分片发送）
//
// 子消息布局（RTPS 2.3 §9.6.3.6）：
//   SubmessageHeader(4: id=0x16, flags, octetsToNextHeader)
//   extraFlags(2) octetsToInlineQos(2) readerId(4) writerId(4) writerSN(8)
//   fragmentStartingNum(4) fragmentsInSubmessage(2) fragmentSize(2) sampleSize(4)
//   serializedPayload
//
// octetsToInlineQos 自 extraFlags 之后算到 inlineQos：DATA 是 16，
// DATA_FRAG 多出 4+2+2+4 = 12 个分片字段，所以是 28。
//
// 为什么需要：DATA 子消息除了 64KiB 的长度上限，在 MTU 1500 的链路上还有
// 1472 字节的不分片上限，超出就依赖 IP 分片，而 IP 分片在 NAT/中继路径上
// 会被丢弃（本项目跨主机实测）。DATA_FRAG 把切分放在 RTPS 层，
// 每个报文都不超过 MTU，才能穿过这类路径。
// ---------------------------------------------------------------------------
std::vector<std::vector<uint8_t>> BuildDataFragPackets(
    const DataPacketParams& p, uint16_t fragment_size) {
  std::vector<std::vector<uint8_t>> out;
  const size_t total = p.payload.size();
  if (fragment_size == 0 || total == 0) {
    return out;
  }

  size_t per = fragment_size;
  if (per > total) {
    per = total;
  }
  const uint32_t nfrags = static_cast<uint32_t>((total + per - 1) / per);
  if (nfrags == 0) {
    return out;
  }

  for (uint32_t i = 0; i < nfrags; ++i) {
    const size_t off = static_cast<size_t>(i) * per;
    const size_t len = (off + per <= total) ? per : (total - off);

    const uint32_t cap = static_cast<uint32_t>(len) + 512;
    CDRMessage_t msg(cap);
    msg.msg_endian = LITTLEEND;
    WriteHeaderAndDst(&msg, p.prefix, nullptr);

    CDRMessage::addOctet(&msg, kSubDataFrag);
    CDRMessage::addOctet(&msg, 0x05);          // flags：与 DATA 保持一致
    const uint32_t size_pos = msg.pos;
    CDRMessage::addUInt16(&msg, 0);            // octetsToNextHeader 占位
    const uint32_t body_start = msg.pos;

    CDRMessage::addUInt16(&msg, 0);            // extraFlags
    CDRMessage::addUInt16(&msg, 28);           // octetsToInlineQos（DATA_FRAG=28）
    EntityId_t unknown(ENTITYID_UNKNOWN);
    CDRMessage::addEntityId(&msg, &unknown);   // readerId
    EntityId_t w(p.writer_entity);
    CDRMessage::addEntityId(&msg, &w);         // writerId
    SequenceNumber_t seq(static_cast<int32_t>(p.sn >> 32),
                         static_cast<uint32_t>(p.sn & 0xFFFFFFFF));
    CDRMessage::addSequenceNumber(&msg, &seq);
    CDRMessage::addUInt32(&msg, i + 1);        // fragmentStartingNum（从 1 起）
    CDRMessage::addUInt16(&msg, 1);            // fragmentsInSubmessage
    CDRMessage::addUInt16(&msg, static_cast<uint16_t>(per));   // fragmentSize
    CDRMessage::addUInt32(&msg, static_cast<uint32_t>(total)); // sampleSize

    if (!CDRMessage::addData(&msg,
                             reinterpret_cast<const octet*>(p.payload.data() + off),
                             static_cast<uint32_t>(len))) {
      std::fprintf(stderr, "[packet] DATA_FRAG addData 失败: off=%zu len=%zu cap=%u\n",
                   off, len, cap);
      std::fflush(stderr);
      return {};
    }
    BackfillSubmessageLength(&msg, size_pos, body_start);
    out.push_back(std::vector<uint8_t>(msg.buffer, msg.buffer + msg.length));
  }
  return out;
}

}  // namespace packet
}  // namespace rtps
}  // namespace protocol
}  // namespace stratum
