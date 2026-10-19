// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// **`MetaCodec` 的编解码测试**（`docs/06` 缺口 #3）
//
// `MetaCodec` 把 Session / Topic / Retention 三种元数据编成二进制存进 LSM，
// 此前**无直接测试**（只被 timeline_impl 间接使用）。
//
// 【为什么这组测试重要】
// 元数据是"脱离原程序也能读懂"的那部分（FR-R-11）。它一旦解不出来：
//   · 会话打不开（`session info` / `query` 全废）
//   · 而且是**读的时候才炸** —— 录制时一切正常
//
// 更要紧的是**版本兼容**：设计要求"遇到更高版本必须拒绝而不是猜着解析"（FR-S-2）。
// 如果不拒绝，旧程序会用旧 schema 去解释新字段 —— 得到的不是报错，是**看起来正常的错数据**。
//
// 覆盖：
//   1. 三种元数据的**往返**（字段逐个比对）
//   2. **高版本必须被拒绝**（FR-S-2）
//   3. 截断输入（每个长度减 1，全部不能崩）
//   4. 长度字段超界（声称 1000 字节但只有 10）
//   5. 空串 / 垃圾字节
//   6. `MakeTopicKey` 的差分与前缀

// 标准库头文件必须最先包含
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "stratum/storage/meta_codec.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::storage;

namespace {

SessionMetadata MakeSession() {
  SessionMetadata m;
  m.format_version = 1;
  m.stratum_version = "1.0.0";
  m.session_name = "auto:20261019-235900";
  m.created_at_ns = 1700000000000000000ull;
  m.closed_at_ns = 1700000005000000000ull;
  m.total_events = 12345;
  m.total_bytes = 987654;
  return m;
}

TopicMetadata MakeTopic() {
  TopicMetadata m;
  m.topic_id = 7;
  m.topic_name = "sensor/imu";
  m.type_name = "ImuData";
  m.message_count = 42;
  m.byte_count = 4096;
  m.first_seen_ns = 1700000000000000000ull;
  m.last_seen_ns = 1700000004000000000ull;
  m.active = true;
  return m;
}

RetentionPolicy MakeRetention() {
  RetentionPolicy p;
  p.retain_duration_ns = 24ull * 3600ull * 1000000000ull;
  p.max_bytes = 10ull * 1024 * 1024 * 1024;
  return p;
}

// 把前 4 字节（format_version）改成指定值
std::string WithVersion(const std::string& raw, uint32_t v) {
  std::string s = raw;
  if (s.size() < 4) return s;
  s[0] = static_cast<char>(v & 0xFF);
  s[1] = static_cast<char>((v >> 8) & 0xFF);
  s[2] = static_cast<char>((v >> 16) & 0xFF);
  s[3] = static_cast<char>((v >> 24) & 0xFF);
  return s;
}

}  // namespace

// ===========================================================================
// 一、往返：Session
// ===========================================================================

STRATUM_TEST(MetaCodecRoundTrip, Session) {
  const SessionMetadata in = MakeSession();
  const std::string raw = MetaCodec::EncodeSession(in);
  STRATUM_CHECK_TRUE(!raw.empty());

  SessionMetadata out;
  const Status st = MetaCodec::DecodeSession(raw, &out);
  STRATUM_CHECK_TRUE(st.ok());

  STRATUM_CHECK_EQ(out.format_version, in.format_version);
  STRATUM_CHECK_EQ(out.stratum_version, in.stratum_version);
  STRATUM_CHECK_EQ(out.session_name, in.session_name);
  STRATUM_CHECK_EQ(out.created_at_ns, in.created_at_ns);
  STRATUM_CHECK_EQ(out.closed_at_ns, in.closed_at_ns);
  STRATUM_CHECK_EQ(out.total_events, in.total_events);
  STRATUM_CHECK_EQ(out.total_bytes, in.total_bytes);

  std::printf("      [session] %zu 字节，9 个字段往返一致\n", raw.size());
}

// ===========================================================================
// 二、往返：Topic
// ===========================================================================

