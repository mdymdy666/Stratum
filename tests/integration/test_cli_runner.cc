// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// **验证 `tests/cli_runner.h` 这个辅助本身是对的**
//
// 为什么"测试测试辅助"也值得一个套件：
//   这个辅助的存在目的就是**消灭一个我踩了 5 次的坑**。
//   如果辅助自己错了（比如把 --format 插到错误的位置），
//   那所有用它写的测试都会以一种**更隐蔽**的方式错 ——
//   它们会拿到 json 却以为是 table，然后断言失败，而我第一反应又是"功能坏了"。
//
// 所以先证明辅助正确，再用它写别的测试。

// 标准库头文件必须最先包含
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "cli_runner.h"
#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"

namespace fs = std::filesystem;
using namespace stratum;
using namespace stratum_cli_test;

namespace {

#ifndef CR_CLI_PATH
#define CR_CLI_PATH "./stratum"
#endif

std::string kCli = CR_CLI_PATH;

std::string TempDir(const char* tag) {
  const char* home = std::getenv("HOME");
  const std::string d = std::string(home ? home : ".") + "/stratum_cr_" + tag;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

bool MakeSession(const std::string& dir, int topics, int events, std::string* err) {
  TimelineImplV2 t;
  if (!t.Open(dir).ok()) {
    if (err) *err = "Open 失败";
    return false;
  }
  for (int i = 0; i < topics; ++i) {
    TopicMetadata m;
    m.topic_name = "c/t" + std::to_string(i);
    m.type_name = "T";
    uint32_t id = 0;
    if (!t.RegisterTopic(m, &id).ok()) {
      t.Close();
      if (err) *err = "RegisterTopic 失败";
      return false;
    }
    std::vector<Event> ev;
    for (int k = 0; k < events; ++k) {
      Event e;
      e.topic_id = id;
      e.seq = static_cast<uint32_t>(k + 1);
      e.source_timestamp_ns = 1700000000000000000ull +
                              static_cast<TimestampNs>(k) * 10000000ull;
      e.reception_timestamp_ns = e.source_timestamp_ns + 100;
      e.payload = "x";
      ev.push_back(std::move(e));
    }
    if (!t.AppendBatch(ev).ok()) {
      t.Close();
      if (err) *err = "AppendBatch 失败";
      return false;
    }
  }
  t.Close();
  return true;
}

}  // namespace

// ===========================================================================
// 一、★ InsertFormat 把 --format 插到子命令**之前**
// ===========================================================================
//
// 这是整个辅助的核心逻辑。插错位置会导致 `--format` 被当成子命令选项，
// 命令直接报 USAGE —— 而错误消息看起来像"功能不支持"，非常误导。

STRATUM_TEST(CliRunnerHelper, InsertFormatPlacesOptionBeforeSubcommand) {
  STRATUM_CHECK_EQ(InsertFormat("stratum verify /data/s", "table"),
                   std::string("stratum --format table verify /data/s"));
  STRATUM_CHECK_EQ(InsertFormat("stratum diff /a /b", "json"),
                   std::string("stratum --format json diff /a /b"));
  // 路径里带空格也不该影响（只认第一个空格后的第一个 token）
  STRATUM_CHECK_EQ(InsertFormat("stratum query /a b/c --list-topics", "table"),
                   std::string("stratum --format table query /a b/c --list-topics"));
  std::printf("      [插入位置] --format 落在可执行文件之后、子命令之前\n");
}

STRATUM_TEST(CliRunnerHelper, InsertFormatDoesNotDuplicate) {
  // 调用方已经传了 --format → 尊重它，不再插入
  const std::string already = "stratum --format csv verify /x";
  STRATUM_CHECK_EQ(InsertFormat(already, "table"), already);
  std::printf("      [不重复] 已含 --format 时原样返回\n");
}

// ===========================================================================
// 二、★ RunCliTable 真的拿到人类可读输出（不是 JSON）
// ===========================================================================

STRATUM_TEST(CliRunnerHelper, TableReallyReturnsHumanText) {
  const std::string dir = TempDir("table");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 2, 20, &err));

  const CliResult r = RunCliTable(kCli + " verify " + dir);
  STRATUM_CHECK_EQ(r.code, 0);
  // 表格输出含中文标签
  STRATUM_CHECK_TRUE(r.output.find("完整") != std::string::npos ||
                     r.output.find("事件") != std::string::npos);
  // ★ 且**不是** JSON（首字符不是 '{'）
  size_t b = 0;
  while (b < r.output.size() && (r.output[b] == ' ' || r.output[b] == '\n')) ++b;
  STRATUM_CHECK_TRUE(b < r.output.size() && r.output[b] != '{');
  std::printf("      [table] 真的拿到了人类可读输出（非 JSON）\n");
}

// ===========================================================================
// 三、★ RunCliJson 真的拿到 JSON
// ===========================================================================

