// ---------------------------------------------------------------------------
// 文件：tools/fastdds_sub_diag/fastdds_sub_diag.cc
// 用途：用**真实 Fast DDS 订阅端**拿到 "Fast DDS 为什么拒绝 Stratum 的
//       SEDP Publication" 的确切原因。
//
// 为什么需要它（背景，来自 design/11-rtps-interop-handoff.md 第 1、6 节）
//   Stratum ↔ Stratum 的 RTPS 互通已完全打通，但与 Fast DDS 的互通卡在
//   "我方 SEDP Publication 未被接受"：
//     STRATUM_DUMP=1 显示全部 32 条用户数据都以 has_user=0 发往 Fast DDS 的
//     metatraffic 端口（7410/7438）。has_user 只能由 HandleSedpSubscription
//     （收到对端 SEDP Subscription）设置，因此对端**没有回 Subscription**
//     ⇒ 对端未接受我方 Publication ⇒ 我们学不到对端 user locator。
//   两条"省事"的路已被验证不可用：
//     · 环境变量 FASTDDS_LOG_VERBOSITY —— libfastdds 中没有该符号
//     · XML <log><verbosity>        —— Fast DDS 3.6 parseLogConfig 不接受该标签
//   所以只能自己写一个最小订阅端，调用 Log::SetVerbosity(Info) 打开日志，
//   让 Fast DDS 自己说出拒绝的理由。
//
// 观察点（按 Fast DDS 内部处理顺序）
//   1) 我方 Publication 是否被成功解析
//      · 成功 → EDPSimpleListeners.cpp 打印
//        "EDPSimpleListener: No TypeLookupManager or TypeInformation.
//         Trying fallback mechanism"（INFO/RTPS_EDP），
//        并触发本程序的 on_data_writer_discovery
//      · 失败（WriterProxyData::read_from_cdr_message 返回 false）→ 该分支
//        在上游**没有任何日志**（EDPSimpleListeners.cpp:76 的 if 直接跳过），
//        表现为两个回调都不触发
//   2) 是否通过匹配判定 EDP::valid_matching（EDP.cpp:856）
//      · QoS 不兼容 → "INCOMPATIBLE QOS ..." WARNING（本程序最想拿到的信息）
//      · topic / type_name 不一致 → 上游**静默失败**（无日志），
//        只能靠"发现了 writer 但没有订阅匹配"反推
//   3) 匹配成功 → on_subscription_matched(current_count_change=+1)
//
// 类型支持复用 examples/cpp/hello_world 的 HelloWorld TypeSupport
//   （types/ 目录，与上游文件 md5 一致，未重新生成 IDL）
//
// 用法：
//   fastdds_sub_diag [seconds] [domain] [topic] [选项]
//     选项：
//       --verbosity=info|warning|error
//       --reader-reliability=reliable|besteffort
//       --reader-durability=volatile|transient_local
//       --reader-representation=xcdr1|xcdr2|both
//
// 输出：
//   · 事件行（[diag][PDP] / [diag][EDP] / [diag][MATCH] / [diag][DATA] / ...）
//   · 最后一行机器可解析的汇总：
//     DIAG_SUMMARY ... writers_discovered=.. matched_events=.. samples=..
// ---------------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fastdds/dds/builtin/topic/ParticipantBuiltinTopicData.hpp>
#include <fastdds/dds/builtin/topic/PublicationBuiltinTopicData.hpp>
#include <fastdds/dds/core/status/IncompatibleQosStatus.hpp>
#include <fastdds/dds/core/status/LivelinessChangedStatus.hpp>
#include <fastdds/dds/core/status/SampleRejectedStatus.hpp>
#include <fastdds/dds/core/status/SubscriptionMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipantListener.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/subscriber/qos/SubscriberQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/qos/TopicQos.hpp>
#include <fastdds/utils/IPLocator.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.hpp>
#include <fastdds/rtps/participant/ParticipantDiscoveryInfo.hpp>
#include <fastdds/rtps/writer/WriterDiscoveryStatus.hpp>

#include "HelloWorldPubSubTypes.hpp"

using namespace eprosima::fastdds::dds;

using eprosima::fastdds::rtps::GUID_t;
using eprosima::fastdds::rtps::iHandle2GUID;
using eprosima::fastdds::rtps::ParticipantDiscoveryStatus;
using eprosima::fastdds::rtps::WriterDiscoveryStatus;

