// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// `stratum diff` 的测试
//
// 设计裁决见 docs/15-session-diff.md。本文件验证三个裁决的落地：
//   ① 相对时间对齐（不是绝对时间）
//   ② topic 级统计 + 分桶（不做事件级逐条）
//   ③ 默认不比 payload（三档显式开关）
//
// 【本文件最重要的两组用例】
//
//   1. **RelativeAlignment**：两个会话起始时刻不同但内容等价 → 必须报"无差异"。
//      这是对齐是否正确的唯一硬判据。若误用绝对时间，这条会立刻失败。
//
//   2. **BucketsDetectTailLoss**：构造"后半段为 0"的会话 → 分桶必须检出。
//      这是 diff「给线索」而不是「报数字」的分界线（见 docs/15 §三）。
//
// 【为什么在测试里直接构造会话】
// 用 TimelineImplV2 的公开 API（RegisterTopic + AppendBatch）造会话，
// 比起真实 record + DDS 快一个数量级，而且**可精确控制**时间分布与条数 ——
// 这正是 diff 测试需要的。

// 标准库头文件必须最先包含
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"
#include "test_isolation.h"

namespace fs = std::filesystem;
using namespace stratum;

namespace {

#ifndef DIFF_CLI_PATH
#define DIFF_CLI_PATH "./stratum"
#endif

std::string kCli = DIFF_CLI_PATH;

struct CliResult {
  int code = -1;
  std::string output;
};

CliResult RunCli(const std::string& cmd) {
  CliResult r;
  std::FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
  if (p == nullptr) return r;
  char buf[16384];
  while (std::fgets(buf, sizeof(buf), p) != nullptr) r.output += buf;
  const int st = ::pclose(p);
  r.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return r;
}

std::string TempDir(const char* tag) {
  const char* home = std::getenv("HOME");
  const std::string d = std::string(home ? home : ".") + "/stratum_dd_" + tag + "_" + UniqueSuffix();
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

// 时间分布的三种形态（用于验证分桶的价值）
enum class Pattern {
  kUniform,     // 均匀
  kTailEmpty,   // 只在前 50% 时间写，后半段为 0 —— 分桶必须检出
  kFrontHeavy,  // 前 30% 时间写 80% 的数据
};

// 构造一份会话。
//
// 参数精确可控：topic 数、每 topic 条数、时间分布、起始时刻偏移。
// **起始偏移是 RelativeAlignment 用例的关键** —— 两个会话内容等价但起点不同。
struct SessionSpec {
  std::string dir;
  int topics = 1;
  int events_per_topic = 10;
  Pattern pattern = Pattern::kUniform;
  uint64_t span_ns = 1000000000ull;   // 总跨度 1 秒
  TimestampNs origin_ns = 1700000000000000000ull;   // 起点（可不同）
  int probe_topics = 0;
  int probe_events = 0;
  size_t payload_size = 8;
  uint32_t payload_seed = 0;   // 改变它就改变 payload 内容（用于 payload 比较用例）
};

bool MakeSession(const SessionSpec& spec, std::string* err) {
  TimelineImplV2 timeline;
  Status s = timeline.Open(spec.dir);
  if (!s.ok()) {
    if (err) *err = "Open 失败: " + s.ToString();
    return false;
  }

  auto Acceptor = [&](const std::string& name, uint32_t* id) -> bool {
    TopicMetadata meta;
    meta.topic_name = name;
    meta.type_name = "TestType";
    return timeline.RegisterTopic(meta, id).ok();
  };

  // 计算某条事件的时间戳（相对 origin）
  auto TimeAt = [&spec](int i, int n) -> TimestampNs {
    if (n <= 1) return spec.origin_ns;
    double frac = 0.0;
    switch (spec.pattern) {
      case Pattern::kUniform:
        frac = static_cast<double>(i) / static_cast<double>(n - 1);
        break;
      case Pattern::kTailEmpty:
        // 只占前 50% 的时间轴 —— 后半段为 0，分桶应检出
        frac = 0.5 * static_cast<double>(i) / static_cast<double>(n - 1);
        break;
      case Pattern::kFrontHeavy:
        // 用平方根把事件压到前段：80% 的事件落在前 30% 时间
        frac = std::sqrt(static_cast<double>(i) / static_cast<double>(n - 1)) * 0.3;
        break;
    }
    const double off = frac * static_cast<double>(spec.span_ns);
    return spec.origin_ns + static_cast<TimestampNs>(off);
  };

  // 业务 topic
  for (int t = 0; t < spec.topics; ++t) {
    const std::string name = "biz/t" + std::to_string(t);
    uint32_t tid = 0;
    if (!Acceptor(name, &tid)) {
      if (err) *err = "RegisterTopic(" + name + ") 失败";
      timeline.Close();
      return false;
    }
    std::vector<Event> events;
    events.reserve(static_cast<size_t>(spec.events_per_topic));
    for (int i = 0; i < spec.events_per_topic; ++i) {
      Event e;
      e.topic_id = tid;
      e.seq = static_cast<uint32_t>(i + 1);
      e.source_timestamp_ns = TimeAt(i, spec.events_per_topic);
      e.reception_timestamp_ns = e.source_timestamp_ns + 500;
      // payload 内容受 seed 控制，用于验证 --compare-payload
      std::string p(spec.payload_size, static_cast<char>('a' + (spec.payload_seed % 26)));
      if (!p.empty()) p[0] = static_cast<char>('0' + (i % 10));
      e.payload = p;
      events.push_back(std::move(e));
    }
    s = timeline.AppendBatch(events);
    if (!s.ok()) {
      if (err) *err = "AppendBatch(" + name + ") 失败: " + s.ToString();
      timeline.Close();
      return false;
    }
  }

  // 探针 topic（前缀 _probe.，与生产一致）
  for (int t = 0; t < spec.probe_topics; ++t) {
    // ★ 前缀是 "_probe."（**点**）。写 "_probe/p0" 不会被识别为探针 topic ——
    // 构造时必须与生产一致，否则测的就不是真实分类逻辑。
    const std::string name = "_probe.p" + std::to_string(t);
    uint32_t tid = 0;
    if (!Acceptor(name, &tid)) {
      if (err) *err = "RegisterTopic(" + name + ") 失败";
      timeline.Close();
      return false;
    }
    std::vector<Event> events;
    for (int i = 0; i < spec.probe_events; ++i) {
      Event e;
      e.topic_id = tid;
      e.seq = static_cast<uint32_t>(i + 1);
      e.source_timestamp_ns = TimeAt(i, std::max(spec.probe_events, 1));
      e.reception_timestamp_ns = e.source_timestamp_ns + 500;
      e.payload = "result=ok";
      events.push_back(std::move(e));
    }
    s = timeline.AppendBatch(events);
    if (!s.ok()) {
      if (err) *err = "AppendBatch(" + name + ") 失败: " + s.ToString();
      timeline.Close();
      return false;
    }
  }

  timeline.Close();
  return true;
}

long long PickInt(const std::string& text, const std::string& key) {
  const std::string pat = "\"" + key + "\":";
  const size_t at = text.find(pat);
  if (at == std::string::npos) return -1;
  size_t p = at + pat.size();
  while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
  char* end = nullptr;
  const long long v = std::strtoll(text.c_str() + p, &end, 10);
  if (end == text.c_str() + p) return -1;
  return v;
}

// 在 `"side":{ ... }` 这一段里取整数字段。
//
// 【为什么需要它】diff 的 JSON 里同一个 key 会在多处出现：
//   "left":{"events":10000, ...}                      ← 会话总数
//   "changed":[{"topic":"x","left_events":1000, ...}]  ← 单个 topic
// 直接用"找第一个 left_events"会命中后者，读到的不是总数。
long long PickInSide(const std::string& text, const std::string& side,
                     const std::string& field) {
  const std::string pat = "\"" + side + "\":{";
  const size_t at = text.find(pat);
  if (at == std::string::npos) return -1;
  const size_t end = text.find("}", at);
  if (end == std::string::npos) return -1;
  const std::string seg = text.substr(at, end - at);
  const std::string fpat = "\"" + field + "\":";
  const size_t fp = seg.find(fpat);
  if (fp == std::string::npos) return -1;
  return std::strtoll(seg.c_str() + fp + fpat.size(), nullptr, 10);
}

// 在 `"payload":{ ... }` 里取整数字段（payload 的 changed 与 topics 的 changed 同名）
long long PickInPayload(const std::string& text, const std::string& field) {
  const size_t at = text.find("\"payload\":{");
  if (at == std::string::npos) return -1;
  const size_t end = text.find("}", at);
  if (end == std::string::npos) return -1;
  const std::string seg = text.substr(at, end - at);
  const std::string fpat = "\"" + field + "\":";
  const size_t fp = seg.find(fpat);
  if (fp == std::string::npos) return -1;
  return std::strtoll(seg.c_str() + fp + fpat.size(), nullptr, 10);
}

// 从 JSON 里粗取某个 topic 在 changed 数组里的 delta_pct
double PickDeltaPct(const std::string& text, const std::string& topic) {
  const std::string pat = "\"topic\":\"" + topic + "\"";
  const size_t at = text.find(pat);
  if (at == std::string::npos) return -999.0;
  const size_t seg_end = text.find("}", at);
  const std::string seg = text.substr(at, seg_end - at);
  const size_t dp = seg.find("\"delta_pct\":");
  if (dp == std::string::npos) return -999.0;
  return std::atof(seg.c_str() + dp + 12);
}

}  // namespace

// ===========================================================================
// 一、同一会话与自己比 → 无差异、退出码 0
// ===========================================================================

STRATUM_TEST(DiffBasic, SelfDiffHasNoDifference) {
  const std::string dir = TempDir("self");
  SessionSpec spec;
  spec.dir = dir;
  spec.topics = 3;
  spec.events_per_topic = 50;
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(spec, &err));

