// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// `stratum session merge` 的测试
//
// 设计裁决见 docs/16-session-merge.md。本文件验证三个裁决的落地：
//   ① 时间轴：source_timestamp_ns + 显式 --offset，绝对合并，不自动校正
//   ② topic：同名合成一个、名字不变（单源合并 ≡ 复制，可用 diff 当硬判据）
//   ③ 去重：(topic, source_ts, payload_hash) 且仅跨源
//
// 【本文件最重要的两组用例】
//
//   1. **BroadcastDuplicatesAreMerged**：两个节点收到同一批广播 → 合并后是
//      **一份**，duplicates 精确。这是"合"与"拼"的分界线：若判错，条数翻倍
//      且所有下游计数一起错，而数字看起来完全自洽。
//
//   2. **ClockSkewDetectionAndOffset**：B 的源时间戳整体偏 100ms →
//      默认去重**失效**（这正是 docs/16 §二 写明的局限），报告必须**观测到**
//      这个偏差（clock_skew）；给 --offset 校正后去重恢复。
//      它验证的不是"能对齐"，而是"对齐规则的失效边界是否可观测"。
//
// 【为什么在测试里直接构造会话】
// 用 TimelineImplV2 的公开 API（RegisterTopic + AppendBatch）造会话，
// 比真实 record + DDS 快一个数量级，且能精确控制时间戳与 payload ——
// 这正是合并测试需要的（时钟偏差、同刻不同内容都是构造出来的）。

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"

#include "cli_runner.h"
#include "test_framework.h"

namespace fs = std::filesystem;
using namespace stratum;
using namespace stratum_cli_test;

namespace {

#ifndef MERGE_CLI_PATH
#define MERGE_CLI_PATH "./stratum"
#endif

const std::string kCli = MERGE_CLI_PATH;

// 测试会话放 $HOME（不放 /tmp：/tmp 可能被并行测试或系统清理干扰）
std::string TempDir(const std::string& tag) {
  const char* home = std::getenv("HOME");
  const std::string d = std::string(home ? home : ".") + "/stratum_merge_" + tag +
                        "_" + std::to_string(static_cast<long>(::getpid()));
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

struct EventSpec {
  TimestampNs ts = 0;
  std::string payload;
};

struct TopicSpec {
  std::string name;
  std::string type = "TestType";
  std::vector<EventSpec> events;
};

bool MakeSession(const std::string& dir, const std::vector<TopicSpec>& topics,
                 std::string* err) {
  TimelineImplV2 timeline;
  Status s = timeline.Open(dir);
  if (!s.ok()) {
    if (err) *err = "Open(" + dir + ") 失败: " + s.ToString();
    return false;
  }
  for (const auto& t : topics) {
    TopicMetadata meta;
    meta.topic_name = t.name;
    meta.type_name = t.type;
    uint32_t id = 0;
    s = timeline.RegisterTopic(meta, &id);
    if (!s.ok()) {
      if (err) *err = "RegisterTopic(" + t.name + ") 失败: " + s.ToString();
      timeline.Close();
      return false;
    }
    if (t.events.empty()) continue;
    std::vector<Event> events;
    events.reserve(t.events.size());
    for (size_t i = 0; i < t.events.size(); ++i) {
      Event e;
      e.topic_id = id;
      e.seq = static_cast<uint32_t>(i + 1);
      e.source_timestamp_ns = t.events[i].ts;
      e.reception_timestamp_ns = t.events[i].ts + 500;   // 本机接收，略晚
      e.payload = t.events[i].payload;
      events.push_back(std::move(e));
    }
    s = timeline.AppendBatch(events);
    if (!s.ok()) {
      if (err) *err = "AppendBatch(" + t.name + ") 失败: " + s.ToString();
      timeline.Close();
      return false;
    }
  }
  timeline.Close();
  return true;
}

// n 条、时间戳 origin + i*step、payload = prefix + i（**每条唯一**）
TopicSpec UniformTopic(const std::string& name, int n, TimestampNs origin,
                       uint64_t step, const std::string& prefix) {
  TopicSpec t;
  t.name = name;
  t.events.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    t.events.push_back(EventSpec{origin + static_cast<TimestampNs>(i) * step,
                                 prefix + std::to_string(i)});
  }
  return t;
}

// n 条、payload 全部相同（广播/心跳形态）
TopicSpec RepeatedTopic(const std::string& name, int n, TimestampNs origin,
                        uint64_t step, const std::string& payload) {
  TopicSpec t;
  t.name = name;
  t.events.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    t.events.push_back(EventSpec{origin + static_cast<TimestampNs>(i) * step,
                                 payload});
  }
  return t;
}

