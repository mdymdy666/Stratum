// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cli_common.cc
#include "cli_common.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>

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
  // 千分位
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
  if (ns == 0) return "(未设置)";
  // 项目的时间戳是单调/系统纳秒；这里给出秒级可读形式
  const double sec = static_cast<double>(ns) / 1e9;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.3f", sec);
  std::string out = buf;
  // 附上 UTC 形式（若数值落在合理区间）
  if (sec > 1.6e9 && sec < 2.0e9) {
    const std::time_t t = static_cast<std::time_t>(sec);
    std::tm tmv{};
    gmtime_r(&t, &tmv);
    char tbuf[32];
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", &tmv);
    out += " (";
    out += tbuf;
    out += "Z)";
  }
  return out;
}

std::string FormatOffsetNs(uint64_t base_ns, uint64_t t_ns) {
  if (t_ns < base_ns) return "-";
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
              "本版本先交付 session / verify / config 三组",
              "可用 `stratum --help` 查看已实现的子命令；"
              "设计见 docs/08-cli-spec.md");
}

}  // namespace cli
}  // namespace stratum
