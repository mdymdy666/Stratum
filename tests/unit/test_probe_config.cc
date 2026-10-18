// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 埋点配置（probe_config）的测试
//
// 需求依据：docs/13-probe-requirements.md
//   PR-05 自动生成（O_EXCL、不覆盖、含注释）
//   PR-06 热生效 / 按 topic 覆盖
//   PR-07 非法或半写 → 保留旧值
//   PR-08 **全局 enabled 是 fail-safe（解析失败 → 关闭）**
//   PR-09 .lock 暂停重载；锁超时强制重载
//
// 【本文件最重要的两组用例】
//   1. **双语义**（PR-07 与 PR-08 的差异）：
//      解析失败时，全局 enabled 变 false，而其余项保留旧值。
//      这两条必须分别断言 —— 混在一起测就会掩盖"只实现了一半"。
//   2. **绝不改写用户文件**（S07 裁决）：
//      GenerateDefault 在文件已存在时必须失败且**内容不变**。
//
// 覆盖三类（项目标准）：
//   · 大数据量 —— 200 个 topic 段的配置
//   · 随机     —— 多种子随机 topic 数与随机开关
//   · 真实场景 —— ① 首次接入自动生成 ② 现场关掉某个 topic ③ 编辑器崩溃留下过期锁

// 标准库头文件必须最先包含
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "stratum/probe/probe_config.h"
#include "test_framework.h"
#include "test_isolation.h"

namespace fs = std::filesystem;

using namespace stratum::probe;

namespace {

std::string TempDir(const char* tag) {
  const std::string d = std::string("/tmp/stratum_pcfg_") + tag + "_" + UniqueSuffix();
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

std::string WriteFile(const std::string& path, const std::string& content) {
  std::ofstream o(path, std::ios::out | std::ios::trunc);
  o << content;
  o.close();
  return path;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// 造一个 .lock 文件：size 非空 = 锁定
void MakeLock(const std::string& cfg_path) {
  std::ofstream o(cfg_path + ".lock", std::ios::out | std::ios::trunc);
  o << "{\"pid\": 999999, \"host\": \"test\"}\n";
}

// 造一个"很旧"的锁：把 mtime 改到很久以前（模拟编辑器崩溃）
void MakeStaleLock(const std::string& cfg_path, int age_seconds) {
  MakeLock(cfg_path);
  std::error_code ec;
  const auto t = fs::file_time_type::clock::now() - std::chrono::seconds(age_seconds);
  fs::last_write_time(cfg_path + ".lock", t, ec);
}

// 一份合法配置
const char* kValid = R"(
[probe]
enabled = true

[defaults]
enabled = true
format = "kv"
sample_rate = 1.0
depth = 8

[topic."order/submit"]
enabled = true
format = "json"

[topic."sensor/temp"]
enabled = false
sample_rate = 0.01
)";

}  // namespace

// ===========================================================================
// 一、解析：基本正确性
// ===========================================================================

STRATUM_TEST(ProbeConfig, ParsesValidConfig) {
  ProbeSettings s;
  std::string err;
  STRATUM_CHECK_TRUE(ParseSettings(kValid, &s, &err));
  STRATUM_CHECK_TRUE(s.global_enabled);
  STRATUM_CHECK_EQ(s.defaults.format, std::string("kv"));
  STRATUM_CHECK_EQ(s.defaults.depth, static_cast<uint32_t>(8));
  STRATUM_CHECK_EQ(s.topics.size(), static_cast<size_t>(2));

  const auto& order = s.topics.at("order/submit");
  STRATUM_CHECK_TRUE(order.enabled);
  STRATUM_CHECK_EQ(order.format, std::string("json"));
  // 未指定的项继承 defaults
  STRATUM_CHECK_EQ(order.depth, static_cast<uint32_t>(8));

  const auto& temp = s.topics.at("sensor/temp");
  STRATUM_CHECK_FALSE(temp.enabled);
  STRATUM_CHECK_TRUE(temp.sample_rate > 0.009 && temp.sample_rate < 0.011);
}

STRATUM_TEST(ProbeConfig, RejectsIllegalValues) {
  // 每一种非法取值都必须被拒，且**不修改 out**
  const char* bad[] = {
      "[probe]\nenabled = maybe\n",                 // 非布尔
      "[defaults]\nformat = \"xml\"\n",             // 未知格式
      "[defaults]\nsample_rate = 2.0\n",            // 超范围
      "[defaults]\ndepth = 0\n",                    // 深度必须为正
      "[defaults]\nmax_rate_hz = abc\n",            // 非数字
      "[topic.\"x\"]\nsample_rate = -1\n",          // 负数
  };
  for (const char* text : bad) {
    ProbeSettings s;
    s.global_enabled = true;
    s.defaults.depth = 42;   // 哨兵：确认失败时未被改
    std::string err;
    const bool ok = ParseSettings(text, &s, &err);
    STRATUM_CHECK_FALSE(ok);
    STRATUM_CHECK_EQ(s.defaults.depth, static_cast<uint32_t>(42));   // 未改
    STRATUM_CHECK_TRUE(!err.empty());
  }
}

STRATUM_TEST(ProbeConfig, CommentsAndBlankLinesAreIgnored) {
  const char* text = R"(
# 这是注释
; 这也是注释

[probe]
enabled = true    # 行尾注释

[defaults]
format = "raw"
)";
  ProbeSettings s;
  std::string err;
  STRATUM_CHECK_TRUE(ParseSettings(text, &s, &err));
  STRATUM_CHECK_TRUE(s.global_enabled);
  STRATUM_CHECK_EQ(s.defaults.format, std::string("raw"));
}

