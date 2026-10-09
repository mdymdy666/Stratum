// M5 产品化验证：Stratum Recorder + RtpsBackend 录制真实 DDS 流量
//
// 这是最初目标的直接验证：
//
//   Recorder ──▶ IDdsBackend（插件）──▶ RtpsBackend ──▶ RtpsReader ──▶ 真实 DDS 网络
//      │
//      ▼
//   TimelineImplV2（LSMengine 后端）──▶ 落盘
//
// 与之前工具的区别：数据不再只是打印，而是**经 Recorder 入队 → 后台批量写入
// 时间线 → 可用 Query 查回**，即完整的产品链路。
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <string>
#include <thread>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"
#include "stratum/protocol/rtps/rtps_backend.h"

using namespace stratum;
using namespace stratum::protocol;
using namespace stratum::protocol::rtps;

int main(int argc, char** argv) {
  // 【参数解析】支持 --overwrite；其余按位置参数处理（seconds/domain/dir）。
  bool overwrite = false;
  std::vector<const char*> pos;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--overwrite") == 0) {
      overwrite = true;
    } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      std::printf("用法: stratum_rtps_record [seconds] [domain] [dir] [--overwrite]\n"
                  "  默认**拒绝**在非空目录上启动（保护已有时间线）；\n"
                  "  确实要清空重录时显式加 --overwrite。\n");
      return 0;
    } else {
      pos.push_back(argv[i]);
    }
  }
  const int seconds = (pos.size() > 0) ? std::atoi(pos[0]) : 25;
  const uint32_t domain =
      (pos.size() > 1) ? static_cast<uint32_t>(std::atoi(pos[1])) : 0;
  const char* dir = (pos.size() > 2) ? pos[2] : "/tmp/stratum_rtps_record";

  // 【O19 修复】此前这里无条件执行 `rm -rf <dir>`，导致"重启录制器"会
  // **静默清空已有时间线** —— 实测同一目录重启后数据从 301 条变成 0 条，
  // 而用户只看到录制正常开始。这属于数据安全性问题。
  // 现在改为：目录非空时**默认拒绝启动**，必须显式 `--overwrite`。
  // 另外不再使用 `system("rm -rf ...")`（既有命令注入风险，也没必要）。
  {
    std::error_code ec;
    const bool exists = std::filesystem::exists(dir, ec);
    if (exists && !std::filesystem::is_empty(dir, ec)) {
      if (!overwrite) {
        std::printf("REFUSED: 目录非空，拒绝清空已有时间线：%s\n"
                    "  如需覆盖重录，请显式加 --overwrite；\n"
                    "  如需保留历史数据，请换一个目录。\n",
                    dir);
        return 3;
      }
      std::filesystem::remove_all(dir, ec);
      if (ec) {
        std::printf("FAILED: 清空目录失败: %s\n", ec.message().c_str());
        return 1;
      }
      std::printf("已按 --overwrite 清空目录：%s\n", dir);
    }
  }

  // --- 时间线（LSMengine 后端）---
  TimelineImplV2 timeline;
  Status s = timeline.Open(dir);
  if (!s.ok()) {
    std::printf("FAILED: timeline.Open: %s\n", s.ToString().c_str());
    return 1;
  }

  // --- RTPS 后端 + Recorder ---
  RtpsBackend backend;
  Recorder recorder(&timeline, &backend);

  RecorderConfig rcfg;
  rcfg.record.max_buffer_bytes = 32 * 1024 * 1024;
  rcfg.write_batch_size = 128;
  rcfg.close_timeline_on_stop = false;

  BackendConfig bcfg;
  bcfg.domain_id = domain;
  bcfg.participant_name = "stratum_recorder";
  // topics 留空 = 自动订阅全部发现到的话题（录制器的默认期望行为）

  // 【必须显式传入】同 stratum_rtps_replay：缺了这行，命令行给的 domain
  // 会被 Recorder 内部默认构造的 BackendConfig 覆盖成 0。
  rcfg.backend = bcfg;

  // Recorder 通过 IDdsBackend 创建数据源
  s = recorder.Start(rcfg);
  if (!s.ok()) {
    std::printf("FAILED: recorder.Start: %s\n", s.ToString().c_str());
    return 1;
  }

  std::printf("录制中（domain=%u，%d 秒）...\n", domain, seconds);
  std::fflush(stdout);

  for (int i = 0; i < seconds; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto st = recorder.GetStats();
    std::printf("  [%2ds] received=%llu recorded=%llu dropped=%llu queue=%llu\n",
                i + 1,
                static_cast<unsigned long long>(st.received),
                static_cast<unsigned long long>(st.recorded),
                static_cast<unsigned long long>(st.dropped),
                static_cast<unsigned long long>(st.queue_depth));
    std::fflush(stdout);
  }

  s = recorder.Stop();
  if (!s.ok()) {
    std::printf("WARN: recorder.Stop: %s\n", s.ToString().c_str());
  }

  const auto st = recorder.GetStats();
  std::printf("\n=== 录制统计 ===\n");
  std::printf("  received      = %llu\n", static_cast<unsigned long long>(st.received));
  std::printf("  recorded      = %llu\n", static_cast<unsigned long long>(st.recorded));
  std::printf("  dropped       = %llu\n", static_cast<unsigned long long>(st.dropped));
  std::printf("  bytes_recorded= %llu\n", static_cast<unsigned long long>(st.bytes_recorded));

  // --- 查询验证：录到的话题 ---
  Query query(&timeline);
  std::vector<TopicSummary> topics;
  s = query.ListTopics(&topics);
  std::printf("\n=== 录到的话题（%zu 个）===\n", topics.size());
  uint64_t total = 0;
  for (const auto& t : topics) {
    std::printf("  %-56s %8llu 条\n", t.topic_name.c_str(),
                static_cast<unsigned long long>(t.message_count));
    total += t.message_count;
  }
  std::printf("  合计 %llu 条\n", static_cast<unsigned long long>(total));

  // --- 质量报告 ---
  QualityReporter reporter(&timeline);
  QualityReport report;
  if (reporter.Generate(0, &report).ok()) {
    std::printf("\n=== 质量报告 ===\n%s\n", report.ToText().c_str());
  }

  timeline.Close();

  if (st.recorded == 0) {
    std::printf("\n** 未录到任何数据（检查是否有 DDS 发布者在该 domain 活跃）**\n");
    return 2;
  }
  std::printf("\n结果：PASS（录到 %llu 条真实 DDS 数据）\n",
              static_cast<unsigned long long>(st.recorded));
  return 0;
}
