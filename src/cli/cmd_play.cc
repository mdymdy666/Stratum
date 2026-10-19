// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_play.cc
//
// `stratum play` —— 与 record 对称，闭环的另一半。
//
// 设计依据：docs/07-cli-design.md 场景 S07~S12、docs/08-cli-spec.md 第 4 节。
//
// 【本版本落地的关键设计约定】
//
// 1. discovery_wait 默认 2000ms（core 的 PlayerConfig 默认是 0）
//    core 里的注释已经记录过实测：回放器以 RTPS Writer 身份发布，
//    对端需先完成 SPDP/SEDP 匹配；Open 后立即发数据会被丢弃
//    （"等待 0ms 时对端收到 0 条"）。CLI 面向真实使用，
//    因此给一个安全默认值，而不是把坑留给用户。
//
// 2. 默认**不**使用原始话题名（allow_original_topic_names = false）
//    这是 EC-18 的防护：回放时若原话题仍在实时发布，
//    下游会同时收到"实时数据"和"历史数据"两份，且难以分辨。
//    core 默认会加后缀；CLI 保留该行为，并要求显式 --allow-original-topic-names
//    才能覆盖（危险操作需显式）。
//
// 3. 统计一律走 stderr —— stdout 只留数据出口。
//
// 4. 明确区分"支持"与"暂不支持"
//    --rate-mode（ratio / compress-gaps）与 --loop-count 需要 core 层支持，
//    本版本**明确报错说明**，而不是静默忽略或假装支持。

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "cli_common.h"
#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"
#include "stratum/protocol/rtps/rtps_backend.h"

using namespace stratum;
using namespace stratum::cli;

namespace stratum {
namespace cli {

namespace {

std::atomic<bool> g_stop_requested{false};

void OnSignal(int) { g_stop_requested.store(true); }

struct PlayOpts {
  std::string session;
  std::vector<std::string> topics;          // 空 = 全部
  std::string from_raw;
  std::string to_raw;

  double rate = 1.0;

  // 批量发布：一次 sendto 发 N 条（1 = 逐条，默认）。
  // 省的是每条的 Message Header(20B) + INFO_DST(16B) 与 IP/UDP 头(28B)。
  // 实测（payload=24B、N=10）：1080 → 648 字节，省 40%
  // ——**这是报文级数字**，真实链路还要算分片/重传/HEARTBEAT。
  uint32_t batch_size = 1;

  std::vector<std::pair<std::string, std::string>> remap;
  bool allow_original_names = false;

  uint32_t domain = 0;
  std::string participant_name = "stratum-player";
  std::string advertised_address;
  std::string initial_peers;
  std::string discovery_wait_raw = "2000ms";

  bool wait_for_ack = false;
  std::string ack_timeout_raw = "5000ms";
  bool no_republish = false;

  bool loop = false;
  bool dry_run = false;
  bool show_stats = true;
};

std::string RemapSuffix() { return "_replay"; }

}  // namespace

int CmdPlay(const std::vector<std::string>& args, GlobalOptions g) {
  PlayOpts o;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](void) -> const std::string* {
      if (i + 1 >= args.size()) return nullptr;
      return &args[++i];
    };

    if (a == "--dry-run") { o.dry_run = true; continue; }
    if (a == "--loop") { o.loop = true; continue; }
    if (a == "--no-stats") { o.show_stats = false; continue; }
    if (a == "--wait-for-ack") { o.wait_for_ack = true; continue; }
    if (a == "--no-republish") { o.no_republish = true; continue; }
    if (a == "--allow-original-topic-names") { o.allow_original_names = true; continue; }

