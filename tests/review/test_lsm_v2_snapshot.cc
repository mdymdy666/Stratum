// v2 回归：保留点时间回溯（P3）
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "stratum/storage/lsm_v2/stratum_db.h"
#include "test_framework.h"
#include "test_isolation.h"

using namespace stratum;
using namespace stratum::storage::lsm_v2;

namespace {
constexpr uint64_t kBaseTs = 1700000000000000000ULL;
}

// ---------------------------------------------------------------------------
// 【P3 验证】创建保留点 → 继续写入 → 用保留点查询历史视图
//
// 修复前：CreateRetentionPoint 返回的快照无法被任何读路径使用
//         （ReadOptions 没接 snapshot），保留点是空转的 API。
// ---------------------------------------------------------------------------
STRATUM_TEST(LsmV2, P3_RetentionPointTimeTravel) {
  const std::string dir = std::string("/tmp/stratum_v2_p3_") + UniqueSuffix();
  if (system(("rm -rf " + dir).c_str()) != 0) {}

  StratumDB* db = nullptr;
  STRATUM_CHECK_OK(StratumDB::Open(dir, &db));

  TopicMetadata meta;
  meta.topic_name = "p3/state";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(db->RegisterTopic(meta, &topic));

  // 写入 t=1000 → "v1"
  {
    Event e;
    e.topic_id = topic;
    e.source_timestamp_ns = kBaseTs + 1000;
    e.payload = "v1";
    std::vector<Event> events{e};
    STRATUM_CHECK_OK(db->AppendBatch(events));
  }

  // 创建保留点
  std::unique_ptr<RetentionPoint> point;
  STRATUM_CHECK_OK(db->CreateRetentionPoint(&point));
  STRATUM_CHECK_TRUE(point != nullptr);

  // 继续写入 t=2000 → "v2"
  {
    Event e;
    e.topic_id = topic;
    e.source_timestamp_ns = kBaseTs + 2000;
    e.payload = "v2";
    std::vector<Event> events{e};
    STRATUM_CHECK_OK(db->AppendBatch(events));
  }

  // 最新视图：t=2000 时值为 "v2"
  {
    StateAtResult r;
    STRATUM_CHECK_OK(db->GetStateAt(topic, kBaseTs + 2500, &r));
    ctx.param_desc = "最新视图=" + r.event.payload;
    STRATUM_CHECK_EQ(r.event.payload, std::string("v2"));
  }

  // 【核心断言】保留点视图：t=2500 时值应为 "v1"（v2 在保留点之后写入）
  {
    StateAtResult r;
    STRATUM_CHECK_OK(db->GetStateAt(topic, kBaseTs + 2500, &r, point.get()));
    ctx.param_desc += " 保留点视图=" + r.event.payload;
    STRATUM_CHECK_EQ(r.event.payload, std::string("v1"));
  }

  // 范围查询同样支持保留点
  {
    std::vector<Event> out;
    STRATUM_CHECK_OK(db->QueryRange(topic, kBaseTs, kBaseTs + 5000, &out,
                                    point.get()));
    ctx.param_desc += " 保留点范围条数=" + std::to_string(out.size());
    STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(1));  // 只有 v1
  }

  // 【生命周期契约】RetentionPoint 必须在 DB 销毁前析构，
  // 否则 ReleaseSnapshot 访问已销毁的 DB（use-after-free）。
  point.reset();
  db->Close();
  delete db;
  if (system(("rm -rf " + dir).c_str()) != 0) {}
}

STRATUM_TEST_MAIN()
