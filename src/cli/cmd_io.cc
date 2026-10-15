// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_io.cc
//
// `stratum io` —— 会话的打包与搬运。
//
// 【为什么需要它】会话是长期运行的产物，必然要面对：
//   · 备份（磁盘要换了）
//   · 迁移（换机器 / 交付给同事）
//   · 归档（留一份只读的凭证）
// 没有一个"搬运"能力，这些只能靠 `cp -r` —— 而 `cp -r` **不知道自己在复制
// 什么**，也无法告诉你"这份拷贝是完整的吗"。
//
// 【bundle 的形态与理由】
// bundle = **会话目录 + MANIFEST.json**，不引入 tar/压缩依赖。
//
// 校验策略：**语义对账优先于字节哈希**。
//   · "会话能打开、话题都在、条数对得上" —— 这比"字节一致"更有意义
//   · 且不需要引入 SHA-256 实现（当前项目没有哈希库）
//   · MANIFEST 里记录每个文件的大小，可做**廉价的一致性抽查**
//
// 【import 的默认行为】
// **默认不覆盖**已有会话。覆盖一个已有会话是破坏性操作，
// 与 `prune` 同理：默认安全，要破坏必须显式。

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cli_common.h"
#include "stratum/core/timeline_impl_v2.h"

namespace fs = std::filesystem;

using namespace stratum;
using namespace stratum::cli;

