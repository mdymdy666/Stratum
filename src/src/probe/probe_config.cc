// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/src/probe/probe_config.cc
//
// 埋点配置的实现。见头文件顶部的需求依据与两条错误语义。

#include "stratum/probe/probe_config.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/file.h>
#include <unistd.h>

#include "stratum/config/config.hpp"

namespace stratum {
namespace probe {

namespace {

// 允许的格式取值
bool IsValidFormat(const std::string& f) {
  return f == "kv" || f == "json" || f == "raw";
}

std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
  return s.substr(b, e - b);
}

// 剥掉成对的单/双引号。
//
// 【为什么需要】INI 里写 `format = "kv"` 是自然写法，但 config::IniFile
// 只做 key/value 切分，**不会剥引号** —— 实测返回值是 `"kv"`（含引号），
// 于是所有带引号的值都会被判为非法（这个 bug 被 14 个用例同时抓到）。
// 与其要求用户"别写引号"，不如在解析层剥掉 —— 更健壮。
std::string Unquote(const std::string& s) {
  const std::string v = Trim(s);
  if (v.size() >= 2) {
    const char f = v.front();
    const char b = v.back();
    if ((f == '"' && b == '"') || (f == '\'' && b == '\'')) {
      return v.substr(1, v.size() - 2);
    }
  }
  return v;
}

bool ParseBool(const std::string& s, bool* out) {
  std::string v;
  v.reserve(s.size());
  for (char c : s) {
    if (c != ' ' && c != '\t') v.push_back(static_cast<char>(::tolower(c)));
  }
  if (v == "true" || v == "1" || v == "yes" || v == "on") { *out = true; return true; }
  if (v == "false" || v == "0" || v == "no" || v == "off") { *out = false; return true; }
  return false;
}

bool ParseU32(const std::string& s, uint32_t* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long v = std::strtoul(s.c_str(), &end, 10);
  if (errno != 0 || end == s.c_str() || *end != '\0') return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

bool ParseDouble(const std::string& s, double* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || end == s.c_str() || *end != '\0') return false;
  *out = v;
  return true;
}

// 从 section 名还原 topic："topic.\"order/submit\"" -> order/submit
std::string SectionToTopic(const std::string& section) {
  static const std::string kPrefix = "topic.";
  if (section.rfind(kPrefix, 0) != 0) return {};
  std::string rest = section.substr(kPrefix.size());
  if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') {
    rest = rest.substr(1, rest.size() - 2);
  }
  return rest;
}

// 锁文件的年龄（秒）。文件不存在返回 -1。
long LockAgeSeconds(const std::string& path) {
  struct stat st;
  if (::stat((path + ".lock").c_str(), &st) != 0) return -1;
  if (st.st_size == 0) return -1;   // 空文件 = 未锁定
  const time_t now = ::time(nullptr);
  if (st.st_mtime >= now) return 0;
  return static_cast<long>(now - st.st_mtime);
}

// 读整个文件
bool ReadFile(const std::string& path, std::string* out, std::string* error) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in) {
    if (error) {
      *error = std::string("无法打开 ") + path + "：" + std::strerror(errno);
    }
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

}  // namespace

const char* LoadOutcomeName(LoadOutcome o) {
  switch (o) {
    case LoadOutcome::kOk: return "ok";
    case LoadOutcome::kMissing: return "missing";
    case LoadOutcome::kUnreadable: return "unreadable";
    case LoadOutcome::kParseError: return "parse_error";
    case LoadOutcome::kLockedByEditor: return "locked_by_editor";
  }
  return "unknown";
}

bool TopicSettings::operator==(const TopicSettings& o) const {
  return enabled == o.enabled && format == o.format &&
         sample_rate == o.sample_rate && max_rate_hz == o.max_rate_hz &&
         reliable == o.reliable && depth == o.depth;
}

bool ProbeSettings::operator==(const ProbeSettings& o) const {
  return global_enabled == o.global_enabled && defaults == o.defaults &&
         topics == o.topics;
}

std::string TopicToSection(const std::string& topic) {
  return "topic.\"" + topic + "\"";
}

