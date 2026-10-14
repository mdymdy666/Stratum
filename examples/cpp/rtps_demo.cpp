// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// examples/cpp/rtps_demo.cpp
//
// ─────────────────────────────────────────────────────────────────────────────
// rtps 模块 · 简单用法演示
//
// rtps 是 Stratum 自带的 RTPS 实现（SPDP/SEDP/DATA/HEARTBEAT/ACKNACK/DATA_FRAG）。
// 从使用者的角度它只有两件事要做：
//
//   ① 配置  —— RtpsWriterConfig / RtpsReaderConfig
//   ② 报文  —— packet::* 纯函数（构造与解析），可脱离网络做字节级验证
//
// 本示例演示：
//   · 发送侧配置：participant_name / advertised_address / initial_peers /
//     max_udp_payload / auto_mtu_detect / 发送窗口 / 限速 / 心跳间隔
//   · 接收侧配置：topic_filter / requested_qos / initial_peers
//   · packet::BuildDataPacket     构造一条 DATA（含 INFO_TS）
//   · packet::BuildDataFragPackets 把大样本切片
//   · 关键常量：子消息类型、EntityId、PID
//
// 注意：本示例**不发起网络通信**，只做配置与字节级构造，
// 因此任何机器上都能直接跑。真实收发的用法见 rtps_full.cpp。
//
// 运行：./rtps_demo
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>
#include <string>
#include <vector>

#include "stratum/protocol/rtps/rtps_packet_utils.h"
#include "stratum/protocol/rtps/rtps_reader.h"
#include "stratum/protocol/rtps/rtps_writer.h"

using namespace stratum;
using namespace stratum::protocol::rtps;
namespace packet = stratum::protocol::rtps::packet;

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "OK" : "FAIL", what);
  if (!ok) ++g_failed;
}

// 把 GUID prefix 打成可读形式
std::string PrefixStr(const uint8_t p[12]) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x.%02x%02x%02x%02x.%02x%02x%02x%02x",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
  return buf;
}

}  // namespace