STRATUM_TEST(ProbeConfig, TopicSectionInheritsDefaults) {
  // 只写 enabled 的 topic 段，其余全部继承 defaults
  const char* text = R"(
[defaults]
format = "json"
sample_rate = 0.5
depth = 32
reliable = true

[topic."a"]
enabled = false
)";
  ProbeSettings s;
  std::string err;
  STRATUM_CHECK_TRUE(ParseSettings(text, &s, &err));
  const auto& a = s.topics.at("a");
  STRATUM_CHECK_FALSE(a.enabled);
  STRATUM_CHECK_EQ(a.format, std::string("json"));      // 继承
  STRATUM_CHECK_TRUE(a.sample_rate > 0.49 && a.sample_rate < 0.51);
  STRATUM_CHECK_EQ(a.depth, static_cast<uint32_t>(32));
  STRATUM_CHECK_TRUE(a.reliable);
}

STRATUM_TEST(ProbeConfig, TopicSectionNameCanContainSlashesAndDots) {
  const char* text = R"(
[topic."a/b/c.d.e"]
enabled = false
)";
  ProbeSettings s;
  std::string err;
  STRATUM_CHECK_TRUE(ParseSettings(text, &s, &err));
  STRATUM_CHECK_EQ(s.topics.size(), static_cast<size_t>(1));
  STRATUM_CHECK_TRUE(s.topics.count("a/b/c.d.e") == 1);
}

// ===========================================================================
// 二、★ 双语义：全局 fail-safe vs 其余 fail-preserve
// ===========================================================================

STRATUM_TEST(ProbeConfigFailsafe, ParseErrorClosesGlobalButKeepsOthers) {
  // 【本文件最重要的用例】
  const std::string dir = TempDir("semantics");
  const std::string cfg = dir + "/probe.ini";
  WriteFile(cfg, kValid);

  ProbeConfig pc;
  LoadReport r = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(r.outcome),
                   static_cast<int>(LoadOutcome::kOk));
  STRATUM_CHECK_TRUE(pc.Snapshot().global_enabled);
  STRATUM_CHECK_EQ(pc.Snapshot().defaults.format, std::string("kv"));
  const uint32_t depth_before = pc.Snapshot().defaults.depth;

  // 把文件写坏（模拟编辑器写到一半）
  WriteFile(cfg, "[defaults]\nformat = \"json\"\ndepth = ???\n");

  r = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(r.outcome),
                   static_cast<int>(LoadOutcome::kParseError));
  STRATUM_CHECK_TRUE(r.global_failsafe_triggered);

  // ★ 全局：fail-safe -> 关闭
  STRATUM_CHECK_FALSE(pc.Snapshot().global_enabled);
  // ★ 其余：fail-preserve -> 保留旧值（不是文件里的 "json"，而是旧的 "kv"）
  STRATUM_CHECK_EQ(pc.Snapshot().defaults.format, std::string("kv"));
  STRATUM_CHECK_EQ(pc.Snapshot().defaults.depth, depth_before);

  // Resolve 也必须体现"全局关 => 一切都关"
  STRATUM_CHECK_FALSE(pc.Resolve("order/submit").enabled);
  std::printf("      [双语义] 解析失败：全局关闭(fail-safe)、其余保留旧值(fail-preserve)\n");
}

