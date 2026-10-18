// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// **metrics 的业务层指标**（含与 verify 的跨命令对账）
//
// 覆盖 docs/14 建议的第 4 项：让 Prometheus 一条告警规则能同时覆盖
// 「数据断了」与「业务层没有数据」。
//
// 四个新指标：
//     stratum_business_events_total{session}   业务 topic 的事件数
//     stratum_business_topics_total{session}   业务 topic 数
//     stratum_probe_events_total{session}      探针 topic 的事件数
//     stratum_probe_topics_total{session}      探针 topic 数
//
// 【为什么只给条数，不给"失败条数"】
// Prometheus 默认 15s 抓一次。要数"探针里有多少条 result=error"必须扫全量 payload ——
// 那会拖垮抓取端点。**深度信号留给 `verify --business-signals`（按需人工执行）。**
// 这是"性能与深度分离"，与 diff 的 --compare-payload 同理。

// 标准库头文件必须最先包含
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"
#include "test_isolation.h"

namespace fs = std::filesystem;
using namespace stratum;

namespace {

#ifndef MB_CLI_PATH
#define MB_CLI_PATH "./stratum"
#endif

std::string kCli = MB_CLI_PATH;

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
  const std::string d = std::string(home ? home : ".") + "/stratum_mb_" + tag + "_" + UniqueSuffix();
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

bool MakeSession(const std::string& dir, int biz_topics, int biz_events,
                 int probe_topics, int probe_events, std::string* err) {
  TimelineImplV2 t;
  if (!t.Open(dir).ok()) {
    if (err) *err = "Open 失败";
    return false;
  }
  auto Add = [&](const std::string& name, int n) -> bool {
    TopicMetadata m;
    m.topic_name = name;
    m.type_name = "T";
    uint32_t id = 0;
    if (!t.RegisterTopic(m, &id).ok()) return false;
    std::vector<Event> ev;
    ev.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      Event e;
      e.topic_id = id;
      e.seq = static_cast<uint32_t>(i + 1);
      e.source_timestamp_ns = 1700000000000000000ull +
                              static_cast<TimestampNs>(i) * 10000000ull;
      e.reception_timestamp_ns = e.source_timestamp_ns + 100;
      e.payload = "x";
      ev.push_back(std::move(e));
    }
    return t.AppendBatch(ev).ok();
  };
  for (int i = 0; i < biz_topics; ++i) {
    if (!Add("biz/t" + std::to_string(i), biz_events)) {
      t.Close();
      if (err) *err = "业务 topic 失败";
      return false;
    }
  }
  // ★ 前缀是 "_probe."（点）
  for (int i = 0; i < probe_topics; ++i) {
    if (!Add("_probe.p" + std::to_string(i), probe_events)) {
      t.Close();
      if (err) *err = "探针 topic 失败";
      return false;
    }
  }
  t.Close();
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

// 从 Prometheus 文本里取某指标的值
long long PickProm(const std::string& text, const std::string& name) {
  const std::string pat = name + "{";
  const size_t at = text.find(pat);
  if (at == std::string::npos) return -1;
  const size_t close = text.find('}', at);
  if (close == std::string::npos) return -1;
  return std::strtoll(text.c_str() + close + 1, nullptr, 10);
}

}  // namespace

// ===========================================================================
// 一、★ 业务层与探针层分开统计
// ===========================================================================

STRATUM_TEST(MetricsBusiness, LayersSeparated) {
  const std::string dir = TempDir("sep");
  std::string err;
  // 3 业务 topic × 20 = 60；2 探针 topic × 15 = 30
  STRATUM_CHECK_TRUE(MakeSession(dir, 3, 20, 2, 15, &err));

  const CliResult r = RunCli(kCli + " metrics " + dir);
  STRATUM_CHECK_EQ(r.code, 0);

  STRATUM_CHECK_EQ(PickProm(r.output, "stratum_business_events_total"), 60LL);
  STRATUM_CHECK_EQ(PickProm(r.output, "stratum_business_topics_total"), 3LL);
  STRATUM_CHECK_EQ(PickProm(r.output, "stratum_probe_events_total"), 30LL);
  STRATUM_CHECK_EQ(PickProm(r.output, "stratum_probe_topics_total"), 2LL);
  // 会话总数仍应为 90（原有指标不受影响）
  STRATUM_CHECK_EQ(PickProm(r.output, "stratum_session_events_total"), 90LL);
  std::printf("      [分层] 业务 60/3，探针 30/2，会话总数 90（不变）\n");
}

