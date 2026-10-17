// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_probe_status.cc
//
// `stratum probe status` —— 埋点运行状态体检。
//
// 需求依据：docs/13-probe-requirements.md
//   PR-11 输出**期望 vs 实际**（QoS / 格式 / 开关），两者不一致要显式标注
//   PR-12 未在配置中的探针必须**可见**（且绝不改写用户配置）
//
// ---------------------------------------------------------------------------
// 为什么"期望 vs 实际"值得单独做一条命令
// ---------------------------------------------------------------------------
// 配置文件说 `enabled = true`，但探针实际上没发数据 —— 这种"配置说谎"是最难查的：
// 看配置觉得没问题，看数据觉得探针坏了，两边都不报错。
//
// 本命令把三方摆在一起：
//   · 配置**期望**什么（来自配置文件）
//   · ring 里**实际**有什么（已提交条数、出现过哪些 topic）
//   · 两者**差异**在哪（未配置的 topic / 配置了但从未出现的 topic）
//
// ---------------------------------------------------------------------------
// 一个诚实性约束
// ---------------------------------------------------------------------------
// ring 里**没有"已注册探针表"**，只有记录。所以本命令看到的 topic 集合
// 只能来自"最近这些记录里出现过的" —— 不能假装是全量注册表。
// 输出里必须说清这一点（`sampled_from` 字段），否则运维会误以为
// "没列出来 = 这个探针不存在"。

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <set>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "cli_common.h"
#include "stratum/probe/probe.h"
#include "stratum/probe/probe_config.h"

using namespace stratum;
using namespace stratum::cli;

