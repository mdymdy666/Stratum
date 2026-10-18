// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// **热重载时序**测试
//
// 与 test_probe_config.cc 的分工：
//   test_probe_config.cc    —— 单次加载的语义（双语义、锁、生成）
//   test_probe_hot_reload.cc —— **连续改动**的时序：
//       · 快速连续改 5 次，最终值必须是最后一次
//       · 改动过程中的中间错误不能污染最终状态
//       · 从"解析失败(fail-safe 关闭)"恢复到"正常"要能恢复
//       · .lock 出现 → 暂停；锁删除 → 立即恢复
//
// 【为什么单独一个文件】实际部署里配置不是"改一次读一次"，
// 而是"有人在编辑器里反复保存"。中间态（半写、语法错、锁存在）
// 会频繁出现 —— 这些中间态不能把最终状态搞坏。

// 标准库头文件必须最先包含
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
  const char* home = std::getenv("HOME");
  const std::string d = std::string(home ? home : ".") + "/stratum_hr_" + tag + "_" + UniqueSuffix();
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

void WriteText(const std::string& path, const std::string& text) {
  std::ofstream o(path, std::ios::out | std::ios::trunc);
  o << text;
}

std::string MakeCfg(const std::string& topic, bool enabled, const char* format = "kv") {
  return std::string("[probe]\nenabled = true\n[defaults]\nformat = \"") + format +
         "\"\n[topic.\"" + topic + "\"]\nenabled = " +
         (enabled ? "true" : "false") + "\n";
}

}  // namespace

// ===========================================================================
// 一、★ 快速连续改 5 次：最终值必须是最后一次
// ===========================================================================

STRATUM_TEST(ProbeReload, RapidChangesEndWithLastValue) {
  const std::string dir = TempDir("rapid");
  const std::string cfg = dir + "/p.ini";

  ProbeConfig pc;
  STRATUM_CHECK_EQ(static_cast<int>(pc.Load(cfg).outcome),
                   static_cast<int>(LoadOutcome::kMissing));

  // 交替 true/false **六次**（i = 0..5）：i 为奇数时 false，末次 i=5 是 false。
  //
  // 【这里踩过一次】最初写"五次 (0..4)"，并断言末次为 false ——
  // 但 i=4 是偶数 → true。周期性奇偶的末项必须自己数一遍，
  // 不能凭"交替几次应该落在某一侧"的直觉。改成偶数轮次后末项与直觉一致。
  const int kRounds = 6;
  for (int i = 0; i < kRounds; ++i) {
    WriteText(cfg, MakeCfg("r/t", (i % 2) == 0));
    const auto rep = pc.Load(cfg);
    STRATUM_CHECK_EQ(static_cast<int>(rep.outcome), static_cast<int>(LoadOutcome::kOk));
  }
  // 末次 i=5 是奇数 → false
  STRATUM_CHECK_FALSE(pc.Resolve("r/t").enabled);
  STRATUM_CHECK_EQ(pc.counters().loads_ok, static_cast<uint64_t>(kRounds));
  std::printf("      [连续改] 5 次交替改动，最终值 = 最后一次（关闭）\n");

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ===========================================================================
// 二、中间出现语法错 → 全局关闭（fail-safe），修好后能恢复
// ===========================================================================
//
// 【为什么这条重要】fail-safe 是"解析失败就关"。
// 但若"关"这个动作是不可恢复的，一次手滑就永久停掉监控。
// 必须验证：修好文件后能重新打开。

STRATUM_TEST(ProbeReload, FailsafeRecoversAfterFix) {
  const std::string dir = TempDir("recover");
  const std::string cfg = dir + "/p.ini";

  ProbeConfig pc;
  WriteText(cfg, MakeCfg("x/y", true));
  (void)pc.Load(cfg);
  STRATUM_CHECK_TRUE(pc.Snapshot().global_enabled);

  // 写坏
  WriteText(cfg, "[probe]\nenabled = notabool\n");
  const auto bad = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(bad.outcome), static_cast<int>(LoadOutcome::kParseError));
  STRATUM_CHECK_FALSE(pc.Snapshot().global_enabled);   // fail-safe

  // 修好
  WriteText(cfg, MakeCfg("x/y", true));
  const auto fixed = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(fixed.outcome), static_cast<int>(LoadOutcome::kOk));
  STRATUM_CHECK_TRUE(pc.Snapshot().global_enabled);    // 恢复
  STRATUM_CHECK_TRUE(pc.Resolve("x/y").enabled);
  std::printf("      [恢复] 写坏 → 全局关闭；修好后 → 全局恢复且 topic 生效\n");

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ===========================================================================
// 三、.lock 出现 → 暂停；锁删除 → 立即恢复
// ===========================================================================

