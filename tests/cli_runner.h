// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// tests/cli_runner.h —— CLI 测试的**专用调用入口**
//
// ---------------------------------------------------------------------------
// 为什么需要这个文件
// ---------------------------------------------------------------------------
//
// 我在同一个坑上栽了 **5 次**：
//
//     测试里断言输出含"完整"、"会话对比"、"事件"这类**人类可读文本**
//     但 `RunCli("verify ...")` 没有传 `--format table`
//     → 全局 `--format` 默认是 json
//     → 输出是 JSON，里面没有那些词
//     → 断言失败，而我第一反应总是"功能坏了"
//
// 我把这条写进了纪律、写进了 docs/14、写进了每轮的 prompt —— **还是照踩**。
//
// 结论：**"写在纪律里"防不住错误，要让错误在命名上就不自然。**
//
// 所以提供两个入口：
//
//     RunCliTable(cmd)  → 强制加 `--format table`，返回人类可读输出
//     RunCliJson(cmd)   → 强制加 `--format json`，返回 JSON 输出
//
// 名字里带格式，**想在 Table 版本里断言 JSON 就先得停下来想一想**。
//
// ---------------------------------------------------------------------------
// 使用约定
// ---------------------------------------------------------------------------
//
//   · 断言中文/表格文本   → RunCliTable
//   · 解析 JSON 字段      → RunCliJson
//   · 要检查 stdout/stderr 分离 → RunCliSplit（见下）
//   · **不要再用裸 RunCli** —— 那正是这个文件要消灭的东西
//
// 注意：`--format` 是**全局选项**，必须放在子命令**之前**。
// 本文件的实现已经处理了这一点（插入到第一个 " <sub>" 之前）。

#ifndef STRATUM_TESTS_CLI_RUNNER_H_
#define STRATUM_TESTS_CLI_RUNNER_H_

#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace stratum_cli_test {

struct CliResult {
  int code = -1;
  std::string output;
};

// 把 `--format <fmt>` 插到命令行里"子命令之前"。
//
// 【为什么需要这个函数】`--format` 是全局选项，必须写在子命令之前：
//     stratum --format table verify /data/s      ✓
//     stratum verify /data/s --format table      ✗（会被当成 verify 的未知选项）
//
// 调用方传进来的是 `stratum verify /data/s` 这种形状，
// 我们找到第一个"看起来是子命令"的位置插进去。
inline std::string InsertFormat(const std::string& cmd, const char* fmt) {
  // 找到可执行文件之后的第一个空格，再把 --format 插到下一个 token 之前
  const size_t first_space = cmd.find(' ');
  if (first_space == std::string::npos) return cmd;
  // 跳过连续空格
  size_t sub = first_space;
  while (sub < cmd.size() && cmd[sub] == ' ') ++sub;
  if (sub >= cmd.size()) return cmd;
  // 若已经是 --format，说明调用方自己传了 —— 尊重它，不再插入
  if (cmd.compare(sub, std::string("--format").size(), "--format") == 0) return cmd;
  return cmd.substr(0, sub) + std::string("--format ") + fmt + " " + cmd.substr(sub);
}

// 执行并合并 stdout/stderr（用于"只看结论"的断言）
inline CliResult RunCapture(const std::string& cmd) {
  CliResult r;
  std::FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
  if (p == nullptr) return r;
  char buf[8192];
  while (std::fgets(buf, sizeof(buf), p) != nullptr) r.output += buf;
  const int st = ::pclose(p);
  r.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return r;
}

// 人类可读输出（强制 --format table）
inline CliResult RunCliTable(const std::string& cmd) {
  return RunCapture(InsertFormat(cmd, "table"));
}

// JSON 输出（强制 --format json）
inline CliResult RunCliJson(const std::string& cmd) {
  return RunCapture(InsertFormat(cmd, "json"));
}

// ---------------------------------------------------------------------------
// stdout / stderr 分离版本
//
// 【为什么需要】验证"stdout 是数据出口"这条纪律时，合并捕获会掩盖问题：
// stderr 的警告混进 stdout，JSON 就不完整了 —— 而这正是要抓的东西。
// ---------------------------------------------------------------------------
struct SplitResult {
  int code = -1;
  std::string out;
  std::string err;
};

inline SplitResult RunSplit(const std::string& cmd) {
  SplitResult r;
  const std::string tmp_out = "/tmp/_cli_runner_out.txt";
  const std::string tmp_err = "/tmp/_cli_runner_err.txt";
  const std::string full = cmd + " > " + tmp_out + " 2> " + tmp_err;
  const int rc = std::system(full.c_str());
  r.code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

  auto ReadAll = [](const std::string& path) -> std::string {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return {};
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
  };
  r.out = ReadAll(tmp_out);
  r.err = ReadAll(tmp_err);
  std::remove(tmp_out.c_str());
  std::remove(tmp_err.c_str());
  return r;
}

inline SplitResult RunSplitTable(const std::string& cmd) {
  return RunSplit(InsertFormat(cmd, "table"));
}

inline SplitResult RunSplitJson(const std::string& cmd) {
  return RunSplit(InsertFormat(cmd, "json"));
}

// 从 JSON 里取整数字段（限定在 `"side":{ ... }` 段内，避免同名 key 歧义）
inline long long PickInSide(const std::string& text, const std::string& side,
                            const std::string& field) {
  const std::string pat = "\"" + side + "\":{";
  const size_t at = text.find(pat);
  if (at == std::string::npos) return -1;
  const size_t end = text.find("}", at);
  if (end == std::string::npos) return -1;
  const std::string seg = text.substr(at, end - at);
  const std::string fpat = "\"" + field + "\":";
  const size_t fp = seg.find(fpat);
  if (fp == std::string::npos) return -1;
  return std::strtoll(seg.c_str() + fp + fpat.size(), nullptr, 10);
}

}  // namespace stratum_cli_test

#endif  // STRATUM_TESTS_CLI_RUNNER_H_