// --- JSON 取值 ---
// 【为什么不用一个"万能解析"】测试只需要几个标量；写全套 JSON 解析器
// 会引入一大段与被测功能无关的代码，出问题时还分不清是谁的 bug。
// 在 `"obj":{ ... }` 里取整数字段（第一个 '}' 为界，够用且不会误取嵌套外字段）
long long PickInObject(const std::string& text, const std::string& obj,
                       const std::string& field) {
  const std::string pat = "\"" + obj + "\":{";
  const size_t at = text.find(pat);
  if (at == std::string::npos) return -1;
  const size_t end = text.find('}', at);
  if (end == std::string::npos) return -1;
  const std::string seg = text.substr(at, end - at);
  const std::string fpat = "\"" + field + "\":";
  const size_t fp = seg.find(fpat);
  if (fp == std::string::npos) return -1;
  return std::strtoll(seg.c_str() + fp + fpat.size(), nullptr, 10);
}

// 在 `"arr":[{...},{...}]` 的第 idx 个元素里取整数字段
long long PickInArray(const std::string& text, const std::string& arr, size_t idx,
                      const std::string& field) {
  const std::string pat = "\"" + arr + "\":[";
  size_t p = text.find(pat);
  if (p == std::string::npos) return -1;
  p += pat.size();
  for (size_t i = 0; i <= idx; ++i) {
    p = text.find('{', p);
    if (p == std::string::npos) return -1;
    if (i != idx) {
      const size_t close = text.find('}', p);
      if (close == std::string::npos) return -1;
      p = close + 1;
    }
  }
  const size_t end = text.find('}', p);
  if (end == std::string::npos) return -1;
  const std::string seg = text.substr(p, end - p);
  const std::string fpat = "\"" + field + "\":";
  const size_t fp = seg.find(fpat);
  if (fp == std::string::npos) return -1;
  return std::strtoll(seg.c_str() + fp + fpat.size(), nullptr, 10);
}

bool HasSub(const std::string& text, const std::string& s) {
  return text.find(s) != std::string::npos;
}

}  // namespace

// ===========================================================================
// 一、单会话合并 ≡ 复制（退化情况）
// ===========================================================================

STRATUM_TEST(MergeBasic, SingleSourceIsCopy) {
  const std::string src = TempDir("copy_src");
  const std::string dst = TempDir("copy_dst") + "/merged";
  std::string err;

  std::vector<TopicSpec> topics;
  topics.push_back(UniformTopic("biz/a", 50, 1700000000000000000ull, 10000000ull, "a"));
  topics.push_back(UniformTopic("biz/b", 50, 1700000000000000000ull, 10000000ull, "b"));
  topics.push_back(UniformTopic("_probe.p0", 50, 1700000000000000000ull, 10000000ull, "p"));
  STRATUM_CHECK_TRUE(MakeSession(src, topics, &err));

  const CliResult m = RunCliJson(kCli + " session merge " + src + " --to " + dst);
  STRATUM_CHECK_EQ(m.code, 0);
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "events"), 150LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "topics"), 3LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 0, "contributed"), 150LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "dedupe", "duplicates_removed"), 0LL);
  STRATUM_CHECK_TRUE(HasSub(m.output, "\"mode\":\"absolute\""));
  STRATUM_CHECK_TRUE(HasSub(m.output, "\"clock\":\"source_timestamp_ns\""));
  // note 是契约的一部分（docs/16 §五）
  STRATUM_CHECK_TRUE(HasSub(m.output, "合并不判断数据对错"));

  // ★ 硬判据：用现有的 diff 比"源"与"合并产物" —— 必须 0 差异。
  // 这一条同时验证了：topic 名不变、时间戳不变、条数不变、payload 不变。
  const CliResult d = RunCliJson(kCli + " diff " + src + " " + dst);
  STRATUM_CHECK_EQ(d.code, 0);
  STRATUM_CHECK_TRUE(HasSub(d.output, "\"added\":[]"));
  STRATUM_CHECK_TRUE(HasSub(d.output, "\"removed\":[]"));
  STRATUM_CHECK_TRUE(HasSub(d.output, "\"changed\":[]"));

  // 合并产物自身必须是干净的（按时间排序追加，不应产生时间戳倒退）
  const CliResult info = RunCliJson(kCli + " session info " + dst);
  STRATUM_CHECK_EQ(info.code, 0);
  STRATUM_CHECK_TRUE(HasSub(info.output, "\"has_integrity_issue\":false"));

  std::printf("      [复制] 150 条 / 3 topic；diff 源↔产物 0 差异；产物无完整性问题\n");
}

// ===========================================================================
// 二、两源无时间重叠 → 并集（不重排、不压缩）
// ===========================================================================

