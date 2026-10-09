// ---------------------------------------------------------------------------
// 文件：tools/fastdds_sub_diag/make_timeline.cc
// 用途：生成一份"内容已知"的时间线，用于端到端验收。
//
// 为什么需要它：
//   Stratum 的**录制方向**（Fast DDS → Stratum）目前仍未打通
//   （历史上也没通过，属于另一个方向的工作），因此无法用"真实录制"
//   产出验收数据；而 /tmp 会被系统清理，早前录好的时间线已丢失。
//   本工具用 TimelineImplV2 的公开 API 直接写入标准 HelloWorld CDR 负载，
//   让"Stratum 回放 → Fast DDS 接收"这条已打通的方向有一个**可重复、
//   自包含**的验收数据源。
//
// 用法：make_timeline <目录> [条数]
// 产物：能在 stratum_rtps_replay 里回放的 hello_world_topic 时间线
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"

using namespace stratum;

namespace {

// HelloWorld { uint32 index; string message; } 的 XCDR1(PL_CDR_LE) 负载
std::string MakeHelloWorldPayload(uint32_t index, const std::string& message) {
    std::string payload;
    payload.push_back('\x00');   // encapsulation identifier
    payload.push_back('\x01');   // CDR_LE
    payload.push_back('\x00');
    payload.push_back('\x00');
    payload.append(reinterpret_cast<const char*>(&index), 4);
    const uint32_t len = static_cast<uint32_t>(message.size() + 1);   // 含 NUL
    payload.append(reinterpret_cast<const char*>(&len), 4);
    payload.append(message);
    payload.push_back('\0');
    while ((payload.size() % 4) != 0) {
        payload.push_back('\0');
    }
    return payload;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tl_gen";
    const int count = (argc > 2) ? std::atoi(argv[2]) : 32;
    // 可选：payload 字节数（默认用 "Hello world"）。
    // 用于验证大 payload 路径：>1500 触发 IP 分片，接近/超过 64KB
    // 会触及 RTPS 的 DATA_FRAG 边界（本实现未支持分片，应如实失败）。
    const size_t payload_bytes =
        (argc > 3) ? static_cast<size_t>(std::atoi(argv[3])) : 0;
    // 可选：**第二话题**的条数（多话题并发验证用）。
    // 话题名 hello_world_topic_2，类型仍是 HelloWorld；
    // payload 的 index 从 1000 起，便于在对账时识别"是否串扰"。
    const int topic2_count = (argc > 4) ? std::atoi(argv[4]) : 0;

    TimelineImplV2 timeline;
    Status s = timeline.Open(dir);
    if (!s.ok()) {
        std::printf("FAILED: Open(%s): %s\n", dir.c_str(), s.ToString().c_str());
        return 1;
    }

    TopicMetadata meta;
    meta.topic_name = "hello_world_topic";
    meta.type_name = "HelloWorld";
    uint32_t topic_id = 0;
    s = timeline.RegisterTopic(meta, &topic_id);
    if (!s.ok()) {
        std::printf("FAILED: RegisterTopic: %s\n", s.ToString().c_str());
        return 1;
    }

    // 每条样本的间隔。用 20ms 而不是 100ms：端到端延迟只能在
    // **原速回放**（rate=1.0）下才有意义，间隔越小、原速回放越快，
    // 压测才能在可接受时间内给出可信的延迟分位。
    constexpr TimestampNs kIntervalNs = 20000000ULL;   // 20ms

    // 基准时间取**当前时刻往前推 count 条**（每条间隔 kIntervalNs）。
    //
    // 曾经用固定历史基准（2023-11-14）：回放时写入 INFO_TS 的源时间戳
    // 与接收端时钟差近 3 年，端到端延迟测出来是 9.1e10 ms（荒谬值）。
    // 时间戳必须落在真实时间轴上，延迟指标才有意义。
    const TimestampNs kBase =
        static_cast<TimestampNs>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count())
        - static_cast<TimestampNs>(count) * kIntervalNs;
    std::vector<Event> events;
    events.reserve(static_cast<size_t>(count));
    for (int i = 1; i <= count; ++i) {
        Event e;
        e.topic_id = topic_id;
        e.seq = static_cast<uint32_t>(i);
        e.source_timestamp_ns = kBase + static_cast<TimestampNs>(i) * kIntervalNs;
        e.reception_timestamp_ns = e.source_timestamp_ns + 1000;
        std::string msg = "Hello world";
        if (payload_bytes > 0) {
            msg.assign(payload_bytes, 'x');
        }
        e.payload = MakeHelloWorldPayload(static_cast<uint32_t>(i), msg);
        events.push_back(std::move(e));
    }

    s = timeline.AppendBatch(events);
    if (!s.ok()) {
        std::printf("FAILED: AppendBatch: %s\n", s.ToString().c_str());
        return 1;
    }
    if (topic2_count > 0) {
        TopicMetadata meta2;
        meta2.topic_name = "hello_world_topic_2";
        meta2.type_name = "HelloWorld";
        uint32_t tid2 = 0;
        s = timeline.RegisterTopic(meta2, &tid2);
        if (!s.ok()) {
            std::printf("FAILED: RegisterTopic(topic2): %s\n", s.ToString().c_str());
            return 1;
        }
        std::vector<Event> ev2;
        ev2.reserve(static_cast<size_t>(topic2_count));
        for (int i = 1; i <= topic2_count; ++i) {
            Event e;
            e.topic_id = tid2;
            e.seq = static_cast<uint32_t>(i);
            e.source_timestamp_ns = kBase + static_cast<TimestampNs>(i) * kIntervalNs;
            e.reception_timestamp_ns = e.source_timestamp_ns + 1000;
            std::string msg = "Hello world";
            if (payload_bytes > 0) {
                msg.assign(payload_bytes, 'x');
            }
            e.payload = MakeHelloWorldPayload(static_cast<uint32_t>(1000 + i), msg);
            ev2.push_back(std::move(e));
        }
        s = timeline.AppendBatch(ev2);
        if (!s.ok()) {
            std::printf("FAILED: AppendBatch(topic2): %s\n", s.ToString().c_str());
            return 1;
        }
    }

    s = timeline.Close();
    if (!s.ok()) {
        std::printf("FAILED: Close: %s\n", s.ToString().c_str());
        return 1;
    }

    std::printf("已生成 %d 条 hello_world_topic + %d 条 hello_world_topic_2（payload %s）→ %s\n",
                count, topic2_count,
                payload_bytes > 0 ? (std::to_string(payload_bytes) + " 字节").c_str()
                                  : "Hello world",
                dir.c_str());
    return 0;
}
