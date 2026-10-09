// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// SPDP 发送侧验证：随机往返 + 压力 + 结构自洽
//
// ===========================================================================
// 闭环设计
// ===========================================================================
//
//   组装侧：BuildSpdpAnnouncement（字段顺序照抄上游 write_to_cdr_message）
//   解析侧：SpdpParser（复用移植的 ParameterList 模板 + 自写 PID 处理器）
//
// 二者独立来源 → 逐字段比对可发现组装偏差。
// 另有两类结构断言，它们针对的是**已实际导致 Fast DDS 拒收**的两类缺陷：
//   1. 子消息长度必须自洽（否则接收方报 SubmessageHeader too short）
//   2. 参数的 length 字段必须与真实 Fast DDS 一致
//      （PID_PROTOCOL_VERSION / PID_VENDORID 实测为 4，而非规范字面的 2）
//
// 端到端验收（Fast DDS 能否真的发现 Stratum）由
// tests/rtps/run_spdp_discovery_test.sh 承担。

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <stratum/rtps/builtin/spdp_announcer.h>
#include <stratum/rtps/builtin/spdp_parser.h>

#include "test_framework.h"

using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;

namespace {

// 结构自洽检查：遍历子消息，验证恰好走到消息末尾
bool SubmessagesSelfConsistent(const std::vector<uint8_t>& m, size_t* end_out) {
  if (m.size() < 20) return false;
  size_t p = 20;
  while (p < m.size()) {
    if (p + 4 > m.size()) {
      *end_out = p;
      return false;
    }
    const uint8_t sid = m[p];
    const uint16_t olen = static_cast<uint16_t>(m[p + 2]) |
                          (static_cast<uint16_t>(m[p + 3]) << 8);
    if (olen == 0 && sid != 0x09 && sid != 0x01) {
      p += 4;
      break;
    }
    p += 4 + olen;
  }
  *end_out = p;
  return p == m.size();
}

void MakeRandomConfig(std::mt19937_64* rng, uint32_t domain,
                      SpdpAnnounceConfig* cfg) {
  cfg->domain_id = domain;
  cfg->vendor_id = VendorId_t{{0x01, 0x0f}};
  cfg->protocol_version = ProtocolVersion_t{2, 4};
  cfg->builtin_endpoints = 0x000f0c3f;
  cfg->lease_duration = Time_t(static_cast<int32_t>(5 + (*rng)() % 300), 0);
  cfg->participant_name = "stratum_" + std::to_string((*rng)() % 100000);

  for (unsigned i = 0; i < GuidPrefix_t::size; ++i) {
    cfg->guid_prefix.value[i] = static_cast<octet>((*rng)() & 0xFF);
  }
  // 前 2 字节必须是 vendorId（RTPS 规范 9.3.1.1）
  cfg->guid_prefix.value[0] = 0x01;
  cfg->guid_prefix.value[1] = 0x0f;

  const int n_meta = 1 + static_cast<int>((*rng)() % 3);
  const int n_default = static_cast<int>((*rng)() % 3);
  for (int i = 0; i < n_meta; ++i) {
    cfg->metatraffic_unicast.push_back(MakeDefaultUdpv4Locator(
        "127.0.0.1", static_cast<uint16_t>(7000 + (*rng)() % 2000)));
  }
  for (int i = 0; i < n_default; ++i) {
    cfg->default_unicast.push_back(MakeDefaultUdpv4Locator(
        "127.0.0.1", static_cast<uint16_t>(7000 + (*rng)() % 2000)));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// 1) 结构自洽（针对已实际发生的 SubmessageHeader too short 缺陷）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSpdpSend, SubmessageChainIsSelfConsistent) {
  constexpr int kIterations = 2000;
  std::mt19937_64 rng(20261201);
  int broken = 0;
  size_t total_bytes = 0;

  for (int i = 0; i < kIterations; ++i) {
    SpdpAnnounceConfig cfg;
    MakeRandomConfig(&rng, static_cast<uint32_t>(i % 200), &cfg);
    const auto m = BuildSpdpAnnouncement(cfg);
    if (m.empty()) { ++broken; continue; }
    total_bytes += m.size();

    size_t end = 0;
    if (!SubmessagesSelfConsistent(m, &end)) {
      ++broken;
      if (broken == 1) {
        ctx.param_desc = "首个不自洽: 总长=" + std::to_string(m.size()) +
                         " 遍历结束=" + std::to_string(end);
      }
    }
  }

  ctx.param_desc += " iterations=" + std::to_string(kIterations) +
                    " bytes=" + std::to_string(total_bytes) +
                    " broken=" + std::to_string(broken);
  STRATUM_CHECK_EQ(broken, 0);
  STRATUM_CHECK_TRUE(total_bytes > 200000);
}

// ---------------------------------------------------------------------------
// 2) 随机往返：组装 → 用移植的解析器解析 → 逐字段比对
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSpdpSend, RandomRoundTripThroughParser) {
  constexpr int kIterations = 3000;
  std::mt19937_64 rng(20261202);
  SpdpParser parser;
  int mismatches = 0;
  uint64_t checked_fields = 0;

  for (int i = 0; i < kIterations; ++i) {
    SpdpAnnounceConfig cfg;
    MakeRandomConfig(&rng, static_cast<uint32_t>(i % 200), &cfg);
    const auto m = BuildSpdpAnnouncement(cfg);
    if (m.empty()) { ++mismatches; continue; }

    std::vector<SpdpParticipantData> out;
    parser.ParseMessage(m.data(), m.size(), &out);
    if (out.empty()) { ++mismatches; continue; }

    const auto& pd = out[0];
    // GUID（前缀 + 实体）
    if (std::memcmp(pd.guid.guidPrefix.value, cfg.guid_prefix.value,
                    GuidPrefix_t::size) != 0) {
      ++mismatches;
    }
    // 租约
    if (pd.lease_duration.seconds() != cfg.lease_duration.seconds()) ++mismatches;
    // 端点集
    if (pd.builtin_endpoints != cfg.builtin_endpoints) ++mismatches;
    // 协议版本 / vendor
    if (pd.protocol_version.m_major != cfg.protocol_version.m_major ||
        pd.protocol_version.m_minor != cfg.protocol_version.m_minor) {
      ++mismatches;
    }
    if (pd.vendor_id[0] != cfg.vendor_id[0] ||
        pd.vendor_id[1] != cfg.vendor_id[1]) {
      ++mismatches;
    }
    // domain
    if (pd.domain_id != cfg.domain_id) ++mismatches;
    // locator 数量
    if (pd.metatraffic_unicast_locators.size() != cfg.metatraffic_unicast.size() ||
        pd.default_unicast_locators.size() != cfg.default_unicast.size()) {
      ++mismatches;
    }
    checked_fields += 6 + cfg.metatraffic_unicast.size() + cfg.default_unicast.size();
  }

  ctx.param_desc = "iterations=" + std::to_string(kIterations) +
                   " fields=" + std::to_string(checked_fields) +
                   " mismatches=" + std::to_string(mismatches);
  STRATUM_CHECK_EQ(mismatches, 0);
  STRATUM_CHECK_TRUE(checked_fields > 20000);
}

// ---------------------------------------------------------------------------
// 3) 参数 length 必须与真实 Fast DDS 一致（针对已实际导致拒收的缺陷）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSpdpSend, ParameterLengthsMatchRealFastDds) {
  SpdpAnnounceConfig cfg;
  const uint8_t prefix[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                              0x54, 0x55, 0x4d, 0x00, 0x00, 0x01};
  std::memcpy(cfg.guid_prefix.value, prefix, 12);
  cfg.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", 7410));

