// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// RtpsReader —— Stratum 自研的 RTPS 订阅端协议引擎
//
// ===========================================================================
// 职责
// ===========================================================================
//
// 完成"作为一个 RTPS Reader 参与 DDS 网络并接收用户数据"的全部协议动作：
//
//   1. 周期发送 SPDP 公告（我方参与者的自我介绍）
//   2. 从对端 SPDP 学习其 metatraffic locator
//   3. 接收并解析 SEDP Publication 公告 → 得知远端有哪些 Writer
//   4. 用 QoS 兼容判定（QosMatcher）筛选可订阅的 Writer
//   5. **为可订阅的 Writer 发布我方 SEDP Subscription 公告**
//   6. 处理 HEARTBEAT → 回 ACKNACK（可靠传输）
//   7. 接收用户数据 DATA → 交给上层回调
//
// ===========================================================================
// 移植/实测基础
// ===========================================================================
//
// 本文件的每条协议规则都来自实测（见 AI_COLLAB.md 记录的 6 轮协议调试）：
//   - DATA 子消息前缀用 DataTypePrefixSize 数据驱动判定（版本号不可靠）
//   - 参数 length 需与真实 Fast DDS 一致（PROTOCOL_VERSION / VENDORID = 4）
//   - ACKNACK 的 SNSet 必须置位缺失位图（否则对端不重传）
//   - ACKNACK 必须发到**学到的对端 metatraffic locator**（源地址可能是组播）
//   - 构造子消息时 octetsToNextHeader 直接写 buffer（addUInt16 会污染 length）
//
// 上述每一点都对应一次真实的失败与定位，注释中标注了原因，请勿"简化"。

#ifndef STRATUM_PROTOCOL_RTPS_RTPS_READER_H_
#define STRATUM_PROTOCOL_RTPS_RTPS_READER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <netinet/in.h>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <stratum/core/status.h>
#include <stratum/core/types.h>
#include <stratum/rtps/builtin/qos_matcher.h>
#include <stratum/rtps/builtin/sedp_parser.h>
#include <stratum/rtps/builtin/spdp_announcer.h>
#include <stratum/rtps/messages/RTPS_messages.hpp>

namespace stratum {
namespace protocol {
namespace rtps {

// 平台无关的 IPv4 端点地址（不暴露平台 socket 类型到头文件）
struct PeerAddress {
  uint32_t ip = 0;      // IPv4，网络字节序
  uint16_t port = 0;    // 端口，网络字节序

  bool valid() const { return port != 0; }
};

// 一个已发现的远端 Writer
struct RemoteWriter {
  std::string topic_name;
  std::string type_name;
  eprosima::fastdds::rtps::GUID_t guid;
  bool subscribed = false;
  ::stratum::rtps::SedpEndpointData endpoint;
};

// 收到一条用户数据时交给上层
struct UserData {
  std::string topic_name;
  std::string type_name;

  // 两种时间戳都保留（见 docs/06-data-trust.md）：
  //   source   —— 发布者写入的时间（来自 INFO_TS，可能不可信）
  //   reception —— 本地收到的时间（可信，用于判断"我们何时看到的"）
  TimestampNs source_timestamp_ns = kInvalidTimestamp;
  TimestampNs reception_timestamp_ns = kInvalidTimestamp;

  uint32_t sequence = 0;
  std::string payload;                // 原始负载（含 encapsulation）

  // keyed 话题的实例标识：来自 inlineQos 的 PID_KEY_HASH（16 字节二进制）。
  // 存储层与 Recorder/Player 早已贯通该字段，这里补齐 RTPS 层的提取。
  std::string instance_key;
  eprosima::fastdds::rtps::GUID_t writer_guid;
};

// 用户数据回调。返回 false 表示要求停止接收。
using UserDataCallback = std::function<bool(const UserData&)>;

struct RtpsReaderConfig {
  uint32_t domain_id = 0;
  std::string participant_name = "stratum_recorder";

  // 我方 GUID 前缀（12 字节）。前 2 字节必须是 vendorId（RTPS 规范 9.3.1.1），
  // 否则对端会当作未知厂商实现而拒绝（实测教训）。
  // 留空则自动生成 01 0f + "STRATUM" + 序号。
  std::vector<uint8_t> guid_prefix;

  // 我方监听端口。metatraffic 用于发现与可靠传输控制，user 用于用户数据。
  // 0 表示按 domain 自动选取（9050/9051 + domain*10）。
  uint16_t metatraffic_port = 0;
  uint16_t user_port = 0;

  // 对外声明的单播地址（空 = 自动探测本机对外地址）。
  std::string advertised_address;

