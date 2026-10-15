// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_prune.cc
//
// `stratum prune` —— 按条件删除持久化数据。
//
// 设计依据：docs/07-cli-design.md 场景 S18、docs/08-cli-spec.md 第 6 节。
//
// 【这是本项目最危险的命令】删除是整个系统里唯一不可逆的操作。
// 因此它有三层保护，且默认不删：
//
//   1. `--where` 必填 —— 拒绝"无条件全删"。
//      空选择器是最容易发生的事故形态（手滑少打一个参数）。
//   2. **默认 --dry-run**，必须显式 `--yes` 才真删。
//      "忘了加 dry-run 就删库"必须由**设计**杜绝，不能靠纪律。
//   3. `--max-delete-bytes`（默认 10GB）与 `--max-delete-percent`（默认 50%）
//      双重上限，超过则**拒绝执行(2)**，除非 `--i-know-what-i-am-doing`。
//      防止"选择器写宽了"造成大面积误删 —— 这正是 EC-17 的教训。
//
// 【dry-run 的输出是设计的一部分，不是实现细节】（docs/08 6.1 节）
// 它必须包含：选了什么、删什么、多少、**保护检查是否通过**、元数据怎么办、
// 审计写到哪。缺任何一项，用户在按下 --yes 前就少了一个判断依据。

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "cli_common.h"
#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"

namespace fs = std::filesystem;

using namespace stratum;
using namespace stratum::cli;