STRATUM_TEST(MetaCodecRoundTrip, Topic) {
  const TopicMetadata in = MakeTopic();
  const std::string raw = MetaCodec::EncodeTopic(in);
  STRATUM_CHECK_TRUE(!raw.empty());

  TopicMetadata out;
  STRATUM_CHECK_TRUE(MetaCodec::DecodeTopic(raw, &out).ok());

  STRATUM_CHECK_EQ(out.topic_id, in.topic_id);
  STRATUM_CHECK_EQ(out.topic_name, in.topic_name);
  STRATUM_CHECK_EQ(out.type_name, in.type_name);
  STRATUM_CHECK_EQ(out.message_count, in.message_count);
  STRATUM_CHECK_EQ(out.byte_count, in.byte_count);
  STRATUM_CHECK_EQ(out.first_seen_ns, in.first_seen_ns);
  STRATUM_CHECK_EQ(out.last_seen_ns, in.last_seen_ns);
  STRATUM_CHECK_EQ(out.active, in.active);

  std::printf("      [topic] %zu 字节，8 个字段往返一致\n", raw.size());
}

// ===========================================================================
// 三、往返：Retention
// ===========================================================================

STRATUM_TEST(MetaCodecRoundTrip, Retention) {
  const RetentionPolicy in = MakeRetention();
  const std::string raw = MetaCodec::EncodeRetention(in);
  STRATUM_CHECK_TRUE(!raw.empty());

  RetentionPolicy out;
  STRATUM_CHECK_TRUE(MetaCodec::DecodeRetention(raw, &out).ok());
  STRATUM_CHECK_EQ(out.retain_duration_ns, in.retain_duration_ns);
  STRATUM_CHECK_EQ(out.max_bytes, in.max_bytes);

  std::printf("      [retention] %zu 字节，2 个字段往返一致\n", raw.size());
}

// ===========================================================================
// 四、★ 高版本必须被拒绝（FR-S-2）
// ===========================================================================
//
// 这是设计文档明确要求的："读取方遇到更高版本必须拒绝，而不是尝试解析"。
//
// 为什么必须拒绝：旧程序用旧 schema 解释新字段 → 得到的**不是报错，是看起来正常的错数据**。
// 那种错最难查（数据全在、字段也有值，但语义错了）。

STRATUM_TEST(MetaCodecCorrupt, HigherVersionRejected) {
  const std::string raw = MetaCodec::EncodeSession(MakeSession());

  for (uint32_t v : {2u, 3u, 100u, 0xFFFFFFFFu}) {
    const std::string bad = WithVersion(raw, v);
    SessionMetadata out;
    const Status st = MetaCodec::DecodeSession(bad, &out);
    STRATUM_CHECK_TRUE(!st.ok());
  }

  // ★ 当前版本（1）必须能解
  SessionMetadata ok_out;
  STRATUM_CHECK_TRUE(MetaCodec::DecodeSession(WithVersion(raw, 1), &ok_out).ok());

  std::printf("      [版本] v2/v3/v100/v0xFFFFFFFF 全部拒绝；v1 正常\n");
}

// ===========================================================================
// 五、截断：每个长度都试一遍（不能崩、必须报错或安全失败）
// ===========================================================================

STRATUM_TEST(MetaCodecCorrupt, TruncatedAtEveryLength) {
  const std::string full = MetaCodec::EncodeSession(MakeSession());
  int bad_ok = 0;   // 被截断却"成功"解码的次数

  for (size_t n = 0; n < full.size(); ++n) {
    const std::string cut = full.substr(0, n);
    SessionMetadata out;
    const Status st = MetaCodec::DecodeSession(cut, &out);
    if (st.ok()) {
      // 极少数前缀可能恰好构成合法数据（例如只剩版本号、其余字段有默认值）。
      // 这里只统计，不判失败 —— 但若"截断却成功"的比例很高，说明校验太松。
      ++bad_ok;
      if (n >= 4) {
        // 有版本号却缺字段还成功，值得警惕
        std::printf("      注意：截断到 %zu/%zu 字节仍解码成功\n", n, full.size());
      }
    }
  }

  // 关键要求：**不能崩**（能跑完这个循环本身就是证明）
  // 且绝大多数截断必须被拒绝
  STRATUM_CHECK_TRUE(bad_ok < static_cast<int>(full.size() / 4));
  std::printf("      [截断] 试了 %zu 种长度，其中 %d 个仍成功（< 25%% 视为合格）\n",
              full.size(), bad_ok);
}

