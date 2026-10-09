// RtpsReader 独立诊断：直接使用协议引擎并打印统计
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>
#include <cstring>
#include <string>
#include <thread>

#include <stratum/protocol/rtps/rtps_reader.h>

using namespace stratum;
using namespace stratum::protocol::rtps;

int main(int argc, char** argv) {
  const int seconds = (argc > 1) ? std::atoi(argv[1]) : 20;
  const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;

  RtpsReader reader;
  RtpsReaderConfig cfg;
  cfg.domain_id = domain;
  cfg.participant_name = "stratum_diag";

  // 可选：指定 Reader 请求的 QoS（A3 QoS 兼容性验证用）。
  //   默认是最宽松取值（BEST_EFFORT + VOLATILE，见 QosRequest 注释），
  //   因此能接纳绝大多数发布者；这里允许收紧以验证"不兼容必须被拒绝"。
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--reader-durability=", 0) == 0) {
      const std::string v = a.substr(std::strlen("--reader-durability="));
      if (v == "transient_local") {
        cfg.requested_qos.durability_kind = 1;
      } else if (v == "volatile") {
        cfg.requested_qos.durability_kind = 0;
      } else {
        std::printf("FAILED: --reader-durability 只能是 volatile|transient_local\n");
        return 2;
      }
    } else if (a.rfind("--reader-reliability=", 0) == 0) {
      const std::string v = a.substr(std::strlen("--reader-reliability="));
      if (v == "reliable") {
        cfg.requested_qos.reliability_kind = 2;
      } else if (v == "best_effort") {
        cfg.requested_qos.reliability_kind = 1;
      } else {
        std::printf("FAILED: --reader-reliability 只能是 best_effort|reliable\n");
        return 2;
      }
    } else {
      std::printf("FAILED: 未知参数 %s\n", a.c_str());
      return 2;
    }
  }
  std::printf("READER_QOS reliability_kind=%u durability_kind=%u\n",
              cfg.requested_qos.reliability_kind,
              cfg.requested_qos.durability_kind);

  Status s = reader.Open(cfg);
  if (!s.ok()) {
    std::printf("FAILED: Open: %s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("Open OK，启动接收...\n");

  size_t samples = 0;

  // 【录制方向的端到端延迟 —— 唯一有意义的实时延迟】
  // 回放是"时间旅行"（源时间戳来自原始生成时刻），now-source 会包含
  // 从生成到回放的间隔；而这里对端是**实时发布**，reception-source 才是真实延迟。
  std::mutex stat_mutex;
  std::vector<double> lat_ms;
  std::vector<double> gap_ms;
  int64_t last_recv_ns = 0;

  s = reader.Start([&](const UserData& ud) -> bool {
    ++samples;
    {
      std::lock_guard<std::mutex> lk(stat_mutex);
      if (ud.source_timestamp_ns > 0 &&
          ud.reception_timestamp_ns >= ud.source_timestamp_ns) {
        lat_ms.push_back(static_cast<double>(ud.reception_timestamp_ns -
                                            ud.source_timestamp_ns) / 1e6);
      }
      if (last_recv_ns > 0 && ud.reception_timestamp_ns > last_recv_ns) {
        gap_ms.push_back(static_cast<double>(ud.reception_timestamp_ns - last_recv_ns) / 1e6);
      }
      last_recv_ns = ud.reception_timestamp_ns;
    }
    if (samples <= 5) {
      std::string key_hex;
      for (unsigned char c : ud.instance_key) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02x", c);
        key_hex += buf;
      }
      std::printf("  [数据 #%zu] topic=\"%s\" type=\"%s\" seq=%u src_ts=%lld "
                  "payload=%zu key_len=%zu key=%s\n",
                  samples, ud.topic_name.c_str(), ud.type_name.c_str(),
                  ud.sequence, static_cast<long long>(ud.source_timestamp_ns),
                  ud.payload.size(), ud.instance_key.size(),
                  key_hex.empty() ? "(无)" : key_hex.c_str());
      std::fflush(stdout);
    }
    return true;
  });
  if (!s.ok()) {
    std::printf("FAILED: Start: %s\n", s.ToString().c_str());
    return 1;
  }

  for (int i = 0; i < seconds; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto st = reader.GetStats();
    std::printf("  [%2ds] pkt=%llu spdp=%llu sedp_pub=%llu hb=%llu ack=%llu "
                "subs=%llu peers=%llu samples=%llu qos_rej=%llu\n",
                i + 1,
                static_cast<unsigned long long>(st.packets_total),
                static_cast<unsigned long long>(st.spdp_received),
                static_cast<unsigned long long>(st.sedp_publications),
                static_cast<unsigned long long>(st.heartbeats),
                static_cast<unsigned long long>(st.acknacks_sent),
                static_cast<unsigned long long>(st.subscriptions_sent),
                static_cast<unsigned long long>(st.peers_learned),
                static_cast<unsigned long long>(st.user_samples),
                static_cast<unsigned long long>(st.qos_rejected));
    std::fflush(stdout);
  }

  reader.Stop();
  auto topics = reader.DiscoveredTopics();
  std::printf("\n发现话题 %zu 个，已订阅 Writer %zu 个\n",
              topics.size(), reader.SubscribedWriterCount());
  for (size_t i = 0; i < topics.size() && i < 10; ++i) {
    std::printf("  %s\n", topics[i].c_str());
  }
  std::printf("用户回调样本 = %zu\n", samples);
  {
    std::lock_guard<std::mutex> lk(stat_mutex);
    // 【必须先排序再取分位与 max】printf 的参数求值顺序未定义，
    // 依赖"pct 内部排序后 back() 即最大值"会得到 max < p99 的荒谬结果。
    auto prep = [](std::vector<double>& v) {
      std::sort(v.begin(), v.end());
    };
    auto pct = [](const std::vector<double>& v, double q) -> double {
      if (v.empty()) return -1.0;
      size_t i = static_cast<size_t>(q * (v.size() - 1) + 0.5);
      return v[std::min(i, v.size() - 1)];
    };
    prep(lat_ms);
    prep(gap_ms);
    std::printf("READER_LATENCY_MS n=%zu min=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",
                lat_ms.size(), lat_ms.empty() ? -1.0 : lat_ms.front(),
                pct(lat_ms, 0.50), pct(lat_ms, 0.95), pct(lat_ms, 0.99),
                lat_ms.empty() ? -1.0 : lat_ms.back());
    std::printf("READER_JITTER_MS n=%zu p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",
                gap_ms.size(), pct(gap_ms, 0.50), pct(gap_ms, 0.95),
                pct(gap_ms, 0.99), gap_ms.empty() ? -1.0 : gap_ms.back());
    std::fflush(stdout);
  }
  return samples > 0 ? 0 : 2;
}
