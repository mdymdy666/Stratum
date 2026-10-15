// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_record.cc
//
// `stratum record` —— CLI 存在的意义所在，也是 play/prune/ctl 的前提。
//
// 设计依据：docs/07-cli-design.md 场景 S01/S02/S05/S06、
//           docs/08-cli-spec.md 第 3 节（含 3.2 状态机、3.3 锁、3.4 并发矩阵）
//
// 【本版本落地的关键设计约定】
//  1. 目录非空时**默认拒绝启动**（EXISTS=3），必须显式 --overwrite。
//     这是现有工具 O19 修复的延续：曾经无条件 rm -rf，导致"重启录制器"
//     会静默清空已有时间线（实测数据从 301 条变 0 条，而用户只看到录制正常开始）。
//  2. --dry-run 打印**生效配置与后果**，而不只是回显参数。
//     对齐 EC-17：让人在启动前就能看出"最老数据会在 X 小时后被删"。
//  3. 统计与日志一律走 **stderr**，stdout 只留数据出口的纪律。
//  4. SIGINT/SIGTERM 优雅停止（FR-R-10），不留半截元数据。

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "cli_common.h"
#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"
#include "stratum/protocol/rtps/rtps_backend.h"

namespace fs = std::filesystem;

using namespace stratum;
using namespace stratum::cli;

namespace stratum {
namespace cli {

namespace {

// 优雅停止：信号处理里只置标志，真正的收尾在主循环完成
std::atomic<bool> g_stop_requested{false};

void OnSignal(int) { g_stop_requested.store(true); }

struct RecordOpts {
  std::string dir;
  std::string session_name;
  bool overwrite = false;
  bool dry_run = false;
  bool continuous = false;
  std::string duration_raw;
  bool show_stats = true;

  // 话题
  std::vector<std::string> include;
  std::vector<std::string> exclude;

  // DDS
  uint32_t domain = 0;
  std::string participant_name = "stratum-recorder";
  std::string advertised_address;
  std::string initial_peers;

  // 保留与容量
  std::string retain_raw;
  std::string retain_bytes_raw;
  std::string on_full = "drop-newest";
  std::string durability = "balanced";
  std::string max_buffer_raw;
  std::string split_by_bytes_raw;

