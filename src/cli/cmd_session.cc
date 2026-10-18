// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_session.cc
//
// `stratum session list` / `stratum session info`
//
// 为什么先做这两个：它们是**只读**的，零风险，而且是使用者的第一需求 ——
// "我录了什么、在哪、可不可信"。没有它，前面所有能力都无从查看。

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "cli_common.h"
#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"

namespace fs = std::filesystem;

using namespace stratum;

namespace stratum {
namespace cli {

namespace {

// ---------------------------------------------------------------------------
// 识别一个目录是否是 Stratum 会话
//
// 会话底层是 LSM 存储，因此带有 LSM 的标志性文件（CURRENT / MANIFEST-*）。
// 用特征识别而不是靠命名约定，这样用户手工改过目录名也能认出来。
// ---------------------------------------------------------------------------
bool LooksLikeSession(const fs::path& p) {
  std::error_code ec;
  if (!fs::is_directory(p, ec)) return false;
  if (fs::exists(p / "CURRENT", ec)) return true;
  fs::directory_iterator it(p, ec);
  if (ec) return false;
  for (const auto& e : it) {
    const std::string n = e.path().filename().string();
    if (n.rfind("MANIFEST-", 0) == 0) return true;
  }
  return false;
}

struct SessionEntry {
  std::string name;
  std::string path;
  uint64_t bytes = 0;
  uint64_t files = 0;
  std::string mtime;
};

// 汇总目录大小（有上限保护：避免对超大目录做无界遍历）
SessionEntry Summarize(const fs::path& p) {
  SessionEntry e;
  e.name = p.filename().string();
  e.path = p.string();
  std::error_code ec;
  for (const auto& f : fs::recursive_directory_iterator(p, ec)) {
    if (ec) break;
    if (!f.is_regular_file(ec)) continue;
    e.bytes += static_cast<uint64_t>(f.file_size(ec));
    ++e.files;
    if (e.files > 200000) break;   // 保护：不无限遍历
  }
  const auto t = fs::last_write_time(p, ec);
  if (!ec) {
    // 转成可读时间（C++17 无直接转换，用 file_clock 差值近似到系统时间）
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
    std::tm tmv{};
    gmtime_r(&tt, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    e.mtime = buf;
  }
  return e;
}

void PrintSessionsTable(const std::vector<SessionEntry>& v, const GlobalOptions& g) {
  PrintTableHeader({{"会话", 28}, {"大小", 10}, {"文件数", 8}, {"最后修改(UTC)", 20}, {"路径", 40}}, g);
  for (const auto& e : v) {
    std::printf("%-28s  %-10s %-8llu %-20s %s\n", e.name.c_str(),
                HumanBytes(e.bytes).c_str(),
                static_cast<unsigned long long>(e.files), e.mtime.c_str(),
                e.path.c_str());
  }
}

void PrintSessionsJson(const std::vector<SessionEntry>& v) {
  std::printf("{\"schema_version\":1,\"sessions\":[");
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) std::printf(",");
    std::printf("{\"name\":\"%s\",\"path\":\"%s\",\"bytes\":%llu,\"files\":%llu,"
                "\"mtime_utc\":\"%s\"}",
                JsonEscape(v[i].name).c_str(), JsonEscape(v[i].path).c_str(),
                static_cast<unsigned long long>(v[i].bytes),
                static_cast<unsigned long long>(v[i].files), v[i].mtime.c_str());
  }
  std::printf("]}\n");
}

// ---------------------------------------------------------------------------
// session list <目录>
// ---------------------------------------------------------------------------
int DoList(const std::vector<std::string>& args, GlobalOptions g) {
  if (args.empty()) {
    return Fail(kUsage, "session list 需要目录参数",
                "需要一个包含会话的目录", "例如: stratum session list /data/stratum");
  }
  const std::string dir = args[0];
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    return Fail(kNotFound, "目录不存在: " + dir, ec ? ec.message() : "路径不是一个目录",
                "确认路径是否正确；录音目录由 `stratum record --dir` 指定");
  }

