// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// TimeKey 编解码测试
//
// 重点验证一个不变量：**字节序 == 时间序**。
// 这个不变量如果被破坏，整个时间轴范围扫描都会失效，且症状隐蔽
// （查询结果"差不多对"但顺序错乱）。所以这里用大量数据验证它。

// 标准库头文件必须最先包含（避免被卷入自定义命名空间）
#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "stratum/storage/time_key.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::storage;

// ---------------------------------------------------------------------------
// 基础：编解码往返
// ---------------------------------------------------------------------------
STRATUM_TEST(TimeKey, EncodeDecodeRoundTrip) {
  const std::vector<TimeKey> cases = {
      {0, 0, 0},
      {1, 1, 1},
      {42, 1700000000000000000ULL, 7},
      {0xFFFFFFFF, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFF},
      {12345, 1, 0},
  };

  for (const auto& expected : cases) {
    ctx.param_desc = expected.DebugString();
    const std::string encoded = TimeKeyCodec::Encode(expected);
    STRATUM_CHECK_EQ(encoded.size(), kTimeKeySize);

    TimeKey decoded;
    STRATUM_CHECK_TRUE(TimeKeyCodec::Decode(encoded, &decoded));
    STRATUM_CHECK_EQ(decoded.topic_id, expected.topic_id);
    STRATUM_CHECK_EQ(decoded.timestamp_ns, expected.timestamp_ns);
    STRATUM_CHECK_EQ(decoded.seq_in_ts, expected.seq_in_ts);
    STRATUM_CHECK_TRUE(decoded == expected);
  }
}

// ---------------------------------------------------------------------------
// 关键不变量：字节序 == 数值序
//
// 用随机数据穷举验证：对任意两个键，其"逻辑序"与"编码后字节序"一致。
// ---------------------------------------------------------------------------
STRATUM_TEST(TimeKey, ByteOrderMatchesLogicalOrder) {
  std::mt19937_64 rng(12345);  // 固定种子保证可复现

  constexpr int kCount = 2000;
  std::vector<TimeKey> keys;
  keys.reserve(kCount);
  for (int i = 0; i < kCount; ++i) {
    // 刻意使用小范围，制造大量相等与相邻的情况
    keys.push_back(TimeKey{
        static_cast<uint32_t>(rng() % 5),
        static_cast<uint64_t>(rng() % 1000),
        static_cast<uint32_t>(rng() % 7),
    });
  }

  // 按逻辑序排序
  std::vector<TimeKey> logical = keys;
  std::sort(logical.begin(), logical.end());

  // 按编码后字节序排序
  std::vector<std::string> encoded;
  encoded.reserve(kCount);
  for (const auto& k : logical) {
    encoded.push_back(TimeKeyCodec::Encode(k));
  }
  std::vector<std::string> byte_sorted = encoded;
  std::sort(byte_sorted.begin(), byte_sorted.end());

  // 两者必须完全一致
  ctx.param_desc = "count=" + std::to_string(kCount);
  for (size_t i = 0; i < encoded.size(); ++i) {
    STRATUM_CHECK_EQ(encoded[i], byte_sorted[i]);
  }
}

// ---------------------------------------------------------------------------
// 关键不变量：同一时间戳的多条消息不互相覆盖
// ---------------------------------------------------------------------------
STRATUM_TEST(TimeKey, SameTimestampProducesDistinctKeys) {
  const uint32_t topic = 7;
  const uint64_t ts = 1700000000123456789ULL;

  std::set<std::string> distinct;
  constexpr uint32_t kSeqs = 1000;
  for (uint32_t seq = 0; seq < kSeqs; ++seq) {
    distinct.insert(TimeKeyCodec::Encode(topic, ts, seq));
  }

  ctx.param_desc = "same-ts=" + std::to_string(kSeqs);
  STRATUM_CHECK_EQ(distinct.size(), static_cast<size_t>(kSeqs));
}

// ---------------------------------------------------------------------------
// 范围构造：话题全范围
// ---------------------------------------------------------------------------
STRATUM_TEST(TimeKey, TopicRangeCoversExactlyThatTopic) {
  const uint32_t topic = 3;

  std::string begin;
  std::string end;
  TimeKeyCodec::MakeTopicRange(topic, &begin, &end);

  // 该话题的键必须落在 [begin, end)
  STRATUM_CHECK_TRUE(begin <= TimeKeyCodec::Encode(topic, 0, 0));
  STRATUM_CHECK_TRUE(TimeKeyCodec::Encode(topic, 0, 0) < end);
  STRATUM_CHECK_TRUE(TimeKeyCodec::Encode(topic, UINT64_MAX, UINT32_MAX) < end);

  // 相邻话题的键不得落入
  const std::string prev_topic_key = TimeKeyCodec::Encode(topic - 1, UINT64_MAX, UINT32_MAX);
  const std::string next_topic_key = TimeKeyCodec::Encode(topic + 1, 0, 0);
  STRATUM_CHECK_TRUE(prev_topic_key < begin);
  STRATUM_CHECK_TRUE(next_topic_key >= end);
}

// ---------------------------------------------------------------------------
// 范围构造：时间区间
// ---------------------------------------------------------------------------
STRATUM_TEST(TimeKey, TimeRangeIsHalfOpen) {
  const uint32_t topic = 5;
  const uint64_t t0 = 1000;
  const uint64_t t1 = 2000;

  std::string begin;
  std::string end;
  TimeKeyCodec::MakeTimeRange(topic, t0, t1, &begin, &end);

  // t0 时刻的所有键都在范围内
  STRATUM_CHECK_TRUE(begin <= TimeKeyCodec::Encode(topic, t0, 0));
  STRATUM_CHECK_TRUE(TimeKeyCodec::Encode(topic, t0, 999) < end);

  // t1 - 1 在内，t1 在外（半开区间）
  STRATUM_CHECK_TRUE(TimeKeyCodec::Encode(topic, t1 - 1, 0) < end);
  STRATUM_CHECK_TRUE(TimeKeyCodec::Encode(topic, t1, 0) >= end);

  // t0 之前的不在范围内
  STRATUM_CHECK_TRUE(TimeKeyCodec::Encode(topic, t0 - 1, 0) < begin);
}

