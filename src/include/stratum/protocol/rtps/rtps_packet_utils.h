// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// RTPS 报文构造与解析的共享工具
//
// ===========================================================================
// 为什么独立成模块
// ===========================================================================
//
// RtpsReader（订阅端）与 RtpsWriter（发布端）是镜像关系，共用大量底层操作：
// ENTITY 判定、RTPS 头 + INFO_DST 组装、HEARTBEAT/ACKNACK 构造、
// SEDP 公告与 DATA 报文组装、地址转换等。
//
// **不把它们各自实现一遍** —— 本项目的实测教训（见 AI_COLLAB.md）：
// 把已验证的逻辑"重新实现"会引入难以察觉的偏差，
// 例如手工解析 HEARTBEAT 曾导致 ACKNACK 完全无效、对端不推送任何数据。
//
// 此外，报文构造是**纯函数**（给定参数产出字节），放在这里可以被
// 单元测试直接驱动 —— 不依赖真实网络，能稳定回归字节级正确性。

#ifndef STRATUM_PROTOCOL_RTPS_RTPS_PACKET_UTILS_H_
#define STRATUM_PROTOCOL_RTPS_RTPS_PACKET_UTILS_H_

#include <cstdint>
#include <string>
#include <vector>

#include <stratum/core/types.h>   // TimestampNs

#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/common/Guid.hpp>
#include <stratum/rtps/common/Locator.hpp>
#include <stratum/rtps/messages/RTPS_messages.hpp>

namespace stratum {
namespace protocol {
namespace rtps {
namespace packet {

// --- RTPS 内建端点（大端数值表示）---
constexpr uint32_t kEntitySpdpWriter = 0x000100c2u;
constexpr uint32_t kEntitySpdpReader = 0x000100c7u;
constexpr uint32_t kEntitySedpPubWriter = 0x000003c2u;
constexpr uint32_t kEntitySedpPubReader = 0x000003c7u;
constexpr uint32_t kEntitySedpSubWriter = 0x000004c2u;
constexpr uint32_t kEntitySedpSubReader = 0x000004c7u;
constexpr uint32_t kEntitySedpTopicsWriter = 0x000002c2u;

// user writer / reader 的 EntityId 数值
constexpr uint32_t kEntityUserWriter = 0x00000103u;
constexpr uint32_t kEntityUserReader = 0x00000104u;

// --- 子消息 id ---
constexpr uint8_t kSubPad = 0x01;
constexpr uint8_t kSubAcknack = 0x06;
constexpr uint8_t kSubHeartbeat = 0x07;
constexpr uint8_t kSubGap = 0x08;
constexpr uint8_t kSubInfoTs = 0x09;
constexpr uint8_t kSubInfoDst = 0x0e;
constexpr uint8_t kSubData = 0x15;

// --- QoS PID（ParameterTypes.hpp 中没有对应的 ParameterXxx_t 类，
//     故按线上格式直接写 PID + length + value）---
constexpr uint16_t kPidSentinel = 0x0001;
constexpr uint16_t kPidTopicName = 0x0005;
constexpr uint16_t kPidTypeName = 0x0007;
constexpr uint16_t kPidProtocolVersion = 0x0015;
constexpr uint16_t kPidVendorId = 0x0016;
constexpr uint16_t kPidReliability = 0x001a;
constexpr uint16_t kPidDurability = 0x001d;
constexpr uint16_t kPidUnicastLocator = 0x002f;
constexpr uint16_t kPidParticipantGuid = 0x0050;
constexpr uint16_t kPidTypeMaxSizeSerialized = 0x0060;
constexpr uint16_t kPidKeyHash = 0x0070;
constexpr uint16_t kPidEndpointGuid = 0x005a;
constexpr uint16_t kPidDataRepresentation = 0x0073;

// DDS QoS 取值
constexpr uint32_t kReliabilityBestEffort = 1;
constexpr uint32_t kReliabilityReliable = 2;
constexpr uint32_t kDurabilityVolatile = 0;
constexpr uint32_t kDurabilityTransientLocal = 1;

// EntityId 的 4 字节 → 大端数值
uint32_t EntityVal(const uint8_t* e);

// 是否为内建 writer（SPDP / SEDP 系列）
bool IsBuiltinWriter(uint32_t v);

// 是否为内建端点（writer 或 reader）
bool IsBuiltinEntity(uint32_t v);

// 组装 RTPS 头；dst_prefix 非空时追加 INFO_DST 子消息
//
// INFO_DST 很关键：缺少它时接收方会认为消息目标未定义而拒绝投递
// （实测 Fast DDS 报 "No Reader accepts this message (directed to: 0.0.0.1)"）。
void WriteHeaderAndDst(eprosima::fastdds::rtps::CDRMessage_t* msg,
                       const uint8_t my_prefix[12],
                       const uint8_t* dst_prefix /* 12 字节或 nullptr */);

// 组装 HEARTBEAT 报文（含 RTPS 头 + 可选 INFO_DST）
std::vector<uint8_t> BuildHeartbeat(const uint8_t my_prefix[12],
                                    const uint8_t* dst_prefix,
                                    uint32_t writer_id, uint32_t reader_id,
                                    int64_t first_sn, int64_t last_sn);

// 组装 ACKNACK 子消息并追加到 msg
bool AppendAcknack(eprosima::fastdds::rtps::CDRMessage_t* msg,
                   uint32_t reader_id, uint32_t writer_id, int64_t base_sn,
                   uint32_t sn_span, int32_t count);

// 回填子消息的 octetsToNextHeader
//
// **必须直接写 buffer**：CDRMessage::addUInt16 会执行 `length += 2`，
// 在"倒回 pos 回填"的场景下会把消息总长污染成多 2 字节，
// 接收方遍历子消息时报 "SubmessageHeader too short"。
void BackfillSubmessageLength(eprosima::fastdds::rtps::CDRMessage_t* msg,
                              uint32_t size_pos, uint32_t body_start);

// ---------------------------------------------------------------------------
// SEDP 公告构造（发布端 / 订阅端共用骨架）
// ---------------------------------------------------------------------------

struct EndpointAnnouncement {
  uint8_t prefix[12] = {0};        // 我方 GUID 前缀
  uint32_t writer_entity = 0;      // 承载该公告的 SEDP writer EntityId
  uint32_t endpoint_entity = 0;    // 被公告端点自身的 EntityId
  std::string topic_name;
  std::string type_name;
  uint16_t unicast_port = 0;       // 我方数据收发端口
  // 我方声明的单播地址（空 = 自动探测本机对外地址，见 DetectLocalAddress）。
  std::string unicast_address;