STRATUM_TEST(CliRunnerHelper, JsonReallyReturnsJson) {
  const std::string dir = TempDir("json");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 1, 10, &err));

  const CliResult r = RunCliJson(kCli + " verify " + dir);
  STRATUM_CHECK_EQ(r.code, 0);
  size_t b = 0;
  while (b < r.output.size() && (r.output[b] == ' ' || r.output[b] == '\n')) ++b;
  STRATUM_CHECK_TRUE(b < r.output.size() && r.output[b] == '{');
  STRATUM_CHECK_TRUE(r.output.find("\"schema_version\":1") != std::string::npos);
  std::printf("      [json] 真的拿到了 JSON（以 { 开头，含 schema_version）\n");
}

// ===========================================================================
// 四、★ 两者在同一命令上**得出不同形态**（证明格式真的生效了）
// ===========================================================================
//
// 这是最有力的判据：如果 - -format 没生效，两者的输出会**一样**。

STRATUM_TEST(CliRunnerHelper, TableAndJsonDiffer) {
  const std::string dir = TempDir("differ");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 1, 10, &err));

  const CliResult t = RunCliTable(kCli + " verify " + dir);
  const CliResult j = RunCliJson(kCli + " verify " + dir);

  STRATUM_CHECK_EQ(t.code, 0);
  STRATUM_CHECK_EQ(j.code, 0);
  // ★ 输出必须不同 —— 否则说明 --format 没生效
  STRATUM_CHECK_TRUE(t.output != j.output);
  // 且各自的形态正确
  STRATUM_CHECK_TRUE(j.output.find("\"events\":10") != std::string::npos);
  STRATUM_CHECK_TRUE(t.output.find("\"events\":10") == std::string::npos);
  std::printf("      [生效] table 与 json 输出不同，证明 --format 真的生效\n");
}

// ===========================================================================
// 五、★ RunSplit 分离 stdout 与 stderr
// ===========================================================================

STRATUM_TEST(CliRunnerHelper, SplitSeparatesStreams) {
  const std::string dir = TempDir("split");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 1, 10, &err));

  // diff 在时长差异大时会往 stderr 写警告
  const std::string dir2 = TempDir("split2");
  {
    TimelineImplV2 t;
    STRATUM_CHECK_TRUE(t.Open(dir2).ok());
    TopicMetadata m;
    m.topic_name = "c/t0";
    m.type_name = "T";
    uint32_t id = 0;
    STRATUM_CHECK_TRUE(t.RegisterTopic(m, &id).ok());
    std::vector<Event> ev;
    for (int k = 0; k < 10; ++k) {
      Event e;
      e.topic_id = id;
      e.seq = static_cast<uint32_t>(k + 1);
      // 跨度 50 秒 vs 原会话 0.09 秒 → 触发 warning
      e.source_timestamp_ns = 1700000000000000000ull +
                              static_cast<TimestampNs>(k) * 5000000000ull;
      e.reception_timestamp_ns = e.source_timestamp_ns + 100;
      e.payload = "x";
      ev.push_back(std::move(e));
    }
    STRATUM_CHECK_TRUE(t.AppendBatch(ev).ok());
    t.Close();
  }

  const SplitResult s = RunSplitJson(kCli + " diff " + dir + " " + dir2);
  // stdout 必须是完整 JSON
  size_t b = 0;
  while (b < s.out.size() && (s.out[b] == ' ' || s.out[b] == '\n')) ++b;
  STRATUM_CHECK_TRUE(b < s.out.size() && s.out[b] == '{');
  size_t e = s.out.size();
  while (e > b && (s.out[e - 1] == ' ' || s.out[e - 1] == '\n')) --e;
  STRATUM_CHECK_TRUE(e > b && s.out[e - 1] == '}');
  // ★ 警告在 stderr，不在 stdout
  STRATUM_CHECK_TRUE(s.out.find("警告") == std::string::npos);
  STRATUM_CHECK_TRUE(s.err.find("警告") != std::string::npos);
  std::printf("      [分离] stdout 是完整 JSON，时长警告只在 stderr\n");
}

// ===========================================================================
// 六、★ PickInSide 正确限定作用域（避免同名 key 歧义）
// ===========================================================================

STRATUM_TEST(CliRunnerHelper, PickInSideScopesCorrectly) {
  // 构造一个含同名 key 的文本：
  //   顶层 left.events = 10000
  //   changed[].left_events = 1000
  const std::string fake =
      "{\"left\":{\"session\":\"/x\",\"events\":10000,\"topics\":10},"
      "\"topics\":{\"changed\":[{\"topic\":\"t\",\"left_events\":1000}]}}";

  // 限定在 left 段 → 10000
  STRATUM_CHECK_EQ(PickInSide(fake, "left", "events"), 10000LL);
  // right 段不存在 → -1
  STRATUM_CHECK_EQ(PickInSide(fake, "right", "events"), -1LL);
  std::printf("      [作用域] PickInSide 取到 left.events=10000，不被 changed 干扰\n");
}

STRATUM_TEST_MAIN()
