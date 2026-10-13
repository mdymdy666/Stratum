// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// RtpsWriter 实现

#include <stratum/protocol/rtps/rtps_writer.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>

#include <stratum/protocol/rtps/rtps_packet_utils.h>
#include <stratum/rtps/builtin/sedp_parser.h>
#include <stratum/rtps/builtin/spdp_parser.h>
#include <stratum/rtps/core/policy/ParameterSerializer.hpp>
#include <stratum/rtps/dds/core/policy/ParameterTypes.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>
#include <stratum/rtps/messages/RTPSMessageCreator.hpp>
#include <stratum/rtps/messages/submessage_parser.h>

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


using namespace eprosima::fastdds::rtps;
using ::stratum::rtps::BuildSpdpAnnouncement;
using ::stratum::rtps::DataTypePrefixSize;
using ::stratum::rtps::HeartbeatInfo;
using ::stratum::rtps::MakeDefaultUdpv4Locator;

namespace {
// 【O10】进程内累计：ACKNACK 三条判定路径各走了多少次。
std::atomic<uint64_t> g_ack_base_acked{0};
std::atomic<uint64_t> g_ack_bitmap_missing{0};
std::atomic<uint64_t> g_ack_bitmap_checked{0};
std::atomic<uint64_t> g_ack_beyond_bitmap{0};
}  // namespace
using ::stratum::rtps::SedpEndpointData;
using ::stratum::rtps::SedpParser;
using ::stratum::rtps::SpdpAnnounceConfig;
using ::stratum::rtps::SpdpParser;
using ::stratum::rtps::SpdpParticipantData;
using ::stratum::rtps::SubmessageParser;
namespace dds = eprosima::fastdds::dds;
namespace pkt = ::stratum::protocol::rtps::packet;

namespace {

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

}  // namespace

RtpsWriter::RtpsWriter() = default;
RtpsWriter::~RtpsWriter() { Stop(); }

Status RtpsWriter::Open(const RtpsWriterConfig& config) {
  if (fd_meta_ >= 0) return Status::InvalidArgument("RtpsWriter 已打开");
  config_ = config;

  if (config_.guid_prefix.size() == 12) {
    std::memcpy(my_prefix_, config_.guid_prefix.data(), 12);
  } else {
    // 前 2 字节必须是 vendorId（RTPS 9.3.1.1），否则对端会忽略；
    // 其余按 hostId/processId 生成，保证跨进程唯一（见下方 helper 说明）。
    uint8_t def[12] = {0};
    FillDefaultGuidPrefix(def);
    std::memcpy(my_prefix_, def, 12);
  }
  // user writer 的 EntityId：0x00000103（user writer with key）
  my_writer_entity_ = 0x00000103u;

  const uint16_t meta_port =
      config_.metatraffic_port != 0
          ? config_.metatraffic_port
          : static_cast<uint16_t>(9060 + config_.domain_id * 10);
  const uint16_t user_port =
      config_.user_port != 0 ? config_.user_port
                             : static_cast<uint16_t>(meta_port + 1);

  SpdpAnnounceConfig sc;
  std::memcpy(sc.guid_prefix.value, my_prefix_, 12);
  sc.domain_id = config_.domain_id;
  sc.builtin_endpoints = 0x000f0c3f;
  sc.participant_name = config_.participant_name;
  const std::string adv = config_.advertised_address.empty()
                            ? pkt::DetectLocalAddress()
                            : config_.advertised_address;
  sc.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator(adv.c_str(), meta_port));
  sc.default_unicast.push_back(MakeDefaultUdpv4Locator(adv.c_str(), user_port));
  spdp_payload_ = BuildSpdpAnnouncement(sc);
  if (spdp_payload_.empty()) {
    last_error_ = "组装 SPDP 公告失败";
    return Status::Internal(last_error_);
  }
  return OpenSockets();
}

Status RtpsWriter::OpenSockets() {
  const uint16_t meta_port =
      config_.metatraffic_port != 0
          ? config_.metatraffic_port
          : static_cast<uint16_t>(9060 + config_.domain_id * 10);

  fd_meta_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_meta_ < 0) return Status::IOError("socket 失败");
  int reuse = 1;
  ::setsockopt(fd_meta_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(meta_port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd_meta_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
    ::close(fd_meta_);
    fd_meta_ = -1;
    last_error_ = "绑定 metatraffic 端口失败（" + std::to_string(meta_port) + "）";
    return Status::IOError(last_error_);
  }
  // 加入 SPDP 组播组 —— 【必须】，理由见 rtps_reader.cc 中同一处的说明。
  // 不加入则收不到对端的 SPDP 与 SEDP Subscription，
  // 于是不知道把数据发往何处（实测回放时对端收到 0 条）。
  {
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = ::inet_addr("239.255.0.1");
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    ::setsockopt(fd_meta_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
  }

  // 【必须同时监听 user 端口】
  //
  // 我们在 SEDP Publication 里声明的 unicast locator 是 user 端口，
  // 对端的 DataReader 会把 ACKNACK 发到那里。只绑 metatraffic 端口时
  // 这些 ACKNACK 全部打进空端口：本端 pending 永不确认、`wait_for_ack`
  // 只能超时，回放时**最后一条往往来不及被确认**（实测条数对账稳定差 1）。
  // 与 RtpsReader 的双端口监听保持一致。
  {
    const uint16_t user_port =
        config_.user_port != 0 ? config_.user_port
                               : static_cast<uint16_t>(meta_port + 1);
    fd_user_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_user_ < 0) {
      CloseSockets();
      last_error_ = "创建 user 端口 socket 失败";
      return Status::IOError(last_error_);
    }
    ::setsockopt(fd_user_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in ua{};
    ua.sin_family = AF_INET;
    ua.sin_port = htons(user_port);
    ua.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd_user_, reinterpret_cast<sockaddr*>(&ua), sizeof(ua)) < 0) {
      ::close(fd_user_);
      fd_user_ = -1;
      last_error_ = "绑定 user 端口失败（" + std::to_string(user_port) + "）";
      return Status::IOError(last_error_);
    }
  }

  // SPDP 公告发往组播端口 7400+domain*250，与 metatraffic 端口不同，
  // 必须单独绑定该端口才能收到新参与者的自我介绍。
  // 详见 rtps_reader.cc 中同一处的完整说明（含实测症状）。
  {
    const uint16_t spdp_port =
        static_cast<uint16_t>(7400 + config_.domain_id * 250);
    fd_spdp_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_spdp_ >= 0) {
      int reuse2 = 1;
      ::setsockopt(fd_spdp_, SOL_SOCKET, SO_REUSEADDR, &reuse2, sizeof(reuse2));
      sockaddr_in sa{};
      sa.sin_family = AF_INET;
      sa.sin_port = htons(spdp_port);
      sa.sin_addr.s_addr = htonl(INADDR_ANY);
      if (::bind(fd_spdp_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        ::close(fd_spdp_);
        fd_spdp_ = -1;
      } else {
        ip_mreq mreq2{};
        mreq2.imr_multiaddr.s_addr = ::inet_addr("239.255.0.1");
        mreq2.imr_interface.s_addr = htonl(INADDR_ANY);
        ::setsockopt(fd_spdp_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq2,
                     sizeof(mreq2));
      }
    }
  }
  fd_tx_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_tx_ < 0) {
    CloseSockets();
    return Status::IOError("创建发送 socket 失败");
  }

  return Status::OK();
}

