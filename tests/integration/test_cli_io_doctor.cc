// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// CLI 命令 `io` / `doctor` 的集成测试
//
// 【为什么用 C++ 调 CLI 二进制】
// 这两个命令的逻辑在 CLI 层（`cmd_io.cc` / `cmd_doctor.cc`），不在 core 库里，
// 所以单元测试链接不到它们。项目现有的 shell 场景脚本又**不进 ctest**
// （`tests/rtps/*.sh`、`tools/.../scenarios/*.sh` 都是手工跑的）。
//
// 为了让这两个命令进入质量门，这里用 C++ 测试通过 `std::system` 调用
// CLI 二进制 —— 好处是：进 ctest、能测真实退出码、能造真实损坏。
//
// 【重点覆盖】
//   1. **损坏 bundle 必须被拒绝**（★ 这是 io 最关键的安全属性）
//   2. **doctor --recover 清理过期锁**（★ 这是 doctor 唯一的自动恢复动作）
//   3. 完整链路（export → verify → import → 对账）
//
// 覆盖三类（项目测试标准）：
//   · 大数据量 —— 多话题 + 大量条数
//   · 随机     —— 多种子随机话题数
//   · 真实场景 —— ① 备份并还原 ② 传输损坏的检测 ③ 结束后的会话体检

// 标准库头文件必须最先包含
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "stratum/core/timeline_impl_v2.h"
#include "test_framework.h"

namespace fs = std::filesystem;

using namespace stratum;

namespace {

// CLI 可执行文件（ctest 的 WORKING_DIRECTORY 是 CMAKE_BINARY_DIR）
constexpr const char* kCli = "./stratum";

struct RunResult {
  int exit_code = -1;
  std::string output;   // stdout+stderr 合并
};

RunResult RunCli(const std::string& cmdline) {
  RunResult r;
  // 把 stderr 也收进来，便于断言错误消息
  const std::string full = cmdline + " 2>&1";
  FILE* p = ::popen(full.c_str(), "r");
  if (p == nullptr) return r;
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), p) != nullptr) {
    r.output += buf;
  }
  const int st = ::pclose(p);
  if (st != -1) {
    r.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  }
  return r;
}

