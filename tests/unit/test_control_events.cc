// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 控制事件（control_events）的存取与语义测试
//
// 【这套测试守的是什么】
//
// `ctl stop --reason` 存在的唯一理由是：**主动停止必须留痕**，
// 否则事后 verify 会把它误判为「发布者停了」(RC-3) 或「空洞」(RC-4)，
// 于是有人去查并不存在的问题。
//
// 所以这些语义必须被固化：
//   1. **累积**：多次操作要全部保留（审计需要完整序列，不能互相覆盖）
//   2. **顺序**：读回来的顺序必须与写入顺序一致（不然时间线是错的）
//   3. **转义**：reason/detail 里若含逗号或换行，会**破坏行格式** ——
//      实现里替换成空格，本测试要断言这一点（这是格式契约）
//   4. **持久化**：重开后仍在，否则审计无从谈起
//
// 覆盖三类（项目测试标准）：
//   · 大数据量 —— 连续记录 200 条
//   · 随机     —— 多种子随机内容（含易破坏格式的字符）
//   · 真实场景 —— ① 记录停止理由 ② 与 pruned/typedef 共存 ③ 完整审计序列

// 标准库头文件必须最先包含
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"

namespace fs = std::filesystem;

using namespace stratum;

namespace {

std::string TempDir(const char* tag) {
  const std::string d = std::string("/tmp/stratum_ctlev_") + tag;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

TopicMetadata MakeTopic(const std::string& name) {
  TopicMetadata m;
  m.topic_name = name;
  m.type_name = "T";
  return m;
}

Event MakeEvent(uint32_t topic_id, uint32_t seq, uint64_t src_ns, size_t len) {
  Event e;
  e.topic_id = topic_id;
  e.seq = seq;
  e.seq_in_ts = 0;
  e.source_timestamp_ns = src_ns;
  e.reception_timestamp_ns = src_ns + 1000;
  e.payload = std::string(len, 'e');
  return e;
}

// 制造一个控制事件；at_ns 递增便于校验顺序
ControlEvent MakeEventAt(uint64_t at_ns, const std::string& action,
                         const std::string& reason, const std::string& detail = "") {
  ControlEvent e;
  e.at_ns = at_ns;
  e.action = action;
  e.reason = reason;
  e.detail = detail;
  return e;
}

}  // namespace

// ===========================================================================
// 一、基础契约
// ===========================================================================

STRATUM_TEST(ControlEvents, EmptyWhenNeverControlled) {
  const std::string dir = TempDir("empty");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  std::vector<ControlEvent> out;
  STRATUM_CHECK_OK(tl.ListControlEvents(&out));
  STRATUM_CHECK_TRUE(out.empty());   // 从未操作 -> 空，不是错误
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(ControlEvents, RecordAndListSingle) {
  const std::string dir = TempDir("single");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  STRATUM_CHECK_OK(tl.RecordControlEvent(
      MakeEventAt(1700000000000000000ULL, "stop", "调试完成，主动停止", "revision=abc")));

  std::vector<ControlEvent> out;
  STRATUM_CHECK_OK(tl.ListControlEvents(&out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(1));
  STRATUM_CHECK_EQ(out[0].at_ns, static_cast<TimestampNs>(1700000000000000000ULL));
  STRATUM_CHECK_EQ(out[0].action, std::string("stop"));
  STRATUM_CHECK_EQ(out[0].reason, std::string("调试完成，主动停止"));
  STRATUM_CHECK_EQ(out[0].detail, std::string("revision=abc"));
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(ControlEvents, AccumulatesInOrder) {
  // 审计需要**完整序列**，不能互相覆盖；且顺序必须与写入一致
  const std::string dir = TempDir("order");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  const int kN = 10;
  for (int i = 0; i < kN; ++i) {
    STRATUM_CHECK_OK(tl.RecordControlEvent(
        MakeEventAt(1700000000000000000ULL + static_cast<uint64_t>(i) * 1000ULL,
                    "stop", "第" + std::to_string(i) + "次", "")));
  }
  std::vector<ControlEvent> out;
  STRATUM_CHECK_OK(tl.ListControlEvents(&out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(kN));
  for (int i = 0; i < kN; ++i) {
    STRATUM_CHECK_EQ(out[static_cast<size_t>(i)].reason,
                     std::string("第") + std::to_string(i) + "次");
    // 时间必须递增（顺序正确）
    if (i > 0) {
      STRATUM_CHECK_TRUE(out[static_cast<size_t>(i)].at_ns >
                        out[static_cast<size_t>(i - 1)].at_ns);
    }
  }
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(ControlEvents, EmptyActionIsInvalidArgument) {
  const std::string dir = TempDir("noaction");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  Status s = tl.RecordControlEvent(MakeEventAt(1, "", "reason", ""));
  STRATUM_CHECK_TRUE(s.code() == StatusCode::kInvalidArgument);
  // 拒绝后不应留下记录
  std::vector<ControlEvent> out;
  STRATUM_CHECK_OK(tl.ListControlEvents(&out));
  STRATUM_CHECK_TRUE(out.empty());
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(ControlEvents, NullOutputIsInvalidArgument) {
  const std::string dir = TempDir("nullout");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  Status s = tl.ListControlEvents(nullptr);
  STRATUM_CHECK_TRUE(s.code() == StatusCode::kInvalidArgument);
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(ControlEvents, NotOpenIsShutdownInProgress) {
  const std::string dir = TempDir("notopen");
  (void)dir;
  TimelineImplV2 tl;   // 未 Open
  std::vector<ControlEvent> out;
  Status s1 = tl.ListControlEvents(&out);
  STRATUM_CHECK_TRUE(s1.code() == StatusCode::kShutdownInProgress);
  Status s2 = tl.RecordControlEvent(MakeEventAt(1, "stop", "r", ""));
  STRATUM_CHECK_TRUE(s2.code() == StatusCode::kShutdownInProgress);
}

// ===========================================================================
// 二、★ 格式契约：逗号与换行必须被转义
// ===========================================================================

STRATUM_TEST(ControlEvents, CommasAndNewlinesDoNotBreakFormat) {
  // 【为什么关键】存储格式是每行 "at_ns,action,reason,detail"。
  // 若 reason 里含逗号或换行，**行格式会被破坏** ——
  // 轻则字段错位，重则一条记录被读成两条。
  //
  // 实现的契约：写入时把逗号与换行替换成空格。
  // 本测试断言这个行为（它是**格式契约**，不是实现细节）。
  const std::string dir = TempDir("escape");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  STRATUM_CHECK_OK(tl.RecordControlEvent(MakeEventAt(
      1700000000000000000ULL, "stop",
      "理由里有,逗号\n和\n换行\r回车", "详情,也有,逗号\n以及换行")));

  std::vector<ControlEvent> out;
  STRATUM_CHECK_OK(tl.ListControlEvents(&out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(1));   // 不能被读成多条

  // 逗号/换行/回车都被替换为空格
  const std::string& r = out[0].reason;
  STRATUM_CHECK_TRUE(r.find(',') == std::string::npos);
  STRATUM_CHECK_TRUE(r.find('\n') == std::string::npos);
  STRATUM_CHECK_TRUE(r.find('\r') == std::string::npos);
  STRATUM_CHECK_TRUE(r.find("理由里有") != std::string::npos);
  STRATUM_CHECK_TRUE(r.find("逗号") != std::string::npos);

  const std::string& d = out[0].detail;
  STRATUM_CHECK_TRUE(d.find(',') == std::string::npos);
  STRATUM_CHECK_TRUE(d.find('\n') == std::string::npos);
  std::printf("      [格式契约] 含逗号与换行的理由被安全转义，未破坏行格式\n");
  STRATUM_CHECK_OK(tl.Close());
}

STRATUM_TEST(ControlEvents, ManyRecordsWithCommasStillParseCorrectly) {
  // 转义后**字段数量**必须仍然正确（否则会串位）
  const std::string dir = TempDir("multi-escape");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  const int kN = 20;
  for (int i = 0; i < kN; ++i) {
    STRATUM_CHECK_OK(tl.RecordControlEvent(MakeEventAt(
        1700000000000000000ULL + static_cast<uint64_t>(i) * 1000ULL, "set",
        "a,b,c," + std::to_string(i), "x,y=" + std::to_string(i * 2))));
  }
  std::vector<ControlEvent> out;
  STRATUM_CHECK_OK(tl.ListControlEvents(&out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(kN));
  for (int i = 0; i < kN; ++i) {
    // 序号必须逐条对上（若串位就会失败）
    STRATUM_CHECK_TRUE(out[static_cast<size_t>(i)].reason.find(
                           std::to_string(i)) != std::string::npos);
    STRATUM_CHECK_TRUE(out[static_cast<size_t>(i)].detail.find(
                           std::to_string(i * 2)) != std::string::npos);
  }
  std::printf("      [转义] %d 条含逗号的记录全部逐条对上，无串位\n", kN);
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 三、持久化
// ===========================================================================

STRATUM_TEST(ControlEvents, SurvivesReopen) {
  const std::string dir = TempDir("reopen");
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    STRATUM_CHECK_OK(tl.RecordControlEvent(
        MakeEventAt(1700000000000000000ULL, "stop", "第一次停止", "ctx=1")));
    STRATUM_CHECK_OK(tl.RecordControlEvent(
        MakeEventAt(1700000000000000001ULL, "snapshot", "做了一次快照", "ctx=2")));
    STRATUM_CHECK_OK(tl.Close());
  }
  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));
    std::vector<ControlEvent> out;
    STRATUM_CHECK_OK(tl.ListControlEvents(&out));
    STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(2));
    STRATUM_CHECK_EQ(out[0].action, std::string("stop"));
    STRATUM_CHECK_EQ(out[1].action, std::string("snapshot"));
    STRATUM_CHECK_EQ(out[1].detail, std::string("ctx=2"));
    STRATUM_CHECK_OK(tl.Close());
  }
}

// ===========================================================================
// 四、大数据量
// ===========================================================================

STRATUM_TEST(ControlEvents, LargeVolumeTwoHundred) {
  const std::string dir = TempDir("large");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  const int kN = 200;
  for (int i = 0; i < kN; ++i) {
    STRATUM_CHECK_OK(tl.RecordControlEvent(MakeEventAt(
        1700000000000000000ULL + static_cast<uint64_t>(i) * 1000000ULL,
        (i % 2 == 0) ? "stop" : "set",
        "第" + std::to_string(i) + "次操作", "d=" + std::to_string(i))));
  }
  std::vector<ControlEvent> out;
  STRATUM_CHECK_OK(tl.ListControlEvents(&out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(kN));
  // 首尾必须正确
  STRATUM_CHECK_TRUE(out.front().reason.find("第0次") != std::string::npos);
  STRATUM_CHECK_TRUE(out.back().reason.find("第199次") != std::string::npos);
  std::printf("      [规模] 连续记录 %d 条控制事件，全部保留且顺序正确\n", kN);
  STRATUM_CHECK_OK(tl.Close());
}

// ===========================================================================
// 五、随机（多种子）
// ===========================================================================

STRATUM_TEST(ControlEvents, RandomContentMultipleSeeds) {
  for (uint64_t seed : {7ULL, 77ULL, 777ULL, 20261015ULL}) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> n_d(1, 40);
    std::uniform_int_distribution<int> ch_d(0, 5);

    const std::string dir = TempDir(("r" + std::to_string(seed)).c_str());
    const int n = n_d(rng);
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(dir));

    std::vector<std::string> reasons;
    for (int i = 0; i < n; ++i) {
      // 随机内容，**刻意混入逗号与换行**（最易破坏格式的字符）
      std::string r = "s" + std::to_string(i);
      const int extra = ch_d(rng);
      for (int k = 0; k < extra; ++k) {
        const char pool[] = {',', '\n', '\r', 'a', 'b', ' '};
        r.push_back(pool[static_cast<size_t>(rng() % 6)]);
      }
      reasons.push_back(r);
      STRATUM_CHECK_OK(tl.RecordControlEvent(
          MakeEventAt(1700000000000000000ULL + static_cast<uint64_t>(i) * 1000ULL,
                      "stop", r, "")));
    }

    std::vector<ControlEvent> out;
    STRATUM_CHECK_OK(tl.ListControlEvents(&out));
    // 【关键】记录条数必须与写入一致 —— 换行若未转义，一条会被读成多条
    STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      // 每条必须以自己的序号开头（顺序不能乱）
      STRATUM_CHECK_TRUE(out[static_cast<size_t>(i)].reason.rfind(
                             "s" + std::to_string(i), 0) == 0);
    }
    STRATUM_CHECK_OK(tl.Close());
  }
}

// ===========================================================================
// 六、真实场景
// ===========================================================================

// ---------------------------------------------------------------------------
// 场景 1：记录"为什么停止" —— 这是本功能存在的理由
// ---------------------------------------------------------------------------
STRATUM_TEST(ControlEventsReal, StopReasonIsRecorded) {
  const std::string dir = TempDir("real1");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));
  uint32_t id = 0;
  STRATUM_CHECK_OK(tl.RegisterTopic(MakeTopic("t/a"), &id));
  std::vector<Event> batch;
  for (int i = 0; i < 50; ++i) {
    batch.push_back(MakeEvent(id, static_cast<uint32_t>(i + 1),
                              1700000000000000000ULL + static_cast<uint64_t>(i) * 1000ULL, 16));
  }
  STRATUM_CHECK_OK(tl.AppendBatch(batch));

  // 用户主动停止并说明理由
  STRATUM_CHECK_OK(tl.RecordControlEvent(MakeEventAt(
      1700000000100000000ULL, "stop", "现场调试完成，主动收工", "operator=zhang")));

  std::vector<ControlEvent> out;
  STRATUM_CHECK_OK(tl.ListControlEvents(&out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(1));
  STRATUM_CHECK_EQ(out[0].reason, std::string("现场调试完成，主动收工"));

  // 数据本身不受影响
  TimeRange all;
  all.begin_ns = kInvalidTimestamp;
  all.end_ns = kInvalidTimestamp;
  uint64_t n = 0;
  STRATUM_CHECK_OK(tl.CountRange(id, all, &n));
  STRATUM_CHECK_EQ(n, static_cast<uint64_t>(50));
  std::printf("      [场景1] 停止理由已留痕（数据不受影响，仍 50 条）\n");
  STRATUM_CHECK_OK(tl.Close());
}

// ---------------------------------------------------------------------------
// 场景 2：与 pruned / typedef 共存（三种独立 meta key 互不干扰）
// ---------------------------------------------------------------------------
STRATUM_TEST(ControlEventsReal, CoexistsWithPrunedAndTypeDefinition) {
  const std::string dir = TempDir("real2");
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

  // 三种元数据都写一遍
  TimeRange hole;
  hole.begin_ns = base + 10 * 1000000ULL;
  hole.end_ns = base + 20 * 1000000ULL;
  STRATUM_CHECK_OK(tl.DeleteRange(id, hole, nullptr, nullptr));
  STRATUM_CHECK_OK(tl.SetTopicTypeDefinition("t/co", "{\"x\":1}"));
  STRATUM_CHECK_OK(tl.RecordControlEvent(
      MakeEventAt(base, "stop", "维护窗口", "")));

  // 三者都必须完好
  std::vector<PrunedRange> pr;
  STRATUM_CHECK_OK(tl.ListPrunedRanges(id, &pr));
  STRATUM_CHECK_EQ(pr.size(), static_cast<size_t>(1));

  std::string def;
  STRATUM_CHECK_OK(tl.GetTopicTypeDefinition("t/co", &def));
  STRATUM_CHECK_EQ(def, std::string("{\"x\":1}"));

  std::vector<ControlEvent> ev;
  STRATUM_CHECK_OK(tl.ListControlEvents(&ev));
  STRATUM_CHECK_EQ(ev.size(), static_cast<size_t>(1));

  uint64_t n = 0;
  TimeRange all;
  all.begin_ns = kInvalidTimestamp;
  all.end_ns = kInvalidTimestamp;
  STRATUM_CHECK_OK(tl.CountRange(id, all, &n));
  STRATUM_CHECK_EQ(n, static_cast<uint64_t>(90));
  std::printf("      [场景2] pruned / typedef / control_events 三个独立 key 共存正常\n");
  STRATUM_CHECK_OK(tl.Close());
}

// ---------------------------------------------------------------------------
// 场景 3：完整审计序列（运维一天的操作记录）
// ---------------------------------------------------------------------------
STRATUM_TEST(ControlEventsReal, FullAuditTrail) {
  const std::string dir = TempDir("real3");
  TimelineImplV2 tl;
  STRATUM_CHECK_OK(tl.Open(dir));

  struct Op {
    const char* action;
    const char* reason;
    const char* detail;
  };
  const Op ops[] = {
      {"start", "开始录制", "domain=44"},
      {"set", "调大保留窗口", "retain=24h"},
      {"snapshot", "打一个检查点", "tag=v1.2"},
      {"stop", "交接班", "operator=li"},
      {"start", "下一班开始", "operator=wang"},
      {"stop", "设备维护", "reason=planned_maintenance"},
  };
  const int kN = 6;
  for (int i = 0; i < kN; ++i) {
    STRATUM_CHECK_OK(tl.RecordControlEvent(MakeEventAt(
        1700000000000000000ULL + static_cast<uint64_t>(i) * 3600000000000ULL,
        ops[i].action, ops[i].reason, ops[i].detail)));
  }

  std::vector<ControlEvent> out;
  STRATUM_CHECK_OK(tl.ListControlEvents(&out));
  STRATUM_CHECK_EQ(out.size(), static_cast<size_t>(kN));
  for (int i = 0; i < kN; ++i) {
    STRATUM_CHECK_EQ(out[static_cast<size_t>(i)].action, std::string(ops[i].action));
    STRATUM_CHECK_EQ(out[static_cast<size_t>(i)].reason, std::string(ops[i].reason));
    STRATUM_CHECK_EQ(out[static_cast<size_t>(i)].detail, std::string(ops[i].detail));
  }
  std::printf("      [场景3] 完整审计序列 %d 条（start/set/snapshot/stop…）逐条还原\n", kN);
  STRATUM_CHECK_OK(tl.Close());

  // 重开后审计序列仍完整（审计的基本要求）
  TimelineImplV2 tl2;
  STRATUM_CHECK_OK(tl2.Open(dir));
  std::vector<ControlEvent> out2;
  STRATUM_CHECK_OK(tl2.ListControlEvents(&out2));
  STRATUM_CHECK_EQ(out2.size(), static_cast<size_t>(kN));
  STRATUM_CHECK_EQ(out2.back().reason, std::string("设备维护"));
  STRATUM_CHECK_OK(tl2.Close());
}

STRATUM_TEST_MAIN()
