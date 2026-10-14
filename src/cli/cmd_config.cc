// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_config.cc
//
// `stratum config validate <文件>` / `stratum config explain <文件>`
//
// 这两条命令是为 EC-16 / EC-17 设计的防线：
//   EC-16  配置了不存在的话题名（最常见误用）
//   EC-17  保留窗口设错，导致数据被立即删除（危险）
//
// `explain` 的价值在于：它把配置翻译成**后果**
// （"按此配置，最老的数据会在 24 小时后被删除，磁盘占用上限 50GB"），
// 让人在启动前就能看出错误 —— 而不是等一天后发现数据没了。
//
// 解析器说明：项目遵循 NFR-3.4（不引入过重依赖），因此这里**不引入 YAML 库**，
// 只实现所需的极小子集：`key: value` 与一层缩进嵌套。够用且零依赖。

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "cli_common.h"


namespace stratum {
namespace cli {

namespace {

// 支持的字段（严格模式：出现未列出的字段即报错，防拼写错误被忽略）
const char* kKnownKeys[] = {
    "schema_version",
    // instance
    "instance.id",
    // record
    "record.dir", "record.include", "record.exclude",
    "record.retain", "record.retain_bytes", "record.on_full",
    "record.on_disk_full", "record.min_free",
    "record.split_by_bytes", "record.split_by_duration",
    "record.clock_policy", "record.durability",
    "record.budget_cpu", "record.budget_io", "record.budget_mem",
    "record.schedule", "record.window", "record.window_keep",
    "record.trigger_on", "record.pre_buffer", "record.post_buffer",
    "record.domain", "record.advertised_address", "record.initial_peers",
    // 顶层
    "tz",
};

bool IsKnownKey(const std::string& k) {
  for (const char* s : kKnownKeys) {
    if (k == s) return true;
  }
  return false;
}

struct Config {
  std::map<std::string, std::string> kv;   // 扁平化后的 key -> value
  std::vector<std::string> unknown;        // 未知字段
  std::vector<std::string> errors;         // 语法错误
};

// 极简 YAML：支持 "# 注释"、"key: value"、"section:" + 缩进子项
Config ParseSimpleYaml(const std::string& path, std::string* raw_out) {
  Config c;
  std::ifstream in(path);
  if (!in) {
    c.errors.push_back("无法打开文件");
    return c;
  }
  std::string line;
  std::string section;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    if (raw_out) { *raw_out += line; *raw_out += "\n"; }

    // 去注释（简单处理：不在引号内）
    const size_t hash = line.find('#');
    if (hash != std::string::npos && (line.find('"') == std::string::npos ||
                                      hash < line.find('"'))) {
      line = line.substr(0, hash);
    }
    // 去尾空白
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
      line.pop_back();
    }
    if (line.empty()) continue;

    const bool indented = std::isspace(static_cast<unsigned char>(line[0])) != 0;
    // 去前导空白
    size_t b = 0;
    while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
    const std::string body = line.substr(b);

    const size_t colon = body.find(':');
    if (colon == std::string::npos) {
      c.errors.push_back("第 " + std::to_string(lineno) + " 行: 缺少 ':'");
      continue;
    }
    std::string key = body.substr(0, colon);
    std::string val = body.substr(colon + 1);
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
    size_t vb = 0;
    while (vb < val.size() && std::isspace(static_cast<unsigned char>(val[vb]))) ++vb;
    val = val.substr(vb);
    // 去引号
    if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                            (val.front() == '\'' && val.back() == '\''))) {
      val = val.substr(1, val.size() - 2);
    }

    if (val.empty() && !indented) {
      section = key;          // 段落头
      continue;
    }
    const std::string full = section.empty() ? key : (section + "." + key);
    if (!IsKnownKey(full)) {
      c.unknown.push_back(full + "  (第 " + std::to_string(lineno) + " 行)");
    }
    c.kv[full] = val;
  }
  return c;
}

// 解析 "24h" / "30m" / "1d" / "500ms" → 纳秒
bool ParseDurationNs(const std::string& s, uint64_t* out) {
  if (s.empty()) return false;
  size_t i = 0;
  while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
  if (i == 0) return false;
  const double v = std::atof(s.substr(0, i).c_str());
  const std::string u = s.substr(i);
  double mul = 0;
  if (u == "ms") mul = 1e6;
  else if (u == "s" || u.empty()) mul = 1e9;
  else if (u == "m") mul = 60e9;
  else if (u == "h") mul = 3600e9;
  else if (u == "d") mul = 86400e9;
  else if (u == "w") mul = 604800e9;
  else return false;
  *out = static_cast<uint64_t>(v * mul);
  return true;
}

