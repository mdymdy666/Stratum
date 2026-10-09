// v2 DB 层快速验证（基于 LSMengine）
#include <cstdint>
#include <string>
#include <vector>

#include "stratum/storage/lsm_v2/stratum_db.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::storage::lsm_v2;

namespace {
constexpr uint64_t kBaseTs = 1700000000000000000ULL;
}

STRATUM_TEST(LsmV2, AppendQueryRange) {
  const std::string dir = "/tmp/stratum_v2_basic";
  if (system(("rm -rf " + dir).c_str()) != 0) {}
  if (system(("mkdir -p " + dir).c_str()) != 0) {}

  StratumDB* db = nullptr;
  STRATUM_CHECK_OK(StratumDB::Open(dir, &db));
  STRATUM_CHECK_TRUE(db != nullptr);

  TopicMetadata meta;
  meta.topic_name = "sensor/temp";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(db->RegisterTopic(meta, &topic));
  STRATUM_CHECK_TRUE(topic > 0);

  // 10000 条追加
  constexpr int kCount = 10000;
  constexpr int kBatch = 1000;
  for (int base = 0; base < kCount; base += kBatch) {
    std::vector<Event> events;
    for (int i = 0; i < kBatch; ++i) {
      Event e;
      e.topic_id = topic;
      e.source_timestamp_ns = kBaseTs + static_cast<uint64_t>(base + i) * 1000;
      e.payload = "v" + std::to_string(base + i);
      events.push_back(std::move(e));
    }
    STRATUM_CHECK_OK(db->AppendBatch(events));
  }

  // 全范围查询
  std::vector<Event> out;
  STRATUM_CHECK_OK(db->QueryRange(topic, kBaseTs, kBaseTs + kCount * 1000ULL, &out));
  ctx.param_desc = "count=" + std::to_string(out.size());
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(kCount));

  // 时间点查询：as_of = 第 5000 条的时刻
  StateAtResult r;
  STRATUM_CHECK_OK(db->GetStateAt(topic, kBaseTs + 5000 * 1000ULL, &r));
  ctx.param_desc += " state=" + std::to_string(
      static_cast<int>(r.kind)) + " ts=" + std::to_string(r.as_of_ns);
  STRATUM_CHECK_EQ(static_cast<int>(r.kind),
                   static_cast<int>(StateAtResult::Kind::kFound));
  STRATUM_CHECK_EQ(r.as_of_ns, kBaseTs + 5000 * 1000ULL);

  // 同时间戳多条：不覆盖
  {
    std::vector<Event> dup;
    for (int i = 0; i < 50; ++i) {
      Event e;
      e.topic_id = topic;
      e.source_timestamp_ns = kBaseTs + kCount * 2000ULL;  // 同一时刻
      e.payload = "same-ts-" + std::to_string(i);
      dup.push_back(std::move(e));
    }
    STRATUM_CHECK_OK(db->AppendBatch(dup));
    uint64_t n = 0;
    STRATUM_CHECK_OK(db->CountRange(topic, kBaseTs + kCount * 2000ULL,
                                    kBaseTs + kCount * 2000ULL + 1, &n));
    ctx.param_desc += " same_ts=" + std::to_string(n);
    STRATUM_CHECK_EQ(n, static_cast<uint64_t>(50));
  }

  // 关闭 + 重开（持久化验证）
  STRATUM_CHECK_OK(db->Close());
  delete db;

  StratumDB* db2 = nullptr;
  STRATUM_CHECK_OK(StratumDB::Open(dir, &db2));
  std::vector<TopicMetadata> topics;
  STRATUM_CHECK_OK(db2->ListTopics(&topics));
  ctx.param_desc += " reopen_topics=" + std::to_string(topics.size());
  STRATUM_CHECK_EQ(topics.size(), static_cast<size_t>(1));

  std::vector<Event> out2;
  STRATUM_CHECK_OK(db2->QueryRange(topic, kBaseTs, kBaseTs + kCount * 1000ULL, &out2));
  ctx.param_desc += " reopen_count=" + std::to_string(out2.size());
  STRATUM_CHECK_EQ(out2.size(), static_cast<size_t>(kCount));

  db2->Close();
  delete db2;
  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

STRATUM_TEST_MAIN()
