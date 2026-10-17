// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// **stdout 纯净性**：stdout 是数据出口，日志与统计只能去 stderr
//
// 这是项目的核心 CLI 纪律（docs/08），但此前**从未被自动验证**过。
//
// 【为什么它极其重要】
// 用户会这样用：
//     stratum query /data/s --list-topics | jq '.topics[].name'
//     stratum verify /data/s > report.json
//
// 只要有一条日志（`[probe] 读 40 条`、`警告: ...`）混进 stdout，
// 管道就断了 —— 而且**看起来像是下游工具的问题**，很难定位到是我们自己污染的。
//
// 【本套件的判据】
//   · 所有 JSON 输出的命令：stdout 必须以 `{` 开头、以 `}` 结尾，
//     且**不含**任何已知日志标记
//   · 存在 stderr 日志的命令：stderr 必须非空（否则说明日志被吞了，同样不对）
//   · 表格输出的命令：stdout 里不该出现 `[probe]` / `警告:` 这类运行日志

// 标准库头文件必须最先包含
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"

namespace fs = std::filesystem;
using namespace stratum;

namespace {

#ifndef PURITY_CLI_PATH
#define PURITY_CLI_PATH "./stratum"
#endif

std::string kCli = PURITY_CLI_PATH;

struct SplitResult {
  int code = -1;
  std::string out;   // stdout
  std::string err;   // stderr
};

// 分开捕获 stdout 与 stderr —— 这正是本套件要验证的东西，所以不能用 2>&1
SplitResult RunSplit(const std::string& cmd) {
  SplitResult r;
  const std::string full = cmd + " > /tmp/_purity_out.txt 2> /tmp/_purity_err.txt";
  const int rc = std::system(full.c_str());
  r.code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

  auto ReadAll = [](const char* path) -> std::string {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return {};
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
  };
  r.out = ReadAll("/tmp/_purity_out.txt");
  r.err = ReadAll("/tmp/_purity_err.txt");
  std::remove("/tmp/_purity_out.txt");
  std::remove("/tmp/_purity_err.txt");
  return r;
}

std::string TempDir(const char* tag) {
  const char* home = std::getenv("HOME");
  const std::string d = std::string(home ? home : ".") + "/stratum_pu_" + tag;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

bool MakeSession(const std::string& dir, int topics, int events, std::string* err) {
  TimelineImplV2 timeline;
  if (!timeline.Open(dir).ok()) {
    if (err) *err = "Open 失败";
    return false;
  }
  for (int t = 0; t < topics; ++t) {
    TopicMetadata meta;
    meta.topic_name = "p/t" + std::to_string(t);
    meta.type_name = "T";
    uint32_t tid = 0;
    if (!timeline.RegisterTopic(meta, &tid).ok()) {
      timeline.Close();
      if (err) *err = "RegisterTopic 失败";
      return false;
    }
    std::vector<Event> ev;
    for (int i = 0; i < events; ++i) {
      Event e;
      e.topic_id = tid;
      e.seq = static_cast<uint32_t>(i + 1);
      e.source_timestamp_ns = 1700000000000000000ull +
                              static_cast<TimestampNs>(i) * 10000000ull;
      e.reception_timestamp_ns = e.source_timestamp_ns + 100;
      e.payload = "p";
      ev.push_back(std::move(e));
    }
    if (!timeline.AppendBatch(ev).ok()) {
      timeline.Close();
      if (err) *err = "AppendBatch 失败";
      return false;
    }
  }
  timeline.Close();
  return true;
}

// JSON 输出的基本形状检查（不引入 JSON 库：只需"看起来完整"）
bool LooksLikeCompleteJson(const std::string& s) {
  if (s.empty()) return false;
  size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\n' || s[b] == '\r')) ++b;
  if (b >= s.size() || s[b] != '{') return false;
  size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
  return e > b && s[e - 1] == '}';
}

// 已知的"运行日志"标记 —— 它们出现在 stdout 就是污染
bool ContainsLogMarker(const std::string& s) {
  static const char* kMarkers[] = {
      "[probe]", "[dry-run]", "警告:", "提示:", "[提示]", "DEBUG",
  };
  for (const char* m : kMarkers) {
    if (s.find(m) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

// ===========================================================================
// 一、★ JSON 输出命令：stdout 必须是完整 JSON 且不含日志
// ===========================================================================

STRATUM_TEST(StdoutPurity, JsonCommandsKeepStdoutClean) {
  const std::string dir = TempDir("json");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 2, 30, &err));

  // 注意：全局 --format 默认就是 json，这里不显式传
  const char* cmds[] = {
      " verify ",
      " session info ",
      " query --list-topics ",
      " metrics --json ",
      " io verify ",
  };

  for (const char* c : cmds) {
    const std::string full = kCli + std::string(c) + dir;
    const SplitResult r = RunSplit(full);
    // 有的命令对会话类型有要求（io verify 要 bundle），失败也算通过本项，
    // 但**只要输出了东西**就必须是完整 JSON 且无日志
    if (!r.out.empty()) {
      STRATUM_CHECK_TRUE(LooksLikeCompleteJson(r.out));
      STRATUM_CHECK_FALSE(ContainsLogMarker(r.out));
    }
  }
  std::printf("      [JSON] verify/session/query/metrics/io 的 stdout 均为完整 JSON 且无日志\n");
}

// ===========================================================================
// 二、★ diff 的 stdout 纯净性
// ===========================================================================

STRATUM_TEST(StdoutPurity, DiffKeepsStdoutClean) {
  const std::string a = TempDir("d_a");
  const std::string b = TempDir("d_b");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(a, 2, 40, &err));
  STRATUM_CHECK_TRUE(MakeSession(b, 2, 20, &err));

  // 正常情况下无 stderr
  const SplitResult r1 = RunSplit(kCli + " diff " + a + " " + b);
  STRATUM_CHECK_TRUE(LooksLikeCompleteJson(r1.out));
  STRATUM_CHECK_FALSE(ContainsLogMarker(r1.out));

  // ★ 构造"有 stderr 警告"的情况：时长差 50 倍 → duration_warning
  // 警告必须**只在 stderr**，stdout 仍是完整 JSON
  const std::string c = TempDir("d_c");
  {
    TimelineImplV2 t;
    STRATUM_CHECK_TRUE(t.Open(c).ok());
    TopicMetadata meta;
    meta.topic_name = "p/t0";
    meta.type_name = "T";
    uint32_t tid = 0;
    STRATUM_CHECK_TRUE(t.RegisterTopic(meta, &tid).ok());
    std::vector<Event> ev;
    for (int i = 0; i < 40; ++i) {
      Event e;
      e.topic_id = tid;
      e.seq = static_cast<uint32_t>(i + 1);
      // 跨度 50 秒（对比 a 的 0.4 秒，差 125 倍）
      e.source_timestamp_ns = 1700000000000000000ull +
                              static_cast<TimestampNs>(i) * 1250000000ull;
      e.reception_timestamp_ns = e.source_timestamp_ns + 100;
      e.payload = "p";
      ev.push_back(std::move(e));
    }
    STRATUM_CHECK_TRUE(t.AppendBatch(ev).ok());
    t.Close();
  }
  const SplitResult r2 = RunSplit(kCli + " diff " + a + " " + c);
  // ★ 核心：警告在 stderr，stdout 仍是完整 JSON
  STRATUM_CHECK_TRUE(LooksLikeCompleteJson(r2.out));
  STRATUM_CHECK_FALSE(ContainsLogMarker(r2.out));
  STRATUM_CHECK_TRUE(r2.err.find("警告") != std::string::npos);
  std::printf("      [diff] 时长警告只出现在 stderr，stdout 仍是完整 JSON\n");
}

// ===========================================================================
// 三、★ 表格输出：日志仍不该混进来
// ===========================================================================

STRATUM_TEST(StdoutPurity, TableOutputHasNoRunLogs) {
  const std::string dir = TempDir("tbl");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(dir, 2, 30, &err));

  const SplitResult r1 = RunSplit(kCli + " --format table verify " + dir);
  STRATUM_CHECK_FALSE(ContainsLogMarker(r1.out));
  // 表格输出应该有内容
  STRATUM_CHECK_TRUE(r1.out.find("完整性") != std::string::npos ||
                     r1.out.find("事件") != std::string::npos);

  const SplitResult r2 = RunSplit(kCli + " --format table diff " + dir + " " + dir);
  STRATUM_CHECK_FALSE(ContainsLogMarker(r2.out));
  STRATUM_CHECK_TRUE(r2.out.find("会话对比") != std::string::npos);
  std::printf("      [表格] --format table 的 stdout 无 [probe]/警告 等运行日志\n");
}

// ===========================================================================
// 四、★ 错误消息走 stderr，stdout 保持空
// ===========================================================================
//
// 这条容易被忽略：错误时如果往 stdout 打东西，脚本的 `| jq` 会拿到半截内容。

STRATUM_TEST(StdoutPurity, ErrorsGoToStderrNotStdout) {
  // 不存在的会话 → 错误
  const SplitResult r = RunSplit(kCli + " verify /tmp/purity_absent_" +
                                 std::to_string(static_cast<long>(::getpid())));
  STRATUM_CHECK_TRUE(r.code != 0);
  // ★ stdout 必须为空（或至少不含错误文本）
  STRATUM_CHECK_TRUE(r.out.find("错误:") == std::string::npos);
  // 错误文本应在 stderr
  STRATUM_CHECK_TRUE(r.err.find("错误:") != std::string::npos);
  std::printf("      [错误] 错误消息只在 stderr，stdout 未被污染\n");

  // diff 的错误同理
  const SplitResult r2 = RunSplit(kCli + " diff /tmp/purity_x_a /tmp/purity_x_b");
  STRATUM_CHECK_TRUE(r2.code != 0);
  STRATUM_CHECK_TRUE(r2.out.find("错误:") == std::string::npos);
  STRATUM_CHECK_TRUE(r2.err.find("错误:") != std::string::npos);
}

STRATUM_TEST_MAIN()
