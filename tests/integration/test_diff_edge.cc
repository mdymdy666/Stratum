// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// **diff 的极端边界**：只在边界上才暴露的算术问题
//
// 覆盖的都是"平时跑不到，一旦触发就静默错"的情况：
//   · span = 0（只有一条事件）→ 分桶会除零
//   · bucket = 1 / 1000 → 桶数极端
//   · left.events = 0 → delta_pct 会除零
//   · 时间戳全部相同 → 所有事件落进同一个桶
//   · 单 topic 单事件（最小非空会话）
//   · 极端 payload 长度（0 字节 / 1 字节）
//
// 【为什么单独一个文件】
// test_diff.cc 覆盖的是正常路径与主要场景；本文件只盯**算术边界**。
// 这类错误的特征是：不崩溃、不报错，只是给出一个荒谬的数字
// （比如 delta_pct = inf，或者所有事件挤在第 0 桶）。

// 标准库头文件必须最先包含
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"

namespace fs = std::filesystem;
using namespace stratum;

namespace {

#ifndef EDGE_CLI_PATH
#define EDGE_CLI_PATH "./stratum"
#endif

std::string kCli = EDGE_CLI_PATH;

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

// 只捕获 stdout —— 用于 JSON 形状检查。
//
// 【为什么需要它】RunCli 用 `2>&1` 合并捕获，于是 stderr 的警告会混进输出，
// 末尾不再是 `}` → 形状检查失败。
// 这不是代码 bug，是对"stdout 才是数据出口"这条纪律的又一次印证：
// **连测试自己都得遵守它。**
std::string RunStdoutOnly(const std::string& cmd) {
  const std::string full = cmd + " 2>/dev/null";
  std::FILE* p = ::popen(full.c_str(), "r");
  if (p == nullptr) return {};
  std::string s;
  char buf[16384];
  while (std::fgets(buf, sizeof(buf), p) != nullptr) s += buf;
  ::pclose(p);
  return s;
}

std::string TempDir(const char* tag) {
  const char* home = std::getenv("HOME");
  const std::string d = std::string(home ? home : ".") + "/stratum_de_" + tag;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

// 精确控制每个事件的时间戳与负载 —— 边界测试必须能手造"所有时间戳相同"这种状态
struct EvSpec {
  TimestampNs ts;
  std::string payload;
};

bool MakeSessionExact(const std::string& dir, const std::vector<EvSpec>& events,
                      std::string* err) {
  TimelineImplV2 timeline;
  if (!timeline.Open(dir).ok()) {
    if (err) *err = "Open 失败";
    return false;
  }
  if (events.empty()) {
    timeline.Close();
    return true;   // 空会话
  }
  TopicMetadata meta;
  meta.topic_name = "e/t";
  meta.type_name = "T";
  uint32_t tid = 0;
  if (!timeline.RegisterTopic(meta, &tid).ok()) {
    timeline.Close();
    if (err) *err = "RegisterTopic 失败";
    return false;
  }
  std::vector<Event> ev;
  ev.reserve(events.size());
  for (size_t i = 0; i < events.size(); ++i) {
    Event e;
    e.topic_id = tid;
    e.seq = static_cast<uint32_t>(i + 1);
    e.source_timestamp_ns = events[i].ts;
    e.reception_timestamp_ns = events[i].ts + 100;
    e.payload = events[i].payload;
    ev.push_back(std::move(e));
  }
  const bool ok = timeline.AppendBatch(ev).ok();
  timeline.Close();
  if (!ok && err) *err = "AppendBatch 失败";
  return ok;
}

bool LooksLikeCompleteJson(const std::string& s) {
  if (s.empty()) return false;
  size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\n' || s[b] == '\r')) ++b;
  if (b >= s.size() || s[b] != '{') return false;
  size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
  return e > b && s[e - 1] == '}';
}

// 检查输出里没有 inf / nan（JSON 不允许，且说明除零）
bool HasNonFinite(const std::string& s) {
  return s.find("inf") != std::string::npos ||
         s.find("nan") != std::string::npos ||
         s.find("INF") != std::string::npos ||
         s.find("NAN") != std::string::npos;
}

}  // namespace

// ===========================================================================
// 一、★ span = 0（所有时间戳相同）→ 分桶不能除零
// ===========================================================================

STRATUM_TEST(DiffEdgeCase, ZeroSpanDoesNotDivideByZero) {
  const std::string a = TempDir("zs_a");
  const std::string b = TempDir("zs_b");
  std::string err;
  // 所有事件同一个时间戳 → span = 0
  std::vector<EvSpec> evs;
  for (int i = 0; i < 20; ++i) {
    evs.push_back({1700000000000000000ull, "x"});
  }
  STRATUM_CHECK_TRUE(MakeSessionExact(a, evs, &err));
  STRATUM_CHECK_TRUE(MakeSessionExact(b, evs, &err));

  const CliResult r = RunCli(kCli + " diff " + a + " " + b);
  // span=0 时两侧内容相同 → 无差异
  STRATUM_CHECK_EQ(r.code, 0);
  STRATUM_CHECK_TRUE(LooksLikeCompleteJson(r.output));
  // ★ 不能出现 inf/nan
  STRATUM_CHECK_FALSE(HasNonFinite(r.output));
  // span 应为 0
  STRATUM_CHECK_TRUE(r.output.find("\"span_ns\":0") != std::string::npos);
  std::printf("      [span=0] 所有时间戳相同：无差异、无 inf/nan、span_ns=0\n");
}

// ===========================================================================
// 二、★ left.events = 0（一侧空）→ delta_pct 不能除零
// ===========================================================================

STRATUM_TEST(DiffEdgeCase, EmptyLeftDoesNotDivideByZero) {
  const std::string a = TempDir("el_a");
  const std::string b = TempDir("el_b");
  std::string err;
  // A 空，B 有 10 条
  STRATUM_CHECK_TRUE(MakeSessionExact(a, {}, &err));
  std::vector<EvSpec> evs;
  for (int i = 0; i < 10; ++i) {
    evs.push_back({1700000000000000000ull + static_cast<TimestampNs>(i) * 1000000ull, "x"});
  }
  STRATUM_CHECK_TRUE(MakeSessionExact(b, evs, &err));

  const CliResult r = RunCli(kCli + " diff " + a + " " + b);
  // A 空 B 非空 → 有差异（added）
  STRATUM_CHECK_EQ(r.code, 5);
  // ★ JSON 形状只看 stdout（stderr 会有 duration 警告）
  STRATUM_CHECK_TRUE(LooksLikeCompleteJson(RunStdoutOnly(kCli + " diff " + a + " " + b)));
  // ★ 从 0 到 10 的百分比：约定为 +100%，不能是 inf
  STRATUM_CHECK_FALSE(HasNonFinite(r.output));
  STRATUM_CHECK_TRUE(r.output.find("\"delta_pct\":100.00") != std::string::npos);
  std::printf("      [空左侧] 0 → 10：delta_pct 约定为 +100%%（不是 inf）\n");
}

// ===========================================================================
// 三、★ 单条事件（最小非空会话）
// ===========================================================================

STRATUM_TEST(DiffEdgeCase, SingleEventSessions) {
  const std::string a = TempDir("se_a");
  const std::string b = TempDir("se_b");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSessionExact(a, {{1700000000000000000ull, "1"}}, &err));
  STRATUM_CHECK_TRUE(MakeSessionExact(b, {{1700000000000000000ull, "2"}}, &err));