  std::vector<SessionEntry> out;
  // 先看目录本身是不是一个会话，再看子目录
  if (LooksLikeSession(dir)) out.push_back(Summarize(dir));
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!e.is_directory(ec)) continue;
    if (!LooksLikeSession(e.path())) continue;
    out.push_back(Summarize(e.path()));
  }
  std::sort(out.begin(), out.end(),
            [](const SessionEntry& a, const SessionEntry& b) { return a.name < b.name; });

  if (g.format == Format::kJson) {
    PrintSessionsJson(out);
  } else {
    if (out.empty() && !g.quiet) {
      std::printf("（该目录下没有找到会话）\n");
      std::printf("  提示: 会话目录的特征是含 CURRENT 或 MANIFEST-* 文件（LSM 存储）\n");
    }
    PrintSessionsTable(out, g);
    if (!g.quiet && !out.empty()) {
      uint64_t total = 0;
      for (const auto& e : out) total += e.bytes;
      std::printf("\n  共 %zu 个会话，合计 %s\n", out.size(), HumanBytes(total).c_str());
    }
  }
  return kOk;
}

// ---------------------------------------------------------------------------
// session info <会话路径>
// ---------------------------------------------------------------------------
int DoInfo(const std::vector<std::string>& args, GlobalOptions g) {
  if (args.empty()) {
    return Fail(kUsage, "session info 需要会话路径",
                "需要一个会话目录或其父目录",
                "例如: stratum session info /data/stratum/s-20261014");
  }
  const std::string target = args[0];

  // 【重要】查询类命令**不得有创建副作用**。
  // TimelineImplV2::Open() 对不存在的路径会创建会话目录（录制时正需如此），
  // 若这里不检查，`session info /typo/path` 会"成功"并显示"话题 0 个"，
  // 使用者会以为"这个会话是空的"，而不是"路径打错了"。
  if (!SessionExists(target)) {
    return Fail(kNotFound, "会话不存在: " + target,
                "路径不存在或不是目录",
                "用 `stratum session list <父目录>` 列出可用会话；"
                "注意 session info 不会创建会话");
  }

  TimelineImplV2 timeline;
  Status s = timeline.Open(target);
  if (!s.ok()) {
    return Fail(ExitCodeOf(s), "无法打开会话: " + target, s.ToString(),
                "确认它是会话目录（含 CURRENT / MANIFEST-*）；"
                "若只想列出会话请用 `stratum session list <父目录>`");
  }

  Query q(&timeline);
  SessionMetadata meta;
  Status ms = q.GetInfo(&meta);
  std::vector<TopicSummary> topics;
  Status ts = q.ListTopics(&topics);

  // 时间跨度（用第一个话题的首尾；会话级跨度需遍历，这里给近似）
  uint64_t first_ns = 0, last_ns = 0;
  uint64_t total_events = 0, total_bytes = 0;
  for (const auto& t : topics) {
    if (first_ns == 0 || (t.first_ns && t.first_ns < first_ns)) first_ns = t.first_ns;
    if (t.last_ns > last_ns) last_ns = t.last_ns;
    total_events += t.message_count;
    total_bytes += t.byte_count;
  }

  const bool has_issue = meta.quality.HasIntegrityIssue();

  if (g.format == Format::kJson) {
    std::printf("{\"schema_version\":1,\"session\":{");
    std::printf("\"path\":\"%s\",", JsonEscape(target).c_str());
    if (ms.ok()) {
      std::printf("\"name\":\"%s\",\"format_version\":%u,\"stratum_version\":\"%s\",",
                  JsonEscape(meta.session_name).c_str(), meta.format_version,
                  JsonEscape(meta.stratum_version).c_str());
      std::printf("\"created_at_ns\":%llu,\"closed_at_ns\":%llu,",
                  static_cast<unsigned long long>(meta.created_at_ns),
                  static_cast<unsigned long long>(meta.closed_at_ns));
      std::printf("\"meta_total_events\":%llu,\"meta_total_bytes\":%llu,",
                  static_cast<unsigned long long>(meta.total_events),
                  static_cast<unsigned long long>(meta.total_bytes));
    }
    std::printf("\"topics\":%zu,\"events\":%llu,\"payload_bytes\":%llu,",
                topics.size(), static_cast<unsigned long long>(total_events),
                static_cast<unsigned long long>(total_bytes));
    std::printf("\"has_integrity_issue\":%s", has_issue ? "true" : "false");
    std::printf("},\"topics\":[");
    for (size_t i = 0; i < topics.size(); ++i) {
      if (i) std::printf(",");
      std::printf("{\"name\":\"%s\",\"type\":\"%s\",\"count\":%llu,\"bytes\":%llu,"
                  "\"first_ns\":%llu,\"last_ns\":%llu}",
                  JsonEscape(topics[i].topic_name).c_str(),
                  JsonEscape(topics[i].type_name).c_str(),
                  static_cast<unsigned long long>(topics[i].message_count),
                  static_cast<unsigned long long>(topics[i].byte_count),
                  static_cast<unsigned long long>(topics[i].first_ns),
                  static_cast<unsigned long long>(topics[i].last_ns));
    }
    std::printf("]}\n");
    timeline.Close();
    return kOk;
  }

  std::printf("会话: %s\n", target.c_str());
  if (ms.ok()) {
    std::printf("  名称:       %s\n", meta.session_name.empty() ? "(未设置)" : meta.session_name.c_str());
    std::printf("  格式版本:   %u\n", meta.format_version);
    std::printf("  写入版本:   %s\n", meta.stratum_version.empty() ? "(未记录)" : meta.stratum_version.c_str());
    std::printf("  创建:       %s\n", FormatTimestampNs(meta.created_at_ns).c_str());
    std::printf("  关闭:       %s\n", meta.closed_at_ns ? FormatTimestampNs(meta.closed_at_ns).c_str()
                                                      : "(未正常关闭 —— 可能崩溃)");
  } else {
    std::printf("  (元数据读取失败: %s)\n", ms.ToString().c_str());
  }

  std::printf("\n话题: %zu 个，事件 %s 条，负载 %s\n", topics.size(),
              HumanCount(total_events).c_str(), HumanBytes(total_bytes).c_str());
  if (first_ns && last_ns > first_ns) {
    std::printf("跨度: %s ~ %s（%.3f 秒）\n", FormatTimestampNs(first_ns).c_str(),
                FormatTimestampNs(last_ns).c_str(),
                static_cast<double>(last_ns - first_ns) / 1e9);
  }

  PrintTableHeader({{"话题", 30}, {"类型", 20}, {"条数", 12}, {"字节", 12}, {"起始偏移", 12}}, g);
  for (const auto& t : topics) {
    std::printf("%-30s  %-20s %-12s %-12s %s\n", t.topic_name.c_str(),
                t.type_name.c_str(), HumanCount(t.message_count).c_str(),
                HumanBytes(t.byte_count).c_str(),
                first_ns ? FormatOffsetNs(first_ns, t.first_ns).c_str() : "-");
  }

  std::printf("\n质量: %s\n", has_issue ? "**存在完整性问题**" : "无已知问题");
  const auto& qy = meta.quality;
  std::printf("  丢弃(本端):   %llu 条 / %s\n",
              static_cast<unsigned long long>(qy.dropped_messages),
              HumanBytes(qy.dropped_bytes).c_str());
  std::printf("  序号缺口(对端): %llu 处 / 缺 %llu 条\n",
              static_cast<unsigned long long>(qy.sequence_gaps),
              static_cast<unsigned long long>(qy.missing_messages));
  std::printf("  时间戳倒退:   %llu 次\n", static_cast<unsigned long long>(qy.timestamp_regressions));
  std::printf("  时钟跳变:     %llu 次\n", static_cast<unsigned long long>(qy.clock_jumps));
  std::printf("  录制中断:     %llu 次\n", static_cast<unsigned long long>(qy.interruptions));
  if (qy.latency_samples > 0) {
    std::printf("  端到端延迟:   平均 %.3f ms（样本 %llu）\n",
                qy.AverageLatencyNs() / 1e6,
                static_cast<unsigned long long>(qy.latency_samples));
  }
  std::printf("\n  提示: 完整性报告请用 `stratum verify %s`\n", target.c_str());

  timeline.Close();
  return kOk;
}

}  // namespace

int CmdSession(const std::vector<std::string>& args, GlobalOptions g) {
  if (args.empty()) {
    return Fail(kUsage, "session 需要子命令", "可用子命令: list / info",
                "例如: stratum session list /data/stratum");
  }
  const std::string sub = args[0];
  const std::vector<std::string> rest(args.begin() + 1, args.end());
  if (sub == "list") return DoList(rest, g);
  if (sub == "info") return DoInfo(rest, g);
  // 实现在 cmd_merge.cc（设计裁决见 docs/16-session-merge.md）
  if (sub == "merge") return CmdMerge(rest, g);
  if (sub == "delete" || sub == "prune" || sub == "tag") {
    return CmdNotImplemented("session " + sub);
  }
  return Fail(kUsage, "未知的 session 子命令: " + sub, "本版本支持 list / info",
              "完整设计见 docs/08-cli-spec.md 第 7 节");
}

}  // namespace cli
}  // namespace stratum
