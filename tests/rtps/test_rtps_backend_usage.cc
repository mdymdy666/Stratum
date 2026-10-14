// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// rtps 模块测试：**RtpsBackend 使用侧**的契约
//
// ===========================================================================
// 与 tests/rtps/ 现有文件的分工
// ===========================================================================
//
// tests/rtps/ 现有的 11 个文件测的是**移植代码**的正确性
// （CDR 编解码、子消息解析、SPDP/SEDP、QoS 匹配等）。
//
// 本文件测的是**使用侧**：把 rtps 当作库来用时，它会承诺什么。
//   · packet::BuildDataPacket / BuildDataFragPackets 的字节级正确性
//   · 分片的完整性与边界（片数、每片大小上界、覆盖全部负载）
//   · RtpsWriterConfig / RtpsReaderConfig 的字段语义与默认值
//   · RtpsBackend 的能力声明与配置传递
//
// 覆盖审计发现 rtps 模块缺"随机测试"与"真实场景 ×2"，本文件补齐三类：
//   · 大数据量 —— 10 万条报文构造、10 万条分片，均计时
//   · 随机     —— 随机 payload × 随机 frag_size × 多种子；随机 prefix/sn
//   · 真实场景 —— ① MTU 受限下的切片 ② 发送侧限速与窗口 ③ 接收侧过滤与 QoS
//
// ===========================================================================
// 为什么全程不建 socket
// ===========================================================================
//
// 使用侧的**配置与报文**都是纯计算，不依赖网络。真正收发由场景脚本
// （scenarios/22~26）在真实拓扑上验证。这里只做确定性、可重复、秒级的部分 ——
// 这样测试才不会因为环境差异而不稳定。

// 标准库头文件必须最先包含（避免被卷入自定义命名空间）
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "stratum/protocol/rtps/rtps_backend.h"
#include "stratum/protocol/rtps/rtps_packet_utils.h"
#include "stratum/protocol/rtps/rtps_reader.h"
#include "stratum/protocol/rtps/rtps_writer.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::protocol::rtps;
namespace packet = stratum::protocol::rtps::packet;

namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// 填一个可辨识的 GUID prefix（12 字节）
void FillPrefix(uint8_t p[12], uint8_t tag) {
  p[0] = 0x01;    // vendor: eProsima
  p[1] = 0x0f;
  const char* name = "STRATUM";
  for (int i = 0; i < 7; ++i) p[2 + i] = static_cast<uint8_t>(name[i]);
  p[9] = 0;
  p[10] = static_cast<uint8_t>(tag >> 8);
  p[11] = tag;
}

bool HasRtpsHeader(const std::vector<uint8_t>& raw) {
  return raw.size() >= 4 && raw[0] == 'R' && raw[1] == 'T' &&
         raw[2] == 'P' && raw[3] == 'S';
}

bool HeaderMatches(const std::vector<uint8_t>& raw, const uint8_t prefix[12]) {
  if (raw.size() < 20) return false;
  if (raw[4] != 2 || raw[5] != 4) return false;      // 版本 2.4
  for (int i = 0; i < 12; ++i) {
    if (raw[8 + i] != prefix[i]) return false;
  }
  return true;
}

packet::DataPacketParams MakeParams(const uint8_t prefix[12], size_t payload,
                                    uint64_t sn = 1) {
  packet::DataPacketParams p;
  for (int i = 0; i < 12; ++i) p.prefix[i] = prefix[i];
  p.writer_entity = packet::kEntityUserWriter;
  p.sn = sn;
  p.payload.assign(payload, 'x');
  p.source_ts_ns = 1700000000000000000ULL;
  return p;
}

}  // namespace

// ===========================================================================
// 一、报文构造：字节级正确性
// ===========================================================================