STRATUM_TEST(MergeBasic, DisjointTimelineIsUnion) {
  const std::string a = TempDir("dis_a");
  const std::string b = TempDir("dis_b");
  const std::string dst = TempDir("dis_dst") + "/merged";
  std::string err;

  const TimestampNs base = 1700000000000000000ull;
  const uint64_t step = 10000000ull;   // 10ms
  // 两个源用**不同的 payload 前缀**：它们录的是两个独立的数据流（不是同一广播），
  // 因此不该出现「同内容跨源」的时钟偏差观测。
  STRATUM_CHECK_TRUE(MakeSession(a, {UniformTopic("t/x", 100, base, step, "a")}, &err));
  // B 从 A 的末尾之后 5 秒开始：完全无重叠
  const TimestampNs b_origin = base + 99 * step + 5000000000ull;
  STRATUM_CHECK_TRUE(MakeSession(b, {UniformTopic("t/x", 80, b_origin, step, "b")}, &err));

  const CliResult m = RunCliJson(kCli + " session merge " + a + " " + b + " --to " + dst);
  STRATUM_CHECK_EQ(m.code, 0);
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "events"), 180LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "topics"), 1LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "first_ns"),
                   static_cast<long long>(base));
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "last_ns"),
                   static_cast<long long>(b_origin + 79 * step));
  // 跨度 = 并集：不是两个各自跨度之和，也不是被压缩的相对轴
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "span_ns"),
                   static_cast<long long>(b_origin + 79 * step - base));
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 0, "contributed"), 100LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 1, "contributed"), 80LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "dedupe", "duplicates_removed"), 0LL);

  std::printf("      [并集] 100 + 80 = 180 条；first/last/span 精确等于并集\n");
}

// ===========================================================================
// 三、★ 两源都收到同一批广播 → 去重规则
// ===========================================================================

STRATUM_TEST(MergeDedup, BroadcastDuplicatesAreMerged) {
  const std::string a = TempDir("dup_a");
  const std::string b = TempDir("dup_b");
  const std::string dst = TempDir("dup_dst") + "/merged";
  std::string err;

  const TimestampNs base = 1700000000000000000ull;
  const uint64_t step = 10000000ull;

  // A：共享 topic 50 条 + 只有 A 有的 topic 10 条
  // B：共享 topic 50 条（**同 ts、同 payload**）+ 只有 B 有的 topic 5 条
  std::vector<TopicSpec> ta;
  ta.push_back(UniformTopic("sensor/imu", 50, base, step, "imu-"));
  ta.push_back(UniformTopic("sensor/only_a", 10, base, step, "a-"));
  STRATUM_CHECK_TRUE(MakeSession(a, ta, &err));

  std::vector<TopicSpec> tb;
  tb.push_back(UniformTopic("sensor/imu", 50, base, step, "imu-"));
  tb.push_back(UniformTopic("sensor/only_b", 5, base, step, "b-"));
  STRATUM_CHECK_TRUE(MakeSession(b, tb, &err));

  const CliResult m = RunCliJson(kCli + " session merge " + a + " " + b + " --to " + dst +
                                 " --dedupe content");
  STRATUM_CHECK_EQ(m.code, 0);
  // 50（共享，只留一份）+ 10 + 5
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "events"), 65LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "topics"), 3LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "dedupe", "duplicates_removed"), 50LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "dedupe", "conflicts"), 0LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 0, "contributed"), 60LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 1, "contributed"), 5LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 1, "duplicates"), 50LL);
  // 共享 topic 报告 sources=2、events=50（覆盖度，不是事件来源）
  // topics 按名字排序：sensor/imu 是第 0 个
  STRATUM_CHECK_EQ(PickInArray(m.output, "topics", 0, "sources"), 2LL);
  // 与源 A 相比：共享 topic 的条数没变（不是 100）
  const CliResult q = RunCliJson(kCli + " query " + dst + " --list-topics");
  STRATUM_CHECK_EQ(q.code, 0);
  STRATUM_CHECK_TRUE(HasSub(q.output, "\"name\":\"sensor/imu\""));
  STRATUM_CHECK_TRUE(HasSub(q.output, "\"count\":50"));

  // --dedupe none：同一批数据全保留（条数 = 各源之和）
  const std::string dst2 = TempDir("dup_dst2") + "/merged";
  const CliResult m2 = RunCliJson(kCli + " session merge " + a + " " + b + " --to " + dst2 +
                                  " --dedupe none");
  STRATUM_CHECK_EQ(m2.code, 0);
  STRATUM_CHECK_EQ(PickInObject(m2.output, "merged", "events"), 115LL);
  STRATUM_CHECK_EQ(PickInObject(m2.output, "dedupe", "duplicates_removed"), 0LL);

  std::printf("      [去重] 广播 50 条只留 1 份；115 → 65；跨源去重，同源不去重\n");
}

// ===========================================================================
// 四、★ 同源内的相同记录**不**去重（去重只在跨源生效）
// ===========================================================================
//
// 同一个录制器在同一时刻记录的两条相同 payload 是两条真实事件（重复发布/
// 重传）。若按"内容相同"一刀切，就会篡改源数据 —— 这是去重规则最危险的
// 越界方向，必须有回归。

