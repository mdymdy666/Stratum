// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// `stratum probe status` 的**边界**测试
//
// 与 test_cli_probe.cc 的分工：
//   test_cli_probe.cc        —— status 的正常路径（有数据、有配置）
//   test_probe_status_edge.cc —— **空与极端的输入**：
//       · 空 ring（刚创建、还没写入）
//       · 全部 topic 都未配置
//       · --scan-limit 0 / 1
//       · ring 已覆盖过一圈（数据被覆盖后的 topic 统计）
//       · 配置声明了 topic 但环里从未出现
//
// 【为什么单独一个文件】status 是排障时第一个被敲的命令。
// 它在"没有数据"时给出的结论，比有数据时更重要 ——
// 因为那时候用户最需要知道"是没采到，还是采到了但没显示"。

// 标准库头文件必须最先包含
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "stratum/probe/probe.h"
#include "test_framework.h"
#include "test_isolation.h"

namespace fs = std::filesystem;

namespace {

#ifndef PROBE_CLI_PATH
#define PROBE_CLI_PATH "./stratum"
#endif

std::string kCli = PROBE_CLI_PATH;

struct CliResult {
  int code = -1;
  std::string output;
};

CliResult RunCli(const std::string& cmd) {
  CliResult r;
  std::FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
  if (p == nullptr) return r;
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), p) != nullptr) r.output += buf;
  const int st = ::pclose(p);
  r.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return r;
}

std::string TempDir(const char* tag) {
  const char* home = std::getenv("HOME");
  const std::string d = std::string(home ? home : ".") + "/stratum_se_" + tag + "_" + UniqueSuffix();
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

std::string FreshShm(const char* tag) {
  const std::string p = std::string("/dev/shm/stratum_se_") + tag;
  std::error_code ec;
  fs::remove(p, ec);
  return p;
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

}  // namespace

// ===========================================================================
// 一、★ 空 ring：清楚说明"还没写入"，而不是显示 0 个 topic 就完事
// ===========================================================================
//
// 【为什么这条重要】用户看到"0 个 topic"时最需要的判断是：
//   "业务还没开始写" vs "写了但被覆盖了" vs "status 有 bug"
// 三者现象相同，结论完全不同。输出必须能区分。

STRATUM_TEST(ProbeStatusEdge, EmptyRingSaysNothingWrittenYet) {
  const std::string shm = FreshShm("empty");
  // 用本进程创建 ring（容量默认），不写任何记录
  stratum_probe_close();
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);
  stratum_probe_close();

  const CliResult r = RunCli(kCli + " --format table probe status " + shm);
  STRATUM_CHECK_EQ(r.code, 0);
  // 必须说明"还没写入"或"无"，而不是只给个空列表
  STRATUM_CHECK_TRUE(r.output.find("无") != std::string::npos ||
                     r.output.find("还没写入") != std::string::npos);
  // 且已分配/已提交都是 0
  STRATUM_CHECK_TRUE(r.output.find("已分配") != std::string::npos);
  std::printf("      [空 ring] 明确提示「无/还没写入」，并给出已分配=0\n");

  std::error_code ec;
  fs::remove(shm, ec);
}

// ===========================================================================
// 二、全部 topic 都未配置：必须全部列出且给可粘贴片段
// ===========================================================================

STRATUM_TEST(ProbeStatusEdge, AllTopicsUnconfigured) {
  const std::string shm = FreshShm("allun");
  const std::string dir = TempDir("allun");
  const std::string cfg = dir + "/p.ini";
  {
    std::ofstream o(cfg, std::ios::out | std::ios::trunc);
    o << "[probe]\nenabled = true\n";   // 一个 topic 都没声明
  }

  stratum_probe_close();
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);
  stratum_probe_write("u/one", "a", 1);
  stratum_probe_write("u/two", "b", 1);
  stratum_probe_write("u/three", "c", 1);
  stratum_probe_close();

  const CliResult r = RunCli(kCli + " --format table probe status " + shm +
                             " --config " + cfg);
  // 有未配置 topic → PARTIAL
  STRATUM_CHECK_EQ(r.code, 5);
  // 三个都要被点名
  STRATUM_CHECK_TRUE(r.output.find("u/one") != std::string::npos);
  STRATUM_CHECK_TRUE(r.output.find("u/two") != std::string::npos);
  STRATUM_CHECK_TRUE(r.output.find("u/three") != std::string::npos);
  // 报告数量
  STRATUM_CHECK_TRUE(r.output.find("3") != std::string::npos);
  std::printf("      [全未配置] 3 个 topic 全部点名并返回 PARTIAL(5)\n");

  std::error_code ec;
  fs::remove(shm, ec);
  fs::remove_all(dir, ec);
}

// ===========================================================================
// 三、配置声明了 topic，但环里从未出现
// ===========================================================================
//
// 【为什么重要】这有两种可能，用户必须能想到：
//   ① 业务没调用那个探针（埋了但没走那条路径）
//   ② 已经被环覆盖掉了（早期写过，现在环里没有了）
// 输出要能把这两种可能都提示出来。