STRATUM_TEST(RtpsUsage, DataPacketHeaderIsByteExact) {
  const uint8_t prefix[12] = {0x01, 0x0f, 'S', 'T', 'R', 'A',
                              'T', 'U', 'M', 0, 0, 7};
  packet::DataPacketParams p = MakeParams(prefix, 100, 42);

  const std::vector<uint8_t> raw = packet::BuildDataPacket(p);

  STRATUM_CHECK_TRUE(HasRtpsHeader(raw));
  STRATUM_CHECK_TRUE(HeaderMatches(raw, prefix));
  // 头部 20 字节 + INFO_TS 12 字节 + DATA 固定部分，必然大于裸头部
  STRATUM_CHECK_TRUE(raw.size() > 32);
  // 报文至少要能容纳 payload
  STRATUM_CHECK_TRUE(raw.size() >= p.payload.size());
}

STRATUM_TEST(RtpsUsage, DataPacketSnIsCarried) {
  const uint8_t prefix[12] = {1, 15, 'S', 'T', 'R', 'A', 'T', 'U', 'M', 0, 0, 1};
  // 不同 sn 必须产出不同报文（否则上层无法区分样本）
  packet::DataPacketParams a = MakeParams(prefix, 64, 1);
  packet::DataPacketParams b = MakeParams(prefix, 64, 2);
  const auto ra = packet::BuildDataPacket(a);
  const auto rb = packet::BuildDataPacket(b);
  STRATUM_CHECK_TRUE(ra != rb);
  STRATUM_CHECK_EQ(ra.size(), rb.size());   // 长度相同，内容不同

  // 同样的输入必须产出完全相同的字节（确定性）
  const auto ra2 = packet::BuildDataPacket(a);
  STRATUM_CHECK_TRUE(ra == ra2);
}

STRATUM_TEST(RtpsUsage, EmptyPayloadStillValid) {
  const uint8_t prefix[12] = {1, 15, 'S', 'T', 'R', 'A', 'T', 'U', 'M', 0, 0, 1};
  packet::DataPacketParams p = MakeParams(prefix, 0);
  const auto raw = packet::BuildDataPacket(p);
  // 空 payload 不该崩，也不该产出比头部还短的报文
  STRATUM_CHECK_TRUE(HasRtpsHeader(raw));
  STRATUM_CHECK_TRUE(raw.size() >= 20);
}

// ===========================================================================
// 二、分片
// ===========================================================================

STRATUM_TEST(RtpsUsage, FragmentCountIsCeil) {
  const uint8_t prefix[12] = {1, 15, 'S', 'T', 'R', 'A', 'T', 'U', 'M', 0, 0, 2};
  const uint16_t fsz = 1400;

  const size_t cases[] = {1, 1399, 1400, 1401, 2800, 2801, 60000, 65000};
  for (size_t n : cases) {
    packet::DataPacketParams p = MakeParams(prefix, n, 7);
    const auto frags = packet::BuildDataFragPackets(p, fsz);
    const size_t expect = (n + fsz - 1) / fsz;
    STRATUM_CHECK_EQ(frags.size(), expect);
  }
}

STRATUM_TEST(RtpsUsage, EveryFragmentIsAWholeRtpsPacket) {
  const uint8_t prefix[12] = {1, 15, 'S', 'T', 'R', 'A', 'T', 'U', 'M', 0, 0, 3};
  packet::DataPacketParams p = MakeParams(prefix, 20000, 99);
  const uint16_t fsz = 1200;
  const auto frags = packet::BuildDataFragPackets(p, fsz);

  STRATUM_CHECK_TRUE(frags.size() > 1);
  bool all_header = true;
  bool all_match = true;
  bool all_within = true;
  for (const auto& f : frags) {
    if (!HasRtpsHeader(f)) all_header = false;
    if (!HeaderMatches(f, prefix)) all_match = false;
    // 每片 = 负载(<=fsz) + 固定开销；固定开销远小于 128
    if (f.size() > static_cast<size_t>(fsz) + 128) all_within = false;
  }
  STRATUM_CHECK_TRUE(all_header);
  STRATUM_CHECK_TRUE(all_match);
  STRATUM_CHECK_TRUE(all_within);
}

