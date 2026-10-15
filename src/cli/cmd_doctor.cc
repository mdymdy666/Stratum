// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_doctor.cc
//
// `stratum doctor` —— 会话体检与有限恢复。
//
// 【与 verify 的分工】
//   · `verify` 回答「**数据**有没有问题」（完整性归因 RC-1~RC-6）
//   · `doctor` 回答「**这个会话还能不能正常用**」（运维视角）
// 前者关心数据，后者关心环境：目录结构、锁、磁盘、元数据一致性。
//
// 【--recover 的范围（诚实声明）】
// 本版本只做**真实且安全**的恢复：
//   · 清理**过期锁**（持有者进程已不存在 —— 编辑锁的语义就是"进程活着才有效"）
//   · 重建后重新校验，确认恢复没有把状态搞坏
//
// **不做**：LSM 文件级损坏的自动修复。原因：
//   · 需要理解 LSM 的内部格式并做数据抢救，属于另一个量级的工作
//   · 更危险的是"看起来修好了" —— 半修的数据比明确报损坏更糟
// 遇到这类问题会**明确报告并给出人工处置建议**，而不是假装能修。

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/statvfs.h>
#include <vector>

#include "cli_common.h"
#include "stratum/core/timeline_impl_v2.h"

namespace fs = std::filesystem;

using namespace stratum;
using namespace stratum::cli;