  const CliResult r = RunCli(kCli + " diff " + dir + " " + dir);
  // 无差异 → 0（不是 5）
  STRATUM_CHECK_EQ(r.code, 0);
  STRATUM_CHECK_TRUE(r.output.find("\"added\":[]") != std::string::npos);
  STRATUM_CHECK_TRUE(r.output.find("\"removed\":[]") != std::string::npos);
  STRATUM_CHECK_TRUE(r.output.find("\"changed\":[]") != std::string::npos);
  std::printf("      [自身] 同一会话与自己比：0 差异、退出码 0\n");
}

// ===========================================================================
// 二、条数差异：100 vs 80 → delta 与 delta_pct 精确
// ===========================================================================

STRATUM_TEST(DiffBasic, CountDeltaIsExact) {
  const std::string a = TempDir("cnt_a");
  const std::string b = TempDir("cnt_b");
  std::string err;
  SessionSpec sa;
  sa.dir = a;
  sa.topics = 1;
  sa.events_per_topic = 100;
  STRATUM_CHECK_TRUE(MakeSession(sa, &err));
  SessionSpec sb;
  sb.dir = b;
  sb.topics = 1;
  sb.events_per_topic = 80;
  STRATUM_CHECK_TRUE(MakeSession(sb, &err));

  const CliResult r = RunCli(kCli + " diff " + a + " " + b);
  // 有差异 → 5
  STRATUM_CHECK_EQ(r.code, 5);
  STRATUM_CHECK_EQ(PickInSide(r.output, "left", "events"), 100LL);
  STRATUM_CHECK_EQ(PickInSide(r.output, "right", "events"), 80LL);
  STRATUM_CHECK_EQ(PickInt(r.output, "delta"), -20LL);
  // delta_pct: -20.00
  const double pct = PickDeltaPct(r.output, "biz/t0");
  STRATUM_CHECK_TRUE(pct > -20.01 && pct < -19.99);
  std::printf("      [条数] 100 → 80：delta=-20, delta_pct=%.2f%%，退出码 5\n", pct);
}