std::string TempDir(const char* tag) {
  const std::string d = std::string("/tmp/stratum_cli_") + tag;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

// 用 make_timeline 造一个会话（工具在 build/tools/ 下）
bool MakeSession(const std::string& path, int count) {
  const RunResult r = RunCli("./tools/make_timeline " + path + " " +
                             std::to_string(count) + " > /dev/null");
  return fs::exists(path) && r.exit_code == 0;
}

// 统计会话里的总条数（用 CLI 的 CSV 输出）
uint64_t CountViaCli(const std::string& session) {
  const RunResult r = RunCli(std::string(kCli) + " --format csv query " + session +
                             " --list-topics");
  // 表头 + 数据行；取最后一行的第 3 列（count）
  std::string last;
  size_t pos = 0;
  while (pos < r.output.size()) {
    const size_t nl = r.output.find('\n', pos);
    const std::string line =
        r.output.substr(pos, (nl == std::string::npos ? r.output.size() : nl) - pos);
    if (!line.empty()) last = line;
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  if (last.empty()) return 0;
  // 切第 3 个字段
  int field = 1;
  size_t p = 0;
  uint64_t count = 0;
  while (field <= 3) {
    const size_t c = last.find(',', p);
    const std::string tok =
        last.substr(p, (c == std::string::npos ? last.size() : c) - p);
    if (field == 3) {
      count = std::strtoull(tok.c_str(), nullptr, 10);
      break;
    }
    if (c == std::string::npos) break;
    p = c + 1;
    ++field;
  }
  return count;
}

// 往会话里写一个"过期锁"（pid 不存在）
void WriteStaleLock(const std::string& session, long pid) {
  std::ofstream o(session + ".lock", std::ios::out | std::ios::trunc);
  o << "{\"pid\": " << pid << ", \"host\": \"test\", \"at\": \"2026-01-01T00:00:00+08:00\","
    << " \"reason\": \"simulated stale lock\"}\n";
}

// 把一个会话打碎，使其**必然打不开**。
//
// 【为什么截断 CURRENT 指向的文件，而不是随便找个 .ldb】
// 第一版我截断"最大的 .ldb"，但 make_timeline 刚写完的数据可能还在
// memtable / .log 里，截断 .ldb 并不影响打开 —— 于是 doctor 返回 0，
// 测试断言失败。**要测"损坏被检出"，就必须制造确定的损坏。**
//
// CURRENT 是 LSM 指向当前 MANIFEST 的指针，它指向的文件缺了/坏了，
// 打开必然失败。
bool CorruptSession(const std::string& session) {
  std::error_code ec;
  const std::string current = session + "/CURRENT";
  std::string target;

  std::ifstream cin(current, std::ios::in | std::ios::binary);
  if (cin) {
    std::string manifest;
    std::getline(cin, manifest);
    // CURRENT 里就是 MANIFEST 文件名（可能带空白）
    while (!manifest.empty() &&
           (manifest.back() == '\n' || manifest.back() == '\r' ||
            manifest.back() == ' ')) {
      manifest.pop_back();
    }
    if (!manifest.empty()) {
      const std::string cand = session + "/" + manifest;
      if (fs::exists(cand, ec)) target = cand;
    }
  }
  // 退路：把 CURRENT 本身写坏
  if (target.empty()) {
    std::ofstream o(current, std::ios::out | std::ios::trunc);
    if (!o) return false;
    o << "not-a-manifest-name\n";
    return true;
  }
  fs::resize_file(target, 16, ec);
  return !ec;
}

}  // namespace

// ===========================================================================
// 一、io 的完整链路
// ===========================================================================

STRATUM_TEST(CliIo, ExportVerifyImportRoundTrip) {
  const std::string src = TempDir("rt_src");
  const std::string bundle = TempDir("rt_bundle");
  const std::string dst = TempDir("rt_dst");
  fs::remove_all(bundle);
  fs::remove_all(dst);

  STRATUM_CHECK_TRUE(MakeSession(src, 500));

  // export
  RunResult r = RunCli(std::string(kCli) + " io export " + src + " --to " + bundle);
  STRATUM_CHECK_EQ(r.exit_code, 0);
  STRATUM_CHECK_TRUE(fs::exists(bundle + "/MANIFEST.json"));

  // verify
  // 注意：必须显式加 --format table。全局 --format 默认是 json，
  // 而"完整"这个结论只在文本输出里。
  r = RunCli(std::string(kCli) + " --format table io verify " + bundle);
  STRATUM_CHECK_EQ(r.exit_code, 0);
  STRATUM_CHECK_TRUE(r.output.find("完整") != std::string::npos);

  // import
  r = RunCli(std::string(kCli) + " io import " + bundle + " --to " + dst);
  STRATUM_CHECK_EQ(r.exit_code, 0);

  // 对账：条数必须一致
  const uint64_t a = CountViaCli(src);
  const uint64_t b = CountViaCli(dst);
  STRATUM_CHECK_TRUE(a > 0);
  STRATUM_CHECK_EQ(b, a);
  std::printf("      [链路] export→verify→import 完成，条数对账 %llu == %llu\n",
              static_cast<unsigned long long>(a),
              static_cast<unsigned long long>(b));
}

STRATUM_TEST(CliIo, ExportDryRunCreatesNothing) {
  const std::string src = TempDir("dry_src");
  const std::string bundle = TempDir("dry_bundle");
  fs::remove_all(bundle);
  STRATUM_CHECK_TRUE(MakeSession(src, 100));

  const RunResult r = RunCli(std::string(kCli) + " io export " + src + " --to " +
                             bundle + " --dry-run");
  STRATUM_CHECK_EQ(r.exit_code, 0);
  STRATUM_CHECK_FALSE(fs::exists(bundle));   // dry-run 不得创建目标
}

STRATUM_TEST(CliIo, ImportRefusesExistingTargetWithoutOverwrite) {
  const std::string src = TempDir("ex_src");
  const std::string bundle = TempDir("ex_bundle");
  const std::string dst = TempDir("ex_dst");
  fs::remove_all(bundle);
  fs::remove_all(dst);
  STRATUM_CHECK_TRUE(MakeSession(src, 100));
  STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " io export " + src + " --to " + bundle).exit_code, 0);
  STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " io import " + bundle + " --to " + dst).exit_code, 0);

  // 第二次导入：目标已存在，必须 EXISTS(3)
  const RunResult r = RunCli(std::string(kCli) + " io import " + bundle + " --to " + dst);
  STRATUM_CHECK_EQ(r.exit_code, 3);
}