  const CliResult r = RunCli(kCli + " diff " + a + " " + b);
  // 条数相同、时间相同、payload 不同 → 默认不比 payload → 无差异
  STRATUM_CHECK_EQ(r.code, 0);
  STRATUM_CHECK_TRUE(LooksLikeCompleteJson(r.output));
  STRATUM_CHECK_TRUE(r.output.find("\"events\":1") != std::string::npos);

  // 加 --compare-payload hash → 应检出差异
  const CliResult r2 = RunCli(kCli + " diff " + a + " " + b + " --compare-payload hash");
  STRATUM_CHECK_EQ(r2.code, 5);
  std::printf("      [单条] 1 条 vs 1 条：默认无差异；hash 模式检出负载不同\n");
}

// ===========================================================================
// 四、★ bucket 极端值（1 与 1000）
// ===========================================================================

STRATUM_TEST(DiffEdgeCase, ExtremeBucketCounts) {
  const std::string a = TempDir("eb_a");
  const std::string b = TempDir("eb_b");
  std::string err;
  std::vector<EvSpec> ea, eb;
  for (int i = 0; i < 50; ++i) {
    ea.push_back({1700000000000000000ull + static_cast<TimestampNs>(i) * 20000000ull, "x"});
  }
  // B 少一半
  for (int i = 0; i < 25; ++i) {
    eb.push_back({1700000000000000000ull + static_cast<TimestampNs>(i) * 20000000ull, "x"});
  }
  STRATUM_CHECK_TRUE(MakeSessionExact(a, ea, &err));
  STRATUM_CHECK_TRUE(MakeSessionExact(b, eb, &err));

  // bucket = 1：所有事件一个桶
  const CliResult r1 = RunCli(kCli + " diff " + a + " " + b + " --bucket 1");
  STRATUM_CHECK_EQ(r1.code, 5);
  STRATUM_CHECK_TRUE(LooksLikeCompleteJson(r1.output));
  STRATUM_CHECK_TRUE(r1.output.find("\"n\":1") != std::string::npos);
  STRATUM_CHECK_FALSE(HasNonFinite(r1.output));

  // bucket = 1000：桶比事件多（大量空桶）
  const CliResult r2 = RunCli(kCli + " diff " + a + " " + b + " --bucket 1000");
  STRATUM_CHECK_EQ(r2.code, 5);
  STRATUM_CHECK_TRUE(LooksLikeCompleteJson(r2.output));
  STRATUM_CHECK_TRUE(r2.output.find("\"n\":1000") != std::string::npos);
  STRATUM_CHECK_FALSE(HasNonFinite(r2.output));

  // bucket = 0：跳过分桶
  const CliResult r3 = RunCli(kCli + " diff " + a + " " + b + " --bucket 0");
  STRATUM_CHECK_EQ(r3.code, 5);   // 条数不同，仍应有差异
  STRATUM_CHECK_TRUE(r3.output.find("\"buckets\"") == std::string::npos);
  std::printf("      [桶数] bucket=1/1000/0 三种极端均正常，无 inf/nan\n");
}

