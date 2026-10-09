// M2 验收监听器：真实 Fast DDS 参与者，报告发现的远端参与者
//
// 只用**公共 API**（DomainParticipantListener + ParticipantBuiltinTopicData），
// 不依赖 src/cpp 内部头，避免版本与路径耦合。

#include <chrono>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <thread>

#include <fastdds/dds/builtin/topic/ParticipantBuiltinTopicData.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipantListener.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/rtps/participant/ParticipantDiscoveryInfo.hpp>

using namespace eprosima::fastdds::dds;

namespace {

int discovered_count = 0;
int removed_count = 0;

class Listener : public DomainParticipantListener {
 public:
  void on_participant_discovery(
      DomainParticipant* /*participant*/,
      eprosima::fastdds::rtps::ParticipantDiscoveryStatus reason,
      const ParticipantBuiltinTopicData& info,
      bool& should_be_ignored) override {
    should_be_ignored = false;
    const auto& p = info.guid.guidPrefix.value;
    char hex[32] = {0};
    std::snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x%02x%02x",
                  p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

    if (reason == eprosima::fastdds::rtps::ParticipantDiscoveryStatus::DISCOVERED_PARTICIPANT) {
      ++discovered_count;
      std::printf("DISCOVERED participant: guidPrefix=%s name=%s\n",
                  hex, info.participant_name.c_str());
    } else {
      ++removed_count;
      std::printf("REMOVED participant: guidPrefix=%s\n", hex);
    }
    std::fflush(stdout);
  }
};

}  // namespace

int main(int argc, char** argv) {
  const int seconds = (argc > 1) ? std::atoi(argv[1]) : 10;
  // domain 必须可配：domain 0 的 SPDP 组播端口 7400 常年被 ROS 2 daemon
  // 占据（实测 ros2-daemon --ros-domain-id 0 占用 7400），会干扰验收。
  const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;

  // 打开 Fast DDS 的详细日志：SPDP 解析失败的原因会在这里说明
  eprosima::fastdds::dds::Log::SetVerbosity(
      eprosima::fastdds::dds::Log::Kind::Info);
  eprosima::fastdds::dds::Log::SetCategoryFilter(
      std::regex("RTPS_MSG_IN|RTPS_PDP|RTPS_SPDP|RTPS_PARTICIPANT"));

  DomainParticipantQos pqos;
  pqos.name("stratum_xcheck_listener");

  Listener listener;
  DomainParticipant* participant =
      DomainParticipantFactory::get_instance()->create_participant(domain, pqos, &listener);
  if (participant == nullptr) {
    std::printf("FAILED: create_participant\n");
    return 1;
  }
  std::printf("Fast DDS 监听中（domain %u），持续 %d 秒...\n", domain, seconds);
  std::fflush(stdout);

  std::this_thread::sleep_for(std::chrono::seconds(seconds));

  DomainParticipantFactory::get_instance()->delete_participant(participant);

  std::printf("结果：discovered=%d removed=%d\n", discovered_count, removed_count);
  return discovered_count > 0 ? 0 : 3;
}