void RtpsWriter::CloseSockets() {
  if (fd_spdp_ >= 0) { ::close(fd_spdp_); fd_spdp_ = -1; }
  if (fd_meta_ >= 0) { ::close(fd_meta_); fd_meta_ = -1; }
  if (fd_user_ >= 0) { ::close(fd_user_); fd_user_ = -1; }
  if (fd_tx_ >= 0) { ::close(fd_tx_); fd_tx_ = -1; }
}

Status RtpsWriter::Start() {
  if (running_.load(std::memory_order_acquire)) {
    return Status::InvalidArgument("RtpsWriter 已在运行");
  }
  if (fd_meta_ < 0) return Status::InvalidArgument("RtpsWriter 尚未 Open");
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this]() { ReceiveLoop(); });
  return Status::OK();
}

Status RtpsWriter::Stop() {
  if (running_.load(std::memory_order_acquire)) {
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
  }
  CloseSockets();
  return Status::OK();
}

// ---------------------------------------------------------------------------
// 接收循环：学习对端地址 + 处理 ACKNACK
// ---------------------------------------------------------------------------

void RtpsWriter::ReceiveLoop() {
  SubmessageParser subm;
  SpdpParser spdp;
  SedpParser sedp;

  // STRATUM_DUMP=1 时逐包逐子消息打印：用来区分
  //   "对端根本没发" 与 "发了但我们没识别/没处理"。
  // 排查 SEDP Subscription 收不到时必需 —— Fast DDS 的 SEDP 公告走可靠
  // 重传，我方不回 ACKNACK 就永远拿不到它，而这一点从计数器上看不出来。
  const bool dump_enabled = (std::getenv("STRATUM_DUMP") != nullptr);

  const auto announce_interval = std::chrono::milliseconds(
      config_.announce_interval_ms == 0 ? 200 : config_.announce_interval_ms);
  const auto hb_interval = std::chrono::milliseconds(
      config_.heartbeat_interval_ms == 0 ? 100 : config_.heartbeat_interval_ms);

  auto last_announce = std::chrono::steady_clock::now() - announce_interval;
  auto last_hb = std::chrono::steady_clock::now();

  pollfd fds[3];
  fds[0].fd = fd_spdp_;   // SPDP 组播
  fds[0].events = POLLIN;
  fds[1].fd = fd_meta_;   // 单播：SEDP Subscription / ACKNACK
  fds[1].events = POLLIN;
  fds[2].fd = fd_user_;   // user 端口：对端 DataReader 的 ACKNACK
  fds[2].events = POLLIN;

  while (!stop_requested_.load(std::memory_order_acquire)) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_announce >= announce_interval) {
      AnnounceOnce();
      last_announce = now;
    }
    if (now - last_hb >= hb_interval) {
      SendHeartbeats();
      last_hb = now;
      // 周期性重发 SEDP Publication：对端可能在数据发出之后才完成匹配，
      // 必须让它持续有机会看到我们的 Writer。
      ReannouncePublications();
    }

    if (::poll(fds, 3, 50) <= 0) continue;

    for (int idx = 0; idx < 3; ++idx) {
    if ((fds[idx].revents & POLLIN) == 0) continue;

    uint8_t buf[65536];
    sockaddr_in src{};
    socklen_t sl = sizeof(src);
    const ssize_t n = ::recvfrom(fds[idx].fd, buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&src), &sl);
    if (n <= 0) continue;
    st_packets_.fetch_add(1, std::memory_order_relaxed);
    char src_ip[32] = {0};
    const unsigned src_port = static_cast<unsigned>(ntohs(src.sin_port));
    if (dump_enabled) {
      ::inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));
    }
    if (dump_enabled) {
      std::fputs("[dump] n=", stderr);
      std::fprintf(stderr, "%zd", n);
      std::fputs(" fd=", stderr);
      std::fprintf(stderr, "%d", fds[idx].fd);
      std::fputs(" head:", stderr);
      // 120 字节：覆盖 SPDP 参数表开头（能看到 PID 0x0015/0x0016/0x0050 以及
      // locator PID 0x0031/0x0032），否则只能看到子消息头，无法判断
      // "收到的到底是谁的公告、里面写的端口是多少"。
      for (int di = 0; di < 120 && di < n; ++di) {
        std::fprintf(stderr, " %02x", buf[di]);
      }
      std::putc(10, stderr);
      std::fflush(stderr);
    }
    if (n < 20 || std::memcmp(buf, "RTPS", 4) != 0) continue;

    const uint8_t ver_minor = buf[5];
    CDRMessage_t msg(static_cast<uint32_t>(n) + 8);
    std::memcpy(msg.buffer, buf, n);
    msg.length = static_cast<uint32_t>(n);
    msg.pos = 20;

    while (msg.pos + 4 <= msg.length) {
      const uint32_t before = msg.pos;
      SubmessageHeader_t smh;
      if (!subm.ReadSubmessageHeader(&msg, &smh)) {
        break;
      }
      const uint32_t body = msg.pos;

      if (smh.submessageId == pkt::kSubAcknack) {
        HandleAcknack(buf, static_cast<size_t>(n), body, smh);
        if (dump_enabled) {
          std::fprintf(stderr, "[rx] ACKNACK from=%s:%u len=%u\n", src_ip,
                       src_port, smh.submessageLength);
        }
      } else if (smh.submessageId == pkt::kSubData) {
        const size_t prefix =
            DataTypePrefixSize(msg.buffer + body, msg.length - body, ver_minor);
        const size_t ids = body + (prefix - 18) + 2;
        if (ids + 8 <= msg.length) {
          const uint32_t wid = pkt::EntityVal(msg.buffer + ids + 4);
          if (dump_enabled) {
            std::fprintf(stderr, "[rx] DATA writer=0x%08x from=%s:%u len=%u\n",
                         wid, src_ip, src_port, smh.submessageLength);
          }
          // 记录已收到的 SN（ACKNACK 据此报告真实缺口）。
          // writerSN 是 8 字节：低 4 字节在 ids+12（ids+8 是恒为 0 的高位）。
          if (ids + 16 <= msg.length) {
            uint32_t sn_low = 0;
            std::memcpy(&sn_low, msg.buffer + ids + 12, 4);
            RecordReceivedSn(wid, sn_low);
          }
          if (wid == pkt::kEntitySpdpWriter) {
            HandleSpdp(buf, static_cast<size_t>(n), src);
          } else if (wid == pkt::kEntitySedpSubWriter) {
            HandleSedpSubscription(buf, static_cast<size_t>(n));
          }
        }
      } else if (smh.submessageId == pkt::kSubHeartbeat) {
        HandleHeartbeat(buf, static_cast<size_t>(n), body, smh, src);
        if (dump_enabled) {
          std::fprintf(stderr, "[rx] HEARTBEAT from=%s:%u len=%u\n", src_ip,
                       src_port, smh.submessageLength);
        }
      } else if (dump_enabled) {
        // 把"收到的其它子消息"也打出来：判断对端是否发了 HEARTBEAT 是
        // 定位"SEDP Subscription 收不到"的关键（Fast DDS 的 SEDP 公告靠
        // 可靠重传，我方不回 ACKNACK 就永远拿不到）。
        std::fprintf(stderr, "[rx] sm=0x%02x len=%u from=%s:%u\n",
                     smh.submessageId, smh.submessageLength, src_ip, src_port);
      }

      if (smh.is_last || body + smh.submessageLength <= before) break;
      msg.pos = body + smh.submessageLength;
    }
    }   // for (idx)
  }
}