namespace {

// ---------------------------------------------------------------------------
// 输出：所有回调都可能在 Fast DDS 内部线程里执行，必须加锁 + 立即 flush，
// 否则进程被 kill 时输出会丢（本项目历史上被 stdout 全缓冲坑过一次）。
// ---------------------------------------------------------------------------
std::mutex g_out_mutex;

void note(
        const char* fmt,
        ...)
{
    std::lock_guard<std::mutex> guard(g_out_mutex);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

std::string guid_str(
        const GUID_t& guid)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x:%02x%02x%02x%02x",
            guid.guidPrefix.value[0], guid.guidPrefix.value[1],
            guid.guidPrefix.value[2], guid.guidPrefix.value[3],
            guid.entityId.value[0], guid.entityId.value[1],
            guid.entityId.value[2], guid.entityId.value[3]);
    return std::string(buf);
}

std::string handle_str(
        const InstanceHandle_t& handle)
{
    return guid_str(iHandle2GUID(handle));
}

// DataRepresentation 是序列：writer 侧只看第一个元素（EDP::checkDataRepresentationQos），
// 打印整个序列便于判断交集。
std::string representations_str(
        const DataRepresentationQosPolicy& policy)
{
    if (policy.m_value.empty())
    {
        return "(empty=default)";
    }
    std::string out;
    for (size_t i = 0; i < policy.m_value.size(); ++i)
    {
        if (i != 0)
        {
            out += ",";
        }
        out += std::to_string(static_cast<int>(policy.m_value[i]));
    }
    return out;
}

// ---------------------------------------------------------------------------
// 统计：全部用 atomic，回调在库线程执行
// ---------------------------------------------------------------------------
struct Stats
{
    std::atomic<uint64_t> participants_discovered{0};
    std::atomic<uint64_t> participants_removed{0};
    std::atomic<uint64_t> writers_discovered{0};
    std::atomic<uint64_t> writers_changed_qos{0};
    std::atomic<uint64_t> writers_removed{0};
    std::atomic<uint64_t> matched_events{0};
    std::atomic<uint64_t> unmatched_events{0};
    std::atomic<int64_t> matched_now{0};
    std::atomic<uint64_t> samples{0};
    // 区分"数据没到 DataReader"与"到了但反序列化/交付失败"：
    // on_data_available 被调用的次数、以及 take_next_sample 的返回情况。
    std::atomic<uint64_t> data_available_calls{0};
    std::atomic<uint64_t> invalid_data_samples{0};
    std::atomic<uint64_t> take_error{0};
    std::atomic<int64_t> last_take_rc{0};
    std::atomic<uint64_t> incompatible_qos_events{0};
    std::atomic<int64_t> last_incompatible_policy{-1};
    std::atomic<uint64_t> liveliness_events{0};
    std::atomic<uint64_t> sample_rejected_events{0};

    // 端到端延迟（接收时刻 - 样本源时间戳），毫秒。
    // 【必须在接收端测量】发送端只能观测到"写出去"，无法知道对端何时读到。
    std::mutex latency_mutex;
    std::vector<double> lat_ms;

    // 到达间隔（抖动）。它与回放速率无关，是回放方向**唯一有意义的时序指标**
    // （回放方向的 now-source 天然包含"原始生成→回放"的间隔，见场景 10 说明）。
    std::vector<double> gap_ms;
    int64_t last_recv_ns = 0;
};

Stats g_stats;

// 延迟分位（输入会被排序；空集返回 -1）
static double LatencyPercentile(std::vector<double>& v, double p)
{
    if (v.empty())
    {
        return -1.0;
    }
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
    if (idx >= v.size())
    {
        idx = v.size() - 1;
    }
    return v[idx];
}

// ---------------------------------------------------------------------------
// Participant 级监听：
//   on_data_writer_discovery 是否被调用，是"我方 Publication 是否被 Fast DDS
//   成功解析并登记"的判据（PDP.cpp:1104 在 addWriterProxyData 成功后才通知）。
// ---------------------------------------------------------------------------
class DiagParticipantListener : public DomainParticipantListener
{
public:

    void on_participant_discovery(
            DomainParticipant* /*participant*/,
            ParticipantDiscoveryStatus reason,
            const ParticipantBuiltinTopicData& info,
            bool& should_be_ignored) override
    {
        should_be_ignored = false;
        switch (reason)
        {
            case ParticipantDiscoveryStatus::DISCOVERED_PARTICIPANT:
                g_stats.participants_discovered.fetch_add(1);
                note("[diag][PDP] DISCOVERED_PARTICIPANT guid=%s name=\"%s\"",
                        guid_str(info.guid).c_str(), info.participant_name.c_str());
                break;
            case ParticipantDiscoveryStatus::REMOVED_PARTICIPANT:
            case ParticipantDiscoveryStatus::DROPPED_PARTICIPANT:
                g_stats.participants_removed.fetch_add(1);
                note("[diag][PDP] REMOVED/DROPPED_PARTICIPANT guid=%s name=\"%s\"",
                        guid_str(info.guid).c_str(), info.participant_name.c_str());
                break;
            case ParticipantDiscoveryStatus::CHANGED_QOS_PARTICIPANT:
                note("[diag][PDP] CHANGED_QOS_PARTICIPANT guid=%s",
                        guid_str(info.guid).c_str());
                break;
            default:
                note("[diag][PDP] IGNORED_PARTICIPANT guid=%s",
                        guid_str(info.guid).c_str());
                break;
        }
    }

    void on_data_writer_discovery(
            DomainParticipant* /*participant*/,
            WriterDiscoveryStatus reason,
            const PublicationBuiltinTopicData& info,
            bool& should_be_ignored) override
    {
        should_be_ignored = false;
        const char* reason_str = "UNKNOWN";
        switch (reason)
        {
            case WriterDiscoveryStatus::DISCOVERED_WRITER:
                reason_str = "DISCOVERED_WRITER";
                g_stats.writers_discovered.fetch_add(1);
                break;
            case WriterDiscoveryStatus::CHANGED_QOS_WRITER:
                reason_str = "CHANGED_QOS_WRITER";
                g_stats.writers_changed_qos.fetch_add(1);
                break;
            case WriterDiscoveryStatus::REMOVED_WRITER:
                reason_str = "REMOVED_WRITER";
                g_stats.writers_removed.fetch_add(1);
                break;
            case WriterDiscoveryStatus::IGNORED_WRITER:
                reason_str = "IGNORED_WRITER";
                break;
        }
        note("[diag][EDP] %s guid=%s participant_guid=%s topic=\"%s\" type=\"%s\" "
             "topic_kind=%d reliability=%d durability=%d ownership=%d deadline=%d.%u "
             "liveliness_kind=%d liveliness_lease=%d.%u representation=%s "
             "max_serialized_size=%u type_information=%s",
                reason_str,
                guid_str(info.guid).c_str(),
                guid_str(info.participant_guid).c_str(),
                info.topic_name.c_str(),
                info.type_name.c_str(),
                static_cast<int>(info.topic_kind),
                static_cast<int>(info.reliability.kind),
                static_cast<int>(info.durability.kind),
                static_cast<int>(info.ownership.kind),
                static_cast<int>(info.deadline.period.seconds),
                static_cast<unsigned>(info.deadline.period.nanosec),
                static_cast<int>(info.liveliness.kind),
                static_cast<int>(info.liveliness.lease_duration.seconds),
                static_cast<unsigned>(info.liveliness.lease_duration.nanosec),
                representations_str(info.representation).c_str(),
                static_cast<unsigned>(info.max_serialized_size),
                info.type_information.assigned() ? "assigned" : "absent");
    }

};

// ---------------------------------------------------------------------------
// Reader 级监听：匹配结果、数据、以及各类"没匹配上"的状态回调
// ---------------------------------------------------------------------------
class DiagReaderListener : public DataReaderListener
{
public:

    void on_subscription_matched(
            DataReader* /*reader*/,
            const SubscriptionMatchedStatus& info) override
    {
        g_stats.matched_now.fetch_add(info.current_count_change);
        if (info.current_count_change > 0)
        {
            g_stats.matched_events.fetch_add(1);
        }
        else if (info.current_count_change < 0)
        {
            g_stats.unmatched_events.fetch_add(1);
        }
        note("[diag][MATCH] change=%d current_count=%d total_count=%d total_change=%d "
             "last_publication=%s",
                static_cast<int>(info.current_count_change),
                static_cast<int>(info.current_count),
                static_cast<int>(info.total_count),
                static_cast<int>(info.total_count_change),
                handle_str(info.last_publication_handle).c_str());
    }

