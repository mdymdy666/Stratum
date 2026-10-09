// 交叉验证生产者：用真实 Fast DDS 创建一个 participant，
// 触发 SPDP 公告（真实 RTPS 网络流量），供 Stratum 解码验证。
//
// 编译：链接 build-fastdds 的 libfastdds.so

#include <chrono>
#include <cstdio>
#include <thread>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>

using namespace eprosima::fastdds::dds;

int main() {
  DomainParticipantQos pqos;
  pqos.name("stratum_xcheck_producer");

  DomainParticipant* participant =
      DomainParticipantFactory::get_instance()->create_participant(0, pqos);

  if (participant == nullptr) {
    std::printf("FAILED: create_participant returned nullptr\n");
    return 1;
  }
  std::printf("OK: participant created (domain 0), sending SPDP...\n");
  std::fflush(stdout);

  // 保持存活，让 SPDP 公告发出去（默认周期 100ms 左右）
  std::this_thread::sleep_for(std::chrono::seconds(4));

  DomainParticipantFactory::get_instance()->delete_participant(participant);
  std::printf("OK: participant deleted\n");
  return 0;
}