// ===========================================================================
// 五、★ 极端 payload（0 字节 / 1 字节）
// ===========================================================================

STRATUM_TEST(DiffEdgeCase, ExtremePayloadSizes) {
  const std::string a = TempDir("ep_a");
  const std::string b = TempDir("ep_b");
  std::string err;
  std::vector<EvSpec> ea, eb;
  for (int i = 0; i < 10; ++i) {
    const TimestampNs ts = 1700000000000000000ull + static_cast<TimestampNs>(i) * 1000000ull;
    ea.push_back({ts, ""});      // 0 字节
    eb.push_back({ts, "z"});     // 1 字节
  }
  STRATUM_CHECK_TRUE(MakeSessionExact(a, ea, &err));
  STRATUM_CHECK_TRUE(MakeSessionExact(b, eb, &err));

  // 空负载的哈希也能算（FNV-1a 的初值），不应崩
  const CliResult r = RunCli(kCli + " diff " + a + " " + b + " --compare-payload hash");
  STRATUM_CHECK_TRUE(LooksLikeCompleteJson(r.output));
  // 0 字节 vs 1 字节 → 哈希不同 → 检出变化
  STRATUM_CHECK_EQ(r.code, 5);
  std::printf("      [负载] 0 字节 vs 1 字节：hash 模式能比且检出差异\n");
}

// ===========================================================================
// 六、两个空会话
// ===========================================================================

STRATUM_TEST(DiffEdgeCase, BothEmptyIsClean) {
  const std::string a = TempDir("be_a");
  const std::string b = TempDir("be_b");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSessionExact(a, {}, &err));
  STRATUM_CHECK_TRUE(MakeSessionExact(b, {}, &err));

  const CliResult r = RunCli(kCli + " diff " + a + " " + b);
  STRATUM_CHECK_EQ(r.code, 0);
  STRATUM_CHECK_TRUE(LooksLikeCompleteJson(r.output));
  STRATUM_CHECK_FALSE(HasNonFinite(r.output));
  // span=0、events=0
  STRATUM_CHECK_TRUE(r.output.find("\"events\":0") != std::string::npos);
  // 表格输出也不能崩
  const CliResult rt = RunCli(kCli + " --format table diff " + a + " " + b);
  STRATUM_CHECK_EQ(rt.code, 0);
  STRATUM_CHECK_TRUE(rt.output.find("会话对比") != std::string::npos);
  std::printf("      [双空] 两个空会话：JSON 与表格输出均正常，无差异\n");
}

STRATUM_TEST_MAIN()