void RtpsWriter::AnnounceOnce() {
  if (fd_tx_ < 0 || spdp_payload_.empty()) return;
  const uint16_t port =
      static_cast<uint16_t>(7400 + config_.domain_id * 250);
  // 【跨主机发现】组播之外，再向单播对端列表发 SPDP（见 config_.initial_peers
  // 的说明：NAT 环境下组播不通，只能靠单播互相发现）。
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

void RtpsWriter::HandleSpdp(const uint8_t* packet, size_t len,
                            const sockaddr_in& src) {
  st_spdp_.fetch_add(1, std::memory_order_relaxed);
  SpdpParser parser;
  std::vector<SpdpParticipantData> pds;
  parser.ParseMessage(packet, len, &pds);

  const bool dump = (std::getenv("STRATUM_DUMP") != nullptr);

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
      if (dump) {
        std::fputs("[spdp] 自身公告（按 GUID 前缀过滤）\n", stderr);
        std::fflush(stderr);
      }
      continue;
    }
    if (dump) {
      // 打印解析出的对端前缀、metatraffic locator **与来源地址**：
      // 只看 locator 无法判断"这个 peer 是不是我在等的那个程序"，
      // 必须连来源一起看（本项目曾因此把环境里的陌生参与者当成对端）。
      char sip[32] = {0};
      ::inet_ntop(AF_INET, &src.sin_addr, sip, sizeof(sip));
      std::fprintf(stderr,
                   "[spdp] peer prefix=%02x%02x%02x%02x from=%s:%u locators=%zu:",
                   pd.guid.guidPrefix.value[0], pd.guid.guidPrefix.value[1],
                   pd.guid.guidPrefix.value[2], pd.guid.guidPrefix.value[3],
                   sip, static_cast<unsigned>(ntohs(src.sin_port)),
                   pd.metatraffic_unicast_locators.size());
      for (const Locator_t& loc : pd.metatraffic_unicast_locators) {
        std::fprintf(stderr, " %u.%u.%u.%u:%u", loc.address[12], loc.address[13],
                     loc.address[14], loc.address[15],
                     static_cast<unsigned>(loc.port));
      }
      std::putc(10, stderr);
      std::fflush(stderr);
    }
    for (const Locator_t& loc : pd.metatraffic_unicast_locators) {
      Peer p;
      p.meta.port = htons(static_cast<uint16_t>(loc.port));
      std::memcpy(&p.meta.ip, &loc.address[12], 4);
      std::memcpy(p.guid_prefix, pd.guid.guidPrefix.value, 12);
      bool dup = false;
      for (const auto& e : peers_) {
        if (e.meta.port == p.meta.port && e.meta.ip == p.meta.ip) dup = true;
      }
      if (!dup) peers_.push_back(p);
    }
  }
}

void RtpsWriter::HandleSedpSubscription(const uint8_t* packet, size_t len) {
  st_sedp_sub_.fetch_add(1, std::memory_order_relaxed);
  SedpParser parser;
  std::vector<SedpEndpointData> eds;
  parser.ParseMessage(packet, len, &eds);

  // 发这条 Subscription 的对端 GUID 前缀（RTPS 头偏移 8..19）。
  // 用它把学到的 user locator **定向**写给该对端自己的 peer 条目。
  uint8_t sender_prefix[12] = {0};
  if (len >= 20) {
    std::memcpy(sender_prefix, packet + 8, 12);
  }

  bool newly_matched = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& ed : eds) {
      // 记录**该对端** Reader 的 user locator（用户数据要发到这里）。
      //
      // 【缺陷修复】这里曾经遍历 peers_ 把同一个 locator 写给所有对端。
      // 后果：多订阅端场景下"最后回 Subscription 的那个"会覆盖其它对端的
      // user locator，于是**只有一个订阅端能收到数据**，其余对端的数据
      // 被发到别人的端口上 —— 两端都没有任何报错。
      // 实测：Stratum 回放时让上游 hello_world subscriber 与自研诊断程序
      // 同时订阅，[send] 只出现一个端口，另一个恒为 0 样本。
      for (const Locator_t& loc : ed.unicast_locators) {
        const uint16_t port = static_cast<uint16_t>(loc.port);
        bool matched_peer = false;
        for (auto& p : peers_) {
          if (std::memcmp(p.guid_prefix, sender_prefix, 12) != 0) {
            continue;
          }
          if (!p.has_user) {
            newly_matched = true;   // 首次学到该对端的 user locator
          }
          p.user.port = htons(port);
          std::memcpy(&p.user.ip, &loc.address[12], 4);
          p.has_user = true;
          matched_peer = true;
        }
        if (!matched_peer && std::getenv("STRATUM_DUMP") != nullptr) {
          // 学到的 locator 找不到对应 peer：说明该对端的 SPDP 还没被处理
          // （或前缀不匹配）。必须能看到，否则又变成静默丢失。
          std::fprintf(stderr,
                       "[sedp] Subscription 来自未知前缀 %02x%02x%02x%02x，"
                       "locator %u.%u.%u.%u:%u 未绑定到任何 peer\n",
                       sender_prefix[0], sender_prefix[1], sender_prefix[2],
                       sender_prefix[3], loc.address[12], loc.address[13],
                       loc.address[14], loc.address[15],
                       static_cast<unsigned>(port));
          std::fflush(stderr);
        }
      }
    }
    st_readers_.fetch_add(1, std::memory_order_relaxed);
  }
  // 【必须在锁外调用】FlushPendingToPeers 内部会再次获取 mutex_，
  // 而 std::mutex 不可重入 —— 在持锁状态下调用会**自死锁**：
  //   接收线程永久阻塞 → 主线程随后的 Publish 也取不到锁 → 整个进程静默卡死
  //   （因 stdout 为全缓冲，卡死后连日志都不会落盘，
  //    表现为"统计停在某个时刻、此后毫无输出"，极难从现象反推）。
  // 学到对端 user locator 后，把之前因"尚无接收方"而缓存的数据补发。
  // 这对应 DDS 的可靠语义：Writer 不应在匹配建立前丢失已发布的数据。
  // 只在**首次**学到对端 user locator 时补发缓存数据。
  // Subscription 是周期公告（Fast DDS 默认 ~100ms 一次），每次收到都
  // 补发会让重传线性增长（实测 8 秒仍涨到 98 次且持续上升）。
  if (newly_matched) {
    FlushPendingToPeers();
  }
}

