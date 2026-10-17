// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// **io 诊断上下文**（`--include-context`）
//
// 设计中最重要的约束（已写进 cmd_io.cc 的注释）：
//   **CONTEXT 是可选附加物，不参与完整性判定。**
//     · 缺少它的 bundle 仍然有效（老 bundle 照用）
//     · verify 不因它改变结论
//     · 写不出来不影响 export 成功（主次不能颠倒）
//
// 本套件验证的正是这几条 —— 尤其是"加了东西之后老行为不能变"。

// 标准库头文件必须最先包含
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"

namespace fs = std::filesystem;
using namespace stratum;

namespace {

#ifndef IOC_CLI_PATH
#define IOC_CLI_PATH "./stratum"
#endif

std::string kCli = IOC_CLI_PATH;

struct CliResult {
  int code = -1;
  std::string output;
};

CliResult RunCli(const std::string& cmd) {
  CliResult r;
  std::FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
  if (p == nullptr) return r;
  char buf[8192];
  while (std::fgets(buf, sizeof(buf), p) != nullptr) r.output += buf;
  const int st = ::pclose(p);
  r.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return r;
}

std::string TempDir(const char* tag) {
  const char* home = std::getenv("HOME");
  const std::string d = std::string(home ? home : ".") + "/stratum_ioc_" + tag;
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
  auto Add = [&](const std::string& n, int cnt) -> bool {
    TopicMetadata m;
    m.topic_name = n;
    m.type_name = "T";
    uint32_t id = 0;
    if (!t.RegisterTopic(m, &id).ok()) return false;
    std::vector<Event> ev;
    for (int i = 0; i < cnt; ++i) {
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

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

// ===========================================================================
// 一、★ 默认不生成 CONTEXT（老行为不变）
// ===========================================================================

STRATUM_TEST(IoContext, AbsentByDefault) {
  const std::string root = TempDir("def");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(root + "/s", 2, 20, 0, 0, &err));

  const CliResult r = RunCli(kCli + " io export " + root + "/s --to " + root + "/b");
  STRATUM_CHECK_EQ(r.code, 0);
  // ★ 默认行为必须完全不变
  STRATUM_CHECK_FALSE(fs::exists(root + "/b/CONTEXT.json"));
  // MANIFEST 仍在
  STRATUM_CHECK_TRUE(fs::exists(root + "/b/MANIFEST.json"));
  std::printf("      [默认] 不带 --include-context 时不生成 CONTEXT.json\n");
}

// ===========================================================================
// 二、★ 带 --include-context 时生成，且分层数字正确
// ===========================================================================

STRATUM_TEST(IoContext, GeneratedWithCorrectLayers) {
  const std::string root = TempDir("gen");
  std::string err;
  // 2 业务 × 20 = 40；1 探针 × 15 = 15
  STRATUM_CHECK_TRUE(MakeSession(root + "/s", 2, 20, 1, 15, &err));

  const CliResult r =
      RunCli(kCli + " io export " + root + "/s --to " + root + "/b --include-context");
  STRATUM_CHECK_EQ(r.code, 0);
  STRATUM_CHECK_TRUE(fs::exists(root + "/b/CONTEXT.json"));

  const std::string ctx_text = ReadFile(root + "/b/CONTEXT.json");
  STRATUM_CHECK_FALSE(ctx_text.empty());
  // 关键字段
  STRATUM_CHECK_TRUE(ctx_text.find("\"kind\": \"stratum-context\"") != std::string::npos);
  STRATUM_CHECK_TRUE(ctx_text.find("\"probe_prefix\": \"_probe.\"") != std::string::npos);
  // 分层数字：业务 40/2，探针 15/1
  STRATUM_CHECK_TRUE(ctx_text.find("\"events\": 40, \"topics\": 2") != std::string::npos);
  STRATUM_CHECK_TRUE(ctx_text.find("\"events\": 15, \"topics\": 1") != std::string::npos);
  // 必须有"可选"的声明
  STRATUM_CHECK_TRUE(ctx_text.find("缺失不影响") != std::string::npos);
  std::printf("      [生成] 业务 40/2、探针 15/1 分层正确，含可选性声明\n");
}

// ===========================================================================
// 三、★ 带 CONTEXT 的 bundle 仍能 verify（附加物不影响完整性判定）
// ===========================================================================

STRATUM_TEST(IoContext, BundleWithContextStillVerifies) {
  const std::string root = TempDir("vf");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(root + "/s", 1, 30, 0, 0, &err));

  const CliResult e =
      RunCli(kCli + " io export " + root + "/s --to " + root + "/b --include-context");
  STRATUM_CHECK_EQ(e.code, 0);

  // ★ 断言人类可读文本必须显式传 --format table（全局 --format 默认是 json）
  const CliResult v = RunCli(kCli + " --format table io verify " + root + "/b");
  // ★ 结论必须是"完整"
  STRATUM_CHECK_EQ(v.code, 0);
  STRATUM_CHECK_TRUE(v.output.find("完整") != std::string::npos ||
                     v.output.find("complete") != std::string::npos);
  std::printf("      [完整性] 带 CONTEXT 的 bundle 仍报完整（退出码 0）\n");
}

// ===========================================================================
// 四、★ 带 CONTEXT 的 bundle 仍能 import，且数据条数正确
// ===========================================================================

STRATUM_TEST(IoContext, BundleWithContextStillImports) {
  const std::string root = TempDir("im");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(root + "/s", 2, 25, 0, 0, &err));   // 50 条

  STRATUM_CHECK_EQ(RunCli(kCli + " io export " + root + "/s --to " + root +
                          "/b --include-context").code, 0);
  const CliResult im = RunCli(kCli + " io import " + root + "/b --to " + root + "/s2");
  STRATUM_CHECK_EQ(im.code, 0);

  // 导入后条数必须正确
  const CliResult q = RunCli(kCli + " query " + root + "/s2 --list-topics");
  STRATUM_CHECK_EQ(q.code, 0);
  STRATUM_CHECK_TRUE(q.output.find("\"count\":25") != std::string::npos);
  std::printf("      [导入] 带 CONTEXT 的 bundle 导入后条数正确（每 topic 25）\n");
}

// ===========================================================================
// 五、★ 老 bundle（无 CONTEXT）不受影响
// ===========================================================================

STRATUM_TEST(IoContext, OldBundleWithoutContextStillWorks) {
  const std::string root = TempDir("old");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(root + "/s", 1, 20, 0, 0, &err));

