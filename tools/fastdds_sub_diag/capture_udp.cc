// ---------------------------------------------------------------------------
// 文件：tools/fastdds_sub_diag/capture_udp.cc
// 用途：以"最小 RTPS 参与者"的身份接收 Stratum 发出的报文，**原样落盘**，
//       用来回答一个不能靠推理解决的问题：
//         Stratum 到底有没有把 SEDP Publication 发给对端？
//
// 为什么不能只用 Stratum 自己的 sedp_recv：
//   sedp_recv 用 Stratum 自己的解析器解读报文，一旦解析器与"发送端"
//   共享同样的错误假设，就会一起错（本项目已有此类教训：把已验证逻辑
//   重新实现一遍会引入难以察觉的偏差）。本程序**不做任何 RTPS 语义解析**，
//   只做两件事：
//     1. 周期发送 SPDP 公告，声明 metatraffic locator = 本程序绑定端口，
//        让 Stratum 把本程序当成一个 peer 并把报文发过来；
//     2. 把收到的每个数据报按 [uint32 LE 长度][原始字节] 追加到文件。
//   解析留给后续步骤（可以用 Fast DDS 自己的解析器对拍）。
//
// 用法：
//   capture_udp [seconds] [domain] [port] [out_file] [prefix_tail]
//     seconds      监听秒数（默认 30）
//     domain       域号（默认 0）
//     port         metatraffic 端口（默认 9090）
//     out_file     落盘文件（默认 /tmp/capture_udp.bin）
//     prefix_tail  GUID 前缀最后一字节（默认 0x09，用于与 Stratum 自身区分）
// ---------------------------------------------------------------------------

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <stratum/rtps/builtin/spdp_announcer.h>

using namespace stratum::rtps;

namespace {

std::string EntityName(
        const uint8_t* e)
{
    const uint32_t v = (static_cast<uint32_t>(e[0]) << 24) |
                       (static_cast<uint32_t>(e[1]) << 16) |
                       (static_cast<uint32_t>(e[2]) << 8) | e[3];
    switch (v)
    {
        case 0x000100c2: return "SPDP_W";
        case 0x000100c7: return "SPDP_R";
        case 0x000003c2: return "SEDP_PUB_W";
        case 0x000003c7: return "SEDP_PUB_R";
        case 0x000004c2: return "SEDP_SUB_W";
        case 0x000004c7: return "SEDP_SUB_R";
        case 0x000002c2: return "SEDP_TOPIC_W";
        case 0x000002c7: return "SEDP_TOPIC_R";
        case 0x00000103: return "USER_W";
        case 0x00000104: return "USER_R";
        default:
        {
            char b[24];
            std::snprintf(b, sizeof(b), "0x%08x", v);
            return b;
        }
    }
}

// 只做子消息边界遍历（不做语义解析），打印每个子消息 id 与长度；
// 对 DATA 子消息额外打印 reader / writer entity，便于一眼看出是不是 SEDP。
std::string Summarize(
        const uint8_t* buf,
        size_t n)
{
    std::string out;
    if (n < 20 || std::memcmp(buf, "RTPS", 4) != 0)
    {
        return "not-rtps";
    }
    const uint8_t ver_minor = buf[5];
    const size_t payload_offset = (ver_minor >= 3) ? 2 : 0;
    size_t p = 20;
    while (p + 4 <= n)
    {
        const uint8_t sid = buf[p];
        const uint16_t olen = static_cast<uint16_t>(buf[p + 2]) |
                              (static_cast<uint16_t>(buf[p + 3]) << 8);
        char item[48];
        std::snprintf(item, sizeof(item), "0x%02x(%u)", sid, olen);
        if (!out.empty())
        {
            out += ",";
        }
        out += item;
        if (sid == 0x15)
        {
            const size_t ids = p + 4 + payload_offset + 2;
            if (ids + 8 <= n)
            {
                out += " reader=" + EntityName(buf + ids);
                out += " writer=" + EntityName(buf + ids + 4);
            }
        }
        if (olen == 0)
        {
            break;
        }
        p += 4 + olen;
    }
    return out;
}

}  // namespace

int main(
        int argc,
        char** argv)
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 30;
    const uint32_t domain = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;
    const uint16_t port = (argc > 3) ? static_cast<uint16_t>(std::atoi(argv[3])) : 9090;
    const std::string out_path = (argc > 4) ? argv[4] : "/tmp/capture_udp.bin";
    const int prefix_tail = (argc > 5) ? std::atoi(argv[5]) : 0x09;

    const int rx = ::socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    ::setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(rx, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0)
    {
        std::printf("FAILED: bind %u\n", port);
        return 1;
    }
    timeval tv{};
    tv.tv_usec = 100000;   // 100ms：与 SPDP 周期同量级
    ::setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    SpdpAnnounceConfig cfg;
    const uint8_t prefix[12] = {0x01, 0x0f, 0x53, 0x54, 0x52, 0x41,
                                0x54, 0x55, 0x4d, 0x00, 0x00,
                                static_cast<uint8_t>(prefix_tail)};
    std::memcpy(cfg.guid_prefix.value, prefix, 12);
    cfg.domain_id = domain;
    cfg.builtin_endpoints = 0x000f0c3f;
    cfg.participant_name = "stratum_capture";
    cfg.metatraffic_unicast.push_back(MakeDefaultUdpv4Locator("127.0.0.1", port));
    cfg.default_unicast.push_back(
        MakeDefaultUdpv4Locator("127.0.0.1", static_cast<uint16_t>(port + 1)));

    SpdpAnnouncer announcer;
    std::string err;
    if (!announcer.Open(cfg, &err))
    {
        std::printf("FAILED: announcer.Open: %s\n", err.c_str());
        return 1;
    }

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        std::printf("FAILED: 无法写 %s\n", out_path.c_str());
        return 1;
    }

    std::printf("capture_udp: domain=%u port=%u out=%s prefix_tail=%d 持续 %ds\n",
                domain, port, out_path.c_str(), prefix_tail, seconds);
    std::fflush(stdout);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    size_t rx_total = 0;
    int printed = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        announcer.AnnounceOnce(nullptr);

        uint8_t buf[65536];
        const ssize_t n = ::recv(rx, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            continue;
        }
        ++rx_total;
        const uint32_t len = static_cast<uint32_t>(n);
        out.write(reinterpret_cast<const char*>(&len), sizeof(len));
        out.write(reinterpret_cast<const char*>(buf), n);

        if (printed < 40)
        {
            ++printed;
            std::printf("[rx %zu] n=%zd %s\n", rx_total, n, Summarize(buf, n).c_str());
            std::fflush(stdout);
        }
    }
    out.flush();
    out.close();
    ::close(rx);

    std::printf("capture_udp: 收到 %zu 个报文，已写入 %s\n", rx_total, out_path.c_str());
    return 0;
}
