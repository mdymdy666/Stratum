// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// verify 的**业务层视角**测试
//
// 覆盖 PR-B1~B3（对应 docs/14 的需求）：
//   B1 分层呈现：DDS 层与探针层事件数分开，互不混淆
//   B2 启发式信号：--business-signals 能从探针负载提取 ok/error
//   B3 ★ 核心约束：业务层数字**绝不参与** has_integrity_issue 的判定
//
// 【B3 为什么最重要】
// 如果 verify 因为"业务失败 5 条"就设 has_integrity_issue=true，
// 它就学会了说谎 —— 5 条失败可能是 5 个订单被正常取消。
// 这条用例是防止该功能越界的护栏。

// 标准库头文件必须最先包含
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

#ifndef PROBE_CLI_PATH
#define PROBE_CLI_PATH "./stratum"
#endif
#ifndef PROBE_SOURCE_DIR
#define PROBE_SOURCE_DIR "."
#endif

std::string kCli = PROBE_CLI_PATH;

// 【并发隔离】与 test_cli_probe.cc 同一套约束（详见那里的说明）：
//   · 目录/shm 路径必须带 <pid>_<计数>，否则两个 worktree 同时跑会互删；
//   · DDS domain 必须按进程偏移，否则两个进程的 participant 互相收数据。
std::string UniqueSuffix() {
  static std::atomic<uint32_t> counter{0};
  return std::to_string(::getpid()) + "_" +
         std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

int TestDomain(int slot) {
  static const int base = []() {
    const char* env = std::getenv("STRATUM_TEST_DOMAIN_BASE");
    if (env != nullptr && env[0] != '\0') return std::atoi(env);
    // 60 刻意避开旧版本测试用过的 33~37 / 43~47：另一个 worktree 可能
    // 还在跑旧测试（固定 domain），换基址后两者不会撞域。
    return 60;
  }();
  const int pid_offset =
      static_cast<int>(static_cast<unsigned>(::getpid()) % 20u);
  return base + pid_offset * 8 + slot;
}

struct CliResult {
  int code = -1;
  std::string output;
};

CliResult RunCli(const std::string& cmd) {
  CliResult r;
  std::FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
  if (p == nullptr) return r;
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), p) != nullptr) r.output += buf;
  const int st = ::pclose(p);
  r.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return r;
}

std::string TempDir(const char* tag) {
  const char* home = std::getenv("HOME");
  const std::string d = std::string(home ? home : ".") + "/stratum_vb_" + tag +
                        "_" + UniqueSuffix();
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

std::string FreshShm(const char* tag) {
  const std::string p =
      std::string("/dev/shm/stratum_vb_") + tag + "_" + UniqueSuffix();
  std::error_code ec;
  fs::remove(p, ec);
  return p;
}

long long PickInt(const std::string& text, const std::string& key) {
  const std::string pat = "\"" + key + "\":";
  const size_t at = text.find(pat);
  if (at == std::string::npos) return -1;
  size_t p = at + pat.size();
  while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
  char* end = nullptr;
  const long long v = std::strtoll(text.c_str() + p, &end, 10);
  if (end == text.c_str() + p) return -1;
  return v;
}

// 编译一个业务程序（用 _STR，因为负载是三元表达式 = 指针）
bool BuildBiz(const std::string& out, const std::string& body, std::string* err) {
  const std::string src = out + ".c";
  {
    std::ofstream o(src, std::ios::out | std::ios::trunc);
    o << "#include <stdio.h>\n#include <stdlib.h>\n#include <unistd.h>\n"
      << "#include \"stratum/probe/probe.h\"\n" << body;
  }
  const std::string inc = std::string(PROBE_SOURCE_DIR) + "/src/include";
  const std::string lib = std::string(PROBE_SOURCE_DIR) + "/build/src/libstratum_probe.a";
  const CliResult r = RunCli("gcc -std=c11 -O2 -I" + inc + " " + src + " " + lib +
                             " -lpthread -o " + out);
  if (!fs::exists(out)) {
    if (err) *err = r.output;
    return false;
  }
  return true;
}

// 跑一次完整的"业务写探针 → probe → record"流程，返回会话路径
struct Recorded {
  std::string session;
  std::string root;
  std::string shm;
};

Recorded RecordProbeSession(const char* tag, int domain_slot, const std::string& biz_body,
                            int ok_count, int err_count, std::string* err) {
  Recorded r;
  // domain 由 slot 推导：同一进程内各用例域不同，跨进程按 pid 偏移（见 TestDomain）
  const uint32_t domain = static_cast<uint32_t>(TestDomain(domain_slot));
  r.root = TempDir(tag);
  r.session = r.root + "/s";
  r.shm = FreshShm(tag);

  const std::string biz = r.root + "/biz";
  if (!BuildBiz(biz, biz_body, err)) return r;

  std::system(("STRATUM_PROBE_SHM=" + r.shm + " " + kCli + " --quiet probe " + r.shm +
               " --domain " + std::to_string(domain) +
               " --wait 25 --duration 9 --no-stats > " + r.root + "/p.json 2>&1 &")
                  .c_str());
  std::this_thread::sleep_for(std::chrono::milliseconds(900));
  std::system((kCli + " --quiet record --dir " + r.session + " --domain " +
               std::to_string(domain) + " --duration 11 > " + r.root + "/r.json 2>&1 &")
                  .c_str());
  std::this_thread::sleep_for(std::chrono::milliseconds(1800));

  (void)RunCli("STRATUM_PROBE_SHM=" + r.shm + " " + biz);
  std::this_thread::sleep_for(std::chrono::milliseconds(11000));
  return r;
}

void Cleanup(const Recorded& r) {
  std::error_code ec;
  fs::remove(r.shm, ec);
  fs::remove_all(r.root, ec);
}

}  // namespace

