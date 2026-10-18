// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/safe_path.h
//
// 删除类操作的共享守卫（header-only，便于 CLI 与测试共用）。
//
// 【为什么需要】
// 本项目的 `--overwrite` / `--force` 路径会调用 `fs::remove_all(用户路径)`：
//   · io export / io import 的 --to
//   · record 的 --dir（--overwrite 时清空目录）
// 用户手滑把目标写成 `/`、`.`、`..`、`$HOME`，或写成规范化后等价于它们的
// 形式（`//`、`/./`、`/data/../..`、指向 `/` 的软链接）时，删除不可恢复。
//
// 【为什么是"黑名单 + 结构白名单"，而不是纯黑名单】
// 纯黑名单的问题是"想漏一条就出洞"：merge 的 `--to /` 漏洞正是因为
// 祖先检查漏了根目录这一条。纯用层数当判据也与危险程度无关：
// `$HOME` 很深却最危险，`/merged` 很浅却很可能合法。
//
// 所以本守卫分四层，越靠内越不依赖前面的正确性：
//   1. 字符串层：非空、非 "."、非 ".."
//   2. 规范化层：weakly_canonical（解析软链接、归一化 . 与 ..、合并 //）
//      之后不等于根目录、不等于 $HOME、不是当前工作目录及其祖先
//   3. ★ 结构白名单：调用方声明"我要覆盖的是什么结构"，守卫验证它确实是
//      那种东西（空目录 / 合法会话 / 合法 bundle）。不是认识的结构就不删。
//   4. 默认拒绝：调用方没提供结构判定 → 直接拒绝（fail-closed）。
//
// 第 3 层是关键：即使第 1/2 层有 bug（大小写折叠、软链接、规范化差异），
// "不是会话/不是 bundle/非空"的普通目录仍然删不掉。
//
// 【为什么不能用字符串前缀比较】
// `path.rfind(target + "/", 0) == 0` 这类写法在 `//`、`/./`、`/data/../etc`、
// 软链接、大小写不敏感文件系统上都会错。这里用 `weakly_canonical` 后的
// **路径组件**比较（见 IsPathPrefixOf）。
//
// 【调用约定】
// 无条件调用（不要只在 --overwrite 时才调）。这样即使守卫将来被改坏，
// 未给 --overwrite 的情形也只会落到 EXISTS 分支，损失上限是"报错退出"。
#ifndef STRATUM_CLI_SAFE_PATH_H_
#define STRATUM_CLI_SAFE_PATH_H_

#include <cstdlib>
#include <filesystem>
#include <string>

namespace stratum {
namespace cli {

// 结构白名单回调：返回 true 表示"这个路径确实是我预期要覆盖的结构"。
// why_not 非空时写入人类可读原因。
using PathStructureCheck = bool (*)(const std::string& path,
                                    std::string* why_not);

// prefix 是否为 p 的**路径组件**前缀（目录包含关系）。
// 用组件迭代器比较，避免 "/data2" 被误判为 "/data" 的子路径。
inline bool IsPathPrefixOf(const std::filesystem::path& prefix,
                           const std::filesystem::path& p) {
  auto pi = prefix.begin();
  auto pj = p.begin();
  for (; pi != prefix.end(); ++pi, ++pj) {
    if (pj == p.end() || *pi != *pj) {
      return false;
    }
  }
  return true;
}

// 规范化：解析软链接 + 归一化 `.` / `..` / 重复斜杠。
// weakly_canonical 不要求路径尾部存在；失败时退回 absolute().lexically_normal()。
inline std::filesystem::path NormalizeForSafety(const std::string& path) {
  std::error_code ec;
  const std::filesystem::path abs = std::filesystem::absolute(path, ec);
  if (ec) {
    return std::filesystem::path(path).lexically_normal();
  }
  std::filesystem::path canon = std::filesystem::weakly_canonical(abs, ec);
  if (ec) {
    canon = abs.lexically_normal();
  }
  return canon;
}

// ---------------------------------------------------------------------------
// 结构白名单（各命令按自己"认识的结构"选用）
// ---------------------------------------------------------------------------

// 空目录，或合法 bundle（含 MANIFEST.json）—— io export 的目标
inline bool LooksLikeEmptyOrBundle(const std::string& path,
                                   std::string* why_not) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return true;
  if (ec) {
    if (why_not != nullptr) *why_not = "无法访问目标：" + ec.message();
    return false;
  }
  if (!std::filesystem::is_directory(path, ec) || ec) {
    if (why_not != nullptr) *why_not = "目标存在但不是目录";
    return false;
  }
  if (std::filesystem::is_empty(path, ec) && !ec) return true;
  if (std::filesystem::exists(std::filesystem::path(path) / "MANIFEST.json",
                              ec) &&
      !ec) {
    return true;   // 是一个已存在的 bundle：允许重新导出覆盖
  }
  if (why_not != nullptr) {
    *why_not =
        "目标非空且不是合法 bundle（缺少 MANIFEST.json）；拒绝覆盖未知目录";
  }
  return false;
}

// 空目录，或合法会话（LSM 会话目录含 CURRENT）—— record 的目标
inline bool LooksLikeEmptyOrSession(const std::string& path,
                                    std::string* why_not) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return true;
  if (ec) {
    if (why_not != nullptr) *why_not = "无法访问目标：" + ec.message();
    return false;
  }
  if (!std::filesystem::is_directory(path, ec) || ec) {
    if (why_not != nullptr) *why_not = "目标存在但不是目录";
    return false;
  }
  if (std::filesystem::is_empty(path, ec) && !ec) return true;
  if (std::filesystem::exists(std::filesystem::path(path) / "CURRENT", ec) &&
      !ec) {
    return true;   // 是一个已存在的会话：允许 --overwrite 重录
  }
  if (why_not != nullptr) {
    *why_not = "目标非空且不是合法会话（缺少 CURRENT）；拒绝清空未知目录";
  }
  return false;
}