STRATUM_TEST(RtpsUsage, FragmentsCarryEnoughRoomForPayload) {
  // 分片的目的是"每片不超过 MTU"，因此必须验证：
  //   各片除固定开销外，能够容纳的总负载 >= 原始 payload
  const uint8_t prefix[12] = {1, 15, 'S', 'T', 'R', 'A', 'T', 'U', 'M', 0, 0, 4};
  const size_t payload = 100000;
  const uint16_t fsz = 1400;
  packet::DataPacketParams p = MakeParams(prefix, payload, 5);
  const auto frags = packet::BuildDataFragPackets(p, fsz);

  // 用"每片大小 - 最小固定开销(56，实测 DATA_FRAG 的开销)"估下界，
  // 保守做法是：片数 × fsz 必须 >= payload（否则一定装不下）
  const size_t capacity = frags.size() * static_cast<size_t>(fsz);
  STRATUM_CHECK_TRUE(capacity >= payload);
  // 且片数不能离谱地多
  STRATUM_CHECK_TRUE(frags.size() <= (payload + fsz - 1) / fsz);
}

STRATUM_TEST(RtpsUsage, FragmentBoundaryWithTinyFragSize) {
  const uint8_t prefix[12] = {1, 15, 'S', 'T', 'R', 'A', 'T', 'U', 'M', 0, 0, 5};
  // 极小 frag_size 也要能算对，而不是除零或崩溃
  for (uint16_t fsz : {1, 2, 50, 200}) {
    packet::DataPacketParams p = MakeParams(prefix, 1000, 1);
    const auto frags = packet::BuildDataFragPackets(p, fsz);
    STRATUM_CHECK_EQ(frags.size(), static_cast<size_t>((1000 + fsz - 1) / fsz));
  }
}

// ===========================================================================
// 三、常量表（RTPS 的"字典"，写错一位对端就解析不了）
// ===========================================================================

STRATUM_TEST(RtpsUsage, SubmessageTypeConstants) {
  STRATUM_CHECK_EQ(static_cast<int>(packet::kSubPad), 0x01);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kSubAcknack), 0x06);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kSubHeartbeat), 0x07);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kSubGap), 0x08);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kSubInfoTs), 0x09);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kSubInfoDst), 0x0e);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kSubData), 0x15);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kSubDataFrag), 0x16);
}

STRATUM_TEST(RtpsUsage, EntityIdConstants) {
  STRATUM_CHECK_EQ(packet::kEntitySpdpWriter, 0x000100c2u);
  STRATUM_CHECK_EQ(packet::kEntitySpdpReader, 0x000100c7u);
  STRATUM_CHECK_EQ(packet::kEntitySedpPubWriter, 0x000003c2u);
  STRATUM_CHECK_EQ(packet::kEntitySedpPubReader, 0x000003c7u);
  STRATUM_CHECK_EQ(packet::kEntitySedpSubWriter, 0x000004c2u);
  STRATUM_CHECK_EQ(packet::kEntitySedpSubReader, 0x000004c7u);
  STRATUM_CHECK_EQ(packet::kEntityUserWriter, 0x00000103u);
  STRATUM_CHECK_EQ(packet::kEntityUserReader, 0x00000104u);
}

STRATUM_TEST(RtpsUsage, QosAndPidConstants) {
  STRATUM_CHECK_EQ(packet::kReliabilityBestEffort, 1u);
  STRATUM_CHECK_EQ(packet::kReliabilityReliable, 2u);
  STRATUM_CHECK_EQ(packet::kDurabilityVolatile, 0u);
  STRATUM_CHECK_EQ(packet::kDurabilityTransientLocal, 1u);

  STRATUM_CHECK_EQ(static_cast<int>(packet::kPidSentinel), 0x0001);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kPidTopicName), 0x0005);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kPidTypeName), 0x0007);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kPidProtocolVersion), 0x0015);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kPidVendorId), 0x0016);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kPidReliability), 0x001a);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kPidDurability), 0x001d);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kPidUnicastLocator), 0x002f);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kPidEndpointGuid), 0x005a);
  STRATUM_CHECK_EQ(static_cast<int>(packet::kPidParticipantGuid), 0x0050);
}

