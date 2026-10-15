// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cli_common.cc
#include "cli_common.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>

#include "stratum/logging/log.hpp"

namespace stratum {
namespace cli {

const char* ExitCodeName(ExitCode c) {
  switch (c) {
    case kOk: return "OK";
    case kError: return "ERROR";
    case kUsage: return "USAGE";
    case kExists: return "EXISTS";
    case kIntegrity: return "INTEGRITY";
    case kPartial: return "PARTIAL";
    case kEnv: return "ENV";
    case kBusy: return "BUSY";
    case kNotFound: return "NOTFOUND";
  }
  return "?";
}

bool ParseFormat(const std::string& s, Format* out) {
  if (s == "table") { *out = Format::kTable; return true; }
  if (s == "json") { *out = Format::kJson; return true; }
  if (s == "yaml") { *out = Format::kYaml; return true; }
  if (s == "csv") { *out = Format::kCsv; return true; }
  return false;
}

const char* FormatName(Format f) {
  switch (f) {
    case Format::kTable: return "table";
    case Format::kJson: return "json";
    case Format::kYaml: return "yaml";
    case Format::kCsv: return "csv";
  }
  return "?";
}

int Fail(ExitCode code, const std::string& symptom, const std::string& cause,
         const std::string& action) {
  std::fprintf(stderr, "错误: %s\n", symptom.c_str());
  if (!cause.empty()) std::fprintf(stderr, "  原因: %s\n", cause.c_str());
  if (!action.empty()) std::fprintf(stderr, "  动作: %s\n", action.c_str());
  std::fprintf(stderr, "  退出码: %d (%s)\n", static_cast<int>(code),
               ExitCodeName(code));
  return static_cast<int>(code);
}

ExitCode ExitCodeOf(const Status& s) {
  if (s.ok()) return kOk;
  const std::string t = s.ToString();
  // Status 的分类 → 退出码（4 与 6 必须分开，见 docs/08 1.2）
  if (t.find("NotFound") != std::string::npos) return kNotFound;
  if (t.find("AlreadyExists") != std::string::npos) return kExists;
  if (t.find("Corruption") != std::string::npos) return kIntegrity;
  if (t.find("IOError") != std::string::npos) return kEnv;
  if (t.find("OutOfSpace") != std::string::npos) return kEnv;
  if (t.find("NotSupported") != std::string::npos) return kUsage;
  if (t.find("InvalidArgument") != std::string::npos) return kUsage;
  if (t.find("ShutdownInProgress") != std::string::npos) return kBusy;
  if (t.find("Timeout") != std::string::npos) return kError;
  return kError;
}

int FailStatus(ExitCode code, const std::string& what, const Status& s) {
  return Fail(code, what + " 失败", s.ToString(), "");
}

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

namespace {

std::string LogDirectory() {
  const char* cache = std::getenv("XDG_CACHE_HOME");
  if (cache && *cache) {
    return std::string(cache) + "/stratum";
  }
  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::string(home) + "/.cache/stratum";
  }
  return "/tmp/stratum";
}

log::Level LevelFromVerbose(int verbose) {
  // 默认 Warn（与移植模块的发布默认一致）；-v 逐级提高
  if (verbose <= 0) return log::Level::Warn;
  if (verbose == 1) return log::Level::Info;
  if (verbose == 2) return log::Level::Debug;
  return log::Level::Trace;
}

std::string PatternFor(const std::string& format) {
  if (format == "json") {
    // JSON lines：便于被日志采集器抓取（NFR-5 可观测性）
    //
    // 【为什么是 elapse_s 而不是时间戳】
    // 移植的 PatternFormatter 实际只注册了
    //   m p N e E f l t F c / logger level message thread fiber
    //   elapse thread_name file line function
    // **没有 time**（尽管头文件注释声称支持 {time}，属上游缺陷）。
    // {elapse} 是进程启动以来的秒数，因此字段如实命名为 elapse_s —— 不假装它是
    // wall-clock 时间戳。需要绝对时间请用默认的 text 格式（由 sink 侧提供）。
    // 不带时间字段：采集器（journald/loki/filebeat）本就会补采集时间戳，
    // 而本模块没有可用的 wall-clock 占位符（见上）。宁可不带，也不输出
    // 字面量 `{time}` 或语义可疑的"运行秒数"。
    return "{\"level\":\"{level}\",\"logger\":\"{logger}\","
           "\"file\":\"{file}\",\"line\":{line},"
           "\"msg\":\"{message}\"}\n";
  }
  return "{time} [{level}] {logger} ({file}:{line}) {message}\n";
}

}  // namespace

void InitLogging(const GlobalOptions& g) {
  const log::Level level = LevelFromVerbose(g.verbose);
  const std::string pattern = PatternFor(g.log_format);

  if (g.log_file == "-") {
    // 显式要求 stderr。**依然不碰 stdout** —— CLI 的数据出口必须干净。
    auto logger = log::LoggerManager::Instance().Get("stratum");
    logger->SetLevel(level);
    std::vector<log::LogItemWorker::ptr> workers;
    workers.push_back(std::make_shared<log::LogItemWorker>(
        std::make_shared<log::MinimumLevelFilter>(level),
        std::make_shared<log::PatternFormatter>(pattern),
        std::make_shared<log::StderrSink>()));
    logger->ConfigureDefaults(std::move(workers));
    return;
  }

  // 默认写文件。目录不存在时创建（否则 FileSink 打开失败，日志静默消失）。
  std::string dir = LogDirectory();
  std::string file = "stratum.log";
  if (!g.log_file.empty()) {
    std::filesystem::path p(g.log_file);
    if (p.has_parent_path()) dir = p.parent_path().string();
    file = p.filename().string();
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  log::LoggerManager::Instance().Configure(level, /*stdout_enabled=*/false,
                                           dir, file, pattern);
}

log::Logger::ptr CliLogger() {
  return log::LoggerManager::Instance().Get("stratum");
}

void LogVerbose(const GlobalOptions& g, int min_level, const std::string& msg) {
  if (g.verbose < min_level) return;
  STRATUM_LOG_INFO(CliLogger()) << msg;
}

// ---------------------------------------------------------------------------
// 解析助手
// ---------------------------------------------------------------------------

bool ParseDurationNs(const std::string& s, uint64_t* out) {
  if (s.empty() || !out) return false;
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
  if (s.empty() || !out) return false;
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

namespace {

uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

// 解析 RFC3339：YYYY-MM-DDTHH:MM:SS[.ffffff][Z|+HH:MM|-HH:MM]
// 返回 false 表示格式不合法（**不猜**，见 docs/08 的 R1）。
bool ParseRfc3339(const std::string& s, uint64_t* out, std::string* err) {
  if (s.size() < 19) {
    if (err) *err = "RFC3339 时间至少需要 YYYY-MM-DDTHH:MM:SS";
    return false;
  }
  std::tm tm{};
  // 手工解析日期时间部分，避免 get_time 对分隔符的宽松行为
  auto num = [&](size_t pos, size_t len) -> int {
    int v = 0;
    for (size_t i = 0; i < len; ++i) {
      const char c = s[pos + i];
      if (!std::isdigit(static_cast<unsigned char>(c))) return -1;
      v = v * 10 + (c - '0');
    }
    return v;
  };
  const int Y = num(0, 4), M = num(5, 2), D = num(8, 2);
  const int h = num(11, 2), m = num(14, 2), sec = num(17, 2);
  if (Y < 0 || M < 0 || D < 0 || h < 0 || m < 0 || sec < 0 ||
      s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ') ||
      s[13] != ':' || s[16] != ':') {
    if (err) *err = "RFC3339 格式应为 YYYY-MM-DDTHH:MM:SS[.fff][Z|±HH:MM]";
    return false;
  }
  tm.tm_year = Y - 1900;
  tm.tm_mon = M - 1;
  tm.tm_mday = D;
  tm.tm_hour = h;
  tm.tm_min = m;
  tm.tm_sec = sec;

  size_t pos = 19;
  double frac = 0.0;
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    double scale = 0.1;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
      frac += (s[pos] - '0') * scale;
      scale *= 0.1;
      ++pos;
    }
  }

  // 时区
  long offset_sec = 0;
  bool has_tz = false;
  if (pos < s.size()) {
    if (s[pos] == 'Z' || s[pos] == 'z') {
      offset_sec = 0;
      has_tz = true;
      ++pos;
    } else if (s[pos] == '+' || s[pos] == '-') {
      const int sign = (s[pos] == '-') ? -1 : 1;
      ++pos;
      if (pos + 5 > s.size()) {
        if (err) *err = "时区偏移格式应为 ±HH:MM";
        return false;
      }
      const int oh = num(pos, 2);
      const int om = num(pos + 3, 2);
      if (oh < 0 || om < 0 || s[pos + 2] != ':') {
        if (err) *err = "时区偏移格式应为 ±HH:MM";
        return false;
      }
      offset_sec = sign * (oh * 3600 + om * 60);
      has_tz = true;
      pos += 5;
    }
  }
  if (pos != s.size()) {
    if (err) *err = "时间字符串尾部有多余内容: " + s.substr(pos);
    return false;
  }

  // 无时区信息时必须显式给 --tz（或配置），否则拒绝 —— 见 R1 无歧义原则
  if (!has_tz) {
    if (err) {
      *err = "裸时间（无时区）有歧义，请补上 Z 或 ±HH:MM（例如 +08:00）";
    }
    return false;
  }

  const std::time_t base = timegm(&tm);   // 先按 UTC 解释
  const int64_t ns = static_cast<int64_t>(base) * 1000000000LL +
                     static_cast<int64_t>(frac * 1e9) -
                     static_cast<int64_t>(offset_sec) * 1000000000LL;
  *out = static_cast<uint64_t>(ns);
  return true;
}

}  // namespace

bool ParseTimeValue(const std::string& s, uint64_t session_created_ns,
                    uint64_t* out, std::string* err) {
  if (s.empty() || !out) {
    if (err) *err = "时间为空";
    return false;
  }

  // 1) 会话内偏移：@session+5s / @session-1s
  //    这是 docs/08 3.2 节要求的三类时间之一 —— SC-02"从故障点往前追"靠它。
  if (s[0] == '@') {
    const size_t sep = s.find_first_of("+-", 1);
    if (sep == std::string::npos) {
      if (err) *err = "@ 形式应写成 @session+5s 或 @session-1s";
      return false;
    }
    const std::string name = s.substr(1, sep - 1);
    if (name != "session" && name != "here") {
      if (err) *err = "本版本只支持 @session 作为会话内基准，收到: @" + name;
      return false;
    }
    if (session_created_ns == 0 || session_created_ns == kInvalidTimestamp) {
      if (err) *err = "会话创建时间未知，无法解释 @" + name + " 偏移";
      return false;
    }
    uint64_t delta = 0;
    if (!ParseDurationNs(s.substr(sep + 1), &delta)) {
      if (err) *err = "偏移量无法解析: " + s.substr(sep + 1);
      return false;
    }
    if (s[sep] == '-') {
      if (delta > session_created_ns) {
        if (err) *err = "会话开始前不存在该时刻";
        return false;
      }
      *out = session_created_ns - delta;
    } else {
      *out = session_created_ns + delta;
    }
    return true;
  }

  // 2) 相对现在：-30m / +2h
  if (s[0] == '-' || s[0] == '+') {
    uint64_t delta = 0;
    if (!ParseDurationNs(s.substr(1), &delta)) {
      if (err) *err = "相对时间无法解析: " + s + "（示例: -30m、+2h）";
      return false;
    }
    const uint64_t now = NowNs();
    if (s[0] == '-') {
      *out = (delta > now) ? 0 : (now - delta);
    } else {
      *out = now + delta;
    }
    return true;
  }

  // 3) 纯数字 -> Unix 纳秒
  bool all_digit = true;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) { all_digit = false; break; }
  }
  if (all_digit) {
    *out = std::strtoull(s.c_str(), nullptr, 10);
    return true;
  }

  // 4) RFC3339 绝对时刻
  return ParseRfc3339(s, out, err);
}