namespace stratum {
namespace cli {

namespace {

// ring 的只读快照（不复用 cmd_probe.cc 里那份，因为这里只需要读 + 统计）
struct RingInfo {
  bool ok = false;
  std::string error;
  uint32_t capacity = 0;
  uint64_t write_seq = 0;
  uint64_t committed = 0;
  uint32_t enabled = 0;
  uint32_t record_size = 0;
  size_t file_bytes = 0;
};

// 从 ring 里扫出的 topic 统计
struct TopicStat {
  uint64_t count = 0;
  uint64_t truncated = 0;
  uint64_t last_seq = 0;
};

}  // namespace

int CmdProbeStatus(const std::vector<std::string>& args, GlobalOptions g) {
  std::string shm;
  std::string config_path;
  size_t scan_limit = 2000;   // 最多扫多少条记录（避免超大 ring 卡住）
  uint64_t scanned_records = 0;   // 实际扫描到的有效记录数（用于 sampled_from）

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](void) -> const std::string* {
      if (i + 1 >= args.size()) return nullptr;
      return &args[++i];
    };
    if (a == "--config") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--config 缺少参数值", "", "probe 配置文件路径");
      config_path = *v;
      continue;
    }
    if (a == "--scan-limit") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--scan-limit 缺少参数值", "", "整数，例如 2000");
      scan_limit = static_cast<size_t>(std::atoll(v->c_str()));
      continue;
    }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "probe status 不支持该选项: " + a,
                  "支持 --config / --scan-limit", "完整说明见 docs/08-cli-spec.md");
    }
    if (shm.empty()) {
      shm = a;
    } else {
      return Fail(kUsage, "多余的位置参数: " + a, "", "");
    }
  }

  if (shm.empty()) {
    return Fail(kUsage, "probe status 需要共享内存路径", "位置参数缺失",
                "例如: stratum probe status /dev/shm/stratum_probe.1234");
  }

  // ---------------------------------------------------------------------
  // 1) 读 ring（只读）
  // ---------------------------------------------------------------------
  RingInfo info;
  std::map<std::string, TopicStat> observed;

  const int fd = ::open(shm.c_str(), O_RDONLY);
  if (fd < 0) {
    return Fail(kEnv, "无法打开 probe ring: " + shm,
                std::string("open 失败：") + std::strerror(errno),
                "确认业务进程已启动并调过探针；文件名形如 /dev/shm/stratum_probe.<pid>");
  }

  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return Fail(kEnv, "fstat 失败", std::string(std::strerror(errno)), "");
  }
  info.file_bytes = static_cast<size_t>(st.st_size);

  void* p = ::mmap(nullptr, info.file_bytes, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    ::close(fd);
    return Fail(kEnv, "mmap 失败", std::string(std::strerror(errno)), "");
  }

  auto* hdr = static_cast<stratum_ring_header*>(p);
  if (hdr->magic != STRATUM_RING_MAGIC || hdr->version != STRATUM_RING_VERSION) {
    ::munmap(p, info.file_bytes);
    ::close(fd);
    return Fail(kEnv, "不是有效的 probe ring: " + shm,
                "magic/版本不匹配（可能来自不同版本，或文件尚未初始化完）",
                "若是旧版本残留，删除该文件后让业务重新创建");
  }

  info.ok = true;
  info.capacity = hdr->capacity;
  info.record_size = hdr->record_size;
  info.write_seq = __atomic_load_n(&hdr->write_seq, __ATOMIC_ACQUIRE);
  info.committed = __atomic_load_n(&hdr->committed_seq, __ATOMIC_ACQUIRE);
  info.enabled = __atomic_load_n(&hdr->enabled, __ATOMIC_ACQUIRE);

  // ---------------------------------------------------------------------
  // 2) 扫现存记录，提取 topic 统计
  //
  // 只扫"环里当前还留着的"记录：从 max(0, committed-capacity) 到 committed。
  // 这就是"最近一个环周期内出现过的 topic" —— 必须如实说明。
  // ---------------------------------------------------------------------
  {
    const uint32_t cap = info.capacity;
    const uint64_t begin = (info.committed > (uint64_t)cap)
                               ? info.committed - (uint64_t)cap
                               : 0;
    const uint64_t end = info.committed;
    uint64_t scanned = 0;
    for (uint64_t seq = end; seq > begin && scanned < scan_limit; --seq) {
      const uint64_t idx = seq - 1;
      const uint32_t slot = static_cast<uint32_t>(idx % cap);
      const stratum_probe_record* rec = stratum_ring_records_const(hdr) + slot;
      if (rec->seq != static_cast<uint32_t>(idx)) continue;   // 已被覆盖
      ++scanned_records;
      const std::string topic(rec->topic, rec->topic_len);
      if (topic.empty()) continue;
      TopicStat& ts = observed[topic];
      ++ts.count;
      if ((rec->flags & 1u) != 0) ++ts.truncated;
      ts.last_seq = rec->seq;
      ++scanned;
    }
  }

  ::munmap(p, info.file_bytes);
  ::close(fd);

  // ---------------------------------------------------------------------
  // 3) 读配置（期望值）
  // ---------------------------------------------------------------------
  probe::ProbeConfig cfg;
  bool have_cfg = !config_path.empty();
  probe::LoadOutcome cfg_outcome = probe::LoadOutcome::kMissing;
  std::string cfg_detail;
  if (have_cfg) {
    const probe::LoadReport rep = cfg.Load(config_path);
    cfg_outcome = rep.outcome;
    cfg_detail = rep.detail;
  }
  const probe::ProbeSettings settings = cfg.Snapshot();
  const std::vector<std::string> declared = cfg.DeclaredTopics();

  // 配置里声明了、但环里从未出现的 topic（可能是：业务没调用 / 开关全关 / 已被覆盖）
  std::set<std::string> declared_set(declared.begin(), declared.end());
  std::vector<std::string> declared_but_unseen;
  for (const auto& t : declared) {
    if (observed.find(t) == observed.end()) declared_but_unseen.push_back(t);
  }
  // 环里出现过、但配置里没有的 topic（PR-12：必须可见）
  std::vector<std::string> observed_not_configured;
  for (const auto& kv : observed) {
    if (have_cfg && declared_set.find(kv.first) == declared_set.end()) {
      observed_not_configured.push_back(kv.first);
    }
  }

  // ---------------------------------------------------------------------
  // 4) 输出
  // ---------------------------------------------------------------------
  if (g.format == Format::kJson) {
    std::printf("{\"schema_version\":1,");
    std::printf("\"shm\":\"%s\",", JsonEscape(shm).c_str());
    std::printf("\"ring\":{\"capacity\":%u,\"record_size\":%u,"
                "\"write_seq\":%llu,\"committed\":%llu,\"enabled\":%s,"
                "\"file_bytes\":%zu},",
                info.capacity, info.record_size,
                static_cast<unsigned long long>(info.write_seq),
                static_cast<unsigned long long>(info.committed),
                info.enabled ? "true" : "false", info.file_bytes);
    if (have_cfg) {
      std::printf("\"config\":{\"path\":\"%s\",\"outcome\":\"%s\","
                  "\"detail\":\"%s\",\"global_enabled\":%s,\"declared\":%zu},",
                  JsonEscape(config_path).c_str(),
                  probe::LoadOutcomeName(cfg_outcome),
                  JsonEscape(cfg_detail).c_str(),
                  settings.global_enabled ? "true" : "false", declared.size());
    } else {
      std::printf("\"config\":null,");
    }
    std::printf("\"observed_topics\":[");
    bool first = true;
    for (const auto& kv : observed) {
      if (!first) std::printf(",");
      first = false;
      // 期望 vs 实际
      bool want = true;
      if (have_cfg) want = cfg.Resolve(kv.first).enabled;
      const bool in_cfg = declared_set.find(kv.first) != declared_set.end();
      std::printf("{\"topic\":\"%s\",\"count\":%llu,\"truncated\":%llu,"
                  "\"configured\":%s,\"expected_enabled\":%s,"
                  "\"mismatch\":%s}",
                  JsonEscape(kv.first).c_str(),
                  static_cast<unsigned long long>(kv.second.count),
                  static_cast<unsigned long long>(kv.second.truncated),
                  in_cfg ? "true" : "false", want ? "true" : "false",
                  (have_cfg && !in_cfg) ? "true" : "false");
    }
    std::printf("],");
    std::printf("\"observed_not_configured\":[");
    for (size_t i = 0; i < observed_not_configured.size(); ++i) {
      std::printf("%s\"%s\"", i ? "," : "",
                  JsonEscape(observed_not_configured[i]).c_str());
    }
    std::printf("],");
    std::printf("\"declared_but_unseen\":[");
    for (size_t i = 0; i < declared_but_unseen.size(); ++i) {
      std::printf("%s\"%s\"", i ? "," : "",
                  JsonEscape(declared_but_unseen[i]).c_str());
    }
    std::printf("],");
    std::printf("\"sampled_from\":\"扫了 ring 里最近 %llu 条记录，"
                "从中看到 %zu 个 topic（不是全量注册表）\"}",
                static_cast<unsigned long long>(scanned_records), observed.size());
    std::printf("\n");
    // 【不要在这里 return】退出码必须与输出格式无关。
    // 早期版本 JSON 分支直接 return kOk，于是"有未配置 topic"在 JSON 模式下
    // 不影响退出码 —— 消费 JSON 的脚本永远看不到不一致。
    return observed_not_configured.empty() ? kOk : kPartial;
  }

  // 表格输出
  std::printf("probe ring: %s\n", shm.c_str());
  std::printf("  容量:     %u 条（每条 %u 字节）\n", info.capacity, info.record_size);
  std::printf("  已分配:   %llu\n", static_cast<unsigned long long>(info.write_seq));
  std::printf("  已提交:   %llu\n", static_cast<unsigned long long>(info.committed));
  std::printf("  全局开关: %s", info.enabled ? "开" : "关（所有探针静默）");
  if (info.committed > (uint64_t)info.capacity) {
    std::printf("  [环已覆盖过一次]");
  }
  std::printf("\n");

  if (have_cfg) {
    std::printf("\n配置: %s\n", config_path.c_str());
    std::printf("  加载结果: %s", probe::LoadOutcomeName(cfg_outcome));
    if (!cfg_detail.empty()) std::printf("（%s）", cfg_detail.c_str());
    std::printf("\n");
    std::printf("  全局开关（期望）: %s\n", settings.global_enabled ? "开" : "关");
    std::printf("  已声明 topic:    %zu 个\n", declared.size());
    if (cfg_outcome == probe::LoadOutcome::kParseError) {
      std::printf("  ** 注意：解析失败，按 fail-safe 语义全局已关闭 **\n");
    }
  } else {
    std::printf("\n配置: （未指定 --config，无法对比期望值）\n");
  }

  std::printf("\n环内出现过的 topic（最近 %zu 条记录）:\n", observed.size());
  if (observed.empty()) {
    std::printf("  （无 —— 业务还没写入，或记录已被覆盖）\n");
  } else {
    for (const auto& kv : observed) {
      const bool in_cfg = declared_set.find(kv.first) != declared_set.end();
      bool want = true;
      if (have_cfg) want = cfg.Resolve(kv.first).enabled;
      std::printf("  %-32s %8llu 条", kv.first.c_str(),
                  static_cast<unsigned long long>(kv.second.count));
      // 【关键】只有**给了配置**时才能说"未在配置中"。
      // 没给配置时说这句话，等于把"没有配置可对比"讲成"配置里缺这个" ——
      // 语义完全不同，会让运维以为配置文件丢了内容。
      if (have_cfg && !in_cfg) {
        std::printf("  [未在配置中：用默认值运行，实际=%s]", want ? "开" : "关");
      } else if (have_cfg && !want) {
        std::printf("  [配置为关]");
      }
      if (kv.second.truncated) {
        std::printf("  [截断 %llu 条]",
                    static_cast<unsigned long long>(kv.second.truncated));
      }
      std::printf("\n");
    }
  }

  if (!observed_not_configured.empty()) {
    std::printf("\n** 有 %zu 个 topic 未出现在配置文件中 **\n",
                observed_not_configured.size());
    std::printf("   它们正在用默认值运行。若要控制它们的开关，请手工加入配置文件：\n");
    for (const auto& t : observed_not_configured) {
      std::printf("     [topic.\"%s\"]\n     enabled = true\n", t.c_str());
    }
    std::printf("   （本工具**不会**自动改写你的配置文件 —— 那会覆盖你的人工注释）\n");
  }

  if (!declared_but_unseen.empty()) {
    std::printf("\n配置里声明、但环内没有出现的 topic（%zu 个）:\n",
                declared_but_unseen.size());
    std::printf("   可能原因：业务尚未调用该探针 / 被配置关闭 / 记录已被环覆盖\n");
    for (const auto& t : declared_but_unseen) {
      const bool want = cfg.Resolve(t).enabled;
      std::printf("  %-32s 期望=%s\n", t.c_str(), want ? "开" : "关");
    }
  }

  std::printf("\n说明: topic 列表来自 ring 里仍有留存的记录，"
              "**不是**全量探针注册表。\n");

  // 有"期望与实际不一致"时返回 PARTIAL，便于脚本察觉
  if (!observed_not_configured.empty()) return kPartial;
  return kOk;
}

}  // namespace cli
}  // namespace stratum