// ---------------------------------------------------------------------------
// 定点查询上界：所有 <= ts 的键都必须小于上界
// ---------------------------------------------------------------------------
STRATUM_TEST(TimeKey, PrefixRangeUpToIsExclusiveUpperBound) {
  const uint32_t topic = 9;
  const uint64_t ts = 5000;

  std::string begin;
  std::string end;
  TimeKeyCodec::MakePrefixRangeUpTo(topic, ts, &begin, &end);

  // ts 时刻的所有可能 seq 都必须 < end
  for (uint32_t seq : {0u, 1u, 100u, UINT32_MAX}) {
    ctx.param_desc = "seq=" + std::to_string(seq);
    STRATUM_CHECK_TRUE(TimeKeyCodec::Encode(topic, ts, seq) < end);
  }

  // ts + 1 时刻的键不得 < end
  STRATUM_CHECK_TRUE(TimeKeyCodec::Encode(topic, ts + 1, 0) >= end);
}

// ---------------------------------------------------------------------------
// 边界：时间戳与话题号的极值
// ---------------------------------------------------------------------------
STRATUM_TEST(TimeKey, ExtremeValuesHandledCorrectly) {
  // 话题号上界：MakeTopicRange 应返回空上界（表示无上界）
  std::string begin;
  std::string end;
  TimeKeyCodec::MakeTopicRange(0xFFFFFFFFu, &begin, &end);
  ctx.param_desc = "topic=UINT32_MAX";
  STRATUM_CHECK_FALSE(begin.empty());
  STRATUM_CHECK_TRUE(end.empty());

  // 时间戳 0 与最大值都能往返
  const TimeKey zero{1, 0, 0};
  const TimeKey max_ts{1, UINT64_MAX, 0};
  TimeKey out;
  STRATUM_CHECK_TRUE(TimeKeyCodec::Decode(TimeKeyCodec::Encode(zero), &out));
  STRATUM_CHECK_TRUE(out == zero);
  STRATUM_CHECK_TRUE(TimeKeyCodec::Decode(TimeKeyCodec::Encode(max_ts), &out));
  STRATUM_CHECK_TRUE(out == max_ts);
}

// ---------------------------------------------------------------------------
// 健壮性：非法输入必须被拒绝，且不修改输出
// ---------------------------------------------------------------------------
STRATUM_TEST(TimeKey, DecodeRejectsMalformedInput) {
  TimeKey sentinel{999, 999, 999};

  // 长度不对
  {
    TimeKey out = sentinel;
    ctx.param_desc = "too-short";
    STRATUM_CHECK_FALSE(TimeKeyCodec::Decode(std::string(19, 'x'), &out));
    STRATUM_CHECK_TRUE(out == sentinel);
  }
  {
    TimeKey out = sentinel;
    ctx.param_desc = "too-long";
    STRATUM_CHECK_FALSE(TimeKeyCodec::Decode(std::string(21, 'x'), &out));
    STRATUM_CHECK_TRUE(out == sentinel);
  }

  // magic 不对
  {
    std::string bad = TimeKeyCodec::Encode(TimeKey{1, 2, 3});
    bad[0] = '\x00';
    TimeKey out = sentinel;
    ctx.param_desc = "bad-magic";
    STRATUM_CHECK_FALSE(TimeKeyCodec::Decode(bad, &out));
    STRATUM_CHECK_TRUE(out == sentinel);
  }

  // 空输入
  {
    TimeKey out = sentinel;
    ctx.param_desc = "empty";
    STRATUM_CHECK_FALSE(TimeKeyCodec::Decode(std::string(), &out));
  }
}

// ---------------------------------------------------------------------------
// 窥探函数与完整解码必须一致
// ---------------------------------------------------------------------------
STRATUM_TEST(TimeKey, PeekMatchesDecode) {
  const TimeKey key{77, 123456789012345ULL, 42};
  const std::string encoded = TimeKeyCodec::Encode(key);

  TimeKey decoded;
  STRATUM_CHECK_TRUE(TimeKeyCodec::Decode(encoded, &decoded));
  STRATUM_CHECK_EQ(TimeKeyCodec::PeekTopicId(encoded), decoded.topic_id);
  STRATUM_CHECK_EQ(TimeKeyCodec::PeekTimestamp(encoded), decoded.timestamp_ns);
  STRATUM_CHECK_EQ(TimeKeyCodec::PeekSeqInTs(encoded), decoded.seq_in_ts);
}

// ---------------------------------------------------------------------------
// LooksLikeTimeKey 只对合法键返回真
// ---------------------------------------------------------------------------
STRATUM_TEST(TimeKey, LooksLikeTimeKeyIsStrict) {
  STRATUM_CHECK_TRUE(TimeKeyCodec::LooksLikeTimeKey(
                          TimeKeyCodec::Encode(TimeKey{1, 2, 3})));
  STRATUM_CHECK_FALSE(TimeKeyCodec::LooksLikeTimeKey(""));
  STRATUM_CHECK_FALSE(TimeKeyCodec::LooksLikeTimeKey("short"));
  STRATUM_CHECK_FALSE(TimeKeyCodec::LooksLikeTimeKey(std::string(20, '\0')));
}

STRATUM_TEST_MAIN()