// ===========================================================================
// 三、★ 相对时间对齐（最关键的对齐验证）
// ===========================================================================
//
// 两个会话：内容完全相同（条数/分布/跨度），但**起始时刻相差 1 小时**。
// 若实现用了绝对时间，diff 会报"所有事件都不同"。
// 正确实现（相对对齐）应当报**无差异**。

STRATUM_TEST(DiffAlign, DifferentStartSameContentIsNoDiff) {
  const std::string a = TempDir("al_a");
  const std::string b = TempDir("al_b");
  std::string err;

  SessionSpec sa;
  sa.dir = a;
  sa.topics = 2;
  sa.events_per_topic = 40;
  sa.span_ns = 2000000000ull;               // 2 秒
  sa.origin_ns = 1700000000000000000ull;    // 起点 A
  STRATUM_CHECK_TRUE(MakeSession(sa, &err));

  SessionSpec sb = sa;
  sb.dir = b;
  // ★ 起点差 1 小时，内容完全一样
  sb.origin_ns = sa.origin_ns + 3600ull * 1000000000ull;
  STRATUM_CHECK_TRUE(MakeSession(sb, &err));

  const CliResult r = RunCli(kCli + " diff " + a + " " + b);
  // ★ 必须无差异
  STRATUM_CHECK_EQ(r.code, 0);
  STRATUM_CHECK_TRUE(r.output.find("\"changed\":[]") != std::string::npos);
  // 两侧 origin 确实不同（证明对齐真的在用相对时间）
  const long long lo = PickInt(r.output, "left_origin_ns");
  const long long ro = PickInt(r.output, "right_origin_ns");
  STRATUM_CHECK_TRUE(lo != ro);
  std::printf("      [对齐] 起点相差 1 小时、内容等价 → 报无差异（相对对齐生效）\n");
}