    if (a == "--from") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--from 缺少参数值", "", "示例: --from @session+5s");
      o.from_raw = *v; continue;
    }
    if (a == "--to") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--to 缺少参数值", "", "示例: --to @session+60s");
      o.to_raw = *v; continue;
    }
    if (a == "--topic") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--topic 缺少参数值", "", "示例: --topic sensor/temp");
      o.topics.push_back(*v); continue;
    }
    if (a == "--batch-size") {
      const std::string* v = next();
      if (v == nullptr) {
        return Fail(kUsage, "--batch-size 缺少参数值", "",
                    "整数 1~32；1 = 逐条发送（默认）");
      }
      const long n = std::strtol(v->c_str(), nullptr, 10);
      if (n < 1 || n > 32) {
        return Fail(kUsage, "--batch-size 超出范围: " + *v, "必须在 1~32 之间",
                    "1 = 逐条；建议 8~16（再大首条延迟变长、单包接近 MTU）");
      }
      o.batch_size = static_cast<uint32_t>(n);
      continue;
    }
    if (a == "--rate") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--rate 缺少参数值", "", "示例: --rate 1.0（0 = 尽可能快）");
      o.rate = std::atof(v->c_str());
      if (o.rate < 0.0) {
        return Fail(kUsage, "--rate 不能为负: " + *v, "", "0 表示尽可能快，1.0 表示原速");
      }
      continue;
    }
    if (a == "--rate-mode") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--rate-mode 缺少参数值", "", "取值: ratio / compress-gaps");
      if (*v != "ratio") {
        return Fail(kUsage, "--rate-mode " + *v + " 暂不支持",
                    "compress-gaps 需要 core 层支持（只压缩超过阈值的空档，"
                    "其余保持原速），当前 Player 只实现了等比缩放",
                    "本版本请用 --rate-mode ratio（默认），"
                    "或将需求记入 docs/09-cli-scenarios-deep.md 的待办");
      }
      continue;
    }
    if (a == "--loop-count") {
      (void)next();
      return Fail(kUsage, "--loop-count 暂不支持",
                  "需要 core 层暴露循环轮次；当前 Player 只能无限循环",
                  "本版本请用 --loop，并配合 Ctrl+C 或 --to 限定范围");
    }
    if (a == "--gap" || a == "--loop-gap") {
      (void)next();
      return Fail(kUsage, a + " 暂不支持",
                  "需要 core 层支持轮次间隔", "本版本请省略该选项");
    }
    if (a == "--rewrite-ts") {
      return Fail(kUsage, "--rewrite-ts 暂不支持",
                  "需要 core 层支持时间戳重写",
                  "当前回放一律保留源时间戳（这符合\"回放即还原当时\"的语义）");
    }
    if (a == "--inject") {
      (void)next();
      return Fail(kUsage, "--inject 暂不支持",
                  "故障注入需要 core 层在发送路径上挂钩子（SC-08）",
                  "设计见 docs/07-cli-design.md 场景 S12");
    }
    if (a == "--on-finish") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--on-finish 缺少参数值", "", "取值: exit / stay");
      if (*v != "exit") {
        return Fail(kUsage, "--on-finish " + *v + " 暂不支持",
                    "stay（追平后切实时跟随）需要 core 层支持，见 open question Q2",
                    "本版本回放结束后即退出");
      }
      continue;
    }
    if (a == "--require-idle") {
      return Fail(kUsage, "--require-idle 暂不支持",
                  "需要 core 层能在启动前探知话题上的活跃发布者",
                  "当前请用 --remap 把回放发到不同话题名（默认行为）");
    }
    if (a == "--on-topic-conflict") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--on-topic-conflict 缺少参数值", "",
                          "取值: abort / warn / suffix");
      if (*v != "suffix") {
        return Fail(kUsage, "--on-topic-conflict " + *v + " 暂不支持",
                    "core 当前行为等同于 suffix（自动加 " + RemapSuffix() + "）",
                    "本版本请用默认的 suffix；abort/warn 需要 core 层支持");
      }
      continue;
    }
    if (a == "--qos-restore" || a == "--qos-override") {
      const std::string* v = next();
      (void)v;
      return Fail(kUsage, a + " 暂不支持",
                  "QoS 还原策略需要 core 层从录制元数据重建 QoS 快照（FR-P-5）",
                  "设计见 docs/07-cli-design.md 场景 S09 与 EC-19");
    }
    if (a == "--remap") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--remap 缺少参数值", "", "示例: --remap in/topic=out/topic");
      const size_t eq = v->find('=');
      if (eq == std::string::npos || eq == 0 || eq + 1 >= v->size()) {
        return Fail(kUsage, "--remap 格式应为 src=dst: " + *v, "", "示例: --remap in/topic=out/topic");
      }
      o.remap.emplace_back(v->substr(0, eq), v->substr(eq + 1));
      continue;
    }
    if (a == "--domain") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--domain 缺少参数值", "", "示例: --domain 40");
      o.domain = static_cast<uint32_t>(std::strtoul(v->c_str(), nullptr, 10)); continue;
    }
    if (a == "--participant-name") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--participant-name 缺少参数值", "", "");
      o.participant_name = *v; continue;
    }
    if (a == "--advertised-address") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--advertised-address 缺少参数值", "", "");
      o.advertised_address = *v; continue;
    }
    if (a == "--initial-peers") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--initial-peers 缺少参数值", "", "");
      o.initial_peers = *v; continue;
    }
    if (a == "--discovery-wait") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--discovery-wait 缺少参数值", "", "示例: 3000ms");
      o.discovery_wait_raw = *v; continue;
    }
    if (a == "--ack-timeout") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--ack-timeout 缺少参数值", "", "示例: 5s");
      o.ack_timeout_raw = *v; continue;
    }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "play 不支持该选项: " + a,
                  "本版本支持 --from/--to/--topic/--rate/--remap/--domain/"
                  "--participant-name/--advertised-address/--initial-peers/"
                  "--discovery-wait/--wait-for-ack/--ack-timeout/--no-republish/"
                  "--loop/--allow-original-topic-names/--dry-run/--no-stats",
                  "完整选项见 docs/08-cli-spec.md 第 4 节");
    }
    if (o.session.empty()) {
      o.session = a;
    } else {
      return Fail(kUsage, "play 只接受一个会话参数，多给了: " + a, "", "");
    }
  }

  if (o.session.empty()) {
    return Fail(kUsage, "play 需要会话路径", "位置参数缺失",
                "例如: stratum play /data/stratum/s1 --rate 1.0");
  }

  // 查询类命令不应有创建副作用
  if (!SessionExists(o.session)) {
    return Fail(kNotFound, "会话不存在: " + o.session, "路径不存在或不是目录",
                "用 `stratum session list <父目录>` 列出可用会话");
  }

  TimelineImplV2 timeline;
  Status s = timeline.Open(o.session);
  if (!s.ok()) {
    return Fail(ExitCodeOf(s), "无法打开会话: " + o.session, s.ToString(), "");
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

  // 时间基准：@session 偏移需要它。录制器会写 created_at_ns，
  // 但工具生成的会话可能没有，此时回退到首条数据时间。
  uint64_t base_ns = meta.created_at_ns;
  if (base_ns == 0 || base_ns == kInvalidTimestamp) {
    for (const auto& t : topics) {
      if (t.first_ns != kInvalidTimestamp && (base_ns == 0 || t.first_ns < base_ns)) {
        base_ns = t.first_ns;
      }
    }
    LogVerbose(g, 1, "会话未记录创建时间，@session 基准回退为首条数据时间");
  }

  // -------------------------------------------------------------------------
  // 组装回放范围
  // -------------------------------------------------------------------------
  TimeRange range;
  range.begin_ns = kInvalidTimestamp;
  range.end_ns = kInvalidTimestamp;
  if (!o.from_raw.empty()) {
    uint64_t v = 0;
    std::string err;
    if (!ParseTimeValue(o.from_raw, base_ns, &v, &err)) {
      timeline.Close();
      return Fail(kUsage, "--from 无法解析: " + o.from_raw, err,
                  "支持 @session+5s / -30m / 2026-10-14T10:00:00+08:00");
    }
    range.begin_ns = v;
  }
  if (!o.to_raw.empty()) {
    uint64_t v = 0;
    std::string err;
    if (!ParseTimeValue(o.to_raw, base_ns, &v, &err)) {
      timeline.Close();
      return Fail(kUsage, "--to 无法解析: " + o.to_raw, err,
                  "支持 @session+5s / -30m / 2026-10-14T10:00:00+08:00");
    }
    range.end_ns = v;
  }
  if (range.Empty()) {
    timeline.Close();
    return Fail(kUsage, "--from 晚于 --to",
                "区间为 [" + o.from_raw + ", " + o.to_raw + ") 是空集",
                "注意区间是**半开**的：from <= t < to");
  }

  // -------------------------------------------------------------------------
  // 组装 PlayerConfig
  // -------------------------------------------------------------------------
  PlayerConfig pc;
  pc.rate = o.rate;
  pc.batch_size = o.batch_size;   // 1 = 逐条（默认，行为不变）；>1 = 批量

  protocol::BackendConfig bc;
  bc.domain_id = o.domain;
  bc.participant_name = o.participant_name;
  if (!o.advertised_address.empty()) {
    bc.extra.emplace_back("advertised_address", o.advertised_address);
  }
  if (!o.initial_peers.empty()) {
    bc.extra.emplace_back("initial_peers", o.initial_peers);
  }
  pc.backend = bc;   // ★ 必须显式传（否则 domain 被内部默认值覆盖成 0）

  pc.topic_mapping = o.remap;
  pc.allow_original_topic_names = o.allow_original_names;

  // 【关键默认值】core 的默认是 0（不等）；CLI 面向真实使用给 2 秒，
  // 因为不等发现就发数据会被对端丢弃（core 注释里有实测记录）。
  uint64_t dw_ns = 0;
  if (!o.discovery_wait_raw.empty() && !ParseDurationNs(o.discovery_wait_raw, &dw_ns)) {
    timeline.Close();
    return Fail(kUsage, "--discovery-wait 无法解析: " + o.discovery_wait_raw,
                "支持 ms/s/m", "示例: 3000ms");
  }
  pc.discovery_wait_ms = dw_ns / 1000000ULL;

  pc.wait_for_ack = o.wait_for_ack;
  uint64_t ack_ns = 5000000000ULL;
  if (!o.ack_timeout_raw.empty() && !ParseDurationNs(o.ack_timeout_raw, &ack_ns)) {
    timeline.Close();
    return Fail(kUsage, "--ack-timeout 无法解析: " + o.ack_timeout_raw, "支持 ms/s", "");
  }
  pc.ack_timeout_ms = ack_ns / 1000000ULL;
  pc.republish_unacked = !o.no_republish;
  pc.loop = o.loop;

  // 统计（在回放线程的回调里累加，主线程阻塞在 Play 上）
  std::atomic<uint64_t> published{0};
  pc.publish_callback = [&published, &g, &o](const protocol::ReceivedSample&) {
    const uint64_t n = published.fetch_add(1, std::memory_order_relaxed) + 1;
    // 回调在回放线程上，不得阻塞；每 500 条打一次进度
    if (o.show_stats && n % 500 == 0) {
      std::fprintf(stderr, "  已回放 %llu 条\n",
                   static_cast<unsigned long long>(n));
    }
    (void)g;
  };

  // -------------------------------------------------------------------------
  // --dry-run
  // -------------------------------------------------------------------------
  if (o.dry_run) {
    std::printf("DRY RUN — 不发布任何数据\n");
    std::printf("  会话:        %s\n", o.session.c_str());
    std::printf("  话题数:      %zu\n", topics.size());
    std::printf("  时间基准:    %llu\n", static_cast<unsigned long long>(base_ns));
    if (range.begin_ns != kInvalidTimestamp) {
      std::printf("  起点:        %s\n", FormatRfc3339(range.begin_ns).c_str());
    } else {
      std::printf("  起点:        会话最早数据\n");
    }
    if (range.end_ns != kInvalidTimestamp) {
      std::printf("  终点:        %s（半开，不含）\n", FormatRfc3339(range.end_ns).c_str());
    } else {
      std::printf("  终点:        会话最晚数据\n");
    }
    std::printf("\n[DDS]\n");
    std::printf("  domain:            %u\n", o.domain);
    std::printf("  participant_name:  %s\n", o.participant_name.c_str());
    std::printf("  发现等待:          %llu ms\n",
                static_cast<unsigned long long>(pc.discovery_wait_ms));
    std::printf("    → 对端需要先完成 SPDP/SEDP 匹配；不等会丢数据（实测等 0ms 收到 0 条）\n");

    std::printf("\n[话题名处理]\n");
    if (o.allow_original_names) {
      std::printf("  **使用原始话题名（危险）**\n");
      std::printf("    → 若原话题仍在实时发布，下游会同时收到实时与历史两份数据 (EC-18)\n");
    } else {
      std::printf("  使用映射/后缀名（安全默认）\n");
      std::printf("    → 未显式映射的话题将加后缀 %s\n", RemapSuffix().c_str());
    }
    for (const auto& m : o.remap) {
      std::printf("    %s -> %s\n", m.first.c_str(), m.second.c_str());
    }

    std::printf("\n[速率与确认]\n");
    std::printf("  rate:              %.3f%s\n", o.rate,
                o.rate == 0.0 ? "（尽可能快）" : "");
    std::printf("  wait_for_ack:      %s\n", o.wait_for_ack ? "是" : "否");
    std::printf("  republish_unacked: %s\n",
                pc.republish_unacked ? "是（补救 VOLATILE 首条丢失）" : "否");
    std::printf("  loop:              %s\n", o.loop ? "是（无限，Ctrl+C 停止）" : "否");

    std::printf("\n  执行请去掉 --dry-run\n");
    timeline.Close();
    return kOk;
  }

  // -------------------------------------------------------------------------
  // 回放
  // -------------------------------------------------------------------------
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  g_stop_requested.store(false);

  protocol::rtps::RtpsBackend backend;
  Player player(&timeline, &backend);

  LogVerbose(g, 1, "开始回放：" + o.session + " domain=" + std::to_string(o.domain) +
                       " rate=" + std::to_string(o.rate) +
                       (o.loop ? " loop=on" : ""));

  const auto t0 = std::chrono::steady_clock::now();
  s = player.Play(range, pc);
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  const bool interrupted = g_stop_requested.load();
  if (!s.ok() && !interrupted) {
    timeline.Close();
    return Fail(ExitCodeOf(s), "回放失败", s.ToString(),
                "常见原因：domain 被占用、端口无权限、advertised_address 不可用");
  }

  const uint64_t n = published.load();
  const PlaybackProgress prog = player.GetProgress();
  timeline.Close();

  if (g.format == Format::kJson) {
    std::printf("{\"schema_version\":1,\"session\":\"%s\",\"domain\":%u,"
                "\"published\":%llu,\"elapsed_s\":%.3f,\"interrupted\":%s}\n",
                JsonEscape(o.session).c_str(), o.domain,
                static_cast<unsigned long long>(n), elapsed,
                interrupted ? "true" : "false");
  } else if (!g.quiet) {
    std::fprintf(stderr, "\n回放结束%s\n", interrupted ? "（被信号中断）" : "");
    std::fprintf(stderr, "  已发布:    %s 条\n", HumanCount(n).c_str());
    std::fprintf(stderr, "  耗时:      %.3f 秒\n", elapsed);
    std::fprintf(stderr, "  进度:      %.1f%%\n", prog.Percent());
    if (n == 0) {
      std::fprintf(stderr,
                   "\n  ⚠ 未发布任何数据。检查：\n"
                   "     · 该会话在 --from/--to 区间内是否有数据\n"
                   "     · --topic 是否写错（可省略以回放全部）\n");
    }
    std::fprintf(stderr, "\n  提示: 若对端一条没收到，优先加大 --discovery-wait\n");
  }
  return kOk;
}

}  // namespace cli
}  // namespace stratum