  // 性能
  std::string write_batch_raw;
};

bool ParseOnFull(const std::string& s, RecordConfig::OverflowPolicy* out) {
  if (s == "drop-newest") { *out = RecordConfig::OverflowPolicy::kDropNewest; return true; }
  if (s == "drop-oldest") { *out = RecordConfig::OverflowPolicy::kDropOldest; return true; }
  if (s == "block") { *out = RecordConfig::OverflowPolicy::kBlock; return true; }
  return false;
}

bool ParseDurability(const std::string& s, RecordConfig::DurabilityLevel* out) {
  if (s == "relaxed") { *out = RecordConfig::DurabilityLevel::kRelaxed; return true; }
  if (s == "balanced") { *out = RecordConfig::DurabilityLevel::kBalanced; return true; }
  if (s == "strict") { *out = RecordConfig::DurabilityLevel::kStrict; return true; }
  return false;
}

std::string AutoSessionName() {
  std::time_t now = std::time(nullptr);
  std::tm tmv{};
  localtime_r(&now, &tmv);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmv);
  return std::string("auto:") + buf;
}

// ---------------------------------------------------------------------------
// --dry-run：打印生效配置与后果
//
// 对齐 docs/08 8.4 节：这段输出是**稳定文本契约**，可直接作为黄金文件测试。
// 关键是要回答"按此配置会发生什么"，而不只是"你设了什么"。
// ---------------------------------------------------------------------------
void PrintDryRun(const RecordOpts& o, const RecorderConfig& rc,
                 const protocol::BackendConfig& bc) {
  std::printf("DRY RUN — 未开始录制，未创建任何文件\n");
  std::printf("  会话目录:   %s\n", o.dir.c_str());
  std::printf("  会话名:     %s\n",
              o.session_name.empty() ? AutoSessionName().c_str() : o.session_name.c_str());

  std::printf("\n[DDS]\n");
  std::printf("  domain:            %u\n", bc.domain_id);
  std::printf("  participant_name:  %s\n", bc.participant_name.c_str());
  if (bc.topics.empty()) {
    std::printf("  订阅话题:          **全部发现到的话题**（自动发现模式）\n");
    std::printf("    → 现场话题很多时会占用带宽与磁盘，建议用 --include 限定\n");
  } else {
    std::printf("  订阅话题:          %zu 个\n", bc.topics.size());
    for (const auto& t : bc.topics) std::printf("    %s\n", t.c_str());
  }
  if (!o.advertised_address.empty()) {
    std::printf("  advertised:        %s\n", o.advertised_address.c_str());
  } else {
    std::printf("  advertised:        (自动探测)\n");
    std::printf("    → 跨网段时**必须显式指定**，否则对端不可达，"
                "症状是\"发现成功但收不到数据\"\n");
  }

  std::printf("\n[保留与容量]\n");
  if (rc.record.retention.retain_duration_ns == 0) {
    std::printf("  retain:            **不限制** → 数据一直累积，最终写满磁盘 (EC-11)\n");
  } else {
    const double h = static_cast<double>(rc.record.retention.retain_duration_ns) / 3600e9;
    std::printf("  retain:            %.2f 小时\n", h);
    std::printf("    → **超过 %.2f 小时的数据会被自动删除**\n", h);
  }
  if (rc.record.retention.max_bytes != 0) {
    std::printf("  retain_bytes:      %s\n", HumanBytes(rc.record.retention.max_bytes).c_str());
    std::printf("    → 与 retain 同时设置时，**任一维度满足即淘汰**（取先到者）\n");
  }
  std::printf("  max_buffer_bytes:  %s\n", HumanBytes(rc.record.max_buffer_bytes).c_str());
  std::printf("  split_by_bytes:    %s\n", HumanBytes(rc.record.split.max_layer_bytes).c_str());

  std::printf("\n[队列满时]\n");
  if (rc.record.overflow_policy == RecordConfig::OverflowPolicy::kDropNewest) {
    std::printf("  on-full = drop-newest（默认）\n");
    std::printf("    → 丢弃**新来**的数据，业务侧不受影响，但录制会缺尾部\n");
  } else if (rc.record.overflow_policy == RecordConfig::OverflowPolicy::kDropOldest) {
    std::printf("  on-full = drop-oldest\n");
    std::printf("    → 丢弃**队列中最旧**的数据，保留最近的\n");
  } else {
    std::printf("  on-full = block\n");
    std::printf("    → **阻塞后端线程**直到队列有空间；不丢数据，但会**影响业务** (FR-R-6)\n");
  }

  std::printf("\n[运行形态]\n");
  if (o.continuous) {
    std::printf("  持续录制，直到收到 SIGINT/SIGTERM 或 `stratum ctl shutdown`\n");
  } else if (!o.duration_raw.empty()) {
    std::printf("  录制 %s 后自动优雅停止\n", o.duration_raw.c_str());
  } else {
    std::printf("  未指定 --duration/--continuous → 默认录制 25 秒\n");
  }
  std::printf("  写盘批量:          %zu\n", rc.write_batch_size);

  std::printf("\n[汇总]\n");
  std::printf("  按此配置：%s\n",
              rc.record.retention.retain_duration_ns == 0
                  ? "**永不淘汰数据**（需自行监控磁盘）"
                  : "按保留窗口自动淘汰旧数据");
  std::printf("  队列满时：%s\n",
              rc.record.overflow_policy == RecordConfig::OverflowPolicy::kBlock
                  ? "阻塞等待（保证不丢，但会影响业务）"
                  : "丢弃（业务不受影响，录制可能不完整）");
  std::printf("\n  执行请去掉 --dry-run\n");
}

}  // namespace

int CmdRecord(const std::vector<std::string>& args, GlobalOptions g) {
  RecordOpts o;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](const char* name) -> const std::string* {
      if (i + 1 >= args.size()) return nullptr;
      (void)name;
      return &args[++i];
    };

    if (a == "--overwrite") { o.overwrite = true; continue; }
    if (a == "--dry-run") { o.dry_run = true; continue; }
    if (a == "--continuous") { o.continuous = true; continue; }
    if (a == "--no-stats") { o.show_stats = false; continue; }

