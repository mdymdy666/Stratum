// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 话题类型定义（typedef/<topic_name>）的存取测试
//
// 【这套测试的核心价值：必须用 ≥2 个话题】
//
// 上一轮实现 `types register` 时踩过一个坑：`LoadTopicsLocked` 里读回类型
// 定义的代码被错误地放进了 `for` 循环**内部**（`return` 落在循环体里），
// 于是**第一次迭代就返回** —— 只有第一个话题能读到定义。
//
// 这个 bug 在**单话题**会话上完全看不出来。所以本文件的核心用例一律
// 使用**多个话题**，并逐个断言。
//
// 覆盖三类（项目测试标准）：
//   · 大数据量 —— 100 个话题各带定义
//   · 随机     —— 多种子随机话题数 / 随机定义内容
//   · 真实场景 —— ① 注册后查询链路可见 ② 覆盖更新 ③ 与话题元数据一致性

// 标准库头文件必须最先包含
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"
#include "test_isolation.h"

namespace fs = std::filesystem;

using namespace stratum;

namespace {

std::string TempDir(const char* tag) {
  const std::string d = std::string("/tmp/stratum_typedef_") + tag + "_" + UniqueSuffix();
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

TopicMetadata MakeTopic(const std::string& name, const std::string& type = "T") {
  TopicMetadata m;
  m.topic_name = name;
  m.type_name = type;
  return m;
}

Event MakeEvent(uint32_t topic_id, uint32_t seq, uint64_t src_ns, size_t len) {
  Event e;
  e.topic_id = topic_id;
  e.seq = seq;
  e.seq_in_ts = 0;
  e.source_timestamp_ns = src_ns;
  e.reception_timestamp_ns = src_ns + 1000;
  e.payload = std::string(len, 't');
  return e;
}

// 注册 n 个话题，返回它们的 topic_id（顺序与名字一致）
std::vector<uint32_t> RegisterN(TimelineImplV2* tl, int n, const char* prefix,
                                const std::string& type = "T") {
  std::vector<uint32_t> ids;
  for (int i = 0; i < n; ++i) {
    uint32_t id = 0;
    STRATUM_CHECK_OK(tl->RegisterTopic(
        MakeTopic(std::string(prefix) + "/" + std::to_string(i), type), &id));
    ids.push_back(id);
  }
  return ids;
}

}  // namespace

// ===========================================================================
// 一、基础契约
// ===========================================================================

STRATUM_TEST(TypeDefinition, SetAndGetRoundTrip) {
  const std::string dir = TempDir("basic");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t id = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("sensor/a"), &id));

  const std::string def = "{\"temperature_c\":\"double\",\"valid\":\"bool\"}";
  STRATUM_CHECK_OK(tl.SetTopicTypeDefinition("sensor/a", def));

  std::string got;
  STRATUM_CHECK_OK(tl.GetTopicTypeDefinition("sensor/a", &got));
  STRATUM_CHECK_EQ(got, def);
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(TypeDefinition, UnknownTopicIsNotFound) {
  const std::string dir = TempDir("notfound");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  Status s1 = tl.SetTopicTypeDefinition("no/such/topic", "{}");
  STRATUM_CHECK_TRUE(s1.code() == StatusCode::kNotFound);

  std::string got;
  Status s2 = tl.GetTopicTypeDefinition("no/such/topic", &got);
  STRATUM_CHECK_TRUE(s2.code() == StatusCode::kNotFound);
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(TypeDefinition, NullOutputIsInvalidArgument) {
  const std::string dir = TempDir("nullout");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t id = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/n"), &id));

  Status s = tl.GetTopicTypeDefinition("t/n", nullptr);
  STRATUM_CHECK_TRUE(s.code() == StatusCode::kInvalidArgument);
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(TypeDefinition, GetOnTopicWithoutDefinitionIsEmptyNotError) {
  const std::string dir = TempDir("nodef");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t id = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/nd"), &id));

  std::string got = "sentinel";
  Status s = tl.GetTopicTypeDefinition("t/nd", &got);
  // 话题存在但没有定义 —— 这是正常状态，不是错误
  STRATUM_CHECK_TRUE(s.ok() || s.IsNotFound());
  STRATUM_CHECK_TRUE(got.empty());
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(TypeDefinition, NotOpenIsShutdownInProgress) {
  const std::string dir = TempDir("notopen");
  TimelineImplV2 tl;   // 未 Open
  std::string got;
  Status s1 = tl.GetTopicTypeDefinition("t/x", &got);
  STRATUM_CHECK_TRUE(s1.code() == StatusCode::kShutdownInProgress);
  Status s2 = tl.SetTopicTypeDefinition("t/x", "{}");
  STRATUM_CHECK_TRUE(s2.code() == StatusCode::kShutdownInProgress);
}

// ===========================================================================
// 二、★ 核心：多话题逐话题读回（单话题会掩盖 bug）
// ===========================================================================

STRATUM_TEST(TypeDefinition, ManyTopicsEachKeepsOwnDefinition) {
  // 【本用例是这套测试的核心】
  // 若读回逻辑被错误放进循环内的 return 之前，只有**第一个**话题能读到定义。
  // 因此这里注册 8 个话题、各设不同定义，逐个断言。
  const std::string dir = TempDir("many");
  const int kN = 8;
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  const std::vector<uint32_t> ids = RegisterN(&tl, kN, "many");

  for (int i = 0; i < kN; ++i) {
    const std::string def = "{\"index\":" + std::to_string(i) + "}";
    STRATUM_CHECK_OK(tl.SetTopicTypeDefinition("many/" + std::to_string(i), def));
  }

  // 逐个校验（顺序也校验：不能张冠李戴）
  for (int i = 0; i < kN; ++i) {
    std::string got;
    STRATUM_CHECK_OK(tl.GetTopicTypeDefinition("many/" + std::to_string(i), &got));
    STRATUM_CHECK_EQ(got, std::string("{\"index\":") + std::to_string(i) + "}");
  }

  // ListTopics 返回的元数据里也必须带上各自的定义
  std::vector<TopicMetadata> topics;
  STRATUM_CHECK_OK(tl.ListTopics(&topics));
  STRATUM_CHECK_EQ(topics.size(), static_cast<size_t>(kN));
  int checked = 0;
  for (const auto& t : topics) {
    // 话题名形如 many/<i>
    const size_t slash = t.topic_name.find('/');
    if (slash == std::string::npos) continue;
    const int idx = std::atoi(t.topic_name.substr(slash + 1).c_str());
    STRATUM_CHECK_EQ(t.type_definition,
                     std::string("{\"index\":") + std::to_string(idx) + "}");
    ++checked;
  }
  STRATUM_CHECK_EQ(checked, kN);
  std::printf("      [核心] %d 个话题各自的定义都正确读回（含 ListTopics 元数据）\n", kN);
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(TypeDefinition, ManyTopicsSurviveReopen) {
  // 重开后仍逐话题正确 —— 这条同时验证"落盘"与"逐话题读回"
  const std::string dir = TempDir("reopen");
  const int kN = 6;
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    (void)RegisterN(&tl, kN, "rp");
    for (int i = 0; i < kN; ++i) {
      STRATUM_CHECK_OK(tl.SetTopicTypeDefinition(
          "rp/" + std::to_string(i), "{\"n\":" + std::to_string(i * 10) + "}"));
    }
    STRATUM_CHECK_OK(tl.Close());
  }
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    for (int i = 0; i < kN; ++i) {
      std::string got;
      STRATUM_CHECK_OK(tl.GetTopicTypeDefinition("rp/" + std::to_string(i), &got));
      STRATUM_CHECK_EQ(got, std::string("{\"n\":") + std::to_string(i * 10) + "}");
    }
    // 内存视图（ListTopics）也必须带上
    std::vector<TopicMetadata> topics;
    STRATUM_CHECK_OK(tl.ListTopics(&topics));
    int with_def = 0;
    for (const auto& t : topics) {
      if (!t.type_definition.empty()) ++with_def;
    }
    STRATUM_CHECK_EQ(with_def, kN);
    std::printf("      [持久化] 重开后 %d 个话题的定义全部还在\n", with_def);
    STRATUM_CHECK_OK(tl.Close());
  }
}

STRATUM_TEST(TypeDefinition, GetTopicMetadataAlsoCarriesDefinition) {
  // 单点读取（GetTopicMetadata）与列表读取必须一致
  const std::string dir = TempDir("byid");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  const std::vector<uint32_t> ids = RegisterN(&tl, 3, "byid");
  for (int i = 0; i < 3; ++i) {
    STRATUM_CHECK_OK(tl.SetTopicTypeDefinition("byid/" + std::to_string(i),
                                              "{\"k\":" + std::to_string(i) + "}"));
  }
  for (int i = 0; i < 3; ++i) {
    TopicMetadata m;
    STRATUM_CHECK_OK(tl.GetTopicMetadata(ids[static_cast<size_t>(i)], &m));
    STRATUM_CHECK_EQ(m.type_definition,
                     std::string("{\"k\":") + std::to_string(i) + "}");
  }
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 三、覆盖更新
// ===========================================================================

STRATUM_TEST(TypeDefinition, OverwriteReplacesOldValue) {
  const std::string dir = TempDir("overwrite");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t id = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/ow"), &id));

  STRATUM_CHECK_OK(tl.SetTopicTypeDefinition("t/ow", "{\"v\":1}"));
  STRATUM_CHECK_OK(tl.SetTopicTypeDefinition("t/ow", "{\"v\":2}"));

  std::string got;
  STRATUM_CHECK_OK(tl.GetTopicTypeDefinition("t/ow", &got));
  STRATUM_CHECK_EQ(got, std::string("{\"v\":2}"));

  // 内存视图也必须是新值（不能只更新一处）
  TopicMetadata m;
  STRATUM_CHECK_OK(tl.GetTopicMetadata(id, &m));
  STRATUM_CHECK_EQ(m.type_definition, std::string("{\"v\":2}"));
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(TypeDefinition, EmptyDefinitionClearsIt) {
  const std::string dir = TempDir("clear");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t id = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/cl"), &id));
  STRATUM_CHECK_OK(tl.SetTopicTypeDefinition("t/cl", "{\"v\":1}"));

  // 空定义 —— 应清空（回到降级状态）
  STRATUM_CHECK_OK(tl.SetTopicTypeDefinition("t/cl", ""));

  std::string got;
  Status s = tl.GetTopicTypeDefinition("t/cl", &got);
  STRATUM_CHECK_TRUE(s.ok() || s.IsNotFound());
  STRATUM_CHECK_TRUE(got.empty());

  TopicMetadata m;
  STRATUM_CHECK_OK(tl.GetTopicMetadata(id, &m));
  STRATUM_CHECK_TRUE(m.type_definition.empty());
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 四、大数据量
// ===========================================================================

STRATUM_TEST(TypeDefinition, HundredTopicsWithDefinitions) {
  const std::string dir = TempDir("hundred");
  const int kN = 100;
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  (void)RegisterN(&tl, kN, "h");

  for (int i = 0; i < kN; ++i) {
    STRATUM_CHECK_OK(tl.SetTopicTypeDefinition(
        "h/" + std::to_string(i),
        "{\"topic\":" + std::to_string(i) + ",\"fields\":[\"a\",\"b\",\"c\"]}"));
  }
  int ok = 0;
  for (int i = 0; i < kN; ++i) {
    std::string got;
    if (!tl.GetTopicTypeDefinition("h/" + std::to_string(i), &got).ok()) continue;
    if (got == "{\"topic\":" + std::to_string(i) + ",\"fields\":[\"a\",\"b\",\"c\"]}") {
      ++ok;
    }
  }
  std::printf("      [规模] %d 个话题各带定义，逐个读回正确 %d 个\n", kN, ok);
  STRATUM_CHECK_EQ(ok, kN);
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 五、随机（多种子）
// ===========================================================================

STRATUM_TEST(TypeDefinition, RandomTopicsAndDefinitionsMultipleSeeds) {
  for (uint64_t seed : {11ULL, 111ULL, 1111ULL, 20261015ULL}) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> n_d(2, 40);
    std::uniform_int_distribution<int> len_d(1, 60);

    const std::string dir = TempDir(("r" + std::to_string(seed)).c_str());
    const int n = n_d(rng);
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    (void)RegisterN(&tl, n, "r");

    std::vector<std::string> defs;
    for (int i = 0; i < n; ++i) {
      std::string d = "{\"s\":\"";
      const int len = len_d(rng);
      for (int k = 0; k < len; ++k) {
        d.push_back(static_cast<char>('a' + (rng() % 26)));
      }
      d += "\"}";
      defs.push_back(d);
      STRATUM_CHECK_OK(tl.SetTopicTypeDefinition("r/" + std::to_string(i), d));
    }
    // 逐话题校验（随机内容也不能串位）
    for (int i = 0; i < n; ++i) {
      std::string got;
      STRATUM_CHECK_OK(tl.GetTopicTypeDefinition("r/" + std::to_string(i), &got));
      STRATUM_CHECK_EQ(got, defs[static_cast<size_t>(i)]);
    }
    STRATUM_CHECK_OK(tl.Close());
  }
}

// ===========================================================================
// 六、真实场景
// ===========================================================================

// ---------------------------------------------------------------------------
// 场景 1：注册前无定义（降级）→ 注册后有定义（可解码）
// 模拟 CLI `types register` 前后 `query --decode` 的行为差异。
// ---------------------------------------------------------------------------
STRATUM_TEST(TypeDefinitionReal, FromDegradedToDecodable) {
  const std::string dir = TempDir("real1");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t id = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("sensor/temp", "sensor/Reading"), &id));
  std::vector<Event> batch;
  for (int i = 0; i < 20; ++i) {
    batch.push_back(MakeEvent(id, static_cast<uint32_t>(i + 1),
                              1700000000000000000ULL + static_cast<uint64_t>(i) * 1000000ULL, 24));
  }
  STRATUM_CHECK_OK(tl.AppendBatch(batch));

  // 注册前：无定义 —— CLI 会标注 reason=no_type_definition
  TopicMetadata before;
  STRATUM_CHECK_OK(tl.GetTopicMetadata(id, &before));
  STRATUM_CHECK_TRUE(before.type_definition.empty());

  // 注册后：有定义 —— CLI 会真正尝试解码
  STRATUM_CHECK_OK(tl.SetTopicTypeDefinition(
      "sensor/temp", "{\"temperature_c\":\"double\",\"valid\":\"bool\"}"));
  TopicMetadata after;
  STRATUM_CHECK_OK(tl.GetTopicMetadata(id, &after));
  STRATUM_CHECK_TRUE(!after.type_definition.empty());
  STRATUM_CHECK_EQ(after.type_definition,
                   std::string("{\"temperature_c\":\"double\",\"valid\":\"bool\"}"));

  // 数据本身没被影响
  TimeRange all;
  all.begin_ns = kInvalidTimestamp;
  all.end_ns = kInvalidTimestamp;
  uint64_t n = 0;
  STRATUM_CHECK_OK(tl.CountRange(id, all, &n));
  STRATUM_CHECK_EQ(n, static_cast<uint64_t>(20));
  std::printf("      [场景1] 注册定义不改变数据（仍 20 条），只补上语义\n");
  STRATUM_CHECK_OK(tl.Close());
}

// ---------------------------------------------------------------------------
// 场景 2：一台设备上多个传感器共用同一类型
// 模拟 CLI `types register <type>` 按 type_name 批量挂定义。
// ---------------------------------------------------------------------------
STRATUM_TEST(TypeDefinitionReal, SameTypeAcrossManyTopics) {
  const std::string dir = TempDir("real2");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  // 6 个话题，类型名相同（同一类传感器的多个测点）
  const int kSensors = 6;
  std::vector<uint32_t> ids;
  for (int i = 0; i < kSensors; ++i) {
    uint32_t id = 0;
    STRATUM_CHECK_OK(tl.RegisterTopic(
        MakeTopic("line3/point" + std::to_string(i), "sensor/Reading"), &id));
    ids.push_back(id);
  }

  // CLI 的 register 是按 type_name 匹配的，这里模拟它把定义挂到全部同类型话题
  const std::string def = "{\"value\":\"double\"}";
  for (int i = 0; i < kSensors; ++i) {
    STRATUM_CHECK_OK(tl.SetTopicTypeDefinition("line3/point" + std::to_string(i), def));
  }

  int with_def = 0;
  for (int i = 0; i < kSensors; ++i) {
    std::string got;
    if (tl.GetTopicTypeDefinition("line3/point" + std::to_string(i), &got).ok() &&
        got == def) {
      ++with_def;
    }
  }
  STRATUM_CHECK_EQ(with_def, kSensors);
  std::printf("      [场景2] 同一类型的 %d 个测点，定义全部挂上\n", kSensors);
  STRATUM_CHECK_OK(tl.Close());
}

// ---------------------------------------------------------------------------
// 场景 3：定义与其它元数据共存（统计、裁剪记录）
// 注册定义不应破坏已有元数据。
// ---------------------------------------------------------------------------
STRATUM_TEST(TypeDefinitionReal, CoexistsWithStatsAndPruned) {
  const std::string dir = TempDir("real3");
  const uint64_t base = 1700000000000000000ULL;
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t id = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/co"), &id));

  std::vector<Event> batch;
  for (int i = 0; i < 100; ++i) {
    batch.push_back(MakeEvent(id, static_cast<uint32_t>(i + 1),
                              base + static_cast<uint64_t>(i) * 1000000ULL, 16));
  }
  STRATUM_CHECK_OK(tl.AppendBatch(batch));

  // 先裁一段，留下 pruned 记录
  TimeRange hole;
  hole.begin_ns = base + 20 * 1000000ULL;
  hole.end_ns = base + 40 * 1000000ULL;
  STRATUM_CHECK_OK(tl.DeleteRange(id, hole, nullptr, nullptr));

  // 再挂定义
  STRATUM_CHECK_OK(tl.SetTopicTypeDefinition("t/co", "{\"x\":1}"));

  // 三者都应完好
  std::string got;
  STRATUM_CHECK_OK(tl.GetTopicTypeDefinition("t/co", &got));
  STRATUM_CHECK_EQ(got, std::string("{\"x\":1}"));

  std::vector<PrunedRange> pr;
  STRATUM_CHECK_OK(tl.ListPrunedRanges(id, &pr));
  STRATUM_CHECK_EQ(pr.size(), static_cast<size_t>(1));

  TimeRange all;
  all.begin_ns = kInvalidTimestamp;
  all.end_ns = kInvalidTimestamp;
  uint64_t n = 0;
  STRATUM_CHECK_OK(tl.CountRange(id, all, &n));
  STRATUM_CHECK_EQ(n, static_cast<uint64_t>(80));
  STRATUM_CHECK_FALSE(tl.GetQualityStats().HasIntegrityIssue());
  std::printf("      [场景3] 定义 / 裁剪记录 / 统计 三者共存互不破坏\n");
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST_MAIN()
