// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// **diff 的对称性与幂等性**
//
// 不变量：
//   · 对称：diff(A,B) 与 diff(B,A) 的差异**集合**必须相同（只是左右互换）
//   · 幂等：同一对会话连比三次，结论必须完全一致
//   · 自反：diff(A,A) 恒为无差异
//
// 【为什么需要这个套件】
// 对称性和幂等性是**最容易被实现细节破坏**的性质：
//   · 若某处用了"取第一个"而不是"取全部"，结果就依赖遍历顺序
//   · 若把 std::map 换成 unordered_map 后忘了排序，输出顺序会抖动
//   · 若某处有隐藏的状态残留，连比两次结果就可能不同
//
// 这三条都是**不会崩溃、不会报错**的错误 —— 只会让"同一件事问两次得到两个答案"，
// 而那比崩溃更难查。

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

#ifndef SYM_CLI_PATH
#define SYM_CLI_PATH "./stratum"
#endif

std::string kCli = SYM_CLI_PATH;

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
  const std::string d = std::string(home ? home : ".") + "/stratum_sym_" + tag;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

bool MakeSession(const std::string& dir, int topics, int events,
                 uint64_t span_ns, std::string* err) {
  TimelineImplV2 timeline;
  if (!timeline.Open(dir).ok()) {
    if (err) *err = "Open 失败";
    return false;
  }
  for (int t = 0; t < topics; ++t) {
    TopicMetadata meta;
    meta.topic_name = "s/t" + std::to_string(t);
    meta.type_name = "T";
    uint32_t tid = 0;
    if (!timeline.RegisterTopic(meta, &tid).ok()) {
      if (err) *err = "RegisterTopic 失败";
      timeline.Close();
      return false;
    }
    std::vector<Event> ev;
    ev.reserve(static_cast<size_t>(events));
    for (int i = 0; i < events; ++i) {
      Event e;
      e.topic_id = tid;
      e.seq = static_cast<uint32_t>(i + 1);
      const double frac = (events > 1) ? static_cast<double>(i) / (events - 1) : 0.0;
      e.source_timestamp_ns = 1700000000000000000ull +
                              static_cast<TimestampNs>(frac * static_cast<double>(span_ns));
      e.reception_timestamp_ns = e.source_timestamp_ns + 100;
      e.payload = "p";
      ev.push_back(std::move(e));
    }
    if (!timeline.AppendBatch(ev).ok()) {
      if (err) *err = "AppendBatch 失败";
      timeline.Close();
      return false;
    }
  }
  timeline.Close();
  return true;
}

// 从 JSON 里抽出"差异描述集合"，用于对称性比较。
//
// 只取**与方向无关的事实**：topic 名、两侧条数（排序后）、delta 绝对值。
// 不取 left/right 标签 —— 那本来就该互换。
struct DiffFact {
  std::string topic;
  long long min_events = 0;
  long long max_events = 0;
  long long abs_delta = 0;
};

std::vector<DiffFact> ExtractFacts(const std::string& text) {
  std::vector<DiffFact> out;
  // 扫所有 "topic":"xxx","is_probe":...,"left_events":A,"right_events":B
  size_t p = 0;
  while (true) {
    const size_t tp = text.find("\"topic\":\"", p);
    if (tp == std::string::npos) break;
    const size_t name_b = tp + 9;
    const size_t name_e = text.find('"', name_b);
    if (name_e == std::string::npos) break;
    const std::string name = text.substr(name_b, name_e - name_b);

    const size_t le = text.find("\"left_events\":", name_e);
    const size_t re = text.find("\"right_events\":", name_e);
    if (le == std::string::npos || re == std::string::npos) { p = name_e; continue; }
    const long long lv = std::strtoll(text.c_str() + le + 14, nullptr, 10);
    const long long rv = std::strtoll(text.c_str() + re + 15, nullptr, 10);

    DiffFact f;
    f.topic = name;
    f.min_events = std::min(lv, rv);
    f.max_events = std::max(lv, rv);
    f.abs_delta = std::llabs(lv - rv);
    out.push_back(f);
    p = name_e;
  }
  // 排序，消除遍历顺序影响
  std::sort(out.begin(), out.end(), [](const DiffFact& a, const DiffFact& b) {
    if (a.topic != b.topic) return a.topic < b.topic;
    if (a.min_events != b.min_events) return a.min_events < b.min_events;
    return a.abs_delta < b.abs_delta;
  });
  return out;
}

}  // namespace

// ===========================================================================
// 一、★ 自反：diff(A,A) 恒为无差异
// ===========================================================================

STRATUM_TEST(DiffSymmetry, Reflexive) {
  const std::string a = TempDir("refl");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(a, 4, 60, 2000000000ull, &err));

  // 连跑三次，每次都必须是 0
  for (int i = 0; i < 3; ++i) {
    const CliResult r = RunCli(kCli + " diff " + a + " " + a);
    STRATUM_CHECK_EQ(r.code, 0);
    STRATUM_CHECK_TRUE(r.output.find("\"changed\":[]") != std::string::npos);
  }
  std::printf("      [自反] diff(A,A) 连跑三次均为无差异、退出码 0\n");
}