bool ParseSizeBytes(const std::string& s, uint64_t* out) {
  if (s.empty()) return false;
  size_t i = 0;
  while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
  if (i == 0) return false;
  const double v = std::atof(s.substr(0, i).c_str());
  const std::string u = s.substr(i);
  double mul = 1;
  if (u.empty() || u == "B") mul = 1;
  else if (u == "KB") mul = 1000;
  else if (u == "MB") mul = 1e6;
  else if (u == "GB") mul = 1e9;
  else if (u == "TB") mul = 1e12;
  else if (u == "KiB") mul = 1024;
  else if (u == "MiB") mul = 1024.0 * 1024;
  else if (u == "GiB") mul = 1024.0 * 1024 * 1024;
  else return false;
  *out = static_cast<uint64_t>(v * mul);
  return true;
}

// ---------------------------------------------------------------------------
// validate
// ---------------------------------------------------------------------------
int DoValidate(const std::vector<std::string>& args, GlobalOptions g) {
  if (args.empty()) {
    return Fail(kUsage, "config validate 需要文件参数", "缺少配置文件路径",
                "例如: stratum config validate /etc/stratum/record.yaml");
  }
  const std::string path = args[0];
  std::string raw;
  Config c = ParseSimpleYaml(path, &raw);

  if (!c.errors.empty()) {
    std::printf("配置校验失败: %s\n", path.c_str());
    for (const auto& e : c.errors) std::printf("  %s\n", e.c_str());
    return Fail(kUsage, "配置文件格式错误: " + path,
                c.errors.front(), "检查是否使用了 'key: value' 形式");
  }
  if (c.kv.empty()) {
    return Fail(kUsage, "配置文件为空或没有可识别的字段: " + path,
                "未解析到任何 key: value", "参考 `stratum config init` 的模板（待实现）");
  }

  // schema_version 必须存在且受支持（对应 FR-S-2：更高版本必须拒绝）
  auto it = c.kv.find("schema_version");
  if (it == c.kv.end()) {
    return Fail(kUsage, "缺少 schema_version",
                "ds/08 8.2 节要求配置文件显式声明 schema_version",
                "在文件开头加一行: schema_version: 1");
  }
  const int ver = std::atoi(it->second.c_str());
  if (ver > 1) {
    return Fail(kUsage, "配置 schema_version 过高: " + it->second,
                "本程序支持到 v1，遇到更高版本必须拒绝（FR-S-2）",
                "升级 stratum，或改用 v1 的配置格式");
  }

  // 未知字段 → 拒绝（防拼写错误，如 retain 写成 retention）
  if (!c.unknown.empty()) {
    std::printf("配置校验失败: %s\n", path.c_str());
    std::printf("  未知字段（%zu 个）:\n", c.unknown.size());
    for (const auto& u : c.unknown) std::printf("    %s\n", u.c_str());
    return Fail(kUsage, "配置含未知字段",
                "严格模式下未知字段被拒绝，以防拼写错误被静默忽略"
                "（例如 retention 写成 retain 之外的形式）",
                "对照 docs/08-cli-spec.md 8.2 节的字段表修正");
  }

  std::printf("配置校验通过: %s\n", path.c_str());
  std::printf("  schema_version: %d\n", ver);
  std::printf("  已识别字段:     %zu 个\n", c.kv.size());

  // 语义检查（危险组合）
  int warnings = 0;
  auto get = [&](const char* k) -> std::string {
    auto i2 = c.kv.find(k);
    return i2 == c.kv.end() ? std::string() : i2->second;
  };

  const std::string retain = get("record.retain");
  const std::string on_disk = get("record.on_disk_full");
  uint64_t retain_ns = 0;
  if (!retain.empty()) {
    if (!ParseDurationNs(retain, &retain_ns)) {
      return Fail(kUsage, "record.retain 格式无法解析: " + retain,
                  "支持 ms/s/m/h/d/w 单位",
                  "例如 record.retain: 24h");
    }
    if (retain_ns == 0) {
      std::printf("\n  ⚠ 警告: record.retain = 0 表示**不限制保留时长**\n");
      ++warnings;
    }
  } else {
    std::printf("\n  ⚠ 警告: 未设置 record.retain —— 数据将一直累积，最终写满磁盘 (EC-11)\n");
    ++warnings;
  }

  if (on_disk == "warn-and-continue" && retain_ns == 0) {
    std::printf("  ⚠ 危险组合: on_disk_full=warn-and-continue 且 retain=0\n");
    std::printf("     → 磁盘写满后既不停止也不淘汰，会持续失败刷日志\n");
    ++warnings;
  }

  const std::string sched = get("record.schedule");
  if (!sched.empty() && get("tz").empty() && g.tz.empty()) {
    std::printf("  ⚠ 使用 record.schedule 但未指定时区（tz 或 --tz）\n");
    std::printf("     → 跨夏令时可能错一小时（NFR-2.5 要求时间语义显式）\n");
    ++warnings;
  }

  const std::string burst = get("record.durability");
  const std::string bio = get("record.budget_io");
  if (burst == "strict" && !bio.empty()) {
    std::printf("  ⚠ durability=strict 需要逐条 fsync，与 budget_io=%s 可能冲突\n",
                bio.c_str());
    ++warnings;
  }

  std::printf("\n  语义警告: %d 条\n", warnings);
  return warnings > 0 ? kPartial : kOk;
}