  const auto m = BuildSpdpAnnouncement(cfg);
  STRATUM_CHECK_TRUE(!m.empty());

  // 定位 DATA 子消息的参数表
  size_t p = 20;
  size_t param_start = 0;
  while (p + 4 <= m.size()) {
    const uint8_t sid = m[p];
    const uint16_t olen = static_cast<uint16_t>(m[p + 2]) |
                          (static_cast<uint16_t>(m[p + 3]) << 8);
    if (sid == 0x15) {
      // extraFlags(2) + octetsToInlineQos(2) + reader(4) + writer(4) + sn(8)
      param_start = p + 4 + 20 + 4;   // + encapsulation(4)
      break;
    }
    if (olen == 0) break;
    p += 4 + olen;
  }
  STRATUM_CHECK_TRUE(param_start > 0 && param_start < m.size());

  // 收集 PID → length
  std::vector<std::pair<uint16_t, uint16_t>> pids;
  size_t q = param_start;
  while (q + 4 <= m.size()) {
    const uint16_t pid = static_cast<uint16_t>(m[q]) |
                         (static_cast<uint16_t>(m[q + 1]) << 8);
    const uint16_t plen = static_cast<uint16_t>(m[q + 2]) |
                          (static_cast<uint16_t>(m[q + 3]) << 8);
    pids.emplace_back(pid, plen);
    q += 4 + ((plen + 3) & ~3u);
    if (pid == 0x0001) break;
  }

