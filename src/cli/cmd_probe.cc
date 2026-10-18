// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_probe.cc
//
// `stratum probe` —— 埋点读取器：读共享内存 ring → 按 topic 分派 → 经 DDS 发布。
//
// 需求依据：docs/13-probe-requirements.md
//   PR-10 读取器：读 ring → 聚合 → DDS 发布
//   PR-13 探针 topic 默认加 `_probe.` 前缀，可单独排除
//   PR-06/07/08 在线开关来自配置文件（热重载、双语义）
//   PR-14 失败必有可区分 reason
//
// ---------------------------------------------------------------------------
// 为什么读取器要独立进程，而不是塞进业务
// ---------------------------------------------------------------------------
// 业务进程里只有一块共享内存（见 probe.h），不链接 DDS。读取器是**另一个进程**：
//   · 业务崩了 → 读取器只是没数据了，不死锁
//   · 读取器崩了 → 业务照跑（写 ring 与读取器无关）
//   · 读取器可以**后启动、可重启**，不要求业务重新编译
//
// ---------------------------------------------------------------------------
// 消费规则（环形覆盖下最重要的一条）
// ---------------------------------------------------------------------------
// ring 会被写者覆盖最旧的。读取器若落后太多，那些记录**已经被覆盖**，
// 读到的会是新数据。所以：
//   · 只消费 <= committed_seq 的记录
//   · 若 last_seq < committed - capacity，说明**已经丢了**（落后超过一圈），
//     此时跳到最旧可用位置，并**明确计数**（dropped_behind），不假装没丢。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "cli_common.h"
#include "stratum/probe/probe.h"
#include "stratum/probe/probe_config.h"
#include "stratum/protocol/idds_backend.h"
#include "stratum/protocol/rtps/rtps_backend.h"

using namespace stratum;
using namespace stratum::cli;