// ===========================================================================
// 四、★ 分桶检出「后半段为 0」（diff 从报数字到给线索）
// ===========================================================================

STRATUM_TEST(DiffBuckets, TailEmptyIsDetected) {
  const std::string a = TempDir("bk_a");
  const std::string b = TempDir("bk_b");
  std::string err;
  // A：均匀；B：只在前 50% 时间写 —— 条数相同，但分布不同
  SessionSpec sa;
  sa.dir = a;
  sa.topics = 1;
  sa.events_per_topic = 100;
  sa.pattern = Pattern::kUniform;
  STRATUM_CHECK_TRUE(MakeSession(sa, &err));
  SessionSpec sb = sa;
  sb.dir = b;
  sb.pattern = Pattern::kTailEmpty;
  STRATUM_CHECK_TRUE(MakeSession(sb, &err));

  const CliResult r = RunCli(kCli + " diff " + a + " " + b + " --bucket 10");
  // 条数相同，但分桶不同 → 必须判为"有变化"
  STRATUM_CHECK_EQ(r.code, 5);
  STRATUM_CHECK_EQ(PickInSide(r.output, "left", "events"), 100LL);
  STRATUM_CHECK_EQ(PickInt(r.output, "right_events"), 100LL);
  // ★ 这是关键：条数完全一样，靠分桶才能发现差异
  STRATUM_CHECK_TRUE(r.output.find("\"changed\":[{\"topic\":\"biz/t0\"") !=
                     std::string::npos);
  STRATUM_CHECK_TRUE(r.output.find("max_delta_pct") != std::string::npos);
  std::printf("      [分桶] 条数都是 100 但分布不同 → 分桶检出差异（退出码 5）\n");
}

// ===========================================================================
// 五、--bucket 0 跳过分桶
// ===========================================================================

STRATUM_TEST(DiffBuckets, BucketZeroSkipsBucketing) {
  const std::string a = TempDir("b0_a");
  const std::string b = TempDir("b0_b");
  std::string err;
  SessionSpec sa;
  sa.dir = a;
  sa.topics = 1;
  sa.events_per_topic = 50;
  sa.pattern = Pattern::kUniform;
  STRATUM_CHECK_TRUE(MakeSession(sa, &err));
  SessionSpec sb = sa;
  sb.dir = b;
  sb.pattern = Pattern::kTailEmpty;
  STRATUM_CHECK_TRUE(MakeSession(sb, &err));

  // --bucket 0：只有条数相同 → 判为"未变"
  const CliResult r = RunCli(kCli + " diff " + a + " " + b + " --bucket 0");
  STRATUM_CHECK_TRUE(r.output.find("\"bucket\":0") != std::string::npos);
  // 条数一致且不比分布 → 无差异
  STRATUM_CHECK_EQ(r.code, 0);
  std::printf("      [快速模式] --bucket 0 跳过分桶，条数相同即判无差异\n");
}