  // QoS：Publication 必须显式声明，否则对端可能判定不兼容而不接收数据
  // （实测：不声明时回放 110 条，Fast DDS subscriber 收到 0 条）。
  uint32_t reliability = kReliabilityReliable;
  uint32_t durability = kDurabilityVolatile;
  // 支持的数据表示集合（0=XCDR1, 1=XCDR2）。
  //
  // 【必须真正发出去】此前的字段从未被序列化成 PID_DATA_REPRESENTATION(0x0073)。
  // 对端对需要 XCDR2 的类型（例如 APPENDABLE 的 ShapeType）会判定
  // 数据表示不兼容 → **根本不匹配**（实测：keyed 示例 publisher 一直等匹配、
  // 零样本到达，而本端 sedp_pub=1、qos_rej=0，看起来"一切正常"）。
  // 空集合 = 不发送该 PID（对端按默认处理）。
  std::vector<uint32_t> data_representations;
  bool include_qos = true;

  uint64_t sn = 1;
};

// 构造 SEDP 公告报文（Publication 与 Subscription 共用此骨架）
std::vector<uint8_t> BuildSedpAnnouncement(const EndpointAnnouncement& a);

// 取本机**对外可达**的 IPv4 地址（探测失败返回 "127.0.0.1"）。
//
// 【为什么必须有】此前所有对外声明的 locator 都硬编码 127.0.0.1：
// 同机通信恰好能走通（对端把 127.0.0.1 当自己回环，而本机就是同一台），
// 一旦**跨主机**，对端会把数据发到它自己的回环 —— 完全不可用。
// 做法：UDP socket "连接"一个外部地址（不发任何包），内核按路由表选出出口地址。
std::string DetectLocalAddress();

// ---------------------------------------------------------------------------
// 用户数据 DATA 报文构造
// ---------------------------------------------------------------------------

struct DataPacketParams {
  uint8_t prefix[12] = {0};
  uint32_t writer_entity = kEntityUserWriter;
  uint64_t sn = 1;
  std::string payload;   // 已序列化的负载（含 encapsulation）

  // 样本的源时间戳（发布者写入时间）。
  // 【必须写入 INFO_TS】它是接收端计算端到端延迟的唯一依据，
  // 也是 docs/06-data-trust.md 要求的"两种时间戳都保留"中的 source。
  // 曾经写死 0（无效），导致延迟无法测量、录制下来的 source 全是无效值。
  TimestampNs source_ts_ns = 0;
};

std::vector<uint8_t> BuildDataPacket(const DataPacketParams& p);

}  // namespace packet
}  // namespace rtps
}  // namespace protocol
}  // namespace stratum

#endif  // STRATUM_PROTOCOL_RTPS_RTPS_PACKET_UTILS_H_