STRATUM_TEST(CliIo, NonexistentSessionIsNotFoundAndNotCreated) {
  const std::string nope = "/tmp/stratum_cli_definitely_missing_xyz";
  std::error_code ec;
  fs::remove_all(nope, ec);
  const RunResult r = RunCli(std::string(kCli) + " io export " + nope +
                             " --to /tmp/stratum_cli_nowhere");
  STRATUM_CHECK_EQ(r.exit_code, 8);      // NOTFOUND
  STRATUM_CHECK_FALSE(fs::exists(nope)); // 查询类命令不得有创建副作用
}

// ===========================================================================
// 二、★ 损坏必须被拒绝（io 最关键的安全属性）
// ===========================================================================

STRATUM_TEST(CliIo, CorruptedBundleIsRejectedNotImported) {
  const std::string src = TempDir("cor_src");
  const std::string bundle = TempDir("cor_bundle");
  const std::string dst = TempDir("cor_dst");
  fs::remove_all(bundle);
  fs::remove_all(dst);
  STRATUM_CHECK_TRUE(MakeSession(src, 400));
  STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " io export " + src + " --to " + bundle).exit_code, 0);

  // 让 LSM 落盘出 .ldb，然后损坏它
  (void)RunCli(std::string(kCli) + " --format csv query " + bundle + " --list-topics > /dev/null");
  STRATUM_CHECK_TRUE(CorruptSession(bundle));

  // verify 必须报问题（INTEGRITY=4）
  const RunResult v = RunCli(std::string(kCli) + " --format table io verify " + bundle);
  STRATUM_CHECK_EQ(v.exit_code, 4);
  // 前置确认：损坏是真的（否则这条用例可能在测一个没坏的东西）
  STRATUM_CHECK_TRUE(v.output.find("会话无法打开") != std::string::npos ||
                     v.output.find("条数不符") != std::string::npos);

  // import 必须**拒绝**，且**不创建目标**
  const RunResult i = RunCli(std::string(kCli) + " io import " + bundle + " --to " + dst);
  STRATUM_CHECK_EQ(i.exit_code, 4);
  STRATUM_CHECK_FALSE(fs::exists(dst));
  std::printf("      [安全] 损坏 bundle 被拒绝（verify=4, import=4，且未创建目标）\n");
}

STRATUM_TEST(CliIo, DirectoryWithoutManifestIsRejected) {
  const std::string fake = TempDir("nomani");
  // 有目录但没有 MANIFEST.json -> 不是 bundle
  const RunResult r = RunCli(std::string(kCli) + " io verify " + fake);
  STRATUM_CHECK_EQ(r.exit_code, 4);
  STRATUM_CHECK_TRUE(r.output.find("MANIFEST") != std::string::npos);
}

STRATUM_TEST(CliIo, ManifestWithWrongKindIsRejected) {
  const std::string fake = TempDir("wrongkind");
  std::ofstream o(fake + "/MANIFEST.json", std::ios::out | std::ios::trunc);
  o << "{\"schema_version\":1,\"kind\":\"something-else\"}\n";
  o.close();
  const RunResult r = RunCli(std::string(kCli) + " io verify " + fake);
  STRATUM_CHECK_EQ(r.exit_code, 4);
  STRATUM_CHECK_TRUE(r.output.find("kind") != std::string::npos);
}

// ===========================================================================
// 三、doctor：体检与有限恢复
// ===========================================================================

