// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// RtpsWriter —— Stratum 自研的 RTPS 发布端协议引擎（回放用）
//
// ===========================================================================
// 与 RtpsReader 的关系
// ===========================================================================
//
// 两者是镜像：
//
//   RtpsReader（录制）            RtpsWriter（回放）
//   ─────────────────────         ─────────────────────
//   发 SPDP                       发 SPDP
//   收 SEDP Publication           收 SEDP Subscription（学习对端 Reader 地址）
//   发 SEDP Subscription          发 SEDP Publication
//   收 HEARTBEAT → 回 ACKNACK     发 HEARTBEAT → 收 ACKNACK → 重传
//   收用户 DATA                   发用户 DATA
//
// 复用：SpdpAnnouncer 的公告构造、ParameterSerializer、CDRMessage、
//       RTPSMessageCreator、SubmessageParser —— 与 Reader 侧同一套基础层。
//
// ===========================================================================
// 已沿用的实测教训（与 Reader 侧相同，勿"简化"）
// ===========================================================================
//   1. DATA 子消息必须含 extraFlags，且 octetsToNextHeader 直接写 buffer
//   2. PROTOCOL_VERSION / VENDORID 的参数 length 用 4（规范字面是 2）
//   3. 接收侧用 DataTypePrefixSize 判定前缀，不信版本号
//   4. 带 INFO_DST 指明目标，否则对端不投递

#ifndef STRATUM_PROTOCOL_RTPS_RTPS_WRITER_H_
#define STRATUM_PROTOCOL_RTPS_RTPS_WRITER_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <deque>
#include <random>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <vector>

#include <stratum/core/status.h>
#include <stratum/core/types.h>
#include <stratum/protocol/rtps/rtps_reader.h>   // 复用 PeerAddress
// ReceivedSample：收尾补救需要保存原始样本（见 PendingSample::sample）
#include <stratum/protocol/idds_backend.h>

namespace stratum {
namespace protocol {
namespace rtps {

struct RtpsWriterConfig {
  uint32_t domain_id = 0;
  std::string participant_name = "stratum_player";

  std::vector<uint8_t> guid_prefix;   // 空 = 自动（01 0f + "STRATUM" + 序号）

  // 我方 metatraffic（接收 ACKNACK/SEDP）与 user（发送数据）端口
  uint16_t metatraffic_port = 0;
  uint16_t user_port = 0;
  // 对外声明的单播地址（空 = 自动探测本机对外地址）。
  // 跨主机通信必须声明可达地址，127.0.0.1 只在同机有效。
  std::string advertised_address;

  // 【跨主机发现】单播 SPDP 对端列表（等价于 Fast DDS 的 initialPeersList）。
  //
  // 为什么必须有：SPDP 默认只发组播 239.255.0.1，而**组播跨不过 NAT 边界**
  // （实测：本机 WSL 为 nat 模式，与局域网内虚拟机的组播不通）。
  // 跨主机时必须显式告诉对方"往这个地址发 SPDP"，否则双方永远发现不了彼此。
  // 可用环境变量 STRATUM_INITIAL_PEERS 传入（逗号分隔多个地址）。
  std::vector<std::string> initial_peers;

  // UDP 载荷上限（0 = 不限制）。
  //
  // 【为什么需要】MTU 1500 的链路上，不分片的 UDP 上限是 1472 字节；
  // 超过就需要 IP 分片，而**分片在 WSL NAT / VMware NAT / UDP 中继这类路径上
  // 不会被转发** —— 包发出去就没了，发送端却照常计入"已发布样本"，
  // 对端一条都收不到（本项目跨主机测试实测到这一点）。
  // 这属于静默失败，违反项目原则，所以提供这个上限让失败可见。
  //
  // 默认 0：单机回环的 MTU 是 65536，大样本（如 60000B）不分片也能过，
  // 因此默认不限制以保持既有行为；跨主机测试时把它设为 1472。
  uint32_t max_udp_payload = 0;

  // 是否自动探测路径 MTU（默认开）。
  //
  // 打开时：若 max_udp_payload 未显式设置（== 0），则用 getsockopt(IP_MTU)
  // 查询默认路由的 MTU，取 mtu - 28（IP 头 20 + UDP 头 8）作为单报文的
  // UDP 载荷上限，超出的样本自动走 DATA_FRAG 分片。
  //
  // 为什么默认开：MTU 1500 的路径上不分片上限就是 1472 字节，超过依赖
  // IP 分片，而 IP 分片在 NAT/中继路径上会被丢弃。自动探测让"大 payload
  // 跨主机"默认就能工作，不必手工设 STRATUM_MAX_UDP_PAYLOAD。
  // 单机场景（loopback MTU 很大）探测结果也很大，行为与不限制基本一致。
  bool auto_mtu_detect = true;

