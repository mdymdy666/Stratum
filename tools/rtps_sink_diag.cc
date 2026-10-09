// RtpsSink 独立诊断：直接观察发布端引擎的行为
//
// 为什么需要它：回放工具通过 Player 间接使用 sink，拿不到 Writer 的统计，
// 因而无法判断"SPDP 是否发出去了""是否发现了对端""数据发到了哪里"。
// 本工具直接持有 RtpsSink，可逐秒打印协议层指标。
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
  const int wait_s = (argc > 2) ? std::atoi(argv[2]) : 12;
  const int send_n = (argc > 3) ? std::atoi(argv[3]) : 20;

  BackendConfig bcfg;
  bcfg.domain_id = domain;
  bcfg.participant_name = "stratum_sink_diag";

  RtpsSink sink(bcfg);
  Status s = sink.Open();
  if (!s.ok()) {
    std::printf("FAILED: sink.Open: %s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("RtpsSink 已打开（domain %u），等待 %d 秒供对端发现...\n",
              domain, wait_s);
  std::fflush(stdout);

  for (int i = 0; i < wait_s; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto st = sink.GetWriterStats();
    std::printf("  [%2ds] pkt=%llu spdp=%llu sedp_sub=%llu peers=%llu "
                "pubs=%llu hb=%llu acks=%llu data=%llu retrans=%llu\n",
                i + 1,
                static_cast<unsigned long long>(st.packets_total),
                static_cast<unsigned long long>(st.spdp_received),
                static_cast<unsigned long long>(st.sedp_subscriptions),
                static_cast<unsigned long long>(st.peers_learned),
                static_cast<unsigned long long>(st.publications_announced),
                static_cast<unsigned long long>(st.heartbeats_sent),
                static_cast<unsigned long long>(st.acknacks_received),
                static_cast<unsigned long long>(st.data_sent),
                static_cast<unsigned long long>(st.retransmits));
    std::fflush(stdout);
  }

  std::printf("\n开始发布 %d 条...\n", send_n);
  // 构造一个类似 hello_world 的负载：encapsulation + uint32 index + string
  for (int i = 0; i < send_n; ++i) {
    std::string payload;
    payload.push_back('\x00');
    payload.push_back('\x03');   // PL_CDR_LE
    payload.push_back('\x00');
    payload.push_back('\x00');
    const uint32_t idx = static_cast<uint32_t>(i);
    payload.append(reinterpret_cast<const char*>(&idx), 4);
    const char* msg = "Hello world";
    const uint32_t len = 12;   // 含结尾 '\0'
    payload.append(reinterpret_cast<const char*>(&len), 4);
    payload.append(msg, 11);
    payload.push_back('\0');
    ReceivedSample sample;
    sample.topic_name = "hello_world_topic";
    sample.type_name = "HelloWorld";
    sample.payload = payload;
    sample.sequence = idx;
    const Status st2 = sink.Publish(sample);
    if (!st2.ok() && i < 3) {
      std::printf("  [发布 %d] %s\n", i, st2.ToString().c_str());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  const auto st = sink.GetWriterStats();
  std::printf("\n=== 发布后统计 ===\n");
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

  std::printf("再等 8 秒观察确认/重传...\n");
  for (int i = 0; i < 8; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto s2 = sink.GetWriterStats();
    std::printf("  [%2ds] acks=%llu retrans=%llu\n", i + 1,
                static_cast<unsigned long long>(s2.acknacks_received),
                static_cast<unsigned long long>(s2.retransmits));
  }

  sink.Close();
  std::printf("\n完成。\n");
  return 0;
}