STRATUM_TEST(ProbeConfigFailsafe, CountersRecordFailsafe) {
  const std::string dir = TempDir("counters");
  const std::string cfg = dir + "/probe.ini";
  WriteFile(cfg, kValid);
  ProbeConfig pc;
  (void)pc.Load(cfg);
  STRATUM_CHECK_EQ(pc.counters().loads_ok, static_cast<uint64_t>(1));

  WriteFile(cfg, "[probe]\nenabled = notabool\n");
  (void)pc.Load(cfg);
  STRATUM_CHECK_EQ(pc.counters().loads_parse_error, static_cast<uint64_t>(1));
  STRATUM_CHECK_EQ(pc.counters().loads_failsafe, static_cast<uint64_t>(1));
}

// ===========================================================================
// 三、热重载（PR-06）
// ===========================================================================

STRATUM_TEST(ProbeConfig, ReloadAppliesChanges) {
  const std::string dir = TempDir("reload");
  const std::string cfg = dir + "/probe.ini";
  WriteFile(cfg, kValid);

  ProbeConfig pc;
  (void)pc.Load(cfg);
  STRATUM_CHECK_TRUE(pc.Resolve("order/submit").enabled);
  STRATUM_CHECK_EQ(pc.Resolve("order/submit").format, std::string("json"));

  // 改：关掉 order/submit，并把 format 改成 raw
  WriteFile(cfg, R"(
[probe]
enabled = true
[defaults]
format = "kv"
[topic."order/submit"]
enabled = false
format = "raw"
)");
  const LoadReport r = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(r.outcome),
                   static_cast<int>(LoadOutcome::kOk));
  STRATUM_CHECK_FALSE(pc.Resolve("order/submit").enabled);
  STRATUM_CHECK_EQ(pc.Resolve("order/submit").format, std::string("raw"));
}

STRATUM_TEST(ProbeConfig, GlobalOffDisablesEverything) {
  const std::string dir = TempDir("globaloff");
  const std::string cfg = dir + "/probe.ini";
  WriteFile(cfg, kValid);
  ProbeConfig pc;
  (void)pc.Load(cfg);
  STRATUM_CHECK_TRUE(pc.Resolve("order/submit").enabled);

  WriteFile(cfg, "[probe]\nenabled = false\n[topic.\"order/submit\"]\nenabled = true\n");
  (void)pc.Load(cfg);
  // 即使 topic 段写 true，全局关也必须是关（安全方向）
  STRATUM_CHECK_FALSE(pc.Resolve("order/submit").enabled);
}

STRATUM_TEST(ProbeConfig, MissingFileKeepsSettings) {
  const std::string dir = TempDir("missing");
  const std::string cfg = dir + "/probe.ini";
  WriteFile(cfg, kValid);
  ProbeConfig pc;
  (void)pc.Load(cfg);
  const auto before = pc.Snapshot();

  fs::remove(cfg);   // 文件消失
  const LoadReport r = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(r.outcome),
                   static_cast<int>(LoadOutcome::kMissing));
  // S04 裁决：文件消失 -> 保留现状（不能突然把监控全关或全开）
  STRATUM_CHECK_TRUE(pc.Snapshot() == before);
}

// ===========================================================================
// 四、.lock（PR-09 / S08）
// ===========================================================================