// ---------------------------------------------------------------------------
// 解析
//
// 【关键】解析失败时**不修改 out**（调用方据此保留旧值）。
// ---------------------------------------------------------------------------
bool ParseSettings(const std::string& text, ProbeSettings* out, std::string* error) {
  if (out == nullptr) return false;

  config::IniFile ini;
  std::string inerr;
  if (!ini.Parse(text, &inerr)) {
    if (error) *error = inerr.empty() ? std::string("INI 语法错误") : inerr;
    return false;
  }

  ProbeSettings s;   // 先在临时对象上构建，成功后才交给调用方

  // --- [probe] 全局段 ---
  if (ini.Has("probe", "enabled")) {
    bool v = true;
    if (!ParseBool(Unquote(ini.Get("probe", "enabled")), &v)) {
      if (error) *error = "probe.enabled 不是合法布尔值: " + Unquote(ini.Get("probe", "enabled"));
      return false;
    }
    s.global_enabled = v;
  }

  // --- [defaults] 全局默认（被 topic 段覆盖）---
  {
    const char* sec = "defaults";
    if (ini.Has(sec, "enabled")) {
      bool v = true;
      if (!ParseBool(Unquote(ini.Get(sec, "enabled")), &v)) {
        if (error) *error = "defaults.enabled 不是合法布尔值";
        return false;
      }
      s.defaults.enabled = v;
    }
    if (ini.Has(sec, "format")) {
      const std::string f = Unquote(ini.Get(sec, "format"));
      if (!IsValidFormat(f)) {
        if (error) *error = "defaults.format 非法（应为 kv/json/raw）: " + f;
        return false;
      }
      s.defaults.format = f;
    }
    if (ini.Has(sec, "sample_rate")) {
      double v = 1.0;
      if (!ParseDouble(Unquote(ini.Get(sec, "sample_rate")), &v) || v < 0.0 || v > 1.0) {
        if (error) *error = "defaults.sample_rate 应在 [0,1]";
        return false;
      }
      s.defaults.sample_rate = v;
    }
    if (ini.Has(sec, "max_rate_hz")) {
      uint32_t v = 0;
      if (!ParseU32(Unquote(ini.Get(sec, "max_rate_hz")), &v)) {
        if (error) *error = "defaults.max_rate_hz 不是非负整数";
        return false;
      }
      s.defaults.max_rate_hz = v;
    }
    if (ini.Has(sec, "reliable")) {
      bool v = false;
      if (!ParseBool(Unquote(ini.Get(sec, "reliable")), &v)) {
        if (error) *error = "defaults.reliable 不是合法布尔值";
        return false;
      }
      s.defaults.reliable = v;
    }
    if (ini.Has(sec, "depth")) {
      uint32_t v = 8;
      if (!ParseU32(Unquote(ini.Get(sec, "depth")), &v) || v == 0) {
        if (error) *error = "defaults.depth 应为正整数";
        return false;
      }
      s.defaults.depth = v;
    }
  }

  // --- [topic."xxx"] 覆盖段 ---
  //
  // 【为什么需要遍历】IniFile 只提供按键查询，不提供"列出所有 section"。
  // 因此先从原文里扫出所有 `[topic."..."]` 的 section 名。
  {
    std::istringstream lines(text);
    std::string line;
    std::vector<std::string> topic_sections;
    while (std::getline(lines, line)) {
      std::string clean;
      clean.reserve(line.size());
      for (char c : line) {
        if (c != ' ' && c != '\t' && c != '\r') clean.push_back(c);
      }
      if (clean.empty() || clean.front() == '#' || clean.front() == ';') continue;
      if (clean.front() != '[') continue;
      const size_t rb = clean.find(']');
      if (rb == std::string::npos) continue;
      const std::string name = clean.substr(1, rb - 1);
      if (!SectionToTopic(name).empty()) topic_sections.push_back(name);
    }

    for (const auto& sec : topic_sections) {
      const std::string topic = SectionToTopic(sec);
      if (topic.empty()) continue;
      TopicSettings t = s.defaults;   // 从默认值出发

      if (ini.Has(sec, "enabled")) {
        bool v = true;
        if (!ParseBool(Unquote(ini.Get(sec, "enabled")), &v)) {
          if (error) *error = "[" + sec + "] enabled 不是合法布尔值";
          return false;
        }
        t.enabled = v;
      }
      if (ini.Has(sec, "format")) {
        const std::string f = Unquote(ini.Get(sec, "format"));
        if (!IsValidFormat(f)) {
          if (error) *error = "[" + sec + "] format 非法（应为 kv/json/raw）: " + f;
          return false;
        }
        t.format = f;
      }
      if (ini.Has(sec, "sample_rate")) {
        double v = 1.0;
        if (!ParseDouble(Unquote(ini.Get(sec, "sample_rate")), &v) || v < 0.0 || v > 1.0) {
          if (error) *error = "[" + sec + "] sample_rate 应在 [0,1]";
          return false;
        }
        t.sample_rate = v;
      }
      if (ini.Has(sec, "max_rate_hz")) {
        uint32_t v = 0;
        if (!ParseU32(Unquote(ini.Get(sec, "max_rate_hz")), &v)) {
          if (error) *error = "[" + sec + "] max_rate_hz 不是非负整数";
          return false;
        }
        t.max_rate_hz = v;
      }
      if (ini.Has(sec, "reliable")) {
        bool v = false;
        if (!ParseBool(Unquote(ini.Get(sec, "reliable")), &v)) {
          if (error) *error = "[" + sec + "] reliable 不是合法布尔值";
          return false;
        }
        t.reliable = v;
      }
      if (ini.Has(sec, "depth")) {
        uint32_t v = 8;
        if (!ParseU32(Unquote(ini.Get(sec, "depth")), &v) || v == 0) {
          if (error) *error = "[" + sec + "] depth 应为正整数";
          return false;
        }
        t.depth = v;
      }
      s.topics[topic] = t;
    }
  }

  *out = std::move(s);   // 只在完全成功时才落地
  return true;
}

