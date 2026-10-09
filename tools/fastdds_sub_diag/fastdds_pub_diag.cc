// ---------------------------------------------------------------------------
// 文件：tools/fastdds_sub_diag/fastdds_pub_diag.cc
// 用途：**最小 Fast DDS 发布端**，用于 QoS 兼容性验证（A3）
//
// 为什么要单独写一个：
//   上游 examples 的 hello_world publisher 用固定默认 QoS，无法造出
//   transient_local / best_effort 这类非默认组合，也就无法验证
//   Stratum 的 MatchWriterToReader 判定在真实对端上是否生效。
//   本程序复用 hello_world 的 TypeSupport，只把 QoS 做成可配置。
//
// 用法：
//   fastdds_pub_diag [--domain=N] [--topic=名] [--count=N] [--rate=每秒条数]
//                    [--durability=volatile|transient_local]
//                    [--reliability=best_effort|reliable]
// 默认：domain=0, topic=hello_world_topic, count=20, rate=2,
//       durability=volatile, reliability=reliable
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "HelloWorldPubSubTypes.hpp"

using namespace eprosima::fastdds::dds;

namespace {

struct Options
{
    uint32_t domain = 0;
    std::string topic = "hello_world_topic";
    int count = 20;
    double rate = 2.0;                       // 每秒条数
    std::string durability = "volatile";
    std::string reliability = "reliable";
};

void Usage()
{
    std::printf(
            "用法: fastdds_pub_diag [--domain=N] [--topic=名] [--count=N] [--rate=每秒条数]\n"
            "                       [--durability=volatile|transient_local]\n"
            "                       [--reliability=best_effort|reliable]\n");
}

bool Parse(int argc, char** argv, Options* opt)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        std::string key = arg;
        std::string value;
        const size_t eq = arg.find('=');
        if (eq != std::string::npos)
        {
            key = arg.substr(0, eq);
            value = arg.substr(eq + 1);
        }
        if (key == "--help" || key == "-h")
        {
            Usage();
            return false;
        }
        else if (key == "--domain")
        {
            opt->domain = static_cast<uint32_t>(std::atoi(value.c_str()));
        }
        else if (key == "--topic")
        {
            opt->topic = value;
        }
        else if (key == "--count")
        {
            opt->count = std::atoi(value.c_str());
        }
        else if (key == "--rate")
        {
            opt->rate = std::atof(value.c_str());
        }
        else if (key == "--durability")
        {
            if (value != "volatile" && value != "transient_local")
            {
                std::fprintf(stderr, "--durability 只能是 volatile|transient_local\n");
                return false;
            }
            opt->durability = value;
        }
        else if (key == "--reliability")
        {
            if (value != "best_effort" && value != "reliable")
            {
                std::fprintf(stderr, "--reliability 只能是 best_effort|reliable\n");
                return false;
            }
            opt->reliability = value;
        }
        else
        {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            Usage();
            return false;
        }
    }
    return true;
}

class PubListener : public DataWriterListener
{
public:

    std::atomic<int> matched{0};

    void on_publication_matched(
            DataWriter* /*writer*/,
            const PublicationMatchedStatus& status) override
    {
        matched.store(status.current_count);
        std::printf("PUB_MATCHED current_count=%d change=%d\n",
                    status.current_count, status.current_count_change);
        std::fflush(stdout);
    }
};

}  // namespace

int main(int argc, char** argv)
{
    Options opt;
    if (!Parse(argc, argv, &opt))
    {
        return 2;
    }

    std::printf("PUB_CONFIG domain=%u topic=\"%s\" count=%d rate=%.2f durability=%s reliability=%s\n",
                opt.domain, opt.topic.c_str(), opt.count, opt.rate,
                opt.durability.c_str(), opt.reliability.c_str());
    std::fflush(stdout);

    DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
    pqos.name("fastdds_pub_diag");
    DomainParticipant* participant =
            DomainParticipantFactory::get_instance()->create_participant(opt.domain, pqos);
    if (participant == nullptr)
    {
        std::fprintf(stderr, "create_participant 失败（domain=%u）\n", opt.domain);
        return 1;
    }

    TypeSupport type(new HelloWorldPubSubType());
    type.register_type(participant);

    Publisher* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    Topic* topic = participant->create_topic(opt.topic, type.get_type_name(), TOPIC_QOS_DEFAULT);
    if (publisher == nullptr || topic == nullptr)
    {
        std::fprintf(stderr, "创建 publisher/topic 失败\n");
        return 1;
    }

    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    publisher->get_default_datawriter_qos(wqos);
    wqos.durability().kind = (opt.durability == "transient_local")
            ? TRANSIENT_LOCAL_DURABILITY_QOS
            : VOLATILE_DURABILITY_QOS;
    wqos.reliability().kind = (opt.reliability == "best_effort")
            ? BEST_EFFORT_RELIABILITY_QOS
            : RELIABLE_RELIABILITY_QOS;

    PubListener listener;
    DataWriter* writer = publisher->create_datawriter(topic, wqos, &listener, StatusMask::all());
    if (writer == nullptr)
    {
        std::fprintf(stderr, "create_datawriter 失败（QoS 组合可能不被支持）\n");
        return 1;
    }

    // 给发现留时间（与消费者脚本的 sleep 配合）
    for (int i = 0; i < 30 && listener.matched.load() == 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const auto interval = (opt.rate > 0.0)
            ? std::chrono::microseconds(static_cast<int64_t>(1000000.0 / opt.rate))
            : std::chrono::microseconds(0);

    HelloWorld sample;
    int sent = 0;
    for (int i = 1; i <= opt.count; ++i)
    {
        sample.index(static_cast<uint32_t>(i));
        sample.message("Hello world");
        if (writer->write(&sample) == RETCODE_OK)
        {
            ++sent;
            if (sent <= 5 || (sent % 10) == 0)
            {
                std::printf("PUB_SENT index=%d\n", i);
                std::fflush(stdout);
            }
        }
        else
        {
            std::printf("PUB_WRITE_FAILED index=%d\n", i);
            std::fflush(stdout);
        }
        if (interval.count() > 0)
        {
            std::this_thread::sleep_for(interval);
        }
    }

    std::printf("PUB_DONE sent=%d matched_now=%d\n", sent, listener.matched.load());
    std::fflush(stdout);

    // 留时间让可靠传输完成
    std::this_thread::sleep_for(std::chrono::seconds(3));
    participant->delete_contained_entities();
    DomainParticipantFactory::get_instance()->delete_participant(participant);
    return 0;
}