  // 【跨主机发现】单播 SPDP 对端列表（等价于 Fast DDS 的 initialPeersList）。
  //
  // 为什么必须有：SPDP 默认只发组播 239.255.0.1，而**组播跨不过 NAT 边界**
  // （实测：本机 WSL 为 nat 模式，与局域网内虚拟机的组播不通）。
  // 跨主机时必须显式告诉对方"往这个地址发 SPDP"，否则双方永远发现不了彼此。
  // 可用环境变量 STRATUM_INITIAL_PEERS 传入（逗号分隔多个地址）。
  std::vector<std::string> initial_peers;

  // 我方作为 Reader 请求的 QoS（默认最宽松，见 QosMatcher::DefaultRecorderQos）
  ::stratum::rtps::QosRequest requested_qos;

  // 只订阅这些话题（空 = 订阅全部发现的 Writer）
  std::set<std::string> topic_filter;

  // SPDP 公告周期（毫秒）
  uint32_t announce_interval_ms = 200;
};

// RTPS 订阅端协议引擎
//
// 线程模型：Start() 启动一个接收线程，Stop() 停止并 join。
// 用户回调在该接收线程上被调用 —— **回调必须快速返回**，
// 否则会拖慢接收（Recorder 用队列解耦，见 recorder.h 的说明）。
class RtpsReader {
 public:
  RtpsReader();
  ~RtpsReader();

  RtpsReader(const RtpsReader&) = delete;
  RtpsReader& operator=(const RtpsReader&) = delete;

  // 打开套接字并组装 SPDP 公告（不启动线程）
  Status Open(const RtpsReaderConfig& config);

  // 启动接收线程
  Status Start(UserDataCallback callback);

  // 停止接收线程并关闭套接字
  Status Stop();

  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  // 已发现且 QoS 兼容的话题名（用于自检与质量报告）
  std::vector<std::string> DiscoveredTopics() const;

  // 已订阅的 Writer 数量
  size_t SubscribedWriterCount() const;

  struct Stats {
    uint64_t packets_total = 0;
    uint64_t spdp_received = 0;
    uint64_t sedp_publications = 0;
    uint64_t heartbeats = 0;
    uint64_t acknacks_sent = 0;
    uint64_t subscriptions_sent = 0;
    uint64_t user_samples = 0;
  uint64_t duplicates = 0;   // 因对端重传而丢弃的重复样本
    uint64_t peers_learned = 0;
    uint64_t qos_rejected = 0;
  };
  Stats GetStats() const;

  const std::string& last_error() const { return last_error_; }

 private:
  struct Peer {
    PeerAddress addr;
    uint8_t guid_prefix[12] = {0};
  };

  void ReceiveLoop();

  // 子消息处理（packet 为整包，body 为子消息体起始偏移）
  // 【DATA_FRAG 重组】按 (writer GUID, SN) 缓存分片。
  //
  // 为什么必须有：MTU 1500 的路径上不分片 UDP 上限是 1472 字节，超出需 IP 分片，
  // 而 IP 分片在 NAT/中继路径上会被丢弃。对端（若支持 DATA_FRAG）会把大样本切片发来，
  // 接收侧必须缓存并重组，否则大 payload 一条都收不到（本项目跨主机实测）。
  struct FragBuf {
    uint32_t sample_size = 0;      // 完整样本长度
    uint16_t frag_size = 0;        // 每片字节数（最后一片可能更短）
    uint32_t got_frags = 0;        // 已收到的片数
    std::vector<uint8_t> data;     // 重组缓冲（sample_size 大小）
    std::vector<bool> have;        // 各片是否已收到
    uint64_t first_seen_ms = 0;    // 用于超时淘汰
    TimestampNs source_ts_ns = kInvalidTimestamp;
  };
  std::map<std::string, FragBuf> frag_bufs_;   // key: guidPrefix+entityId+SN
  size_t frag_bufs_bytes_ = 0;                 // 缓存占用（用于上限控制）

  // 回收超时/超量的分片缓存（避免内存无界增长）
  void SweepFragBufs(uint64_t now_ms);

  // 处理 DATA_FRAG 子消息；收齐后走与 DATA 相同的交付路径
  void HandleDataFrag(const uint8_t* buf, size_t n, uint32_t body);

  // 【接收侧丢包注入，仅测试用】由 STRATUM_DEBUG_DROP_RECV_PERCENT 控制。
  //
  // 为什么需要它：发送侧的 STRATUM_DEBUG_DROP_SEND_PERCENT 是在 Publish 入口
  // 丢弃，样本**从未上过网络**，对端根本不知道存在那个 SN，ACKNACK 自然不会
  // 报告缺失，重传路径永远不触发（实测 [resend] 从未打印）。
  // 在**接收侧**注入则相反：包已到达但被丢弃、且**不记录 SN**，
  // 于是 ACKNACK 会如实报缺口，发送端的退避重传才有事可做。
  static bool ShouldDropRecvForTest();