STRATUM_TEST(MergeDedup, SameSourceNotDeduped) {
  const std::string a = TempDir("same_a");
  const std::string b = TempDir("same_b");
  const std::string dst = TempDir("same_dst") + "/merged";
  std::string err;

  const TimestampNs base = 1700000000000000000ull;
  // A：同一时刻、同一 payload 共 3 条（同源重复）
  STRATUM_CHECK_TRUE(MakeSession(a, {RepeatedTopic("t/same", 3, base, 0, "same")}, &err));
  // B：同样一条 → 只能作为"跨源重复"被去掉 1 条
  STRATUM_CHECK_TRUE(MakeSession(b, {RepeatedTopic("t/same", 1, base, 0, "same")}, &err));

  const CliResult m = RunCliJson(kCli + " session merge " + a + " " + b + " --to " + dst);
  STRATUM_CHECK_EQ(m.code, 0);
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "events"), 3LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "dedupe", "duplicates_removed"), 1LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 0, "contributed"), 3LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 1, "contributed"), 0LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 1, "duplicates"), 1LL);

  std::printf("      [同源] 同源同刻同内容 3 条全保留；只有跨源的 1 条被去重\n");
}

// ===========================================================================
// 五、时钟偏差：默认去重失效，报告必须观测到；--offset 校正后恢复
// ===========================================================================

STRATUM_TEST(MergeSkew, ClockSkewDetectionAndOffset) {
  const std::string a = TempDir("skew_a");
  const std::string b = TempDir("skew_b");
  const std::string dst = TempDir("skew_dst") + "/merged";
  const std::string dst2 = TempDir("skew_dst2") + "/merged";
  std::string err;

  const TimestampNs base = 1700000000000000000ull;
  const uint64_t step = 10000000ull;   // 10ms
  const TimestampNs skew = 100000000ull;   // B 的钟快 100ms

  STRATUM_CHECK_TRUE(MakeSession(a, {UniformTopic("sensor/imu", 40, base, step, "e")}, &err));
  // B：内容完全相同，但源时间戳整体 +100ms（模拟时钟不同源）
  STRATUM_CHECK_TRUE(MakeSession(b, {UniformTopic("sensor/imu", 40, base + skew, step, "e")},
                                 &err));

  // --- 默认（不校正）：去重**失效**，这是文档写明的失效边界 ---
  const CliResult m = RunCliJson(kCli + " session merge " + a + " " + b + " --to " + dst);
  STRATUM_CHECK_EQ(m.code, 5);   // PARTIAL：合并成功但有可疑偏差
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "events"), 80LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "dedupe", "duplicates_removed"), 0LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "skew", "median_ns"),
                   static_cast<long long>(skew));
  STRATUM_CHECK_TRUE(HasSub(m.output, "\"warning\":true"));
  STRATUM_CHECK_TRUE(HasSub(m.output, "clock_skew_suspected"));

  // --- 显式校正：--offset 让 B 回到 A 的时基，去重恢复 ---
  const CliResult m2 = RunCliJson(kCli + " session merge " + a + " " + b + " --to " + dst2 +
                                  " --offset " + b + "=-100ms");
  STRATUM_CHECK_EQ(m2.code, 0);
  STRATUM_CHECK_EQ(PickInObject(m2.output, "merged", "events"), 40LL);
  STRATUM_CHECK_EQ(PickInObject(m2.output, "dedupe", "duplicates_removed"), 40LL);
  STRATUM_CHECK_EQ(PickInObject(m2.output, "skew", "median_ns"), 0LL);
  STRATUM_CHECK_TRUE(HasSub(m2.output, "\"warning\":false"));
  STRATUM_CHECK_EQ(PickInArray(m2.output, "sources", 1, "offset_ns"), -100000000LL);
  STRATUM_CHECK_EQ(PickInArray(m2.output, "sources", 1, "contributed"), 0LL);

  std::printf("      [时钟] 偏 100ms：默认 80 条+警告(退出 5)；--offset 校正后 40 条\n");
}

// ===========================================================================
// 五之二、阈值两级与 --skew-warn（notice 不改变退出码）
// ===========================================================================
//
// 【为什么需要这条】跨主机 NTP 级别的 1–10ms 偏差是常态。若 1ms 就返回 5，
// 5 会变成"跨机合并的默认值"，信号失效。所以：
//   1ms < 幅度 ≤ 阈值（默认 10ms）→ notice（只记录，退出码 0）
//   幅度 > 阈值                    → warning（退出码 5）
// 这条用例把两个档位、阈值可配、以及"去重失效 ≠ 退出码 5"都钉住。

