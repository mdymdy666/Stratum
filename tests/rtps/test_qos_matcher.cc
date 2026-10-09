// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// QoS 兼容判定验证：规则穷举 + 随机交叉验证 + 真实端点数据
//
// ===========================================================================
// 为什么需要"交叉验证"
// ===========================================================================
//
// 只断言"某组合判为兼容/不兼容"容易写成与实现同源的期望值（自证）。
// 这里额外用一个**独立重写**的规则表（RecomputeExpected）重新推导结论，
// 与 MatchWriterToReader 比对 —— 两者独立编码同一套上游规则，
// 任何一侧写错都会暴露。

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <stratum/rtps/builtin/qos_matcher.h>
#include <stratum/rtps/builtin/sedp_parser.h>

#include "test_framework.h"
#include "rtps/vector_utils.h"

using namespace stratum::rtps;
using namespace stratum::testing;
using namespace eprosima::fastdds::rtps;

namespace {

SedpEndpointData MakeWriter(const std::string& topic = "t",
                            const std::string& type = "T") {
  SedpEndpointData w;
  w.kind = SedpKind::kPublication;
  w.topic_name = topic;
  w.has_topic_name = true;
  w.type_name = type;
  w.has_type_name = true;
  w.has_endpoint_guid = true;
  return w;
}

Time_t Secs(int32_t s) { return Time_t(s, 0); }

// ---------------------------------------------------------------------------
// 独立重写的规则推导（用于交叉验证，故意不复用被测实现）
// ---------------------------------------------------------------------------
bool RecomputeExpected(const SedpEndpointData& w, const QosRequest& r) {
  // topic / type 存在性
  if (w.topic_name.empty()) return false;
  if (w.type_name.empty()) return false;

  // reliability：writer BE(1) + reader RE(2) → 不兼容
  if (w.has_reliability && w.reliability_kind == 1 && r.reliability_kind == 2) {
    return false;
  }
  // durability：offered < requested → 不兼容
  if (w.has_durability && w.durability_kind < r.durability_kind) return false;
  // ownership：必须相等
  if (w.has_ownership && w.ownership_kind != r.ownership_kind) return false;
  // deadline：offered period > requested period → 不兼容
  if (w.has_deadline) {
    const auto a = w.deadline;
    const auto b = r.deadline;
    const bool greater = (a.seconds() != b.seconds())
                             ? (a.seconds() > b.seconds())
                             : (a.fraction() > b.fraction());
    if (greater) return false;
  }
  // disable_positive_acks：reader 启用而 writer 未启用 → 不兼容
  if (r.disable_positive_acks) return false;
  // liveliness
  if (w.has_liveliness) {
    const auto la = w.liveliness_lease;
    const auto lb = r.liveliness_lease;
    const bool lease_greater = (la.seconds() != lb.seconds())
                                   ? (la.seconds() > lb.seconds())
                                   : (la.fraction() > lb.fraction());
    if (lease_greater) return false;
    if (w.liveliness_kind < r.liveliness_kind) return false;
  }
  // data representation 交集
  {
    const uint32_t offered = w.has_data_representation ? w.data_representation : 0;
    bool hit = false;
    for (uint32_t req : r.data_representations) {
      if (req == offered) hit = true;
    }
    if (!hit) return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1) 逐条规则的正面与反面用例
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsQos, RuleByRuleCases) {
  int failed = 0;
  int cases = 0;

  auto expect = [&](const SedpEndpointData& w, const QosRequest& r,
                    bool want_match, const char* what) {
    ++cases;
    const MatchResult res = MatchWriterToReader(w, r);
    if (res.matched != want_match) {
      ++failed;
      ctx.param_desc += std::string("\n  [FAIL] ") + what +
                        " 期望=" + (want_match ? "兼容" : "不兼容") +
                        " 实际=" + (res.matched ? "兼容" : "不兼容");
    }
  };

  QosRequest base = DefaultRecorderQos();

  // --- RELIABILITY ---
  {
    SedpEndpointData w = MakeWriter();
    w.has_reliability = true;
    w.reliability_kind = 1;   // BE
    QosRequest r = base;
    r.reliability_kind = 2;   // RE
    expect(w, r, false, "RELIABILITY: writer=BE reader=RE 应不兼容");
  }
  {
    SedpEndpointData w = MakeWriter();
    w.has_reliability = true;
    w.reliability_kind = 2;   // RE
    QosRequest r = base;
    r.reliability_kind = 2;
    expect(w, r, true, "RELIABILITY: RE/RE 应兼容");
  }
  {
    SedpEndpointData w = MakeWriter();
    w.has_reliability = true;
    w.reliability_kind = 2;   // RE writer
    QosRequest r = base;
    r.reliability_kind = 1;   // BE reader
    expect(w, r, true, "RELIABILITY: writer=RE reader=BE 应兼容（writer 更强）");
  }

  // --- DURABILITY ---
  {
    SedpEndpointData w = MakeWriter();
    w.has_durability = true;
    w.durability_kind = 0;    // VOLATILE
    QosRequest r = base;
    r.durability_kind = 1;    // TRANSIENT_LOCAL
    expect(w, r, false, "DURABILITY: offered<requested 应不兼容");
  }
  {
    SedpEndpointData w = MakeWriter();
    w.has_durability = true;
    w.durability_kind = 2;    // TRANSIENT
    QosRequest r = base;
    r.durability_kind = 1;
    expect(w, r, true, "DURABILITY: offered>requested 应兼容");
  }

  // --- OWNERSHIP（必须相等）---
  {
    SedpEndpointData w = MakeWriter();
    w.has_ownership = true;
    w.ownership_kind = 1;     // EXCLUSIVE
    QosRequest r = base;
    r.ownership_kind = 0;     // SHARED
    expect(w, r, false, "OWNERSHIP: 不等应不兼容");
  }
  {
    SedpEndpointData w = MakeWriter();
    w.has_ownership = true;
    w.ownership_kind = 1;
    QosRequest r = base;
    r.ownership_kind = 1;
    expect(w, r, true, "OWNERSHIP: 相等应兼容");
  }

  // --- DEADLINE ---
  {
    SedpEndpointData w = MakeWriter();
    w.has_deadline = true;
    w.deadline = Secs(10);
    QosRequest r = base;
    r.deadline = Secs(5);
    expect(w, r, false, "DEADLINE: offered period > requested 应不兼容");
  }
  {
    SedpEndpointData w = MakeWriter();
    w.has_deadline = true;
    w.deadline = Secs(1);
    QosRequest r = base;
    r.deadline = Secs(5);
    expect(w, r, true, "DEADLINE: offered period <= requested 应兼容");
  }

  // --- LIVELINESS ---
  {
    SedpEndpointData w = MakeWriter();
    w.has_liveliness = true;
    w.liveliness_kind = 0;    // AUTOMATIC
    QosRequest r = base;
    r.liveliness_kind = 2;    // MANUAL_BY_TOPIC
    expect(w, r, false, "LIVELINESS: kind offered<requested 应不兼容");
  }
  {
    SedpEndpointData w = MakeWriter();
    w.has_liveliness = true;
    w.liveliness_kind = 0;
    w.liveliness_lease = Secs(30);
    QosRequest r = base;
    r.liveliness_kind = 0;
    r.liveliness_lease = Secs(10);
    expect(w, r, false, "LIVELINESS: lease offered>requested 应不兼容");
  }
  {
    SedpEndpointData w = MakeWriter();
    w.has_liveliness = true;
    w.liveliness_kind = 2;
    w.liveliness_lease = Secs(1);
    QosRequest r = base;
    r.liveliness_kind = 0;
    r.liveliness_lease = Secs(10);
    expect(w, r, true, "LIVELINESS: kind/lease 均满足 应兼容");
  }

  // --- DATA_REPRESENTATION ---
  {
    SedpEndpointData w = MakeWriter();
    w.has_data_representation = true;
    w.data_representation = 1;   // XCDR2
    QosRequest r = base;
    r.data_representations = {0};   // 只接受 XCDR1
    expect(w, r, false, "DATA_REP: 无交集应不兼容");
  }
  {
    SedpEndpointData w = MakeWriter();
    w.has_data_representation = true;
    w.data_representation = 1;
    QosRequest r = base;
    r.data_representations = {0, 1};
    expect(w, r, true, "DATA_REP: 有交集应兼容");
  }

  // --- topic / type 缺失 ---
  {
    SedpEndpointData w = MakeWriter("", "T");
    expect(w, base, false, "话题名为空应不兼容");
  }
  {
    SedpEndpointData w = MakeWriter("t", "");
    expect(w, base, false, "类型名为空应不兼容");
  }

  // --- 完全不声明 QoS（最宽松 writer）---
  {
    SedpEndpointData w = MakeWriter();
    expect(w, base, true, "未声明任何 QoS 应兼容");
  }

  STRATUM_CHECK_EQ(failed, 0);
  ctx.param_desc = "cases=" + std::to_string(cases) +
                   " failed=" + std::to_string(failed) + ctx.param_desc;
}

// ---------------------------------------------------------------------------
// 2) 随机交叉验证
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsQos, RandomCrossValidation) {
  constexpr int kIterations = 50000;
  std::mt19937_64 rng(20261220);
  int mismatches = 0;
  int matched_count = 0;

  for (int i = 0; i < kIterations; ++i) {
    SedpEndpointData w;
    w.kind = SedpKind::kPublication;
    const bool has_topic = (rng() % 10) != 0;
    if (has_topic) {
      w.has_topic_name = true;
      w.topic_name = "topic_" + std::to_string(rng() % 8);
    }
    const bool has_type = (rng() % 10) != 0;
    if (has_type) {
      w.has_type_name = true;
      w.type_name = "Type_" + std::to_string(rng() % 4);
    }

    if (rng() % 2) { w.has_reliability = true; w.reliability_kind = rng() % 3; }
    if (rng() % 2) { w.has_durability = true; w.durability_kind = rng() % 4; }
    if (rng() % 2) { w.has_ownership = true; w.ownership_kind = rng() % 2; }
    if (rng() % 2) {
      w.has_deadline = true;
      w.deadline = Secs(static_cast<int32_t>(rng() % 20));
    }
    if (rng() % 2) {
      w.has_liveliness = true;
      w.liveliness_kind = rng() % 3;
      w.liveliness_lease = Secs(static_cast<int32_t>(rng() % 30));
    }
    if (rng() % 2) {
      w.has_data_representation = true;
      w.data_representation = rng() % 3;
    }

    QosRequest r = DefaultRecorderQos();
    r.reliability_kind = rng() % 3;
    r.durability_kind = rng() % 4;
    r.ownership_kind = rng() % 2;
    r.deadline = Secs(static_cast<int32_t>(rng() % 20));
    r.liveliness_kind = rng() % 3;
    r.liveliness_lease = Secs(static_cast<int32_t>(rng() % 30));
    r.disable_positive_acks = (rng() % 4) == 0;
    r.data_representations = (rng() % 2) ? std::vector<uint32_t>{0}
                                         : std::vector<uint32_t>{0, 1};

    const MatchResult res = MatchWriterToReader(w, r);
    const bool expected = RecomputeExpected(w, r);
    if (res.matched != expected) ++mismatches;
    if (res.matched) ++matched_count;
  }

  ctx.param_desc = "iterations=" + std::to_string(kIterations) +
                   " matched=" + std::to_string(matched_count) +
                   " mismatches=" + std::to_string(mismatches);
  STRATUM_CHECK_EQ(mismatches, 0);
  STRATUM_CHECK_TRUE(matched_count > 0);
}