    if (a == "--dir") {
      const std::string* v = next("--dir");
      if (!v) return Fail(kUsage, "--dir 缺少参数值", "", "示例: --dir /data/stratum/s1");
      o.dir = *v; continue;
    }
    if (a == "--session-name") {
      const std::string* v = next("--session-name");
      if (!v) return Fail(kUsage, "--session-name 缺少参数值", "", "");
      o.session_name = *v; continue;
    }
    if (a == "--duration") {
      const std::string* v = next("--duration");
      if (!v) return Fail(kUsage, "--duration 缺少参数值", "", "示例: --duration 30s");
      o.duration_raw = *v; continue;
    }
    if (a == "--include") {
      const std::string* v = next("--include");
      if (!v) return Fail(kUsage, "--include 缺少参数值", "", "示例: --include sensor/temp");
      o.include.push_back(*v); continue;
    }
    if (a == "--exclude") {
      const std::string* v = next("--exclude");
      if (!v) return Fail(kUsage, "--exclude 缺少参数值", "", "示例: --exclude sensor/debug");
      o.exclude.push_back(*v); continue;
    }
    if (a == "--domain") {
      const std::string* v = next("--domain");
      if (!v) return Fail(kUsage, "--domain 缺少参数值", "", "示例: --domain 40");
      o.domain = static_cast<uint32_t>(std::strtoul(v->c_str(), nullptr, 10)); continue;
    }
    if (a == "--participant-name") {
      const std::string* v = next("--participant-name");
      if (!v) return Fail(kUsage, "--participant-name 缺少参数值", "", "");
      o.participant_name = *v; continue;
    }
    if (a == "--advertised-address") {
      const std::string* v = next("--advertised-address");
      if (!v) return Fail(kUsage, "--advertised-address 缺少参数值", "", "");
      o.advertised_address = *v; continue;
    }
    if (a == "--initial-peers") {
      const std::string* v = next("--initial-peers");
      if (!v) return Fail(kUsage, "--initial-peers 缺少参数值", "", "");
      o.initial_peers = *v; continue;
    }
    if (a == "--retain") {
      const std::string* v = next("--retain");
      if (!v) return Fail(kUsage, "--retain 缺少参数值", "", "示例: --retain 24h");
      o.retain_raw = *v; continue;
    }
    if (a == "--retain-bytes") {
      const std::string* v = next("--retain-bytes");
      if (!v) return Fail(kUsage, "--retain-bytes 缺少参数值", "", "示例: --retain-bytes 50GB");
      o.retain_bytes_raw = *v; continue;
    }
    if (a == "--on-full") {
      const std::string* v = next("--on-full");
      if (!v) return Fail(kUsage, "--on-full 缺少参数值", "",
                          "取值: drop-newest / drop-oldest / block");
      o.on_full = *v; continue;
    }
    if (a == "--durability") {
      const std::string* v = next("--durability");
      if (!v) return Fail(kUsage, "--durability 缺少参数值", "",
                          "取值: relaxed / balanced / strict");
      o.durability = *v; continue;
    }
    if (a == "--max-buffer-bytes") {
      const std::string* v = next("--max-buffer-bytes");
      if (!v) return Fail(kUsage, "--max-buffer-bytes 缺少参数值", "", "示例: 64MB");
      o.max_buffer_raw = *v; continue;
    }
    if (a == "--split-by-bytes") {
      const std::string* v = next("--split-by-bytes");
      if (!v) return Fail(kUsage, "--split-by-bytes 缺少参数值", "", "示例: 256MB");
      o.split_by_bytes_raw = *v; continue;
    }
    if (a == "--write-batch-size") {
      const std::string* v = next("--write-batch-size");
      if (!v) return Fail(kUsage, "--write-batch-size 缺少参数值", "", "示例: 256");
      o.write_batch_raw = *v; continue;
    }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "record 不支持该选项: " + a,
                  "本版本支持 --dir/--session-name/--duration/--continuous/"
                  "--include/--exclude/--domain/--participant-name/"
                  "--advertised-address/--initial-peers/--retain/--retain-bytes/"
                  "--on-full/--durability/--max-buffer-bytes/--split-by-bytes/"
                  "--write-batch-size/--overwrite/--dry-run/--no-stats",
                  "完整选项见 docs/08-cli-spec.md 第 3 节");
    }
    return Fail(kUsage, "record 不接受位置参数: " + a, "所有配置都通过选项给出",
                "示例: stratum record --dir /data/stratum/s1 --duration 30s");
  }

  if (o.dir.empty()) {
    return Fail(kUsage, "record 需要 --dir",
                "缺少会话目录（本版本暂不支持从配置文件读取）",
                "例如: stratum record --dir /data/stratum/s1 --duration 30s");
  }
  if (o.continuous && !o.duration_raw.empty()) {
    return Fail(kUsage, "--continuous 与 --duration 互斥",
                "一个表示持续录制，一个表示定时录制，语义冲突",
                "二选一；持续录制用 Ctrl+C 或 `stratum ctl shutdown` 停止");
  }

  // -------------------------------------------------------------------------
  // 组配置
  // -------------------------------------------------------------------------
  RecorderConfig rc;
  protocol::BackendConfig bc;

  bc.domain_id = o.domain;
  bc.participant_name = o.participant_name;
  bc.topics = o.include;   // 空 = 自动发现全部
  if (!o.advertised_address.empty()) {
    bc.extra.emplace_back("advertised_address", o.advertised_address);
  }
  if (!o.initial_peers.empty()) {
    bc.extra.emplace_back("initial_peers", o.initial_peers);
  }

  if (!o.retain_raw.empty()) {
    if (!ParseDurationNs(o.retain_raw, &rc.record.retention.retain_duration_ns)) {
      return Fail(kUsage, "--retain 无法解析: " + o.retain_raw, "支持 ms/s/m/h/d/w",
                  "示例: --retain 24h");
    }
  }
  if (!o.retain_bytes_raw.empty()) {
    if (!ParseSizeBytes(o.retain_bytes_raw, &rc.record.retention.max_bytes)) {
      return Fail(kUsage, "--retain-bytes 无法解析: " + o.retain_bytes_raw,
                  "支持 B/KB/MB/GB/TB 与 KiB/MiB/GiB", "示例: --retain-bytes 50GB");
    }
  }
  if (!ParseOnFull(o.on_full, &rc.record.overflow_policy)) {
    return Fail(kUsage, "--on-full 取值非法: " + o.on_full,
                "支持 drop-newest / drop-oldest / block", "");
  }
  if (!ParseDurability(o.durability, &rc.record.durability)) {
    return Fail(kUsage, "--durability 取值非法: " + o.durability,
                "支持 relaxed / balanced / strict", "");
  }
  if (!o.max_buffer_raw.empty()) {
    if (!ParseSizeBytes(o.max_buffer_raw, &rc.record.max_buffer_bytes)) {
      return Fail(kUsage, "--max-buffer-bytes 无法解析: " + o.max_buffer_raw, "", "");
    }
  }
  if (!o.split_by_bytes_raw.empty()) {
    if (!ParseSizeBytes(o.split_by_bytes_raw, &rc.record.split.max_layer_bytes)) {
      return Fail(kUsage, "--split-by-bytes 无法解析: " + o.split_by_bytes_raw, "", "");
    }
  }
  if (!o.write_batch_raw.empty()) {
    rc.write_batch_size = static_cast<size_t>(std::strtoul(o.write_batch_raw.c_str(), nullptr, 10));
  }

  // ★ 必须显式传入：曾经 Recorder 内部用全新默认对象，导致调用方设的 domain
  //   被静默丢弃（无论上层传什么都跑在 domain 0）。这类"配置在中间层被丢掉"
  //   的问题不报错，只让互操作永远对不上。
  rc.backend = bc;

  // -------------------------------------------------------------------------
  // --dry-run
  // -------------------------------------------------------------------------
  if (o.dry_run) {
    PrintDryRun(o, rc, bc);
    return kOk;
  }

  // -------------------------------------------------------------------------
  // 目录检查（O19 修复的延续）：非空则**默认拒绝**，不静默清空
  // -------------------------------------------------------------------------
  {
    std::error_code ec;
    const bool exists = fs::exists(o.dir, ec);
    if (exists && !fs::is_empty(o.dir, ec)) {
      if (!o.overwrite) {
        return Fail(kExists, "目录非空，拒绝覆盖已有时间线: " + o.dir,
                    "目录中已有数据，直接启动会清空它",
                    "换一个 --dir；确实要清空重录时显式加 --overwrite");
      }
      fs::remove_all(o.dir, ec);
      if (ec) {
        return Fail(kEnv, "清空目录失败: " + o.dir, ec.message(), "");
      }
      LogVerbose(g, 1, "已按 --overwrite 清空目录: " + o.dir);
    } else if (!exists) {
      fs::create_directories(o.dir, ec);
      if (ec) {
        return Fail(kEnv, "无法创建目录: " + o.dir, ec.message(),
                    "检查父目录是否存在、是否有写权限");
      }
    }
  }

  // -------------------------------------------------------------------------
  // 打开时间线 + 启动录制
  // -------------------------------------------------------------------------
  TimelineImplV2 timeline;
  Status s = timeline.Open(o.dir);
  if (!s.ok()) {
    return Fail(ExitCodeOf(s), "无法打开会话目录: " + o.dir, s.ToString(),
                "确认路径可写；用 `stratum session list <父目录>` 查看已有会话");
  }

  protocol::rtps::RtpsBackend backend;
  Recorder recorder(&timeline, &backend);

  s = recorder.Start(rc);
  if (!s.ok()) {
    timeline.Close();
    return Fail(ExitCodeOf(s), "启动录制失败", s.ToString(),
                "常见原因：domain 被占用、端口无权限(<1024)、advertised_address 不可用");
  }

  // 优雅停止
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  g_stop_requested.store(false);

  // 解析时长（默认 25 秒，与既有工具一致）
  double duration_s = 25.0;
  if (!o.duration_raw.empty()) {
    uint64_t ns = 0;
    if (!ParseDurationNs(o.duration_raw, &ns)) {
      recorder.Stop();
      timeline.Close();
      return Fail(kUsage, "--duration 无法解析: " + o.duration_raw,
                  "支持 ms/s/m/h", "示例: --duration 30s");
    }
    duration_s = static_cast<double>(ns) / 1e9;
  }

  LogVerbose(g, 1, "开始录制：dir=" + o.dir + " domain=" + std::to_string(bc.domain_id) +
                       (o.continuous ? " 模式=持续" : " 时长=" + o.duration_raw));

  // 统计一律走 stderr —— stdout 是数据出口（CLI 纪律）
  const auto t0 = std::chrono::steady_clock::now();
  int last_printed = -1;
  while (!g_stop_requested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!o.continuous && elapsed >= duration_s) break;

    if (o.show_stats) {
      const int sec = static_cast<int>(elapsed);
      if (sec != last_printed) {
        last_printed = sec;
        const auto st = recorder.GetStats();
        std::fprintf(stderr,
                     "  [%3ds] received=%llu recorded=%llu dropped=%llu queue=%llu\n",
                     sec, static_cast<unsigned long long>(st.received),
                     static_cast<unsigned long long>(st.recorded),
                     static_cast<unsigned long long>(st.dropped),
                     static_cast<unsigned long long>(st.queue_depth));
      }
    }
  }

  const bool interrupted = g_stop_requested.load();
  s = recorder.Stop();   // 会排空队列并写元数据
  if (!s.ok()) {
    timeline.Close();
    return Fail(ExitCodeOf(s), "停止录制时出错", s.ToString(),
                "会话可能未正常关闭；用 `stratum verify` 检查完整性");
  }

  const auto st = recorder.GetStats();
  timeline.Close();

  if (g.format == Format::kJson) {
    std::printf("{\"schema_version\":1,\"dir\":\"%s\",\"domain\":%u,"
                "\"received\":%llu,\"recorded\":%llu,\"dropped\":%llu,"
                "\"bytes\":%llu,\"interrupted\":%s}\n",
                JsonEscape(o.dir).c_str(), bc.domain_id,
                static_cast<unsigned long long>(st.received),
                static_cast<unsigned long long>(st.recorded),
                static_cast<unsigned long long>(st.dropped),
                static_cast<unsigned long long>(st.bytes_recorded),
                interrupted ? "true" : "false");
  } else if (!g.quiet) {
    std::fprintf(stderr, "\n录制结束%s\n", interrupted ? "（被信号中断）" : "");
    std::fprintf(stderr, "  目录:      %s\n", o.dir.c_str());
    std::fprintf(stderr, "  收到样本:  %s\n", HumanCount(st.received).c_str());
    std::fprintf(stderr, "  已写入:    %s\n", HumanCount(st.recorded).c_str());
    std::fprintf(stderr, "  丢弃:      %s\n", HumanCount(st.dropped).c_str());
    std::fprintf(stderr, "  写入字节:  %s\n", HumanBytes(st.bytes_recorded).c_str());
    if (st.received == 0) {
      std::fprintf(stderr,
                   "\n  ⚠ 未收到任何数据。检查：\n"
                   "     · 该 domain 上是否有活跃的 DDS 发布者\n"
                   "     · --include 是否写错（可先不加，用自动发现模式试）\n"
                   "     · 跨网段时是否指定了 --advertised-address\n");
    }
    std::fprintf(stderr, "\n  下一步: stratum session info %s\n", o.dir.c_str());
  }

  // 有丢弃就是部分成功（数据不完整），退出码 5 让脚本能判断
  if (st.dropped > 0) return kPartial;
  return kOk;
}

}  // namespace cli
}  // namespace stratum