bool SessionExists(const std::string& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return false;
  return std::filesystem::is_directory(path, ec);
}

std::string FormatRfc3339(uint64_t ns) {
  if (ns == 0) return "(未设置)";
  const std::time_t t = static_cast<std::time_t>(ns / 1000000000ULL);
  std::tm tmv{};
  localtime_r(&t, &tmv);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tmv);
  std::string out = buf;
  // %z 给出 +0800，插入冒号变成 +08:00
  if (out.size() >= 5) {
    const size_t n = out.size();
    out = out.substr(0, n - 2) + ":" + out.substr(n - 2);
  }
  return out;
}

// ---------------------------------------------------------------------------
// 输出助手
// ---------------------------------------------------------------------------

std::string JsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else {
          o += static_cast<char>(c);
        }
    }
  }
  return o;
}

std::string HumanBytes(uint64_t n) {
  static const char* u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(n);
  int i = 0;
  while (v >= 1000.0 && i < 4) { v /= 1000.0; ++i; }
  char buf[64];
  if (i == 0) {
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(n));
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f %s", v, u[i]);
  }
  return buf;
}

std::string HumanCount(uint64_t n) {
  std::string s = std::to_string(n);
  std::string o;
  int c = 0;
  for (auto it = s.rbegin(); it != s.rend(); ++it) {
    if (c && c % 3 == 0) o += ',';
    o += *it;
    ++c;
  }
  std::reverse(o.begin(), o.end());
  return o;
}

