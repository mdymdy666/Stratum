// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// SEDP 解析器验证：真实流量 + 随机变异 + 压力
//
// 真实流量向量由 tools/discovery_client.cc 采集：
//   它向真实 Fast DDS 发 SPDP → 收 HEARTBEAT → 回 ACKNACK
//   → 收到 Fast DDS 的 SEDP 公告 → 存为向量。
// 因此这些字节是**真实发现协议交互的产物**，不是构造的。

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <stratum/rtps/builtin/sedp_parser.h>

#include "test_framework.h"
#include "rtps/vector_utils.h"

using namespace stratum::rtps;
using namespace stratum::testing;

STRATUM_TEST(RtpsSedp, ParseRealCapture) {
  std::vector<std::vector<uint8_t>> packets;
  // 【不可静默跳过】向量缺失即真实流量验证失效，必须失败。
  if (!LoadCaptureVectors("tests/vectors/sedp_capture.bin", &packets)) {
    ctx.param_desc = "**向量文件缺失**: tests/vectors/sedp_capture.bin";
    STRATUM_CHECK_TRUE(false);
    return;
  }

  SedpParser parser;
  std::vector<SedpEndpointData> endpoints;
  size_t with_topic = 0, with_type = 0, with_guid = 0, with_qos = 0;
  size_t with_locator = 0;

  for (const auto& p : packets) {
    parser.ParseMessage(p.data(), p.size(), &endpoints);
  }

  for (const auto& e : endpoints) {
    if (e.has_topic_name && !e.topic_name.empty()) ++with_topic;
    if (e.has_type_name && !e.type_name.empty()) ++with_type;
    if (e.has_endpoint_guid) ++with_guid;
    if (e.has_reliability) ++with_qos;
    if (!e.unicast_locators.empty() || !e.multicast_locators.empty()) ++with_locator;
  }

  ctx.param_desc = "packets=" + std::to_string(packets.size()) +
                   " endpoints=" + std::to_string(endpoints.size()) +
                   " topic=" + std::to_string(with_topic) +
                   " type=" + std::to_string(with_type) +
                   " guid=" + std::to_string(with_guid) +
                   " qos=" + std::to_string(with_qos) +
                   " locator=" + std::to_string(with_locator);

  STRATUM_CHECK_TRUE(!endpoints.empty());
  // 真实 SEDP 公告必须能解析出话题与类型 —— 这是发现的核心产出
  STRATUM_CHECK_TRUE(with_topic > 0);
  STRATUM_CHECK_TRUE(with_type > 0);
  STRATUM_CHECK_TRUE(with_guid > 0);
  STRATUM_CHECK_TRUE(with_qos > 0);
  STRATUM_CHECK_TRUE(with_locator > 0);
}

// ---------------------------------------------------------------------------
// 随机变异：对真实向量做随机字节翻转/截断，解析器不得崩溃
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSedp, MutatedInputIsSafe) {
  std::vector<std::vector<uint8_t>> packets;
  if (!LoadCaptureVectors("tests/vectors/sedp_capture.bin", &packets)) {
    ctx.param_desc = "**向量文件缺失**";
    STRATUM_CHECK_TRUE(false);
    return;
  }

  constexpr int kIterations = 20000;
  std::mt19937_64 rng(20261210);
  SedpParser parser;
  size_t survived = 0;

  for (int i = 0; i < kIterations; ++i) {
    std::vector<uint8_t> buf = packets[rng() % packets.size()];
    const int mode = static_cast<int>(rng() % 3);
    if (mode == 0) {
      // 随机翻转若干字节
      const int flips = 1 + static_cast<int>(rng() % 16);
      for (int k = 0; k < flips; ++k) {
        buf[rng() % buf.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
      }
    } else if (mode == 1) {
      // 随机截断
      buf.resize(1 + (rng() % buf.size()));
    } else {
      // 随机插入垃圾
      buf.insert(buf.begin() + (rng() % buf.size()),
                 static_cast<uint8_t>(rng()));
    }

    std::vector<SedpEndpointData> out;
    parser.ParseMessage(buf.data(), buf.size(), &out);   // 不得崩溃
    ++survived;
  }

  ctx.param_desc = "iterations=" + std::to_string(kIterations) +
                   " survived=" + std::to_string(survived);
  STRATUM_CHECK_EQ(survived, static_cast<size_t>(kIterations));
}

// ---------------------------------------------------------------------------
// 压力：重复解析真实向量（吞吐 + 稳定性）
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSedp, RepeatedParseStress) {
  std::vector<std::vector<uint8_t>> packets;
  if (!LoadCaptureVectors("tests/vectors/sedp_capture.bin", &packets)) {
    ctx.param_desc = "**向量文件缺失**";
    STRATUM_CHECK_TRUE(false);
    return;
  }

  constexpr int kRounds = 500;
  SedpParser parser;
  size_t total_endpoints = 0;
  size_t total_bytes = 0;

  for (int r = 0; r < kRounds; ++r) {
    for (const auto& p : packets) {
      std::vector<SedpEndpointData> out;
      parser.ParseMessage(p.data(), p.size(), &out);
      total_endpoints += out.size();
      total_bytes += p.size();
    }
  }

  ctx.param_desc = "rounds=" + std::to_string(kRounds) +
                   " packets_per_round=" + std::to_string(packets.size()) +
                   " bytes=" + std::to_string(total_bytes) +
                   " endpoints=" + std::to_string(total_endpoints);
  STRATUM_CHECK_TRUE(total_endpoints > 0);
  STRATUM_CHECK_TRUE(total_bytes > 1000000);
}

// ---------------------------------------------------------------------------
// 全零/超短输入必须安全
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsSedp, DegenerateInputsAreSafe) {
  SedpParser parser;
  int safe = 0, cases = 0;

  auto try_parse = [&](const uint8_t* d, size_t n) {
    ++cases;
    std::vector<SedpEndpointData> out;
    parser.ParseMessage(d, n, &out);
    ++safe;
  };

  const uint8_t zeros[64] = {0};
  try_parse(nullptr, 0);
  try_parse(zeros, 0);
  try_parse(zeros, 1);
  try_parse(zeros, 19);
  try_parse(zeros, 20);
  try_parse(zeros, 64);

  uint8_t only_magic[24] = {'R', 'T', 'P', 'S', 2, 3};
  try_parse(only_magic, sizeof(only_magic));

  ctx.param_desc = "cases=" + std::to_string(cases) +
                   " safe=" + std::to_string(safe);
  STRATUM_CHECK_EQ(safe, cases);
}

STRATUM_TEST_MAIN()
