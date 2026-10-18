// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// **verify 与 diff 的一致性**（跨命令不变量）
//
// 这不是 diff 的单元测试，而是**两个命令之间的契约**：
//   · diff 报的 left.events / right.events 必须等于对应会话的 verify events
//   · diff 报"无差异"时，两侧 verify 的关键结论必须一致
//   · 两者对"探针 topic"的分类必须一致（同一个前缀约定）
//
// 【为什么需要这个套件】
// verify 和 diff 各自有测试，但**没有人检查它们对同一份数据的结论是否自洽**。
// 两条命令各说一套数，用户就不知道该信谁 —— 而这种不一致不会报错，
// 只会让人在复盘时对不上账。
//
// 这类"跨命令不变量"是最容易在重构中被悄悄破坏的：
// 改了一边忘了另一边，两边的测试都还绿着。

// 标准库头文件必须最先包含
#include <chrono>
#include <cmath>
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

#ifndef XCHECK_CLI_PATH
#define XCHECK_CLI_PATH "./stratum"
#endif

std::string kCli = XCHECK_CLI_PATH;

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
  const std::string d = std::string(home ? home : ".") + "/stratum_xc_" + tag + "_" + UniqueSuffix();
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

// 在 `"side":{` 段里取整数字段（避免 key 在多处重名）
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

// 造会话：biz_topics 个业务 topic + probe_topics 个探针 topic
bool MakeSession(const std::string& dir, int biz_topics, int biz_events,
                 int probe_topics, int probe_events, std::string* err) {
  TimelineImplV2 timeline;
  if (!timeline.Open(dir).ok()) {
    if (err) *err = "Open 失败";
    return false;
  }
  auto Add = [&](const std::string& name, int n) -> bool {
    TopicMetadata meta;
    meta.topic_name = name;
    meta.type_name = "T";
    uint32_t tid = 0;
    if (!timeline.RegisterTopic(meta, &tid).ok()) return false;
    std::vector<Event> ev;
    ev.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      Event e;
      e.topic_id = tid;
      e.seq = static_cast<uint32_t>(i + 1);
      // 每 10ms 一条，保证跨度可预测
      e.source_timestamp_ns = 1700000000000000000ull +
                              static_cast<TimestampNs>(i) * 10000000ull;
      e.reception_timestamp_ns = e.source_timestamp_ns + 500;
      e.payload = "x";
      ev.push_back(std::move(e));
    }
    return timeline.AppendBatch(ev).ok();
  };

  for (int t = 0; t < biz_topics; ++t) {
    if (!Add("biz/t" + std::to_string(t), biz_events)) {
      if (err) *err = "业务 topic 写入失败";
      timeline.Close();
      return false;
    }
  }
  // ★ 前缀是 "_probe."（点），与 cmd_diff / cmd_verify 的默认值一致
  for (int t = 0; t < probe_topics; ++t) {
    if (!Add("_probe.p" + std::to_string(t), probe_events)) {
      if (err) *err = "探针 topic 写入失败";
      timeline.Close();
      return false;
    }
  }
  timeline.Close();
  return true;
}

}  // namespace

// ===========================================================================
// 一、★ diff 的 events 必须等于 verify 的 events
// ===========================================================================
//
// 两条命令读同一份数据。若 diff 报 1000 而 verify 报 998，用户不知道信谁。

STRATUM_TEST(CrossCheck, DiffEventsMatchVerifyEvents) {
  const std::string dir = TempDir("ev");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 3, 40, 1, 10, &err));   // 3×40 + 10 = 130

  const CliResult v = RunCli(kCli + " verify " + dir);
  const CliResult d = RunCli(kCli + " diff " + dir + " " + dir);
  STRATUM_CHECK_EQ(v.code, 0);
  STRATUM_CHECK_EQ(d.code, 0);   // 自己比自己无差异

  const long long verify_events = PickInt(v.output, "events");
  const long long diff_left = PickInSide(d.output, "left", "events");
  const long long diff_right = PickInSide(d.output, "right", "events");

  STRATUM_CHECK_EQ(verify_events, 130LL);
  // ★ 不变量
  STRATUM_CHECK_EQ(diff_left, verify_events);
  STRATUM_CHECK_EQ(diff_right, verify_events);
  std::printf("      [一致] verify events=%lld == diff 两侧 events=%lld\n",
              verify_events, diff_left);
}

// ===========================================================================
// 二、★ 探针分类必须一致（同一个前缀约定的两个消费者）
// ===========================================================================

STRATUM_TEST(CrossCheck, ProbeClassificationIsConsistent) {
  const std::string dir = TempDir("probe");
  std::string err;
  // 2 个业务 topic × 30 条 = 60；2 个探针 topic × 25 条 = 50
  STRATUM_CHECK_TRUE(MakeSession(dir, 2, 30, 2, 25, &err));

  const CliResult v = RunCli(kCli + " verify " + dir);
  const CliResult d = RunCli(kCli + " diff " + dir + " " + dir);

  // verify 的业务层
  const long long v_probe = PickInt(v.output, "probe_events");
  const long long v_dds = PickInt(v.output, "dds_events");

  // diff 的业务层（在 business_layer.left 里）
  const size_t bl_at = d.output.find("\"business_layer\":{\"left\":{");
  STRATUM_CHECK_TRUE(bl_at != std::string::npos);
  const std::string seg = d.output.substr(bl_at, 300);
  const size_t de = seg.find("\"dds_events\":");
  const size_t pe = seg.find("\"probe_events\":");
  STRATUM_CHECK_TRUE(de != std::string::npos);
  STRATUM_CHECK_TRUE(pe != std::string::npos);
  const long long d_dds = std::strtoll(seg.c_str() + de + 13, nullptr, 10);
  const long long d_probe = std::strtoll(seg.c_str() + pe + 15, nullptr, 10);

  // ★ 不变量：两条命令对"哪些是探针 topic"的判断必须一致
  STRATUM_CHECK_EQ(v_dds, 60LL);
  STRATUM_CHECK_EQ(v_probe, 50LL);
  STRATUM_CHECK_EQ(d_dds, v_dds);
  STRATUM_CHECK_EQ(d_probe, v_probe);
  std::printf("      [一致] 探针分类：verify(%lld/%lld) == diff(%lld/%lld)\n",
              v_dds, v_probe, d_dds, d_probe);
}