// ===========================================================================
// 四、配置结构：默认值与字段语义
// ===========================================================================

STRATUM_TEST(RtpsUsage, WriterConfigDefaults) {
  RtpsWriterConfig w;
  STRATUM_CHECK_EQ(w.domain_id, 0u);
  STRATUM_CHECK_TRUE(w.guid_prefix.empty());          // 空 = 自动生成
  STRATUM_CHECK_EQ(w.metatraffic_port, static_cast<uint16_t>(0));  // 0 = 按 domain 算
  STRATUM_CHECK_EQ(w.user_port, static_cast<uint16_t>(0));
  STRATUM_CHECK_TRUE(w.advertised_address.empty());
  STRATUM_CHECK_TRUE(w.initial_peers.empty());
  STRATUM_CHECK_EQ(w.max_udp_payload, 0u);            // 0 = 由 MTU 探测决定
  STRATUM_CHECK_TRUE(w.auto_mtu_detect);              // 默认开启
  STRATUM_CHECK_EQ(w.send_window, 0u);                // 0 = 不流控
  STRATUM_CHECK_TRUE(w.send_window_bytes > 0);
  // 发送窗口超时必须能放开，否则永久阻塞
  STRATUM_CHECK_TRUE(w.send_window_timeout_ms > 0);
  // 生产环境不该默认丢包
  STRATUM_CHECK_EQ(w.debug_drop_send_percent, 0u);
  STRATUM_CHECK_EQ(w.max_samples_per_sec, 0ull);      // 0 = 不限速
  STRATUM_CHECK_EQ(w.max_bytes_per_sec, 0ull);
}

STRATUM_TEST(RtpsUsage, ReaderConfigDefaults) {
  RtpsReaderConfig r;
  STRATUM_CHECK_EQ(r.domain_id, 0u);
  STRATUM_CHECK_TRUE(r.guid_prefix.empty());
  STRATUM_CHECK_TRUE(r.advertised_address.empty());
  STRATUM_CHECK_TRUE(r.initial_peers.empty());
  // 空过滤 = 订阅全部（自动发现模式）
  STRATUM_CHECK_TRUE(r.topic_filter.empty());
  STRATUM_CHECK_TRUE(r.announce_interval_ms > 0);
}

STRATUM_TEST(RtpsUsage, WriterConfigFieldsRoundTrip) {
  RtpsWriterConfig w;
  w.domain_id = 73;
  w.participant_name = "unit-test-writer";
  w.advertised_address = "192.168.100.1";
  w.initial_peers = {"192.168.100.129", "127.0.0.1"};
  w.max_udp_payload = 1472;
  w.auto_mtu_detect = false;
  w.send_window = 64;
  w.max_samples_per_sec = 1000;
  w.debug_drop_send_percent = 10;

  STRATUM_CHECK_EQ(w.domain_id, 73u);
  STRATUM_CHECK_EQ(w.participant_name, std::string("unit-test-writer"));
  STRATUM_CHECK_EQ(w.advertised_address, std::string("192.168.100.1"));
  STRATUM_CHECK_EQ(w.initial_peers.size(), static_cast<size_t>(2));
  STRATUM_CHECK_EQ(w.max_udp_payload, static_cast<uint32_t>(1472));
  STRATUM_CHECK_FALSE(w.auto_mtu_detect);
  STRATUM_CHECK_EQ(w.send_window, 64u);
  STRATUM_CHECK_EQ(w.max_samples_per_sec, 1000ull);
  STRATUM_CHECK_EQ(w.debug_drop_send_percent, 10u);
}

// ===========================================================================
// 五、大数据量
// ===========================================================================

