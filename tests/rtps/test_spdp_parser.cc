// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// SPDP 发现解析验证：真实流量 + 随机 + 压力
//
// 真实流量用例是 M2 的验收标准：能从 Fast DDS 实际发出的
// SPDP 公告中解析出参与者 GUID / 租约 / 端点集 / locator。

#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <stratum/rtps/builtin/spdp_parser.h>
#include <stratum/rtps/common/CDRMessage_t.hpp>
#include <stratum/rtps/messages/CDRMessage.hpp>

#include "test_framework.h"

using namespace stratum::rtps;
using namespace eprosima::fastdds::rtps;

namespace {

// 读取抓包向量文件（格式：[u32 count][u32 len][bytes]...）
bool LoadVectors(const std::string& path,
                 std::vector<std::vector<uint8_t>>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  uint32_t count = 0;
  in.read(reinterpret_cast<char*>(&count), sizeof(count));
  if (count == 0 || count > 100000) return false;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (len == 0 || len > 65535) break;
    std::vector<uint8_t> buf(len);
    in.read(reinterpret_cast<char*>(buf.data()), len);
    out->push_back(std::move(buf));
  }
  return !out->empty();
}

}  // namespace

// ---------------------------------------------------------------------------
// 1) 真实 Fast DDS SPDP 公告解析（M2 验收）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSpdp, ParseRealFastDdsAnnouncement) {
  std::vector<std::vector<uint8_t>> packets;
  const char* path = "tests/vectors/spdp_capture.bin";
  // 【不可静默跳过】向量文件是真实流量验证的基础。
  // 早期版本在文件缺失时打印"跳过"并断言 true —— 那是**假通过**，
  // 会让"真实流量验证"在 CI 上悄悄失效。缺失即失败。
  if (!LoadVectors(path, &packets)) {
    ctx.param_desc = std::string("**向量文件缺失或损坏**（真实流量验证无法执行）: ") + path;
    STRATUM_CHECK_TRUE(false);
    return;
  }

  SpdpParser parser;
  std::vector<SpdpParticipantData> participants;
  for (const auto& p : packets) {
    parser.ParseMessage(p.data(), p.size(), &participants);
  }

  ctx.param_desc = "packets=" + std::to_string(packets.size()) +
                   " participants=" + std::to_string(participants.size());
  STRATUM_CHECK_TRUE(!participants.empty());

  const SpdpParticipantData& pd = participants[0];

  // GUID（SPDP 公告的核心）
  char guid_str[64] = {0};
  std::snprintf(guid_str, sizeof(guid_str),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x",
                pd.guid.guidPrefix.value[0], pd.guid.guidPrefix.value[1],
                pd.guid.guidPrefix.value[2], pd.guid.guidPrefix.value[3],
                pd.guid.guidPrefix.value[4], pd.guid.guidPrefix.value[5],
                pd.guid.guidPrefix.value[6], pd.guid.guidPrefix.value[7]);
  ctx.param_desc += " first_guid=" + std::string(guid_str) +
                    " vendor=" + std::to_string(pd.source_vendor_id[0]) +
                    std::to_string(pd.source_vendor_id[1]) +
                    " lease_sec=" + std::to_string(pd.lease_duration.seconds()) +
                    " builtin_ep=0x" + std::to_string(pd.builtin_endpoints) +
                    " params=" + std::to_string(pd.parameters_seen);

  STRATUM_CHECK_TRUE(pd.has_guid);
  STRATUM_CHECK_TRUE(pd.has_lease_duration);
  STRATUM_CHECK_TRUE(pd.has_builtin_endpoints);
  STRATUM_CHECK_TRUE(pd.has_protocol_version);
  STRATUM_CHECK_TRUE(pd.has_vendor_id);

  // eProsima vendor id
  STRATUM_CHECK_EQ(static_cast<int>(pd.vendor_id[0]), 0x01);
  STRATUM_CHECK_EQ(static_cast<int>(pd.vendor_id[1]), 0x0f);

  // 协议版本 2.x
  STRATUM_CHECK_EQ(static_cast<int>(pd.protocol_version.m_major), 2);

  // 租约时长应为正（Fast DDS 默认 20s 左右）
  STRATUM_CHECK_TRUE(pd.lease_duration.seconds() > 0);
  STRATUM_CHECK_TRUE(pd.lease_duration.seconds() <= 3600);

  // 至少有一个 locator
  const size_t locators = pd.default_unicast_locators.size() +
                          pd.metatraffic_unicast_locators.size() +
                          pd.default_multicast_locators.size() +
                          pd.metatraffic_multicast_locators.size();
  ctx.param_desc += " locators=" + std::to_string(locators);
  STRATUM_CHECK_TRUE(locators > 0);

  // 解析器不应报畸形
  ctx.param_desc += " malformed=" + std::to_string(parser.stats().malformed);
  STRATUM_CHECK_EQ(parser.stats().malformed, 0);
}

