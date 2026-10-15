// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_metrics.cc
//
// `stratum metrics` —— 把会话状态导出为可抓取的指标。
//
// 【为什么需要它】`ctl status` 是给人看的；监控系统需要**机器可读、可反复抓取**
// 的形态。Stratum 的目标场景是长期常开，运维必须能回答：
//   · 这个会话现在有多少数据？（增长趋势）
//   · 完整性有没有问题？（告警）
//   · 被裁剪过多少？控制过几次？（审计）
//
// 【为什么自己输出 Prometheus 文本，而不是依赖客户端库】
//   · Prometheus 的文本格式极其简单（`# HELP` / `# TYPE` / `名字{标签} 值`）
//   · 引入一个客户端库只为拼接几行文本，是**依赖与收益不成比例**
//   · 输出到 stdout，外部用 textfile collector、curl、或任何抓取器都能用
//
// 【默认格式】Prometheus 文本 —— 这是本命令存在的理由。
// 想要结构化数据用 `--json`（**不继承**全局 `--format`：全局默认是 json，
// 若继承它，"默认"就会把本命令的主要产物挤掉）。

#include <cstdio>
#include <string>
#include <vector>

#include "cli_common.h"
#include "stratum/core/timeline_impl_v2.h"

using namespace stratum;
using namespace stratum::cli;