STRATUM_TEST(ProbeReload, LockPausesThenResumes) {
  const std::string dir = TempDir("lockresume");
  const std::string cfg = dir + "/p.ini";

  ProbeConfig pc;
  WriteText(cfg, MakeCfg("l/t", true));
  (void)pc.Load(cfg);
  STRATUM_CHECK_TRUE(pc.Resolve("l/t").enabled);

  // 加锁 + 改配置（想关掉）
  WriteText(cfg, MakeCfg("l/t", false));
  {
    std::ofstream o(cfg + ".lock", std::ios::out | std::ios::trunc);
    o << "editing\n";
  }
  const auto locked = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(locked.outcome),
                   static_cast<int>(LoadOutcome::kLockedByEditor));
  STRATUM_CHECK_TRUE(pc.Resolve("l/t").enabled);   // 旧值

  // 删锁 → 下一轮应读到新值
  std::error_code ec;
  fs::remove(cfg + ".lock", ec);
  const auto resumed = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(resumed.outcome), static_cast<int>(LoadOutcome::kOk));
  STRATUM_CHECK_FALSE(pc.Resolve("l/t").enabled);
  std::printf("      [锁] 锁存在时跳过（保留旧值），删锁后立即读到新值\n");

  fs::remove_all(dir, ec);
}

// ===========================================================================
// 四、format / sample_rate / depth 的连续切换
// ===========================================================================

STRATUM_TEST(ProbeReload, TuningChangesApplyInOrder) {
  const std::string dir = TempDir("tune");
  const std::string cfg = dir + "/p.ini";

  ProbeConfig pc;
  WriteText(cfg, MakeCfg("t/x", true, "kv"));
  (void)pc.Load(cfg);
  STRATUM_CHECK_EQ(pc.Resolve("t/x").format, std::string("kv"));

  const char* formats[] = {"json", "raw", "kv", "json"};
  for (const char* f : formats) {
    WriteText(cfg, MakeCfg("t/x", true, f));
    const auto rep = pc.Load(cfg);
    STRATUM_CHECK_EQ(static_cast<int>(rep.outcome), static_cast<int>(LoadOutcome::kOk));
    STRATUM_CHECK_EQ(pc.Resolve("t/x").format, std::string(f));
  }
  // 最后一轮是 json
  STRATUM_CHECK_EQ(pc.Resolve("t/x").format, std::string("json"));
  std::printf("      [调参] kv→json→raw→kv→json 四次切换全部按序生效\n");

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ===========================================================================
// 五、配置文件被删除 → 保留现状（不清零、不重置）
// ===========================================================================
//
// S04 的裁决：文件消失 -> 保留当前配置。
// 若实现"文件消失就恢复默认"，一次误删就会把运维精心设的开关全冲掉。

STRATUM_TEST(ProbeReload, FileDeletionKeepsCurrentSettings) {
  const std::string dir = TempDir("del");
  const std::string cfg = dir + "/p.ini";

  ProbeConfig pc;
  WriteText(cfg, "[probe]\nenabled = false\n[topic.\"d/x\"]\nenabled = false\n");
  (void)pc.Load(cfg);
  STRATUM_CHECK_FALSE(pc.Snapshot().global_enabled);
  STRATUM_CHECK_FALSE(pc.Resolve("d/x").enabled);

  std::error_code ec;
  fs::remove(cfg, ec);

  const auto rep = pc.Load(cfg);
  STRATUM_CHECK_EQ(static_cast<int>(rep.outcome), static_cast<int>(LoadOutcome::kMissing));
  // 状态完全不变
  STRATUM_CHECK_FALSE(pc.Snapshot().global_enabled);
  STRATUM_CHECK_FALSE(pc.Resolve("d/x").enabled);
  std::printf("      [删除] 文件消失后配置保持不变（不重置为默认）\n");

  fs::remove_all(dir, ec);
}

// ===========================================================================
// 六、多轮 Load 的计数器准确
// ===========================================================================

STRATUM_TEST(ProbeReload, CountersAccumulateCorrectly) {
  const std::string dir = TempDir("cnt");
  const std::string cfg = dir + "/p.ini";

  ProbeConfig pc;
  WriteText(cfg, MakeCfg("c/x", true));
  for (int i = 0; i < 3; ++i) (void)pc.Load(cfg);
  STRATUM_CHECK_EQ(pc.counters().loads_ok, static_cast<uint64_t>(3));

  WriteText(cfg, "[probe]\nenabled = bad\n");
  for (int i = 0; i < 2; ++i) (void)pc.Load(cfg);
  STRATUM_CHECK_EQ(pc.counters().loads_parse_error, static_cast<uint64_t>(2));
  STRATUM_CHECK_EQ(pc.counters().loads_failsafe, static_cast<uint64_t>(2));

  WriteText(cfg, MakeCfg("c/x", true));
  {
    std::ofstream o(cfg + ".lock", std::ios::out | std::ios::trunc);
    o << "x\n";
  }
  for (int i = 0; i < 4; ++i) (void)pc.Load(cfg);
  STRATUM_CHECK_EQ(pc.counters().loads_locked_skipped, static_cast<uint64_t>(4));
  std::printf("      [计数] ok=3 / parse_error=2 / failsafe=2 / locked=4，各自独立累计\n");

  std::error_code ec;
  fs::remove_all(dir, ec);
}

STRATUM_TEST_MAIN()