// ---------------------------------------------------------------------------
// 2) 随机字节流压力：不得崩溃、不得越界
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSpdp, RandomBytesStress) {
  constexpr int kIterations = 30000;
  std::mt19937_64 rng(20261120);
  SpdpParser parser;

  size_t accepted = 0;
  for (int i = 0; i < kIterations; ++i) {
    const size_t len = 20 + (rng() % 512);
    std::vector<uint8_t> buf(len);
    // 一半用真魔数开头，一半完全随机（考验头部校验）
    if ((rng() & 1) != 0) {
      std::memcpy(buf.data(), "RTPS", 4);
      buf[4] = 2;
      buf[5] = 2;
      for (size_t k = 6; k < len; ++k) {
        buf[k] = static_cast<uint8_t>(rng());
      }
    } else {
      for (size_t k = 0; k < len; ++k) {
        buf[k] = static_cast<uint8_t>(rng());
      }
    }
    std::vector<SpdpParticipantData> out;
    parser.ParseMessage(buf.data(), buf.size(), &out);
    accepted += out.size();
  }

  ctx.param_desc = "iterations=" + std::to_string(kIterations) +
                   " incidentally_parsed=" + std::to_string(accepted);
  STRATUM_CHECK_TRUE(true);   // 未崩溃即通过
}

// ---------------------------------------------------------------------------
// 3) 截断压力：真实公告的每个前缀长度都必须安全
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSpdp, TruncationStress) {
  std::vector<std::vector<uint8_t>> packets;
  // 【不可静默跳过】见 ParseRealFastDdsAnnouncement 中的说明。
  if (!LoadVectors("tests/vectors/spdp_capture.bin", &packets)) {
    ctx.param_desc = "**向量文件缺失**（截断压力测试无法执行）";
    STRATUM_CHECK_TRUE(false);
    return;
  }

  // 取第一个真实包，从 0 字节逐步截断到全长
  const auto& base = packets[0];
  SpdpParser parser;
  size_t safe = 0;
  size_t truncated_cases = 0;

  for (size_t cut = 0; cut <= base.size(); ++cut) {
    std::vector<SpdpParticipantData> out;
    // 不得崩溃
    parser.ParseMessage(base.data(), cut, &out);
    // 截断到不足 20 字节时必须解析不出参与者
    if (cut < 20) {
      ++truncated_cases;
      if (out.empty()) ++safe;
    } else {
      ++truncated_cases;
      ++safe;
    }
  }

  ctx.param_desc = "cases=" + std::to_string(truncated_cases) +
                   " safe=" + std::to_string(safe);
  STRATUM_CHECK_EQ(safe, truncated_cases);
}

// ---------------------------------------------------------------------------
// 4) 重复解析压力（吞吐 + 稳定性）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSpdp, RepeatedParseStress) {
  std::vector<std::vector<uint8_t>> packets;
  // 【不可静默跳过】见 ParseRealFastDdsAnnouncement 中的说明。
  if (!LoadVectors("tests/vectors/spdp_capture.bin", &packets)) {
    ctx.param_desc = "**向量文件缺失**（截断压力测试无法执行）";
    STRATUM_CHECK_TRUE(false);
    return;
  }

  constexpr int kRounds = 2000;
  SpdpParser parser;
  size_t total_participants = 0;
  size_t total_bytes = 0;

  for (int r = 0; r < kRounds; ++r) {
    for (const auto& p : packets) {
      std::vector<SpdpParticipantData> out;
      total_participants += parser.ParseMessage(p.data(), p.size(), &out);
      total_bytes += p.size();
    }
  }

  ctx.param_desc = "rounds=" + std::to_string(kRounds) +
                   " packets_per_round=" + std::to_string(packets.size()) +
                   " bytes=" + std::to_string(total_bytes) +
                   " participants=" + std::to_string(total_participants);
  STRATUM_CHECK_TRUE(total_participants > 0);
}

STRATUM_TEST_MAIN()