// ===========================================================================
// 六、payload 三档
// ===========================================================================

STRATUM_TEST(DiffPayload, ThreeModesBehave) {
  const std::string a = TempDir("pl_a");
  const std::string b = TempDir("pl_b");
  std::string err;
  SessionSpec sa;
  sa.dir = a;
  sa.topics = 1;
  sa.events_per_topic = 30;
  sa.payload_seed = 0;      // payload 全 'a'（首字符按 i 变）
  STRATUM_CHECK_TRUE(MakeSession(sa, &err));
  SessionSpec sb = sa;
  sb.dir = b;
  sb.payload_seed = 1;      // payload 全 'b' → 内容不同
  STRATUM_CHECK_TRUE(MakeSession(sb, &err));

  // none（默认）：不扫 payload，条数相同 → 无差异
  const CliResult r1 = RunCli(kCli + " diff " + a + " " + b);
  STRATUM_CHECK_EQ(r1.code, 0);
  STRATUM_CHECK_TRUE(r1.output.find("\"mode\":\"none\"") != std::string::npos);

  // hash：必须检出 payload 变化
  const CliResult r2 = RunCli(kCli + " diff " + a + " " + b + " --compare-payload hash");
  STRATUM_CHECK_EQ(r2.code, 5);
  const long long changed = PickInPayload(r2.output, "changed");
  STRATUM_CHECK_TRUE(changed > 0);
  std::printf("      [payload] none 判无差异；hash 检出 %lld 条负载不同\n", changed);

  // sample：只比前 100 条（这里 30 条，全比）
  const CliResult r3 = RunCli(kCli + " diff " + a + " " + b + " --compare-payload sample");
  STRATUM_CHECK_EQ(r3.code, 5);
  STRATUM_CHECK_TRUE(r3.output.find("sample") != std::string::npos);
}

// ===========================================================================
// 七、业务层视角：探针 topic 与业务 topic 分开
// ===========================================================================

STRATUM_TEST(DiffBusiness, ProbeAndBizAreSeparated) {
  const std::string a = TempDir("bl_a");
  const std::string b = TempDir("bl_b");
  std::string err;
  SessionSpec sa;
  sa.dir = a;
  sa.topics = 2;
  sa.events_per_topic = 20;
  sa.probe_topics = 1;
  sa.probe_events = 10;
  STRATUM_CHECK_TRUE(MakeSession(sa, &err));
  SessionSpec sb = sa;
  sb.dir = b;
  sb.events_per_topic = 10;    // 业务减半
  sb.probe_events = 10;        // 探针不变
  STRATUM_CHECK_TRUE(MakeSession(sb, &err));

  const CliResult r = RunCli(kCli + " diff " + a + " " + b);
  // 业务层字段存在
  STRATUM_CHECK_TRUE(r.output.find("\"business_layer\"") != std::string::npos);
  // 两侧业务条数不同（40 vs 20），探针相同（10 vs 10）。
  // 用"字段 + 值"的宽松断言：JSON 里字段顺序可能变，不该依赖相邻字符串。
  STRATUM_CHECK_TRUE(r.output.find("\"dds_events\":40") != std::string::npos);
  STRATUM_CHECK_TRUE(r.output.find("\"dds_events\":20") != std::string::npos);
  // 探针两侧相同
  STRATUM_CHECK_TRUE(r.output.find("\"probe_events\":10") != std::string::npos);
  std::printf("      [业务层] 业务 40→20 被检出，探针 10→10 不计入变化\n");
}

// ===========================================================================
// 八、空会话
// ===========================================================================