STRATUM_TEST(ProbeConfigLock, LockedEditorSkipsReload) {
  const std::string dir = TempDir("lock");
  const std::string cfg = dir + "/probe.ini";
  WriteFile(cfg, kValid);
  ProbeConfig pc;
  (void)pc.Load(cfg);
  const bool before = pc.Snapshot().topics.at("order/submit").enabled;

  // 改内容，但放一个"新鲜"的锁
  WriteFile(cfg, "[probe]\nenabled = true\n[topic.\"order/submit\"]\nenabled = false\n");
  MakeLock(cfg);

  const LoadReport r = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(r.outcome),
                   static_cast<int>(LoadOutcome::kLockedByEditor));
  STRATUM_CHECK_FALSE(r.applied);
  STRATUM_CHECK_EQ(pc.Snapshot().topics.at("order/submit").enabled, before);
  STRATUM_CHECK_EQ(pc.counters().loads_locked_skipped, static_cast<uint64_t>(1));
}

STRATUM_TEST(ProbeConfigLock, StaleLockForcesReload) {
  // S08 的裁决：锁超时必须强制重载，否则崩溃的编辑器会让监控永久冻结
  const std::string dir = TempDir("stalelock");
  const std::string cfg = dir + "/probe.ini";
  WriteFile(cfg, kValid);
  ProbeConfig pc;
  (void)pc.Load(cfg);

  WriteFile(cfg, "[probe]\nenabled = true\n[topic.\"order/submit\"]\nenabled = false\n");
  MakeStaleLock(cfg, 20 * 60);   // 20 分钟前的锁

  // 锁超时设为 5 分钟 -> 20 分钟已超时 -> 应强制重载
  const LoadReport r = pc.Load(cfg, std::chrono::milliseconds(5 * 60 * 1000));
  STRATUM_CHECK_EQ(static_cast<int>(r.outcome),
                   static_cast<int>(LoadOutcome::kOk));
  STRATUM_CHECK_FALSE(pc.Resolve("order/submit").enabled);
  STRATUM_CHECK_EQ(pc.counters().lock_forced_reloads, static_cast<uint64_t>(1));
  std::printf("      [锁超时] 20 分钟的过期锁触发强制重载\n");
}

STRATUM_TEST(ProbeConfigLock, EmptyLockFileIsNotALock) {
  const std::string dir = TempDir("emptylock");
  const std::string cfg = dir + "/probe.ini";
  WriteFile(cfg, kValid);
  ProbeConfig pc;
  (void)pc.Load(cfg);

  // 空文件 = 未锁定（与 config 模块的 .lock 语义一致）
  WriteFile(cfg + ".lock", "");
  WriteFile(cfg, "[probe]\nenabled = false\n");
  const LoadReport r = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(r.outcome),
                   static_cast<int>(LoadOutcome::kOk));
  STRATUM_CHECK_FALSE(pc.Snapshot().global_enabled);
}

// ===========================================================================
// 五、★ GenerateDefault：绝不改写用户文件（S07 裁决）
// ===========================================================================

STRATUM_TEST(ProbeGenerate, CreatesFileWithComments) {
  const std::string dir = TempDir("gen");
  const std::string cfg = dir + "/probe.ini";
  std::string err;
  STRATUM_CHECK_TRUE(ProbeConfig::GenerateDefault(cfg, {"order/submit", "sensor/temp"}, &err));
  STRATUM_CHECK_TRUE(fs::exists(cfg));

  const std::string text = ReadFile(cfg);
  // 必须含：全局开关、两条错误语义的说明、两个 topic 段
  STRATUM_CHECK_TRUE(text.find("[probe]") != std::string::npos);
  STRATUM_CHECK_TRUE(text.find("enabled = true") != std::string::npos);
  STRATUM_CHECK_TRUE(text.find("fail-safe") != std::string::npos);
  STRATUM_CHECK_TRUE(text.find("fail-preserve") != std::string::npos);
  STRATUM_CHECK_TRUE(text.find("order/submit") != std::string::npos);
  STRATUM_CHECK_TRUE(text.find("sensor/temp") != std::string::npos);
  // 生成的配置必须能被自己解析回来
  ProbeSettings s;
  STRATUM_CHECK_TRUE(ParseSettings(text, &s, &err));
  STRATUM_CHECK_EQ(s.topics.size(), static_cast<size_t>(2));
}