namespace stratum {
namespace cli {

namespace {

constexpr const char* kDefaultTopicPrefix = "_probe.";

// 读取器侧的统计。**每条都要能解释**（PR-14 的同一个精神：不笼统报"失败"）
struct ProbeReaderStats {
  uint64_t read_ok = 0;          // 成功读到并尝试发布
  uint64_t published = 0;        // 实际发布成功
  uint64_t skipped_disabled = 0; // 因 topic/全局关闭而跳过
  uint64_t skipped_sampled = 0;  // 因采样跳过
  uint64_t dropped_behind = 0;   // 落后超过一圈，被覆盖
  uint64_t publish_failed = 0;   // DDS 发布**真失败**
  uint64_t buffered_pending = 0; // 尚无匹配读者，数据已缓存待补发（正常状态）
  uint64_t truncated = 0;        // 负载被探针侧截断（写侧 flags bit0）
  // PR-14：失败必须能区分。这三类**不能**合并成一个"失败"计数，
  // 否则排障时无法判断是"格式不对"还是"数据被截断"。
  uint64_t reason_format_mismatch = 0;  // 声明 json 但内容不是
  uint64_t reason_payload_truncated = 0; // 因截断而跳过的条数（当 format 要求完整时）
  uint64_t reason_unknown_probe = 0;     // 配置里没有、且策略要求拒绝
};

// 一个已打开的 ring 的只读视图
struct RingView {
  int fd = -1;
  void* map = nullptr;
  size_t map_size = 0;
  stratum_ring_header* hdr = nullptr;
  std::string path;
};

bool OpenRingReadOnly(const std::string& path, RingView* out, std::string* err) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (err) *err = "无法打开 " + path + "：" + std::strerror(errno);
    return false;
  }
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    if (err) *err = "fstat 失败：" + std::string(std::strerror(errno));
    return false;
  }
  // 先映射头部，读容量，再映射整体（容量由写侧决定）
  const size_t head = sizeof(stratum_ring_header);
  if ((size_t)st.st_size < head) {
    ::close(fd);
    if (err) *err = "共享内存太小（不足一个头部），不是有效的 probe ring";
    return false;
  }
  void* p = ::mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    ::close(fd);
    if (err) *err = "mmap 失败：" + std::string(std::strerror(errno));
    return false;
  }
  auto* hdr = static_cast<stratum_ring_header*>(p);
  if (hdr->magic != STRATUM_RING_MAGIC) {
    ::munmap(p, (size_t)st.st_size);
    ::close(fd);
    if (err) *err = "magic 不符（不是 probe ring，或写侧版本不同）";
    return false;
  }
  if (hdr->version != STRATUM_RING_VERSION) {
    ::munmap(p, (size_t)st.st_size);
    ::close(fd);
    if (err) *err = "版本不符：文件 " + std::to_string(hdr->version) +
                    "，本程序支持 " + std::to_string(STRATUM_RING_VERSION);
    return false;
  }
  if (hdr->record_size != sizeof(stratum_probe_record)) {
    ::munmap(p, (size_t)st.st_size);
    ::close(fd);
    if (err) *err = "记录大小不符：文件 " + std::to_string(hdr->record_size) +
                    "，本程序 " + std::to_string(sizeof(stratum_probe_record));
    return false;
  }
  // 【越界修复】capacity 决定槽位偏移，必须与文件长度自洽。
  //
  // 主循环用 `last_seq % capacity` 定位记录，且只把 committed_seq 当上界。
  // 若头部声称的容量远大于文件实际长度（写侧崩溃留下截断的段、旧版本残留、
  // 或人为构造），槽位偏移会越过 mmap —— 读取器直接 SIGSEGV（已实测：
  // 头部 capacity=1M、文件 4KB、committed=200000 时崩溃）。
  // 这里是读取器唯一能发现"段不完整"的地方，必须在建视图前拒绝。
  {
    const uint64_t need = stratum_ring_bytes(hdr->capacity);
    const uint64_t have = static_cast<uint64_t>(st.st_size);
    if (hdr->capacity == 0 || have < need) {
      const uint32_t declared = hdr->capacity;
      ::munmap(p, (size_t)st.st_size);
      ::close(fd);
      if (err) {
        *err = "共享内存长度不足：头部声明 capacity=" +
               std::to_string(declared) + "（需要 " + std::to_string(need) +
               " 字节），文件只有 " + std::to_string(have) +
               " 字节 —— 段不完整（写侧崩溃或旧版本残留），请让业务重新创建";
      }
      return false;
    }
  }
  out->fd = fd;
  out->map = p;
  out->map_size = (size_t)st.st_size;
  out->hdr = hdr;
  out->path = path;
  return true;
}

void CloseRing(RingView* v) {
  if (v->map != nullptr) ::munmap(v->map, v->map_size);
  if (v->fd >= 0) ::close(v->fd);
  *v = RingView{};
}

// 把探针记录转成 DDS 样本。
//
// 【为什么用探针的 realtime 时间戳做 source_timestamp】
// 这样探针数据与业务 DDS 数据落在**同一条时间轴**上（S10 的诉求），
// `query --state-at` 才能同时看到两者。
protocol::ReceivedSample ToSample(const stratum_probe_record& r,
                        const std::string& topic_prefix) {
  protocol::ReceivedSample s;
  s.topic_name = topic_prefix + std::string(r.topic, r.topic_len);
  s.type_name = "stratum/Probe";   // 固定类型名，便于下游识别
  s.source_timestamp_ns = r.ts_realtime_ns;
  s.reception_timestamp_ns = r.ts_realtime_ns;
  s.sequence = r.seq;
  s.payload.assign(r.payload, r.payload_len);
  return s;
}