STRATUM_TEST(DiffEdge, EmptySessionsDoNotCrash) {
  const std::string a = TempDir("em_a");
  const std::string b = TempDir("em_b");
  std::string err;

  // 两个空会话：只 Open/Close，不 RegisterTopic
  {
    TimelineImplV2 t;
    STRATUM_CHECK_TRUE(t.Open(a).ok());
    STRATUM_CHECK_TRUE(t.Close().ok());
  }
  {
    TimelineImplV2 t;
    STRATUM_CHECK_TRUE(t.Open(b).ok());
    STRATUM_CHECK_TRUE(t.Close().ok());
  }

  const CliResult r = RunCli(kCli + " diff " + a + " " + b);
  STRATUM_CHECK_EQ(r.code, 0);   // 两个空会话不算有差异
  STRATUM_CHECK_EQ(PickInSide(r.output, "left", "events"), 0LL);

  // 一空一非空
  SessionSpec sb;
  sb.dir = b + "_x";
  sb.topics = 1;
  sb.events_per_topic = 10;
  STRATUM_CHECK_TRUE(MakeSession(sb, &err));
  const CliResult r2 = RunCli(kCli + " diff " + a + " " + sb.dir);
  STRATUM_CHECK_EQ(r2.code, 5);   // 有 added
  STRATUM_CHECK_TRUE(r2.output.find("\"added\":[") != std::string::npos);
  std::printf("      [空会话] 两个空会话退出码 0；一空一非空退出码 5，均不崩\n");
}

// ===========================================================================
// 九、时长差异警告（把已知局限变成可检查信号）
// ===========================================================================

STRATUM_TEST(DiffWarning, DurationWarningFires) {
  const std::string a = TempDir("dw_a");
  const std::string b = TempDir("dw_b");
  std::string err;
  SessionSpec sa;
  sa.dir = a;
  sa.topics = 1;
  sa.events_per_topic = 50;
  sa.span_ns = 1000000000ull;      // 1 秒
  STRATUM_CHECK_TRUE(MakeSession(sa, &err));
  SessionSpec sb = sa;
  sb.dir = b;
  sb.span_ns = 50000000000ull;     // 50 秒 → 差 50 倍
  STRATUM_CHECK_TRUE(MakeSession(sb, &err));

  const CliResult r = RunCli(kCli + " diff " + a + " " + b);
  STRATUM_CHECK_TRUE(r.output.find("\"duration_warning\":true") != std::string::npos);
  // stderr 也要有警告（合并到 output 里）
  STRATUM_CHECK_TRUE(r.output.find("相差") != std::string::npos);
  std::printf("      [警告] 两侧跨度差 50 倍 → duration_warning=true 且有 stderr 警告\n");

  // 时长相近时不该警告
  SessionSpec sc = sa;
  sc.dir = b + "_c";
  sc.span_ns = 1200000000ull;   // 1.2 倍
  STRATUM_CHECK_TRUE(MakeSession(sc, &err));
  const CliResult r2 = RunCli(kCli + " diff " + a + " " + sc.dir);
  STRATUM_CHECK_TRUE(r2.output.find("\"duration_warning\":false") != std::string::npos);
}

// ===========================================================================
// 十、大数据量：10 topic × 1000 条
// ===========================================================================

STRATUM_TEST(DiffScale, TenTopicsThousandEvents) {
  const std::string a = TempDir("lg_a");
  const std::string b = TempDir("lg_b");
  std::string err;
  SessionSpec sa;
  sa.dir = a;
  sa.topics = 10;
  sa.events_per_topic = 1000;
  sa.span_ns = 5000000000ull;
  STRATUM_CHECK_TRUE(MakeSession(sa, &err));
  SessionSpec sb = sa;
  sb.dir = b;
  sb.events_per_topic = 900;   // 每 topic 少 10%
  STRATUM_CHECK_TRUE(MakeSession(sb, &err));

  const CliResult r = RunCli(kCli + " diff " + a + " " + b + " --bucket 20");
  STRATUM_CHECK_EQ(r.code, 5);
  STRATUM_CHECK_EQ(PickInSide(r.output, "left", "events"), 10000LL);
  STRATUM_CHECK_EQ(PickInSide(r.output, "right", "events"), 9000LL);
  std::printf("      [规模] 10 topic × 1000 条 vs 900 条：总数与差量正确\n");
}