    void on_data_available(
            DataReader* reader) override
    {
        const uint64_t call_no = g_stats.data_available_calls.fetch_add(1) + 1;
        if (call_no <= 3)
        {
            note("[diag][DATA] on_data_available 第 %llu 次被调用（说明数据已到达 DataReader）",
                    static_cast<unsigned long long>(call_no));
        }

        SampleInfo info;
        ReturnCode_t rc;
        while ((rc = reader->take_next_sample(&sample_, &info)) == RETCODE_OK)
        {
            if (!info.valid_data)
            {
                // 反序列化失败或非 ALIVE 实例都会走到这里。
                // 必须报出来：否则"数据到了但读不出来"会被误判成"数据没到"。
                const uint64_t bad = g_stats.invalid_data_samples.fetch_add(1) + 1;
                if (bad <= 3)
                {
                    note("[diag][DATA] 第 %llu 条 invalid_data（instance_state=%d，"
                         "反序列化失败或实例非 ALIVE）",
                            static_cast<unsigned long long>(bad),
                            static_cast<int>(info.instance_state));
                }
                continue;
            }
            // 记录端到端延迟：now - 源时间戳（INFO_TS 由发送端写入）。
            {
                const int64_t src_ns = info.source_timestamp.to_ns();
                const int64_t now_ns =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
                if (src_ns > 0 && now_ns >= src_ns)
                {
                    std::lock_guard<std::mutex> lk(g_stats.latency_mutex);
                    g_stats.lat_ms.push_back(
                            static_cast<double>(now_ns - src_ns) / 1e6);
                }
                {
                    std::lock_guard<std::mutex> lk(g_stats.latency_mutex);
                    if (g_stats.last_recv_ns > 0 && now_ns > g_stats.last_recv_ns)
                    {
                        g_stats.gap_ms.push_back(
                                static_cast<double>(now_ns - g_stats.last_recv_ns) / 1e6);
                    }
                    g_stats.last_recv_ns = now_ns;
                }
            }
            const uint64_t n = g_stats.samples.fetch_add(1) + 1;
            // 【降低打印频率】此前每 10 条就 fprintf 一次；在"尽快/数万条每秒"
            // 的档位下，这个输出本身会成为接收端瓶颈（日志重定向到文件），
            // 表现成"Stratum 丢包"，实为测试工具自伤。
            if (n <= 3 || (n % 1000) == 0)
            {
                note("[diag][DATA] #%llu index=%u message=\"%s\"",
                        static_cast<unsigned long long>(n), sample_.index(),
                        sample_.message().c_str());
            }
        }
        g_stats.last_take_rc.store(static_cast<int64_t>(rc));
        if (rc != RETCODE_NO_DATA)
        {
            const uint64_t errs = g_stats.take_error.fetch_add(1) + 1;
            if (errs <= 5)
            {
                note("[diag][DATA] take_next_sample 返回非 NO_DATA 码 = %d",
                        static_cast<int>(rc));
            }
        }
    }

    void on_requested_incompatible_qos(
            DataReader* /*reader*/,
            const RequestedIncompatibleQosStatus& status) override
    {
        g_stats.incompatible_qos_events.fetch_add(1);
        g_stats.last_incompatible_policy.store(static_cast<int64_t>(status.last_policy_id));
        note("[diag][QOS] REQUESTED_INCOMPATIBLE_QOS total_count=%u total_change=%u "
             "last_policy_id=%d",
                status.total_count, status.total_count_change,
                static_cast<int>(status.last_policy_id));
    }

    void on_liveliness_changed(
            DataReader* /*reader*/,
            const LivelinessChangedStatus& status) override
    {
        g_stats.liveliness_events.fetch_add(1);
        note("[diag][LIVELINESS] alive_count=%d not_alive_count=%d alive_change=%d "
             "not_alive_change=%d last_publication=%s",
                static_cast<int>(status.alive_count),
                static_cast<int>(status.not_alive_count),
                static_cast<int>(status.alive_count_change),
                static_cast<int>(status.not_alive_count_change),
                handle_str(status.last_publication_handle).c_str());
    }

    void on_sample_rejected(
            DataReader* /*reader*/,
            const SampleRejectedStatus& status) override
    {
        g_stats.sample_rejected_events.fetch_add(1);
        note("[diag][REJECTED] total_count=%u total_change=%u last_reason=%d "
             "last_instance=%s",
                status.total_count, status.total_count_change,
                static_cast<int>(status.last_reason),
                handle_str(status.last_instance_handle).c_str());
    }

private:

    HelloWorld sample_;
};

// ---------------------------------------------------------------------------
// 命令行
// ---------------------------------------------------------------------------
struct Options
{
    int seconds = 20;
    uint32_t domain = 0;
    std::string topic = "hello_world_topic";
    Log::Kind verbosity = Log::Kind::Info;

    bool set_reliability = false;
    ReliabilityQosPolicyKind reliability = RELIABLE_RELIABILITY_QOS;

    bool set_durability = false;
    DurabilityQosPolicyKind durability = VOLATILE_DURABILITY_QOS;

    bool set_representation = false;
    std::vector<DataRepresentationId_t> representation;