// ===========================================================================
// 三、★ diff 报"无差异"时，两侧 verify 必须给出相同结论
// ===========================================================================
//
// 这是"diff 说一样"这件事的可信度来源。若 diff 说无差异，
// 但两个会话的 verify 结论不同（一个有缺口一个没有），那 diff 就在骗人。

STRATUM_TEST(CrossCheck, NoDiffImpliesSameVerifyVerdict) {
  const std::string a = TempDir("nv_a");
  const std::string b = TempDir("nv_b");
  std::string err;
  // 完全相同的两份（内容、条数、分布）
  STRATUM_CHECK_TRUE(MakeSession(a, 2, 20, 1, 5, &err));
  STRATUM_CHECK_TRUE(MakeSession(b, 2, 20, 1, 5, &err));

  const CliResult d = RunCli(kCli + " diff " + a + " " + b);
  STRATUM_CHECK_EQ(d.code, 0);   // 无差异

  const CliResult va = RunCli(kCli + " verify " + a);
  const CliResult vb = RunCli(kCli + " verify " + b);

  // ★ 不变量：无差异 ⇒ 结论一致
  const bool va_ok = va.output.find("\"has_integrity_issue\":false") != std::string::npos;
  const bool vb_ok = vb.output.find("\"has_integrity_issue\":false") != std::string::npos;
  STRATUM_CHECK_EQ(va_ok, vb_ok);
  STRATUM_CHECK_EQ(PickInt(va.output, "events"), PickInt(vb.output, "events"));
  std::printf("      [一致] diff 报无差异 → 两侧 verify 结论与条数都相同\n");
}

// ===========================================================================
// 四、diff 说"有差异"时，条数差必须与两侧 verify 的条数差吻合
// ===========================================================================

STRATUM_TEST(CrossCheck, DiffDeltaMatchesVerifyDifference) {
  const std::string a = TempDir("dl_a");
  const std::string b = TempDir("dl_b");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(a, 2, 50, 0, 0, &err));   // 100
  STRATUM_CHECK_TRUE(MakeSession(b, 2, 35, 0, 0, &err));   // 70

  const CliResult va = RunCli(kCli + " verify " + a);
  const CliResult vb = RunCli(kCli + " verify " + b);
  const CliResult d = RunCli(kCli + " diff " + a + " " + b);

  const long long ea = PickInt(va.output, "events");
  const long long eb = PickInt(vb.output, "verify_nonexistent_key");  // 用下面的方式
  (void)eb;
  const long long eb2 = PickInt(vb.output, "events");

  // ★ 不变量：diff 的 left/right events 就是各会话 verify 的 events
  STRATUM_CHECK_EQ(PickInSide(d.output, "left", "events"), ea);
  STRATUM_CHECK_EQ(PickInSide(d.output, "right", "events"), eb2);
  // 且差量 = 两者之差
  STRATUM_CHECK_EQ(ea - eb2, 30LL);
  std::printf("      [一致] diff(100 vs 70) 与 verify(100) / verify(70) 吻合\n");
}

// ===========================================================================
// 五、--probe-prefix 变更必须在两个命令上表现一致
// ===========================================================================

STRATUM_TEST(CrossCheck, CustomPrefixBehavesSameInBoth) {
  const std::string dir = TempDir("pf");
  std::string err;
  // 2 业务 + 1 探针（_probe.p0，10 条）
  STRATUM_CHECK_TRUE(MakeSession(dir, 2, 10, 1, 10, &err));

  // 换一个不匹配任何 topic 的前缀 → 所有 topic 都算业务
  const CliResult v = RunCli(kCli + " verify --probe-prefix zzz. " + dir);
  const CliResult d = RunCli(kCli + " diff --probe-prefix zzz. " + dir + " " + dir);

  // verify: probe_events 应为 0
  STRATUM_CHECK_EQ(PickInt(v.output, "probe_events"), 0LL);
  // diff: business_layer 的 probe_events 也应为 0
  const size_t at = d.output.find("\"business_layer\":{\"left\":{");
  STRATUM_CHECK_TRUE(at != std::string::npos);
  const std::string seg = d.output.substr(at, 300);
  const size_t pe = seg.find("\"probe_events\":");
  STRATUM_CHECK_TRUE(pe != std::string::npos);
  const long long d_probe = std::strtoll(seg.c_str() + pe + 15, nullptr, 10);
  STRATUM_CHECK_EQ(d_probe, 0LL);
  std::printf("      [一致] --probe-prefix zzz. 使两个命令都判 0 个探针 topic\n");
}

STRATUM_TEST_MAIN()
