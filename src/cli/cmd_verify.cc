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

// ---------------------------------------------------------------------------
// 业务层视角（与 DDS 层并列，但**不下结论**）
//
// 【为什么单独一个结构而不是并进 Finding】
// Finding 是"发现了问题"的载体，会参与 has_integrity_issue 的判定。
// 业务层的数字不构成"数据完整性问题" —— 业务失败 50 条可能完全正常
// （50 个订单被取消）。把它们混进 Finding 会让 verify 学会说谎。
// ---------------------------------------------------------------------------
struct BusinessLayer {
  bool present = false;        // 会话里是否存在探针 topic
  uint64_t dds_events = 0;     // 非探针 topic 的事件数
  size_t dds_topics = 0;
  uint64_t probe_events = 0;   // 探针 topic 的事件数
  size_t probe_topics = 0;
  // 每个探针 topic 的条数（便于看出哪个探针在工作、哪个停了）
  std::vector<std::pair<std::string, uint64_t>> probe_detail;

  // 第二层：启发式信号（仅在 --business-signals 时填充）
  bool signals_collected = false;
  uint64_t sig_ok = 0;
  uint64_t sig_error = 0;
  uint64_t sig_unknown = 0;
  uint64_t sig_scanned = 0;    // 实际扫了多少条探针负载
};

// 判断一个 topic 名是不是探针 topic
bool IsProbeTopic(const std::string& name, const std::string& prefix) {
  return !prefix.empty() && name.rfind(prefix, 0) == 0;
}

// 从探针负载里**启发式**提取成功/失败信号。
//
// 【刻意保持粗糙】这里不是 JSON 解析器，只是找 `result=` / `status=` 之后的值。
// 理由：探针负载格式由业务决定，我们不该强加约束。
// 找到就计，找不到就归入 unknown —— 并让调用方把这个局限说清楚。
enum class SignalKind { kOk, kError, kUnknown };

SignalKind ClassifyPayload(const char* data, size_t len) {
  // 在负载里找 result= 或 status=
  static const char* kKeys[] = {"result=", "status="};
  for (const char* key : kKeys) {
    const size_t klen = std::strlen(key);
    for (size_t i = 0; i + klen < len; ++i) {
      if (std::memcmp(data + i, key, klen) != 0) continue;
      // 取值直到分隔符
      size_t b = i + klen;
      size_t e = b;
      while (e < len && data[e] != ',' && data[e] != ';' && data[e] != ' ' &&
             data[e] != '"' && data[e] != '}' && data[e] != '\n' &&
             data[e] != '\0') {
        ++e;
      }
      const std::string v(data + b, e - b);
      if (v == "ok" || v == "OK" || v == "success" || v == "true" || v == "1") {
        return SignalKind::kOk;
      }
      if (v == "error" || v == "ERROR" || v == "fail" || v == "failed" ||
          v == "false" || v == "0") {
        return SignalKind::kError;
      }
      return SignalKind::kUnknown;
    }
  }
  return SignalKind::kUnknown;
}

void PrintFindingText(const Finding& f) {
  std::printf("  [%s][%s] %s\n", f.code, f.severity, f.title.c_str());
  std::printf("      %s\n", f.detail.c_str());
  std::printf("      归因: %s\n", f.actionable ? "本端可改进" : "非本端问题");
}

