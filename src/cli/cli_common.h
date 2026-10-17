// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cli_common.h
//
// CLI 的公共设施：退出码、输出格式、全局选项、三段式错误、日志、时间解析。
//
// 设计依据：docs/08-cli-spec.md
//   · 退出码 8 级（4 与 6 必须分开 —— 数据问题 vs 环境问题）
//   · --format / --quiet / --verbose / --tz / --dry-run / --yes
//   · 错误消息三段式：症状 → 原因 → 动作
//   · 三类时间必须能区分：绝对 / 相对 / 会话内偏移（NFR-2.5、EC-01）
//
// 这些不是"实现细节"，而是**契约**：CI 依赖退出码，脚本依赖输出格式，
// 运维依赖错误消息里的"动作"能直接照做。

#ifndef STRATUM_CLI_COMMON_H_
#define STRATUM_CLI_COMMON_H_

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "stratum/core/status.h"
#include "stratum/core/types.h"
#include "stratum/log.hpp"

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

  // 日志（移植自 Go2Cpp 的 logging 模块）
  //   log_file 为空  -> 默认写 <cache>/stratum/stratum.log（**不碰 stdout**）
  //   log_file == "-" -> 写 stderr（**仍不碰 stdout**）
  std::string log_format = "text";   // text | json
  std::string log_file;

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
// 日志（接入移植的 logging 模块）
//
// 【关键约束】CLI 的**数据出口是 stdout**。日志绝不能写 stdout，
// 否则 `stratum query ... --format csv > data.csv` 会被日志污染。
// 因此本 CLI 只把日志送往：文件（默认）或 stderr（--log-file -）。
// ---------------------------------------------------------------------------
// 在子命令执行前调用一次；无副作用地重复调用也安全。
void InitLogging(const GlobalOptions& g);

// CLI 自己的日志句柄（惰性创建）
log::Logger::ptr CliLogger();

// 记一条 CLI 级日志（对应用户的 --verbose 意图）
void LogVerbose(const GlobalOptions& g, int min_level, const std::string& msg);

// ---------------------------------------------------------------------------
// 解析助手（query / record 等命令共用）
// ---------------------------------------------------------------------------
// "24h" / "30m" / "500ms" / "1d" / "2w" -> 纳秒
bool ParseDurationNs(const std::string& s, uint64_t* out);
// "10GB" / "512MB" / "1KB" / "1024"（无单位按字节）
// 十进制 KB/MB/GB，二进制 KiB/MiB/GiB
bool ParseSizeBytes(const std::string& s, uint64_t* out);

// 三类时间值（docs/08 3.2 节）：
//   @session+5s / @session-1s   会话内偏移（基准 = 会话 created_at_ns）
//   -30m / +2h                  相对现在（wall clock）
//   2026-10-14T10:00:00+08:00   RFC3339 绝对时刻
//   1700000000000000000          纯数字按 Unix 纳秒
// 解析失败时填充 err 并返回 false（**不猜**，见 R1 无歧义原则）。
bool ParseTimeValue(const std::string& s, uint64_t session_created_ns,
                    uint64_t* out, std::string* err);

// 会话目录是否已存在。
//
// 【为什么需要】TimelineImplV2::Open() 对不存在的路径会**创建**会话
// （LSM 引擎的正常行为，录制时正需要如此）。但**查询类命令不应有创建副作用** ——
// 否则 `stratum query /typo/path` 会静默返回空结果并成功退出，
// 使用者会以为"这个会话是空的"，而不是"路径打错了"。
bool SessionExists(const std::string& path);

// 把纳秒时间戳格式化成 RFC3339（本地时区），用于输出
std::string FormatRfc3339(uint64_t ns);

// ---------------------------------------------------------------------------
// 输出助手
// ---------------------------------------------------------------------------
std::string JsonEscape(const std::string& s);

std::string HumanBytes(uint64_t n);
std::string HumanCount(uint64_t n);

// 纳秒时间戳 -> 可读字符串（相对 + 绝对）
std::string FormatTimestampNs(uint64_t ns);
// 会话内相对偏移（如 "+12.345s"）
std::string FormatOffsetNs(uint64_t base_ns, uint64_t t_ns);

void PrintTableHeader(const std::vector<std::pair<std::string, int>>& cols,
                      const GlobalOptions& g);

// ---------------------------------------------------------------------------
// 子命令
// ---------------------------------------------------------------------------
int CmdSession(const std::vector<std::string>& args, GlobalOptions g);
int CmdVerify(const std::vector<std::string>& args, GlobalOptions g);
int CmdConfig(const std::vector<std::string>& args, GlobalOptions g);
int CmdQuery(const std::vector<std::string>& args, GlobalOptions g);
int CmdRecord(const std::vector<std::string>& args, GlobalOptions g);
int CmdPlay(const std::vector<std::string>& args, GlobalOptions g);
int CmdPrune(const std::vector<std::string>& args, GlobalOptions g);
int CmdTypes(const std::vector<std::string>& args, GlobalOptions g);
int CmdCtl(const std::vector<std::string>& args, GlobalOptions g);
int CmdIo(const std::vector<std::string>& args, GlobalOptions g);
int CmdDoctor(const std::vector<std::string>& args, GlobalOptions g);
int CmdMetrics(const std::vector<std::string>& args, GlobalOptions g);
int CmdProbe(const std::vector<std::string>& args, GlobalOptions g);
int CmdProbeStatus(const std::vector<std::string>& args, GlobalOptions g);
int CmdDiff(const std::vector<std::string>& args, GlobalOptions g);

// 尚未实现的子命令（明确报错，不静默）
int CmdNotImplemented(const std::string& name);

}  // namespace cli
}  // namespace stratum

#endif  // STRATUM_CLI_COMMON_H_