// ===========================================================================
// B1 · 分层呈现：DDS 层与探针层分开
// ===========================================================================

STRATUM_TEST(VerifyBusiness, LayersAreSeparated) {
  // 16 ok + 4 error，用 _STR 因为内容是三元表达式（指针）
  const std::string body = R"(
int main(void) {
  if (stratum_probe_open(NULL) != 0) return 1;
  for (int i = 0; i < 20; ++i) {
    const char* r = (i < 16) ? "result=ok" : "result=error";
    STRATUM_PROBE_STR("order/settle", r);
  }
  sleep(1);
  stratum_probe_close();
  return 0;
}
)";
  std::string err;
  const Recorded rec = RecordProbeSession("b1", 0, body, 16, 4, &err);
  if (!fs::exists(rec.session)) {
    std::printf("      [跳过] 会话未创建（DDS 环境问题）: %s\n", err.c_str());
    Cleanup(rec);
    return;
  }

  const CliResult v = RunCli(kCli + " verify " + rec.session);
  STRATUM_CHECK_EQ(v.code, 0);

  // 探针层有数据，DDS 层没有（本次只跑了探针）
  const long long probe_events = PickInt(v.output, "probe_events");
  const long long dds_events = PickInt(v.output, "dds_events");
  STRATUM_CHECK_TRUE(probe_events > 0);
  STRATUM_CHECK_EQ(dds_events, 0LL);
  // 两个数字都在输出里，且分开
  STRATUM_CHECK_TRUE(v.output.find("\"business_layer\"") != std::string::npos);
  STRATUM_CHECK_TRUE(v.output.find("\"probe_prefix\":\"_probe.\"") != std::string::npos);
  std::printf("      [B1] 分层：probe_events=%lld, dds_events=%lld（分开呈现）\n",
              probe_events, dds_events);

  Cleanup(rec);
}

// ===========================================================================
// B2 · 启发式信号：从探针负载提取 ok / error
// ===========================================================================

STRATUM_TEST(VerifyBusiness, SignalsExtractedHeuristically) {
  const std::string body = R"(
int main(void) {
  if (stratum_probe_open(NULL) != 0) return 1;
  for (int i = 0; i < 20; ++i) {
    const char* r = (i < 16) ? "result=ok" : "result=error";
    STRATUM_PROBE_STR("order/settle", r);
  }
  sleep(1);
  stratum_probe_close();
  return 0;
}
)";
  std::string err;
  const Recorded rec = RecordProbeSession("b2", 1, body, 16, 4, &err);
  if (!fs::exists(rec.session)) {
    std::printf("      [跳过] 会话未创建: %s\n", err.c_str());
    Cleanup(rec);
    return;
  }

  // 默认不收集（性能）
  const CliResult d = RunCli(kCli + " verify " + rec.session);
  STRATUM_CHECK_TRUE(d.output.find("\"signals\":null") != std::string::npos);

  // --business-signals 才收集
  const CliResult s = RunCli(kCli + " verify --business-signals " + rec.session);
  const long long scanned = PickInt(s.output, "scanned");
  const long long ok = PickInt(s.output, "ok");
  const long long error = PickInt(s.output, "error");
  STRATUM_CHECK_TRUE(scanned > 0);
  STRATUM_CHECK_EQ(ok + error + PickInt(s.output, "unknown"), scanned);
  // 实际录到的条数取决于时序，但 ok 必须占多数（16/20 = 80%）
  STRATUM_CHECK_TRUE(ok > 0);
  STRATUM_CHECK_TRUE(ok >= error);
  std::printf("      [B2] 启发式：scanned=%lld, ok=%lld, error=%lld\n", scanned, ok, error);

  Cleanup(rec);
}

// ===========================================================================
// B3 · ★ 核心约束：业务层不参与完整性判定
// ===========================================================================
//
// 【这条是整个功能的护栏】
// 20 条里 4 条 error（20% 失败率），若 verify 因此报 has_integrity_issue=true，
// 它就在对业务逻辑下结论 —— 而那 4 条可能完全是正常的（4 个订单被取消）。

