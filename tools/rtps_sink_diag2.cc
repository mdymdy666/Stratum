// RtpsSink 诊断工具 v2：带协议层调试输出
//
// 与 v1 的差别：设置 STRATUM_RTPS_DEBUG=1 时，RtpsWriter 会打印
// 收到的每个 DATA 子消息的 writerId 与来源地址，用于判断
// "是对端没发"还是"发了但被丢弃/解析不符"。
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "stratum/protocol/rtps/rtps_backend.h"

using namespace stratum;
using namespace stratum::protocol;
using namespace stratum::protocol::rtps;

int main(int argc, char** argv) {
  const uint32_t domain = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 5;
  const int wait_s = (argc > 2) ? std::atoi(argv[2]) : 15;
  const int send_n = (argc > 3) ? std::atoi(argv[3]) : 10;

  BackendConfig bcfg;
  bcfg.domain_id = domain;
  bcfg.participant_name = "stratum_sink_diag2";

  RtpsSink sink(bcfg);
  Status s = sink.Open();
  if (!s.ok()) {
    std::printf("FAILED: sink.Open: %s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("RtpsSink 打开（domain %u），等 %d 秒...\n", domain, wait_s);
  std::fflush(stdout);

  for (int i = 0; i < wait_s; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto st = sink.GetWriterStats();
    std::printf("  [%2ds] pkt=%llu spdp=%llu sedp_sub=%llu peers=%llu pubs=%llu\n",
                i + 1,
                static_cast<unsigned long long>(st.packets_total),
                static_cast<unsigned long long>(st.spdp_received),
                static_cast<unsigned long long>(st.sedp_subscriptions),
                static_cast<unsigned long long>(st.peers_learned),
                static_cast<unsigned long long>(st.publications_announced));
    std::fflush(stdout);
  }

  std::printf("\n开始发布 %d 条...\n", send_n);
  for (int i = 0; i < send_n; ++i) {
    ReceivedSample sample;
    sample.topic_name = "hello_world_topic";
    sample.type_name = "HelloWorld";
    std::string payload;
    payload.push_back('\x00');
    payload.push_back('\x03');
    payload.push_back('\x00');
    payload.push_back('\x00');
    const uint32_t idx = static_cast<uint32_t>(i);
    payload.append(reinterpret_cast<const char*>(&idx), 4);
    const uint32_t len = 12;
    payload.append(reinterpret_cast<const char*>(&len), 4);
    payload.append("Hello world", 11);
    payload.push_back('\0');
    sample.payload = payload;
    sample.sequence = idx;
    const Status ps = sink.Publish(sample);
    if (i < 3 || !ps.ok()) {
      std::printf("  [发布 %d] %s\n", i, ps.ToString().c_str());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  const auto st = sink.GetWriterStats();
  std::printf("\n=== 发布后 ===\n");
  std::printf("  pkt=%llu spdp=%llu sedp_sub=%llu peers=%llu pubs=%llu "
              "hb=%llu acks=%llu data=%llu retrans=%llu\n",
              static_cast<unsigned long long>(st.packets_total),
              static_cast<unsigned long long>(st.spdp_received),
              static_cast<unsigned long long>(st.sedp_subscriptions),
              static_cast<unsigned long long>(st.peers_learned),
              static_cast<unsigned long long>(st.publications_announced),
              static_cast<unsigned long long>(st.heartbeats_sent),
              static_cast<unsigned long long>(st.acknacks_received),
              static_cast<unsigned long long>(st.data_sent),
              static_cast<unsigned long long>(st.retransmits));

  std::printf("再等 8 秒...\n");
  for (int i = 0; i < 8; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto s2 = sink.GetWriterStats();
    std::printf("  [%2ds] pkt=%llu sedp_sub=%llu acks=%llu retrans=%llu\n",
                i + 1,
                static_cast<unsigned long long>(s2.packets_total),
                static_cast<unsigned long long>(s2.sedp_subscriptions),
                static_cast<unsigned long long>(s2.acknacks_received),
                static_cast<unsigned long long>(s2.retransmits));
  }

  sink.Close();
  std::printf("完成。\n");
  return 0;
}