// ---------------------------------------------------------------------------
// explain
// ---------------------------------------------------------------------------
int DoExplain(const std::vector<std::string>& args, GlobalOptions g) {
  if (args.empty()) {
    return Fail(kUsage, "config explain 需要文件参数", "缺少配置文件路径",
                "例如: stratum config explain /etc/stratum/record.yaml");
  }
  const std::string path = args[0];
  std::string raw;
  Config c = ParseSimpleYaml(path, &raw);
  if (!c.errors.empty() || c.kv.empty()) {
    return Fail(kUsage, "无法解析配置: " + path,
                c.errors.empty() ? "没有可识别的字段" : c.errors.front(),
                "先用 `stratum config validate` 定位问题");
  }

  auto get = [&](const char* k) -> std::string {
    auto i2 = c.kv.find(k);
    return i2 == c.kv.end() ? std::string() : i2->second;
  };

  std::printf("配置效果解释: %s\n", path.c_str());
  std::printf("（逐条说明每一项**会怎样生效**，而非重复它的字面值）\n\n");

  // 保留 —— EC-17 的正面防线
  const std::string retain = get("record.retain");
  const std::string rbytes = get("record.retain_bytes");
  uint64_t retain_ns = 0, rbytes_v = 0;
  std::printf("[保留策略]\n");
  if (!retain.empty() && ParseDurationNs(retain, &retain_ns)) {
    const double h = static_cast<double>(retain_ns) / 3600e9;
    std::printf("  record.retain = %s\n", retain.c_str());
    std::printf("    → 超过 %.2f 小时的数据会被**自动删除**\n", h);
    std::printf("    → 最老的数据最多保留 %.2f 小时\n", h);
  } else {
    std::printf("  record.retain 未设置\n");
    std::printf("    → 数据**永不淘汰**，最终会写满磁盘（EC-11）\n");
  }
  if (!rbytes.empty() && ParseSizeBytes(rbytes, &rbytes_v)) {
    std::printf("  record.retain_bytes = %s\n", rbytes.c_str());
    std::printf("    → 占用超过 %s 时开始淘汰最旧数据\n", HumanBytes(rbytes_v).c_str());
    std::printf("    → 与 record.retain 同时设置时，**任一维度满足即淘汰**（取先到者）\n");
  }

  // 磁盘满
  const std::string odf = get("record.on_disk_full");
  std::printf("\n[磁盘满的行为]\n");
  if (odf.empty() || odf == "stop") {
    std::printf("  on_disk_full = %s（默认 stop）\n", odf.empty() ? "(默认)" : odf.c_str());
    std::printf("    → 磁盘满时**停止录制**并正常关闭会话\n");
    std::printf("    → 留下的是\"完整的历史到某一刻\"；适合复盘场景 (SC-01)\n");
  } else if (odf == "drop-oldest") {
    std::printf("  on_disk_full = drop-oldest\n");
    std::printf("    → 磁盘满时**删除最旧数据**继续录\n");
    std::printf("    → 留下的是\"最近的但不含起因\"；适合长期常开 (DP-02)\n");
    std::printf("    → ⚠ 复盘时可能发现\"故障前的数据已被删\"\n");
  } else {
    std::printf("  on_disk_full = %s\n", odf.c_str());
    std::printf("    → ⚠ 该策略下录制不会因磁盘满而停止，请确保有其他保护\n");
  }

  // 分片
  const std::string split = get("record.split_by_bytes");
  std::printf("\n[分片]\n");
  uint64_t sv = 0;
  if (!split.empty() && ParseSizeBytes(split, &sv)) {
    std::printf("  split_by_bytes = %s\n", split.c_str());
    std::printf("    → 单个地层文件达到 %s 时切分\n", HumanBytes(sv).c_str());
  } else {
    std::printf("  split_by_bytes 未设置 → 使用默认 256MB\n");
  }

  // 录制范围
  const std::string inc = get("record.include");
  const std::string exc = get("record.exclude");
  std::printf("\n[录制范围]\n");
  if (inc.empty() && exc.empty()) {
    std::printf("  未设置 include/exclude\n");
    std::printf("    → **录制所有发现到的话题**\n");
    std::printf("    → ⚠ 现场话题很多时会占满带宽与磁盘\n");
  } else {
    if (!inc.empty()) std::printf("  include: %s\n    → 只有匹配的话题会被录制\n", inc.c_str());
    if (!exc.empty()) std::printf("  exclude: %s\n    → 这些话题被排除（**排除优先于包含**）\n", exc.c_str());
  }

  // 时钟
  const std::string clock = get("record.clock_policy");
  std::printf("\n[时钟策略]\n");
  if (clock.empty()) {
    std::printf("  clock_policy 未设置 → 使用默认 clamp\n");
    std::printf("    → 源时间戳**倒退**时会被夹到上一条之后（保住 LSM 的范围扫描不变量）\n");
    std::printf("    → ⚠ 原始时间戳应保留在 raw_source_timestamp_ns，判断真实顺序时以它为准\n");
  } else if (clock == "accept") {
    std::printf("  clock_policy = accept\n");
    std::printf("    → ⚠ 时间戳原样写入。**倒退会破坏\"字节序==时间序\"**，\n");
    std::printf("      导致按时间范围查询结果错乱，且症状隐蔽（\"差不多对但顺序不对\"）\n");
  } else {
    std::printf("  clock_policy = %s\n", clock.c_str());
  }

  // 调度
  const std::string sched = get("record.schedule");
  const std::string tz = get("tz");
  if (!sched.empty()) {
    std::printf("\n[调度]\n");
    std::printf("  schedule = %s\n", sched.c_str());
    std::printf("  tz       = %s\n", tz.empty() ? "**(未设置 —— 危险)**" : tz.c_str());
    if (tz.empty() && g.tz.empty()) {
      std::printf("    → ⚠ 未指定时区，跨夏令时会错一小时（NFR-2.5）\n");
    }
  }

  std::printf("\n[汇总]\n");
  std::printf("  按此配置，系统会：%s\n",
              retain.empty() ? "**永不淘汰数据**（需自行监控磁盘）"
                             : "按保留窗口自动淘汰旧数据");
  std::printf("  磁盘满时：%s\n",
              (odf.empty() || odf == "stop") ? "停止录制并正常关闭会话"
                                             : "继续录制（删除或忽略）");
  std::printf("\n  提示: 建议先用 `stratum record --dry-run` 验证实际生效值。\n");
  return kOk;
}

}  // namespace

int CmdConfig(const std::vector<std::string>& args, GlobalOptions g) {
  if (args.empty()) {
    return Fail(kUsage, "config 需要子命令", "可用: validate / explain / init",
                "例如: stratum config validate /etc/stratum/record.yaml");
  }
  const std::string sub = args[0];
  const std::vector<std::string> rest(args.begin() + 1, args.end());
  if (sub == "validate") return DoValidate(rest, g);
  if (sub == "explain") return DoExplain(rest, g);
  if (sub == "init") return CmdNotImplemented("config init");
  return Fail(kUsage, "未知的 config 子命令: " + sub, "本版本支持 validate / explain",
              "完整设计见 docs/08-cli-spec.md 第 8 节");
}

}  // namespace cli
}  // namespace stratum