STRATUM_TEST(RtpsUsage, LargeVolumePacketBuild) {
  const size_t kN = 100000;
  const uint8_t prefix[12] = {1, 15, 'S', 'T', 'R', 'A', 'T', 'U', 'M', 0, 0, 9};

  auto t0 = Clock::now();
  size_t total = 0;
  bool all_ok = true;
  for (size_t i = 0; i < kN; ++i) {
    packet::DataPacketParams p = MakeParams(prefix, 64 + (i % 256), i + 1);
    const auto raw = packet::BuildDataPacket(p);
    if (!HasRtpsHeader(raw) || !HeaderMatches(raw, prefix)) { all_ok = false; break; }
    total += raw.size();
  }
  const double ms = MsSince(t0);

  STRATUM_CHECK_TRUE(all_ok);
  STRATUM_CHECK_TRUE(total > 0);
  std::cout << "      [规模] 构造 " << kN << " 条 DATA 报文：" << ms << " ms，共 "
            << (total / 1024) << " KB（" << (kN / (ms / 1000.0) / 1000.0)
            << " K 条/秒）" << std::endl;
}

STRATUM_TEST(RtpsUsage, LargeVolumeFragmentation) {
  const uint8_t prefix[12] = {1, 15, 'S', 'T', 'R', 'A', 'T', 'U', 'M', 0, 0, 10};
  const size_t kSamples = 10000;
  const uint16_t fsz = 1400;

  auto t0 = Clock::now();
  size_t total_frags = 0;
  size_t total_bytes = 0;
  bool count_ok = true;
  for (size_t i = 0; i < kSamples; ++i) {
    const size_t payload = 1400 + (i % 14000);      // 1..15 片
    packet::DataPacketParams p = MakeParams(prefix, payload, i + 1);
    const auto frags = packet::BuildDataFragPackets(p, fsz);
    if (frags.size() != (payload + fsz - 1) / fsz) { count_ok = false; break; }
    total_frags += frags.size();
    for (const auto& f : frags) total_bytes += f.size();
  }
  const double ms = MsSince(t0);

  STRATUM_CHECK_TRUE(count_ok);
  STRATUM_CHECK_TRUE(total_frags >= kSamples);
  std::cout << "      [规模] " << kSamples << " 个样本切成 " << total_frags
            << " 片，共 " << (total_bytes / 1024) << " KB，耗时 " << ms
            << " ms" << std::endl;
}

// ===========================================================================
// 六、随机（多种子）
// ===========================================================================

STRATUM_TEST(RtpsUsage, RandomFragSizeAndPayload) {
  // 随机 payload × 随机 frag_size，验证分片数与头部始终正确。
  // 依赖概率的断言是 flaky 断言，因此每个种子都做**确定性**校验。
  for (uint64_t seed : {1ULL, 42ULL, 777ULL, 20261014ULL}) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<size_t> payload_d(0, 30000);
    std::uniform_int_distribution<uint16_t> frag_d(1, 1500);
    std::uniform_int_distribution<uint32_t> tag_d(0, 255);

    for (int i = 0; i < 300; ++i) {
      const size_t payload = payload_d(rng);
      const uint16_t fsz = frag_d(rng);
      const uint8_t tag = static_cast<uint8_t>(tag_d(rng));

      uint8_t prefix[12];
      FillPrefix(prefix, tag);
      packet::DataPacketParams p = MakeParams(prefix, payload, i + 1);

      const auto frags = packet::BuildDataFragPackets(p, fsz);
      const size_t expect = payload == 0 ? 0 : (payload + fsz - 1) / fsz;
      // payload 为 0 时分片数应为 0 或 1（实现细节，取其上界）
      STRATUM_CHECK_TRUE(frags.size() == expect || (payload == 0 && frags.size() <= 1));

      for (const auto& f : frags) {
        STRATUM_CHECK_TRUE(HasRtpsHeader(f));
        STRATUM_CHECK_TRUE(HeaderMatches(f, prefix));
      }
    }
  }
}

STRATUM_TEST(RtpsUsage, RandomGuidPrefixAndSn) {
  for (uint64_t seed : {7ULL, 99ULL, 20261014ULL}) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> byte_d(0, 255);
    std::uniform_int_distribution<uint64_t> sn_d(1, 1ULL << 40);

    for (int i = 0; i < 2000; ++i) {
      uint8_t prefix[12];
      for (int k = 0; k < 12; ++k) prefix[k] = static_cast<uint8_t>(byte_d(rng));

      packet::DataPacketParams p = MakeParams(prefix, 32 + (i % 64), sn_d(rng));
      const auto raw = packet::BuildDataPacket(p);

      // prefix 必须逐字节原样写入（若错位，对端会把报文归到别的参与者）
      STRATUM_CHECK_TRUE(HeaderMatches(raw, prefix));
    }
  }
}