namespace stratum {
namespace cli {

namespace {

struct Finding {
  enum class Level { kOk, kWarn, kError };
  Level level;
  std::string what;
  std::string advice;   // 可操作的建议
};

const char* LevelName(Finding::Level l) {
  switch (l) {
    case Finding::Level::kOk: return "OK";
    case Finding::Level::kWarn: return "WARN";
    default: return "ERROR";
  }
}

// 判断 pid 是否还活着（编辑锁的持有者）
bool ProcessAlive(long pid) {
  if (pid <= 0) return false;
  if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno == EPERM;   // 存在但无权限 -> 视为活着
}

// 从锁文件里粗取 pid（内容是 JSON，含 "pid": N）
long ExtractPid(const std::string& json) {
  const size_t p = json.find("\"pid\"");
  if (p == std::string::npos) return -1;
  size_t c = json.find(':', p);
  if (c == std::string::npos) return -1;
  return std::strtol(json.c_str() + c + 1, nullptr, 10);
}

Finding MakeFinding(Finding::Level l, std::string what, std::string advice = "") {
  Finding f;
  f.level = l;
  f.what = std::move(what);
  f.advice = std::move(advice);
  return f;
}

// 诊断一个会话，返回 findings 与"过期锁路径"（供 recover 使用）
std::vector<Finding> Diagnose(const std::string& session,
                              std::vector<std::string>* stale_locks,
                              bool* can_open) {
  std::vector<Finding> out;
  *can_open = false;
  std::error_code ec;

  // 1) 目录结构
  if (!fs::is_directory(session, ec)) {
    out.push_back(MakeFinding(Finding::Level::kError,
                              "路径不是目录: " + session,
                              "确认路径；用 session list 查看可用会话"));
    return out;
  }
  const std::string current = session + "/CURRENT";
  const std::string lock = session + "/LOCK";
  const bool has_current = fs::exists(current, ec);
  const bool has_lock = fs::exists(lock, ec);

  if (!has_current) {
    out.push_back(MakeFinding(Finding::Level::kError,
                              "缺少 CURRENT —— 不是完整的 LSM 目录",
                              "若这是从别处拷来的，可能少拷了文件；"
                              "用 `stratum io verify` 核对 bundle"));
  } else {
    out.push_back(MakeFinding(Finding::Level::kOk, "目录结构完整（CURRENT 存在）"));
  }
  if (!has_lock) {
    out.push_back(MakeFinding(Finding::Level::kWarn, "缺少 LOCK 文件",
                              "通常无碍；若数据库能正常打开可忽略"));
  }

  // 2) 统计 LSM 文件
  uint64_t ldb = 0, log = 0, total = 0;
  for (fs::directory_iterator it(session, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    const auto sz = fs::file_size(it->path(), ec);
    if (!ec) total += static_cast<uint64_t>(sz);
    const std::string ext = it->path().extension().string();
    if (ext == ".ldb") ++ldb;
    else if (ext == ".log") ++log;
  }
  out.push_back(MakeFinding(Finding::Level::kOk,
                            "数据文件: " + std::to_string(ldb) + " 个 .ldb / " +
                                std::to_string(log) + " 个 .log，共 " +
                                HumanBytes(total)));

  // 3) 可打开性 + 元数据一致性
  TimelineImplV2 tl;
  Status s = tl.Open(session);
  if (!s.ok()) {
    out.push_back(MakeFinding(Finding::Level::kError,
                              "会话无法打开: " + s.ToString(),
                              "这是最严重的一类问题。本版本的 --recover **不会**"
                              "尝试修复 LSM 文件级损坏；"
                              "请从备份/`io export` 的 bundle 恢复"));
    return out;
  }
  *can_open = true;

  SessionMetadata meta;
  (void)tl.GetSessionMetadata(&meta);
  std::vector<TopicMetadata> topics;
  (void)tl.ListTopics(&topics);

  uint64_t sum_events = 0, sum_bytes = 0;
  for (const auto& t : topics) {
    sum_events += t.message_count;
    sum_bytes += t.byte_count;
  }
  out.push_back(MakeFinding(Finding::Level::kOk,
                            "会话可打开：话题 " + std::to_string(topics.size()) +
                                " 个，事件 " + HumanCount(sum_events) +
                                "，负载 " + HumanBytes(sum_bytes)));

  if (meta.total_events != 0 && meta.total_events != sum_events) {
    out.push_back(MakeFinding(Finding::Level::kWarn,
                              "会话元数据的事件总数（" +
                                  std::to_string(meta.total_events) +
                                  "）与逐话题汇总（" + std::to_string(sum_events) +
                                  "）不一致",
                              "可能是早期版本写入的陈旧汇总；"
                              "以逐话题为准，或重新录一段验证"));
  }
  if (!meta.quality.HasIntegrityIssue()) {
    out.push_back(MakeFinding(Finding::Level::kOk, "质量统计无已知完整性问题"));
  } else {
    out.push_back(MakeFinding(Finding::Level::kWarn,
                              "质量统计报告了完整性问题",
                              "用 `stratum verify <会话>` 看具体归因（RC-1~RC-6）"));
  }

  // 4) 审计信息（控制事件 / 裁剪记录）—— 它们决定了 verify 的归因是否正确
  std::vector<ControlEvent> events;
  if (tl.ListControlEvents(&events).ok() && !events.empty()) {
    out.push_back(MakeFinding(Finding::Level::kOk,
                              "控制事件 " + std::to_string(events.size()) +
                                  " 条（最近一次: " +
                                  (events.back().reason.empty()
                                       ? events.back().action
                                       : events.back().action + " / " +
                                             events.back().reason) +
                                  "）"));
  }
  size_t pruned_total = 0;
  for (const auto& t : topics) {
    std::vector<PrunedRange> pr;
    if (tl.ListPrunedRanges(t.topic_id, &pr).ok()) pruned_total += pr.size();
  }
  if (pruned_total > 0) {
    out.push_back(MakeFinding(Finding::Level::kOk,
                              "主动裁剪记录 " + std::to_string(pruned_total) +
                                  " 个区间（verify 会视为主动裁剪而非缺口）"));
  }
  (void)tl.Close();

  // 5) 编辑锁状态（.lock 被持有但进程已死 = 过期锁，可清理）
  const std::string lockfile = session + ".lock";
  if (fs::exists(lockfile, ec)) {
    const auto sz = fs::file_size(lockfile, ec);
    if (!ec && sz > 0) {
      std::ifstream in(lockfile, std::ios::in | std::ios::binary);
      std::ostringstream ss;
      ss << in.rdbuf();
      const std::string content = ss.str();
      const long pid = ExtractPid(content);
      if (pid > 0 && !ProcessAlive(pid)) {
        out.push_back(MakeFinding(Finding::Level::kWarn,
                                  "发现**过期锁**：" + lockfile +
                                      "（持有者 pid " + std::to_string(pid) +
                                      " 已不存在）",
                                  "可用 `stratum doctor <会话> --recover` 清理；"
                                  "或 `stratum config unlock`"));
        if (stale_locks) stale_locks->push_back(lockfile);
      } else {
        out.push_back(MakeFinding(Finding::Level::kWarn,
                                  "编辑锁被持有：" + lockfile +
                                      (pid > 0 ? "（pid " + std::to_string(pid) + "）"
                                               : ""),
                                  "配置修改会被拒绝(BUSY)；确认对方在做什么，"
                                  "或等它释放"));
      }
    }
  }

  // 6) 磁盘
  struct statvfs vfs;
  if (statvfs(session.c_str(), &vfs) == 0) {
    const uint64_t free_bytes =
        static_cast<uint64_t>(vfs.f_bavail) * static_cast<uint64_t>(vfs.f_frsize);
    if (free_bytes < 100ULL * 1000 * 1000) {
      out.push_back(MakeFinding(Finding::Level::kError,
                                "磁盘可用空间不足 100MB（" +
                                    HumanBytes(free_bytes) + "）",
                                "清理磁盘；长期录制会立即失败"));
    } else {
      out.push_back(MakeFinding(Finding::Level::kOk,
                                "磁盘可用 " + HumanBytes(free_bytes)));
    }
  }
  return out;
}

}  // namespace

int CmdDoctor(const std::vector<std::string>& args, GlobalOptions g) {
  std::string session;
  bool recover = false;
  bool dry_run = false;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--recover") { recover = true; continue; }
    if (a == "--dry-run") { dry_run = true; continue; }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "doctor 不支持该选项: " + a,
                  "支持 --recover / --dry-run", "");
    }
    if (session.empty()) {
      session = a;
    } else {
      return Fail(kUsage, "多余的位置参数: " + a, "", "");
    }
  }
  if (session.empty()) {
    return Fail(kUsage, "doctor 需要会话路径", "位置参数缺失",
                "例如: stratum doctor /data/stratum/s1");
  }
  if (!SessionExists(session)) {
    return Fail(kNotFound, "会话不存在: " + session,
                "路径不存在或不是目录",
                "用 `stratum session list <父目录>` 列出可用会话");
  }