int main() {
  std::printf("=== rtps_demo：配置与报文构造 ===\n\n");

  // ═══════════════════════════════════════════════════════════════════════
  // ① 发送侧配置（RtpsWriterConfig）
  //
  // 每个字段都对应一个真实需求，注释里写明"不设它会怎样"。
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("[1] RtpsWriterConfig\n");
  {
    RtpsWriterConfig w;
    Check(w.domain_id == 0, "domain_id 默认 0");
    Check(w.auto_mtu_detect, "auto_mtu_detect 默认开启");
    Check(w.max_udp_payload == 0, "max_udp_payload 默认 0（由 MTU 探测决定）");

    // —— 身份与可达性 ——
    w.domain_id = 70;
    w.participant_name = "rtps_demo_writer";
    // advertised_address：告诉对端"往哪个地址回我"。
    // 【不设会怎样】构造公告时回退到 DetectLocalAddress()，在 NAT/中继拓扑下
    // 该地址对端不可达 —— 症状是"发现成功、SEDP 正常、Writer 已匹配，
    // 但一条数据都收不到"，且没有任何报错。
    w.advertised_address = "127.0.0.1";
    // initial_peers：对端地址。设了它就不依赖组播，也用于 MTU 探测取对端。
    w.initial_peers = {"127.0.0.1"};

    // —— 分片与流控 ——
    // max_udp_payload = 0 且 auto_mtu_detect = true：按对端探测路径 MTU，
    // 超限样本自动走 DATA_FRAG。单机 loopback 探测到 65535，因此不会分片；
    // 跨主机探测到 1500，自动按 1472 切片。
    w.max_udp_payload = 0;
    w.auto_mtu_detect = true;
    // 发送窗口：未确认条数上限。0 = 关闭（不流控）。
    // 打开后靠 SendWindow 限制在途数据，避免对端处理不过来。
    w.send_window = 0;
    w.send_window_bytes = 65536;
    w.send_window_timeout_ms = 1000;   // 到点仍未放开就继续发，避免永久阻塞

    // —— 心跳 ——
    w.announce_interval_ms = 200;      // SPDP/SEDP 公告间隔
    w.heartbeat_interval_ms = 100;     // HEARTBEAT 间隔（驱动对端 ACKNACK）

    // —— 测试注入（生产环境保持默认 0）——
    w.debug_drop_send_percent = 0;     // 故意丢发送的百分比，用于验证重传
    w.max_samples_per_sec = 0;         // 限速（0 = 不限）
    w.max_bytes_per_sec = 0;

    Check(w.domain_id == 70 && !w.advertised_address.empty(),
          "发送侧配置可设置");
    std::printf("      participant=%s advertised=%s peers=%zu\n",
                w.participant_name.c_str(), w.advertised_address.c_str(),
                w.initial_peers.size());
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ② 接收侧配置（RtpsReaderConfig）
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[2] RtpsReaderConfig\n");
  {
    RtpsReaderConfig r;
    r.domain_id = 70;
    r.participant_name = "rtps_demo_reader";
    r.advertised_address = "127.0.0.1";
    r.initial_peers = {"127.0.0.1"};
    // topic_filter：只订阅关心的话题。空 = 全部（自动发现模式）。
    // 【为什么要能过滤】现场话题可能上百个，全订阅会把带宽和磁盘吃光。
    r.topic_filter = {"demo/*"};
    r.announce_interval_ms = 200;

    Check(r.topic_filter.size() == 1, "接收侧可设话题过滤");
    std::printf("      filter=%s（空 = 订阅全部）\n", r.topic_filter.begin()->c_str());
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ③ packet::BuildDataPacket —— 构造一条 DATA
  //
  // 一个 RTPS 报文的骨架：
  //   [RTPS 头 20B] [INFO_TS 12B] [DATA ...]
  // RTPS 头固定 20 字节：'RTPS'(4) + version(2) + vendorId(2) + guidPrefix(12)
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[3] packet::BuildDataPacket\n");
  {
    packet::DataPacketParams p;
    // guidPrefix：12 字节的参与者标识。空 = 由实现自动生成。
    const uint8_t prefix[12] = {0x01, 0x0f, 'S', 'T', 'R', 'A',
                                'T', 'U', 'M', 0, 0, 1};
    for (int i = 0; i < 12; ++i) p.prefix[i] = prefix[i];
    p.writer_entity = packet::kEntityUserWriter;   // 用户数据的 writer 实体
    p.sn = 42;                                     // 序号
    p.payload = std::string("\x00\x01\x00\x00hello", 9);  // 已序列化负载
    // source_ts_ns：源时间戳，会写进 INFO_TS。
    // 【不设会怎样】写死 0 会让接收端无法计算端到端延迟，
    // 录制下来的 source 全是无效值 —— 事后无法判断"当时数据是什么时候产生的"。
    p.source_ts_ns = 1700000000000000000ULL;

    const std::vector<uint8_t> raw = packet::BuildDataPacket(p);
    Check(raw.size() > 20, "报文长度大于 RTPS 头");
    Check(raw.size() > 32, "报文包含头 + INFO_TS + DATA");

    // 字节级检查：RTPS 头的魔数
    const bool magic_ok = raw.size() >= 4 && raw[0] == 'R' && raw[1] == 'T' &&
                          raw[2] == 'P' && raw[3] == 'S';
    Check(magic_ok, "报文以 'RTPS' 开头");

    // 版本号：2.4
    Check(raw.size() >= 6 && raw[4] == 2 && raw[5] == 4, "版本为 2.4");

    // guidPrefix 与传入一致
    bool prefix_ok = raw.size() >= 20;
    for (int i = 0; i < 12 && prefix_ok; ++i) {
      if (raw[8 + i] != prefix[i]) prefix_ok = false;
    }
    Check(prefix_ok, "guidPrefix 原样写入头部");

    std::printf("      报文 %zu 字节，prefix=%s sn=%llu\n", raw.size(),
                PrefixStr(prefix).c_str(),
                static_cast<unsigned long long>(p.sn));
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ④ packet::BuildDataFragPackets —— 大样本切片
  //
  // 为什么需要它：MTU 1500 的路径上，不分片的 UDP 上限是 1472 字节，
  // 超过就要依赖 **IP 分片**，而 IP 分片在 NAT / 中继路径上会被丢弃 ——
  // 症状是"发送端显示已发布、对端一条都收不到"，且没有任何报错。
  // DATA_FRAG 把切分放到 RTPS 层，每个报文都不超过 MTU。
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[4] packet::BuildDataFragPackets（分片）\n");
  {
    packet::DataPacketParams p;
    const uint8_t prefix[12] = {0x01, 0x0f, 'S', 'T', 'R', 'A',
                                'T', 'U', 'M', 0, 0, 2};
    for (int i = 0; i < 12; ++i) p.prefix[i] = prefix[i];
    p.writer_entity = packet::kEntityUserWriter;
    p.sn = 7;
    p.payload.assign(6000, 'A');          // 6 KB，远超 1472
    p.source_ts_ns = 1700000000000000000ULL;

    const uint16_t frag_size = 1400;      // 每片负载上限
    const auto frags = packet::BuildDataFragPackets(p, frag_size);

    const size_t expected = (6000 + frag_size - 1) / frag_size;
    std::printf("      payload=6000B frag_size=%u -> %zu 片（期望 %zu 片）\n",
                frag_size, frags.size(), expected);
    Check(frags.size() == expected, "分片数符合预期（向上取整）");

    bool all_have_magic = true;
    bool all_within_limit = true;
    size_t total = 0;
    for (const auto& f : frags) {
      if (f.size() < 4 || f[0] != 'R' || f[1] != 'T' || f[2] != 'P' || f[3] != 'S') {
        all_have_magic = false;
      }
      // 固定开销（RTPS 头 + INFO_TS + DATA_FRAG 固定部分）之外才是负载
      if (f.size() > frag_size + 64) all_within_limit = false;
      total += f.size();
    }
    Check(all_have_magic, "每一片都是完整 RTPS 报文（含头部）");
    Check(all_within_limit, "每一片都在 MTU 量级以内（可穿过 NAT/中继）");
    std::printf("      分片总字节 %zu（含各片固定开销）\n", total);
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ⑤ 关键常量：这些数字就是 RTPS 的"字典"
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[5] 关键常量\n");
  {
    Check(packet::kSubData == 0x15, "kSubData = 0x15");
    Check(packet::kSubDataFrag == 0x16, "kSubDataFrag = 0x16");
    Check(packet::kSubInfoTs == 0x09, "kSubInfoTs = 0x09（携带源时间戳）");
    Check(packet::kSubHeartbeat == 0x07, "kSubHeartbeat = 0x07");
    Check(packet::kSubAcknack == 0x06, "kSubAcknack = 0x06");
    Check(packet::kSubGap == 0x08, "kSubGap = 0x08");

    // EntityId 决定"这条报文属于哪个内建端点"
    Check(packet::kEntitySpdpWriter == 0x000100c2u, "SPDP writer = 0x000100c2");
    Check(packet::kEntitySedpPubWriter == 0x000003c2u,
          "SEDP publications writer = 0x000003c2");
    Check(packet::kEntitySedpSubWriter == 0x000004c2u,
          "SEDP subscriptions writer = 0x000004c2");
    Check(packet::kEntityUserWriter == 0x00000103u, "用户数据 writer = 0x00000103");

    // QoS 取值
    Check(packet::kReliabilityBestEffort == 1 && packet::kReliabilityReliable == 2,
          "可靠性：BEST_EFFORT=1 / RELIABLE=2");
    Check(packet::kDurabilityVolatile == 0 && packet::kDurabilityTransientLocal == 1,
          "持久性：VOLATILE=0 / TRANSIENT_LOCAL=1");

    // 参数 ID
    Check(packet::kPidTopicName == 0x0005 && packet::kPidTypeName == 0x0007,
          "PID：topicName=0x0005 / typeName=0x0007");
    Check(packet::kPidReliability == 0x001a && packet::kPidDurability == 0x001d,
          "PID：reliability=0x001a / durability=0x001d");

    std::printf("      端口公式：SPDP=7400+d*250  metatraffic=9050+d*10  user=9051+d*10\n");
  }

  std::printf("\n");
  if (g_failed == 0) {
    std::printf("全部通过。下一步看 rtps_full.cpp —— 三个真实场景。\n");
    return 0;
  }
  std::printf("有 %d 项未通过\n", g_failed);
  return 1;
}