// ---------------------------------------------------------------------------
// 生成默认配置文件（PR-05 / S07）
//
// 【三条约束】
//   1. O_EXCL 原子创建 —— 已存在则**不覆盖**（多进程同时启动只有一个能建）
//   2. **绝不改写用户文件**（S07 的核心裁决：宁可报告未配置项，也不悄悄改）
//   3. 内容带注释，说明每一项是什么、改它会发生什么
// ---------------------------------------------------------------------------
bool ProbeConfig::GenerateDefault(const std::string& path,
                                  const std::vector<std::string>& known_topics,
                                  std::string* error) {
  const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
  if (fd < 0) {
    if (error) {
      if (errno == EEXIST) {
        *error = "配置文件已存在，不覆盖: " + path;
      } else {
        *error = std::string("无法创建 ") + path + "：" + std::strerror(errno);
      }
    }
    return false;
  }

  std::ostringstream o;
  o << "# stratum_probe.toml —— 由埋点库自动生成\n";
  o << "#\n";
  o << "# 改动本文件**即时生效**，不需要重启业务进程（轮询周期约 1 秒）。\n";
  o << "# 想开启或关闭某个 topic：把对应段落的 enabled 改成 true / false。\n";
  o << "#\n";
  o << "# 错误处理的两条不同语义（重要）：\n";
  o << "#   · [probe] enabled  ← 解析失败时**强制关闭**（fail-safe）；\n";
  o << "#     因为改配置的动机往往是「出事了要关掉它」，此时保留旧值（开着）会让熔断失效。\n";
  o << "#   · 其余所有项        ← 解析失败时**保留旧值**（fail-preserve）。\n";
  o << "#\n";
  o << "# 本文件不会被埋点库改写。业务新增探针后，未出现在这里的探针会用默认值运行，\n";
  o << "# 并可通过 `stratum probe status` 查看到「未配置的探针」。\n";
  o << "\n";
  o << "[probe]\n";
  o << "enabled = true          # 全局总开关；false = 所有探针静默（fail-safe 项）\n";
  o << "\n";
  o << "[defaults]\n";
  o << "enabled = true          # 未被下面任何 topic 段覆盖时的默认开关\n";
  o << "format = \"kv\"           # 信封格式：kv | json | raw\n";
  o << "sample_rate = 1.0       # (0,1]，1 = 不采样；0 等价于关闭\n";
  o << "max_rate_hz = 0         # 0 = 不限；超过则丢弃并计数\n";
  o << "reliable = false        # DDS 可靠性：false = best_effort（低开销）\n";
  o << "depth = 8               # DDS 历史深度；**调小会立即丢弃已排队样本**\n";

  for (const auto& t : known_topics) {
    o << "\n";
    o << "[" << TopicToSection(t) << "]\n";
    o << "enabled = true          # ← 想关掉这个 topic 就改这里\n";
  }

  const std::string content = o.str();
  size_t off = 0;
  while (off < content.size()) {
    const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
    if (n <= 0) {
      ::close(fd);
      if (error) *error = std::string("写入失败：") + std::strerror(errno);
      return false;
    }
    off += static_cast<size_t>(n);
  }
  ::close(fd);
  return true;
}