// 采样判断：读取器侧也要做（写侧已经做过一次，但配置可能变过）。
// 用序号做确定性采样 —— 同一序号在不同进程/不同时刻的判断一致，便于复现。
// 轻量 JSON 形状检查。
//
// 【为什么不用真解析器】读取器是热路径上的转发器，不该为校验付出完整解析的代价。
// 我们要判的是"明显不像 JSON"，不是"完全符合 RFC 8259"。
// 因此：合法 JSON 不会误判；明显错误（不以 { 或 [ 开头、括号不配对、
// 引号未闭合）能被抓到 —— 这正好覆盖"声明 json 实际发的是二进制"这个场景。
bool LooksLikeJson(const char* data, size_t len) {
  if (len == 0) return false;
  size_t b = 0, e = len;
  while (b < e && (data[b] == ' ' || data[b] == '\t' || data[b] == '\n' ||
                   data[b] == '\r')) ++b;
  while (e > b && (data[e - 1] == ' ' || data[e - 1] == '\t' ||
                   data[e - 1] == '\n' || data[e - 1] == '\r')) --e;
  if (b >= e) return false;
  if (data[b] != '{' && data[b] != '[') return false;
  if (data[e - 1] != '}' && data[e - 1] != ']') return false;

  int depth = 0;
  bool in_str = false;
  bool esc = false;
  for (size_t i = b; i < e; ++i) {
    const char c = data[i];
    if (in_str) {
      if (esc) { esc = false; continue; }
      if (c == '\\') { esc = true; continue; }
      if (c == '"') in_str = false;
      continue;
    }
    if (c == '"') { in_str = true; continue; }
    if (c == '{' || c == '[') ++depth;
    else if (c == '}' || c == ']') {
      --depth;
      if (depth < 0) return false;
    }
  }
  return depth == 0 && !in_str;
}

bool SampleKeep(uint32_t seq, double rate) {
  if (rate >= 1.0) return true;
  if (rate <= 0.0) return false;
  constexpr uint32_t kBuckets = 10000;
  const uint32_t bucket = seq % kBuckets;
  return bucket < static_cast<uint32_t>(rate * kBuckets);
}

}  // namespace