  uint32_t announce_interval_ms = 200;
  uint32_t heartbeat_interval_ms = 100;

  // 发送窗口（流控）：未确认样本数达到该值就暂停发送，等对端 ACK 推进。
  //
  // 【为什么必须流控】没有它时回放器以最快速度灌数据（实测 ≈8 万条/秒），
  // 远超接收端处理能力 → UDP 接收缓冲溢出 → 丢包：
  //   8000 B @ 5 万条/秒 丢 22%；8000 B @ 尽快 丢 39%；小包高速偶发丢 1~3 条。
  // "可靠回放不应丢数据"，所以窗口必须存在。0 = 关闭（仅用于吞吐压测）。
  // 【默认关闭】开启后实测会让正常回放退化（对账 A 从 200/200 掉到 184、
  // 且首条补救重发被窗口等待拖后），因此默认 0；需要"高速回放也尽量不丢"
  // 时再显式开启（例如 send_window=128, send_window_bytes=65536,
  // send_window_timeout_ms=1000）。
  //
  // 现状说明：即使开启，也**只能减轻**而不能根治高速突发丢包 ——
  // 重传路径（HandleAcknack 的补发、收尾重发）不经过窗口，仍是突发来源。
  // ---- 发送速率限制（不过依赖对端 ACK，对**所有**发送路径生效）----
  //
  // 背景：早期尝试用"未确认样本窗口"限流，但 acked 标志依赖对端 ACKNACK 的
  // 及时性 —— 对端 ACK 慢时窗口长期占满，反而拖慢正常回放
  // （实测对账 A 从 200/200 掉到 184）。而且重传路径（补发、收尾重发）
  // 绕过窗口，突发依旧。
  //
  // 现改为**按时间的滑动窗口限速**（1 秒窗口），并且在 Publish、两处重发、
  // 收尾重发**四条路径**上都调用 —— 这样"突发"从根上被削平，
  // 且不依赖对端行为，正常回放（原速，流量远低于上限）完全不受影响。
  //
  // 0 = 该项不限。默认值按实测"接收端能稳定消化"的量级选取。
  // 【默认关闭】实测限速**并未消除**高速档丢包（连续两轮尝试：ACK 窗口、
  // 滑动窗口限速），只是把吞吐从 ≈8 万条/秒压到 ≈2 万条/秒。
  // 无收益、有代价 → 默认 0（关闭）；保留配置供实验用。
  // ---- 调试用丢包注入（异常路径测试；默认 0 = 完全不丢）----
  //
  // 【为什么要这个开关】PROJECT_RULES §3 要求"涉及网络/IO 必须包含异常路径"。
  // 常规做法是用 `tc netem` 或用户态 UDP 代理，但本环境：
  //   · `tc` 不可用（WSL 权限）；`iptables` 存在但 Permission denied；
  //   · 用户态代理**插不进 RTPS 之间** —— 对端 locator 是协商出来的、端口固定，
  //     代理无法占用那个端口（除非改对端配置）。
  // 因此改为在**本进程的发送路径**上按概率丢包：可控、可复现、无需 root，
  // 而且能**分别**测"发送方向丢包"与"接收方向丢包"。
  // 生产默认 0，不影响任何正常行为。
  uint32_t debug_drop_send_percent = 0;
  uint64_t max_samples_per_sec = 0;
  uint64_t max_bytes_per_sec = 0;

  // 旧的"未确认窗口"方案保留但默认关闭（见续 32 的教训：它依赖对端 ACK 及时性）。
  uint32_t send_window = 0;                // 未确认条数上限（0 = 关闭）

