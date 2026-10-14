// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/main.cc
//
// stratum CLI 入口：全局参数解析 + 子命令分发。
//
// 设计依据：docs/08-cli-spec.md
//   · 全局参数见 1 节
//   · 退出码见 1.2 节
//   · 错误消息三段式见 8.1 节
//
// 本版本交付：session / verify / config 三组。
// 其余子命令**明确报"未实现"**（而非静默忽略），见 CmdNotImplemented。

#include <unistd.h>   // isatty

#include <cstring>
#include <string>
#include <vector>

#include "cli_common.h"

using stratum::Status;
using namespace stratum::cli;

namespace {

const char* kVersion = "1.0.0";
const char* kCliSpecVersion = "docs/08-cli-spec.md";

void PrintVersion() {
  std::printf("stratum %s\n", kVersion);
  std::printf("CLI 规范: %s\n", kCliSpecVersion);
  std::printf("设计文档: docs/07-cli-design.md（32 场景）\n");
}

void PrintHelp() {
  std::printf(
      "stratum —— 面向 DDS 的可回溯数据平面\n"
      "\n"
      "用法: stratum <子命令> [参数...]\n"
      "\n"
      "已实现的子命令:\n"
      "  session list  <目录>            列出该目录下的所有会话\n"
      "  session info  <会话>            显示会话元数据、话题摘要与质量统计\n"
      "  verify        <会话>            完整性校验与质量报告\n"
      "  config validate <文件>          校验配置文件（防 EC-16/EC-17 误用）\n"
      "  config explain  <文件>          逐条解释配置的实际效果\n"
      "\n"
      "尚未实现（明确报错，不静默）:\n"
      "  record / play / query / prune / ctl / io / doctor\n"
      "  设计已完整，见 docs/08-cli-spec.md 与 docs/09-cli-scenarios-deep.md\n"
      "\n"
      "全局参数:\n"
      "  --format table|json|yaml|csv   输出格式（非 TTY 时默认 json）\n"
      "  --quiet, -q                    仅输出结果\n"
      "  --verbose, -v                  可重复；-v 信息 / -vv 调试\n"
      "  --tz <IANA>                    解析裸时间所必需\n"
      "  --config <文件>                配置文件\n"
      "  --no-config                    忽略配置文件\n"
      "  --dry-run                      预演（危险命令默认开启）\n"
      "  --yes                          跳过确认\n"
      "  --help, -h                     本帮助\n"
      "  --version, -V                  版本\n"
      "\n"
      "退出码:\n"
      "  0 OK   1 ERROR   2 USAGE   3 EXISTS   4 INTEGRITY(数据问题)\n"
      "  5 PARTIAL   6 ENV(环境问题，可重试)   7 BUSY   8 NOTFOUND\n");
}

// 需要参数的全局选项
bool NeedsValue(const std::string& a) {
  return a == "--format" || a == "--tz" || a == "--config" || a == "--socket" ||
         a == "--color" || a == "--log-format" || a == "--log-file";
}

}  // namespace

int main(int argc, char** argv) {
  GlobalOptions g;

  // 审计用：原样保留调用串（docs/08 2.3 节：选择器不做规范化改写）
  for (int i = 0; i < argc; ++i) {
    if (i) g.raw_invocation += ' ';
    g.raw_invocation += argv[i];
  }

  std::vector<std::string> rest;
  bool want_help = false;
  bool want_version = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];

    if (a == "--help" || a == "-h") { want_help = true; continue; }
    if (a == "--version" || a == "-V") { want_version = true; continue; }
    if (a == "--quiet" || a == "-q") { g.quiet = true; continue; }
    if (a == "--verbose" || a == "-v") { ++g.verbose; continue; }
    if (a == "--dry-run") { g.dry_run = true; continue; }
    if (a == "--yes") { g.yes = true; continue; }
    if (a == "--no-config") { g.no_config = true; continue; }

    if (NeedsValue(a)) {
      if (i + 1 >= argc) {
        return Fail(kUsage, "选项缺少参数值: " + a,
                    "该选项需要一个字符串参数", "示例: " + a + " <值>");
      }
      const std::string v = argv[++i];
      if (a == "--format") {
        if (!ParseFormat(v, &g.format)) {
          return Fail(kUsage, "无法识别的输出格式: " + v,
                      "支持 table / json / yaml / csv",
                      "例如 --format json");
        }
        g.format_explicit = true;
      } else if (a == "--tz") {
        g.tz = v;
      } else if (a == "--config") {
        g.config_path = v;
      } else if (a == "--socket") {
        g.socket = v;
      } else if (a == "--color") {
        g.color = v;
      } else if (a == "--log-format") {
        // 本版本仅记录，日志分流在后续实现
      } else if (a == "--log-file") {
        // 同上
      }
      continue;
    }

    if (a.size() >= 2 && a[0] == '-' && a[1] == '-') {
      return Fail(kUsage, "未知选项: " + a, "该选项不在规范内",
                  "可用 `stratum --help` 查看支持的选项");
    }
    rest.push_back(a);
  }

  // --format 未显式给且非 TTY → json（脚本友好；docs/08 1 节）
  if (!g.format_explicit && !isatty(STDOUT_FILENO)) {
    g.format = Format::kJson;
  }

  if (want_version) { PrintVersion(); return kOk; }
  if (want_help || rest.empty()) { PrintHelp(); return rest.empty() ? kUsage : kOk; }

  const std::string cmd = rest[0];
  const std::vector<std::string> args(rest.begin() + 1, rest.end());

  if (cmd == "session") return CmdSession(args, g);
  if (cmd == "verify") return CmdVerify(args, g);
  if (cmd == "config") return CmdConfig(args, g);

  if (cmd == "record" || cmd == "play" || cmd == "query" || cmd == "prune" ||
      cmd == "ctl" || cmd == "io" || cmd == "doctor") {
    return CmdNotImplemented(cmd);
  }

  return Fail(kUsage, "未知子命令: " + cmd, "该子命令不在规范内",
              "可用 `stratum --help` 查看；设计见 docs/07-cli-design.md");
}