STRATUM_TEST(MergeSkew, ThresholdLevels) {
  const std::string a = TempDir("thr_a");
  const std::string b = TempDir("thr_b");
  const std::string c = TempDir("thr_c");
  const std::string d1 = TempDir("thr_d1") + "/merged";
  const std::string d2 = TempDir("thr_d2") + "/merged";
  const std::string d3 = TempDir("thr_d3") + "/merged";
  std::string err;
  const TimestampNs base = 1700000000000000000ull;
  const uint64_t step = 10000000ull;            // 10ms
  const TimestampNs skew5 = 5000000ull;         // 5ms：NTP 常态
  // 105ms 而不是 100ms：A 的事件在 0/10/.../190ms，C 若也从 100ms 起，
  // 会和 A 的 i=10（恰好在 100ms）撞上同一时刻但 payload 不同 —— 那会被正确
  // 判为冲突（退出 5），把这条用例要验证的东西盖掉。用非整倍数错开。
  const TimestampNs skew100 = 105000000ull;     // 105ms

  STRATUM_CHECK_TRUE(MakeSession(a, {UniformTopic("sensor/imu", 20, base, step, "e")}, &err));
  STRATUM_CHECK_TRUE(
      MakeSession(b, {UniformTopic("sensor/imu", 20, base + skew5, step, "e")}, &err));
  STRATUM_CHECK_TRUE(
      MakeSession(c, {UniformTopic("sensor/imu", 20, base + skew100, step, "e")}, &err));

  // 默认阈值 10ms：5ms → notice、退出 0
  const CliResult m1 = RunCliJson(kCli + " session merge " + a + " " + b + " --to " + d1);
  STRATUM_CHECK_EQ(m1.code, 0);
  STRATUM_CHECK_TRUE(HasSub(m1.output, "\"level\":\"notice\""));
  STRATUM_CHECK_TRUE(HasSub(m1.output, "\"warning\":false"));
  STRATUM_CHECK_TRUE(HasSub(m1.output, "\"threshold_ns\":10000000"));
  STRATUM_CHECK_TRUE(HasSub(m1.output, "clock_skew_notice"));
  // ★ 5ms 已足以让"时间戳必须完全相等"的去重失效 —— 这点必须能从报告看出，
  //   而且它**不**改变退出码（去重是否生效看 dedupe，不看退出码）。
  STRATUM_CHECK_EQ(PickInObject(m1.output, "merged", "events"), 40LL);
  STRATUM_CHECK_EQ(PickInObject(m1.output, "dedupe", "duplicates_removed"), 0LL);

  // 收紧到 1ms：同一个输入变成 warning + 退出 5
  const CliResult m2 = RunCliJson(kCli + " session merge " + a + " " + b + " --to " + d2 +
                                  " --skew-warn 1ms");
  STRATUM_CHECK_EQ(m2.code, 5);
  STRATUM_CHECK_TRUE(HasSub(m2.output, "\"level\":\"warning\""));
  STRATUM_CHECK_TRUE(HasSub(m2.output, "\"threshold_ns\":1000000"));
  STRATUM_CHECK_TRUE(HasSub(m2.output, "clock_skew_suspected"));

  // 放宽到 200ms：100ms 的偏差降级为 notice（脚本自己决定要不要看）
  const CliResult m3 = RunCliJson(kCli + " session merge " + a + " " + c + " --to " + d3 +
                                  " --skew-warn 200ms");
  STRATUM_CHECK_EQ(m3.code, 0);
  STRATUM_CHECK_TRUE(HasSub(m3.output, "\"level\":\"notice\""));
  STRATUM_CHECK_TRUE(HasSub(m3.output, "\"threshold_ns\":200000000"));
  // 这条场景里没有同刻不同内容 —— 5 只能来自"偏差超过阈值"这一个来源
  STRATUM_CHECK_EQ(PickInObject(m3.output, "dedupe", "conflicts"), 0LL);

  // 非法阈值 → USAGE(2)
  const CliResult bad = RunCliTable(kCli + " session merge " + a + " --to " +
                                    TempDir("thr_bad") + "/m --skew-warn 0");
  STRATUM_CHECK_EQ(bad.code, 2);

  std::printf("      [阈值] 5ms 默认 notice(退出 0)；--skew-warn 1ms → warning(5)；200ms → notice\n");
}

// ===========================================================================
// 六、空会话参与合并
// ===========================================================================

STRATUM_TEST(MergeEdge, EmptySource) {
  const std::string empty = TempDir("empty_src");
  const std::string full = TempDir("empty_full");
  const std::string dst = TempDir("empty_dst") + "/merged";
  const std::string dst2 = TempDir("empty_dst2") + "/merged";
  std::string err;

  STRATUM_CHECK_TRUE(MakeSession(empty, {}, &err));   // 空会话（无 topic）
  STRATUM_CHECK_TRUE(
      MakeSession(full, {UniformTopic("t/x", 20, 1700000000000000000ull, 10000000ull, "x")},
                  &err));

  const CliResult m = RunCliJson(kCli + " session merge " + empty + " " + full + " --to " + dst);
  STRATUM_CHECK_EQ(m.code, 0);
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "events"), 20LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 0, "events"), 0LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 0, "contributed"), 0LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 1, "contributed"), 20LL);

  // 两个空会话 → 空产物，不崩、退出码 0
  const CliResult m2 = RunCliJson(kCli + " session merge " + empty + " " + empty + " --to " +
                                  dst2);
  STRATUM_CHECK_EQ(m2.code, 0);
  STRATUM_CHECK_EQ(PickInObject(m2.output, "merged", "events"), 0LL);
  STRATUM_CHECK_EQ(PickInObject(m2.output, "merged", "topics"), 0LL);

  std::printf("      [空] 空+20 = 20 条；空+空 = 0 条，退出码 0，不崩\n");
}