void RtpsWriter::FlushPendingToPeers() {
  std::vector<Peer> peers;
  std::vector<PendingSample> todo;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peers = peers_;
    for (const auto& p : pending_) {
      if (!p.acked) todo.push_back(p);
    }
  }
  if (peers.empty() || todo.empty()) return;

  for (const Peer& peer : peers) {
    if (!peer.has_user) continue;   // 还不知道对端的数据接收地址
    const sockaddr_in sa = ToSockaddr(peer.user);
    for (const auto& item : todo) {
      ThrottleOrWait(item.packet.size());   // 重传同样受限速约束
      ::sendto(fd_tx_, item.packet.data(), item.packet.size(), 0,
               reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
      st_retrans_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void RtpsWriter::ReannouncePublications() {
  std::vector<std::pair<std::string, std::string>> topics;
  std::vector<Peer> peers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : announced_) topics.push_back(kv.second);
    peers = peers_;
  }
  if (topics.empty() || peers.empty()) return;

  for (const auto& t : topics) {
    const auto pub = BuildPublication(t.first, t.second);
    if (pub.empty()) {
      // 同上：构造失败不能沉默。这里每 100ms 会被调用一次，
      // 所以只累计计数与记录 last_error_，不刷屏。
      st_pub_build_failed_.fetch_add(1, std::memory_order_relaxed);
      last_error_ = "SEDP Publication 周期性重发失败（topic=\"" + t.first +
                    "\" type=\"" + t.second + "\"）";
      continue;
    }
    for (const Peer& peer : peers) {
      const sockaddr_in sa = ToSockaddr(peer.meta);
      ::sendto(fd_tx_, pub.data(), pub.size(), 0,
               reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
    }
  }
}

// ---------------------------------------------------------------------------
// 收到对端 HEARTBEAT → 回 ACKNACK，把对端 SEDP writer 的历史拉过来
// ---------------------------------------------------------------------------
void RtpsWriter::HandleHeartbeat(const uint8_t* packet, size_t len,
                                 uint32_t body, const SubmessageHeader_t& smh,
                                 const sockaddr_in& src) {
  st_hb_recv_.fetch_add(1, std::memory_order_relaxed);

  // 用已移植、被真实流量验证过的 ProcHeartbeat 解析，
  // 不手工按偏移 memcpy —— 本项目教训：手工解析 HEARTBEAT 曾让 ACKNACK
  // 完全无效，且没有任何报错。
  CDRMessage_t msg(static_cast<uint32_t>(len) + 8);
  std::memcpy(msg.buffer, packet, len);
  msg.length = static_cast<uint32_t>(len);
  msg.pos = body;

  SubmessageParser subm;
  HeartbeatInfo hb;
  if (!subm.ProcHeartbeat(&msg, &smh, &hb)) return;

  const uint32_t writer_id = pkt::EntityVal(hb.writer_id.value);
  // 只有 SEDP subscriptions writer 的公告是我们需要的：
  // 它携带对端 DataReader 的 user locator（数据最终要发到那里）。
  if (writer_id != pkt::kEntitySedpSubWriter) return;

  const int64_t first_sn = hb.first_sn.to64long();
  const int64_t last_sn = hb.last_sn.to64long();
  if (last_sn < first_sn) return;   // 空历史，没有可请求的公告

  std::vector<Peer> peers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peers = peers_;
  }

  if (peers.empty()) {
    // 还没学到任何 peer：按源地址回一次（与 Reader 侧相同的兜底）。
    Peer p;
    p.meta.port = src.sin_port;
    std::memcpy(&p.meta.ip, &src.sin_addr, 4);
    std::memcpy(p.guid_prefix, packet + 8, 12);
    SendAcknackToPeer(p, writer_id, first_sn, last_sn);
    return;
  }

  // ACKNACK 必须发到**学到的对端 metatraffic locator**：回源地址
  // （可能是组播或临时端口）不会被对端处理，这个坑本项目踩过两次。
  for (const Peer& peer : peers) {
    SendAcknackToPeer(peer, writer_id, first_sn, last_sn);
  }
}

void RtpsWriter::RecordReceivedSn(uint32_t writer_entity, uint64_t sn) {
  constexpr uint64_t kKeepWindow = 4096;
  std::lock_guard<std::mutex> lock(mutex_);
  auto& set = received_sn_[writer_entity];
  set.insert(sn);
  if (set.size() > kKeepWindow) {
    const uint64_t oldest = sn > kKeepWindow ? (sn - kKeepWindow) : 0;
    set.erase(set.begin(), set.lower_bound(oldest));
  }
}

void RtpsWriter::SendAcknackToPeer(const Peer& peer, uint32_t writer_entity,
                                   int64_t first_sn, int64_t last_sn) {
  CDRMessage_t ack(512);
  ack.msg_endian = LITTLEEND;
  pkt::WriteHeaderAndDst(&ack, my_prefix_, peer.guid_prefix);

  EntityId_t wid(writer_entity);
  // 与 Reader 侧同构：由对端 writer 推出我方对应的 reader 身份。
  EntityId_t rid(writer_entity == pkt::kEntitySedpSubWriter
                     ? pkt::kEntitySedpSubReader
                     : pkt::kEntitySedpPubReader);

  // 【按真实收到的 SN 报告缺口】（与 rtps_reader.cc 的 SendAcknack 同构）
  //
  // ACKNACK 语义：base_sn 之前都已收到；位图标记 >= base_sn 的缺失 SN。
  // 旧实现把整个 [firstSN,lastSN] 标成缺失，等于宣称"一条都没收到"，
  // 对端于是反复重传全部历史公告（并持续报 Inconsistent acknack received）。
  std::set<uint64_t> received;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = received_sn_.find(writer_entity);
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
  constexpr uint64_t kBitmapLimit = 256;
  uint64_t marks = 0;
  for (uint64_t sn = base; sn <= hi && marks < kBitmapLimit; ++sn) {
    if (received.count(sn) == 0) {
      sns.add(SequenceNumber_t(0, static_cast<uint32_t>(sn)));
      ++marks;
    }
  }

  if (!RTPSMessageCreator::addSubmessageAcknack(
          &ack, rid, wid, sns,
          static_cast<int32_t>(st_acknack_sent_.load(std::memory_order_relaxed)),
          false)) {
    return;
  }
  const sockaddr_in sa = ToSockaddr(peer.meta);
  if (::sendto(fd_tx_, ack.buffer, ack.length, 0,
               reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) > 0) {
    st_acknack_sent_.fetch_add(1, std::memory_order_relaxed);
  }
}

std::vector<ReceivedSample> RtpsWriter::UnackedSamples() const {
  std::vector<ReceivedSample> out;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& p : pending_) {
    if (!p.acked) {
      out.push_back(p.sample);
    }
  }
  return out;
}

