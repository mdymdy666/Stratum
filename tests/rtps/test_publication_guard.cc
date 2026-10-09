// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 回归测试：SEDP Publication 构造失败必须**显式报错**，不能静默跳过
//
// ===========================================================================
// 为什么需要这个测试
// ===========================================================================
// 真实事故（见 tests/review/test_topic_meta_type_name.cc 的完整根因链）：
//   RtpsWriter::Publish 拿到空的 type_name 时，BuildPublication 返回空
//   vector，旧代码 `if (!pub.empty()) { ... }` 直接跳过发送 ——
//   **不报错、不计数、不影响返回值**。于是：
//     · 本端以为发布成功（数据照发）
//     · 对端从未收到 Publication，自然不回 SEDP Subscription
//     · 用户数据全被发到 metatraffic 端口，对端 DataReader 收到 0 条
//     · 两端日志都干干净净
//   这个"静默"让排查绕了很远（先把 QoS、KEY_HASH、DATA_REPRESENTATION
//   都怀疑了一遍）。本测试把行为固化为：构造失败 → 返回错误 + 计数。
//
// 本测试不依赖任何对端，只验证"契约"。
// ===========================================================================

#include <string>

#include "stratum/protocol/rtps/rtps_writer.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::protocol::rtps;

namespace {
// 用非默认端口，避免与其它 RTPS 测试/ROS 2 daemon 抢端口。
// domain 7 → SPDP 组播 7400 + 7*250 = 9150。
constexpr uint32_t kTestDomain = 7;
constexpr uint16_t kTestMetaPort = 19310;
constexpr uint16_t kTestUserPort = 19311;
}  // namespace

STRATUM_TEST(PublicationGuard, EmptyTypeNameFailsLoudly) {
  RtpsWriter writer;
  RtpsWriterConfig cfg;
  cfg.domain_id = kTestDomain;
  cfg.participant_name = "publication_guard_test";
  cfg.metatraffic_port = kTestMetaPort;
  cfg.user_port = kTestUserPort;

  STRATUM_CHECK_OK(writer.Open(cfg));
  STRATUM_CHECK_OK(writer.Start());

  // 1) 空 type_name：必须失败，且是参数错误（不是静默成功）
  const Status empty_type =
      writer.Publish("hello_world_topic", "", "payload-bytes", 0);
  ctx.param_desc = "status=" + empty_type.ToString();
  STRATUM_CHECK_TRUE(!empty_type.ok());
  STRATUM_CHECK_EQ(static_cast<int>(empty_type.code()),
                   static_cast<int>(StatusCode::kInvalidArgument));
  STRATUM_CHECK_EQ(writer.GetStats().publications_build_failed,
                   static_cast<uint64_t>(1));
  // 失败信息必须能指出是哪个 (topic, type)，否则等于没报
  STRATUM_CHECK_TRUE(empty_type.ToString().find("hello_world_topic") !=
                     std::string::npos);

  // 2) 空 topic_name 在入口就被参数检查拒绝（不进入公告构造，
  //    因此不计入 publications_build_failed）
  const Status empty_topic = writer.Publish("", "HelloWorld", "payload-bytes", 0);
  STRATUM_CHECK_TRUE(!empty_topic.ok());
  STRATUM_CHECK_EQ(static_cast<int>(empty_topic.code()),
                   static_cast<int>(StatusCode::kInvalidArgument));
  STRATUM_CHECK_EQ(writer.GetStats().publications_build_failed,
                   static_cast<uint64_t>(1));

  // 3) 合法的 (topic, type)：不能再计入构造失败。
  //    此时还没有任何对端，数据会进 pending 缓存，返回 kNotFound 属于预期。
  const Status good =
      writer.Publish("hello_world_topic", "HelloWorld", "payload-bytes", 0);
  ctx.param_desc = "good_status=" + good.ToString();
  STRATUM_CHECK_TRUE(good.ok() || good.IsNotFound());
  STRATUM_CHECK_EQ(writer.GetStats().publications_build_failed,
                   static_cast<uint64_t>(1));
  // 合法路径必须真的构造出公告（publications_announced 至少一次）
  STRATUM_CHECK_TRUE(writer.GetStats().publications_announced >= 1);

  STRATUM_CHECK_OK(writer.Stop());
}

// 同一 (topic, type) 反复发布只公告一次，且不会重复计入
STRATUM_TEST(PublicationGuard, RepeatedPublishAnnouncesOnce) {
  RtpsWriter writer;
  RtpsWriterConfig cfg;
  cfg.domain_id = kTestDomain;
  cfg.participant_name = "publication_guard_test2";
  cfg.metatraffic_port = static_cast<uint16_t>(kTestMetaPort + 10);
  cfg.user_port = static_cast<uint16_t>(kTestUserPort + 10);

  STRATUM_CHECK_OK(writer.Open(cfg));
  STRATUM_CHECK_OK(writer.Start());

  for (int i = 0; i < 5; ++i) {
    const Status s =
        writer.Publish("hello_world_topic", "HelloWorld", "payload", 0);
    STRATUM_CHECK_TRUE(s.ok() || s.IsNotFound());
  }
  STRATUM_CHECK_EQ(writer.GetStats().publications_announced,
                   static_cast<uint64_t>(1));
  STRATUM_CHECK_EQ(writer.GetStats().publications_build_failed,
                   static_cast<uint64_t>(0));

  STRATUM_CHECK_OK(writer.Stop());
}

STRATUM_TEST_MAIN()