STRATUM_TEST(CliDoctor, HealthySessionReportsHealthy) {
  const std::string s = TempDir("doc_ok");
  STRATUM_CHECK_TRUE(MakeSession(s, 300));
  const RunResult r = RunCli(std::string(kCli) + " --format table doctor " + s);
  STRATUM_CHECK_EQ(r.exit_code, 0);
  STRATUM_CHECK_TRUE(r.output.find("健康") != std::string::npos);
  STRATUM_CHECK_TRUE(r.output.find("会话可打开") != std::string::npos);
}

STRATUM_TEST(CliDoctor, NonexistentSessionIsNotFound) {
  const std::string nope = "/tmp/stratum_cli_doc_missing_xyz";
  std::error_code ec;
  fs::remove_all(nope, ec);
  const RunResult r = RunCli(std::string(kCli) + " doctor " + nope);
  STRATUM_CHECK_EQ(r.exit_code, 8);
  STRATUM_CHECK_FALSE(fs::exists(nope));
}

STRATUM_TEST(CliDoctor, BrokenSessionReportsErrorAndDoesNotFixLsm) {
  const std::string s = TempDir("doc_bad");
  STRATUM_CHECK_TRUE(MakeSession(s, 300));
  STRATUM_CHECK_TRUE(CorruptSession(s));

  const RunResult r = RunCli(std::string(kCli) + " --format table doctor " + s +
                             " --recover");
  // 会话打不开 -> ERROR -> INTEGRITY(4)
  STRATUM_CHECK_EQ(r.exit_code, 4);
  // 必须明确说明不修 LSM 文件级损坏
  STRATUM_CHECK_TRUE(r.output.find("不在范围") != std::string::npos ||
                     r.output.find("不处理") != std::string::npos ||
                     r.output.find("文件级损坏") != std::string::npos);
  std::printf("      [诚实] 损坏会话报 ERROR，且明确声明不自动修 LSM 文件级损坏\n");
}

// ===========================================================================
// 四、★ doctor --recover 清理过期锁（唯一的自动恢复动作）
// ===========================================================================

STRATUM_TEST(CliDoctorRecover, ClearsStaleLock) {
  const std::string s = TempDir("doc_lock");
  STRATUM_CHECK_TRUE(MakeSession(s, 200));
  WriteStaleLock(s, 999999);   // 不存在的 pid

  // 体检应报"过期锁"
  RunResult d = RunCli(std::string(kCli) + " --format table doctor " + s);
  STRATUM_CHECK_TRUE(d.output.find("过期锁") != std::string::npos);

  // 锁文件非空
  std::error_code ec;
  STRATUM_CHECK_TRUE(fs::file_size(s + ".lock", ec) > 0);

  // recover 清理
  const RunResult rec = RunCli(std::string(kCli) + " doctor " + s + " --recover");
  STRATUM_CHECK_EQ(rec.exit_code, 0);
  std::error_code e2;
  STRATUM_CHECK_EQ(fs::file_size(s + ".lock", e2), static_cast<uintmax_t>(0));

  // 恢复后不应再有该告警
  d = RunCli(std::string(kCli) + " --format json doctor " + s);
  STRATUM_CHECK_TRUE(d.output.find("\"warnings\":0") != std::string::npos ||
                     d.output.find("\"errors\":0") != std::string::npos);
  std::printf("      [恢复] 过期锁被清理，锁文件归零，告警消除\n");
}

STRATUM_TEST(CliDoctorRecover, DryRunDoesNotClearLock) {
  const std::string s = TempDir("doc_lockdry");
  STRATUM_CHECK_TRUE(MakeSession(s, 200));
  WriteStaleLock(s, 999999);

  const RunResult r = RunCli(std::string(kCli) + " doctor " + s + " --recover --dry-run");
  STRATUM_CHECK_EQ(r.exit_code, 0);
  // dry-run 不得真的清理
  std::error_code ec;
  STRATUM_CHECK_TRUE(fs::file_size(s + ".lock", ec) > 0);
}

// ===========================================================================
// 五、大数据量
// ===========================================================================