  // 未确认**字节数**上限。
  //
  // 【为什么条数不够】只限条数时，128 条 × 8000 B = 1 MB，仍远超对端
  // UDP 接收缓冲（~212 KB）→ 依然丢包（实测 8000 B 档改条数窗口后仍丢 43%/35%）。
  // 必须同时按字节限流；64 KB 远小于缓冲，留足余量。
  uint32_t send_window_bytes = 65536;
  uint32_t send_window_timeout_ms = 1000;  // 到点仍未放开就继续发，避免永久阻塞
};

// 一条待确认的已发数据（用于 ACKNACK 触发的重传）
struct PendingSample {
  uint64_t sn = 0;
  std::vector<uint8_t> packet;   // 完整 RTPS 报文（可原样重发）
  bool acked = false;
  // 原始样本副本。用途：VOLATILE durability 下最早发出的数据会永久丢失
  // （reader 不索要匹配完成前的数据，重发**相同 SN** 无效），
  // 上层据此用**新 SN** 重新发布同一份内容（见 Player 的收尾补救）。
  ::stratum::protocol::ReceivedSample sample;

  // 【分片样本的重传】分片路径不缓存那个可能超过 64KiB 的完整报文
  // （packet 留空），改为记录"这个样本需要分片"以及分片参数，
  // 重传时按原始 payload **重新切片**发送。
  // 这样既保留了重传能力，又不会让 pending_ 里堆积巨型缓冲。
  bool is_fragmented = false;
  uint32_t writer_entity = 0;
  uint16_t frag_payload = 0;      // 每片样本字节数
};

class RtpsWriter {
 public:
  RtpsWriter();
  ~RtpsWriter();

  RtpsWriter(const RtpsWriter&) = delete;
  RtpsWriter& operator=(const RtpsWriter&) = delete;

  Status Open(const RtpsWriterConfig& config);
  Status Start();
  Status Stop();

  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  // 发布一条样本。首次见到 (topic,type) 时会先发送该话题的 SEDP Publication。
  Status Publish(const std::string& topic_name, const std::string& type_name,
                 const std::string& payload, TimestampNs source_ts_ns);

  // 是否已有对端 reader 完成匹配（收到过它的 SEDP Subscription 与 ACKNACK）。
  //
  // 判据用"Subscription + ACKNACK 都到过"：前者说明对端 reader 存在，
  // 后者说明它已经处理过我们的 writer —— 此时发第一条才不会被 VOLATILE
  // 语义丢掉（见 ISink::ReadyForPublish 的说明）。
  bool HasMatchedReader() const {
    return st_readers_.load(std::memory_order_relaxed) > 0 &&
           st_acks_.load(std::memory_order_relaxed) > 0;
  }

  // 尚未被对端确认的样本（副本）。供上层用新 SN 重新发布 —— 见 Player 收尾补救。
  std::vector<::stratum::protocol::ReceivedSample> UnackedSamples() const;

  // 等待所有已发数据被确认（在 timeout 内轮询 acked 标志）
  Status WaitForAcknowledgments(uint64_t timeout_ms);

  struct Stats {
    uint64_t packets_total = 0;
    uint64_t spdp_received = 0;
    uint64_t sedp_subscriptions = 0;
    uint64_t remote_readers = 0;
    uint64_t data_sent = 0;
    uint64_t heartbeats_sent = 0;
    uint64_t acknacks_received = 0;
    uint64_t retransmits = 0;
    uint64_t publications_announced = 0;
    // SEDP Publication 构造失败的次数（例如 type_name 为空导致
    // PID_TYPE_NAME 无法序列化）。必须可观测：这个失败在上游是静默的，
    // 曾让"对端收不到数据"的排查绕了很远（见 AI_COLLAB 记录）。
    uint64_t publications_build_failed = 0;
    // 收到对端 HEARTBEAT 的次数，以及因此回出的 ACKNACK 次数。
    // 这两个数一起看，就能区分"对端没发心跳"与"我们没应答"。
    uint64_t heartbeats_received = 0;
    uint64_t acknacks_sent = 0;
    uint64_t peers_learned = 0;
  };
  Stats GetStats() const;

  const std::string& last_error() const { return last_error_; }

 private:
  struct Peer {
    PeerAddress meta;              // 对端 metatraffic（发 SEDP/HEARTBEAT）
    PeerAddress user;              // 对端 user locator（发数据）
    bool has_user = false;
    uint8_t guid_prefix[12] = {0};
  };

  void ReceiveLoop();
  void AnnounceOnce();
  // 调试用：按 debug_drop_send_percent 的概率决定是否丢弃本次发送。
  bool ShouldDropForTest();

  // 速率限制：必要时等待，使 1 秒窗口内的样本数/字节数不超过配置上限。
  // 四条发送路径（Publish / 两处重发 / 收尾重发）都必须调用。
  void ThrottleOrWait(size_t bytes);