// ===========================================================================
// 二、★ 幂等：同一对会话连比三次，差异集合完全一致
// ===========================================================================

STRATUM_TEST(DiffSymmetry, Idempotent) {
  const std::string a = TempDir("idem_a");
  const std::string b = TempDir("idem_b");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(a, 3, 100, 1000000000ull, &err));
  STRATUM_CHECK_TRUE(MakeSession(b, 3, 80, 900000000ull, &err));

  std::vector<DiffFact> first;
  for (int i = 0; i < 3; ++i) {
    const CliResult r = RunCli(kCli + " diff " + a + " " + b);
    STRATUM_CHECK_EQ(r.code, 5);
    const auto facts = ExtractFacts(r.output);
    if (i == 0) {
      first = facts;
      STRATUM_CHECK_EQ(first.size(), static_cast<size_t>(3));   // 3 个 topic 都变了
    } else {
      // ★ 完全一致
      STRATUM_CHECK_EQ(facts.size(), first.size());
      for (size_t k = 0; k < facts.size() && k < first.size(); ++k) {
        STRATUM_CHECK_EQ(facts[k].topic, first[k].topic);
        STRATUM_CHECK_EQ(facts[k].min_events, first[k].min_events);
        STRATUM_CHECK_EQ(facts[k].max_events, first[k].max_events);
        STRATUM_CHECK_EQ(facts[k].abs_delta, first[k].abs_delta);
      }
    }
  }
  std::printf("      [幂等] 连比三次，3 个 topic 的差异集合完全一致\n");
}

// ===========================================================================
// 三、★ 对称：diff(A,B) 与 diff(B,A) 的差异集合相同（左右互换）
// ===========================================================================

STRATUM_TEST(DiffSymmetry, SymmetricUnderSwap) {
  const std::string a = TempDir("sym_a");
  const std::string b = TempDir("sym_b");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(a, 2, 100, 1500000000ull, &err));
  STRATUM_CHECK_TRUE(MakeSession(b, 2, 70, 1200000000ull, &err));

  const CliResult ab = RunCli(kCli + " diff " + a + " " + b);
  const CliResult ba = RunCli(kCli + " diff " + b + " " + a);
  STRATUM_CHECK_EQ(ab.code, 5);
  STRATUM_CHECK_EQ(ba.code, 5);

  const auto fab = ExtractFacts(ab.output);
  const auto fba = ExtractFacts(ba.output);
  STRATUM_CHECK_EQ(fab.size(), fba.size());
  // ★ 与方向无关的事实必须完全相同
  for (size_t i = 0; i < fab.size() && i < fba.size(); ++i) {
    STRATUM_CHECK_EQ(fab[i].topic, fba[i].topic);
    STRATUM_CHECK_EQ(fab[i].min_events, fba[i].min_events);
    STRATUM_CHECK_EQ(fab[i].max_events, fba[i].max_events);
    STRATUM_CHECK_EQ(fab[i].abs_delta, fba[i].abs_delta);
  }
  // 方向确实反了：delta 符号相反
  STRATUM_CHECK_TRUE(ab.output.find("\"delta\":-30") != std::string::npos);
  STRATUM_CHECK_TRUE(ba.output.find("\"delta\":30") != std::string::npos);
  std::printf("      [对称] A→B 与 B→A 的差异集合一致，delta 符号相反（-30 / +30）\n");
}

// ===========================================================================
// 四、topic 增减在方向互换后应对调
// ===========================================================================

STRATUM_TEST(DiffSymmetry, AddRemoveSwapUnderSwap) {
  const std::string a = TempDir("ar_a");
  const std::string b = TempDir("ar_b");
  std::string err;
  // A：3 个 topic；B：2 个 topic（少一个）
  STRATUM_CHECK_TRUE(MakeSession(a, 3, 20, 1000000000ull, &err));
  STRATUM_CHECK_TRUE(MakeSession(b, 2, 20, 1000000000ull, &err));

  const CliResult ab = RunCli(kCli + " diff " + a + " " + b);
  const CliResult ba = RunCli(kCli + " diff " + b + " " + a);
  STRATUM_CHECK_EQ(ab.code, 5);
  STRATUM_CHECK_EQ(ba.code, 5);

  // A→B：一个 removed（s/t2）
  STRATUM_CHECK_TRUE(ab.output.find("\"removed\":[{\"topic\":\"s/t2\"") !=
                     std::string::npos);
  STRATUM_CHECK_EQ(ab.output.find("\"added\":[{\"topic\""), std::string::npos);
  // B→A：一个 added（s/t2）——★ 方向互换后 add/remove 对调
  STRATUM_CHECK_TRUE(ba.output.find("\"added\":[{\"topic\":\"s/t2\"") !=
                     std::string::npos);
  STRATUM_CHECK_EQ(ba.output.find("\"removed\":[{\"topic\""), std::string::npos);
  std::printf("      [对称] A→B 报 removed(s/t2)，B→A 报 added(s/t2)\n");
}

STRATUM_TEST_MAIN()
