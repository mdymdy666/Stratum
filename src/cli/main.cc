// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/main.cc
//
// stratum CLI 入口：全局参数解析 + 子命令分发 + 日志初始化。
//
// 设计依据：docs/08-cli-spec.md
//   · 全局参数见 1 节
//   · 退出码见 1.2 节
//   · 错误消息三段式见 8.1 节
//
// 本版本交付：session / verify / config / query 四组。
// 其余子命令**明确报"未实现"**（而非静默忽略），见 CmdNotImplemented。
//
// 【stdout 纪律】CLI 的数据出口是 stdout。日志只去文件或 stderr，
// 绝不写 stdout —— 否则 `stratum query ... --format csv > d.csv` 会被污染。
// 这条约束由 InitLogging 实现保证（stdout_enabled 恒为 false）。

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
      "  query         <会话>            查询：列话题 / 按时间范围导出 / 定点状态\n"
      "  verify        <会话>            完整性校验与质量报告\n"
      "  record        <选项>            录制（本版本支持基础选项）\n"
      "  config validate <文件>          校验配置文件\n"
      "  config explain  <文件>          逐条解释配置的实际效果\n"
      "  config lock/unlock/status <文件> 配置文件编辑锁\n"
      "\n"
      "record 的选项:\n"
      "  --dir <目录>                    会话目录（必填）\n"
      "  --domain <n>                    DDS 域号\n"
      "  --duration <时长>               录制时长后自动停止（默认 25s）\n"
      "  --continuous                    持续录制，直到 SIGINT/SIGTERM\n"
      "  --include <话题>  --exclude <话题>  订阅筛选（可重复）\n"
      "  --retain <时长>  --retain-bytes <大小>  保留策略\n"
      "  --on-full drop-newest|drop-oldest|block  队列满时的策略\n"
      "  --durability relaxed|balanced|strict\n"
      "  --advertised-address <ip>       跨网段时**必须**显式指定\n"
      "  --overwrite                     允许清空非空目录重录\n"
      "  --dry-run                       只打印生效配置与后果，不录制\n"
      "\n"
      "query 的选项:\n"
      "  --list-topics                   列出话题（默认行为）\n"
      "  --topic <名字>                  导出该话题的数据\n"
      "  --state-at <时刻>               定点状态（Stratum 的差异化能力）\n"
      "  --all-topics                    配 --state-at：查询全部话题\n"
      "  --since <时刻>  --until <时刻>   半开区间 [since, until)\n"
      "  --limit N  --offset N           分页\n"
      "  --with-payload                  附带 payload 的 hex 前缀\n"
      "  --stream                        流式输出（JSON Lines）\n"
      "  --out <文件>                    写到文件而非 stdout\n"
      "\n"
      "  时刻的三类写法（必须能区分，见设计文档 3.2 节）:\n"
      "    @session+5s                    会话内偏移（会话开始 +5 秒）\n"
      "    -30m / +2h                     相对现在\n"
      "    2026-10-14T10:00:00+08:00      RFC3339 绝对时刻\n"
      "\n"
      "尚未实现（明确报错，不静默）:\n"
      "  play / prune / ctl / io / doctor\n"
      "  设计已完整，见 docs/08-cli-spec.md 与 docs/09-cli-scenarios-deep.md\n"
      "\n"
      "全局参数:\n"
      "  --format table|json|yaml|csv   输出格式（非 TTY 时默认 json）\n"
      "  --quiet, -q                    仅输出结果\n"
      "  --verbose, -v                  可重复；-v 信息 / -vv 调试 / -vvv 追踪\n"
      "  --log-format text|json         日志格式\n"
      "  --log-file <文件>              日志文件（\"-\" 表示 stderr）\n"
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
      "  5 PARTIAL   6 ENV(环境问题，可重试)   7 BUSY   8 NOTFOUND\n"
      "\n"
      "日志位置: $XDG_CACHE_HOME/stratum/stratum.log（默认不写 stdout）\n");
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

    // 【参数解析边界】遇到第一个非选项参数即认定为子命令，
    // **其后所有参数原样移交**，main 不再解释它们。
    // 否则子命令自己的选项（如 query --list-topics）会被误判为"未知全局选项"。
    // 模型：stratum [全局选项] <子命令> [子命令选项...]
    if (a == "--") {
      rest.assign(argv + i + 1, argv + argc);
      break;
    }
    if (!a.empty() && a[0] != '-') {
      rest.assign(argv + i, argv + argc);
      break;
    }

    if (a == "--help" || a == "-h") { want_help = true; continue; }
    if (a == "--version" || a == "-V") { want_version = true; continue; }
    if (a == "--quiet" || a == "-q") { g.quiet = true; continue; }
    if (a == "--verbose" || a == "-v") { ++g.verbose; continue; }

    // 组合短选项：-vv / -vvv / -qv（与 getopt 惯例一致）
    if (a.size() >= 2 && a[0] == '-' && a[1] != '-') {
      bool all_known = true;
      for (size_t k = 1; k < a.size(); ++k) {
        if (a[k] == 'v') ++g.verbose;
        else if (a[k] == 'q') g.quiet = true;
        else if (a[k] == 'h') want_help = true;
        else if (a[k] == 'V') want_version = true;
        else { all_known = false; break; }
      }
      if (all_known) continue;
      // 含未知短选项 -> 当作子命令的参数交给它（不在这里报错）
      rest.assign(argv + i, argv + argc);
      break;
    }
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
        if (v != "text" && v != "json") {
          return Fail(kUsage, "无法识别的日志格式: " + v,
                      "支持 text / json", "例如 --log-format json");
        }
        g.log_format = v;
      } else if (a == "--log-file") {
        g.log_file = v;
      }
      continue;
    }

    if (a.size() >= 2 && a[0] == '-' && a[1] == '-') {
      return Fail(kUsage, "未知全局选项: " + a, "该选项不在规范内",
                  "全局选项必须写在子命令之前；子命令选项请写在子命令之后。"
                  "可用 `stratum --help` 查看支持的全局选项");
    }
    // 短选项（-q/-v/-h/-V）已在上方处理；其余单个 '-' 视为位置参数
    rest.assign(argv + i, argv + argc);
    break;
  }

  // --format 未显式给且非 TTY → json（脚本友好；docs/08 1 节）
  if (!g.format_explicit && !isatty(STDOUT_FILENO)) {
    g.format = Format::kJson;
  }

  if (want_version) { PrintVersion(); return kOk; }
  if (want_help || rest.empty()) { PrintHelp(); return rest.empty() ? kUsage : kOk; }

  // 日志必须在子命令之前初始化（否则子命令里的日志用的是默认配置）
  InitLogging(g);

  const std::string cmd = rest[0];
  const std::vector<std::string> args(rest.begin() + 1, rest.end());

  int rc = kError;
  if (cmd == "session") rc = CmdSession(args, g);
  else if (cmd == "query") rc = CmdQuery(args, g);
  else if (cmd == "verify") rc = CmdVerify(args, g);
  else if (cmd == "config") rc = CmdConfig(args, g);
  else if (cmd == "record") rc = CmdRecord(args, g);
  else if (cmd == "play") rc = CmdPlay(args, g);
  else if (cmd == "prune") rc = CmdPrune(args, g);
  else if (cmd == "types") rc = CmdTypes(args, g);
  else if (cmd == "ctl") rc = CmdCtl(args, g);
  else if (cmd == "io") rc = CmdIo(args, g);
  else if (cmd == "doctor") {
    rc = CmdNotImplemented(cmd);
  } else {
    rc = Fail(kUsage, "未知子命令: " + cmd, "该子命令不在规范内",
              "可用 `stratum --help` 查看；设计见 docs/07-cli-design.md");
  }

  // 日志是异步的，退出前刷盘，避免最后几条丢失
  stratum::log::LoggerManager::Instance().Flush();
  return rc;
}
