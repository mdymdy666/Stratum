// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cli_common.h
//
// CLI 的公共设施：退出码、输出格式、全局选项、三段式错误。
//
// 设计依据：docs/08-cli-spec.md
//   · 退出码 8 级（4 与 6 必须分开 —— 数据问题 vs 环境问题）
//   · --format / --quiet / --verbose / --tz / --dry-run / --yes
//   · 错误消息三段式：症状 → 原因 → 动作
//
// 这些不是"实现细节"，而是**契约**：CI 依赖退出码，脚本依赖输出格式，
// 运维依赖错误消息里的"动作"能直接照做。

#ifndef STRATUM_CLI_COMMON_H_
#define STRATUM_CLI_COMMON_H_

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "stratum/core/status.h"

namespace stratum {
namespace cli {

// ---------------------------------------------------------------------------
// 退出码（与 docs/08-cli-spec.md 1.2 节逐条对应）
// ---------------------------------------------------------------------------
enum ExitCode : int {
  kOk = 0,         // 成功
  kError = 1,      // 一般错误
  kUsage = 2,      // 参数/语法错误
  kExists = 3,     // 目标已存在（record 拒绝写非空目录）
  kIntegrity = 4,  // 完整性校验失败 / 性能门禁未达   ← 数据问题
  kPartial = 5,    // 部分成功（有数据但有缺口/丢弃）
  kEnv = 6,        // 环境不满足（权限、rmem_max 过小） ← 环境问题，可重试
  kBusy = 7,       // 被锁拒绝 / 另一实例在运行
  kNotFound = 8,   // 会话/话题/文件不存在
};

const char* ExitCodeName(ExitCode c);

// ---------------------------------------------------------------------------
// 输出格式
// ---------------------------------------------------------------------------
enum class Format { kTable, kJson, kYaml, kCsv };

bool ParseFormat(const std::string& s, Format* out);
const char* FormatName(Format f);

// ---------------------------------------------------------------------------
// 全局选项（所有子命令共有）
// ---------------------------------------------------------------------------
struct GlobalOptions {
  Format format = Format::kTable;
  bool format_explicit = false;   // 用户是否显式给了 --format
  bool quiet = false;
  int verbose = 0;
  std::string tz;                 // 空 = 用系统时区；解析裸时间时必需
  std::string config_path;        // 空 = 默认路径
  bool no_config = false;
  std::string socket;

  // 危险操作的默认值由**子命令**决定（见 08 的 1.1 节）：
  //   prune/delete/repair 默认 dry-run=true，其他默认 false
  bool dry_run = false;
  bool yes = false;
  std::string color = "auto";

  // 用户原始命令（含选择器原样），用于审计（docs/08 2.3 节）
  std::string raw_invocation;

  // 是否输出人类可读的表头（--quiet 时关闭）
  bool ShowHeaders() const { return !quiet; }
};

// ---------------------------------------------------------------------------
// 三段式错误输出（docs/08 8.1 节）
//
// 【为什么是三段】项目里吃过亏：rmem_max 被截断时工具只说
// "请求 8388608 实际生效 425984"，没说"所以会怎样、该怎么办"，
// 导致排查花了很多时间。因此错误必须包含**动作**。
// ---------------------------------------------------------------------------
int Fail(ExitCode code, const std::string& symptom,
         const std::string& cause = "", const std::string& action = "");

// 把 Status 转成合适的退出码
ExitCode ExitCodeOf(const Status& s);

// 打印 Status（带三段式上下文）
int FailStatus(ExitCode code, const std::string& what, const Status& s);

// ---------------------------------------------------------------------------
// 输出助手
// ---------------------------------------------------------------------------
std::string JsonEscape(const std::string& s);

// 人类可读的字节数 / 条数
std::string HumanBytes(uint64_t n);
std::string HumanCount(uint64_t n);

// 纳秒时间戳 -> 可读字符串（相对 + 绝对）
std::string FormatTimestampNs(uint64_t ns);
// 会话内相对偏移（如 "+12.345s"）
std::string FormatOffsetNs(uint64_t base_ns, uint64_t t_ns);

// 简单的表头输出（table 模式）
void PrintTableHeader(const std::vector<std::pair<std::string, int>>& cols,
                      const GlobalOptions& g);

// ---------------------------------------------------------------------------
// 子命令
// ---------------------------------------------------------------------------
int CmdSession(const std::vector<std::string>& args, GlobalOptions g);
int CmdVerify(const std::vector<std::string>& args, GlobalOptions g);
int CmdConfig(const std::vector<std::string>& args, GlobalOptions g);

// 尚未实现的子命令（明确报错，不静默）
int CmdNotImplemented(const std::string& name);

}  // namespace cli
}  // namespace stratum

#endif  // STRATUM_CLI_COMMON_H_
