// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_types.cc
//
// `stratum types` —— 管理话题的类型定义。
//
// 【为什么需要这个命令】
//
// 调研（本轮）发现两件事叠加，导致 `query --decode` 永远走降级路径：
//
//   1. DDS 的 SEDP 不保证携带类型定义，record 写下的 `type_definition` 常为空；
//   2. `StratumDB::RegisterTopic` 只编码 `(id, type_name)` ——
//      **type_definition 根本没进存储**。
//
// 于是"能把原始字节解成结构化数据"这条能力始终无法端到端跑通。
//
// 本命令让用户**显式注册**一个自描述定义（当前支持 JSON），
// 之后 `query --decode` 就能真的解出 `"decoded":true,"value":{...}`。
//
// 【设计约束】
//   · 定义只挂在**已存在的话题**上（SetTopicTypeDefinition 会校验），
//     避免出现"有定义但没有数据"的悬空条目
//   · 注册时**校验 JSON 合法性** —— 否则错误会推迟到查询时才暴露
//   · 输出走 stdout（数据出口），提示走 stderr

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cli_common.h"
#include "stratum/core/timeline_impl_v2.h"
#include "stratum/types/json_serializer.h"

using namespace stratum;
using namespace stratum::cli;

namespace stratum {
namespace cli {

namespace {

// 读出 --from 指定的定义文本；"-" 表示 stdin
bool ReadDefinition(const std::string& from, std::string* out, std::string* err) {
  if (from == "-") {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    *out = ss.str();
  } else {
    std::ifstream in(from, std::ios::in | std::ios::binary);
    if (!in) {
      if (err) *err = "无法打开文件: " + from;
      return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
  }
  // 去掉首尾空白，避免"-"读到空串被判为非法
  size_t b = out->find_first_not_of(" \t\r\n");
  size_t e = out->find_last_not_of(" \t\r\n");
  if (b == std::string::npos) {
    *out = "";
  } else {
    *out = out->substr(b, e - b + 1);
  }
  if (out->empty()) {
    if (err) *err = "定义内容为空";
    return false;
  }
  return true;
}

// 用 JSON 序列化器校验定义文本是否合法自描述 JSON
bool ValidateJsonDefinition(const std::string& text, std::string* err) {
  types::JsonSerializer s;
  types::DynamicValue dv;
  Status st = s.Deserialize(text, &dv);
  if (!st.ok()) {
    if (err) *err = st.message();
    return false;
  }
  return true;
}

int Usage() {
  return Fail(kUsage, "types 需要子命令",
              "可用: register / list / show",
              "示例: stratum types register MyType --from def.json --session /data/s1");
}

}  // namespace

int CmdTypes(const std::vector<std::string>& args, GlobalOptions g) {
  if (args.empty()) return Usage();
  const std::string sub = args[0];

  std::string session;
  std::string from;
  std::string type_name;

  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](void) -> const std::string* {
      if (i + 1 >= args.size()) return nullptr;
      return &args[++i];
    };
    if (a == "--session") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--session 缺少参数值", "", "");
      session = *v;
      continue;
    }
    if (a == "--from") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--from 缺少参数值", "", "用 - 表示从 stdin 读");
      from = *v;
      continue;
    }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "types " + sub + " 不支持该选项: " + a,
                  "支持 --session / --from",
                  "完整说明见 docs/08-cli-spec.md");
    }
    if (type_name.empty()) {
      type_name = a;
    } else {
      return Fail(kUsage, "多余的位置参数: " + a, "", "");
    }
  }

  if (session.empty()) {
    return Fail(kUsage, "types 需要 --session <会话路径>",
                "类型定义挂在具体会话的话题上",
                "例如: --session /data/stratum/s1");
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

  // 直接使用 TimelineImplV2 的公开接口。
  // （Query / TopicSummary 是 cmd_query.cc 与 recorder.h 里的另一套包装，
  //  本命令只需要话题元数据，用 ITimeline 的接口更直接、依赖更少。）
  SessionMetadata meta;
  (void)timeline.GetSessionMetadata(&meta);
  std::vector<TopicMetadata> topics;
  s = timeline.ListTopics(&topics);
  if (!s.ok()) {
    timeline.Close();
    return Fail(ExitCodeOf(s), "列出话题失败", s.ToString(), "");
  }

  // -------------------------------------------------------------------------
  // register
  // -------------------------------------------------------------------------
  if (sub == "register") {
    if (type_name.empty()) {
      timeline.Close();
      return Fail(kUsage, "register 需要 <type_name>", "位置参数缺失",
                  "例如: stratum types register MyType --from def.json --session <会话>");
    }
    if (from.empty()) {
      timeline.Close();
      return Fail(kUsage, "register 需要 --from <文件>",
                  "没有类型定义就无法解码",
                  "用 - 表示从 stdin 读");
    }

    std::string definition, err;
    if (!ReadDefinition(from, &definition, &err)) {
      timeline.Close();
      return Fail(kUsage, "读取类型定义失败: " + from, err,
                  "确认文件存在且非空");
    }
    if (!ValidateJsonDefinition(definition, &err)) {
      timeline.Close();
      return Fail(kUsage, "类型定义不是合法的自描述 JSON", err,
                  "当前只支持 definition_format=json；"
                  "其它格式见 docs/03-open-questions.md 的 Q4");
    }

    // 定义挂在**话题名**上；type_name 用于提示用户它应匹配话题的 type
    int matched = 0;
    for (const auto& t : topics) {
      if (t.type_name != type_name) continue;
      Status rs = timeline.SetTopicTypeDefinition(t.topic_name, definition);
      if (!rs.ok()) {
        std::fprintf(stderr, "  跳过 %s: %s\n", t.topic_name.c_str(),
                     rs.ToString().c_str());
        continue;
      }
      ++matched;
      LogVerbose(g, 1, "已为话题 " + t.topic_name + " 注册类型定义");
    }

    if (matched == 0) {
      timeline.Close();
      // 给出**可操作**的提示：列出会话里实际存在的类型名
      std::string have;
      for (const auto& t : topics) {
        if (!have.empty()) have += ", ";
        have += t.type_name;
      }
      return Fail(kNotFound,
                  "会话中没有 type_name 为 " + type_name + " 的话题",
                  "会话里实际存在的类型: " + (have.empty() ? "(无话题)" : have),
                  "用 `stratum query <会话> --list-topics` 查看；"
                  "register 只能挂在已有话题上");
    }

    (void)timeline.Flush();
    timeline.Close();
    if (g.format == Format::kJson) {
      std::printf("{\"schema_version\":1,\"session\":\"%s\",\"type_name\":\"%s\","
                  "\"topics_updated\":%d,\"definition_bytes\":%zu}\n",
                  JsonEscape(session).c_str(), JsonEscape(type_name).c_str(),
                  matched, definition.size());
    } else if (!g.quiet) {
      std::fprintf(stderr, "类型定义已注册\n");
      std::fprintf(stderr, "  类型:    %s\n", type_name.c_str());
      std::fprintf(stderr, "  话题:    %d 个\n", matched);
      std::fprintf(stderr, "  定义:    %zu 字节（JSON）\n", definition.size());
      std::fprintf(stderr, "\n  下一步: stratum query %s --decode --topic <话题>\n",
                   session.c_str());
    }
    return kOk;
  }

  // -------------------------------------------------------------------------
  // list
  // -------------------------------------------------------------------------
  if (sub == "list") {
    // 逐话题查是否有定义（便于判断能不能解码）
    struct Row {
      std::string topic;
      std::string type;
      bool has_def;
      size_t def_bytes;
      uint64_t count;
    };
    std::vector<Row> rows;
    size_t with_def = 0;
    for (const auto& t : topics) {
      std::string def;
      Status gs = timeline.GetTopicTypeDefinition(t.topic_name, &def);
      Row r;
      r.topic = t.topic_name;
      r.type = t.type_name;
      r.has_def = gs.ok() && !def.empty();
      r.def_bytes = r.has_def ? def.size() : 0;
      r.count = t.message_count;
      if (r.has_def) ++with_def;
      rows.push_back(r);
    }

    if (g.format == Format::kJson) {
      std::printf("{\"schema_version\":1,\"session\":\"%s\",\"topics\":[",
                  JsonEscape(session).c_str());
      for (size_t i = 0; i < rows.size(); ++i) {
        if (i) std::printf(",");
        std::printf("{\"topic\":\"%s\",\"type\":\"%s\",\"has_definition\":%s,"
                    "\"definition_bytes\":%zu,\"count\":%llu}",
                    JsonEscape(rows[i].topic).c_str(),
                    JsonEscape(rows[i].type).c_str(),
                    rows[i].has_def ? "true" : "false", rows[i].def_bytes,
                    static_cast<unsigned long long>(rows[i].count));
      }
      std::printf("]}\n");
    } else if (g.format == Format::kCsv) {
      std::printf("topic,type,has_definition,definition_bytes,count\n");
      for (const auto& r : rows) {
        std::printf("%s,%s,%d,%zu,%llu\n", r.topic.c_str(), r.type.c_str(),
                    r.has_def ? 1 : 0, r.def_bytes,
                    static_cast<unsigned long long>(r.count));
      }
    } else {
      std::printf("会话: %s\n", session.c_str());
      std::printf("话题 %zu 个，其中 %zu 个有类型定义（可解码）\n\n",
                  rows.size(), with_def);
      std::printf("%-30s %-18s %-12s %10s\n", "话题", "类型", "可解码", "条数");
      std::printf("%s\n", std::string(74, '-').c_str());
      for (const auto& r : rows) {
        std::printf("%-30s %-18s %-12s %10s\n", r.topic.c_str(), r.type.c_str(),
                    r.has_def ? "是" : "否（降级）",
                    HumanCount(r.count).c_str());
      }
      if (with_def == 0) {
        std::printf("\n提示: 所有话题都没有类型定义 —— `query --decode` 会走降级路径。\n");
        std::printf("      可先注册: stratum types register <类型名> --from <json文件> "
                    "--session %s\n", session.c_str());
      }
    }
    timeline.Close();
    return kOk;
  }

  // -------------------------------------------------------------------------
  // show
  // -------------------------------------------------------------------------
  if (sub == "show") {
    if (type_name.empty()) {
      timeline.Close();
      return Fail(kUsage, "show 需要 <type_name>", "", "");
    }
    int shown = 0;
    for (const auto& t : topics) {
      if (t.type_name != type_name) continue;
      std::string def;
      Status gs = timeline.GetTopicTypeDefinition(t.topic_name, &def);
      const bool has = gs.ok() && !def.empty();
      if (g.format == Format::kJson) {
        if (shown) std::printf(",");
        std::printf("{\"topic\":\"%s\",\"type\":\"%s\",\"has_definition\":%s,"
                    "\"definition\":\"%s\"}",
                    JsonEscape(t.topic_name).c_str(), JsonEscape(t.type_name).c_str(),
                    has ? "true" : "false", JsonEscape(def).c_str());
      } else {
        std::printf("话题: %s\n", t.topic_name.c_str());
        std::printf("类型: %s\n", t.type_name.c_str());
        if (has) {
          std::printf("定义（%zu 字节）:\n%s\n", def.size(), def.c_str());
        } else {
          std::printf("定义: **无** —— `query --decode` 会走降级路径\n");
          std::printf("  可用 types register 注册一个 JSON 自描述定义\n");
        }
      }
      ++shown;
    }
    timeline.Close();
    if (shown == 0) {
      return Fail(kNotFound, "会话中没有 type_name 为 " + type_name + " 的话题",
                  "该类型在本会话中不存在",
                  "用 `stratum types list --session <会话>` 查看已有类型");
    }
    return kOk;
  }

  timeline.Close();
  return Fail(kUsage, "types 不支持的子命令: " + sub,
              "可用: register / list / show", "");
}

}  // namespace cli
}  // namespace stratum