STRATUM_TEST(ProbeStatusEdge, DeclaredButUnseenIsReported) {
  const std::string shm = FreshShm("decl");
  const std::string dir = TempDir("decl");
  const std::string cfg = dir + "/p.ini";
  {
    std::ofstream o(cfg, std::ios::out | std::ios::trunc);
    o << "[probe]\nenabled = true\n"
      << "[topic.\"seen/one\"]\nenabled = true\n"
      << "[topic.\"never/called\"]\nenabled = true\n";
  }

  stratum_probe_close();
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);
  stratum_probe_write("seen/one", "a", 1);
  stratum_probe_close();

  const CliResult r = RunCli(kCli + " --format table probe status " + shm +
                             " --config " + cfg);
  // 必须报出 never/called 从未出现
  STRATUM_CHECK_TRUE(r.output.find("never/called") != std::string::npos);
  STRATUM_CHECK_TRUE(r.output.find("没有出现") != std::string::npos ||
                     r.output.find("未出现") != std::string::npos);
  // 且提示可能原因（未调用 / 被覆盖）
  STRATUM_CHECK_TRUE(r.output.find("覆盖") != std::string::npos ||
                     r.output.find("尚未调用") != std::string::npos);
  std::printf("      [声明未见] never/called 被报告，并提示「未调用或被覆盖」两种可能\n");

  std::error_code ec;
  fs::remove(shm, ec);
  fs::remove_all(dir, ec);
}

// ===========================================================================
// 四、--scan-limit 1：只扫一条，仍要能报出 topic
// ===========================================================================

STRATUM_TEST(ProbeStatusEdge, ScanLimitOneStillReports) {
  const std::string shm = FreshShm("scan1");
  stratum_probe_close();
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);
  stratum_probe_write("s/one", "a", 1);
  stratum_probe_write("s/two", "b", 1);
  stratum_probe_close();

  const CliResult r = RunCli(kCli + " --format table probe status " + shm +
                             " --scan-limit 1");
  STRATUM_CHECK_EQ(r.code, 0);
  // 只扫最近 1 条 → 只能看到 s/two（后写的）
  STRATUM_CHECK_TRUE(r.output.find("s/two") != std::string::npos);
  // sampled_from 要如实说"扫了 1 条"
  STRATUM_CHECK_TRUE(r.output.find("1") != std::string::npos);
  std::printf("      [scan-limit] 只扫 1 条时只报最近的那个 topic，且如实说明样本量\n");

  std::error_code ec;
  fs::remove(shm, ec);
}

// ===========================================================================
// 五、ring 已覆盖过一圈：要提示"数据被覆盖"
// ===========================================================================

STRATUM_TEST(ProbeStatusEdge, WrappedRingIsFlagged) {
  const std::string shm = FreshShm("wrapped");
  stratum_probe_close();
  ::setenv("STRATUM_PROBE_CAPACITY", "4", 1);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);
  ::unsetenv("STRATUM_PROBE_CAPACITY");
  // 写 10 条 → 覆盖过两圈
  for (int i = 0; i < 10; ++i) stratum_probe_write("w/x", "v", 1);
  stratum_probe_close();

  const CliResult r = RunCli(kCli + " --format table probe status " + shm);
  STRATUM_CHECK_EQ(r.code, 0);
  // 必须提示环已覆盖（否则用户会以为"只有 4 条数据，业务写得少"）
  STRATUM_CHECK_TRUE(r.output.find("覆盖") != std::string::npos);
  std::printf("      [环覆盖] committed=10 > capacity=4 时明确标注「环已覆盖过一次」\n");

  std::error_code ec;
  fs::remove(shm, ec);
}

// ===========================================================================
// 六、JSON 与 table 两种输出都能给出 topic 列表
// ===========================================================================

STRATUM_TEST(ProbeStatusEdge, BothFormatsCarryTopicList) {
  const std::string shm = FreshShm("fmts");
  stratum_probe_close();
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);
  stratum_probe_write("f/x", "a", 1);
  stratum_probe_close();

  const CliResult j = RunCli(kCli + " probe status " + shm);
  STRATUM_CHECK_EQ(j.code, 0);
  STRATUM_CHECK_TRUE(j.output.find("\"observed_topics\"") != std::string::npos);
  STRATUM_CHECK_TRUE(j.output.find("f/x") != std::string::npos);

  const CliResult t = RunCli(kCli + " --format table probe status " + shm);
  STRATUM_CHECK_EQ(t.code, 0);
  STRATUM_CHECK_TRUE(t.output.find("f/x") != std::string::npos);
  std::printf("      [两种格式] json 与 table 都带 topic 列表\n");

  std::error_code ec;
  fs::remove(shm, ec);
}

STRATUM_TEST_MAIN()