std::string FormatTimestampNs(uint64_t ns) {
  if (ns == 0 || ns == kInvalidTimestamp) return "(未设置)";
  const double sec = static_cast<double>(ns) / 1e9;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.3f", sec);
  std::string out = buf;
  if (sec > 1.6e9 && sec < 2.0e9) {
    out += " (";
    out += FormatRfc3339(ns);
    out += ")";
  }
  return out;
}

std::string FormatOffsetNs(uint64_t base_ns, uint64_t t_ns) {
  if (t_ns == kInvalidTimestamp || t_ns < base_ns) return "-";
  const double d = static_cast<double>(t_ns - base_ns) / 1e9;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "+%.3fs", d);
  return buf;
}

void PrintTableHeader(const std::vector<std::pair<std::string, int>>& cols,
                      const GlobalOptions& g) {
  if (!g.ShowHeaders()) return;
  std::string line;
  for (const auto& c : cols) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%-*s", c.second, c.first.c_str());
    line += buf;
    line += "  ";
  }
  std::printf("%s\n", line.c_str());
  std::string sep;
  for (const auto& c : cols) sep.append(static_cast<size_t>(c.second), '-');
  std::printf("%s\n", sep.c_str());
}

int CmdNotImplemented(const std::string& name) {
  return Fail(kUsage, "子命令尚未实现: " + name,
              "本版本交付 session / verify / config / query 四组",
              "可用 `stratum --help` 查看已实现的子命令；"
              "设计见 docs/08-cli-spec.md");
}

}  // namespace cli
}  // namespace stratum
