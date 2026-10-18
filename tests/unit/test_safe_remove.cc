// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/safe_path.h 的回归测试
//
// 【为什么需要】`--overwrite` 路径会 fs::remove_all(用户输入)。历史缺陷
// （merge 的 `--to /`）说明"有检查"不等于"检查在边界上正确"。本文件按
// 层验证守卫，并用"含 keep.txt 的普通目录"验证**结构白名单**：
// 不是会话/不是 bundle 的非空目录，永远不能被当作可覆盖目标。
//
// 用例设计原则（学 merge 的教训）：守卫是纯函数，本文件**不执行任何删除**，
// 所以即使守卫被改坏，测试失败也不会毁掉任何数据 —— 损失上限可控。

// 标准库头文件必须最先包含
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "safe_path.h"
#include "test_framework.h"

namespace fs = std::filesystem;

using stratum::cli::IsSafeToRemove;
using stratum::cli::LooksLikeEmptyOrBundle;
using stratum::cli::LooksLikeEmptyOrSession;
using stratum::cli::LooksLikeEmptyOrSessionOrBundle;

namespace {

// 只用于单独验证第 1/2 层（把结构层短路成"永远接受"）
bool AlwaysAccept(const std::string&, std::string*) { return true; }

std::string Home() {
  const char* home = std::getenv("HOME");
  return home != nullptr ? std::string(home) : std::string(".");
}

std::string UniqueDir(const char* tag) {
  const std::string d =
      Home() + "/stratum_safe_" + tag + "_" + std::to_string(::getpid());
  std::error_code ec;
  fs::remove_all(d, ec);
  return d;
}

}  // namespace

// ===========================================================================
// 一、第 1/2 层：规范化后拒绝根目录 / $HOME / 当前目录及其祖先
// ===========================================================================

STRATUM_TEST(SafeRemove, RejectsRootAndDotForms) {
  const char* bad[] = {"/", "//", "///", "/.", "/..", "/tmp/../..", ".", "..", ""};
  for (const char* p : bad) {
    std::string why;
    ctx.param_desc = std::string("path=") + p;
    STRATUM_CHECK_FALSE(IsSafeToRemove(p, AlwaysAccept, &why));
    STRATUM_CHECK_FALSE(why.empty());
  }
  std::printf("      [第1/2层] / // /. /.. /tmp/../.. . .. 空串 全部拒绝\n");
}

STRATUM_TEST(SafeRemove, RejectsHomeAndCwdAncestors) {
  std::string why;
  const std::string home = Home();
  STRATUM_CHECK_FALSE(IsSafeToRemove(home, AlwaysAccept, &why));
  STRATUM_CHECK_FALSE(IsSafeToRemove(home + "/", AlwaysAccept, &why));
  STRATUM_CHECK_FALSE(IsSafeToRemove(home + "/.", AlwaysAccept, &why));

  std::error_code ec;
  const std::string cwd = fs::current_path(ec).string();
  STRATUM_CHECK_FALSE(IsSafeToRemove(cwd, AlwaysAccept, &why));
  // 当前目录的祖先（例如 build 的父目录）
  STRATUM_CHECK_FALSE(IsSafeToRemove(cwd + "/..", AlwaysAccept, &why));
  // 软链接绕过：link -> 当前目录（规范化后仍是祖先）
  const std::string link = cwd + "/../stratum_safe_link_to_cwd_" +
                           std::to_string(::getpid());
  fs::remove(link, ec);
  fs::create_symlink(cwd, link, ec);
  STRATUM_CHECK_FALSE(ec);
  STRATUM_CHECK_FALSE(IsSafeToRemove(link, AlwaysAccept, &why));
  fs::remove(link, ec);
  std::printf("      [第2层] $HOME / cwd / cwd 祖先 / 软链接到 cwd 全部拒绝\n");
}

// ===========================================================================
// 二、★ 第 3 层：结构白名单 —— 含 keep.txt 的普通目录不能当可覆盖目标
// ===========================================================================

STRATUM_TEST(SafeRemove, StructureWhitelistRejectsUnrelatedDir) {
  const std::string dir = UniqueDir("unrelated");
  std::error_code ec;
  fs::create_directories(dir, ec);
  STRATUM_CHECK_FALSE(ec);
  {
    std::ofstream o(dir + "/keep.txt");
    o << "precious\n";
  }

  std::string why;
  // 一个"非空且不是会话/不是 bundle"的目录：三层白名单都必须拒绝
  STRATUM_CHECK_FALSE(IsSafeToRemove(dir, LooksLikeEmptyOrSession, &why));
  STRATUM_CHECK_TRUE(why.find("CURRENT") != std::string::npos);
  STRATUM_CHECK_FALSE(IsSafeToRemove(dir, LooksLikeEmptyOrBundle, &why));
  STRATUM_CHECK_TRUE(why.find("MANIFEST") != std::string::npos);
  STRATUM_CHECK_FALSE(
      IsSafeToRemove(dir, LooksLikeEmptyOrSessionOrBundle, &why));

  // 没有任何结构性依据时 fail-closed
  STRATUM_CHECK_FALSE(IsSafeToRemove(dir, nullptr, &why));
  STRATUM_CHECK_FALSE(why.empty());

  // 文件必须还在（本函数不该碰文件系统）
  STRATUM_CHECK_TRUE(fs::exists(dir + "/keep.txt"));
  std::printf("      [第3层] 含 keep.txt 的普通目录：三种白名单都拒绝，文件仍在\n");
  fs::remove_all(dir, ec);
}

STRATUM_TEST(SafeRemove, StructureWhitelistAcceptsKnownStructures) {
  std::string why;

  // 不存在 → 没有东西可删（创建新目录是安全的）
  const std::string absent = UniqueDir("absent");
  STRATUM_CHECK_TRUE(IsSafeToRemove(absent, LooksLikeEmptyOrSession, &why));

  // 空目录 → 可覆盖
  const std::string empty = UniqueDir("empty");
  std::error_code ec;
  fs::create_directories(empty, ec);
  STRATUM_CHECK_TRUE(IsSafeToRemove(empty, LooksLikeEmptyOrSession, &why));

  // 合法会话（有 CURRENT）→ 可覆盖
  const std::string session = UniqueDir("session");
  fs::create_directories(session, ec);
  { std::ofstream o(session + "/CURRENT"); o << "MANIFEST-000001\n"; }
  STRATUM_CHECK_TRUE(IsSafeToRemove(session, LooksLikeEmptyOrSession, &why));
  STRATUM_CHECK_FALSE(IsSafeToRemove(session, LooksLikeEmptyOrBundle, &why));

  // 合法 bundle（有 MANIFEST.json）→ 可覆盖
  const std::string bundle = UniqueDir("bundle");
  fs::create_directories(bundle, ec);
  { std::ofstream o(bundle + "/MANIFEST.json"); o << "{}\n"; }
  STRATUM_CHECK_TRUE(IsSafeToRemove(bundle, LooksLikeEmptyOrBundle, &why));
  STRATUM_CHECK_TRUE(
      IsSafeToRemove(bundle, LooksLikeEmptyOrSessionOrBundle, &why));

  fs::remove_all(empty, ec);
  fs::remove_all(session, ec);
  fs::remove_all(bundle, ec);
  std::printf("      [白名单正例] 不存在/空目录/会话/bundle 可覆盖；会话≠bundle\n");
}

STRATUM_TEST_MAIN()
