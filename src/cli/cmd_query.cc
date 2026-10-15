// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_query.cc
//
// `stratum query` —— 把库里已有的查询能力暴露给命令行。
//
// 设计依据：docs/07-cli-design.md 场景 S13/S14、docs/08-cli-spec.md 第 5 节。
//
// 【两个关键裁决，都有依据】
//
// 1. 定点查询必须区分 kNotFound / kDisposed / kOutOfRange（docs/08 第 5 节）。
//    合并成"没有数据"会让 SC-02（从故障点往前追）的结论不可靠：
//      · not_found    = 那一刻之前确实没数据（发布者还没开始）
//      · disposed     = 那一刻之前被显式注销了
//      · out_of_range = **查询本身问错了范围**（不是数据的问题）
//    三者对复盘的含义完全不同。
//
// 2. 默认不打印 payload 内容。
//    payload 是"存储层不理解其内容"的原始字节，直接打到终端既无意义又可能
//    破坏终端。默认只给长度，需要时用 --with-payload 输出 hex。

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cli_common.h"
#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"
#include "stratum/types/json_serializer.h"
#include "stratum/types/iserializer.h"

using namespace stratum;

namespace stratum {
namespace cli {

namespace {

// ---------------------------------------------------------------------------
// 选项
// ---------------------------------------------------------------------------
struct QueryOpts {
  std::string session;
  bool list_topics = false;
  std::string topic;
  bool all_topics = false;
  std::string since_raw;
  std::string until_raw;
  std::string state_at_raw;
  bool with_payload = false;
  // 类型解码（可选）。不加时行为与以前完全一致。
  bool decode = false;
  std::string type_format = "json";   // 当前只实现了 json
  uint64_t limit = 0;    // 0 = 不限
  uint64_t offset = 0;
  bool stream = false;
  std::string out_path;
};

std::string HexPrefix(const std::string& s, size_t max_bytes) {
  static const char* d = "0123456789abcdef";
  const size_t n = std::min(max_bytes, s.size());
  std::string o;
  o.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    o += d[c >> 4];
    o += d[c & 0xF];
  }
  if (s.size() > n) o += "...";
  return o;
}

const char* StateKindName(StateAtResult::Kind k) {
  switch (k) {
    case StateAtResult::Kind::kFound: return "found";
    case StateAtResult::Kind::kNotFound: return "not_found";
    case StateAtResult::Kind::kDisposed: return "disposed";
    case StateAtResult::Kind::kOutOfRange: return "out_of_range";
  }
  return "?";
}

// 输出目标（--out 时写文件，否则 stdout）
class Sink {
 public:
  explicit Sink(const std::string& path) : path_(path) {
    if (!path.empty() && path != "-") {
      file_.open(path, std::ios::out | std::ios::trunc);
      ok_ = file_.is_open();
    }
  }
  bool ok() const { return path_.empty() || path_ == "-" || ok_; }
  void Print(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (file_.is_open()) {
      file_ << buf;
    } else {
      std::fputs(buf, stdout);
    }
  }
  void Flush() { if (file_.is_open()) file_.flush(); }