  ctx.param_desc = "params=" + std::to_string(pids.size());
  STRATUM_CHECK_TRUE(pids.size() >= 8);

  // 【关键断言】实测真实 Fast DDS 的长度：
  //   PID_PROTOCOL_VERSION(0x0015) = 4，PID_VENDORID(0x0016) = 4
  // 早期实现按规范字面写 2 → Fast DDS 静默跳过，参与者永不被发现。
  bool proto_ok = false, vendor_ok = false, guid_ok = false, lease_ok = false;
  for (const auto& kv : pids) {
    if (kv.first == 0x0015) { proto_ok = (kv.second == 4); }
    if (kv.first == 0x0016) { vendor_ok = (kv.second == 4); }
    if (kv.first == 0x0050) { guid_ok = (kv.second == 16); }
    if (kv.first == 0x0002) { lease_ok = (kv.second == 8); }
  }
  ctx.param_desc += " proto4=" + std::string(proto_ok ? "Y" : "N") +
                    " vendor4=" + std::string(vendor_ok ? "Y" : "N") +
                    " guid16=" + std::string(guid_ok ? "Y" : "N") +
                    " lease8=" + std::string(lease_ok ? "Y" : "N");
  STRATUM_CHECK_TRUE(proto_ok);
  STRATUM_CHECK_TRUE(vendor_ok);
  STRATUM_CHECK_TRUE(guid_ok);
  STRATUM_CHECK_TRUE(lease_ok);
}

// ---------------------------------------------------------------------------
// 4) 压力：大 locator 列表 + 长名称 + 大量迭代
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSpdpSend, StressLargeConfigs) {
  constexpr int kIterations = 5000;
  std::mt19937_64 rng(20261203);
  int failures = 0;
  size_t total_bytes = 0;

  for (int i = 0; i < kIterations; ++i) {
    SpdpAnnounceConfig cfg;
    MakeRandomConfig(&rng, 0, &cfg);
    // 放大：最多 16 个 locator + 长名称
    for (int k = 0; k < 16; ++k) {
      cfg.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator(
          "127.0.0.1", static_cast<uint16_t>(6000 + (rng() % 4000))));
    }
    cfg.participant_name = std::string(200, 'x');

    const auto m = BuildSpdpAnnouncement(cfg);
    if (m.empty()) { ++failures; continue; }
    total_bytes += m.size();

    size_t end = 0;
    if (!SubmessagesSelfConsistent(m, &end)) ++failures;
  }

  ctx.param_desc = "iterations=" + std::to_string(kIterations) +
                   " bytes=" + std::to_string(total_bytes) +
                   " failures=" + std::to_string(failures);
  STRATUM_CHECK_EQ(failures, 0);
}

STRATUM_TEST_MAIN()
