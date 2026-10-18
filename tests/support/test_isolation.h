// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// tests/support/test_isolation.h
//
// 测试进程之间的隔离约定（2026-10-18 加入）。
//
// 【为什么必须有】
// 两个 worktree（或同一 worktree 的两个 ctest 进程）同时跑全量测试时，
// 测试代码里硬编码的 `/tmp/stratum_*`、`/dev/shm/stratum_*` 会被两个进程共用：
//   · 后启动的 fs::remove_all 删掉先启动的会话/共享内存；
//   · LSMengine 的 LOCK 文件让第二个进程直接打不开库（**确定性失败**）；
//   · DDS 同 domain 的 participant 互相发现、互收样本。
// 实测后果：test_cli_probe / test_verify_business 随机失败，单跑即通过。
//
// 【约定】
//   · 一切测试路径 = 前缀 + tag + "_" + UniqueSuffix()
//   · 会话放 $HOME，不放 /tmp（项目纪律：/tmp 会被系统清理）
//   · 需要 DDS 的测试用 TestDomain(slot)，不要硬编码 domain
//
// 【新测试请直接用】
//   const std::string dir = UniqueDir("stratum_mytest", tag);
//   const int domain = TestDomain(0);
//
// 【回归方式】CI 里"两份全量 ctest 同时跑"必须都绿；只跑一遍无法发现本类问题。
#ifndef STRATUM_TESTS_TEST_ISOLATION_H_
#define STRATUM_TESTS_TEST_ISOLATION_H_

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace stratum_test_isolation {

// 进程唯一后缀：<pid>_<进程内计数>。
// 用计数而不是只用 pid：同一进程内同一个 tag 被调用多次（例如随机用例的
// 循环）也必须落不同目录，否则会自己删自己。
inline std::string UniqueSuffix() {
  static std::atomic<uint32_t> counter{0};
  return std::to_string(::getpid()) + "_" +
         std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

// 会话/临时目录：$HOME/<prefix>_<tag>_<pid>_<n>
inline std::string UniqueDir(const char* prefix, const std::string& tag) {
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/" + prefix + "_" + tag + "_" +
         UniqueSuffix();
}

// DDS domain：base + (pid % 20) * 8 + slot。
// base 默认 60 —— 刻意避开历史测试用过的 33~37 / 43~47，这样即使另一个
// worktree 跑的是**旧版本测试**（固定 domain），也不会与本进程撞域。
// 可用 STRATUM_TEST_DOMAIN_BASE 显式覆盖。
// 残留风险：两个进程 pid 模 20 相同（约 5%）。需要绝对隔离时给两边设
// 不同的 STRATUM_TEST_DOMAIN_BASE。
inline int TestDomain(int slot) {
  static const int base = []() {
    const char* env = std::getenv("STRATUM_TEST_DOMAIN_BASE");
    if (env != nullptr && env[0] != '\0') return std::atoi(env);
    return 60;
  }();
  const int pid_offset =
      static_cast<int>(static_cast<unsigned>(::getpid()) % 20u);
  return base + pid_offset * 8 + slot;   // 最大 60 + 152 + 7 = 219 <= 232
}

}  // namespace stratum_test_isolation

// 简写，测试文件里直接 UniqueSuffix() / UniqueDir() / TestDomain()
using stratum_test_isolation::TestDomain;
using stratum_test_isolation::UniqueDir;
using stratum_test_isolation::UniqueSuffix;

#endif  // STRATUM_TESTS_TEST_ISOLATION_H_