    // 显式端口与初始对端。
    //
    // 为什么需要：本环境的 WSL 是 mirrored networking，组播加入会失败
    // （日志里的 "Error joining multicast group on 10.255.255.254:
    //  Address already in use"），仅靠 SPDP 组播经常发现不了对端；
    // 用 initialPeersList 直接单播给对端的 metatraffic 端口可以绕开组播。
    // 固定端口则让抓包/端口对照成为可能（不再依赖 Fast DDS 的自动分配）。
    uint16_t meta_port = 0;
    uint16_t user_port = 0;
    // 指定 participantId（决定端口 7410+2*id 与 7411+2*id）。
    //
    // 比直接改 locator 列表更安全：清空 metatrafficUnicastLocatorList 会
    // 让 SPDP 发现彻底失效（实测 participants_discovered 直接变 0）。
    // 而改 participantId 只挪端口号，不动发现逻辑，还能让本端**独享**
    // user 端口 —— 否则与同机其它 participant 共享端口时，内核会把数据报
    // 投递给别的 socket，表现为"匹配成功但一条样本也收不到"。
    int32_t participant_id = -1;
    // participant 级 listener 的事件掩码。
    //
    // 【必须为 none —— 这是一个真实踩过的坑】
    // DomainParticipantListener 同时继承 SubscriberListener → DataReaderListener，
    // 因此它也有一份 on_data_available。若 participant 用 StatusMask::all() 注册，
    // DataReader 的 DATA_AVAILABLE 事件会走 participant 级 listener 的默认空实现
    // （本类没有重写它）→ **事件被吞掉**，DataReader 自己的 listener 永远收不到。
    // 症状极具迷惑性：匹配成功、数据与心跳都确认发到本端端口，
    // 但 on_data_available 一次都不触发（data_available_calls=0）。
    // 实测对照（上游 hello_world publisher → 本程序）：
    //   --participant-mask=all  → samples=0
    //   --participant-mask=none → samples=10
    // 注意：掩码设为 none 之后 on_participant_discovery / on_data_writer_discovery
    // **仍然照常回调**（它们属于发现层回调，不受 StatusMask 控制），
    // 因此诊断能力没有损失。上游 hello_world 示例同样使用 StatusMask::none()。
    bool participant_mask_none = true;
    std::vector<std::string> peers;   // "ip:port"
    // 限定 Fast DDS 使用的本地接口（如 127.0.0.1）。
    // 本环境 WSL 接口枚举会把 10.255.255.254 之类地址也纳入，组播加入失败、
    // 且监听可能落在非 loopback 地址上，导致对端（只往 127.0.0.1 发）的
    // SPDP 收不到。限定接口是"最小化环境变量"的必要手段。
    std::string local_address;
};

bool split_option(
        const std::string& arg,
        std::string& key,
        std::string& value)
{
    const size_t eq = arg.find('=');
    if ((arg.rfind("--", 0) != 0) || (eq == std::string::npos))
    {
        return false;
    }
    key = arg.substr(2, eq - 2);
    value = arg.substr(eq + 1);
    return true;
}

void usage()
{
    std::printf(
        "用法: fastdds_sub_diag [seconds] [domain] [topic] [选项]\n"
        "  --verbosity=info|warning|error\n"
        "  --reader-reliability=reliable|besteffort\n"
        "  --reader-durability=volatile|transient_local\n"
        "  --reader-representation=xcdr1|xcdr2|both\n"
        "  --meta-port=<n>            固定本端 metatraffic 单播端口\n"
        "  --user-port=<n>            固定本端 user 单播端口\n"
        "  --peer=<ip:port>           初始对端（可重复，绕开组播发现）\n"
        "  --local-address=<ip>       只在指定本地接口收发（如 127.0.0.1）\n"
        "  --participant-id=<n>       指定 participantId（独享端口，避开同机冲突）\n"
        "  --participant-mask=none|all participant 级事件掩码（对照实验用）\n");
}