// ===========================================================================
// 二、★ 与 verify 对账（跨命令一致性）
// ===========================================================================
//
// metrics 与 verify 必须给出相同的分层数字。
// 两条命令各说一套数，监控告警与人工排查就会对不上。

STRATUM_TEST(MetricsBusiness, ConsistentWithVerify) {
  const std::string dir = TempDir("xchk");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 2, 25, 1, 12, &err));

  const CliResult v = RunCli(kCli + " verify " + dir);
  const CliResult m = RunCli(kCli + " metrics --json " + dir);

  const long long v_dds = PickInt(v.output, "dds_events");
  const long long v_probe = PickInt(v.output, "probe_events");
  const long long m_biz = PickInt(m.output, "business_events_total");
  const long long m_probe = PickInt(m.output, "probe_events_total");

  // ★ 不变量
  STRATUM_CHECK_EQ(v_dds, 50LL);    // 2 × 25
  STRATUM_CHECK_EQ(v_probe, 12LL);  // 1 × 12
  STRATUM_CHECK_EQ(m_biz, v_dds);
  STRATUM_CHECK_EQ(m_probe, v_probe);
  std::printf("      [对账] verify(dds=%lld,probe=%lld) == metrics(biz=%lld,probe=%lld)\n",
              v_dds, v_probe, m_biz, m_probe);
}

// ===========================================================================
// 三、JSON 与 Prometheus 两种输出给出同样的数字
// ===========================================================================

STRATUM_TEST(MetricsBusiness, JsonMatchesPrometheus) {
  const std::string dir = TempDir("fmt");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 2, 30, 2, 20, &err));

  const CliResult p = RunCli(kCli + " metrics " + dir);
  const CliResult j = RunCli(kCli + " metrics --json " + dir);

  // Prometheus 文本 ↔ JSON 必须一致
  STRATUM_CHECK_EQ(PickProm(p.output, "stratum_business_events_total"),
                   PickInt(j.output, "business_events_total"));
  STRATUM_CHECK_EQ(PickProm(p.output, "stratum_probe_events_total"),
                   PickInt(j.output, "probe_events_total"));
  STRATUM_CHECK_EQ(PickProm(p.output, "stratum_business_topics_total"),
                   PickInt(j.output, "business_topics_total"));
  STRATUM_CHECK_EQ(PickProm(p.output, "stratum_probe_topics_total"),
                   PickInt(j.output, "probe_topics_total"));
  std::printf("      [一致] Prometheus 文本与 JSON 四种指标值完全相同\n");
}

// ===========================================================================
// 四、--probe-prefix 改变分类
// ===========================================================================

STRATUM_TEST(MetricsBusiness, CustomPrefixReclassifies) {
  const std::string dir = TempDir("pfx");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 2, 10, 1, 10, &err));

  // 默认前缀：1 个探针 topic
  const CliResult a = RunCli(kCli + " metrics " + dir);
  STRATUM_CHECK_EQ(PickProm(a.output, "stratum_probe_topics_total"), 1LL);

  // 换前缀 → 所有 topic 都算业务
  const CliResult b = RunCli(kCli + " metrics --probe-prefix zzz. " + dir);
  STRATUM_CHECK_EQ(PickProm(b.output, "stratum_probe_topics_total"), 0LL);
  STRATUM_CHECK_EQ(PickProm(b.output, "stratum_business_topics_total"), 3LL);
  std::printf("      [前缀] 换成 zzz. 后探针 topic 归零、业务 topic 变 3\n");
}

// ===========================================================================
// 五、没有探针 topic 时指标为 0（不是缺失）
// ===========================================================================

STRATUM_TEST(MetricsBusiness, ZeroProbeStillReported) {
  const std::string dir = TempDir("noprobe");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 2, 10, 0, 0, &err));

  const CliResult r = RunCli(kCli + " metrics " + dir);
  // ★ 必须输出 0，而不是不输出 —— 否则 Prometheus 里会出现"指标消失"，
  // 与"值为 0"是两回事（前者会让告警规则失效）
  STRATUM_CHECK_TRUE(r.output.find("stratum_probe_events_total{") != std::string::npos);
  STRATUM_CHECK_EQ(PickProm(r.output, "stratum_probe_events_total"), 0LL);
  STRATUM_CHECK_EQ(PickProm(r.output, "stratum_probe_topics_total"), 0LL);
  STRATUM_CHECK_EQ(PickProm(r.output, "stratum_business_events_total"), 20LL);
  std::printf("      [零值] 无探针 topic 时指标输出 0 而非缺失（告警规则才有效）\n");
}

STRATUM_TEST_MAIN()