STRATUM_TEST(ProbeGenerate, NeverOverwritesExistingFile) {
  // 【S07 的核心裁决】宁可报告未配置项，也绝不悄悄改用户文件
  const std::string dir = TempDir("gen_noover");
  const std::string cfg = dir + "/probe.ini";
  const std::string user_content = "# 用户的注释\n[probe]\nenabled = false\n";
  WriteFile(cfg, user_content);

  std::string err;
  STRATUM_CHECK_FALSE(ProbeConfig::GenerateDefault(cfg, {"x"}, &err));
  STRATUM_CHECK_TRUE(err.find("已存在") != std::string::npos ||
                     err.find("不覆盖") != std::string::npos);
  // 内容必须原封不动
  STRATUM_CHECK_EQ(ReadFile(cfg), user_content);
  std::printf("      [裁决] 已存在时不覆盖，用户文件内容原封不动\n");
}

STRATUM_TEST(ProbeGenerate, ReportsUnwritableDirectory) {
  const std::string cfg = "/proc/definitely_not_writable/probe.ini";
  std::string err;
  STRATUM_CHECK_FALSE(ProbeConfig::GenerateDefault(cfg, {}, &err));
  STRATUM_CHECK_TRUE(!err.empty());
}

// ===========================================================================
// 六、大数据量
// ===========================================================================

STRATUM_TEST(ProbeConfig, LargeConfigTwoHundredTopics) {
  std::ostringstream o;
  o << "[probe]\nenabled = true\n[defaults]\nformat = \"kv\"\n";
  const int kN = 200;
  for (int i = 0; i < kN; ++i) {
    o << "\n[topic.\"big/" << i << "\"]\n";
    o << "enabled = " << ((i % 3 == 0) ? "false" : "true") << "\n";
    o << "sample_rate = 0." << (i % 9 + 1) << "\n";
  }
  ProbeSettings s;
  std::string err;
  STRATUM_CHECK_TRUE(ParseSettings(o.str(), &s, &err));
  STRATUM_CHECK_EQ(s.topics.size(), static_cast<size_t>(kN));
  // 抽查：每 3 个关掉一个
  int off = 0;
  for (int i = 0; i < kN; ++i) {
    if (!s.topics.at("big/" + std::to_string(i)).enabled) ++off;
  }
  STRATUM_CHECK_EQ(off, (kN + 2) / 3);
  std::printf("      [规模] %d 个 topic 段解析正确，其中 %d 个关闭\n", kN, off);
}

// ===========================================================================
// 七、随机（多种子）
// ===========================================================================

STRATUM_TEST(ProbeConfig, RandomConfigsMultipleSeeds) {
  for (uint64_t seed : {5ULL, 55ULL, 555ULL}) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> n_d(1, 30);
    const int n = n_d(rng);

    std::ostringstream o;
    o << "[probe]\nenabled = " << ((rng() % 2) ? "true" : "false") << "\n";
    o << "[defaults]\nformat = \"kv\"\n";
    std::vector<std::string> names;
    std::vector<bool> en;
    for (int i = 0; i < n; ++i) {
      const std::string name = "rnd/" + std::to_string(i);
      const bool e = (rng() % 2) == 0;
      names.push_back(name);
      en.push_back(e);
      o << "\n[topic.\"" << name << "\"]\nenabled = " << (e ? "true" : "false") << "\n";
    }
    ProbeSettings s;
    std::string err;
    STRATUM_CHECK_TRUE(ParseSettings(o.str(), &s, &err));
    STRATUM_CHECK_EQ(s.topics.size(), static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      STRATUM_CHECK_EQ(s.topics.at(names[static_cast<size_t>(i)]).enabled,
                       en[static_cast<size_t>(i)]);
    }
  }
}

// ===========================================================================
// 八、真实场景（对应 docs/13 的场景编号）
// ===========================================================================