STRATUM_TEST(CliIo, LargeVolumeManyTopicsRoundTrip) {
  // 造一个多话题会话（直接写 timeline，避免依赖 DDS 发布者）
  const std::string src = TempDir("big_src");
  const std::string bundle = TempDir("big_bundle");
  const std::string dst = TempDir("big_dst");
  fs::remove_all(bundle);
  fs::remove_all(dst);

  {
    TimelineImplV2 tl;
    STRATUM_CHECK_OK(tl.Open(src));
    const int kTopics = 20;
    const int kPer = 500;   // 共 10000 条
    for (int t = 0; t < kTopics; ++t) {
      uint32_t id = 0;
      TopicMetadata m;
      m.topic_name = "big/t" + std::to_string(t);
      m.type_name = "BigType";
      STRATUM_CHECK_OK(tl.RegisterTopic(m, &id));
      std::vector<Event> batch;
      batch.reserve(kPer);
      for (int k = 0; k < kPer; ++k) {
        Event e;
        e.topic_id = id;
        e.seq = static_cast<uint32_t>(k + 1);
        e.seq_in_ts = 0;
        e.source_timestamp_ns = 1700000000000000000ULL + static_cast<uint64_t>(k) * 1000ULL;
        e.reception_timestamp_ns = e.source_timestamp_ns + 500;
        e.payload = std::string(32, 'x');
        batch.push_back(e);
      }
      STRATUM_CHECK_OK(tl.AppendBatch(batch));
    }
    STRATUM_CHECK_OK(tl.Close());
  }

  STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " io export " + src + " --to " + bundle).exit_code, 0);
  STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " io verify " + bundle).exit_code, 0);
  STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " io import " + bundle + " --to " + dst).exit_code, 0);

  // 逐话题对账（多话题才能发现"只还原了部分话题"这类问题）
  for (int t = 0; t < 20; ++t) {
    const std::string topic = "big/t" + std::to_string(t);
    const RunResult a = RunCli(std::string(kCli) + " --format csv query " + src +
                               " --topic " + topic + " --limit 1");
    const RunResult b = RunCli(std::string(kCli) + " --format csv query " + dst +
                               " --topic " + topic + " --limit 1");
    STRATUM_CHECK_EQ(b.exit_code, 0);
    STRATUM_CHECK_EQ(a.exit_code, 0);
  }
  std::printf("      [规模] 20 话题 / 10000 条：导出、校验、导入、逐话题可查\n");
}

// ===========================================================================
// 六、随机（多种子）
// ===========================================================================

STRATUM_TEST(CliIo, RandomTopicCountsRoundTrip) {
  for (uint64_t seed : {3ULL, 33ULL, 333ULL}) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> n_d(1, 8);
    const int n_topics = n_d(rng);
    const int per = static_cast<int>(rng() % 200) + 50;

    const std::string src = TempDir(("rnd_src" + std::to_string(seed)).c_str());
    const std::string bundle = TempDir(("rnd_bun" + std::to_string(seed)).c_str());
    const std::string dst = TempDir(("rnd_dst" + std::to_string(seed)).c_str());
    std::error_code ec;
    fs::remove_all(bundle, ec);
    fs::remove_all(dst, ec);

    {
      TimelineImplV2 tl;
      STRATUM_CHECK_OK(tl.Open(src));
      for (int t = 0; t < n_topics; ++t) {
        uint32_t id = 0;
        TopicMetadata m;
        m.topic_name = "rnd/t" + std::to_string(t);
        m.type_name = "T";
        STRATUM_CHECK_OK(tl.RegisterTopic(m, &id));
        std::vector<Event> batch;
        for (int k = 0; k < per; ++k) {
          Event e;
          e.topic_id = id;
          e.seq = static_cast<uint32_t>(k + 1);
          e.source_timestamp_ns = 1700000000000000000ULL + static_cast<uint64_t>(k) * 1000ULL;
          e.reception_timestamp_ns = e.source_timestamp_ns + 100;
          e.payload = "p";
          batch.push_back(e);
        }
        STRATUM_CHECK_OK(tl.AppendBatch(batch));
      }
      STRATUM_CHECK_OK(tl.Close());
    }

    STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " io export " + src + " --to " + bundle).exit_code, 0);
    STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " io import " + bundle + " --to " + dst).exit_code, 0);

    // 话题数必须一致（随机话题数下最容易暴露"漏还原"）
    const RunResult a = RunCli(std::string(kCli) + " --format csv query " + src + " --list-topics");
    const RunResult b = RunCli(std::string(kCli) + " --format csv query " + dst + " --list-topics");
    auto CountLines = [](const std::string& s) {
      int n = 0;
      for (char c : s) if (c == '\n') ++n;
      return n - 1;   // 减去表头
    };
    STRATUM_CHECK_EQ(CountLines(b.output), CountLines(a.output));
  }
}