  // 不带 context 导出（等同"老 bundle"）
  STRATUM_CHECK_EQ(RunCli(kCli + " io export " + root + "/s --to " + root + "/b").code, 0);
  STRATUM_CHECK_FALSE(fs::exists(root + "/b/CONTEXT.json"));

  // verify 与 import 都必须正常
  STRATUM_CHECK_EQ(RunCli(kCli + " io verify " + root + "/b").code, 0);
  STRATUM_CHECK_EQ(RunCli(kCli + " io import " + root + "/b --to " + root + "/s2").code, 0);
  STRATUM_CHECK_TRUE(fs::exists(root + "/s2"));
  std::printf("      [兼容] 无 CONTEXT 的老 bundle 照常 verify / import\n");
}

// ===========================================================================
// 六、--probe-prefix 影响 CONTEXT 的分层
// ===========================================================================

STRATUM_TEST(IoContext, CustomPrefixChangesLayers) {
  const std::string root = TempDir("pfx");
  std::string err;
  STRATUM_CHECK_TRUE(MakeSession(root + "/s", 2, 10, 1, 10, &err));   // 20 业务 + 10 探针

  // 默认前缀
  STRATUM_CHECK_EQ(RunCli(kCli + " io export " + root + "/s --to " + root +
                          "/b1 --include-context").code, 0);
  const std::string c1 = ReadFile(root + "/b1/CONTEXT.json");
  STRATUM_CHECK_TRUE(c1.find("\"events\": 20, \"topics\": 2") != std::string::npos);
  STRATUM_CHECK_TRUE(c1.find("\"events\": 10, \"topics\": 1") != std::string::npos);

  // 换前缀 → 探针归零、业务变 3
  STRATUM_CHECK_EQ(RunCli(kCli + " io export " + root + "/s --to " + root +
                          "/b2 --include-context --probe-prefix zzz.").code, 0);
  const std::string c2 = ReadFile(root + "/b2/CONTEXT.json");
  STRATUM_CHECK_TRUE(c2.find("\"events\": 30, \"topics\": 3") != std::string::npos);
  STRATUM_CHECK_TRUE(c2.find("\"events\": 0, \"topics\": 0") != std::string::npos);
  std::printf("      [前缀] 换前缀后 CONTEXT 的分层随之改变（30/3 + 0/0）\n");
}

STRATUM_TEST_MAIN()