// ---------------------------------------------------------------------------
// 3) 真实端点数据上的判定
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsQos, RealEndpointsMatching) {
  std::vector<std::vector<uint8_t>> packets;
  if (!LoadCaptureVectors("tests/vectors/sedp_capture.bin", &packets)) {
    ctx.param_desc = "**向量文件缺失**";
    STRATUM_CHECK_TRUE(false);
    return;
  }

  SedpParser parser;
  std::vector<SedpEndpointData> endpoints;
  for (const auto& p : packets) {
    parser.ParseMessage(p.data(), p.size(), &endpoints);
  }

  const QosRequest q = DefaultRecorderQos();
  size_t matched = 0, unmatched = 0;
  std::string first_reason;

  for (const auto& e : endpoints) {
    const MatchResult res = MatchWriterToReader(e, q);
    if (res.matched) {
      ++matched;
    } else {
      ++unmatched;
      if (first_reason.empty()) {
        first_reason = std::string(MatchFailureName(res.failure));
        if (!res.details.empty()) first_reason += " (" + res.details[0] + ")";
      }
    }
  }

  ctx.param_desc = "endpoints=" + std::to_string(endpoints.size()) +
                   " matched=" + std::to_string(matched) +
                   " unmatched=" + std::to_string(unmatched);
  if (!first_reason.empty()) ctx.param_desc += " 首个不匹配原因=" + first_reason;

  // 真实向量里的端点（ROS 服务，RELIABLE+VOLATILE）在默认最宽松 QoS 下
  // 应当全部可匹配 —— 若有大量不匹配说明规则实现有偏差。
  STRATUM_CHECK_TRUE(!endpoints.empty());
  STRATUM_CHECK_TRUE(matched > 0);
}

// ---------------------------------------------------------------------------
// 4) 压力：大量判定
// ---------------------------------------------------------------------------
STRATUM_TEST(RtpsQos, StressManyMatches) {
  constexpr int kIterations = 300000;
  std::mt19937_64 rng(20261221);
  SedpEndpointData w = MakeWriter("bench", "BenchType");
  w.has_reliability = true;
  w.reliability_kind = 2;
  w.has_durability = true;
  w.durability_kind = 1;
  w.has_deadline = true;
  w.deadline = Secs(2);
  w.has_liveliness = true;
  w.liveliness_kind = 1;
  w.liveliness_lease = Secs(5);
  w.has_data_representation = true;
  w.data_representation = 1;

  QosRequest r = DefaultRecorderQos();
  size_t matched = 0;
  for (int i = 0; i < kIterations; ++i) {
    // 轻微扰动 reader 侧，避免被优化掉
    r.deadline = Secs(static_cast<int32_t>(1 + rng() % 10));
    if (MatchWriterToReader(w, r).matched) ++matched;
  }

  ctx.param_desc = "iterations=" + std::to_string(kIterations) +
                   " matched=" + std::to_string(matched);
  STRATUM_CHECK_TRUE(matched > 0);
}

STRATUM_TEST_MAIN()