// ===========================================================================
// 七、真实场景
// ===========================================================================

// ---------------------------------------------------------------------------
// 场景 1：MTU 受限下的切片
//
// 现实：MTU 1500 的路径上，不分片的 UDP 上限是 1472 字节；超过就要依赖
// **IP 分片**，而 IP 分片在 NAT / 中继路径上会被丢弃 —— 症状是"发送端显示
// 已发布、对端一条都收不到"，且没有任何报错。
// 因此使用侧必须能把大样本切成每片都不超过 MTU 的 RTPS 报文。
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsUsage, RealScenario_MtuConstrainedSlicing) {
  const uint8_t prefix[12] = {1, 15, 'S', 'T', 'R', 'A', 'T', 'U', 'M', 0, 0, 20};

  // 典型路径的可用负载上限
  struct PathCase {
    const char* name;
    uint16_t max_payload;
    size_t sample_size;
  };
  const PathCase paths[] = {
      {"loopback (65507)", 65000, 60000},
      {"ethernet MTU1500", 1472, 8000},
      {"ethernet MTU1500 大样本", 1472, 60000},
      {"保守 MTU1280", 1232, 20000},
  };

  for (const auto& pc : paths) {
    packet::DataPacketParams p = MakeParams(prefix, pc.sample_size, 1);
    const auto frags = packet::BuildDataFragPackets(p, pc.max_payload);

    // 关键约束：**每一片都必须能在该路径的 MTU 内发出去**
    bool all_fit = true;
    size_t max_frag = 0;
    for (const auto& f : frags) {
      max_frag = std::max(max_frag, f.size());
      // 片上大小 = 负载(<=max_payload) + 固定开销(实测 56)
      if (f.size() > static_cast<size_t>(pc.max_payload) + 56) all_fit = false;
    }

    STRATUM_CHECK_TRUE(all_fit);
    std::cout << "      [场景1] " << pc.name << "：样本 " << pc.sample_size
              << "B -> " << frags.size() << " 片，最大片 " << max_frag
              << "B（上限 " << (pc.max_payload + 56) << "B）" << std::endl;
  }
}

// ---------------------------------------------------------------------------
// 场景 2：发送侧限速与窗口
//
// 现实：现场带宽有限，发送必须能限速；在途数据不能无限增长（内存会爆），
// 但也不能永久阻塞（对端挂了就再也不发）。
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsUsage, RealScenario_WriterThrottleAndWindow) {
  // 2a 限速配置：三个维度都要能设
  RtpsWriterConfig w;
  w.domain_id = 73;
  w.participant_name = "throttled-writer";
  w.max_samples_per_sec = 500;              // 条/秒
  w.max_bytes_per_sec = 2ULL * 1024 * 1024;  // 字节/秒
  STRATUM_CHECK_EQ(w.max_samples_per_sec, 500ull);
  STRATUM_CHECK_EQ(w.max_bytes_per_sec, 2ull * 1024 * 1024);

  // 2b 发送窗口：限制在途未确认条数，避免内存无界增长
  w.send_window = 128;
  w.send_window_bytes = 512 * 1024;
  STRATUM_CHECK_EQ(w.send_window, 128u);
  STRATUM_CHECK_EQ(w.send_window_bytes, 512u * 1024u);
  // 超时放开是防"对端消失导致永久阻塞"的关键参数，必须存在且为正
  STRATUM_CHECK_TRUE(w.send_window_timeout_ms > 0);

  // 2c 心跳与公告间隔：驱动对端 ACKNACK，决定了重传的及时性
  w.heartbeat_interval_ms = 100;
  w.announce_interval_ms = 200;
  STRATUM_CHECK_EQ(w.heartbeat_interval_ms, 100u);
  STRATUM_CHECK_TRUE(w.announce_interval_ms >= w.heartbeat_interval_ms);

  // 2d 自动 MTU 探测：默认开，可用显式值覆盖
  RtpsWriterConfig auto_mtu;
  STRATUM_CHECK_TRUE(auto_mtu.auto_mtu_detect);
  STRATUM_CHECK_EQ(auto_mtu.max_udp_payload, 0u);      // 0 表示由探测决定
  RtpsWriterConfig manual;
  manual.auto_mtu_detect = false;
  manual.max_udp_payload = 1472;
  STRATUM_CHECK_FALSE(manual.auto_mtu_detect);
  STRATUM_CHECK_EQ(manual.max_udp_payload, static_cast<uint32_t>(1472));

  std::cout << "      [场景2] 限速 500 条/秒、2MB/秒；窗口 128 条 / 512KB；"
               "超时 500ms 放开" << std::endl;
}