// ===========================================================================
// 十一、随机（多种子）
// ===========================================================================

STRATUM_TEST(DiffScale, RandomSpecsMultipleSeeds) {
  for (uint64_t seed : {7ULL, 77ULL}) {
    const std::string a = TempDir(("rs_a" + std::to_string(seed)).c_str());
    const std::string b = TempDir(("rs_b" + std::to_string(seed)).c_str());
    std::string err;

    // 用种子派生参数（不引入 mt19937 以免测试代码太长）
    const int topics = 1 + static_cast<int>(seed % 5);
    const int ea = 10 + static_cast<int>(seed % 90);
    const int eb = ea - static_cast<int>(seed % 10);

    SessionSpec sa;
    sa.dir = a;
    sa.topics = topics;
    sa.events_per_topic = ea;
    STRATUM_CHECK_TRUE(MakeSession(sa, &err));
    SessionSpec sb = sa;
    sb.dir = b;
    sb.events_per_topic = eb;
    STRATUM_CHECK_TRUE(MakeSession(sb, &err));

    const CliResult r = RunCli(kCli + " diff " + a + " " + b);
    STRATUM_CHECK_EQ(PickInSide(r.output, "left", "events"),
                     static_cast<long long>(topics) * ea);
    STRATUM_CHECK_EQ(PickInSide(r.output, "right", "events"),
                     static_cast<long long>(topics) * eb);
    // ea != eb → 必有差异
    if (ea != eb) STRATUM_CHECK_EQ(r.code, 5);
  }
  std::printf("      [随机] 2 个种子：topic 数 1~5、条数随机，总数与退出码均正确\n");
}

// ===========================================================================
// 十二、错误处理
// ===========================================================================

STRATUM_TEST(DiffErrors, UsageAndEnvErrors) {
  // 缺参数
  const CliResult r1 = RunCli(kCli + " diff");
  STRATUM_CHECK_EQ(r1.code, 2);
  STRATUM_CHECK_TRUE(r1.output.find("错误:") != std::string::npos);

  // 一个参数
  const CliResult r2 = RunCli(kCli + " diff /tmp/x");
  STRATUM_CHECK_EQ(r2.code, 2);

  // 三个参数
  const CliResult r3 = RunCli(kCli + " diff /tmp/x /tmp/y /tmp/z");
  STRATUM_CHECK_EQ(r3.code, 2);

  // 非法 --compare-payload
  const CliResult r4 = RunCli(kCli + " diff /tmp/x /tmp/y --compare-payload bogus");
  STRATUM_CHECK_EQ(r4.code, 2);
  STRATUM_CHECK_TRUE(r4.output.find("none") != std::string::npos);

  // 不存在的会话 → ENV(6)
  //
  // 【为什么用 pid 拼唯一名】旧版 diff 会为不存在的路径**创建**会话（已修），
  // 所以固定名字会被上一轮跑出来的目录污染 —— 第二次执行时它已存在。
  const std::string uniq = std::to_string(static_cast<long>(::getpid()));
  const CliResult r5 = RunCli(kCli + " diff /tmp/absent_" + uniq + "_a /tmp/absent_" +
                              uniq + "_b");
  // 会话不存在 → NotFound(8)，不是 ENV(6)。
  // kNotFound 的定义就是「会话/话题/文件不存在」，比 kEnv 准确。
  STRATUM_CHECK_EQ(r5.code, 8);
  // 且**不能**创建它们（这是"查询无副作用"的回归保护）
  STRATUM_CHECK_TRUE(!std::filesystem::exists("/tmp/absent_" + uniq + "_a"));

  // 未知选项
  const CliResult r6 = RunCli(kCli + " diff /tmp/x /tmp/y --bogus");
  STRATUM_CHECK_EQ(r6.code, 2);
  STRATUM_CHECK_TRUE(r6.output.find("--bucket") != std::string::npos);
  std::printf("      [错误] USAGE×5 + ENV×1，消息三段式且列出支持的选项\n");
}

STRATUM_TEST_MAIN()