namespace stratum {
namespace cli {

namespace {

// ---------------------------------------------------------------------------
// 选择器（简化版：只支持 topic / since / until 与 and 连接）
//
// 完整表达式语言见 docs/08 第 3 节，其中 or / not / 括号 / seq / size / qos
// 等尚未实现 —— 遇到不认识的子句**明确报错**，不猜测语义。
// ---------------------------------------------------------------------------
struct Selector {
  std::vector<std::string> topics;      // 精确话题名
  std::vector<std::string> patterns;    // 前缀通配（如 sensor/*）
  bool has_since = false;
  bool has_until = false;
  std::string since_raw;
  std::string until_raw;
  std::string original;                 // 原样保留，供审计（docs/08 2.3 节）
};

std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::string Unquote(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

// 把选择器字符串按 " and " 切分（大小写不敏感）
std::vector<std::string> SplitAnd(const std::string& s) {
  std::vector<std::string> out;
  std::string lower = s;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  size_t pos = 0;
  while (pos <= s.size()) {
    const size_t found = lower.find(" and ", pos);
    if (found == std::string::npos) {
      out.push_back(s.substr(pos));
      break;
    }
    out.push_back(s.substr(pos, found - pos));
    pos = found + 5;
  }
  return out;
}

bool ParseSelector(const std::string& text, Selector* out, std::string* err) {
  out->original = text;
  const std::string t = Trim(text);
  if (t.empty()) {
    if (err) *err = "选择器为空";
    return false;
  }
  for (const std::string& raw : SplitAnd(t)) {
    const std::string c = Trim(raw);
    if (c.empty()) continue;

    // topic = X  |  topic ~ X
    if (c.rfind("topic", 0) == 0) {
      size_t i = 5;
      while (i < c.size() && std::isspace(static_cast<unsigned char>(c[i]))) ++i;
      if (i >= c.size() || (c[i] != '=' && c[i] != '~')) {
        if (err) *err = "topic 子句应为 `topic = <名字>` 或 `topic ~ <前缀*>`，收到: " + c;
        return false;
      }
      const bool is_pattern = (c[i] == '~');
      const std::string val = Unquote(Trim(c.substr(i + 1)));
      if (val.empty()) {
        if (err) *err = "topic 子句缺少取值: " + c;
        return false;
      }
      if (is_pattern) out->patterns.push_back(val);
      else out->topics.push_back(val);
      continue;
    }
    // since X
    if (c.rfind("since", 0) == 0) {
      out->since_raw = Unquote(Trim(c.substr(5)));
      if (out->since_raw.empty()) {
        if (err) *err = "since 子句缺少时间值";
        return false;
      }
      out->has_since = true;
      continue;
    }
    // until X
    if (c.rfind("until", 0) == 0) {
      out->until_raw = Unquote(Trim(c.substr(5)));
      if (out->until_raw.empty()) {
        if (err) *err = "until 子句缺少时间值";
        return false;
      }
      out->has_until = true;
      continue;
    }
    if (err) {
      *err = "暂不支持的子句: " + c +
             "（本版本支持 topic = / topic ~ / since / until，"
             "并用 and 连接；完整表达式语言见 docs/08 第 3 节）";
    }
    return false;
  }
  if (out->topics.empty() && out->patterns.empty()) {
    if (err) *err = "选择器必须至少包含一个 topic 子句（拒绝无条件全删）";
    return false;
  }
  return true;
}

// 前缀通配匹配（项目现有语义：sensor/* 匹配 sensor/a 与 sensor/a/b）
bool MatchPattern(const std::string& name, const std::string& pat) {
  if (!pat.empty() && pat.back() == '*') {
    const std::string prefix = pat.substr(0, pat.size() - 1);
    return name.rfind(prefix, 0) == 0;
  }
  return name == pat;
}

// ---------------------------------------------------------------------------
// 计划中的一个话题
// ---------------------------------------------------------------------------
struct PlanItem {
  std::string name;
  std::string type;
  uint32_t topic_id = 0;
  uint64_t total_events = 0;      // 该话题全部条数
  uint64_t total_bytes = 0;       // 该话题全部字节
  uint64_t hit_events = 0;        // 落在删除区间内的条数
  uint64_t hit_bytes_est = 0;     // 估算字节（按平均负载推算）
  bool emptied = false;           // 删后是否为空
};

std::string NowStamp() {
  std::time_t now = std::time(nullptr);
  std::tm tmv{};
  localtime_r(&now, &tmv);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%S", &tmv);
  return buf;
}

// 写审计记录（docs/08 8.3 节）。选择器**原样**记录，便于 SC-12 复盘。
void WriteAudit(const std::string& dir, const std::string& action,
                const std::string& session, const std::string& selector,
                const std::vector<PlanItem>& items, uint64_t events, uint64_t bytes,
                const std::string& operator_name) {
  std::error_code ec;
  const std::string adir = dir + "/.audit";
  fs::create_directories(adir, ec);
  if (ec) return;
  const std::string path = adir + "/" + NowStamp() + "-" + action + ".json";
  std::ofstream o(path, std::ios::out | std::ios::trunc);
  if (!o) return;

  std::string host;
  char hb[256] = {0};
  if (gethostname(hb, sizeof(hb) - 1) == 0) host = hb;

  o << "{\"schema_version\":1,\"action\":\"" << JsonEscape(action) << "\""
    << ",\"session\":\"" << JsonEscape(session) << "\""
    << ",\"selector\":\"" << JsonEscape(selector) << "\""
    << ",\"deleted\":{\"topics\":" << items.size()
    << ",\"events\":" << events << ",\"bytes\":" << bytes << "}"
    << ",\"operator\":\"" << JsonEscape(operator_name) << "\""
    << ",\"host\":\"" << JsonEscape(host) << "\""
    << ",\"at\":\"" << NowStamp() << "\""
    << ",\"stored_at\":\"" << JsonEscape(path) << "\"}\n";
}

}  // namespace

int CmdPrune(const std::vector<std::string>& args, GlobalOptions g) {
  std::string session;
  std::string where_raw;
  std::string max_bytes_raw;
  std::string max_percent_raw;
  std::string quarantine;
  bool yes = false;
  bool dry_run_explicit = false;
  bool i_know = false;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](void) -> const std::string* {
      if (i + 1 >= args.size()) return nullptr;
      return &args[++i];
    };
    if (a == "--yes" || a == "-y") { yes = true; continue; }
    if (a == "--dry-run") { dry_run_explicit = true; continue; }
    if (a == "--i-know-what-i-am-doing") { i_know = true; continue; }
    if (a == "--where") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--where 缺少参数值", "",
                          "示例: --where 'topic = sensor/debug_a and since @session+10s'");
      where_raw = *v; continue;
    }
    if (a == "--max-delete-bytes") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--max-delete-bytes 缺少参数值", "", "示例: 10GB");
      max_bytes_raw = *v; continue;
    }
    if (a == "--max-delete-percent") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--max-delete-percent 缺少参数值", "", "示例: 50%");
      max_percent_raw = *v; continue;
    }
    if (a == "--to-quarantine") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--to-quarantine 缺少参数值", "", "");
      quarantine = *v; continue;
    }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "prune 不支持该选项: " + a,
                  "本版本支持 --where/--yes/--dry-run/--max-delete-bytes/"
                  "--max-delete-percent/--i-know-what-i-am-doing",
                  "完整选项见 docs/08-cli-spec.md 第 6 节");
    }
    if (session.empty()) {
      session = a;
    } else {
      return Fail(kUsage, "prune 只接受一个会话参数，多给了: " + a, "", "");
    }
  }

  // --to-quarantine：诚实说明暂不支持，不假装
  if (!quarantine.empty()) {
    return Fail(kUsage, "--to-quarantine 暂不支持",
                "隔离区需要先把命中数据完整导出到另一位置，再执行删除；"
                "当前 DeleteRange 是直接删除，没有中间态",
                "若需可恢复，请先 `stratum io export` 导出该范围（尚未实现），"
                "或先备份整个会话目录");
  }

  if (session.empty()) {
    return Fail(kUsage, "prune 需要会话路径", "位置参数缺失",
                "例如: stratum prune /data/stratum/s1 --where 'topic = x'");
  }
  // 【保护 1】--where 必填：拒绝无条件全删
  if (where_raw.empty()) {
    return Fail(kUsage, "prune 必须指定 --where",
                "拒绝不带选择器的删除 —— 那等于清空整个会话",
                "示例: --where 'topic = sensor/debug_a'；"
                "确实要清空请直接删除会话目录（更明确）");
  }

  if (!SessionExists(session)) {
    return Fail(kNotFound, "会话不存在: " + session, "路径不存在或不是目录",
                "用 `stratum session list <父目录>` 列出可用会话");
  }

  Selector sel;
  std::string sel_err;
  if (!ParseSelector(where_raw, &sel, &sel_err)) {
    return Fail(kUsage, "选择器无法解析: " + where_raw, sel_err,
                "本版本语法: topic = <名> / topic ~ <前缀*> / since <时刻> / "
                "until <时刻>，用 and 连接");
  }

  TimelineImplV2 timeline;
  Status s = timeline.Open(session);
  if (!s.ok()) {
    return Fail(ExitCodeOf(s), "无法打开会话: " + session, s.ToString(), "");
  }

  Query q(&timeline);
  SessionMetadata meta;
  (void)q.GetInfo(&meta);
  std::vector<TopicSummary> topics;
  s = q.ListTopics(&topics);
  if (!s.ok()) {
    timeline.Close();
    return Fail(ExitCodeOf(s), "列出话题失败", s.ToString(), "");
  }

  // 时间基准（@session 偏移需要）
  uint64_t base_ns = meta.created_at_ns;
  if (base_ns == 0 || base_ns == kInvalidTimestamp) {
    for (const auto& t : topics) {
      if (t.first_ns != kInvalidTimestamp && (base_ns == 0 || t.first_ns < base_ns)) {
        base_ns = t.first_ns;
      }
    }
  }

  TimeRange range;
  range.begin_ns = kInvalidTimestamp;
  range.end_ns = kInvalidTimestamp;
  if (sel.has_since) {
    uint64_t v = 0;
    std::string err;
    if (!ParseTimeValue(sel.since_raw, base_ns, &v, &err)) {
      timeline.Close();
      return Fail(kUsage, "since 无法解析: " + sel.since_raw, err,
                  "支持 @session+60s / -7d / 2026-10-14T10:00:00+08:00");
    }
    range.begin_ns = v;
  }
  if (sel.has_until) {
    uint64_t v = 0;
    std::string err;
    if (!ParseTimeValue(sel.until_raw, base_ns, &v, &err)) {
      timeline.Close();
      return Fail(kUsage, "until 无法解析: " + sel.until_raw, err,
                  "支持 @session+120s / -1h / 2026-10-14T11:00:00+08:00");
    }
    range.end_ns = v;
  }
  if (range.Empty()) {
    timeline.Close();
    return Fail(kUsage, "since 晚于 until",
                "区间为 [" + sel.since_raw + ", " + sel.until_raw + ") 是空集",
                "注意区间是**半开**的：since <= t < until");
  }

  // -------------------------------------------------------------------------
  // 计算计划
  // -------------------------------------------------------------------------
  std::vector<PlanItem> plan;
  uint64_t total_hit_events = 0, total_hit_bytes = 0;
  uint64_t session_events = 0, session_bytes = 0;
  uint64_t first_hit_ns = 0, last_hit_ns = 0;

  for (const auto& t : topics) {
    session_events += t.message_count;
    session_bytes += t.byte_count;

    bool matched = false;
    for (const auto& n : sel.topics) {
      if (n == t.topic_name) { matched = true; break; }
    }
    if (!matched) {
      for (const auto& p : sel.patterns) {
        if (MatchPattern(t.topic_name, p)) { matched = true; break; }
      }
    }
    if (!matched) continue;

    TopicMetadata tm;
    if (!q.FindTopic(t.topic_name, &tm).ok()) continue;

    uint64_t hit = 0;
    Status cs = timeline.CountRange(tm.topic_id, range, &hit);
    if (!cs.ok() || hit == 0) continue;

    PlanItem it;
    it.name = t.topic_name;
    it.type = t.type_name;
    it.topic_id = tm.topic_id;
    it.total_events = t.message_count;
    it.total_bytes = t.byte_count;
    it.hit_events = hit;
    // 【诚实】字节数按平均负载估算：CountRange 只给条数，精确字节需遍历数据。
    // dry-run 的输出里会标注"（估算）"，不谎称精确。
    it.hit_bytes_est = (t.message_count > 0)
                           ? (t.byte_count / t.message_count) * hit
                           : 0;
    it.emptied = (hit >= t.message_count);

    total_hit_events += hit;
    total_hit_bytes += it.hit_bytes_est;
    if (t.first_ns != kInvalidTimestamp &&
        (first_hit_ns == 0 || t.first_ns < first_hit_ns)) {
      first_hit_ns = t.first_ns;
    }
    if (t.last_ns > last_hit_ns) last_hit_ns = t.last_ns;
    plan.push_back(it);
  }

  if (plan.empty()) {
    timeline.Close();
    if (g.format == Format::kJson) {
      std::printf("{\"schema_version\":1,\"session\":\"%s\",\"dry_run\":true,"
                  "\"matched_topics\":0,\"deleted_events\":0}\n",
                  JsonEscape(session).c_str());
    } else if (!g.quiet) {
      std::printf("没有匹配的数据，未做任何修改\n");
      std::printf("  会话:   %s\n", session.c_str());
      std::printf("  选择器: %s\n", where_raw.c_str());
      std::printf("  提示:   用 `stratum query %s --list-topics` 查看可用话题\n",
                  session.c_str());
    }
    return kOk;
  }

  // -------------------------------------------------------------------------
  // 保护检查（2/3）
  // -------------------------------------------------------------------------
  uint64_t max_bytes = 10ULL * 1000 * 1000 * 1000;   // 默认 10GB
  if (!max_bytes_raw.empty() && !ParseSizeBytes(max_bytes_raw, &max_bytes)) {
    timeline.Close();
    return Fail(kUsage, "--max-delete-bytes 无法解析: " + max_bytes_raw,
                "支持 B/KB/MB/GB/TB", "示例: 10GB");
  }
  double max_percent = 50.0;
  if (!max_percent_raw.empty()) {
    max_percent = std::atof(max_percent_raw.c_str());
    if (max_percent <= 0.0 || max_percent > 100.0) {
      timeline.Close();
      return Fail(kUsage, "--max-delete-percent 应在 (0, 100] 之间: " + max_percent_raw,
                  "", "示例: 50%");
    }
  }

  const double percent = (session_bytes > 0)
                             ? (100.0 * static_cast<double>(total_hit_bytes) /
                                static_cast<double>(session_bytes))
                             : 0.0;
  const bool over_bytes = total_hit_bytes > max_bytes;
  const bool over_percent = percent > max_percent;
  const bool protection_ok = (!over_bytes && !over_percent) || i_know;

  // -------------------------------------------------------------------------
  // dry-run（默认）
  // -------------------------------------------------------------------------
  const bool dry_run = (!yes) || dry_run_explicit;

  if (dry_run) {
    if (g.format == Format::kJson) {
      std::printf("{\"schema_version\":1,\"session\":\"%s\",\"dry_run\":true,"
                  "\"selector\":\"%s\",\"matched_topics\":%zu,"
                  "\"deleted_events\":%llu,\"deleted_bytes_est\":%llu,"
                  "\"percent\":%.2f,\"protection_ok\":%s}\n",
                  JsonEscape(session).c_str(), JsonEscape(where_raw).c_str(),
                  plan.size(),
                  static_cast<unsigned long long>(total_hit_events),
                  static_cast<unsigned long long>(total_hit_bytes), percent,
                  protection_ok ? "true" : "false");
      timeline.Close();
      return protection_ok ? kOk : kUsage;
    }

    std::printf("DRY RUN — 未修改任何数据\n");
    std::printf("  会话:      %s\n", session.c_str());
    std::printf("  选择器:    %s\n", where_raw.c_str());
    std::printf("  将删除:\n");
    std::printf("    话题:     %zu 个", plan.size());
    if (plan.size() <= 6) {
      std::printf(" (");
      for (size_t i = 0; i < plan.size(); ++i) {
        std::printf("%s%s", i ? ", " : "", plan[i].name.c_str());
      }
      std::printf(")");
    }
    std::printf("\n");
    std::printf("    条数:     %s\n", HumanCount(total_hit_events).c_str());
    std::printf("    字节:     %s（估算，占会话 %.2f%%）\n",
                HumanBytes(total_hit_bytes).c_str(), percent);
    if (first_hit_ns != 0) {
      std::printf("    时间范围: %s ~ %s\n", FormatRfc3339(first_hit_ns).c_str(),
                  FormatRfc3339(last_hit_ns).c_str());
    }
    std::printf("  逐话题:\n");
    for (const auto& it : plan) {
      std::printf("    %-28s %10s 条 / %10s%s%s\n", it.name.c_str(),
                  HumanCount(it.hit_events).c_str(),
                  HumanBytes(it.hit_bytes_est).c_str(),
                  it.emptied ? "  ← 将删空" : "",
                  it.emptied ? "（元数据保留，标记为空）" : "");
    }
    std::printf("  保护检查:\n");
    std::printf("    --max-delete-bytes   %-8s %s\n", HumanBytes(max_bytes).c_str(),
                over_bytes ? "**超出 → 拒绝**" : "通过");
    std::printf("    --max-delete-percent %.1f%%     %s\n", max_percent,
                over_percent ? "**超出 → 拒绝**" : "通过");
    if (!protection_ok) {
      std::printf("    ⚠ 保护未通过，真删会被拒绝。确认无误可加 "
                  "--i-know-what-i-am-doing\n");
    }
    std::printf("  元数据:    保留（删空的话题将标记为空，符合 FR-R-11 自描述）\n");
    std::printf("  审计:      将写入 %s/.audit/<时间戳>-prune.json\n", session.c_str());
    std::printf("  空洞标记:  已记录的区间将被 verify 视为「主动裁剪」而非数据缺口\n");
    if (!yes) {
      std::printf("\n  执行请加 --yes\n");
    } else {
      std::printf("\n  （--dry-run 优先于 --yes，未执行删除）\n");
    }
    timeline.Close();
    return protection_ok ? kOk : kUsage;
  }

  // -------------------------------------------------------------------------
  // 真删
  // -------------------------------------------------------------------------
  if (!protection_ok) {
    timeline.Close();
    if (over_bytes) {
      return Fail(kUsage, "删除量超出 --max-delete-bytes 上限",
                  "将删除 " + HumanBytes(total_hit_bytes) + "，上限 " +
                      HumanBytes(max_bytes),
                  "确认无误请加 --i-know-what-i-am-doing，或先用 --dry-run 核对选择器");
    }
    return Fail(kUsage, "删除占比超出 --max-delete-percent 上限",
                "将删除 " + std::to_string(percent).substr(0, 5) + "%，上限 " +
                    std::to_string(max_percent).substr(0, 5) + "%",
                "确认无误请加 --i-know-what-i-am-doing，或先用 --dry-run 核对选择器");
  }

  uint64_t actual_events = 0, actual_bytes = 0;
  int failed = 0;
  for (const auto& it : plan) {
    uint64_t de = 0, db = 0;
    Status ds = timeline.DeleteRange(it.topic_id, range, &de, &db);
    if (!ds.ok()) {
      ++failed;
      std::fprintf(stderr, "  删除失败 %s: %s\n", it.name.c_str(), ds.ToString().c_str());
      continue;
    }
    actual_events += de;
    actual_bytes += db;
    LogVerbose(g, 1, "已删除 " + it.name + "：" + std::to_string(de) + " 条");
  }

  // 落盘 + 审计
  (void)timeline.Flush();
  WriteAudit(session, "prune", session, where_raw, plan, actual_events, actual_bytes,
             "cli");
  timeline.Close();

  if (g.format == Format::kJson) {
    std::printf("{\"schema_version\":1,\"session\":\"%s\",\"dry_run\":false,"
                "\"selector\":\"%s\",\"topics\":%zu,\"deleted_events\":%llu,"
                "\"deleted_bytes\":%llu,\"failed\":%d}\n",
                JsonEscape(session).c_str(), JsonEscape(where_raw).c_str(),
                plan.size(), static_cast<unsigned long long>(actual_events),
                static_cast<unsigned long long>(actual_bytes), failed);
  } else if (!g.quiet) {
    std::fprintf(stderr, "\n删除完成\n");
    std::fprintf(stderr, "  话题:    %zu 个\n", plan.size());
    std::fprintf(stderr, "  条数:    %s\n", HumanCount(actual_events).c_str());
    std::fprintf(stderr, "  字节:    %s\n", HumanBytes(actual_bytes).c_str());
    if (failed > 0) std::fprintf(stderr, "  失败:    %d 个话题\n", failed);
    std::fprintf(stderr, "\n  提示: 被裁剪的区间已记录，verify 会视为「主动裁剪」而非缺口\n");
  }
  return failed > 0 ? kPartial : kOk;
}

}  // namespace cli
}  // namespace stratum
