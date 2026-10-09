// M5 回放验证：用 Stratum Player + RtpsBackend 把录制的数据回放到 DDS 网络
//
// 与 stratum_rtps_record.cc 构成闭环：
//
//   第 1 步（录制）：真实 DDS 流量 → RtpsReader → Recorder → Timeline
//   第 2 步（回放）：Timeline → Player → RtpsWriter → 真实 DDS 网络
//   第 3 步（验证）：用 Fast DDS 的 hello_world subscriber 接收，观察是否收到
//
// 关键配置：allow_original_topic_names = true
//   Player 默认会把话题重命名（避免污染实时数据流，这是正确的默认值）。
//   但本验证要让 Fast DDS 的 subscriber 收到，因此必须用原名。
#include <cstdio>
#include <cstring>
#include <string>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"
#include "stratum/protocol/rtps/rtps_backend.h"

using namespace stratum;
using namespace stratum::protocol;
using namespace stratum::protocol::rtps;

int main(int argc, char** argv) {
  const char* dir = (argc > 1) ? argv[1] : "/tmp/stratum_rec_final";
  const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;
  const double rate = (argc > 3) ? std::atof(argv[3]) : 1.0;

  TimelineImplV2 timeline;
  // 回放以只读方式打开
  Status s = timeline.Open(dir);
  if (!s.ok()) {
    std::printf("FAILED: timeline.Open(%s): %s\n", dir, s.ToString().c_str());
    return 1;
  }

  // 先确认录到了什么
  Query query(&timeline);
  std::vector<TopicSummary> topics;
  query.ListTopics(&topics);
  std::printf("待回放话题（%zu 个）：\n", topics.size());
  uint64_t total = 0;
  for (const auto& t : topics) {
    std::printf("  %-40s %8llu 条\n", t.topic_name.c_str(),
                static_cast<unsigned long long>(t.message_count));
    total += t.message_count;
  }
  if (total == 0) {
    std::printf("** 时间线为空，无可回放数据 **\n");
    return 2;
  }

  RtpsBackend backend;
  Player player(&timeline, &backend);

  BackendConfig bcfg;
  bcfg.domain_id = domain;
  bcfg.participant_name = "stratum_player";

  PlayerConfig pcfg;
  // 【必须显式传入】过去 pcfg 没有 backend 字段，这里构造的 bcfg 被丢弃，
  // 于是无论命令行给什么 domain，回放都发生在 domain 0 —— 而进度打印却
  // 显示 domain=3，极具误导性（见 test_backend_config_propagation）。
  pcfg.backend = bcfg;
  pcfg.rate = rate;
  pcfg.allow_original_topic_names = true;   // 见文件头说明
  pcfg.discovery_wait_ms = 5000;   // 见 PlayerConfig 中的说明
  pcfg.wait_for_ack = true;
  pcfg.ack_timeout_ms = 2000;
  pcfg.loop = false;

  size_t published = 0;
  pcfg.publish_callback = [&published](const ReceivedSample& rs) {
    ++published;
    if (published <= 3) {
      std::printf("  [回放 #%zu] topic=\"%s\" seq=%u payload=%zu\n",
                  published, rs.topic_name.c_str(), rs.sequence,
                  rs.payload.size());
      // STRATUM_DUMP=1 时打印负载头：核对它是否是合法的 CDR encapsulation
      // （PL_CDR_LE = 00 03 00 00 / CDR_LE = 00 01 00 00）。
      // 负载格式错误时对端会静默丢弃，本端毫无察觉 —— 必须能从这一跳看到。
      if (std::getenv("STRATUM_DUMP") != nullptr) {
        std::printf("      payload head:");
        for (size_t i = 0; i < 12 && i < rs.payload.size(); ++i) {
          std::printf(" %02x", static_cast<unsigned char>(rs.payload[i]));
        }
        std::printf("\n");
      }
      std::fflush(stdout);
    }
  };

  std::printf("\n开始回放（domain=%u rate=%.1f）...\n", domain, rate);
  std::fflush(stdout);

  s = player.Play(TimeRange{}, pcfg);
  const auto prog = player.GetProgress();

  std::printf("\n=== 回放结果 ===\n");
  std::printf("  Play 返回值    = %s\n", s.ToString().c_str());
  std::printf("  已发布样本     = %zu\n", published);
  {
    // 【O10】打印 ACKNACK 三条判定路径的分布，作为"位图分支被真实走到"的证据。
    const auto ack = ::stratum::protocol::rtps::AckPathStatsSnapshot();
    std::printf("  ACKNACK 判定: base_sn=%llu  位图已解析=%llu  位图判缺失=%llu  位图外=%llu\n",
                static_cast<unsigned long long>(ack.base_acked),
                static_cast<unsigned long long>(ack.bitmap_checked),
                static_cast<unsigned long long>(ack.bitmap_missing),
                static_cast<unsigned long long>(ack.beyond_bitmap));
  }
  std::printf("  进度：total=%llu published=%llu\n",
              static_cast<unsigned long long>(prog.total),
              static_cast<unsigned long long>(prog.published));

  timeline.Close();

  if (published == 0) {
    std::printf("\n** 未回放任何数据 **\n");
    return 3;
  }
  std::printf("\n结果：PASS（回放 %zu 条数据到 DDS 网络）\n", published);
  return 0;
}