// ---------------------------------------------------------------------------
// 加载（PR-06/07/08/09）
// ---------------------------------------------------------------------------
LoadReport ProbeConfig::Load(const std::string& path,
                             std::chrono::milliseconds lock_timeout) {
  LoadReport rep;

  // 1) .lock 检查（S08）：非空且未超时 -> 跳过（不是错误）
  const long age = LockAgeSeconds(path);
  if (age >= 0) {
    const auto timeout_s =
        std::chrono::duration_cast<std::chrono::seconds>(lock_timeout).count();
    if (age < timeout_s) {
      rep.outcome = LoadOutcome::kLockedByEditor;
      rep.detail = "编辑锁存在（年龄 " + std::to_string(age) +
                   " 秒），本轮跳过重载";
      std::lock_guard<std::mutex> lock(mutex_);
      ++counters_.loads_locked_skipped;
      return rep;
    }
    // 锁超时 -> 强制重载（否则崩溃的编辑器会让监控永久冻结）
    rep.detail = "编辑锁已存在 " + std::to_string(age) +
                 " 秒（超过 " + std::to_string(timeout_s) +
                 " 秒），强制重载";
  }

  // 2) 读文件
  std::string text;
  std::string err;
  if (!ReadFile(path, &text, &err)) {
    rep.outcome = (errno == ENOENT) ? LoadOutcome::kMissing
                                    : LoadOutcome::kUnreadable;
    rep.detail = err;
    std::lock_guard<std::mutex> lock(mutex_);
    if (rep.outcome == LoadOutcome::kUnreadable) {
      // 读不了：fail-preserve（不动 settings），但要记录
      counters_.loads_parse_error += 0;
    }
    return rep;
  }

  // 3) 解析到临时对象
  ProbeSettings parsed;
  std::string perr;
  if (!ParseSettings(text, &parsed, &perr)) {
    // ★ 关键：解析失败。
    //   全局 enabled -> fail-safe（关闭）
    //   其余项       -> 保留旧值
    rep.outcome = LoadOutcome::kParseError;
    rep.detail = perr;
    rep.global_failsafe_triggered = true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      settings_.global_enabled = false;   // 只改这一项
      ++counters_.loads_parse_error;
      ++counters_.loads_failsafe;
    }
    rep.applied = true;
    return rep;
  }

  // 4) 成功 -> 整体替换
  {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = std::move(parsed);
    ++counters_.loads_ok;
    if (age >= 0) ++counters_.lock_forced_reloads;
  }
  rep.outcome = LoadOutcome::kOk;
  rep.applied = true;
  if (rep.detail.empty()) rep.detail = "已加载";
  return rep;
}

TopicSettings ProbeConfig::Resolve(const std::string& topic) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = settings_.topics.find(topic);
  TopicSettings t = (it != settings_.topics.end()) ? it->second : settings_.defaults;
  // 全局总开关优先：关掉就是关掉（fail-safe 语义的延伸）
  if (!settings_.global_enabled) t.enabled = false;
  return t;
}

ProbeSettings ProbeConfig::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return settings_;
}

std::vector<std::string> ProbeConfig::DeclaredTopics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  out.reserve(settings_.topics.size());
  for (const auto& kv : settings_.topics) out.push_back(kv.first);
  std::sort(out.begin(), out.end());
  return out;
}

ProbeConfig::Counters ProbeConfig::counters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counters_;
}

}  // namespace probe
}  // namespace stratum
