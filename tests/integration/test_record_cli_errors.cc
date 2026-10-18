// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// `stratum record --write-batch-size` 的**参数校验**回归
//
// 背景（实测 bug）：
//   --write-batch-size 直接 strtoul 后赋值，没有任何范围校验：
//     · -1  → ULONG_MAX → WriterLoop 里 batch.reserve(SIZE_MAX)
//             → std::length_error → std::terminate（实测 rc=134）
//     · 0   → 写盘线程的出队条件 `batch.size() < 0` 永不成立
//             → 队列永不消费、Stop 永久阻塞（u 测试用 MockBackend 复现：
//               received=1 / recorded=0 / queue=1，Stop 3 秒不返回）
//     · abc → strtoul 返回 0，同样落入"永不消费"分支
//   以上全部**静默接受**：用户只看到进程卡死或崩溃。
//
// 本用例锁住修复后的契约：非法值一律在**进入录制前**以 USAGE(2) 拒绝，
// 且不创建会话目录（不能在报错前留下副作用）；合法值仍能通过 --dry-run。
//
// 【为什么测 CLI 而不只测 Recorder】Recorder::Start 的校验兜底的是 API 调用方，
// 但用户撞上的是命令行；两层的错误码与消息都必须对。

// 标准库头文件必须最先包含
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

#ifndef RECORD_CLI_PATH
#define RECORD_CLI_PATH "./stratum"
#endif

const std::string kCli = RECORD_CLI_PATH;

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
  // 进程唯一：两个 worktree 同时跑时不能共用同一目录（见 test_isolation.h）
  const std::string d = std::string(home ? home : ".") + "/stratum_record_err_" +
                        tag + "_" + std::to_string(::getpid());
  std::error_code ec;
  fs::remove_all(d, ec);
  return d;
}

}  // namespace

// ===========================================================================
// 一、★ 非法 write_batch_size → USAGE(2)，且不创建目录
// ===========================================================================

STRATUM_TEST(RecordCliErr, InvalidWriteBatchSizeIsUsageError) {
  const char* bad[] = {"0", "-1", "abc", "2000000", "999999999999999999999"};
  for (const char* v : bad) {
    const std::string dir = TempDir("wbs");
    // 注意：--format 是全局选项，必须在子命令之前（否则会被当成 record 的选项）
    const CliResult r = RunCli(kCli + " --format table record --dir " + dir +
                               " --write-batch-size " + v + " --duration 1s");
    ctx.param_desc = std::string("value=") + v;

    STRATUM_CHECK_EQ(r.code, 2);
    STRATUM_CHECK_TRUE(r.output.find("--write-batch-size") != std::string::npos);
    STRATUM_CHECK_TRUE(r.output.find("1..1000000") != std::string::npos);
    // 参数错误必须在产生副作用之前返回：目录不能被创建
    STRATUM_CHECK_FALSE(fs::exists(dir));
  }
  std::printf("      [USAGE] 5 个非法值全部退出码 2、点明选项、未创建目录\n");
}

// ===========================================================================
// 二、正例：合法值必须仍被接受（防止校验写反）
// ===========================================================================

STRATUM_TEST(RecordCliErr, ValidWriteBatchSizeAccepted) {
  const std::string dir = TempDir("wbs_ok");
  const CliResult r = RunCli(kCli + " --format table record --dir " + dir +
                             " --write-batch-size 512 --dry-run");
  STRATUM_CHECK_EQ(r.code, 0);
  STRATUM_CHECK_TRUE(r.output.find("写盘批量") != std::string::npos);
  STRATUM_CHECK_TRUE(r.output.find("512") != std::string::npos);
  // --dry-run 不创建任何文件
  STRATUM_CHECK_FALSE(fs::exists(dir));
  std::printf("      [正例] --write-batch-size 512 被接受，dry-run 不落盘\n");
}

// ===========================================================================
// 三、★ 删除安全：--overwrite 只能删"我认识的目录"
//
// 背景：merge 的 `--to /` 漏洞证明"有检查"不等于"边界正确"。record 的
// --overwrite 会 fs::remove_all(--dir)，所以：
//   · 含无关文件的普通目录必须被拒绝（结构白名单）；
//   · 危险路径（/、.、..、$HOME）即使**不带** --overwrite 也要在参数阶段
//     就被拒绝 —— 这样即使守卫将来被改坏，也只会落到 EXISTS 分支，
//     损失上限是"命令报错"，不会删任何东西。
// ===========================================================================

STRATUM_TEST(RecordCliErr, OverwriteRefusesUnrelatedDirAndKeepsFiles) {
  const std::string dir = TempDir("guard_keep");
  std::error_code ec;
  fs::create_directories(dir, ec);
  STRATUM_CHECK_FALSE(ec);
  {
    std::ofstream o(dir + "/keep.txt", std::ios::out | std::ios::trunc);
    o << "precious\n";
  }

  const CliResult r = RunCli(kCli + " --format table record --dir " + dir +
                             " --overwrite --duration 1s");
  STRATUM_CHECK_EQ(r.code, 2);   // USAGE：目标结构不是会话
  STRATUM_CHECK_TRUE(r.output.find("不安全") != std::string::npos);
  // 【关键】断言"文件还在"，而不只是退出码对 —— 退出码对了不代表没删
  STRATUM_CHECK_TRUE(fs::exists(dir + "/keep.txt"));
  std::printf("      [删除安全] --overwrite 拒绝含 keep.txt 的普通目录，文件仍在\n");
  fs::remove_all(dir, ec);
}

STRATUM_TEST(RecordCliErr, DangerousDirRejectedWithoutOverwrite) {
  const char* bad[] = {"/", ".", ".."};
  for (const char* p : bad) {
    ctx.param_desc = std::string("dir=") + p;
    const CliResult r =
        RunCli(kCli + " --format table record --dir " + p + " --duration 1s");
    STRATUM_CHECK_EQ(r.code, 2);
    STRATUM_CHECK_TRUE(r.output.find("不安全") != std::string::npos);
    STRATUM_CHECK_TRUE(fs::exists(p));   // 目标必须仍然存在
  }
  const char* home = std::getenv("HOME");
  if (home != nullptr) {
    const CliResult r = RunCli(kCli + " --format table record --dir " + home +
                               " --duration 1s");
    STRATUM_CHECK_EQ(r.code, 2);
    STRATUM_CHECK_TRUE(fs::exists(home));
  }
  std::printf("      [删除安全] / . .. $HOME 在参数阶段即拒绝，目标均未被删除\n");
}

STRATUM_TEST_MAIN()