void RtpsWriter::HandleAcknack(const uint8_t* packet, size_t len,
                               uint32_t body, const SubmessageHeader_t& smh) {
  // ACKNACK 体：readerId(4) writerId(4) base_sn(8) numBits(4) bitmap(...)
  if (body + 24 > len) return;
  st_acks_.fetch_add(1, std::memory_order_relaxed);
  (void)smh;

  // 【按 SN 精确记账】
  //
  // ACKNACK 的语义是"我缺哪些"：
  //   · 所有 < base_sn 的 SN           → reader 已收到
  //   · base_sn + i 的位图位为 1        → 该 SN 缺失（需要重传）
  //   · 位图之外（>= base_sn + numBits） → 也不可认为已收到
  //
  // 旧实现是"收到任意一个 ACKNACK 就把所有 pending 标记 acked"，
  // 于是：① 重传统计严重偏大（实测 8 秒 retrans 涨到 920）；
  //      ② 收尾补发必须无视 acked 标志才能生效；
  //      ③ wait_for_ack 的语义被架空（永远"全部已确认"）。
  uint32_t base_low = 0;
  std::memcpy(&base_low, packet + body + 12, 4);   // base_sn 的低 32 位（小端）
  uint32_t num_bits = 0;
  std::memcpy(&num_bits, packet + body + 20, 4);

  // 位图按大端位序：第 i 位（从 0 起）对应 base_sn + i，取 byte i/8 的 bit (7 - i%8)。
  auto is_missing = [&](uint32_t low) -> bool {
    if (low < base_low) {
      g_ack_base_acked.fetch_add(1, std::memory_order_relaxed);   // ① 路径
      return false;                                  // reader 已确认收到
    }
    const uint32_t idx = low - base_low;
    if (idx >= num_bits) {
      g_ack_beyond_bitmap.fetch_add(1, std::memory_order_relaxed); // ③ 路径
      return true;                                   // 位图未覆盖 → 视为缺失
    }
    const size_t off = static_cast<size_t>(body) + 24 + (idx / 8);
    if (off >= len) {
      g_ack_beyond_bitmap.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    // 走到这里说明**真的解析了位图的某一位** —— 这才是 O10 要的证据。
    g_ack_bitmap_checked.fetch_add(1, std::memory_order_relaxed);
    const bool miss = (packet[off] & (1u << (7 - (idx % 8)))) != 0;
    if (miss) {
      g_ack_bitmap_missing.fetch_add(1, std::memory_order_relaxed); // ② 路径：缺失
    }
    return miss;
  };

  std::vector<PendingSample> resend;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : pending_) {
      const uint32_t low = static_cast<uint32_t>(p.sn & 0xFFFFFFFFu);
      if (is_missing(low)) {
        p.acked = false;
        resend.push_back(p);                         // 只重传真正缺失的
      } else {
        p.acked = true;
      }
    }
  }
  if (resend.empty()) return;

  std::vector<Peer> peers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peers = peers_;
  }
  for (const Peer& peer : peers) {
    const PeerAddress& dst = peer.has_user ? peer.user : peer.meta;
    const sockaddr_in sa = ToSockaddr(dst);
    for (const auto& item : resend) {
      ThrottleOrWait(item.packet.size());   // 重传同样受限速约束
      ::sendto(fd_tx_, item.packet.data(), item.packet.size(), 0,
               reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
      st_retrans_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void RtpsWriter::SendHeartbeats() {
  // 【必须覆盖每一个 writer】
  //
  // 多话题并发时每个话题有独立的 writer entityId，而可靠 reader 要等到
  // 属于**自己那个 writer** 的 HEARTBEAT 才会把样本标记为 readable 并交付。
  // 只给固定的 my_writer_entity_ 发心跳 ⇒ 第二个话题的数据永远不交付。
  std::vector<Peer> peers;
  std::map<uint32_t, uint64_t> writers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peers = peers_;
    writers = writer_sn_;
  }
  if (peers.empty() || writers.empty()) return;

  for (const auto& kv : writers) {
    const uint32_t writer_entity = kv.first;
    const uint64_t max_sn = kv.second;
    if (max_sn == 0) continue;

    for (const Peer& peer : peers) {
      const auto hb = pkt::BuildHeartbeat(my_prefix_, peer.guid_prefix,
                                          writer_entity,
                                          0x00000000u /* unknown reader */,
                                          1, static_cast<int64_t>(max_sn));
      // 心跳应发到对端的 **user locator**（学到之后）：用户数据的 DataReader
      // 监听在 user 端口，而 metatraffic 端口上只有 SEDP reader。RTPS 可靠
      // reader 的样本要等 HEARTBEAT 到达才会变为 readable 并交付给应用，
      // 因此心跳发错端口会让"数据已送达"表现为"应用一条也看不到"。
      const PeerAddress& dst = peer.has_user ? peer.user : peer.meta;
      const sockaddr_in sa = ToSockaddr(dst);
      if (std::getenv("STRATUM_DUMP") != nullptr) {
        char dip[32] = {0};
        ::inet_ntop(AF_INET, &sa.sin_addr, dip, sizeof(dip));
        std::fprintf(stderr, "[hb] -> %s:%u writer=0x%08x first=1 last=%llu\n",
                     dip, static_cast<unsigned>(ntohs(sa.sin_port)),
                     writer_entity,
                     static_cast<unsigned long long>(max_sn));
        std::fflush(stderr);
      }
      ::sendto(fd_tx_, hb.data(), hb.size(), 0,
               reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
      st_heartbeats_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

// ---------------------------------------------------------------------------
// 构造与发布
// ---------------------------------------------------------------------------

std::vector<uint8_t> RtpsWriter::BuildPublication(const std::string& topic,
                                                  const std::string& type) {
  CDRMessage_t msg(1024);
  msg.msg_endian = LITTLEEND;
  pkt::WriteHeaderAndDst(&msg, my_prefix_, nullptr);

  // INFO_TS
  CDRMessage::addOctet(&msg, pkt::kSubInfoTs);
  CDRMessage::addOctet(&msg, 0x01);
  CDRMessage::addUInt16(&msg, 8);
  CDRMessage::addInt32(&msg, 0);
  CDRMessage::addUInt32(&msg, 0);

  // DATA（writerId = SEDP publications writer）
  CDRMessage::addOctet(&msg, pkt::kSubData);
  CDRMessage::addOctet(&msg, 0x05);
  const uint32_t size_pos = msg.pos;
  CDRMessage::addUInt16(&msg, 0);
  const uint32_t body_start = msg.pos;
  CDRMessage::addUInt16(&msg, 0);    // extraFlags
  CDRMessage::addUInt16(&msg, 16);   // octetsToInlineQos
  EntityId_t unknown(ENTITYID_UNKNOWN);
  CDRMessage::addEntityId(&msg, &unknown);
  EntityId_t pub_writer(pkt::kEntitySedpPubWriter);
  CDRMessage::addEntityId(&msg, &pub_writer);
  // SEDP Publication 用**独立的** SN 计数器：与用户数据共用会让用户数据的
  // SN 跳号，对端因此永远认为有缺口而不交付样本（详见 rtps_writer.h）。
  const uint64_t sn = ++sedp_sn_;
  SequenceNumber_t seq(static_cast<int32_t>(sn >> 32),
                       static_cast<uint32_t>(sn & 0xFFFFFFFF));
  CDRMessage::addSequenceNumber(&msg, &seq);

  // encapsulation
  {
    bool ok = CDRMessage::addOctet(&msg, 0);
    ok &= CDRMessage::addOctet(&msg, static_cast<octet>(PL_CDR_LE - msg.msg_endian));
    ok &= CDRMessage::addUInt16(&msg, 0);
    if (!ok) return {};
  }

  GUID_t writer_guid;
  std::memcpy(writer_guid.guidPrefix.value, my_prefix_, 12);
  writer_guid.entityId = EntityId_t(WriterEntityFor(topic, type));
  GUID_t part_guid;
  std::memcpy(part_guid.guidPrefix.value, my_prefix_, 12);
  part_guid.entityId = EntityId_t(ENTITYID_RTPSParticipant);

  const uint16_t user_port =
      config_.user_port != 0 ? config_.user_port
                             : static_cast<uint16_t>(9061 + config_.domain_id * 10);

  {
    dds::ParameterGuid_t p(dds::PID_ENDPOINT_GUID, 16, writer_guid);
    if (!dds::ParameterSerializer<dds::ParameterGuid_t>::add_to_cdr_message(p, &msg)) return {};
  }
  {
    dds::ParameterGuid_t p(dds::PID_PARTICIPANT_GUID, 16, part_guid);
    if (!dds::ParameterSerializer<dds::ParameterGuid_t>::add_to_cdr_message(p, &msg)) return {};
  }
  {
    dds::ParameterString_t p(dds::PID_TOPIC_NAME, 0, topic);
    if (!dds::ParameterSerializer<dds::ParameterString_t>::add_to_cdr_message(p, &msg)) return {};
  }
  {
    dds::ParameterString_t p(dds::PID_TYPE_NAME, 0, type);
    if (!dds::ParameterSerializer<dds::ParameterString_t>::add_to_cdr_message(p, &msg)) return {};
  }
  {
    dds::ParameterLocator_t p(dds::PID_UNICAST_LOCATOR, 24);
    const std::string adv2 = config_.advertised_address.empty()
                                 ? pkt::DetectLocalAddress()
                                 : config_.advertised_address;
    p.locator = MakeDefaultUdpv4Locator(adv2.c_str(), user_port);
    if (!dds::ParameterSerializer<dds::ParameterLocator_t>::add_to_cdr_message(p, &msg)) return {};
  }
  // --- QoS 参数 ---
  //
  // 【为什么必须有】Fast DDS 的 DataReader 默认请求 RELIABLE。
  // SEDP Publication 若不声明 RELIABILITY，对端会按默认推断，
  // 很可能得出"该 Writer 是 BEST_EFFORT"的结论 —— 于是 QoS 判定为
  // 不兼容，**Reader 根本不会接收数据**。
  // 实测：不声明 QoS 时，回放了 110 条数据，Fast DDS subscriber 收到 0 条。
  //
  // ParameterTypes.hpp 中没有 Reliability/Durability 的 Parameter 类
  // （Fast DDS 内部另有序列化路径），故此处按线上格式直接写
  // PID + length + value：
  //   PID_RELIABILITY(0x001a)：kind(uint32) + max_blocking_time(sec int32 + frac uint32)
  //   PID_DURABILITY (0x001d)：kind(uint32)
  {
    CDRMessage::addUInt16(&msg, static_cast<uint16_t>(dds::PID_RELIABILITY));
    CDRMessage::addUInt16(&msg, 12);
    CDRMessage::addUInt32(&msg, 2);          // 2 = RELIABLE_RELIABILITY_QOS
    CDRMessage::addInt32(&msg, 0);           // max_blocking_time.seconds
    CDRMessage::addUInt32(&msg, 100000000);  // max_blocking_time.fraction（0.1s）
  }
  {
    CDRMessage::addUInt16(&msg, static_cast<uint16_t>(dds::PID_DURABILITY));
    CDRMessage::addUInt16(&msg, 4);
    CDRMessage::addUInt32(&msg, 0);          // 0 = VOLATILE_DURABILITY_QOS
  }
  {
    // 数据表示：PL_CDR（XCDR1），与 hello_world 的默认一致
    CDRMessage::addUInt16(&msg, static_cast<uint16_t>(dds::PID_DATA_REPRESENTATION));
    CDRMessage::addUInt16(&msg, 4);
    CDRMessage::addUInt32(&msg, 0);
  }
  {
    // 长度 4：实测真实 Fast DDS 取值（规范字面是 2）
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

  pkt::BackfillSubmessageLength(&msg, size_pos, body_start);
  return std::vector<uint8_t>(msg.buffer, msg.buffer + msg.length);
}

uint32_t RtpsWriter::WriterEntityFor(const std::string& topic,
                                     const std::string& type) {
  const std::string key = topic + "|" + type;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = topic_writers_.find(key);
  if (it != topic_writers_.end()) {
    return it->second;
  }
  // entityKey 从 1 起（key=1 即 0x00000103，与原固定值一致 → 单话题行为不变）
  const uint32_t entity = (next_entity_key_ << 8) | 0x03u;
  ++next_entity_key_;
  topic_writers_[key] = entity;
  return entity;
}

uint64_t RtpsWriter::NextSnFor(uint32_t writer_entity) {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t& sn = writer_sn_[writer_entity];
  ++sn;
  return sn;
}

std::vector<uint8_t> RtpsWriter::BuildDataPacket(uint64_t sn,
                                                 const std::string& payload,
                                                 TimestampNs source_ts_ns,
                                                 uint32_t writer_entity) {
  // 【委托给 packet::BuildDataPacket —— 不要在这里重复实现】
  //
  // 这里曾有一份 CDRMessage_t(2048) 的固定缓冲副本：负载超过约 2 KiB 时
  // addData 返回 false 被忽略，于是发出一份"只有头部、没有负载"的 DATA
  // —— sendto 成功、回放端显示"已发布"、**订阅端一条都收不到**
  // （实测 8 KiB 负载时报文仅 56 字节，且与回放速率无关）。
  // packet::BuildDataPacket 已经按负载大小动态分配（其注释记录了同一个坑，
  // 并由 RtpsPacket.DegenerateInputs 用 8 KiB 负载守护），此处必须复用，
  // 避免同一份逻辑再次分叉。
  pkt::DataPacketParams params;
  std::memcpy(params.prefix, my_prefix_, 12);
  params.writer_entity = writer_entity;
  params.sn = sn;
  params.payload = payload;
  params.source_ts_ns = source_ts_ns;   // 写入 INFO_TS（延迟测量与数据可信度都依赖它）
  return pkt::BuildDataPacket(params);
}

bool RtpsWriter::ShouldDropForTest() {
  const uint32_t pct = config_.debug_drop_send_percent;
  if (pct == 0) {
    return false;   // 生产路径：零开销（只比较一次整数）
  }
  // 每个线程独立引擎即可：这里只需统计意义上的丢包率。
  static thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<uint32_t> dist(1, 100);
  return dist(rng) <= pct;
}

void RtpsWriter::ThrottleOrWait(size_t bytes) {
  if (config_.max_samples_per_sec == 0 && config_.max_bytes_per_sec == 0) {
    return;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(throttle_mutex_);
      // 1 秒滑动窗口
      while (!send_log_.empty() &&
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 now - send_log_.front().first).count() >= 1000) {
        send_log_.pop_front();
      }
      size_t bytes_in_window = 0;
      for (const auto& e : send_log_) {
        bytes_in_window += e.second;
      }
      const bool ok_bytes =
          (config_.max_bytes_per_sec == 0) ||
          (bytes_in_window + bytes <= config_.max_bytes_per_sec);
      const bool ok_samples =
          (config_.max_samples_per_sec == 0) ||
          (send_log_.size() + 1 <= config_.max_samples_per_sec);
      if (ok_bytes && ok_samples) {
        send_log_.push_back({now, bytes});
        return;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      st_throttle_timeouts_.fetch_add(1, std::memory_order_relaxed);
      return;   // 不无限等待：宁可多发一点，也不能卡死
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

void RtpsWriter::WaitForSendWindow() {
  if (config_.send_window == 0) {
    return;   // 关闭流控（吞吐压测用）
  }
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(config_.send_window_timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    size_t unacked = 0;
    size_t unacked_bytes = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& p : pending_) {
        if (!p.acked) {
          ++unacked;
          unacked_bytes += p.packet.size();
        }
      }
    }
    // 条数与字节双限：大 payload 时必须看字节（见 send_window_bytes 注释）。
    if (unacked < config_.send_window &&
        unacked_bytes < config_.send_window_bytes) {
      return;
    }
    if (unacked_bytes >= config_.send_window_bytes) {
      st_window_bytes_waits_.fetch_add(1, std::memory_order_relaxed);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // 超时：继续发送。宁可丢一点，也不能因为对端不回 ACK 就永久卡住。
  st_window_timeouts_.fetch_add(1, std::memory_order_relaxed);
}

Status RtpsWriter::Publish(const std::string& topic_name,
                           const std::string& type_name,
                           const std::string& payload,
                           TimestampNs source_ts_ns) {
  if (fd_tx_ < 0) return Status::InvalidArgument("RtpsWriter 尚未 Open");
  if (topic_name.empty()) return Status::InvalidArgument("话题名为空");

  // 【调试用丢包注入】在构造报文之前就决定是否"丢掉"这一条。
  // 返回 Ok 是刻意的：真实网络里丢包对发送方也是"发出去就不知道了"，
  // 这样上层统计（已发布样本）会照常计数，而接收端收不到 ——
  // 正是我们要模拟的场景。生产环境该值为 0，只比较一次整数。
  if (ShouldDropForTest()) {
    st_debug_dropped_.fetch_add(1, std::memory_order_relaxed);
    return Status::OK();
  }

  // 速率限制（见 RtpsWriterConfig::max_samples_per_sec 的说明）。
  // 用 payload 估算报文大小（+32 覆盖 RTPS 头与子消息头）。
  ThrottleOrWait(payload.size() + 32);

  const std::string key = topic_name + "|" + type_name;

  // 首次见到该话题：先发布 SEDP Publication 让对端建立 Writer 代理
  bool need_announce = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (announced_.find(key) == announced_.end()) {
      announced_[key] = std::make_pair(topic_name, type_name);
      need_announce = true;
    }
  }
  if (need_announce) {
    const auto pub = BuildPublication(topic_name, type_name);
    if (pub.empty()) {
      // 【不再静默失败】BuildPublication 只有在参数不合法时返回空，
      // 最常见的就是 type_name 为空（PID_TYPE_NAME 不接受空字符串）。
      // 上游若把空 type_name 传进来，旧行为是"跳过公告但继续发数据"，
      // 结果是：对端永远发现不了本 Writer → 不建立匹配 → 用户数据
      // 全被发到 metatraffic 端口 → 对端收 0 条，且两端都没有任何报错。
      // 这里改为显式报错 + 计数，让同类问题在第一次调用时就暴露。
      last_error_ = "SEDP Publication 构造失败（topic=\"" + topic_name +
                    "\" type=\"" + type_name + "\"）";
      st_pub_build_failed_.fetch_add(1, std::memory_order_relaxed);
      std::fprintf(stderr, "[writer] %s\n", last_error_.c_str());
      std::fflush(stderr);
      return Status::InvalidArgument(last_error_);
    }
    {
      std::vector<Peer> peers;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        peers = peers_;
      }
      for (const Peer& peer : peers) {
        const sockaddr_in sa = ToSockaddr(peer.meta);
        ::sendto(fd_tx_, pub.data(), pub.size(), 0,
                 reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
      }
      st_pubs_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  const uint32_t writer_entity = WriterEntityFor(topic_name, type_name);
  const uint64_t sn = NextSnFor(writer_entity);
  auto packet = BuildDataPacket(sn, payload, source_ts_ns, writer_entity);

  // 【跨主机 MTU 保护】超限即显式失败，不静默丢给网络层（见配置项注释）。
  if (config_.max_udp_payload > 0 &&
      packet.size() > static_cast<size_t>(config_.max_udp_payload)) {
    return Status::InvalidArgument(
        "样本超过配置的 UDP 载荷上限（报文 " + std::to_string(packet.size()) +
        " 字节 > 上限 " + std::to_string(config_.max_udp_payload) +
        " 字节）：MTU 1500 的路径不分片上限为 1472，超出会被路径丢弃、"
        "对端收不到。需要更大样本请实现 RTPS DATA_FRAG；"
        "单机回环可将 STRATUM_MAX_UDP_PAYLOAD 设为 0 关闭本检查。");
  }
  if (packet.empty()) {
    // 报文构造失败（例如样本超过 16 位长度上限）：必须让调用方知道，
    // 而不是"计数为已发布、实际什么都没发"。
    st_pub_build_failed_.fetch_add(1, std::memory_order_relaxed);
    return Status::InvalidArgument(
        "报文构造失败：样本可能超过 RTPS 单子消息上限（需要 DATA_FRAG 分片）");
  }
  if (packet.empty()) return Status::Internal("构造 DATA 报文失败");

  std::vector<Peer> peers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peers = peers_;
  }
  size_t sent = 0;
  for (const Peer& peer : peers) {
    // 数据发到对端声明的 user locator；尚未学到时退回 metatraffic
    const PeerAddress& dst = peer.has_user ? peer.user : peer.meta;
    const sockaddr_in sa = ToSockaddr(dst);
    if (std::getenv("STRATUM_DUMP") != nullptr) {
      char dip[32] = {0};
      ::inet_ntop(AF_INET, &sa.sin_addr, dip, sizeof(dip));
      std::fputs("[send] has_user=", stderr);
      std::fprintf(stderr, "%d", peer.has_user ? 1 : 0);
      std::fputs(" -> ", stderr);
      std::fputs(dip, stderr);
      std::fputs(":", stderr);
      std::fprintf(stderr, "%u", static_cast<unsigned>(ntohs(sa.sin_port)));
      std::fputs(" bytes=", stderr);
      std::fprintf(stderr, "%zu", packet.size());
      std::putc(10, stderr);
      std::fflush(stderr);
    }
    const ssize_t n_sent =
        ::sendto(fd_tx_, packet.data(), packet.size(), 0,
                 reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
    if (n_sent > 0) {
      ++sent;
    } else {
      // 【不能静默】大 payload 时内核可能直接拒绝（EMSGSIZE/ENOBUFS），
      // 静默忽略会表现为"回放端显示已发布、订阅端一条都收不到"。
      st_send_errors_.fetch_add(1, std::memory_order_relaxed);
      if (std::getenv("STRATUM_DUMP") != nullptr) {
        std::fprintf(stderr, "[send] FAILED bytes=%zu errno=%d (%s)\n",
                     packet.size(), errno, std::strerror(errno));
        std::fflush(stderr);
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    PendingSample ps;
    ps.sn = sn;
    ps.packet = packet;
    // 就地构造样本副本：重发时只需 topic/type/payload
    // （时间戳与 sequence 由上层 RtpsSink 传入，writer 层不掌握）。
    ps.sample.topic_name = topic_name;
    ps.sample.type_name = type_name;
    ps.sample.payload = payload;
    ps.acked = false;
    pending_.push_back(std::move(ps));
    // 限制缓存规模（回放长序列时不无限增长）
    if (pending_.size() > 4096) {
      pending_.erase(pending_.begin(), pending_.begin() + 1024);
    }
  }
  st_data_sent_.fetch_add(1, std::memory_order_relaxed);

  if (sent == 0) {
    // 尚无已匹配的接收方：数据已进入 pending 缓存，
    // 待 HandleSedpSubscription 学到对端 user locator 后由
    // FlushPendingToPeers() 补发 —— 而不是直接丢弃。
    return Status::NotFound("尚无已发现的对端 Reader（数据已缓存待补发）");
  }
  return Status::OK();
}

Status RtpsWriter::WaitForAcknowledgments(uint64_t timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      bool all_acked = true;
      for (const auto& p : pending_) {
        if (!p.acked) { all_acked = false; break; }
      }
      if (all_acked && !pending_.empty()) return Status::OK();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // 【超时收尾：无条件重发开头一小段】
  //
  // 为什么必须做、且必须**无视 acked 标志**：
  //   · 对端 reader 在匹配刚建立的一瞬间可能丢掉最早到达的 DATA（DDS 启动
  //     竞态），而它**不会为"从未见过"的 SN 发 ACKNACK** —— 那条数据永久丢失。
  //     实测回放 200 条时订阅端稳定收到 199 条，缺的**恒为第一条**
  //     （首=2 尾=200）；真实录制回放同样（首=2 尾=149）。
  //   · `HandleAcknack` 目前是"收到一次 ACKNACK 就把所有 pending 标记 acked"
  //     的粗糙实现，所以按 acked 过滤的补发会被整段跳过（本轮实测：加了
  //     FlushPendingToPeers 仍然缺首条）。
  // 折中：收尾时对开头若干条做一次幂等重发（对端按 SN 去重），
  // 既覆盖启动窗口，又不至于把整个回放再灌一遍。
  {
    constexpr size_t kResendHead = 16;
    std::vector<Peer> peers;
    std::vector<PendingSample> head;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      peers = peers_;
      const size_t k = std::min(pending_.size(), kResendHead);
      head.assign(pending_.begin(), pending_.begin() + static_cast<long>(k));
    }
    for (const Peer& peer : peers) {
      if (!peer.has_user) continue;
      const sockaddr_in sa = ToSockaddr(peer.user);
      for (const auto& item : head) {
        ThrottleOrWait(item.packet.size());   // 重传同样受限速约束
      ::sendto(fd_tx_, item.packet.data(), item.packet.size(), 0,
                 reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
        st_retrans_.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  return Status::Timeout("等待确认超时（已重发开头样本）");
}

RtpsWriter::Stats RtpsWriter::GetStats() const {
  Stats s;
  s.packets_total = st_packets_.load(std::memory_order_relaxed);
  s.spdp_received = st_spdp_.load(std::memory_order_relaxed);
  s.sedp_subscriptions = st_sedp_sub_.load(std::memory_order_relaxed);
  s.remote_readers = st_readers_.load(std::memory_order_relaxed);
  s.data_sent = st_data_sent_.load(std::memory_order_relaxed);
  s.heartbeats_sent = st_heartbeats_.load(std::memory_order_relaxed);
  s.acknacks_received = st_acks_.load(std::memory_order_relaxed);
  s.retransmits = st_retrans_.load(std::memory_order_relaxed);
  s.publications_announced = st_pubs_.load(std::memory_order_relaxed);
  s.publications_build_failed = st_pub_build_failed_.load(std::memory_order_relaxed);
  s.heartbeats_received = st_hb_recv_.load(std::memory_order_relaxed);
  s.acknacks_sent = st_acknack_sent_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    s.peers_learned = peers_.size();
  }
  return s;
}

AckPathStats AckPathStatsSnapshot() {
  AckPathStats s;
  s.base_acked = g_ack_base_acked.load(std::memory_order_relaxed);
  s.bitmap_missing = g_ack_bitmap_missing.load(std::memory_order_relaxed);
  s.bitmap_checked = g_ack_bitmap_checked.load(std::memory_order_relaxed);
  s.beyond_bitmap = g_ack_beyond_bitmap.load(std::memory_order_relaxed);
  return s;
}

}  // namespace rtps
}  // namespace protocol
}  // namespace stratum