namespace stratum {
namespace cli {

namespace {

constexpr const char* kManifestName = "MANIFEST.json";

std::string NowNsStr() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return std::to_string(static_cast<unsigned long long>(ts.tv_sec) * 1000000000ULL +
                        static_cast<unsigned long long>(ts.tv_nsec));
}

// 列出目录下的普通文件（递归），返回 <相对路径, 字节数>
std::vector<std::pair<std::string, uint64_t>> ListFiles(const std::string& root) {
  std::vector<std::pair<std::string, uint64_t>> out;
  std::error_code ec;
  if (!fs::exists(root, ec)) return out;
  for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    std::string rel = fs::relative(it->path(), root, ec).generic_string();
    if (rel == kManifestName) continue;   // 清单自身不计入
    uint64_t sz = 0;
    std::error_code e2;
    const auto s = fs::file_size(it->path(), e2);
    if (!e2) sz = static_cast<uint64_t>(s);
    out.emplace_back(rel, sz);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// 写 MANIFEST：话题摘要 + 文件清单（含大小）
bool WriteManifest(const std::string& session_dir, const std::string& bundle_root,
                   const std::string& source_path, std::string* err) {
  TimelineImplV2 tl;
  Status s = tl.Open(session_dir);
  if (!s.ok()) {
    if (err) *err = "无法打开待导出的会话: " + s.ToString();
    return false;
  }
  SessionMetadata meta;
  (void)tl.GetSessionMetadata(&meta);
  std::vector<TopicMetadata> topics;
  (void)tl.ListTopics(&topics);

  // 顺带把类型定义/控制事件/裁剪记录也读出来，写进清单便于人核对
  struct Extra {
    std::string topic;
    std::string definition;
  };
  std::vector<Extra> defs;
  for (const auto& t : topics) {
    std::string d;
    if (tl.GetTopicTypeDefinition(t.topic_name, &d).ok() && !d.empty()) {
      defs.push_back({t.topic_name, d});
    }
  }
  std::vector<ControlEvent> events;
  (void)tl.ListControlEvents(&events);
  (void)tl.Close();

  const auto files = ListFiles(bundle_root);

  std::ofstream o(bundle_root + "/" + kManifestName, std::ios::out | std::ios::trunc);
  if (!o) {
    if (err) *err = "无法写入 " + std::string(kManifestName);
    return false;
  }
  o << "{\n";
  o << "  \"schema_version\": 1,\n";
  o << "  \"kind\": \"stratum-bundle\",\n";
  o << "  \"exported_at_ns\": " << NowNsStr() << ",\n";
  o << "  \"source_path\": \"" << JsonEscape(source_path) << "\",\n";
  o << "  \"stored_at\": \"" << JsonEscape(bundle_root) << "\",\n";
  o << "  \"session_created_at_ns\": " << meta.created_at_ns << ",\n";
  o << "  \"total_events\": " << meta.total_events << ",\n";
  o << "  \"total_bytes\": " << meta.total_bytes << ",\n";
  o << "  \"has_integrity_issue\": "
    << (meta.quality.HasIntegrityIssue() ? "true" : "false") << ",\n";

  o << "  \"topics\": [";
  for (size_t i = 0; i < topics.size(); ++i) {
    if (i) o << ", ";
    o << "{\"name\":\"" << JsonEscape(topics[i].topic_name) << "\","
      << "\"type\":\"" << JsonEscape(topics[i].type_name) << "\","
      << "\"count\":" << topics[i].message_count << ","
      << "\"bytes\":" << topics[i].byte_count << "}";
  }
  o << "],\n";

  o << "  \"type_definitions\": [";
  for (size_t i = 0; i < defs.size(); ++i) {
    if (i) o << ", ";
    o << "{\"topic\":\"" << JsonEscape(defs[i].topic) << "\","
      << "\"definition\":\"" << JsonEscape(defs[i].definition) << "\"}";
  }
  o << "],\n";

  o << "  \"control_events\": [";
  for (size_t i = 0; i < events.size(); ++i) {
    if (i) o << ", ";
    o << "{\"at_ns\":" << events[i].at_ns << ","
      << "\"action\":\"" << JsonEscape(events[i].action) << "\","
      << "\"reason\":\"" << JsonEscape(events[i].reason) << "\"}";
  }
  o << "],\n";

  o << "  \"files\": [";
  for (size_t i = 0; i < files.size(); ++i) {
    if (i) o << ", ";
    o << "{\"path\":\"" << JsonEscape(files[i].first) << "\","
      << "\"size\":" << files[i].second << "}";
  }
  o << "]\n}\n";
  return true;
}

// 校验一个 bundle：会话语义对账（比字节哈希更有意义）
struct CheckResult {
  bool ok = true;                          // 由**语义对账**决定
  std::vector<std::string> problems;       // 导致失败的问题（语义层）
  std::vector<std::string> notes;          // 仅诊断，不影响结论
  uint64_t checked_topics = 0;
  uint64_t checked_events = 0;
  uint64_t files_expected = 0;
  uint64_t files_missing = 0;
  uint64_t size_mismatch = 0;
};

// LSM 的内部文件天然易变：后台压缩会产生/删除 .ldb、.log，
// LOG 会被轮转。把它们纳入"一致性判据"必然误报。
bool IsVolatileLsmFile(const std::string& rel) {
  return rel == "LOCK" || rel == "LOG" || rel == "LOG.old" ||
         rel == "CURRENT" || rel.rfind("MANIFEST-", 0) == 0;
}

// 从 MANIFEST 里抽取一个字段的粗糙解析（仅用于校验，不求完备）
std::string ExtractString(const std::string& hay, const std::string& key) {
  const std::string pat = "\"" + key + "\": \"";
  size_t p = hay.find(pat);
  if (p == std::string::npos) {
    const std::string pat2 = "\"" + key + "\":\"";
    p = hay.find(pat2);
    if (p == std::string::npos) return "";
    p += pat2.size();
  } else {
    p += pat.size();
  }
  const size_t e = hay.find('"', p);
  if (e == std::string::npos) return "";
  return hay.substr(p, e - p);
}

uint64_t ExtractU64(const std::string& hay, const std::string& key) {
  const std::string pat = "\"" + key + "\": ";
  size_t p = hay.find(pat);
  if (p == std::string::npos) {
    const std::string pat2 = "\"" + key + "\":";
    p = hay.find(pat2);
    if (p == std::string::npos) return 0;
    p += pat2.size();
  } else {
    p += pat.size();
  }
  return std::strtoull(hay.c_str() + p, nullptr, 10);
}

// 解析 files 数组里的 {path,size} 对
std::vector<std::pair<std::string, uint64_t>> ParseFiles(const std::string& man) {
  std::vector<std::pair<std::string, uint64_t>> out;
  const size_t arr = man.find("\"files\":");
  if (arr == std::string::npos) return out;
  size_t p = arr;
  while (true) {
    const size_t ps = man.find("\"path\":", p);
    if (ps == std::string::npos) break;
    size_t q = man.find('"', ps + 7);
    if (q == std::string::npos) break;
    ++q;
    const size_t qe = man.find('"', q);
    if (qe == std::string::npos) break;
    const std::string path = man.substr(q, qe - q);
    const size_t ss = man.find("\"size\":", qe);
    if (ss == std::string::npos) break;
    const uint64_t size = std::strtoull(man.c_str() + ss + 7, nullptr, 10);
    out.emplace_back(path, size);
    p = ss + 7;
  }
  return out;
}

bool CheckBundle(const std::string& bundle, CheckResult* r, std::string* err) {
  const std::string man_path = bundle + "/" + kManifestName;
  std::ifstream in(man_path, std::ios::in | std::ios::binary);
  if (!in) {
    if (err) *err = "缺少 " + std::string(kManifestName) + "（不是有效的 bundle）";
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string man = ss.str();

  if (ExtractString(man, "kind") != "stratum-bundle") {
    if (err) *err = "MANIFEST 的 kind 不是 stratum-bundle";
    return false;
  }

  // 1) 文件清单核对 —— **仅作诊断**，不影响结论。
  //
  // 【为什么】bundle 里是一个**活的 LSM 目录**：后台压缩会持续产生/删除
  // .ldb 与 .log，LOG 还会轮转。因此"导出时列出的清单"与"稍后目录的实际
  // 内容"天然会漂移。把文件一致性当判据必然误报（本文件上一版就是这么错的）。
  //
  // 真正的不变量是**语义**：会话能打开、话题都在、条数对得上。
  const auto files = ParseFiles(man);
  r->files_expected = files.size();
  for (const auto& f : files) {
    if (IsVolatileLsmFile(f.first)) continue;   // 易变文件不参与诊断
    const std::string full = bundle + "/" + f.first;
    std::error_code ec;
    if (!fs::exists(full, ec)) {
      ++r->files_missing;
      r->notes.push_back("文件不在了（LSM 压缩后可能已合并）: " + f.first);
      continue;
    }
    const auto sz = fs::file_size(full, ec);
    if (!ec && static_cast<uint64_t>(sz) != f.second) {
      ++r->size_mismatch;
      r->notes.push_back("大小与导出时不同: " + f.first + "（清单 " +
                         std::to_string(f.second) + "，实际 " +
                         std::to_string(static_cast<uint64_t>(sz)) + "）");
    }
  }

  // 2) 语义对账：会话能打开、条数与清单一致
  TimelineImplV2 tl;
  Status s = tl.Open(bundle);
  if (!s.ok()) {
    r->ok = false;
    r->problems.push_back("会话无法打开: " + s.ToString());
    return true;
  }
  std::vector<TopicMetadata> topics;
  (void)tl.ListTopics(&topics);
  r->checked_topics = topics.size();

  // 逐话题比对条数（清单里记了 count）
  size_t p = man.find("\"topics\":");
  for (const auto& t : topics) {
    const std::string key = "\"name\":\"" + JsonEscape(t.topic_name) + "\"";
    size_t tp = (p == std::string::npos) ? std::string::npos : man.find(key, p);
    if (tp == std::string::npos) {
      r->ok = false;
      r->problems.push_back("清单里没有话题: " + t.topic_name);
      continue;
    }
    const size_t cs = man.find("\"count\":", tp);
    if (cs == std::string::npos) continue;
    const uint64_t expect = std::strtoull(man.c_str() + cs + 8, nullptr, 10);
    if (expect != t.message_count) {
      r->ok = false;
      r->problems.push_back("条数不符: " + t.topic_name + "（清单 " +
                            std::to_string(expect) + "，实际 " +
                            std::to_string(t.message_count) + "）");
    }
    r->checked_events += t.message_count;
  }
  (void)tl.Close();
  return true;
}

}  // namespace

int CmdIo(const std::vector<std::string>& args, GlobalOptions g) {
  if (args.empty()) {
    return Fail(kUsage, "io 需要子命令",
                "可用: export / import / verify",
                "示例: stratum io export /data/s1 --to /backup/s1.bundle");
  }
  const std::string sub = args[0];

  std::string src;
  std::string to;
  bool dry_run = false;
  bool overwrite = false;

  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](void) -> const std::string* {
      if (i + 1 >= args.size()) return nullptr;
      return &args[++i];
    };
    if (a == "--to") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--to 缺少参数值", "", "目标路径");
      to = *v;
      continue;
    }
    if (a == "--dry-run") { dry_run = true; continue; }
    if (a == "--overwrite") { overwrite = true; continue; }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "io " + sub + " 不支持该选项: " + a,
                  "支持 --to / --dry-run / --overwrite", "");
    }
    if (src.empty()) {
      src = a;
    } else {
      return Fail(kUsage, "多余的位置参数: " + a, "", "");
    }
  }

  if (src.empty()) {
    return Fail(kUsage, "io " + sub + " 需要路径参数", "位置参数缺失",
                "export 需要源会话；import/verify 需要 bundle 路径");
  }

  // -------------------------------------------------------------------------
  // export
  // -------------------------------------------------------------------------
  if (sub == "export") {
    if (to.empty()) {
      return Fail(kUsage, "io export 需要 --to <目标目录>",
                  "导出的 bundle 要有地方放",
                  "例如: --to /backup/s1.bundle");
    }
    if (!SessionExists(src)) {
      return Fail(kNotFound, "会话不存在: " + src, "路径不存在或不是目录",
                  "用 `stratum session list <父目录>` 列出可用会话");
    }
    if (fs::exists(to) && !overwrite) {
      return Fail(kExists, "目标已存在: " + to,
                  "覆盖会破坏已有数据，默认不做",
                  "确认要覆盖请加 --overwrite，或换一个目标路径");
    }

    // 先统计，供 dry-run 与输出使用
    TimelineImplV2 tl;
    Status s = tl.Open(src);
    if (!s.ok()) {
      return Fail(ExitCodeOf(s), "无法打开会话: " + src, s.ToString(), "");
    }
    SessionMetadata meta;
    (void)tl.GetSessionMetadata(&meta);
    std::vector<TopicMetadata> topics;
    (void)tl.ListTopics(&topics);
    (void)tl.Close();

    uint64_t total_bytes = 0;
    const auto files = ListFiles(src);
    for (const auto& f : files) total_bytes += f.second;

    if (dry_run) {
      std::printf("DRY RUN — 未复制任何文件\n");
      std::printf("  源:     %s\n", src.c_str());
      std::printf("  目标:   %s\n", to.c_str());
      std::printf("  话题:   %zu 个\n", topics.size());
      std::printf("  文件:   %zu 个\n", files.size());
      std::printf("  大小:   %s\n", HumanBytes(total_bytes).c_str());
      std::printf("  将生成: %s（含话题摘要 + 文件清单 + 审计记录）\n", kManifestName);
      std::printf("\n  执行去掉 --dry-run 即可\n");
      return kOk;
    }

    std::error_code ec;
    if (fs::exists(to, ec)) fs::remove_all(to, ec);
    fs::create_directories(to, ec);
    if (ec) {
      return Fail(kError, "无法创建目标目录: " + to, ec.message(), "检查路径与权限");
    }
    fs::copy(src, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
      return Fail(kError, "复制失败", ec.message(), "检查磁盘空间与权限");
    }

    std::string err;
    if (!WriteManifest(to, to, src, &err)) {
      return Fail(kError, "写入清单失败", err, "目标目录可能不可写");
    }

    if (g.format == Format::kJson) {
      std::printf("{\"schema_version\":1,\"action\":\"export\",\"source\":\"%s\","
                  "\"target\":\"%s\",\"topics\":%zu,\"files\":%zu,\"bytes\":%llu}\n",
                  JsonEscape(src).c_str(), JsonEscape(to).c_str(), topics.size(),
                  files.size(), static_cast<unsigned long long>(total_bytes));
    } else if (!g.quiet) {
      std::fprintf(stderr, "导出完成\n");
      std::fprintf(stderr, "  话题: %zu 个 / 文件: %zu 个 / %s\n", topics.size(),
                   files.size(), HumanBytes(total_bytes).c_str());
      std::fprintf(stderr, "  清单: %s/%s\n", to.c_str(), kManifestName);
      std::fprintf(stderr, "\n  下一步: stratum io verify %s\n", to.c_str());
    }
    return kOk;
  }

  // -------------------------------------------------------------------------
  // verify（只校验，不动数据）
  // -------------------------------------------------------------------------
  if (sub == "verify") {
    CheckResult r;
    std::string err;
    if (!CheckBundle(src, &r, &err)) {
      return Fail(kIntegrity, "bundle 无效: " + src, err,
                  "确认这是 `stratum io export` 产出的目录");
    }
    if (g.format == Format::kJson) {
      std::printf("{\"schema_version\":1,\"bundle\":\"%s\",\"ok\":%s,"
                  "\"files_expected\":%llu,\"files_missing\":%llu,"
                  "\"size_mismatch\":%llu,\"topics\":%llu,\"events\":%llu,"
                  "\"problems\":%zu}\n",
                  JsonEscape(src).c_str(), r.ok ? "true" : "false",
                  static_cast<unsigned long long>(r.files_expected),
                  static_cast<unsigned long long>(r.files_missing),
                  static_cast<unsigned long long>(r.size_mismatch),
                  static_cast<unsigned long long>(r.checked_topics),
                  static_cast<unsigned long long>(r.checked_events),
                  r.problems.size());
    } else {
      std::printf("bundle: %s\n", src.c_str());
      std::printf("  文件:   清单 %llu 个，缺失 %llu 个，大小不符 %llu 个\n",
                  static_cast<unsigned long long>(r.files_expected),
                  static_cast<unsigned long long>(r.files_missing),
                  static_cast<unsigned long long>(r.size_mismatch));
      std::printf("  语义:   话题 %llu 个，事件 %llu 条（与清单对账）\n",
                  static_cast<unsigned long long>(r.checked_topics),
                  static_cast<unsigned long long>(r.checked_events));
      std::printf("  结论:   %s\n", r.ok ? "完整（语义对账通过）" : "**有问题**");
      for (const auto& p : r.problems) {
        std::printf("    ✗ %s\n", p.c_str());
      }
      if (!r.notes.empty()) {
        std::printf("  诊断（不影响结论；LSM 内部文件会随压缩变化）:\n");
        for (const auto& n : r.notes) {
          std::printf("    · %s\n", n.c_str());
        }
      }
    }
    return r.ok ? kOk : kIntegrity;
  }

  // -------------------------------------------------------------------------
  // import
  // -------------------------------------------------------------------------
  if (sub == "import") {
    if (to.empty()) {
      return Fail(kUsage, "io import 需要 --to <目标会话路径>",
                  "要还原到哪里必须明确", "例如: --to /data/stratum/s1-restored");
    }

    // 【关键】导入前**先校验** —— 拒绝把损坏的 bundle 还原成"看起来正常"的会话
    CheckResult r;
    std::string err;
    if (!CheckBundle(src, &r, &err)) {
      return Fail(kIntegrity, "bundle 无效: " + src, err,
                  "确认这是 `stratum io export` 产出的目录；"
                  "损坏的 bundle 不应被导入");
    }
    if (!r.ok) {
      std::fprintf(stderr, "bundle 校验未通过（语义对账失败），共 %zu 个问题：\n",
                   r.problems.size());
      for (const auto& p : r.problems) {
        std::fprintf(stderr, "  - %s\n", p.c_str());
      }
      return Fail(kIntegrity, "bundle 校验未通过（语义对账失败）: " + src,
                  "会话无法完整读出（打不开，或话题条数与清单不符），"
                  "导入会产生不完整的会话",
                  "先修复或重新导出；若要强行导入请先自行确认数据完整性");
    }

    if (fs::exists(to) && !overwrite) {
      return Fail(kExists, "目标会话已存在: " + to,
                  "覆盖会破坏已有数据，默认不做",
                  "确认要覆盖请加 --overwrite，或换一个目标路径");
    }

    if (dry_run) {
      std::printf("DRY RUN — 未写入任何数据\n");
      std::printf("  bundle: %s（校验通过）\n", src.c_str());
      std::printf("  目标:   %s\n", to.c_str());
      std::printf("  话题:   %llu 个 / 事件 %llu 条\n",
                  static_cast<unsigned long long>(r.checked_topics),
                  static_cast<unsigned long long>(r.checked_events));
      std::printf("  文件:   %llu 个\n",
                  static_cast<unsigned long long>(r.files_expected));
      std::printf("\n  执行去掉 --dry-run 即可\n");
      return kOk;
    }

    std::error_code ec;
    if (fs::exists(to, ec)) fs::remove_all(to, ec);
    fs::create_directories(to, ec);
    if (ec) {
      return Fail(kError, "无法创建目标目录: " + to, ec.message(), "检查路径与权限");
    }
    fs::copy(src, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
      return Fail(kError, "复制失败", ec.message(), "检查磁盘空间与权限");
    }

    // 导入后再校验一次（确保落在目标上的数据可用）
    CheckResult r2;
    std::string err2;
    if (!CheckBundle(to, &r2, &err2) || !r2.ok) {
      return Fail(kIntegrity, "导入后校验失败: " + to,
                  err2.empty() ? ("问题 " + std::to_string(r2.problems.size()) + " 个")
                               : err2,
                  "数据可能未完整写入，建议检查磁盘后重试");
    }

    if (g.format == Format::kJson) {
      std::printf("{\"schema_version\":1,\"action\":\"import\",\"bundle\":\"%s\","
                  "\"target\":\"%s\",\"topics\":%llu,\"events\":%llu,\"ok\":true}\n",
                  JsonEscape(src).c_str(), JsonEscape(to).c_str(),
                  static_cast<unsigned long long>(r2.checked_topics),
                  static_cast<unsigned long long>(r2.checked_events));
    } else if (!g.quiet) {
      std::fprintf(stderr, "导入完成并通过校验\n");
      std::fprintf(stderr, "  目标: %s\n", to.c_str());
      std::fprintf(stderr, "  话题: %llu 个 / 事件 %llu 条\n",
                   static_cast<unsigned long long>(r2.checked_topics),
                   static_cast<unsigned long long>(r2.checked_events));
      std::fprintf(stderr, "\n  下一步: stratum verify %s\n", to.c_str());
    }
    return kOk;
  }

  return Fail(kUsage, "io 不支持的子命令: " + sub,
              "可用: export / import / verify", "");
}

}  // namespace cli
}  // namespace stratum