namespace stratum {
namespace cli {

namespace {

// Prometheus 标签值转义：反斜杠、双引号、换行
std::string PromEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

void HelpType(const char* name, const char* help, const char* type) {
  std::printf("# HELP %s %s\n", name, help);
  std::printf("# TYPE %s %s\n", name, type);
}

}  // namespace

int CmdMetrics(const std::vector<std::string>& args, GlobalOptions g) {
  std::string session;
  // 【为什么不继承全局 --format】全局默认是 json，而本命令的主要产物是
  // Prometheus 文本。若继承默认，用户"不加任何参数"却拿不到主要产物 ——
  // 那"默认"就失去意义了。所以这里用显式的 --json 切换。
  bool want_json = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--json") { want_json = true; continue; }
    if (a == "--session") {
      if (i + 1 >= args.size()) {
        return Fail(kUsage, "--session 缺少参数值", "", "");
      }
      session = args[++i];
      continue;
    }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "metrics 不支持该选项: " + a,
                  "metrics 只接受会话路径；输出默认是 Prometheus 文本，"
                  "用 --json 切换为 JSON",
                  "例如: stratum metrics /data/s1 或 stratum metrics --json /data/s1");
    }
    if (session.empty()) {
      session = a;
    } else {
      return Fail(kUsage, "多余的位置参数: " + a, "", "");
    }
  }
  if (session.empty()) {
    return Fail(kUsage, "metrics 需要会话路径", "位置参数缺失",
                "例如: stratum metrics /data/stratum/s1");
  }
  // 查询类命令不得有创建副作用
  if (!SessionExists(session)) {
    return Fail(kNotFound, "会话不存在: " + session,
                "路径不存在或不是目录",
                "用 `stratum session list <父目录>` 列出可用会话");
  }

  TimelineImplV2 timeline;
  Status s = timeline.Open(session);
  if (!s.ok()) {
    return Fail(ExitCodeOf(s), "无法打开会话: " + session, s.ToString(),
                "会话可能损坏；用 `stratum doctor <会话>` 体检");
  }

  SessionMetadata meta;
  (void)timeline.GetSessionMetadata(&meta);
  std::vector<TopicMetadata> topics;
  (void)timeline.ListTopics(&topics);

  uint64_t sum_events = 0, sum_bytes = 0, with_def = 0;
  for (const auto& t : topics) {
    sum_events += t.message_count;
    sum_bytes += t.byte_count;
    std::string def;
    if (timeline.GetTopicTypeDefinition(t.topic_name, &def).ok() && !def.empty()) {
      ++with_def;
    }
  }
  std::vector<ControlEvent> events;
  (void)timeline.ListControlEvents(&events);
  uint64_t pruned_ranges = 0, pruned_events = 0;
  for (const auto& t : topics) {
    std::vector<PrunedRange> pr;
    if (timeline.ListPrunedRanges(t.topic_id, &pr).ok()) {
      pruned_ranges += pr.size();
      for (const auto& r : pr) pruned_events += r.deleted_events;
    }
  }
  const bool integrity = meta.quality.HasIntegrityIssue();
  (void)timeline.Close();

  const std::string sess = PromEscape(session);

  // -------------------------------------------------------------------------
  // JSON（全局 --format json 时）
  // -------------------------------------------------------------------------
  if (want_json) {
    std::printf("{\"schema_version\":1,\"session\":\"%s\","
                "\"session_events_total\":%llu,\"session_bytes_total\":%llu,"
                "\"topics_total\":%zu,\"topics_with_definition\":%llu,"
                "\"integrity_issue\":%s,\"pruned_ranges_total\":%llu,"
                "\"pruned_events_total\":%llu,\"control_events_total\":%zu,"
                "\"topics\":[",
                JsonEscape(session).c_str(),
                static_cast<unsigned long long>(sum_events),
                static_cast<unsigned long long>(sum_bytes), topics.size(),
                static_cast<unsigned long long>(with_def),
                integrity ? "true" : "false",
                static_cast<unsigned long long>(pruned_ranges),
                static_cast<unsigned long long>(pruned_events), events.size());
    for (size_t i = 0; i < topics.size(); ++i) {
      if (i) std::printf(",");
      std::printf("{\"topic\":\"%s\",\"type\":\"%s\",\"events\":%llu,"
                  "\"bytes\":%llu,\"active\":%s}",
                  JsonEscape(topics[i].topic_name).c_str(),
                  JsonEscape(topics[i].type_name).c_str(),
                  static_cast<unsigned long long>(topics[i].message_count),
                  static_cast<unsigned long long>(topics[i].byte_count),
                  topics[i].active ? "true" : "false");
    }
    std::printf("]}\n");
    return kOk;
  }

  // -------------------------------------------------------------------------
  // Prometheus 文本（默认）
  // -------------------------------------------------------------------------
  HelpType("stratum_session_events_total",
           "会话内的事件总数（逐话题汇总）", "gauge");
  std::printf("stratum_session_events_total{session=\"%s\"} %llu\n",
              sess.c_str(), static_cast<unsigned long long>(sum_events));

  HelpType("stratum_session_bytes_total",
           "会话内的负载总字节数（逐话题汇总）", "gauge");
  std::printf("stratum_session_bytes_total{session=\"%s\"} %llu\n",
              sess.c_str(), static_cast<unsigned long long>(sum_bytes));

  HelpType("stratum_topics_total", "会话内的话题数量", "gauge");
  std::printf("stratum_topics_total{session=\"%s\"} %zu\n", sess.c_str(),
              topics.size());

  HelpType("stratum_topics_with_definition_total",
           "已注册类型定义的话题数量（这些话题可被 --decode 解出结构化数据）",
           "gauge");
  std::printf("stratum_topics_with_definition_total{session=\"%s\"} %llu\n",
              sess.c_str(), static_cast<unsigned long long>(with_def));

  // 0/1 形式的告警指标：便于直接写告警规则（== 1 即告警）
  HelpType("stratum_integrity_issue",
           "会话是否存在完整性问题（1 = 有，需用 verify 查看归因）", "gauge");
  std::printf("stratum_integrity_issue{session=\"%s\"} %d\n", sess.c_str(),
              integrity ? 1 : 0);

  HelpType("stratum_pruned_ranges_total",
           "被主动裁剪的区间数（主动行为，不是数据丢失）", "gauge");
  std::printf("stratum_pruned_ranges_total{session=\"%s\"} %llu\n", sess.c_str(),
              static_cast<unsigned long long>(pruned_ranges));

  HelpType("stratum_pruned_events_total",
           "被主动裁剪的事件条数（主动行为，不是数据丢失）", "gauge");
  std::printf("stratum_pruned_events_total{session=\"%s\"} %llu\n", sess.c_str(),
              static_cast<unsigned long long>(pruned_events));

  HelpType("stratum_control_events_total",
           "控制事件条数（含 ctl stop 等主动操作）", "gauge");
  std::printf("stratum_control_events_total{session=\"%s\"} %zu\n", sess.c_str(),
              events.size());

  if (!topics.empty()) {
    HelpType("stratum_topic_events", "单个话题的事件条数", "gauge");
    for (const auto& t : topics) {
      std::printf("stratum_topic_events{session=\"%s\",topic=\"%s\",type=\"%s\"} "
                  "%llu\n",
                  sess.c_str(), PromEscape(t.topic_name).c_str(),
                  PromEscape(t.type_name).c_str(),
                  static_cast<unsigned long long>(t.message_count));
    }

    HelpType("stratum_topic_bytes", "单个话题的负载字节数", "gauge");
    for (const auto& t : topics) {
      std::printf("stratum_topic_bytes{session=\"%s\",topic=\"%s\",type=\"%s\"} "
                  "%llu\n",
                  sess.c_str(), PromEscape(t.topic_name).c_str(),
                  PromEscape(t.type_name).c_str(),
                  static_cast<unsigned long long>(t.byte_count));
    }

    HelpType("stratum_topic_active",
             "话题是否仍在接收数据（1 = 是，0 = 已停止或已删空）", "gauge");
    for (const auto& t : topics) {
      std::printf("stratum_topic_active{session=\"%s\",topic=\"%s\"} %d\n",
                  sess.c_str(), PromEscape(t.topic_name).c_str(),
                  t.active ? 1 : 0);
    }
  }

  return kOk;
}

}  // namespace cli
}  // namespace stratum