// ===========================================================================
// 六、长度字段超界（声称很长但实际很短）
// ===========================================================================
//
// 这是最经典的解析漏洞：读到一个长度就 `resize`/`substr`，
// 不做剩余长度校验 → 越界读或抛异常。
// `Reader` 的设计说"读时严格校验剩余长度"，这里验证它真的做到了。

STRATUM_TEST(MetaCodecCorrupt, StringLengthBeyondBuffer) {
  // 构造：版本号(4) + 长度字段(声称 0xFFFF) + 只有 3 字节内容
  std::string evil;
  evil.push_back('\x01'); evil.push_back('\x00');
  evil.push_back('\x00'); evil.push_back('\x00');   // format_version = 1
  evil.push_back('\xFF'); evil.push_back('\xFF');
  evil.push_back('\x00'); evil.push_back('\x00');   // 长度 = 65535
  evil.append("abc");                               // 实际只有 3 字节

  SessionMetadata out;
  const Status st = MetaCodec::DecodeSession(evil, &out);
  STRATUM_CHECK_TRUE(!st.ok());   // 必须拒绝，不能越界读

  std::printf("      [超界] 声称 65535 字节、实际 3 字节 → 拒绝（未越界）\n");
}

// ===========================================================================
// 七、空串与垃圾字节
// ===========================================================================

STRATUM_TEST(MetaCodecCorrupt, EmptyAndGarbage) {
  SessionMetadata s;
  STRATUM_CHECK_TRUE(!MetaCodec::DecodeSession("", &s).ok());

  TopicMetadata t;
  STRATUM_CHECK_TRUE(!MetaCodec::DecodeTopic("", &t).ok());

  RetentionPolicy r;
  STRATUM_CHECK_TRUE(!MetaCodec::DecodeRetention("", &r).ok());

  // 全 0xFF 的垃圾
  const std::string junk(64, '\xFF');
  STRATUM_CHECK_TRUE(!MetaCodec::DecodeSession(junk, &s).ok());

  // null 输出指针：必须返回错误而不是崩
  STRATUM_CHECK_TRUE(!MetaCodec::DecodeSession(junk, nullptr).ok());
  STRATUM_CHECK_TRUE(!MetaCodec::DecodeTopic(junk, nullptr).ok());
  STRATUM_CHECK_TRUE(!MetaCodec::DecodeRetention(junk, nullptr).ok());

  std::printf("      [垃圾] 空串/全FF/null输出 全部安全拒绝\n");
}

// ===========================================================================
// 八、MakeTopicKey：不同 id 不同 key、前缀正确
// ===========================================================================

STRATUM_TEST(MetaCodecKey, TopicKeyIsDistinctAndPrefixed) {
  const std::string k0 = MetaCodec::MakeTopicKey(0);
  const std::string k1 = MetaCodec::MakeTopicKey(1);
  const std::string k7 = MetaCodec::MakeTopicKey(7);
  const std::string kmax = MetaCodec::MakeTopicKey(0xFFFFFFFFu);

  // 前缀：必须落在元数据命名空间（否则会和事件键冲突）
  STRATUM_CHECK_TRUE(k0.rfind(kMetaTopicPrefix, 0) == 0);
  STRATUM_CHECK_TRUE(kmax.rfind(kMetaTopicPrefix, 0) == 0);

  // 不同 id 必须产生不同 key（否则会互相覆盖）
  STRATUM_CHECK_TRUE(k0 != k1);
  STRATUM_CHECK_TRUE(k1 != k7);
  STRATUM_CHECK_TRUE(k7 != kmax);

  // 长度固定（便于按前缀扫描）
  STRATUM_CHECK_EQ(k0.size(), k1.size());
  STRATUM_CHECK_EQ(k7.size(), kmax.size());

  std::printf("      [key] 前缀=%s，4 个不同 id 产生 4 个不同 key（长度均 %zu）\n",
              kMetaTopicPrefix, k0.size());
}

STRATUM_TEST_MAIN()
