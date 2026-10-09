// v2 回归：seq_in_ts 计数器跨重启恢复（P1）
#include <cstdint>
#include <string>
#include <vector>

#include "stratum/storage/lsm_v2/stratum_db.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::storage::lsm_v2;

namespace {
constexpr uint64_t kBaseTs = 1700000000000000000ULL;

Status RegisterAndWrite(StratumDB* db, uint32_t* topic, TimestampNs ts,
                        const std::string& payload) {
  TopicMetadata meta;
  meta.topic_name = "p1/topic";
  uint32_t t = 0;
  Status s = db->RegisterTopic(meta, &t);
  if (!s.ok()) return s;
  if (topic != nullptr) *topic = t;

  Event e;
  e.topic_id = t;
  e.source_timestamp_ns = ts;
  e.payload = payload;
  std::vector<Event> events{e};
  return db->AppendBatch(events);
}

}  // namespace

// ---------------------------------------------------------------------------
// 【P1 验证】重启后写同一 (topic, ts)，历史数据必须还在
//
// 修复前：seq_in_ts 计数器重启归零 → 新条目 key 与历史条目相同
//         → LevelDB 覆盖写 → 历史数据静默消失
// ---------------------------------------------------------------------------
STRATUM_TEST(LsmV2, P1_SeqCounterSurvivesRestart) {
  const std::string dir = "/tmp/stratum_v2_p1";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  uint32_t topic = 0;
  const TimestampNs kTs = kBaseTs + 123456789;

  // --- 第一次运行：写入 3 条同一时间戳的数据 ---
  {
    StratumDB* db = nullptr;
    STRATUM_CHECK_OK(StratumDB::Open(dir, &db));
    STRATUM_CHECK_OK(RegisterAndWrite(db, &topic, kTs, "OLD-1"));
    STRATUM_CHECK_OK(RegisterAndWrite(db, &topic, kTs, "OLD-2"));
    STRATUM_CHECK_OK(RegisterAndWrite(db, &topic, kTs, "OLD-3"));

    uint64_t n = 0;
    STRATUM_CHECK_OK(db->CountRange(topic, kTs, kTs + 1, &n));
    ctx.param_desc = "首次写入后同 ts 条数=" + std::to_string(n);
    STRATUM_CHECK_EQ(n, static_cast<uint64_t>(3));
    db->Close();
    delete db;
  }

  // --- 第二次运行：重启后写同一时间戳 ---
  {
    StratumDB* db = nullptr;
    STRATUM_CHECK_OK(StratumDB::Open(dir, &db));
    STRATUM_CHECK_OK(RegisterAndWrite(db, &topic, kTs, "NEW-1"));
    STRATUM_CHECK_OK(RegisterAndWrite(db, &topic, kTs, "NEW-2"));

    // 【核心断言】同 ts 必须是 5 条：3 旧 + 2 新。
    // 修复前只有 2 条（新写入覆盖了历史）。
    uint64_t n = 0;
    STRATUM_CHECK_OK(db->CountRange(topic, kTs, kTs + 1, &n));
    ctx.param_desc += " 重启再写后同 ts 条数=" + std::to_string(n);
    STRATUM_CHECK_EQ(n, static_cast<uint64_t>(5));

    // 内容验证：OLD-1 必须还在
    std::vector<Event> out;
    STRATUM_CHECK_OK(db->QueryRange(topic, kTs, kTs + 1, &out));
    bool found_old1 = false;
    for (const auto& e : out) {
      if (e.payload == "OLD-1") found_old1 = true;
    }
    ctx.param_desc += " OLD-1存在=" + std::string(found_old1 ? "是" : "否");
    STRATUM_CHECK_TRUE(found_old1);

    db->Close();
    delete db;
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

// ---------------------------------------------------------------------------
// 【P2 验证】未注册 topic 必须被拒绝
// ---------------------------------------------------------------------------
STRATUM_TEST(LsmV2, P2_UnregisteredTopicRejected) {
  const std::string dir = "/tmp/stratum_v2_p2";
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  StratumDB* db = nullptr;
  STRATUM_CHECK_OK(StratumDB::Open(dir, &db));

  Event e;
  e.topic_id = 999;  // 从未注册
  e.source_timestamp_ns = kBaseTs;
  e.payload = "orphan";
  std::vector<Event> events{e};
  const Status s = db->AppendBatch(events);
  ctx.param_desc = "未注册话题写入返回=" + s.ToString();
  STRATUM_CHECK_TRUE(s.code() == StatusCode::kInvalidArgument);

  db->Close();
  delete db;
  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

STRATUM_TEST_MAIN()