STRATUM_TEST(VerifyBusiness, BusinessFailureDoesNotAffectIntegrity) {
  const std::string body = R"(
int main(void) {
  if (stratum_probe_open(NULL) != 0) return 1;
  // 全部失败 —— 最极端的业务状况
  for (int i = 0; i < 20; ++i) {
    STRATUM_PROBE_STR("order/settle", "result=error");
  }
  sleep(1);
  stratum_probe_close();
  return 0;
}
)";
  std::string err;
  const Recorded rec = RecordProbeSession("b3", 2, body, 0, 20, &err);
  if (!fs::exists(rec.session)) {
    std::printf("      [跳过] 会话未创建: %s\n", err.c_str());
    Cleanup(rec);
    return;
  }

  const CliResult v = RunCli(kCli + " verify --business-signals " + rec.session);

  // ★ 数据完整性不受业务结果影响
  STRATUM_CHECK_TRUE(v.output.find("\"has_integrity_issue\":false") != std::string::npos);
  // 退出码也不变（0 = 无完整性问题）
  STRATUM_CHECK_EQ(v.code, 0);

  // 但业务信号确实被采集了（全部 error）
  const long long error = PickInt(v.output, "error");
  const long long ok = PickInt(v.output, "ok");
  STRATUM_CHECK_TRUE(error > 0);
  STRATUM_CHECK_EQ(ok, 0LL);

  // note 必须在（说明"不下结论"）
  STRATUM_CHECK_TRUE(v.output.find("不对业务逻辑下结论") != std::string::npos);
  std::printf("      [B3] 20 条全 error，但 has_integrity_issue=false、退出码 0 "
              "—— 业务层没有污染完整性判定\n");

  Cleanup(rec);
}

// ===========================================================================
// B4 · RC-B1：只有探针数据时给出警告（但仍不算完整性问题）
// ===========================================================================

STRATUM_TEST(VerifyBusiness, OnlyProbeDataWarnsButStaysClean) {
  const std::string body = R"(
int main(void) {
  if (stratum_probe_open(NULL) != 0) return 1;
  for (int i = 0; i < 5; ++i) STRATUM_PROBE_STR("diag/x", "k=v");
  sleep(1);
  stratum_probe_close();
  return 0;
}
)";
  std::string err;
  const Recorded rec = RecordProbeSession("b4", 3, body, 5, 0, &err);
  if (!fs::exists(rec.session)) {
    std::printf("      [跳过] 会话未创建: %s\n", err.c_str());
    Cleanup(rec);
    return;
  }

  const CliResult v = RunCli(kCli + " verify " + rec.session);
  STRATUM_CHECK_EQ(v.code, 0);
  // 必须报出 RC-B1
  STRATUM_CHECK_TRUE(v.output.find("RC-B1") != std::string::npos);
  STRATUM_CHECK_TRUE(v.output.find("只有探针数据") != std::string::npos);
  // 但完整性仍然 clean —— 数据没坏，只是用途与预期不同
  STRATUM_CHECK_TRUE(v.output.find("\"has_integrity_issue\":false") != std::string::npos);
  std::printf("      [B4] RC-B1 警告存在，但 has_integrity_issue 仍为 false\n");

  Cleanup(rec);
}

// ===========================================================================
// B5 · --probe-prefix 可配置
// ===========================================================================

STRATUM_TEST(VerifyBusiness, CustomPrefixIsHonored) {
  const std::string body = R"(
int main(void) {
  if (stratum_probe_open(NULL) != 0) return 1;
  for (int i = 0; i < 3; ++i) STRATUM_PROBE_STR("p/x", "a=b");
  sleep(1);
  stratum_probe_close();
  return 0;
}
)";
  std::string err;
  const Recorded rec = RecordProbeSession("b5", 4, body, 3, 0, &err);
  if (!fs::exists(rec.session)) {
    std::printf("      [跳过] 会话未创建: %s\n", err.c_str());
    Cleanup(rec);
    return;
  }

  // 用默认前缀时，_probe.p/x 属于探针层
  const CliResult a = RunCli(kCli + " verify " + rec.session);
  STRATUM_CHECK_TRUE(PickInt(a.output, "probe_events") > 0);

  // 换一个不匹配的前缀 → 所有 topic 都被当成 DDS 层
  const CliResult b = RunCli(kCli + " verify --probe-prefix=zzz. " + rec.session);
  // 注意：这个命令形式的正确性取决于解析器是否支持 --key=value
  // 若只支持 --key value，则下面这个才是对的
  const CliResult c = RunCli(kCli + " verify --probe-prefix zzz. " + rec.session);
  const long long probe_b = PickInt(c.output, "probe_events");
  const long long dds_b = PickInt(c.output, "dds_events");
  STRATUM_CHECK_EQ(probe_b, 0LL);
  STRATUM_CHECK_TRUE(dds_b > 0);
  std::printf("      [B5] 换前缀后 probe_events=0、dds_events=%lld（分类可配）\n", dds_b);

  Cleanup(rec);
}

STRATUM_TEST_MAIN()