// ===========================================================================
// 七、大规模：10 个会话
// ===========================================================================

STRATUM_TEST(MergeScale, TenSources) {
  std::vector<std::string> dirs;
  const TimestampNs base = 1700000000000000000ull;
  std::string err;

  std::string cmd = kCli + " session merge";
  for (int s = 0; s < 10; ++s) {
    const std::string d = TempDir("scale_" + std::to_string(s));
    // 每个源的 payload 唯一（避免跨源重复），时间戳也错开（避免"同刻不同内容"冲突）
    STRATUM_CHECK_TRUE(
        MakeSession(d, {UniformTopic("many/t", 100,
                                     base + static_cast<TimestampNs>(s) * 100000000ull,
                                     1000000ull, "s" + std::to_string(s) + "-")},
                    &err));
    dirs.push_back(d);
    cmd += " " + d;
  }
  const std::string dst = TempDir("scale_dst") + "/merged";
  cmd += " --to " + dst;

  const CliResult m = RunCliJson(cmd);
  STRATUM_CHECK_EQ(m.code, 0);
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "events"), 1000LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "topics"), 1LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "topics", 0, "sources"), 10LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 0, "contributed"), 100LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 9, "contributed"), 100LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "dedupe", "duplicates_removed"), 0LL);

  // 产物本身可查询，条数一致
  const CliResult q = RunCliJson(kCli + " query " + dst + " --list-topics");
  STRATUM_CHECK_EQ(q.code, 0);
  STRATUM_CHECK_TRUE(HasSub(q.output, "\"count\":1000"));

  std::printf("      [规模] 10 源 × 100 = 1000 条；topic 覆盖度 10；产物可查\n");
}

// ===========================================================================
// 八、冲突：同名同刻不同内容 → 两条都保留、计数、退出码 5
// ===========================================================================

STRATUM_TEST(MergeConflict, SameTimestampDifferentPayload) {
  const std::string a = TempDir("conf_a");
  const std::string b = TempDir("conf_b");
  const std::string dst = TempDir("conf_dst") + "/merged";
  std::string err;

  const TimestampNs base = 1700000000000000000ull;
  STRATUM_CHECK_TRUE(MakeSession(a, {UniformTopic("topic/x", 5, base, 10000000ull, "A-")}, &err));
  STRATUM_CHECK_TRUE(MakeSession(b, {UniformTopic("topic/x", 5, base, 10000000ull, "B-")}, &err));

  const CliResult m = RunCliJson(kCli + " session merge " + a + " " + b + " --to " + dst);
  // 合并完成但存在冲突 → PARTIAL(5)，不是 ERROR
  STRATUM_CHECK_EQ(m.code, 5);
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "events"), 10LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "dedupe", "conflicts"), 5LL);
  STRATUM_CHECK_EQ(PickInObject(m.output, "dedupe", "duplicates_removed"), 0LL);
  STRATUM_CHECK_TRUE(HasSub(m.output, "conflicts:"));

  std::printf("      [冲突] 同刻不同内容 5 处：两条都保留、退出码 5、报告可见\n");
}

// ===========================================================================
// 九、错误处理：NOTFOUND / EXISTS / USAGE
// ===========================================================================