// ---------------------------------------------------------------------------
// 场景 1（S07）：首次接入 —— 业务跑起来，配置自动生成
// ---------------------------------------------------------------------------
STRATUM_TEST(ProbeConfigReal, FirstIntegrationAutoGenerates) {
  const std::string dir = TempDir("real1");
  const std::string cfg = dir + "/probe.ini";

  // 业务第一次调用探针时，配置还不存在
  ProbeConfig pc;
  LoadReport r = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(r.outcome),
                   static_cast<int>(LoadOutcome::kMissing));

  // 库里发现"文件不存在"，于是生成一份默认配置
  std::string err;
  STRATUM_CHECK_TRUE(ProbeConfig::GenerateDefault(cfg, {"order/submit"}, &err));

  // 再加载：应当成功，且探针默认开启
  r = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(r.outcome),
                   static_cast<int>(LoadOutcome::kOk));
  STRATUM_CHECK_TRUE(pc.Resolve("order/submit").enabled);
  std::printf("      [场景1/S07] 首次接入：自动生成配置，探针默认开启\n");
}

// ---------------------------------------------------------------------------
// 场景 2（S04）：现场发现某 topic 太吵，改配置关掉它
// ---------------------------------------------------------------------------
STRATUM_TEST(ProbeConfigReal, OperatorDisablesNoisyTopic) {
  const std::string dir = TempDir("real2");
  const std::string cfg = dir + "/probe.ini";
  WriteFile(cfg, R"(
[probe]
enabled = true
[defaults]
format = "kv"
[topic."sensor/temp"]
enabled = true
[topic."debug/verbose"]
enabled = true
)");
  ProbeConfig pc;
  (void)pc.Load(cfg);
  STRATUM_CHECK_TRUE(pc.Resolve("debug/verbose").enabled);

  // 运维改一行：把 debug/verbose 关掉
  WriteFile(cfg, R"(
[probe]
enabled = true
[defaults]
format = "kv"
[topic."sensor/temp"]
enabled = true
[topic."debug/verbose"]
enabled = false
)");
  const LoadReport r = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(r.outcome),
                   static_cast<int>(LoadOutcome::kOk));
  STRATUM_CHECK_FALSE(pc.Resolve("debug/verbose").enabled);
  // 另一个 topic 不受影响 —— 这是"按 topic 覆盖"的意义
  STRATUM_CHECK_TRUE(pc.Resolve("sensor/temp").enabled);
  std::printf("      [场景2/S04] 改一行关掉吵的那个 topic，其他不受影响\n");
}

// ---------------------------------------------------------------------------
// 场景 3（S08）：编辑器崩溃留下过期锁，监控不能永久冻结
// ---------------------------------------------------------------------------
STRATUM_TEST(ProbeConfigReal, CrashedEditorStaleLockRecovered) {
  const std::string dir = TempDir("real3");
  const std::string cfg = dir + "/probe.ini";
  WriteFile(cfg, kValid);
  ProbeConfig pc;
  (void)pc.Load(cfg);

  // 有人用编辑器改了配置，然后编辑器崩了 —— 留下一个锁和半写文件
  WriteFile(cfg, "[probe]\nenabled = true\n[topic.\"order/submit\"]\nenabled = false\n");
  MakeStaleLock(cfg, 30 * 60);   // 30 分钟前

  // 第一轮：锁还没到超时（设为 60 分钟）-> 跳过，监控保持旧状态
  LoadReport r = pc.Load(cfg, std::chrono::milliseconds(60 * 60 * 1000));
  STRATUM_CHECK_EQ(static_cast<int>(r.outcome),
                   static_cast<int>(LoadOutcome::kLockedByEditor));
  STRATUM_CHECK_TRUE(pc.Resolve("order/submit").enabled);   // 仍是旧值

  // 第二轮：按 5 分钟超时 -> 30 分钟已超时 -> 强制重载，新配置生效
  r = pc.Load(cfg, std::chrono::milliseconds(5 * 60 * 1000));
  STRATUM_CHECK_EQ(static_cast<int>(r.outcome),
                   static_cast<int>(LoadOutcome::kOk));
  STRATUM_CHECK_FALSE(pc.Resolve("order/submit").enabled);
  std::printf("      [场景3/S08] 崩溃编辑器留下的锁在超时后被越过，配置恢复生效\n");
}

STRATUM_TEST_MAIN()