int CmdProbe(const std::vector<std::string>& args, GlobalOptions g) {
  // `stratum probe status ...` 是 probe 的子命令：转给专门的实现。
  // 【为什么放在最前面】--config / --scan-limit 与读取器的选项不同，
  // 早分派可以避免两套选项混在一起解析。
  if (!args.empty() && args[0] == "status") {
    const std::vector<std::string> tail(args.begin() + 1, args.end());
    return CmdProbeStatus(tail, g);
  }

  std::string shm;
  std::string config_path;
  std::string prefix = kDefaultTopicPrefix;
  uint32_t domain = 0;
  std::string participant = "stratum-probe";
  int duration_s = 0;     // 0 = 一直跑
  bool once = false;
  bool dry_run = false;
  bool show_stats = true;
  int wait_seconds = 0;   // >0 时等待 ring 文件出现（常驻读取器常用）

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](void) -> const std::string* {
      if (i + 1 >= args.size()) return nullptr;
      return &args[++i];
    };
    if (a == "--config") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--config 缺少参数值", "", "probe 配置文件路径");
      config_path = *v;
      continue;
    }
    if (a == "--domain") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--domain 缺少参数值", "", "例如: --domain 44");
      domain = static_cast<uint32_t>(std::strtoul(v->c_str(), nullptr, 10));
      continue;
    }
    if (a == "--participant-name") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--participant-name 缺少参数值", "", "");
      participant = *v;
      continue;
    }
    if (a == "--topic-prefix") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--topic-prefix 缺少参数值", "",
                          std::string("默认 ") + kDefaultTopicPrefix);
      prefix = *v;
      continue;
    }
    if (a == "--duration") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--duration 缺少参数值", "", "秒数，0 = 一直跑");
      duration_s = std::atoi(v->c_str());
      continue;
    }
    if (a == "--once") { once = true; continue; }
    if (a == "--dry-run") { dry_run = true; continue; }
    if (a == "--no-stats") { show_stats = false; continue; }
    if (a == "--wait") {
      const std::string* v = next();
      if (!v) return Fail(kUsage, "--wait 缺少参数值", "", "秒数，例如 --wait 30");
      wait_seconds = std::atoi(v->c_str());
      continue;
    }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "probe 不支持该选项: " + a,
                  "支持 --config / --domain / --participant-name / "
                  "--topic-prefix / --duration / --once / --dry-run / --no-stats / --wait",
                  "完整说明见 docs/08-cli-spec.md");
    }
    if (shm.empty()) {
      shm = a;
    } else {
      return Fail(kUsage, "多余的位置参数: " + a, "", "");
    }
  }

  if (shm.empty()) {
    return Fail(kUsage, "probe 需要共享内存路径", "位置参数缺失",
                "例如: stratum probe /dev/shm/stratum_probe.1234");
  }

  // 1) 等待 ring 出现（可选），然后打开（只读）
  //
  // 【为什么要等】常驻读取器常常先于业务启动。第一版直接 open 会在
  // 业务还没创建 shm 时以 ENV 退出 —— 这让"先起读取器再起业务"不可用。
  RingView ring;
  std::string err;
  // 记录"是不是等到了一个新创建的 ring" —— 决定起始位置（见下面 last_seq）
  bool waited_for_new_ring = false;
  if (wait_seconds > 0 && ::access(shm.c_str(), F_OK) != 0) {
    waited_for_new_ring = true;
  }

  // 【关键】--wait 等的是"**可用的** ring"，不是"文件存在"。
  //
  // 业务侧 open(O_CREAT) 之后文件立刻出现，但 magic 要到 memset 之后才写好。
  // 实测（ctest 并行时）会撞进这个窗口，报「magic 不符」。
  // 所以这里循环"尝试打开"，成功才算到。
  if (wait_seconds > 0) {
    if (!g.quiet) {
      std::fprintf(stderr, "  [probe] 等待可用 ring（最多 %d 秒）：%s\n",
                   wait_seconds, shm.c_str());
    }
    const auto wait_start = std::chrono::steady_clock::now();
    while (true) {
      if (OpenRingReadOnly(shm, &ring, &err)) break;
      const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - wait_start)
                              .count();
      if (waited >= wait_seconds) {
        return Fail(kEnv, "等待超时，没有得到可用的 probe ring: " + shm,
                    "已等待 " + std::to_string(wait_seconds) + " 秒；最后一次原因: " + err,
                    "确认业务进程确实调用过探针；若文件已存在但无效，"
                    "可能是旧版本残留，删除后重试");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!g.quiet) std::fprintf(stderr, "  [probe] ring 可用\n");
  } else if (waited_for_new_ring) {
    // 理论上到不了这里；保留分支避免"未使用变量"的误解
    if (!OpenRingReadOnly(shm, &ring, &err)) {
      return Fail(kEnv, "无法读取 probe ring: " + shm, err,
                  "确认业务进程已启动并调过探针；"
                  "文件名形如 /dev/shm/stratum_probe.<pid>");
    }
  } else if (!OpenRingReadOnly(shm, &ring, &err)) {
    return Fail(kEnv, "无法读取 probe ring: " + shm, err,
                "确认业务进程已启动并调过探针（它会在首次写入时创建该文件）；"
                "文件名形如 /dev/shm/stratum_probe.<pid>");
  }

  // 2) 配置（可选）
  probe::ProbeConfig cfg;
  // 【注意】不能用 SessionExists —— 那是查"会话目录"的；
  // 这里要的是配置文件，存在与否由 Load 的分支自行判断（kMissing 也算正常）。
  //
  // PR-16 多实例隔离：显式 --config 优先，其次读 STRATUM_PROBE_CONFIG 环境变量。
  //
  // 【为什么用环境变量而不是"默认路径含 pid"】
  // 业务与读取器是**两个不同进程**，pid 必然不同 ——
  // 默认路径若含 pid，两侧永远读不到同一份配置。
  // 环境变量是唯一能跨进程共享的机制（同一个 shell 里启动即共享）。
  std::string cfg_source;
  if (!config_path.empty()) {
    cfg_source = "--config";
  } else {
    const char* env_cfg = ::getenv("STRATUM_PROBE_CONFIG");
    if (env_cfg != nullptr && env_cfg[0] != '\0') {
      config_path = env_cfg;
      cfg_source = "STRATUM_PROBE_CONFIG";
      if (!g.quiet) {
        std::fprintf(stderr, "  [probe] 使用环境变量中的配置: %s\n",
                     config_path.c_str());
      }
    }
  }
  const bool have_cfg = !config_path.empty();
  if (!have_cfg && !g.quiet) {
    std::fprintf(stderr,
                 "  [提示] 未指定配置（--config 或 STRATUM_PROBE_CONFIG），"
                 "所有 topic 都会发出，不做开关/格式过滤\n");
  }
  if (have_cfg) {
    const probe::LoadReport rep = cfg.Load(config_path);
    if (rep.outcome == probe::LoadOutcome::kParseError) {
      std::fprintf(stderr,
                   "  警告: 配置文件解析失败（%s），已按 fail-safe 语义把全局开关置为关闭；"
                   "其余项保留默认值\n", rep.detail.c_str());
    } else if (rep.outcome == probe::LoadOutcome::kMissing) {
      std::fprintf(stderr,
                   "  警告: 配置文件不存在（%s），本次不按配置过滤（全部 topic 都会发）\n",
                   config_path.c_str());
    }
  }

  // 3) DDS sink
  protocol::DdsBackendPtr backend;
  protocol::SinkPtr sink;
  if (!dry_run) {
    protocol::rtps::RtpsBackendFactory factory;
    backend = factory.Create();
    if (!backend) {
      CloseRing(&ring);
      return Fail(kEnv, "无法创建 DDS 后端", "RtpsBackendFactory 返回空",
                  "本项目应内置 RTPS 后端；若失败请报告");
    }
    protocol::BackendConfig bc;
    bc.domain_id = domain;
    bc.participant_name = participant;
    // topics 留空 = 自动发现模式；但我们是**发布方**，
    // 不需要预声明话题列表（CreateSink 会按需创建 Writer）
    Status s = backend->CreateSink(bc, &sink);
    if (!s.ok() || !sink) {
      CloseRing(&ring);
      return Fail(ExitCodeOf(s), "无法创建 DDS Sink", s.ToString(),
                  "确认域号未被占用、网络配置正常");
    }
    s = sink->Open();
    if (!s.ok()) {
      CloseRing(&ring);
      return Fail(ExitCodeOf(s), "无法打开 DDS Sink", s.ToString(),
                  "用 `stratum doctor` 检查环境");
    }
  }

  // 4) 主循环
  ProbeReaderStats st;
  const uint32_t cap = ring.hdr->capacity;
  // 起始位置的语义（这条曾导致端到端实测读到 0 条）：
  //
  //   · 默认（读取一个**长期运行**的 ring）：从当前 committed 开始，
  //     不重放历史 —— 否则每次重启读取器都会把整个环再发一遍。
  //   · --wait 等到的**新建** ring / --once：从 0 开始。
  //     它的全部历史就是我们要读的东西。
  //
  // 【实测教训】第一版无条件从 committed 开始。`--wait` 等到文件时，
  // 业务已经在 100ms 轮询窗口内写完 40 条，committed=40 → last_seq=40
  // → 读到 0 条。而"读取器先启动"恰是最常见的部署时序。
  uint64_t last_seq = 0;
  if (!once && !waited_for_new_ring) {
    last_seq = __atomic_load_n(&ring.hdr->committed_seq, __ATOMIC_ACQUIRE);
  }

  const auto start = std::chrono::steady_clock::now();
  auto last_cfg_reload = start;
  auto last_report = start;
  bool printed_first = false;

  while (true) {
    const uint64_t committed =
        __atomic_load_n(&ring.hdr->committed_seq, __ATOMIC_ACQUIRE);

    // 落后超过一圈 → 数据已被覆盖，跳到最旧可用并计数
    if (committed > (uint64_t)cap &&
        last_seq < committed - (uint64_t)cap) {
      const uint64_t new_last = committed - (uint64_t)cap;
      st.dropped_behind += new_last - last_seq;
      last_seq = new_last;
    }

    // 消费新记录
    while (last_seq < committed) {
      const uint32_t slot = static_cast<uint32_t>(last_seq % cap);
      const stratum_probe_record* rec = stratum_ring_records_const(ring.hdr) + slot;

      // 二次确认：写者可能正在重写该槽（环形覆盖）。
      // 若记录的 seq 与我们要读的不一致，说明已被覆盖，跳过。
      if (rec->seq != static_cast<uint32_t>(last_seq)) {
        ++st.dropped_behind;
        ++last_seq;
        continue;
      }
      ++st.read_ok;
      if ((rec->flags & 1u) != 0) ++st.truncated;

      const std::string topic(rec->topic, rec->topic_len);
      bool keep = true;
      if (have_cfg) {
        const probe::TopicSettings ts = cfg.Resolve(topic);
        if (!ts.enabled) {
          ++st.skipped_disabled;
          keep = false;
        } else if (!SampleKeep(rec->seq, ts.sample_rate)) {
          ++st.skipped_sampled;
          keep = false;
        } else if (ts.format == "json" &&
                   !LooksLikeJson(rec->payload, rec->payload_len)) {
          // PR-14：声明 json 但内容不像 —— 跳过并**单独计数**。
          //
          // 【为什么不发出去】发了下游会拿到"看起来有数据"的垃圾，
          // 比明确报错危险得多：明确报错会有人去查，静默垃圾不会。
          ++st.reason_format_mismatch;
          if (st.reason_format_mismatch == 1) {
            std::fprintf(stderr,
                         "  [probe] 首条 format_mismatch: topic=%s "
                         "声明 format=json 但内容不像 JSON（payload_len=%u）\n",
                         topic.c_str(), rec->payload_len);
          }
          keep = false;
        }
      }
      // 截断单独计数：它说明"数据本来更长"，与格式错误是两回事
      if (keep && (rec->flags & 1u) != 0) {
        ++st.reason_payload_truncated;
        // 截断的数据仍然发出（业务可能只关心前缀），但要计数让运维知道
      }
      if (keep) {
        if (dry_run) {
          ++st.published;
          if (!printed_first) {
            printed_first = true;
            std::fprintf(stderr, "  [dry-run] 首条：topic=%s%s payload=%.*s\n",
                         prefix.c_str(), topic.c_str(),
                         static_cast<int>(rec->payload_len), rec->payload);
          }
        } else {
          const protocol::ReceivedSample s2 = ToSample(*rec, prefix);
          const Status ps = sink->Publish(s2);
          if (ps.ok()) {
            ++st.published;
          } else if (ps.code() == StatusCode::kNotFound) {
            // 【这不是失败】RTPS 在尚未发现对端 Reader 时会把样本缓存，
            // 待匹配后补发。端到端实测：record 最终收到 40 条，全部靠补发。
            // 把它算成失败会永久显示一批假失败 —— 排障时最误导人。
            ++st.buffered_pending;
          } else {
            if (st.publish_failed == 0) {
              std::fprintf(stderr, "  [probe] 首次发布失败: code=%s detail=%s\n",
                           StatusCodeName(ps.code()), ps.ToString().c_str());
            }
            ++st.publish_failed;
          }
        }
      }
      ++last_seq;
    }

    // 周期性重载配置（PR-06 的在线开关）
    const auto now = std::chrono::steady_clock::now();
    if (have_cfg &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_cfg_reload)
                .count() >= 1000) {
      last_cfg_reload = now;
      (void)cfg.Load(config_path);   // 双语义在 ProbeConfig 内部
    }

    // 退出条件
    if (once) break;
    if (duration_s > 0) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
      if (elapsed >= duration_s) break;
    }

    // 进度（每 5 秒，走 stderr 不污染 stdout）
    if (show_stats && !g.quiet &&
        std::chrono::duration_cast<std::chrono::seconds>(now - last_report).count() >= 5) {
      last_report = now;
      std::fprintf(stderr,
                   "  [probe] 读 %llu / 发 %llu / 跳过(关闭) %llu / 跳过(采样) %llu / "
                   "落后丢弃 %llu / 发布失败 %llu\n",
                   static_cast<unsigned long long>(st.read_ok),
                   static_cast<unsigned long long>(st.published),
                   static_cast<unsigned long long>(st.skipped_disabled),
                   static_cast<unsigned long long>(st.skipped_sampled),
                   static_cast<unsigned long long>(st.dropped_behind),
                   static_cast<unsigned long long>(st.publish_failed));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  // 5) 收尾
  if (sink) {
    (void)sink->Close();
  }
  CloseRing(&ring);

  if (g.format == Format::kJson) {
    std::printf("{\"schema_version\":1,\"shm\":\"%s\",\"domain\":%u,"
                "\"config_source\":\"%s\","
                "\"read_ok\":%llu,\"published\":%llu,\"skipped_disabled\":%llu,"
                "\"skipped_sampled\":%llu,\"dropped_behind\":%llu,"
                "\"publish_failed\":%llu,\"buffered_pending\":%llu,"
                "\"truncated\":%llu,\"reason_format_mismatch\":%llu,"
                "\"reason_payload_truncated\":%llu}\n",
                JsonEscape(shm).c_str(), domain,
                cfg_source.empty() ? "none" : JsonEscape(cfg_source).c_str(),
                static_cast<unsigned long long>(st.read_ok),
                static_cast<unsigned long long>(st.published),
                static_cast<unsigned long long>(st.skipped_disabled),
                static_cast<unsigned long long>(st.skipped_sampled),
                static_cast<unsigned long long>(st.dropped_behind),
                static_cast<unsigned long long>(st.publish_failed),
                static_cast<unsigned long long>(st.buffered_pending),
                static_cast<unsigned long long>(st.truncated),
                static_cast<unsigned long long>(st.reason_format_mismatch),
                static_cast<unsigned long long>(st.reason_payload_truncated));
  } else if (!g.quiet) {
    std::fprintf(stderr, "\n读取结束\n");
    std::fprintf(stderr, "  读入:     %llu 条\n",
                 static_cast<unsigned long long>(st.read_ok));
    std::fprintf(stderr, "  已发布:   %llu 条\n",
                 static_cast<unsigned long long>(st.published));
    if (st.skipped_disabled) {
      std::fprintf(stderr, "  按配置关闭跳过: %llu 条\n",
                   static_cast<unsigned long long>(st.skipped_disabled));
    }
    if (st.skipped_sampled) {
      std::fprintf(stderr, "  按采样跳过:     %llu 条\n",
                   static_cast<unsigned long long>(st.skipped_sampled));
    }
    if (st.dropped_behind) {
      std::fprintf(stderr, "  **落后被覆盖**: %llu 条（读取器跟不上写入）\n",
                   static_cast<unsigned long long>(st.dropped_behind));
    }
    if (st.buffered_pending) {
      std::fprintf(stderr,
                   "  待补发（尚无匹配读者）: %llu 条 —— 这是正常状态，"
                   "对端发现后会自动补发\n",
                   static_cast<unsigned long long>(st.buffered_pending));
    }
    if (st.publish_failed) {
      std::fprintf(stderr, "  发布失败:       %llu 条\n",
                   static_cast<unsigned long long>(st.publish_failed));
    }
    if (st.truncated) {
      std::fprintf(stderr, "  负载被截断:     %llu 条\n",
                   static_cast<unsigned long long>(st.truncated));
    }
    // PR-14：把 reason 分开列出，让排障的人一眼看出是哪一类问题
    if (st.reason_format_mismatch) {
      std::fprintf(stderr,
                   "  格式不匹配跳过: %llu 条（声明 format=json 但内容不是 JSON）\n",
                   static_cast<unsigned long long>(st.reason_format_mismatch));
    }
    if (st.reason_payload_truncated) {
      std::fprintf(stderr,
                   "  已截断仍发送:   %llu 条（原负载超过 %d 字节上限）\n",
                   static_cast<unsigned long long>(st.reason_payload_truncated),
                   STRATUM_PROBE_PAYLOAD_MAX);
    }
  }

  // 有丢弃/失败 → PARTIAL，让脚本能察觉
  // 有任何"数据没能完整送到下游"的情况都是 PARTIAL：
  // 真失败、落后被覆盖、格式不匹配被跳过 —— 三者原因不同，但都意味着
  // 下游看到的数据比业务产生的少。
  if (st.publish_failed > 0 || st.dropped_behind > 0 ||
      st.reason_format_mismatch > 0) {
    return kPartial;
  }
  return kOk;
}

}  // namespace cli
}  // namespace stratum