  std::mutex throttle_mutex_;
  std::deque<std::pair<std::chrono::steady_clock::time_point, size_t>> send_log_;
  std::atomic<uint64_t> st_throttle_timeouts_{0};
  std::atomic<uint64_t> st_debug_dropped_{0};
  std::atomic<uint64_t> st_frag_samples_{0};   // 走 DATA_FRAG 的样本数
  std::atomic<uint64_t> st_frag_packets_{0};   // DATA_FRAG 报文总数

   // 调试丢包注入实际丢弃的条数

  // 发送窗口流控（见 RtpsWriterConfig::send_window）。
  void WaitForSendWindow();

  void SendHeartbeats();
  void ReannouncePublications();
  void FlushPendingToPeers();
  void HandleSpdp(const uint8_t* packet, size_t len, const sockaddr_in& src);
  void HandleSedpSubscription(const uint8_t* packet, size_t len);
  void HandleAcknack(const uint8_t* packet, size_t len, uint32_t body,
                     const eprosima::fastdds::rtps::SubmessageHeader_t& smh);
  // 收到对端 SEDP writer 的 HEARTBEAT 后回 ACKNACK。
  //
  // 【为什么必须有】对端的 SEDP 公告走 RTPS 可靠传输：它先把公告写进历史，
  // 然后靠"HEARTBEAT → 对端 ACKNACK → 重传"把历史推给新匹配的 reader。
  // 我方若不回 ACKNACK，对端就认为没有 reader 需要数据，
  // **永远收不到它的 SEDP Subscription** → 学不到对端 user locator
  // → 用户数据全被发到 metatraffic 端口 → 对端 DataReader 收到 0 条。
  // 实测（STRATUM_DUMP 逐子消息统计）：收到 78 个 HEARTBEAT、
  // 0 条 SEDP Subscription DATA。
  void HandleHeartbeat(const uint8_t* packet, size_t len, uint32_t body,
                       const eprosima::fastdds::rtps::SubmessageHeader_t& smh,
                       const sockaddr_in& src);
  void SendAcknackToPeer(const Peer& peer, uint32_t writer_entity,
                         int64_t first_sn, int64_t last_sn);


  std::vector<uint8_t> BuildPublication(const std::string& topic,
                                        const std::string& type);
  std::vector<uint8_t> BuildDataPacket(uint64_t sn, const std::string& payload,
                                       TimestampNs source_ts_ns,
                                       uint32_t writer_entity);

  Status OpenSockets();
  void CloseSockets();

  RtpsWriterConfig config_;
  std::string last_error_;

  int fd_spdp_ = -1;   // SPDP 组播监听（见 OpenSockets 说明）
  int fd_meta_ = -1;
  // user 端口监听：对端 DataReader 的 ACKNACK 发到这里（见 OpenSockets 说明）。
  // 不绑它 → pending 永不确认、wait_for_ack 只能超时、条数对账总差尾条。
  int fd_user_ = -1;
  int fd_tx_ = -1;

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread thread_;

  mutable std::mutex mutex_;
  std::vector<Peer> peers_;

  // 话题 → writer entityId 映射。
  //
  // 【为什么必须每话题一个】RTPS 中一个 DataWriter 只能对应**一个** topic。
  // 此前所有话题共用固定的 0x00000103，对端 Fast DDS 会看到"同一个 writer
  // 公告了多个不同 topic"，于是只认其中一个（或直接判定异常），
  // 多话题并发必然串扰/丢话题。
  // entityId 布局 = [entityKey(3B) | entityKind(1B)]，kind=0x03（user writer with key），
  // entityKey 从 1 递增（key=1 即原来的 0x00000103，保持单话题兼容）。
  std::map<std::string, uint32_t> topic_writers_;
  uint32_t next_entity_key_ = 1;

  // 每个 writer 自己的序列号计数器（RTPS 要求各 writer 的 SN 独立且连续）。
  std::map<uint32_t, uint64_t> writer_sn_;

  // 取（必要时分配）某话题的 writer entityId。
  uint32_t WriterEntityFor(const std::string& topic, const std::string& type);
  // 取该 writer 的下一个 SN。
  uint64_t NextSnFor(uint32_t writer_entity);