 private:
  std::string path_;
  std::ofstream file_;
  bool ok_ = true;
};

// ---------------------------------------------------------------------------
// 模式 1：列出话题
// ---------------------------------------------------------------------------
int DoListTopics(TimelineImplV2& tl, const SessionMetadata& meta, uint64_t base_ns,
                 const std::vector<TopicSummary>& topics, const QueryOpts& o,
                 const GlobalOptions& g) {
  Sink out(o.out_path);
  if (!out.ok()) {
    return Fail(kEnv, "无法写入输出文件: " + o.out_path, "打开文件失败",
                "检查目录是否存在、是否有写权限");
  }

  if (g.format == Format::kJson) {
    out.Print("{\"schema_version\":1,\"session\":\"%s\",\"topics\":[",
              JsonEscape(o.session).c_str());
    for (size_t i = 0; i < topics.size(); ++i) {
      if (i) out.Print(",");
      out.Print("{\"name\":\"%s\",\"type\":\"%s\",\"count\":%llu,\"bytes\":%llu,"
                "\"first_ns\":%llu,\"last_ns\":%llu,\"active\":%s}",
                JsonEscape(topics[i].topic_name).c_str(),
                JsonEscape(topics[i].type_name).c_str(),
                static_cast<unsigned long long>(topics[i].message_count),
                static_cast<unsigned long long>(topics[i].byte_count),
                static_cast<unsigned long long>(topics[i].first_ns),
                static_cast<unsigned long long>(topics[i].last_ns),
                topics[i].active ? "true" : "false");
    }
    out.Print("]}\n");
    out.Flush();
    return kOk;
  }

  if (g.format == Format::kCsv) {
    out.Print("topic,type,count,bytes,first_ns,last_ns,active\n");
    for (const auto& t : topics) {
      out.Print("%s,%s,%llu,%llu,%llu,%llu,%d\n", t.topic_name.c_str(),
                t.type_name.c_str(),
                static_cast<unsigned long long>(t.message_count),
                static_cast<unsigned long long>(t.byte_count),
                static_cast<unsigned long long>(t.first_ns),
                static_cast<unsigned long long>(t.last_ns), t.active ? 1 : 0);
    }
    out.Flush();
    return kOk;
  }

  if (g.ShowHeaders()) {
    std::printf("会话: %s\n", o.session.c_str());
    if (meta.created_at_ns != kInvalidTimestamp) {
      std::printf("创建: %s\n", FormatRfc3339(meta.created_at_ns).c_str());
    }
    std::printf("话题 %zu 个\n\n", topics.size());
  }
  PrintTableHeader({{"话题", 32}, {"类型", 20}, {"条数", 12}, {"字节", 12}, {"起始偏移", 12}}, g);
  for (const auto& t : topics) {
    out.Print("%-32s  %-20s %-12s %-12s %s\n", t.topic_name.c_str(),
              t.type_name.c_str(), HumanCount(t.message_count).c_str(),
              HumanBytes(t.byte_count).c_str(),
              (base_ns && t.first_ns != kInvalidTimestamp)
                  ? FormatOffsetNs(base_ns, t.first_ns).c_str()
                  : "-");
  }
  out.Flush();
  return kOk;
}

// 把"是否成功解码"附加到事件 JSON 上。
//
// 三种情形必须可区分：
//   · 未启用 --decode      -> 不加任何字段（保持既有输出不变）
//   · 启用但无类型定义     -> decoded:false + reason:"no_type_definition"
//   · 启用且解码失败       -> decoded:false + reason:"decode_failed"
//   · 解码成功             -> decoded:true  + value:{...}
void AppendDecodedJson(Sink* out, const QueryOpts& o,
                       const types::SerializerPtr& decoder, bool can_decode,
                       const std::string& payload) {
  if (!o.decode) return;
  if (!can_decode || decoder == nullptr || !decoder->supports_deserialization()) {
    out->Print(",\"decoded\":false,\"reason\":\"no_type_definition\"");
    return;
  }
  types::DynamicValue dv;
  Status ds = decoder->Deserialize(payload, &dv);
  if (!ds.ok()) {
    out->Print(",\"decoded\":false,\"reason\":\"decode_failed\"");
    return;
  }
  out->Print(",\"decoded\":true,\"value\":%s", dv.ToString().c_str());
}


// ---------------------------------------------------------------------------
// 模式 2：按时间范围导出
// ---------------------------------------------------------------------------
int DoExport(TimelineImplV2& tl, const SessionMetadata& meta, uint64_t base_ns,
             const QueryOpts& o, const GlobalOptions& g) {
  TopicMetadata tm;
  Status s = Query(&tl).FindTopic(o.topic, &tm);
  if (!s.ok()) {
    return Fail(kNotFound, "找不到话题: " + o.topic, s.ToString(),
                "用 `stratum query " + o.session + " --list-topics` 查看可用话题");
  }

  // -------------------------------------------------------------------------
  // 类型解码器（仅在 --decode 时创建）
  //
  // 现实：record 会写 type_definition（recorder.cc 从 ReceivedSample 取），
  // 但该字段**常为空** —— DDS 的 SEDP 不保证携带类型定义（见 Q11）。
  // 因此"解不出来"是常态而不是异常，必须在输出里**如实标注**，
  // 否则使用者会以为"这条数据里没有内容"。
  // -------------------------------------------------------------------------
  types::SerializerPtr decoder;
  bool can_decode = false;
  if (o.decode) {
    types::TypeDescriptor td;
    td.name = tm.type_name;
    td.definition = tm.type_definition;
    td.definition_format = tm.type_definition.empty() ? std::string() : o.type_format;
    can_decode = td.HasDefinition();
    decoder = types::CreateSerializer(td);
    LogVerbose(g, 1, std::string("类型解码：") +
                          (can_decode ? "有类型定义，尝试解码"
                                      : "无类型定义（降级为原始字节）") +
                          "，type=" + tm.type_name);
  }

  TimeRange range;
  range.begin_ns = kInvalidTimestamp;
  range.end_ns = kInvalidTimestamp;

  if (!o.since_raw.empty()) {
    uint64_t v = 0;
    std::string err;
    if (!ParseTimeValue(o.since_raw, base_ns, &v, &err)) {
      return Fail(kUsage, "--since 无法解析: " + o.since_raw, err,
                  "支持 @session+5s / -30m / 2026-10-14T10:00:00+08:00");
    }
    range.begin_ns = v;
  }
  if (!o.until_raw.empty()) {
    uint64_t v = 0;
    std::string err;
    if (!ParseTimeValue(o.until_raw, base_ns, &v, &err)) {
      return Fail(kUsage, "--until 无法解析: " + o.until_raw, err,
                  "支持 @session+5s / -30m / 2026-10-14T10:00:00+08:00");
    }
    range.end_ns = v;
  }
  if (range.Empty()) {
    return Fail(kUsage, "--since 晚于 --until",
                "区间为 [" + o.since_raw + ", " + o.until_raw + ") 是空集",
                "注意区间是**半开**的：since <= t < until");
  }

  std::vector<Event> events;
  s = Query(&tl).ExportRange(tm.topic_id, range, &events);
  if (!s.ok()) {
    return Fail(ExitCodeOf(s), "读取区间失败", s.ToString(),
                "若区间超出录制范围，请先用 --list-topics 查看实际跨度");
  }

  // 分页。注意：--limit 先于 --stream 生效（限制总数，而不是限制缓冲）
  const uint64_t total = events.size();
  uint64_t begin_i = std::min<uint64_t>(o.offset, total);
  uint64_t end_i = total;
  if (o.limit > 0) {
    end_i = std::min<uint64_t>(begin_i + o.limit, total);
  }

  Sink out(o.out_path);
  if (!out.ok()) {
    return Fail(kEnv, "无法写入输出文件: " + o.out_path, "打开文件失败", "");
  }

  LogVerbose(g, 1, "导出话题 " + o.topic + "（topic_id=" +
                       std::to_string(tm.topic_id) + "）共 " +
                       std::to_string(total) + " 条，输出 " +
                       std::to_string(end_i - begin_i) + " 条");

  if (g.format == Format::kCsv) {
    out.Print("topic,seq,seq_in_ts,source_ts_ns,reception_ts_ns,payload_bytes%s\n",
              o.with_payload ? ",payload_hex" : "");
    for (uint64_t i = begin_i; i < end_i; ++i) {
      const Event& e = events[i];
      out.Print("%s,%u,%u,%llu,%llu,%zu",
                tm.topic_name.c_str(), e.seq, e.seq_in_ts,
                static_cast<unsigned long long>(e.source_timestamp_ns),
                static_cast<unsigned long long>(e.reception_timestamp_ns),
                e.payload.size());
      if (o.with_payload) out.Print(",%s", HexPrefix(e.payload, 64).c_str());
      out.Print("\n");
    }
    out.Flush();
    return kOk;
  }

  if (g.format == Format::kJson) {
    if (o.stream) {
      // 流式：JSON Lines，每行一条，避免大结果把内存吃满
      for (uint64_t i = begin_i; i < end_i; ++i) {
        const Event& e = events[i];
        out.Print("{\"topic\":\"%s\",\"seq\":%u,\"seq_in_ts\":%u,"
                  "\"source_ts_ns\":%llu,\"reception_ts_ns\":%llu,\"bytes\":%zu",
                  JsonEscape(tm.topic_name).c_str(), e.seq, e.seq_in_ts,
                  static_cast<unsigned long long>(e.source_timestamp_ns),
                  static_cast<unsigned long long>(e.reception_timestamp_ns),
                  e.payload.size());
        if (!e.instance_key.empty()) {
          out.Print(",\"instance_key\":\"%s\"", JsonEscape(e.instance_key).c_str());
        }
        if (o.with_payload) {
          out.Print(",\"payload_hex\":\"%s\"", HexPrefix(e.payload, 64).c_str());
        }
        AppendDecodedJson(&out, o, decoder, can_decode, e.payload);
        out.Print("}\n");
      }
    } else {
      out.Print("{\"schema_version\":1,\"session\":\"%s\",\"topic\":\"%s\","
                "\"total\":%llu,\"returned\":%llu,\"events\":[",
                JsonEscape(o.session).c_str(), JsonEscape(tm.topic_name).c_str(),
                static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(end_i - begin_i));
      for (uint64_t i = begin_i; i < end_i; ++i) {
        const Event& e = events[i];
        if (i > begin_i) out.Print(",");
        out.Print("{\"seq\":%u,\"seq_in_ts\":%u,\"source_ts_ns\":%llu,"
                  "\"reception_ts_ns\":%llu,\"bytes\":%zu",
                  e.seq, e.seq_in_ts,
                  static_cast<unsigned long long>(e.source_timestamp_ns),
                  static_cast<unsigned long long>(e.reception_timestamp_ns),
                  e.payload.size());
        if (!e.instance_key.empty()) {
          out.Print(",\"instance_key\":\"%s\"", JsonEscape(e.instance_key).c_str());
        }
        if (o.with_payload) {
          out.Print(",\"payload_hex\":\"%s\"", HexPrefix(e.payload, 64).c_str());
        }
        AppendDecodedJson(&out, o, decoder, can_decode, e.payload);
        out.Print("}");
      }
      out.Print("]}\n");
    }
    out.Flush();
    return kOk;
  }

  // table
  if (g.ShowHeaders()) {
    std::printf("话题: %s（%s）\n", tm.topic_name.c_str(), tm.type_name.c_str());
    std::printf("命中 %s 条，输出 %llu 条\n\n", HumanCount(total).c_str(),
                static_cast<unsigned long long>(end_i - begin_i));
  }
  PrintTableHeader({{"#", 8}, {"seq", 10}, {"源时间(相对)", 16},
                    {"接收偏移", 14}, {"字节", 10}, {"实例key", 12}}, g);
  for (uint64_t i = begin_i; i < end_i; ++i) {
    const Event& e = events[i];
    out.Print("%-8llu  %-10u %-16s %-14s %-10zu %s\n",
              static_cast<unsigned long long>(i), e.seq,
              (base_ns && e.source_timestamp_ns != kInvalidTimestamp)
                  ? FormatOffsetNs(base_ns, e.source_timestamp_ns).c_str()
                  : "-",
              (base_ns && e.reception_timestamp_ns != kInvalidTimestamp)
                  ? FormatOffsetNs(base_ns, e.reception_timestamp_ns).c_str()
                  : "-",
              e.payload.size(), e.instance_key.c_str());
  }
  out.Flush();
  return kOk;
}

// ---------------------------------------------------------------------------
// 模式 3：定点状态（Stratum 的差异化能力）
// ---------------------------------------------------------------------------
int DoStateAt(TimelineImplV2& tl, const SessionMetadata& meta, uint64_t base_ns,
              const std::vector<TopicSummary>& topics, const QueryOpts& o,
              const GlobalOptions& g) {
  uint64_t as_of = 0;
  std::string err;
  if (!ParseTimeValue(o.state_at_raw, base_ns, &as_of, &err)) {
    return Fail(kUsage, "--state-at 无法解析: " + o.state_at_raw, err,
                "支持 @session+5s / 2026-10-14T10:05:00+08:00 / Unix 纳秒");
  }

  // 决定要查询哪些话题
  std::vector<std::string> names;
  if (!o.topic.empty()) {
    names.push_back(o.topic);
  } else {
    for (const auto& t : topics) names.push_back(t.topic_name);
  }
  if (names.empty()) {
    return Fail(kNotFound, "会话里没有任何话题", o.session,
                "用 `stratum query " + o.session + " --list-topics` 确认");
  }

  Query q(&tl);
  Sink out(o.out_path);
  if (!out.ok()) {
    return Fail(kEnv, "无法写入输出文件: " + o.out_path, "打开文件失败", "");
  }

  struct Row {
    std::string name;
    StateAtResult r;
    std::string err;
  };
  std::vector<Row> rows;
  int out_of_range_count = 0;
  for (const auto& n : names) {
    TopicMetadata tm;
    Status s = q.FindTopic(n, &tm);
    Row row;
    row.name = n;
    if (!s.ok()) {
      row.err = s.ToString();
      row.r.kind = StateAtResult::Kind::kNotFound;
      rows.push_back(row);
      continue;
    }
    s = q.GetStateAt(tm.topic_id, as_of, &row.r);
    if (!s.ok()) row.err = s.ToString();
    if (row.r.kind == StateAtResult::Kind::kOutOfRange) ++out_of_range_count;
    rows.push_back(row);
  }

  if (g.format == Format::kJson) {
    out.Print("{\"schema_version\":1,\"session\":\"%s\",\"as_of_ns\":%llu,"
              "\"session_range\":[%llu,%llu],\"states\":[",
              JsonEscape(o.session).c_str(),
              static_cast<unsigned long long>(as_of),
              static_cast<unsigned long long>(base_ns),
              static_cast<unsigned long long>(meta.closed_at_ns));
    for (size_t i = 0; i < rows.size(); ++i) {
      if (i) out.Print(",");
      const Row& r = rows[i];
      out.Print("{\"topic\":\"%s\",\"state\":\"%s\"",
                JsonEscape(r.name).c_str(), StateKindName(r.r.kind));
      if (r.r.kind == StateAtResult::Kind::kFound) {
        out.Print(",\"as_of_ns\":%llu,\"seq\":%u,\"seq_in_ts\":%u,\"bytes\":%zu",
                  static_cast<unsigned long long>(r.r.as_of_ns), r.r.event.seq,
                  r.r.event.seq_in_ts, r.r.event.payload.size());
        if (!r.r.event.instance_key.empty()) {
          out.Print(",\"instance_key\":\"%s\"",
                    JsonEscape(r.r.event.instance_key).c_str());
        }
      } else if (r.r.kind == StateAtResult::Kind::kOutOfRange) {
        // 【关键】out_of_range 表示"查询本身问错了范围"，必须与"没数据"区分
        out.Print(",\"requested_ns\":%llu,\"hint\":\"请求时刻超出录制范围\"",
                  static_cast<unsigned long long>(as_of));
      }
      if (!r.err.empty()) out.Print(",\"error\":\"%s\"", JsonEscape(r.err).c_str());
      out.Print("}");
    }
    out.Print("]}\n");
    out.Flush();
    return out_of_range_count > 0 ? kPartial : kOk;
  }

  if (g.ShowHeaders()) {
    std::printf("定点状态: %s（as_of=%s）\n", o.session.c_str(),
                FormatRfc3339(as_of).c_str());
    std::printf("会话范围: %s ~ %s\n\n",
                FormatRfc3339(meta.created_at_ns).c_str(),
                meta.closed_at_ns ? FormatRfc3339(meta.closed_at_ns).c_str() : "(未关闭)");
  }
  PrintTableHeader({{"话题", 30}, {"状态", 14}, {"命中时刻", 24}, {"seq", 10}}, g);
  for (const Row& r : rows) {
    const char* kind = StateKindName(r.r.kind);
    std::string when = "-";
    if (r.r.kind == StateAtResult::Kind::kFound) {
      when = FormatRfc3339(r.r.as_of_ns);
    } else if (r.r.kind == StateAtResult::Kind::kOutOfRange) {
      when = "(超出范围)";
    } else if (r.r.kind == StateAtResult::Kind::kNotFound) {
      when = "(该时刻前无数据)";
    } else if (r.r.kind == StateAtResult::Kind::kDisposed) {
      when = "(已被 dispose)";
    }
    out.Print("%-30s  %-14s %-24s %s\n", r.name.c_str(), kind, when.c_str(),
              r.r.kind == StateAtResult::Kind::kFound
                  ? std::to_string(r.r.event.seq).c_str()
                  : "-");
  }
  if (out_of_range_count > 0) {
    out.Print("\n  注意: 有 %d 个话题的请求时刻**超出录制范围** —— "
              "这不是\"没有数据\"，而是查询问错了区间。\n", out_of_range_count);
  }
  out.Flush();
  return out_of_range_count > 0 ? kPartial : kOk;
}

}  // namespace

int CmdQuery(const std::vector<std::string>& args, GlobalOptions g) {
  QueryOpts o;
  if (g.format == Format::kJson && !g.format_explicit) o.stream = false;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto need = [&](const char* name) -> const std::string* {
      if (i + 1 >= args.size()) return nullptr;
      (void)name;
      return &args[++i];
    };

    if (a == "--list-topics") { o.list_topics = true; continue; }
    if (a == "--all-topics") { o.all_topics = true; continue; }
    if (a == "--with-payload") { o.with_payload = true; continue; }
    if (a == "--decode") { o.decode = true; continue; }
    if (a == "--type-format") {
      const std::string* v = need("--type-format");
      if (!v) return Fail(kUsage, "--type-format 缺少参数值", "",
                          "当前支持: json");
      o.type_format = *v;
      if (o.type_format != "json") {
        return Fail(kUsage, "--type-format " + *v + " 暂不支持",
                    "类型系统当前只实现了 JSON 自描述格式",
                    "请用 --type-format json；其它格式（idl/typeobject/xtypes）"
                    "见 docs/03-open-questions.md 的 Q4");
      }
      continue;
    }
    if (a == "--stream") { o.stream = true; continue; }
    if (a == "--topic") {
      const std::string* v = need("--topic");
      if (!v) return Fail(kUsage, "--topic 缺少参数值", "", "示例: --topic sensor/temp");
      o.topic = *v;
      continue;
    }
    if (a == "--since") {
      const std::string* v = need("--since");
      if (!v) return Fail(kUsage, "--since 缺少参数值", "", "示例: --since -30m");
      o.since_raw = *v;
      continue;
    }
    if (a == "--until") {
      const std::string* v = need("--until");
      if (!v) return Fail(kUsage, "--until 缺少参数值", "", "示例: --until @session+60s");
      o.until_raw = *v;
      continue;
    }
    if (a == "--state-at") {
      const std::string* v = need("--state-at");
      if (!v) return Fail(kUsage, "--state-at 缺少参数值", "",
                          "示例: --state-at @session+90s");
      o.state_at_raw = *v;
      continue;
    }
    if (a == "--limit") {
      const std::string* v = need("--limit");
      if (!v) return Fail(kUsage, "--limit 缺少参数值", "", "示例: --limit 100");
      o.limit = std::strtoull(v->c_str(), nullptr, 10);
      continue;
    }
    if (a == "--offset") {
      const std::string* v = need("--offset");
      if (!v) return Fail(kUsage, "--offset 缺少参数值", "", "示例: --offset 100");
      o.offset = std::strtoull(v->c_str(), nullptr, 10);
      continue;
    }
    if (a == "--out") {
      const std::string* v = need("--out");
      if (!v) return Fail(kUsage, "--out 缺少参数值", "", "示例: --out result.csv");
      o.out_path = *v;
      continue;
    }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "query 不支持该选项: " + a,
                  "本版本支持 --list-topics/--topic/--since/--until/--state-at/"
                  "--limit/--offset/--out/--with-payload/--stream/--decode/"
                  "--type-format",
                  "完整选项见 docs/08-cli-spec.md 第 5 节");
    }
    if (o.session.empty()) {
      o.session = a;
    } else {
      return Fail(kUsage, "query 只接受一个会话参数，多给了: " + a, "",
                  "若要跨会话查询，见设计文档 S16（尚未实现）");
    }
  }

  if (o.session.empty()) {
    return Fail(kUsage, "query 需要会话路径",
                "位置参数缺失",
                "例如: stratum query /data/stratum/s-20261014 --list-topics");
  }

  // 查询命令**不得有创建副作用**：先确认会话已存在
  if (!SessionExists(o.session)) {
    return Fail(kNotFound, "会话不存在: " + o.session,
                "路径不存在或不是目录",
                "用 `stratum session list <父目录>` 列出可用会话；"
                "注意 query 不会创建会话");
  }

  TimelineImplV2 tl;
  Status s = tl.Open(o.session);
  if (!s.ok()) {
    return Fail(ExitCodeOf(s), "无法打开会话: " + o.session, s.ToString(),
                "用 `stratum session list <父目录>` 列出可用会话");
  }

  Query q(&tl);
  SessionMetadata meta;
  Status ms = q.GetInfo(&meta);
  if (!ms.ok()) {
    LogVerbose(g, 1, "会话元数据读取失败（继续查询）: " + ms.ToString());
  }
  std::vector<TopicSummary> topics;
  Status ts = q.ListTopics(&topics);
  if (!ts.ok()) {
    tl.Close();
    return Fail(ExitCodeOf(ts), "列出话题失败", ts.ToString(), "");
  }

  // 【时间基准】@session+Ns 需要"会话开始时刻"。
  // 录制器会写 created_at_ns，但工具生成的会话（如 make_timeline）可能没写。
  // 此时**回退到首条数据的时间**，而不是直接失败 ——
  // 否则用户会看到"会话创建时间未知"，而实际上数据是完整可用的。
  uint64_t base_ns = meta.created_at_ns;
  if (base_ns == 0 || base_ns == kInvalidTimestamp) {
    for (const auto& t : topics) {
      if (t.first_ns != kInvalidTimestamp &&
          (base_ns == 0 || t.first_ns < base_ns)) {
        base_ns = t.first_ns;
      }
    }
    if (base_ns != 0) {
      LogVerbose(g, 1, "会话未记录创建时间，@session 基准回退为首条数据时间");
    }
  }

  LogVerbose(g, 1, "打开会话 " + o.session + "：话题 " +
                       std::to_string(topics.size()) + " 个，时间基准 " +
                       std::to_string(base_ns));

  // 模式分派：--state-at > --topic > --list-topics > 默认列话题
  int rc = kOk;
  if (!o.state_at_raw.empty()) {
    rc = DoStateAt(tl, meta, base_ns, topics, o, g);
  } else if (!o.topic.empty()) {
    rc = DoExport(tl, meta, base_ns, o, g);
  } else {
    rc = DoListTopics(tl, meta, base_ns, topics, o, g);
  }

  tl.Close();
  return rc;
}

}  // namespace cli
}  // namespace stratum