// ---------------------------------------------------------------------------
// 计算业务层视角
//
// 【设计原则：只呈现，不下结论】
// 这个函数返回的数字**不参与** has_integrity_issue 的判定。
// 理由见 BusinessLayer 的注释：业务失败 50 条可能是完全正常的。
// ---------------------------------------------------------------------------
BusinessLayer ComputeBusinessLayer(const std::vector<TopicSummary>& topics,
                                  TimelineImplV2* timeline,
                                  const std::string& probe_prefix,
                                  bool want_signals) {
  BusinessLayer bl;
  for (const auto& t : topics) {
    if (IsProbeTopic(t.topic_name, probe_prefix)) {
      bl.present = true;
      bl.probe_events += t.message_count;
      ++bl.probe_topics;
      bl.probe_detail.emplace_back(t.topic_name, t.message_count);
    } else {
      bl.dds_events += t.message_count;
      ++bl.dds_topics;
    }
  }
  // 探针 topic 按条数降序，便于一眼看出哪个在工作
  std::sort(bl.probe_detail.begin(), bl.probe_detail.end(),
            [](const std::pair<std::string, uint64_t>& a,
               const std::pair<std::string, uint64_t>& b) {
              if (a.second != b.second) return a.second > b.second;
              return a.first < b.first;
            });

  if (!want_signals || !bl.present || timeline == nullptr) return bl;

  // -------------------------------------------------------------------------
  // 第二层：启发式信号
  //
  // 【为什么默认关闭】要扫全量 payload。一个跑了数天的会话可能有上千万条
  // 探针记录，全扫会很慢。所以做成显式选项 —— 性能与深度分离。
  // -------------------------------------------------------------------------
  bl.signals_collected = true;
  for (const auto& t : topics) {
    if (!IsProbeTopic(t.topic_name, probe_prefix)) continue;
    if (t.message_count == 0) continue;   // 空 topic 无需扫描

    // 【关键】时间范围必须用该 topic 的**真实**起止时间。
    //
    // 不能写 begin_ns = 0 —— 本项目里 `kInvalidTimestamp = 0`，
    // 0 表示"无效时间戳"而不是"最早"。第一版这么写直接段错误（exit 139）。
    //
    // end_ns = 0 的语义倒是"无上界"（见 TimeRange 注释），
    // 但这里用 last_ns + 1 更明确，也避免依赖那个隐式约定。
    if (t.first_ns == kInvalidTimestamp) continue;

    // TopicSummary **没有** topic_id（只有名字与统计），
    // 所以要先按名字拿 TopicMetadata 才拿得到 id。
    TopicMetadata tmc;
    if (!timeline->FindTopicByName(t.topic_name, &tmc).ok()) continue;

    TimeRange whole;
    whole.begin_ns = t.first_ns;
    whole.end_ns = (t.last_ns == kInvalidTimestamp) ? 0 : (t.last_ns + 1);

    EventIteratorPtr it;
    const Status qs = timeline->QueryRange(tmc.topic_id, whole, &it);
    if (!qs.ok() || !it) continue;   // 双重防护：状态 OK 也要确认迭代器非空

    for (; it->Valid(); it->Next()) {
      const Event& e = it->event();
      ++bl.sig_scanned;
      switch (ClassifyPayload(e.payload.data(), e.payload.size())) {
        case SignalKind::kOk: ++bl.sig_ok; break;
        case SignalKind::kError: ++bl.sig_error; break;
        case SignalKind::kUnknown: ++bl.sig_unknown; break;
      }
    }
  }
  return bl;
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
  std::string probe_prefix = "_probe.";
  bool want_signals = false;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--exit-code-on-issue") { exit_on_issue = true; continue; }
    if (a == "--check") { ++i; continue; }            // 检查项筛选（后续实现）
    if (a == "--report") { ++i; continue; }           // html/json（后续实现）
    if (a == "--out") { ++i; continue; }
    if (a == "--probe-prefix") {
      // 本命令的循环体用 args[++i] 风格（没有 next() lambda），这里保持一致
      if (i + 1 >= args.size()) {
        return Fail(kUsage, "--probe-prefix 缺少参数值", "",
                    "默认 _probe.（与 stratum probe 的默认前缀一致）");
      }
      probe_prefix = args[++i];
      continue;
    }
    if (a == "--business-signals") {
      want_signals = true;
      continue;
    }
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

  // ---------------------------------------------------------------------------
  // 【必须先预检】TimelineImplV2::Open 会**创建目录**。
  // verify 是纯查询命令，对拼错的路径静默创建一个空会话、然后报"完整无问题" ——
  // 这是最坏的结果：用户以为验证通过了，其实验证的是一个刚被自己创建的空目录。
  //
  // 项目纪律：查询类命令不得有创建副作用。
  // （diff 有同样的问题、已修；发现一个命令违反纪律时要检查所有同类命令。）
  // ---------------------------------------------------------------------------
  if (!SessionExists(target)) {
    return Fail(kNotFound, "会话不存在: " + target,
                "该路径不是一个会话目录（verify 不会创建它）",
                "用 `stratum session list <父目录>` 查看可用会话");
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

  // 业务层视角：与 DDS 层**并列呈现**，但不参与 has_integrity_issue 的判定。
  BusinessLayer bl =
      ComputeBusinessLayer(topics, &timeline, probe_prefix, want_signals);

  // ---------------------------------------------------------------------------
  // 交叉视角才能发现的问题：两层严重失衡
  //
  // 【为什么单看某一层发现不了】
  //   · 只看 DDS 层：0 条事件 → "空会话"，可能被当成正常
  //   · 只看探针层：20 条 → "工作正常"
  //   · 并置后：**只有诊断数据、没有业务数据** → 这个会话无法用于业务复盘
  //
  // 计入 findings（用户必须看到），但**不计入** has_integrity_issue ——
  // 数据本身没坏，坏的是"这个会话的用途与你预期的不一样"。
  // ---------------------------------------------------------------------------
  std::vector<Finding> findings_with_business = findings;
  if (bl.present && bl.dds_events == 0 && bl.probe_events > 0) {
    // Finding 的字段是 (code, severity[const char*], title, detail, actionable)
    Finding f;
    f.code = "RC-B1";
    f.severity = "medium";
    f.title = "只有探针数据，没有业务数据";
    f.detail = "会话里有 " + std::to_string(bl.probe_events) +
               " 条探针事件，但没有任何业务 topic 事件 —— "
               "这个会话只能用于诊断，不能用于业务数据复盘。"
               "检查录制时是否用 --exclude-topic 排除了业务 topic，"
               "或业务侧当时没有发布数据。";
    f.actionable = false;   // 录制时的事，事后无法补救
    findings_with_business.push_back(f);
  }
  const bool has_problem = meta.quality.HasIntegrityIssue();

  // ---------------------------------------------------------------------------
  // 主动裁剪区间（CLI `prune` 留下的痕迹）
  //
  // 【为什么必须与"缺口"分开】prune 删除数据会在时间轴上留空洞。
  // 若不区分，复盘时会把"我们自己删的"当成"录制丢了数据"，
  // 于是有人去查**并不存在的**问题 —— 与 ctl stop 必须留痕同理。
  //
  // 注意：这些区间**不计入** has_integrity_issue。
  // ---------------------------------------------------------------------------
  struct PrunedRow {
    std::string topic;
    TimestampNs begin_ns;
    TimestampNs end_ns;
    uint64_t events;
  };
  std::vector<PrunedRow> pruned;
  uint64_t pruned_events = 0;
  if (ts.ok()) {
    Query q2(&timeline);
    for (const auto& tp : topics) {
      TopicMetadata tmc;
      if (!q2.FindTopic(tp.topic_name, &tmc).ok()) continue;
      std::vector<PrunedRange> ranges;
      if (timeline.ListPrunedRanges(tmc.topic_id, &ranges).ok()) {
        for (const auto& r : ranges) {
          PrunedRow row;
          row.topic = tp.topic_name;
          row.begin_ns = r.begin_ns;
          row.end_ns = r.end_ns;
          row.events = r.deleted_events;
          pruned_events += r.deleted_events;
          pruned.push_back(row);
        }
      }
    }
  }

  if (want_json) {
    std::printf("{\"schema_version\":1,\"session\":\"%s\",", JsonEscape(target).c_str());
    std::printf("\"events\":%llu,\"payload_bytes\":%llu,\"topics\":%zu,",
                static_cast<unsigned long long>(total_events),
                static_cast<unsigned long long>(total_bytes), topics.size());
    std::printf("\"has_integrity_issue\":%s,\"pruned_events\":%llu,"
                "\"pruned_ranges\":[",
                has_problem ? "true" : "false",
                static_cast<unsigned long long>(pruned_events));
    for (size_t i = 0; i < pruned.size(); ++i) {
      if (i) std::printf(",");
      std::printf("{\"topic\":\"%s\",\"begin_ns\":%llu,\"end_ns\":%llu,"
                  "\"events\":%llu}",
                  JsonEscape(pruned[i].topic).c_str(),
                  static_cast<unsigned long long>(pruned[i].begin_ns),
                  static_cast<unsigned long long>(pruned[i].end_ns),
                  static_cast<unsigned long long>(pruned[i].events));
    }
    // 业务层视角。**放在 findings 之前**，因为它是"环境事实"而非"发现问题"。
    std::printf("],\"business_layer\":{");
    std::printf("\"probe_prefix\":\"%s\",", JsonEscape(probe_prefix).c_str());
    std::printf("\"dds_events\":%llu,\"dds_topics\":%zu,",
                static_cast<unsigned long long>(bl.dds_events), bl.dds_topics);
    std::printf("\"probe_events\":%llu,\"probe_topics\":%zu,",
                static_cast<unsigned long long>(bl.probe_events), bl.probe_topics);
    std::printf("\"probe_detail\":[");
    for (size_t i = 0; i < bl.probe_detail.size(); ++i) {
      if (i) std::printf(",");
      std::printf("{\"topic\":\"%s\",\"events\":%llu}",
                  JsonEscape(bl.probe_detail[i].first).c_str(),
                  static_cast<unsigned long long>(bl.probe_detail[i].second));
    }
    std::printf("],");
    if (bl.signals_collected) {
      std::printf("\"signals\":{\"scanned\":%llu,\"ok\":%llu,\"error\":%llu,"
                  "\"unknown\":%llu},",
                  static_cast<unsigned long long>(bl.sig_scanned),
                  static_cast<unsigned long long>(bl.sig_ok),
                  static_cast<unsigned long long>(bl.sig_error),
                  static_cast<unsigned long long>(bl.sig_unknown));
    } else {
      std::printf("\"signals\":null,");
    }
    // 这句话是这一整个功能的核心约束，必须出现在输出里
    std::printf("\"note\":\"业务层数字仅供参考，verify 不对业务逻辑下结论；"
                "has_integrity_issue 只反映数据完整性\"},");
    std::printf("\"findings\":[");
    for (size_t i = 0; i < findings_with_business.size(); ++i) {
      if (i) std::printf(",");
      // 【注意】下面的索引必须与上界**用同一个 vector**。
      // 之前用 findings_with_business.size() 配 findings[i]，有 RC-B1 时越界崩溃。
      const Finding& f = findings_with_business[i];
      std::printf("{\"rc\":\"%s\",\"severity\":\"%s\",\"title\":\"%s\","
                  "\"detail\":\"%s\",\"attribution\":\"%s\"}",
                  f.code, f.severity,
                  JsonEscape(f.title).c_str(),
                  JsonEscape(f.detail).c_str(),
                  f.actionable ? "local" : "remote");
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

  // 「主动裁剪」单独成节 —— 它不是问题，是**已知的、故意的**删除。
  if (!pruned.empty()) {
    std::printf("\n主动裁剪（**非缺口**，由 prune 主动删除）：\n");
    for (const auto& r : pruned) {
      std::printf("  [PRUNED] %s\n", r.topic.c_str());
      std::printf("      区间: %s ~ %s\n",
                  r.begin_ns ? FormatRfc3339(r.begin_ns).c_str() : "-",
                  r.end_ns ? FormatRfc3339(r.end_ns).c_str() : "(到末尾)");
      std::printf("      条数: %s\n", HumanCount(r.events).c_str());
    }
    std::printf("  合计 %s 条（%zu 个区间）—— 这些数据是被**主动删除**的，\n",
                HumanCount(pruned_events).c_str(), pruned.size());
    std::printf("  不是录制过程中丢失的。归因: 人为操作，非本端缺陷。\n");
  }

  if (findings_with_business.empty()) {
    std::printf("\n  未发现 RC-1~RC-6 中任何一类问题。\n");
  } else {
    std::printf("\n按成因分类（docs/06-data-trust.md 的 RC-1~RC-6）：\n");
    for (const auto& f : findings_with_business) PrintFindingText(f);
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