bool parse_args(
        int argc,
        char** argv,
        Options& opt)
{
    int positional = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        std::string key;
        std::string value;
        if (split_option(arg, key, value))
        {
            if (key == "verbosity")
            {
                if (value == "info")
                {
                    opt.verbosity = Log::Kind::Info;
                }
                else if (value == "warning")
                {
                    opt.verbosity = Log::Kind::Warning;
                }
                else if (value == "error")
                {
                    opt.verbosity = Log::Kind::Error;
                }
                else
                {
                    std::fprintf(stderr, "未知 verbosity: %s\n", value.c_str());
                    return false;
                }
            }
            else if (key == "reader-reliability")
            {
                opt.set_reliability = true;
                if (value == "reliable")
                {
                    opt.reliability = RELIABLE_RELIABILITY_QOS;
                }
                else if (value == "besteffort")
                {
                    opt.reliability = BEST_EFFORT_RELIABILITY_QOS;
                }
                else
                {
                    std::fprintf(stderr, "未知 reader-reliability: %s\n", value.c_str());
                    return false;
                }
            }
            else if (key == "reader-durability")
            {
                opt.set_durability = true;
                if (value == "volatile")
                {
                    opt.durability = VOLATILE_DURABILITY_QOS;
                }
                else if ((value == "transient_local") || (value == "transient-local"))
                {
                    opt.durability = TRANSIENT_LOCAL_DURABILITY_QOS;
                }
                else
                {
                    std::fprintf(stderr, "未知 reader-durability: %s\n", value.c_str());
                    return false;
                }
            }
            else if (key == "reader-representation")
            {
                opt.set_representation = true;
                opt.representation.clear();
                if ((value == "xcdr1") || (value == "both"))
                {
                    opt.representation.push_back(XCDR_DATA_REPRESENTATION);
                }
                if ((value == "xcdr2") || (value == "both"))
                {
                    opt.representation.push_back(XCDR2_DATA_REPRESENTATION);
                }
                if (opt.representation.empty())
                {
                    std::fprintf(stderr, "未知 reader-representation: %s\n", value.c_str());
                    return false;
                }
            }
            else if (key == "meta-port")
            {
                opt.meta_port = static_cast<uint16_t>(std::atoi(value.c_str()));
            }
            else if (key == "user-port")
            {
                opt.user_port = static_cast<uint16_t>(std::atoi(value.c_str()));
            }
            else if (key == "peer")
            {
                if (value.find(':') == std::string::npos)
                {
                    std::fprintf(stderr, "peer 必须是 ip:port 形式: %s\n", value.c_str());
                    return false;
                }
                opt.peers.push_back(value);
            }
            else if (key == "local-address")
            {
                opt.local_address = value;
            }
            else if (key == "participant-id")
            {
                opt.participant_id = std::atoi(value.c_str());
            }
            else if (key == "participant-mask")
            {
                if (value == "none") { opt.participant_mask_none = true; }
                else if (value == "all") { opt.participant_mask_none = false; }
                else
                {
                    std::fprintf(stderr, "participant-mask 只能是 none|all: %s\n", value.c_str());
                    return false;
                }
            }
            else
            {
                std::fprintf(stderr, "未知选项: %s\n", arg.c_str());
                return false;
            }
        }
        else if (positional == 0)
        {
            opt.seconds = std::atoi(arg.c_str());
            ++positional;
        }
        else if (positional == 1)
        {
            opt.domain = static_cast<uint32_t>(std::atoi(arg.c_str()));
            ++positional;
        }
        else if (positional == 2)
        {
            opt.topic = arg;
            ++positional;
        }
        else
        {
            std::fprintf(stderr, "多余的位置参数: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

// 便于人类阅读的 QoS 描述（打印本端请求的 QoS）
std::string reader_qos_str(
        const Options& opt,
        const DataReaderQos& rqos)
{
    std::string out;
    out += "reliability=";
    out += std::to_string(static_cast<int>(rqos.reliability().kind));
    out += " durability=";
    out += std::to_string(static_cast<int>(rqos.durability().kind));
    out += " representation=";
    out += representations_str(rqos.representation());
    out += opt.set_representation ? " (explicit)" : " (library default)";
    return out;
}

}  // namespace

int main(
        int argc,
        char** argv)
{
    Options opt;
    if (!parse_args(argc, argv, opt))
    {
        usage();
        return 2;
    }

    // 关键：打开 Fast DDS 日志（这是本程序存在的理由）。
    // 必须在使用任何 DDS 实体之前设置。
    Log::SetVerbosity(opt.verbosity);
    Log::Flush();
    note("[diag] Fast DDS Log verbosity = %s (0=Error,1=Warning,2=Info)",
            opt.verbosity == Log::Kind::Info ? "Info" :
            (opt.verbosity == Log::Kind::Warning ? "Warning" : "Error"));
    note("[diag] domain=%u topic=\"%s\" duration=%ds",
            opt.domain, opt.topic.c_str(), opt.seconds);

    DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
    pqos.name("fastdds_sub_diag");

    // 指定 participantId：让本端独享 metatraffic/user 端口（见 Options 注释）
    if (opt.participant_id >= 0)
    {
        pqos.wire_protocol().participant_id = opt.participant_id;
        note("[diag] participant_id=%d（metatraffic=%d user=%d）",
                opt.participant_id, 7410 + 2 * opt.participant_id,
                7411 + 2 * opt.participant_id);
    }

    // 限定本地接口（可选）：只在指定地址上收发。
    if (!opt.local_address.empty())
    {
        auto udp = std::make_shared<eprosima::fastdds::rtps::UDPv4TransportDescriptor>();
        udp->interfaceWhiteList.push_back(opt.local_address);
        pqos.transport().user_transports.push_back(udp);
        pqos.transport().use_builtin_transports = false;
        note("[diag] 传输层限定接口: %s（禁用内建传输的其它接口）",
                opt.local_address.c_str());
    }

    // 固定本端 locator（可选）：让"数据到底该发到哪个端口"变成确定事实，
    // 而不是每次运行都依赖 Fast DDS 的自动端口分配。
    if (opt.meta_port != 0)
    {
        eprosima::fastdds::rtps::Locator_t loc;
        eprosima::fastdds::rtps::IPLocator::setIPv4(loc, "127.0.0.1");
        loc.port = opt.meta_port;
        pqos.wire_protocol().builtin.metatrafficUnicastLocatorList.clear();
        pqos.wire_protocol().builtin.metatrafficUnicastLocatorList.push_back(loc);
    }
    if (opt.user_port != 0)
    {
        eprosima::fastdds::rtps::Locator_t loc;
        eprosima::fastdds::rtps::IPLocator::setIPv4(loc, "127.0.0.1");
        loc.port = opt.user_port;
        pqos.wire_protocol().default_unicast_locator_list.clear();
        pqos.wire_protocol().default_unicast_locator_list.push_back(loc);
    }

    // 初始对端（单播）：本环境组播不可靠，直接告诉 Fast DDS 往哪里发 SPDP。
    for (const auto& peer : opt.peers)
    {
        const size_t colon = peer.rfind(':');
        const std::string ip = peer.substr(0, colon);
        const uint16_t port = static_cast<uint16_t>(std::atoi(peer.substr(colon + 1).c_str()));
        eprosima::fastdds::rtps::Locator_t loc;
        if (!eprosima::fastdds::rtps::IPLocator::setIPv4(loc, ip))
        {
            note("[diag] FATAL: --peer 地址无法解析: %s", peer.c_str());
            return 2;
        }
        loc.port = port;
        pqos.wire_protocol().builtin.initialPeersList.push_back(loc);
        note("[diag] 初始对端: %s:%u", ip.c_str(), port);
    }

    DomainParticipantFactory* factory = DomainParticipantFactory::get_instance();
    DiagParticipantListener participant_listener;
    const StatusMask participant_mask =
            opt.participant_mask_none ? StatusMask::none() : StatusMask::all();
    note("[diag] participant 事件掩码 = %s",
            opt.participant_mask_none ? "none" : "all");
    DomainParticipant* participant =
            factory->create_participant(opt.domain, pqos, &participant_listener, participant_mask);
    if (participant == nullptr)
    {
        note("[diag] FATAL: create_participant 失败（domain=%u）", opt.domain);
        return 1;
    }

    TypeSupport type(new HelloWorldPubSubType());
    type.register_type(participant);

    SubscriberQos sqos = SUBSCRIBER_QOS_DEFAULT;
    Subscriber* subscriber = participant->create_subscriber(sqos, nullptr, StatusMask::none());
    if (subscriber == nullptr)
    {
        note("[diag] FATAL: create_subscriber 失败");
        return 1;
    }

    TopicQos tqos = TOPIC_QOS_DEFAULT;
    Topic* topic = participant->create_topic(opt.topic, type.get_type_name(), tqos);
    if (topic == nullptr)
    {
        note("[diag] FATAL: create_topic 失败");
        return 1;
    }

    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    subscriber->get_default_datareader_qos(rqos);
    if (opt.set_reliability)
    {
        rqos.reliability().kind = opt.reliability;
    }
    if (opt.set_durability)
    {
        rqos.durability().kind = opt.durability;
    }
    if (opt.set_representation)
    {
        rqos.representation().m_value = opt.representation;
    }

    DiagReaderListener reader_listener;
    DataReader* reader = subscriber->create_datareader(topic, rqos, &reader_listener, StatusMask::all());
    if (reader == nullptr)
    {
        note("[diag] FATAL: create_datareader 失败");
        return 1;
    }

    note("[diag] reader 就绪: type_name=\"%s\" requested_qos: %s",
            type.get_type_name().c_str(), reader_qos_str(opt, rqos).c_str());
    note("[diag] 等待对端 Publication（Fast DDS 内部匹配日志会直接打到 stdout）...");

    for (int t = 1; t <= opt.seconds; ++t)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        note("[diag][t=%ds] participants=%llu writers_discovered=%llu writers_qos_changed=%llu "
             "writers_removed=%llu matched_now=%lld matched_events=%llu samples=%llu "
             "incompatible_qos=%llu liveliness=%llu rejected=%llu",
                t,
                static_cast<unsigned long long>(g_stats.participants_discovered.load()),
                static_cast<unsigned long long>(g_stats.writers_discovered.load()),
                static_cast<unsigned long long>(g_stats.writers_changed_qos.load()),
                static_cast<unsigned long long>(g_stats.writers_removed.load()),
                static_cast<long long>(g_stats.matched_now.load()),
                static_cast<unsigned long long>(g_stats.matched_events.load()),
                static_cast<unsigned long long>(g_stats.samples.load()),
                static_cast<unsigned long long>(g_stats.incompatible_qos_events.load()),
                static_cast<unsigned long long>(g_stats.liveliness_events.load()),
                static_cast<unsigned long long>(g_stats.sample_rejected_events.load()));
    }

    Log::Flush();

    {
        std::vector<double> lat;
        {
            std::lock_guard<std::mutex> lk(g_stats.latency_mutex);
            lat = g_stats.lat_ms;
        }
        // 先排序（LatencyPercentile 内部也会排序，但 printf 参数求值顺序未定义，
        // 不能依赖"某个参数先排序、back() 后取最大值"）。
        std::sort(lat.begin(), lat.end());
        note("DIAG_LATENCY_MS n=%llu min=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f",
                static_cast<unsigned long long>(lat.size()),
                lat.empty() ? -1.0 : lat.front(),
                LatencyPercentile(lat, 0.50),
                LatencyPercentile(lat, 0.95),
                LatencyPercentile(lat, 0.99),
                lat.empty() ? -1.0 : lat.back());
    }
    {
        std::vector<double> gaps;
        {
            std::lock_guard<std::mutex> lk(g_stats.latency_mutex);
            gaps = g_stats.gap_ms;
        }
        std::sort(gaps.begin(), gaps.end());
        note("DIAG_JITTER_MS n=%llu p50=%.3f p95=%.3f p99=%.3f max=%.3f",
                static_cast<unsigned long long>(gaps.size()),
                LatencyPercentile(gaps, 0.50), LatencyPercentile(gaps, 0.95),
                LatencyPercentile(gaps, 0.99),
                gaps.empty() ? -1.0 : gaps.back());
    }
    note("DIAG_SUMMARY seconds=%d domain=%u topic=\"%s\" readers=1 "
         "participants_discovered=%llu participants_removed=%llu "
         "writers_discovered=%llu writers_changed_qos=%llu writers_removed=%llu "
         "matched_events=%llu unmatched_events=%llu matched_now=%lld samples=%llu "
         "data_available_calls=%llu invalid_data_samples=%llu take_error=%llu "
         "incompatible_qos_events=%llu last_incompatible_policy=%lld "
         "liveliness_events=%llu sample_rejected_events=%llu",
            opt.seconds, opt.domain, opt.topic.c_str(),
            static_cast<unsigned long long>(g_stats.participants_discovered.load()),
            static_cast<unsigned long long>(g_stats.participants_removed.load()),
            static_cast<unsigned long long>(g_stats.writers_discovered.load()),
            static_cast<unsigned long long>(g_stats.writers_changed_qos.load()),
            static_cast<unsigned long long>(g_stats.writers_removed.load()),
            static_cast<unsigned long long>(g_stats.matched_events.load()),
            static_cast<unsigned long long>(g_stats.unmatched_events.load()),
            static_cast<long long>(g_stats.matched_now.load()),
            static_cast<unsigned long long>(g_stats.samples.load()),
            static_cast<unsigned long long>(g_stats.data_available_calls.load()),
            static_cast<unsigned long long>(g_stats.invalid_data_samples.load()),
            static_cast<unsigned long long>(g_stats.take_error.load()),
            static_cast<unsigned long long>(g_stats.incompatible_qos_events.load()),
            static_cast<long long>(g_stats.last_incompatible_policy.load()),
            static_cast<unsigned long long>(g_stats.liveliness_events.load()),
            static_cast<unsigned long long>(g_stats.sample_rejected_events.load()));

    participant->delete_contained_entities();
    factory->delete_participant(participant);
    Log::Flush();
    return 0;
}