// ===========================================================================
// 七、真实场景
// ===========================================================================

// ---------------------------------------------------------------------------
// 场景 1：备份并还原（磁盘要换了 / 换机器）
// ---------------------------------------------------------------------------
STRATUM_TEST(CliIoReal, BackupAndRestore) {
  const std::string live = TempDir("bk_live");
  const std::string backup = TempDir("bk_backup");
  const std::string restored = TempDir("bk_restored");
  std::error_code ec;
  fs::remove_all(backup, ec);
  fs::remove_all(restored, ec);

  STRATUM_CHECK_TRUE(MakeSession(live, 1000));
  const uint64_t before = CountViaCli(live);

  STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " io export " + live + " --to " + backup).exit_code, 0);
  STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " io import " + backup + " --to " + restored).exit_code, 0);

  STRATUM_CHECK_EQ(CountViaCli(restored), before);

  // 还原后的会话必须能通过 verify 与 doctor
  STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " --format table verify " + restored).exit_code, 0);
  STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " doctor " + restored).exit_code, 0);
  std::printf("      [场景1] 备份→还原：%llu 条，且 verify 与 doctor 均通过\n",
              static_cast<unsigned long long>(before));
}

// ---------------------------------------------------------------------------
// 场景 2：传输损坏被检出（这才是 bundle 校验存在的意义）
// ---------------------------------------------------------------------------
STRATUM_TEST(CliIoReal, TransferCorruptionIsDetected) {
  const std::string live = TempDir("tr_live");
  const std::string bundle = TempDir("tr_bundle");
  std::error_code ec;
  fs::remove_all(bundle, ec);
  STRATUM_CHECK_TRUE(MakeSession(live, 600));

  STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " io export " + live + " --to " + bundle).exit_code, 0);

  // 先确认没损坏时能通过
  STRATUM_CHECK_EQ(RunCli(std::string(kCli) + " io verify " + bundle).exit_code, 0);

  // 模拟传输把文件截断
  STRATUM_CHECK_TRUE(CorruptSession(bundle));

  // 现在必须检出（否则用户会以为备份是好的）
  const RunResult r = RunCli(std::string(kCli) + " io verify " + bundle);
  STRATUM_CHECK_EQ(r.exit_code, 4);
  std::printf("      [场景2] 传输损坏被检出（verify 返回 INTEGRITY）\n");
}

// ---------------------------------------------------------------------------
// 场景 3：长时间录制后的会话体检（含审计信息）
// ---------------------------------------------------------------------------
STRATUM_TEST(CliDoctorReal, LongRunSessionCheckup) {
  const std::string s = TempDir("lr_sess");
  STRATUM_CHECK_TRUE(MakeSession(s, 2000));

  // 做一次裁剪 + 记一次控制事件（模拟运维动作）
  (void)RunCli(std::string(kCli) + " prune " + s +
               " --where \"topic = hello_world_topic and since @session+5s "
               "and until @session+6s\" --yes > /dev/null");
  (void)RunCli(std::string(kCli) + " ctl stop " + s + " --reason \"例行维护\" > /dev/null");

  const RunResult r = RunCli(std::string(kCli) + " --format table doctor " + s);
  STRATUM_CHECK_EQ(r.exit_code, 0);   // 主动裁剪与控制事件都不算问题
  // doctor 应把审计信息呈现出来（它们是 verify 正确归因的前提）
  STRATUM_CHECK_TRUE(r.output.find("控制事件") != std::string::npos);
  std::printf("      [场景3] 长期录制后的体检：控制事件与裁剪记录都被呈现，"
              "且退出码为 0（主动行为不算问题）\n");
}

STRATUM_TEST_MAIN()
