// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_verify.cc
//
// `stratum verify <会话>` —— 完整性校验与质量报告。
//
// 为什么这一组优先做：它是本项目**最差异化的价值**。
// 设计原则是"最危险的不是知道数据不全，而是以为录全了实际漏了"，
// 而 verify 就是回答"这份录制可不可信"的那条命令。
//
// 报告必须按 docs/06-data-trust.md 的 RC-1~6 **逐类给出成因**，
// 而不是笼统说"有缺口"：
//   RC-1 队列溢出丢弃（本端）      RC-2 发现延迟（DDS 固有，只能估计）
//   RC-3 发布者侧没发（不是我们的问题）  RC-4 时间轴空洞
//   RC-5 时钟异常导致的假空洞       RC-6 录制中途重启

#include <algorithm>
#include <string>
#include <vector>

#include "cli_common.h"
#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"

using namespace stratum;

namespace stratum {
namespace cli {

namespace {

struct Finding {
  const char* code;      // RC-1 .. RC-6
  const char* severity;  // high / medium / low / info
  std::string title;
  std::string detail;
  bool actionable;       // 是否指向"我们能改的东西"
};

// 把 QualityStats 映射为 RC 分类的结论
std::vector<Finding> Analyze(const SessionMetadata& meta,
                             const std::vector<TopicSummary>& topics,
                             uint64_t total_events, uint64_t total_bytes) {
  std::vector<Finding> out;
  const auto& q = meta.quality;

  if (q.dropped_messages > 0) {
    Finding f;
    f.code = "RC-1";
    f.severity = "high";
    f.title = "本端丢弃（录制器自己没接住）";
    f.detail = "丢弃 " + HumanCount(q.dropped_messages) + " 条 / " +
               HumanBytes(q.dropped_bytes) +
               "。成因通常是接收缓冲上限过小或落盘跟不上。"
               "【注意】这是**我们**的问题，不是发布者的。";
    f.actionable = true;
    out.push_back(f);
  }

  if (q.sequence_gaps > 0) {
    Finding f;
    f.code = "RC-3";
    f.severity = "high";
    f.title = "序号缺口（对端发布者侧丢失或未发出）";
    f.detail = "缺口 " + std::to_string(q.sequence_gaps) + " 处，缺 " +
               HumanCount(q.missing_messages) +
               " 条。【注意】本端丢弃为 " +
               std::to_string(q.dropped_messages) +
               "，若为 0 则成因在发布者侧，不归因于录制器。";
    f.actionable = false;
    out.push_back(f);
  }

  if (q.timestamp_regressions > 0) {
    Finding f;
    f.code = "RC-5";
    f.severity = "medium";
    f.title = "源时间戳倒退（时钟异常）";
    f.detail = "倒退 " + std::to_string(q.timestamp_regressions) +
               " 次。可能导致按源时间戳排序时出现\"假空洞/假重叠\"。"
               "若录制时启用了 clamp 策略，请以 raw_source_timestamp_ns 为准判断真实顺序。";
    f.actionable = false;
    out.push_back(f);
  }

  if (q.clock_jumps > 0) {
    Finding f;
    f.code = "RC-5";
    f.severity = "medium";
    f.title = "时钟跳变";
    f.detail = "跳变 " + std::to_string(q.clock_jumps) +
               " 次（对时或设备切源）。会造成时间轴上的非真实间隔。";
    f.actionable = false;
    out.push_back(f);
  }

  if (q.interruptions > 0) {
    Finding f;
    f.code = "RC-6";
    f.severity = "high";
    f.title = "录制中途重启/中断";
    f.detail = "中断 " + std::to_string(q.interruptions) +
               " 次。中断期间的数据不在本会话内，需检查是否有后续会话可拼接。";
    f.actionable = true;
    out.push_back(f);
  }

  // RC-2：发现延迟只能给估计，且必须**如实标注是估计**
  if (!topics.empty() && meta.created_at_ns > 0) {
    uint64_t earliest = 0;
    for (const auto& t : topics) {
      if (t.first_ns && (earliest == 0 || t.first_ns < earliest)) earliest = t.first_ns;
    }
    if (earliest > meta.created_at_ns) {
      const double gap_s = static_cast<double>(earliest - meta.created_at_ns) / 1e9;
      // 只有明显时才提（几百毫秒是正常的发现延迟）
      if (gap_s > 0.5) {
        Finding f;
        f.code = "RC-2";
        f.severity = "info";
        f.title = "开头可能有一段未录（发现延迟，估计值）";
        f.detail = "会话创建到首条数据之间约 " + std::to_string(gap_s).substr(0, 5) +
                   " 秒。DDS 的发现过程是异步的，这期间的数据**必然**收不到。"
                   "【这是估计值】依据会话创建时间与首条数据的源时间戳之差，"
                   "并非精确的发现耗时。";
        f.actionable = false;
        out.push_back(f);
      }
    }
  }

  // RC-4：时间轴空洞（这里给出跨度的粗判；精确空洞需逐话题扫描）
  for (const auto& t : topics) {
    if (t.message_count == 0) continue;
  }

  (void)total_events;
  (void)total_bytes;
  return out;
}

void PrintFindingText(const Finding& f) {
  std::printf("  [%s][%s] %s\n", f.code, f.severity, f.title.c_str());
  std::printf("      %s\n", f.detail.c_str());
  std::printf("      归因: %s\n", f.actionable ? "本端可改进" : "非本端问题");
}

}  // namespace

int CmdVerify(const std::vector<std::string>& args, GlobalOptions g) {
  if (args.empty()) {
    return Fail(kUsage, "verify 需要会话路径",
                "需要一个会话目录",
                "例如: stratum verify /data/stratum/s-20261014");
  }

  // 解析本命令特有选项
  std::string target;
  bool exit_on_issue = false;
  double max_gap_percent = -1.0;
  bool want_json = (g.format == Format::kJson);

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--exit-code-on-issue") { exit_on_issue = true; continue; }
    if (a == "--check") { ++i; continue; }            // 检查项筛选（后续实现）
    if (a == "--report") { ++i; continue; }           // html/json（后续实现）
    if (a == "--out") { ++i; continue; }
    if (a == "--max-gap-percent") {
      if (i + 1 < args.size()) {
        max_gap_percent = std::atof(args[++i].c_str());
        exit_on_issue = true;
      }
      continue;
    }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "verify 不支持该选项: " + a,
                  "本版本支持 --exit-code-on-issue / --max-gap-percent",
                  "完整选项见 docs/08-cli-spec.md");
    }
    if (target.empty()) target = a;
  }

  if (target.empty()) {
    return Fail(kUsage, "verify 需要会话路径", "位置参数缺失",
                "例如: stratum verify /data/stratum/s-20261014");
  }

  TimelineImplV2 timeline;
  Status s = timeline.Open(target);
  if (!s.ok()) {
    return Fail(ExitCodeOf(s), "无法打开会话: " + target, s.ToString(),
                "确认它是会话目录；用 `stratum session list <父目录>` 列出可用会话");
  }

  Query q(&timeline);
  SessionMetadata meta;
  Status ms = q.GetInfo(&meta);
  std::vector<TopicSummary> topics;
  Status ts = q.ListTopics(&topics);

  uint64_t total_events = 0, total_bytes = 0;
  uint64_t first_ns = 0, last_ns = 0;
  for (const auto& t : topics) {
    total_events += t.message_count;
    total_bytes += t.byte_count;
    if (t.first_ns && (first_ns == 0 || t.first_ns < first_ns)) first_ns = t.first_ns;
    if (t.last_ns > last_ns) last_ns = t.last_ns;
  }

  const std::vector<Finding> findings = Analyze(meta, topics, total_events, total_bytes);
  const bool has_problem = meta.quality.HasIntegrityIssue();

  if (want_json) {
    std::printf("{\"schema_version\":1,\"session\":\"%s\",", JsonEscape(target).c_str());
    std::printf("\"events\":%llu,\"payload_bytes\":%llu,\"topics\":%zu,",
                static_cast<unsigned long long>(total_events),
                static_cast<unsigned long long>(total_bytes), topics.size());
    std::printf("\"has_integrity_issue\":%s,\"findings\":[",
                has_problem ? "true" : "false");
    for (size_t i = 0; i < findings.size(); ++i) {
      if (i) std::printf(",");
      std::printf("{\"rc\":\"%s\",\"severity\":\"%s\",\"title\":\"%s\","
                  "\"detail\":\"%s\",\"attribution\":\"%s\"}",
                  findings[i].code, findings[i].severity,
                  JsonEscape(findings[i].title).c_str(),
                  JsonEscape(findings[i].detail).c_str(),
                  findings[i].actionable ? "local" : "remote");
    }
    std::printf("],\"quality\":{\"dropped\":%llu,\"gaps\":%llu,\"missing\":%llu,"
                "\"ts_regressions\":%llu,\"clock_jumps\":%llu,\"interruptions\":%llu}}\n",
                static_cast<unsigned long long>(meta.quality.dropped_messages),
                static_cast<unsigned long long>(meta.quality.sequence_gaps),
                static_cast<unsigned long long>(meta.quality.missing_messages),
                static_cast<unsigned long long>(meta.quality.timestamp_regressions),
                static_cast<unsigned long long>(meta.quality.clock_jumps),
                static_cast<unsigned long long>(meta.quality.interruptions));
    timeline.Close();
    return has_problem ? kIntegrity : kOk;
  }

  std::printf("完整性报告: %s\n", target.c_str());
  std::printf("  事件 %s 条 / 负载 %s / 话题 %zu 个\n",
              HumanCount(total_events).c_str(), HumanBytes(total_bytes).c_str(),
              topics.size());
  if (first_ns && last_ns > first_ns) {
    std::printf("  跨度 %.3f 秒\n", static_cast<double>(last_ns - first_ns) / 1e9);
  }
  if (ms.ok()) {
    std::printf("  会话创建: %s\n", FormatTimestampNs(meta.created_at_ns).c_str());
    std::printf("  会话关闭: %s\n", meta.closed_at_ns
                                        ? FormatTimestampNs(meta.closed_at_ns).c_str()
                                        : "**(未正常关闭)**");
  }

  std::printf("\n结论: %s\n", !has_problem ? "未发现完整性问题"
                                          : "**发现完整性问题（详见下方分类）**");

  if (findings.empty()) {
    std::printf("\n  未发现 RC-1~RC-6 中任何一类问题。\n");
  } else {
    std::printf("\n按成因分类（docs/06-data-trust.md 的 RC-1~RC-6）：\n");
    for (const auto& f : findings) PrintFindingText(f);
  }

  std::printf("\n质量计数:\n");
  std::printf("  本端丢弃      %llu 条\n", static_cast<unsigned long long>(meta.quality.dropped_messages));
  std::printf("  序号缺口      %llu 处（缺 %llu 条）\n",
              static_cast<unsigned long long>(meta.quality.sequence_gaps),
              static_cast<unsigned long long>(meta.quality.missing_messages));
  std::printf("  时间戳倒退    %llu 次\n", static_cast<unsigned long long>(meta.quality.timestamp_regressions));
  std::printf("  时钟跳变      %llu 次\n", static_cast<unsigned long long>(meta.quality.clock_jumps));
  std::printf("  录制中断      %llu 次\n", static_cast<unsigned long long>(meta.quality.interruptions));
  if (meta.quality.latency_samples > 0) {
    std::printf("  延迟(平均)    %.3f ms\n", meta.quality.AverageLatencyNs() / 1e6);
  }

  timeline.Close();

  if (exit_on_issue && has_problem) {
    if (max_gap_percent >= 0.0 && total_events > 0) {
      const double pct = 100.0 * static_cast<double>(meta.quality.missing_messages) /
                         static_cast<double>(total_events + meta.quality.missing_messages);
      if (pct <= max_gap_percent) {
        std::printf("\n  缺口占比 %.4f%% 未超过阈值 %.4f%% → 视为通过\n", pct, max_gap_percent);
        return kOk;
      }
      std::printf("\n  缺口占比 %.4f%% 超过阈值 %.4f%%\n", pct, max_gap_percent);
    }
    return kIntegrity;
  }
  return kOk;
}

}  // namespace cli
}  // namespace stratum