STRATUM_TEST(MergeErrors, CliErrors) {
  const std::string src = TempDir("err_src");
  std::string err;
  STRATUM_CHECK_TRUE(
      MakeSession(src, {UniformTopic("t/x", 20, 1700000000000000000ull, 10000000ull, "x")},
                  &err));

  const std::string uniq = std::to_string(static_cast<long>(::getpid()));

  // 1) 源不存在 → NOTFOUND(8)，且不创建目标
  const std::string missing = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
                              "/stratum_merge_absent_" + uniq;
  std::error_code ec;
  fs::remove_all(missing, ec);
  const std::string ghost_target = TempDir("err_ghost") + "/should_not_exist";
  const CliResult r1 = RunCliTable(kCli + " session merge " + missing + " --to " + ghost_target);
  STRATUM_CHECK_EQ(r1.code, 8);
  STRATUM_CHECK_TRUE(!fs::exists(ghost_target));

  // 2) 目标已存在且非空 → EXISTS(3)
  const std::string existing = TempDir("err_existing");
  STRATUM_CHECK_TRUE(
      MakeSession(existing, {UniformTopic("t/old", 7, 1700000000000000000ull, 10000000ull, "o")},
                  &err));
  const CliResult r2 = RunCliTable(kCli + " session merge " + src + " --to " + existing);
  STRATUM_CHECK_EQ(r2.code, 3);
  STRATUM_CHECK_TRUE(HasSub(r2.output, "--overwrite"));

  // 3) --overwrite → 成功，且是重建（不是追加）
  const CliResult r3 = RunCliJson(kCli + " session merge " + src + " --to " + existing +
                                  " --overwrite");
  STRATUM_CHECK_EQ(r3.code, 0);
  STRATUM_CHECK_EQ(PickInObject(r3.output, "merged", "events"), 20LL);

  // 4) 参数不足：没有源 → USAGE(2)
  const CliResult r4 = RunCliTable(kCli + " session merge --to " + existing + "x");
  STRATUM_CHECK_EQ(r4.code, 2);
  STRATUM_CHECK_TRUE(HasSub(r4.output, "至少一个源会话"));

  // 5) 缺 --to → USAGE(2)
  const CliResult r5 = RunCliTable(kCli + " session merge " + src);
  STRATUM_CHECK_EQ(r5.code, 2);
  STRATUM_CHECK_TRUE(HasSub(r5.output, "--to"));

  // 6) 目标 == 源 → USAGE(2)（写源会破坏源数据）
  const CliResult r6 = RunCliTable(kCli + " session merge " + src + " --to " + src);
  STRATUM_CHECK_EQ(r6.code, 2);

  // 7) 非法 --dedupe / --offset → USAGE(2)
  const CliResult r7 = RunCliTable(kCli + " session merge " + src + " --to " +
                                   TempDir("err_d7") + "/m --dedupe bogus");
  STRATUM_CHECK_EQ(r7.code, 2);
  const CliResult r8 = RunCliTable(kCli + " session merge " + src + " --to " +
                                   TempDir("err_d8") + "/m --offset nope");
  STRATUM_CHECK_EQ(r8.code, 2);

  // 8) 目标是根目录 → USAGE(2)（安全阀：否则 --overwrite 会 remove_all("/")）
  //    这里刻意不加 --overwrite：即使安全阀失效，也只会走到 EXISTS(3)，不会删任何东西。
  const CliResult r9 = RunCliTable(kCli + " session merge " + src + " --to /");
  STRATUM_CHECK_EQ(r9.code, 2);

  std::printf("      [错误] NOTFOUND×1 / EXISTS×1 / USAGE×6，且失败不创建目标\n");
}

// ===========================================================================
// 十、双输出：--format table 与 json 语义一致
// ===========================================================================

STRATUM_TEST(MergeFormat, TableMatchesJson) {
  const std::string src = TempDir("fmt_src");
  const std::string dst_t = TempDir("fmt_t") + "/merged";
  const std::string dst_j = TempDir("fmt_j") + "/merged";
  std::string err;
  STRATUM_CHECK_TRUE(
      MakeSession(src, {UniformTopic("t/x", 20, 1700000000000000000ull, 10000000ull, "x")},
                  &err));

  const CliResult t = RunCliTable(kCli + " session merge " + src + " --to " + dst_t);
  STRATUM_CHECK_EQ(t.code, 0);
  STRATUM_CHECK_TRUE(HasSub(t.output, "会话合并"));
  STRATUM_CHECK_TRUE(HasSub(t.output, "绝对时间轴"));
  STRATUM_CHECK_TRUE(HasSub(t.output, "20 条"));
  STRATUM_CHECK_TRUE(HasSub(t.output, "说明: 合并不判断数据对错"));
  // ★ 人类可读路径不是"顺手打印"，它也被锁住：该有的段落一个不能少
  STRATUM_CHECK_TRUE(HasSub(t.output, "来源（1）"));
  STRATUM_CHECK_TRUE(HasSub(t.output, "话题（1）"));
  STRATUM_CHECK_TRUE(HasSub(t.output, "时钟偏差观测"));
  STRATUM_CHECK_TRUE(HasSub(t.output, "冲突: 0 处"));
  STRATUM_CHECK_TRUE(HasSub(t.output, "偏移 +0.00ms"));
  // --format 真的生效了：table 输出里不能混进 JSON
  STRATUM_CHECK_TRUE(!HasSub(t.output, "\"schema_version\""));

  const CliResult j = RunCliJson(kCli + " session merge " + src + " --to " + dst_j);
  STRATUM_CHECK_EQ(j.code, 0);
  STRATUM_CHECK_EQ(PickInObject(j.output, "merged", "events"), 20LL);

  // 两种输出描述的是同一件事：table 里的条数与 json 的 merged.events 一致
  STRATUM_CHECK_TRUE(HasSub(t.output, "写入 20 条"));

  // --report：合并报告可另存（"产出合并后的会话 + 一份合并报告"）
  const std::string report_path = TempDir("fmt_report") + "/merge.json";
  const CliResult r = RunCliJson(kCli + " session merge " + src + " --to " +
                                 TempDir("fmt_r") + "/merged --report " + report_path);
  STRATUM_CHECK_EQ(r.code, 0);
  std::ifstream rf(report_path);
  STRATUM_CHECK_TRUE(rf.good());
  const std::string report((std::istreambuf_iterator<char>(rf)),
                           std::istreambuf_iterator<char>());
  STRATUM_CHECK_EQ(PickInObject(report, "merged", "events"), 20LL);
  STRATUM_CHECK_TRUE(HasSub(report, "\"dedupe\""));
  // note 在文件里也必须存在（不只是 stdout）
  STRATUM_CHECK_TRUE(HasSub(report, "合并不判断数据对错"));

  // --help：选项说明必须可达（--to/--dedupe/--offset/--report + 退出码 + 文档指向）
  const CliResult h = RunCliTable(kCli + " session merge --help");
  STRATUM_CHECK_EQ(h.code, 0);
  STRATUM_CHECK_TRUE(HasSub(h.output, "--to"));
  STRATUM_CHECK_TRUE(HasSub(h.output, "--dedupe"));
  STRATUM_CHECK_TRUE(HasSub(h.output, "--offset"));
  STRATUM_CHECK_TRUE(HasSub(h.output, "--report"));
  STRATUM_CHECK_TRUE(HasSub(h.output, "退出码"));
  STRATUM_CHECK_TRUE(HasSub(h.output, "docs/16-session-merge.md"));

  // 全局 --help 里也要能查到 merge 的选项（不只子命令 help）
  const CliResult gh = RunCliTable(kCli + " --help");
  STRATUM_CHECK_EQ(gh.code, 0);
  STRATUM_CHECK_TRUE(HasSub(gh.output, "session merge 的选项"));
  STRATUM_CHECK_TRUE(HasSub(gh.output, "--offset <会话>=<±时长>"));

  std::printf("      [双输出] table 段落齐全且不混 JSON；json 含 note；--report 落盘；--help 可达\n");
}