// 空目录、合法会话或合法 bundle —— io import 的目标（写的是会话，
// 但也允许覆盖一个旧 bundle 目录，保持与 export 的对称）
inline bool LooksLikeEmptyOrSessionOrBundle(const std::string& path,
                                            std::string* why_not) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return true;
  if (!std::filesystem::is_directory(path, ec) || ec) {
    if (why_not != nullptr) *why_not = "目标存在但不是目录";
    return false;
  }
  if (LooksLikeEmptyOrSession(path, nullptr)) return true;
  if (LooksLikeEmptyOrBundle(path, nullptr)) return true;
  if (why_not != nullptr) {
    *why_not =
        "目标非空且既不是合法会话（无 CURRENT）也不是合法 bundle（无 MANIFEST.json）";
  }
  return false;
}

// ---------------------------------------------------------------------------
// 主守卫
// ---------------------------------------------------------------------------

// 返回 false 表示这个路径**不能**作为"可删除/可覆盖"的目标。
// reason 非空时写入人类可读原因（用于 CLI 三段式错误消息）。
inline bool IsSafeToRemove(const std::string& path,
                           PathStructureCheck looks_like_expected,
                           std::string* reason) {
  auto reject = [reason](const std::string& why) {
    if (reason != nullptr) *reason = why;
    return false;
  };

  // 第 1 层：字符串
  if (path.empty()) {
    return reject("路径为空");
  }
  if (path == "." || path == "..") {
    return reject("路径是 \"" + path + "\"");
  }

  // 第 2 层：规范化后比较（不是字符串前缀比较）
  const std::filesystem::path canon = NormalizeForSafety(path);
  const std::string shown = canon.string();
  if (canon.empty() || canon == canon.root_path()) {
    return reject("规范化后是根目录 " + shown);
  }
  if (const char* home = std::getenv("HOME");
      home != nullptr && home[0] != '\0') {
    if (canon == NormalizeForSafety(home)) {
      return reject("是 $HOME 本身（" + shown + "）");
    }
  }
  {
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec && IsPathPrefixOf(canon, NormalizeForSafety(cwd.string()))) {
      return reject("是当前工作目录或其祖先（" + shown + "）");
    }
  }

  // 第 3 层：结构白名单（不依赖第 1/2 层是否正确）
  if (looks_like_expected == nullptr) {
    return reject("调用方未提供结构白名单判定（fail-closed，拒绝删除）");
  }
  std::string why_not;
  if (!looks_like_expected(shown, &why_not)) {
    return reject(why_not.empty() ? ("目标结构不符合预期：" + shown) : why_not);
  }

  return true;
}

}  // namespace cli
}  // namespace stratum

#endif  // STRATUM_CLI_SAFE_PATH_H_