  // 已收到的 DATA 序列号（按 writer entityId 分组）。
  //
  // 【为什么需要】此前 SendAcknackToPeer 每次把心跳宣告的 [firstSN,lastSN]
  // **整段**说成缺失，等于宣称"我一条都没收到"：对端的 SEDP writer 因此
  // 反复重传全部历史公告，且持续打印 `Inconsistent acknack received`。
  // 与 rtps_reader.cc 中的同名成员同构（理由见那里的完整说明）。
  std::map<uint32_t, std::set<uint64_t>> received_sn_;
  void RecordReceivedSn(uint32_t writer_entity, uint64_t sn);
  // topic|type → (topic, type)：保存内容以便周期重发公告。
  // 【为什么必须周期重发】回放时 SEDP Publication 若只发一次，
  // 会与数据同时到达对端 —— 对端尚未完成匹配，数据被丢弃。
  // 实测：只发一次时 Fast DDS subscriber 收到 0 条。
  std::map<std::string, std::pair<std::string, std::string>> announced_;
  std::vector<PendingSample> pending_;         // 已发待确认

  // 用户数据（user writer 0x00000103）的序列号计数器。
  uint64_t next_sn_ = 0;

  // SEDP Publication（builtin writer 0x000003c2）的序列号计数器。
  //
  // 【为什么必须独立】RTPS 要求**每个 writer 的 SN 序列各自独立且连续**。
  // 曾经 BuildPublication 与 Publish 共用 next_sn_，而 Publication 每 100ms
  // 就要重发一次，于是用户数据的 SN 被公告"吃掉"号段：实际发出的是
  // 1, 5, 9, ... 这类跳号序列。对端 StatefulReader 认为 2/3/4 永远缺失，
  // 于是**一条样本都不会交付给应用** —— 而两端都没有任何报错，
  // 表现为"匹配成功、数据也发出去了，就是收不到"。
  uint64_t sedp_sn_ = 0;

  uint8_t my_prefix_[12] = {0};
  uint32_t my_writer_entity_ = 0;              // user writer 的 EntityId 数值

  std::vector<uint8_t> spdp_payload_;

  std::atomic<uint64_t> st_packets_{0};
  std::atomic<uint64_t> st_send_errors_{0};   // sendto 失败次数（大 payload 会暴露）
  std::atomic<uint64_t> st_spdp_{0};
  std::atomic<uint64_t> st_sedp_sub_{0};
  std::atomic<uint64_t> st_readers_{0};
  std::atomic<uint64_t> st_data_sent_{0};
  std::atomic<uint64_t> st_window_timeouts_{0};
  std::atomic<uint64_t> st_window_bytes_waits_{0};   // 流控窗口等待超时次数（对端迟迟不 ACK）
  std::atomic<uint64_t> st_heartbeats_{0};
  std::atomic<uint64_t> st_acks_{0};
  std::atomic<uint64_t> st_retrans_{0};
  std::atomic<uint64_t> st_pubs_{0};
  std::atomic<uint64_t> st_pub_build_failed_{0};
  std::atomic<uint64_t> st_hb_recv_{0};
  std::atomic<uint64_t> st_acknack_sent_{0};
};

// 【O10 取证】ACKNACK 三条判定路径的进程内累计计数。
//
// 为什么要自由函数而不是成员方法：`RtpsBackend` 只是工厂（不持有 writer），
// 而 writer 是 `Player` 内部通过 `CreateSink` 建出来的，外部拿不到句柄。
// 测试工具只需要知道"位图分支是否被走到（>0）"，进程内计数足够，
// 也避免为取证去改 Player/Backend 的接口。
struct AckPathStats {
  uint64_t base_acked = 0;       // ① low < base_sn：reader 已收到
  uint64_t bitmap_missing = 0;   // ② 位图位为 1：缺失，需重传（关键证据）
  // 【关键】位图**位被解析过**的次数（无论最终判缺失还是不缺失）。
  // 之前只统计"判为缺失"，而实测中重传很及时，位图窗口内多数时候已无缺失，
  // 于是 bitmap_missing 恒 0 —— 掩盖了"位图路径其实一直在走"这个事实。
  uint64_t bitmap_checked = 0;
  uint64_t beyond_bitmap = 0;    // ③ 位图未覆盖：视为缺失
};
AckPathStats AckPathStatsSnapshot();

}  // namespace rtps
}  // namespace protocol
}  // namespace stratum

#endif  // STRATUM_PROTOCOL_RTPS_RTPS_WRITER_H_