// ===========================================================================
// 十一、随机（固定种子，可复现）：不变量 —— 每个 (ts,payload) 保留"源序号最小的源"的条数
// ===========================================================================
//
// 随机构造 3 个源，事件的时间戳与 payload 从固定集合里随机取，
// 于是**天然产生**三类情况：
//   · 跨源完全相同 → 去重
//   · 同源内相同 → 保留
//   · 同刻不同内容 → 冲突（保留）
//
// 不变量（可精确计算，不依赖实现的内部结构）：
//     key = (合并后时间戳, payload)。同 key 的多条来自不同源时，保留
//     **源参数顺序最靠前**的那个源的全部记录（同刻 → 排序只差源序号），
//     其余源的同 key 记录被丢弃。所以：
//         merged.events = Σ_key 条数(key, 源序号最小的那个源)
//         merged.events + duplicates_removed = 三个源的条数之和
//
// 固定种子保证失败可复现；换种子只是换一组随机输入，断言不变。

STRATUM_TEST(MergeRandom, InvariantMaxPerSource) {
  const TimestampNs base = 1700000000000000000ull;
  std::mt19937 rng(20261018u);
  std::map<std::string, std::map<uint32_t, int>> per_key;   // key -> 源 -> 条数
  std::string cmd = kCli + " session merge";
  std::string err;

  for (int s = 0; s < 3; ++s) {
    TopicSpec t;
    t.name = "rand/t";
    for (int i = 0; i < 20; ++i) {
      // 键空间刻意做小（8 个时刻 × 5 种内容 = 40 个 key / 60 条事件）：
      // 这样随机输入**必然**产生跨源重复与同源重复，去重规则才会被真正压到。
      const TimestampNs ts = base + (rng() % 8) * 1000000ull;
      const std::string p = "p" + std::to_string(rng() % 5);
      t.events.push_back(EventSpec{ts, p});
      per_key[std::to_string(ts) + "|" + p][static_cast<uint32_t>(s)] += 1;
    }
    const std::string d = TempDir("rand_" + std::to_string(s));
    STRATUM_CHECK_TRUE(MakeSession(d, {t}, &err));
    cmd += " " + d;
  }

  uint64_t expected = 0;
  for (const auto& kv : per_key) {
    // std::map<uint32_t,int> 按源序号升序：begin() 就是"参数顺序最靠前的源"
    auto first = kv.second.begin();
    int count = (first == kv.second.end()) ? 0 : first->second;
    expected += static_cast<uint64_t>(count);
  }

  const std::string dst = TempDir("rand_dst") + "/merged";
  cmd += " --to " + dst;
  const CliResult m = RunCliJson(cmd);
  // 随机数据里同刻不同内容会出现 → 允许 5（PARTIAL），但绝不允许其它错误码
  STRATUM_CHECK_TRUE(m.code == 0 || m.code == 5);
  STRATUM_CHECK_EQ(PickInObject(m.output, "merged", "events"),
                   static_cast<long long>(expected));
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 0, "events"), 20LL);
  STRATUM_CHECK_EQ(PickInArray(m.output, "sources", 2, "events"), 20LL);
  const long long dup = PickInObject(m.output, "dedupe", "duplicates_removed");
  STRATUM_CHECK_EQ(static_cast<long long>(expected) + dup, 60LL);

  std::printf("      [随机] 3×20 条：保留 %llu 条（源序号最小的那个源），去重 %lld 条，合计 60\n",
              static_cast<unsigned long long>(expected), dup);
}

STRATUM_TEST_MAIN()