  void HandleHeartbeat(const uint8_t* packet, size_t len, uint32_t body,
                       const eprosima::fastdds::rtps::SubmessageHeader_t& smh,
                       const PeerAddress& src);
  void HandleSpdp(const uint8_t* packet, size_t len);
  void HandleSedpPublication(const uint8_t* packet, size_t len);
  void HandleInfoTimestamp(const uint8_t* packet, size_t len, uint32_t body);

  void PublishSubscription(const RemoteWriter& writer);
  void ReannounceSubscriptions();
  void SendAcknack(const Peer& peer, uint32_t writer_id, uint32_t reader_id,
                   int64_t first_sn, int64_t last_sn);
  void SendHeartbeat(const Peer& peer, uint32_t writer_id, int64_t first_sn,
                     int64_t last_sn);
  void AnnounceOnce();

  Status OpenSockets();
  void CloseSockets();

  RtpsReaderConfig config_;
  std::string last_error_;

  int fd_spdp_ = -1;   // 监听 SPDP 组播端口（见 OpenSockets 说明）
  int fd_meta_ = -1;
  int fd_user_ = -1;
  int fd_tx_ = -1;

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread thread_;
  UserDataCallback callback_;

  mutable std::mutex mutex_;
  std::vector<Peer> peers_;
  std::map<std::string, RemoteWriter> writers_;   // key: topic|type
  uint64_t subscription_sn_ = 0;

  // 已收到的 DATA 序列号，按 writer entityId 分组。
  //
  // 【为什么必须有】ACKNACK 的语义是"我缺哪些"：
  //   base_sn 表示"小于它的 SN 都已收到"，位图标记 >= base_sn 的缺失 SN。
  // 旧实现把 HEARTBEAT 宣告的 [firstSN, lastSN] **整段标记为缺失**，
  // 于是对端认为本 reader 一条都没收到：反复重传、持续打印
  // `Inconsistent acknack received`（请求区间里已确认的 SN）。
  // 对真实 publisher（Fast DDS hello_world）更致命 —— 它在这种 ACKNACK 下
  // **只发出第一条就停止**，录制方向因此完全不可用（实测 samples 停在 1）。
  std::map<uint32_t, std::set<uint64_t>> received_sn_;

  // 记录一个已收到的 DATA 序列号（内部会裁剪过老的条目，限制内存）。
  // 返回 true 表示该 SN 是**首次**收到（false = 对端重传的重复数据）。
  bool RecordReceivedSn(uint32_t writer_entity, uint64_t sn);

  // 收到 DATA 后主动回一次 ACKNACK（"eager ack"）。
  //
  // 【为什么必须主动确认】对端的可靠 writer 默认 history depth = 1、
  // max_blocking_time 很短；若只在收到 HEARTBEAT（Fast DDS 默认 3 秒一次）
  // 时才回 ACKNACK，第 2 条数据就会因 history 未释放而写失败 ——
  // 实测上游 hello_world publisher 在这种情况**只发出第一条就停止**。
  // 真实 DDS 实现同样会在收到数据后立即确认。
  void SendAcknackForReceived(uint32_t writer_entity, uint64_t sn);

  // 按 RTPS 配对规则由 writer entityId 推出对应的 reader entityId
  // （内建 writer 用内建 reader；用户 writer 末字节 0x03 ↔ reader 0x04）。
  static uint32_t PairReaderEntity(uint32_t writer_entity);

  uint8_t my_prefix_[12] = {0};
  eprosima::fastdds::rtps::GUID_t my_reader_guid_;
  eprosima::fastdds::rtps::GUID_t my_participant_guid_;

  std::vector<uint8_t> spdp_payload_;
  // 对外声明的地址（SPDP 与 SEDP 必须用同一个，见 rtps_reader.cc 注释）
  std::string advertised_address_;

  std::atomic<uint64_t> st_packets_{0};
  std::atomic<uint64_t> st_spdp_{0};
  std::atomic<uint64_t> st_sedp_pub_{0};
  std::atomic<uint64_t> st_heartbeats_{0};
  std::atomic<uint64_t> st_acks_{0};
  std::atomic<uint64_t> st_subs_{0};
  std::atomic<uint64_t> st_samples_{0};
  std::atomic<uint64_t> st_duplicates_{0};   // 对端重传导致的重复样本（已丢弃）
  std::atomic<uint64_t> st_qos_rejected_{0};

  // INFO_TS 对随后的 DATA 生效，故需记住最近一次
  TimestampNs last_info_ts_ns_ = kInvalidTimestamp;
};

}  // namespace rtps
}  // namespace protocol
}  // namespace stratum

#endif  // STRATUM_PROTOCOL_RTPS_RTPS_READER_H_