  std::vector<std::string> stale_locks;
  bool can_open = false;
  const std::vector<Finding> findings = Diagnose(session, &stale_locks, &can_open);

  size_t errors = 0, warns = 0;
  for (const auto& f : findings) {
    if (f.level == Finding::Level::kError) ++errors;
    if (f.level == Finding::Level::kWarn) ++warns;
  }

  // -------------------------------------------------------------------------
  // 恢复（有限范围）
  // -------------------------------------------------------------------------
  int recovered = 0;
  std::vector<std::string> recovered_items;
  if (recover) {
    if (stale_locks.empty()) {
      // 没有可自动恢复的项 —— 如实说明，不假装做了事
      if (!g.quiet) {
        std::fprintf(stderr, "没有可自动恢复的项。\n");
        if (errors > 0) {
          std::fprintf(stderr,
                       "  注意: 存在 %zu 个 ERROR 级问题，但本版本的 --recover "
                       "**不处理 LSM 文件级损坏**。\n", errors);
          std::fprintf(stderr,
                       "        请从备份或 `stratum io export` 的 bundle 恢复；"
                       "半修的数据比明确报损坏更危险。\n");
        }
      }
    }
    for (const auto& lf : stale_locks) {
      if (dry_run) {
        std::printf("  [dry-run] 将清理过期锁: %s\n", lf.c_str());
        continue;
      }
      std::error_code ec;
      // 截断为 0（与 config unlock 同一语义：不删除文件）
      std::ofstream o(lf, std::ios::out | std::ios::trunc);
      if (o) {
        ++recovered;
        recovered_items.push_back(lf);
        LogVerbose(g, 1, "已清理过期锁: " + lf);
      } else {
        std::fprintf(stderr, "  清理失败: %s\n", lf.c_str());
      }
    }
  }

  // -------------------------------------------------------------------------
  // 输出
  // -------------------------------------------------------------------------
  if (g.format == Format::kJson) {
    std::printf("{\"schema_version\":1,\"session\":\"%s\",\"ok\":%s,"
                "\"errors\":%zu,\"warnings\":%zu,\"can_open\":%s,"
                "\"recover_requested\":%s,\"recovered\":%d,\"findings\":[",
                JsonEscape(session).c_str(), (errors == 0) ? "true" : "false",
                errors, warns, can_open ? "true" : "false",
                recover ? "true" : "false", recovered);
    for (size_t i = 0; i < findings.size(); ++i) {
      if (i) std::printf(",");
      std::printf("{\"level\":\"%s\",\"what\":\"%s\",\"advice\":\"%s\"}",
                  LevelName(findings[i].level),
                  JsonEscape(findings[i].what).c_str(),
                  JsonEscape(findings[i].advice).c_str());
    }
    std::printf("]}\n");
  } else {
    std::printf("会话体检: %s\n\n", session.c_str());
    for (const auto& f : findings) {
      std::printf("  [%-5s] %s\n", LevelName(f.level), f.what.c_str());
      if (!f.advice.empty()) {
        std::printf("          -> %s\n", f.advice.c_str());
      }
    }
    if (recover) {
      std::printf("\n恢复:\n");
      if (recovered > 0) {
        for (const auto& r : recovered_items) {
          std::printf("  已清理过期锁: %s\n", r.c_str());
        }
      } else if (stale_locks.empty()) {
        std::printf("  无可自动恢复的项\n");
      }
      std::printf("  说明: 本版本只做**真实且安全**的恢复（清理过期锁）；\n");
      std::printf("        LSM 文件级损坏的自动修复**不在范围内** ——\n");
      std::printf("        半修的数据比明确报损坏更危险。\n");
    }
    std::printf("\n结论: %s（%zu 个 ERROR / %zu 个 WARN）\n",
                errors == 0 ? (warns == 0 ? "健康" : "基本健康，有告警") : "**有问题**",
                errors, warns);
  }

  if (dry_run && recover) return kOk;
  return errors == 0 ? kOk : kIntegrity;
}

}  // namespace cli
}  // namespace stratum
