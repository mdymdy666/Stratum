// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_ctl.cc
//
// `stratum ctl` —— 会话的运行时控制与留痕。
//
// 【本版本的范围（诚实说明）】
// 完整 ctl 需要录制进程常驻 + Unix socket 控制通道，本版本**未实现在线控制**。
// 这里做的是**离线子集**：对一个（可能已结束的）会话执行
// `status` / `stop` / `events`。`--session` 不要求进程在跑。
//
// 【最重要的一条设计：`stop --reason` 必填】
//
// verify 会检查"录制是否中断"。若用户主动停止，事后复盘时 verify 会把它
// 归为 **RC-3（发布者停了）** 或 **RC-4（空洞）** —— 于是有人去查
// **并不存在的**问题。
//
// 这与 pruned 区间必须留痕完全同理（那个已实现）：**主动行为必须留痕**，
// 才能与"故障"区分开。所以 reason 不是可选的装饰，而是这条记录的意义所在。

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/statvfs.h>
#include <vector>

#include "cli_common.h"
#include "stratum/core/timeline_impl_v2.h"

using namespace stratum;
using namespace stratum::cli;

namespace stratum {
namespace cli {

namespace {

std::string NowNsStr() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return std::to_string(static_cast<unsigned long long>(ts.tv_sec) * 1000000000ULL +
                        static_cast<unsigned long long>(ts.tv_nsec));
}

// 用 statvfs 估算"还剩多少空间可以写"
// 这是 `status` 要回答的问题之一："还健康吗、多久会写满"
void PrintDiskEstimate(const std::string& path, uint64_t session_bytes,
                       const GlobalOptions& g) {
  struct statvfs vfs;
  if (statvfs(path.c_str(), &vfs) != 0) return;
  const uint64_t free_bytes =
      static_cast<uint64_t>(vfs.f_bavail) * static_cast<uint64_t>(vfs.f_frsize);
  std::fprintf(stderr, "  磁盘可用: %s\n", HumanBytes(free_bytes).c_str());
  if (session_bytes > 0 && free_bytes > 0) {
    // 粗略外推：假设以当前会话的平均速率继续增长
    const double ratio = static_cast<double>(free_bytes) /
                         static_cast<double>(session_bytes);
    std::fprintf(stderr, "  容量提示: 本会话已占 %s；若按同样体量继续，"
                         "还可容纳约 %.1f 倍\n",
                 HumanBytes(session_bytes).c_str(), ratio);
  }
  (void)g;
}

}  // namespace

int CmdCtl(const std::vector<std::string>& args, GlobalOptions g) {
  if (args.empty()) {
    return Fail(kUsage, "ctl 需要子命令",
                "可用: status / stop / events",
                "示例: stratum ctl status /data/stratum/s1");
  }
  const std::string sub = args[0];

  std::string session;
  std::string reason;
  std::string detail;
  bool dry_run = false;

  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](void) -> const std::string* {
      if (i + 1 >= args.size()) return nullptr;
      return &args[++i];
    };
    if (a == "--reason") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--reason 缺少参数值", "",
                          "例如: --reason '调试完成，主动停止'");
      reason = *v;
      continue;
    }
    if (a == "--detail") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--detail 缺少参数值", "", "");
      detail = *v;
      continue;
    }
    if (a == "--dry-run") { dry_run = true; continue; }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "ctl " + sub + " 不支持该选项: " + a,
                  "支持 --reason / --detail / --dry-run",
                  "完整说明见 docs/08-cli-spec.md");
    }
    if (session.empty()) {
      session = a;
    } else {
      return Fail(kUsage, "多余的位置参数: " + a, "", "");
    }
  }

  if (session.empty()) {
    return Fail(kUsage, "ctl " + sub + " 需要会话路径", "位置参数缺失",
                "例如: stratum ctl status /data/stratum/s1");
  }
  if (!SessionExists(session)) {
    return Fail(kNotFound, "会话不存在: " + session,
                "路径不存在或不是目录",
                "用 `stratum session list <父目录>` 列出可用会话");
  }

  TimelineImplV2 timeline;
  Status s = timeline.Open(session);
  if (!s.ok()) {
    return Fail(ExitCodeOf(s), "无法打开会话: " + session, s.ToString(), "");
  }

  SessionMetadata meta;
  (void)timeline.GetSessionMetadata(&meta);
  std::vector<TopicMetadata> topics;
  (void)timeline.ListTopics(&topics);

  uint64_t total_events = 0, total_bytes = 0;
  for (const auto& t : topics) {
    total_events += t.message_count;
    total_bytes += t.byte_count;
  }

  // -------------------------------------------------------------------------
  // status —— 回答"还健康吗、控制过几次、还能写多少"
  // -------------------------------------------------------------------------
  if (sub == "status") {
    std::vector<ControlEvent> events;
    Status es = timeline.ListControlEvents(&events);
    const bool events_supported = es.ok();

    if (g.format == Format::kJson) {
      std::printf("{\"schema_version\":1,\"session\":\"%s\",\"topics\":%zu,"
                  "\"events\":%llu,\"bytes\":%llu,\"has_integrity_issue\":%s,"
                  "\"control_events\":%zu,\"online_control\":false}\n",
                  JsonEscape(session).c_str(), topics.size(),
                  static_cast<unsigned long long>(total_events),
                  static_cast<unsigned long long>(total_bytes),
                  meta.quality.HasIntegrityIssue() ? "true" : "false",
                  events_supported ? events.size() : 0);
    } else {
      std::printf("会话: %s\n", session.c_str());
      std::printf("  话题:     %zu 个\n", topics.size());
      std::printf("  事件:     %s\n", HumanCount(total_events).c_str());
      std::printf("  负载:     %s\n", HumanBytes(total_bytes).c_str());
      std::printf("  完整性:   %s\n",
                  meta.quality.HasIntegrityIssue() ? "**有问题**（用 verify 查看）"
                                                   : "无已知问题");
      std::printf("  控制事件: %s\n",
                  events_supported ? (std::to_string(events.size()) + " 条").c_str()
                                   : "（本实现不支持）");
      if (events_supported && !events.empty()) {
        std::printf("    最近一次: %s (%s)\n", events.back().action.c_str(),
                    events.back().reason.empty() ? "无理由记录"
                                                 : events.back().reason.c_str());
      }
      PrintDiskEstimate(session, total_bytes, g);
      std::printf("\n  说明: 本版本为**离线**控制（不要求录制进程在跑）；\n"
                  "        在线的 socket 控制通道尚未实现。\n");
    }
    timeline.Close();
    return kOk;
  }

  // -------------------------------------------------------------------------
  // stop —— 记录"主动停止"，使 verify 不把它误判为故障
  // -------------------------------------------------------------------------
  if (sub == "stop") {
    // 【关键】reason 必填
    if (reason.empty()) {
      timeline.Close();
      return Fail(kUsage, "ctl stop 必须提供 --reason",
                  "主动停止若不留理由，事后 verify 会把它误判为"
                  "「发布者停了」(RC-3) 或「空洞」(RC-4)，"
                  "于是有人去查并不存在的问题。理由正是这条记录的意义。",
                  "例如: --reason '调试完成，主动停止'；"
                  "若确实是异常中断，请如实写 '发布者崩溃'");
    }

    if (dry_run) {
      std::printf("DRY RUN — 未写入任何记录\n");
      std::printf("  会话:   %s\n", session.c_str());
      std::printf("  动作:   stop\n");
      std::printf("  理由:   %s\n", reason.c_str());
      if (!detail.empty()) std::printf("  详情:   %s\n", detail.c_str());
      std::printf("  影响:   仅追加一条控制事件（不改动任何数据）\n");
      std::printf("  效果:   verify 将把此后的中断视为「主动停止」而非故障\n");
      std::printf("\n  执行去掉 --dry-run 即可\n");
      timeline.Close();
      return kOk;
    }

    ControlEvent e;
    e.at_ns = std::strtoull(NowNsStr().c_str(), nullptr, 10);
    e.action = "stop";
    e.reason = reason;
    e.detail = detail;

    Status rs = timeline.RecordControlEvent(e);
    if (!rs.ok()) {
      timeline.Close();
      return Fail(ExitCodeOf(rs), "记录控制事件失败", rs.ToString(),
                  "确认会话可写");
    }
    (void)timeline.Flush();
    timeline.Close();

    if (g.format == Format::kJson) {
      std::printf("{\"schema_version\":1,\"session\":\"%s\",\"action\":\"stop\","
                  "\"reason\":\"%s\",\"recorded\":true}\n",
                  JsonEscape(session).c_str(), JsonEscape(reason).c_str());
    } else if (!g.quiet) {
      std::fprintf(stderr, "已记录主动停止\n");
      std::fprintf(stderr, "  会话: %s\n", session.c_str());
      std::fprintf(stderr, "  理由: %s\n", reason.c_str());
      std::fprintf(stderr, "\n  提示: 此记录使 verify 能把「主动停止」"
                           "与「发布者异常停止」区分开\n");
    }
    return kOk;
  }

  // -------------------------------------------------------------------------
  // events —— 列出控制事件（审计）
  // -------------------------------------------------------------------------
  if (sub == "events") {
    std::vector<ControlEvent> events;
    Status es = timeline.ListControlEvents(&events);
    if (!es.ok()) {
      timeline.Close();
      return Fail(ExitCodeOf(es), "本实现不支持读取控制事件", es.ToString(), "");
    }
    if (g.format == Format::kJson) {
      std::printf("{\"schema_version\":1,\"session\":\"%s\",\"control_events\":[",
                  JsonEscape(session).c_str());
      for (size_t i = 0; i < events.size(); ++i) {
        if (i) std::printf(",");
        std::printf("{\"at_ns\":%llu,\"action\":\"%s\",\"reason\":\"%s\","
                    "\"detail\":\"%s\"}",
                    static_cast<unsigned long long>(events[i].at_ns),
                    JsonEscape(events[i].action).c_str(),
                    JsonEscape(events[i].reason).c_str(),
                    JsonEscape(events[i].detail).c_str());
      }
      std::printf("]}\n");
    } else if (g.format == Format::kCsv) {
      std::printf("at_ns,action,reason,detail\n");
      for (const auto& e : events) {
        std::printf("%llu,%s,%s,%s\n",
                    static_cast<unsigned long long>(e.at_ns), e.action.c_str(),
                    e.reason.c_str(), e.detail.c_str());
      }
    } else {
      std::printf("会话: %s\n", session.c_str());
      if (events.empty()) {
        std::printf("  没有控制事件记录（从未执行过 ctl 操作）\n");
      } else {
        std::printf("  共 %zu 条控制事件（按时间顺序）:\n\n", events.size());
        for (const auto& e : events) {
          std::printf("  [%s] %s\n",
                      e.at_ns != kInvalidTimestamp ? FormatRfc3339(e.at_ns).c_str()
                                                   : "(无时间戳)",
                      e.action.c_str());
          std::printf("      理由: %s\n",
                      e.reason.empty() ? "(未记录)" : e.reason.c_str());
          if (!e.detail.empty()) std::printf("      详情: %s\n", e.detail.c_str());
        }
      }
    }
    timeline.Close();
    return kOk;
  }

  timeline.Close();
  return Fail(kUsage, "ctl 不支持的子命令: " + sub,
              "本版本支持: status / stop / events（离线控制）",
              "在线的 resume / pause / add / set / snapshot / rotate "
              "需要常驻进程与 socket 通道，尚未实现");
}

}  // namespace cli
}  // namespace stratum