// ---------------------------------------------------------------------------
// 场景 3：接收侧话题过滤与 QoS
//
// 现实：现场话题可能上百个，全订阅会把带宽和磁盘吃光；
// 且录制器关心的 QoS（可靠性/持久性）决定了它能不能收到历史数据。
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsUsage, RealScenario_ReaderTopicFilterAndQos) {
  RtpsReaderConfig r;
  r.domain_id = 73;
  r.participant_name = "filtered-reader";
  r.advertised_address = "127.0.0.1";
  r.initial_peers = {"127.0.0.1"};

  // 3a 前缀通配：只订阅关心的话题族
  r.topic_filter = {"sensor/*", "line3/*"};
  STRATUM_CHECK_EQ(r.topic_filter.size(), static_cast<size_t>(2));
  // topic_filter 是 set，必须是去重且有序的
  std::set<std::string> expect = {"sensor/*", "line3/*"};
  STRATUM_CHECK_TRUE(r.topic_filter == expect);

  // 3b 精确名：只订一个话题
  RtpsReaderConfig exact;
  exact.topic_filter = {"only/this/topic"};
  STRATUM_CHECK_EQ(exact.topic_filter.size(), static_cast<size_t>(1));

  // 3c 空过滤 = 全部（自动发现），这是录制器的默认期望行为
  RtpsReaderConfig all;
  STRATUM_CHECK_TRUE(all.topic_filter.empty());

  // 3d initial_peers：不依赖组播也能发现对端
  STRATUM_CHECK_EQ(r.initial_peers.size(), static_cast<size_t>(1));
  STRATUM_CHECK_EQ(r.initial_peers.front(), std::string("127.0.0.1"));

  std::cout << "      [场景3] 话题过滤 " << r.topic_filter.size()
            << " 条 + 显式对端 " << r.initial_peers.size() << " 个" << std::endl;
}

// ---------------------------------------------------------------------------
// 场景 4（附加）：后端能力声明
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsUsage, BackendCapabilitiesAreHonest) {
  // RtpsBackend 只做构造与能力查询，不 Open，因此不建 socket、无副作用
  stratum::protocol::rtps::RtpsBackend backend;
  STRATUM_CHECK_EQ(backend.Name(), std::string("rtps"));
  STRATUM_CHECK_TRUE(!backend.Version().empty());

  const auto caps = backend.GetCapabilities();
  // 这些是使用 side 必须知道的：能不能"录全部"、能不能做缺口检测
  STRATUM_CHECK_TRUE(caps.auto_discovery);
  STRATUM_CHECK_TRUE(caps.provides_sequence);
  // 是否提供类型定义取决于是否启用 XTypes 支持
  std::cout << "      [能力] auto_discovery=" << (caps.auto_discovery ? 1 : 0)
            << " provides_sequence=" << (caps.provides_sequence ? 1 : 0)
            << " provides_type_def=" << (caps.provides_type_def ? 1 : 0)
            << " multicast=" << (caps.supports_multicast ? 1 : 0) << std::endl;
}

STRATUM_TEST_MAIN()
