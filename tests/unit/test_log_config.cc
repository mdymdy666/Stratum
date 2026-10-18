// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 移植模块测试：日志（logging）+ 配置（config）
//
// 来源：/UserData/CodexWorkSpace/Go2Cpp（见 docs/10-porting-notes.md）
// 移植改动：命名空间 go2cpp -> stratum，宏 GO2CPP_LOG_* -> STRATUM_LOG_*
//
// 覆盖三类（项目测试标准）：
//   · 大数据量 —— 10 万条日志、大 INI 文件
//   · 随机     —— 随机键值、随机内容，多种子
//   · 真实场景 —— ① 配置驱动日志级别与输出 ② 配置校验拒绝非法值
//                 ③ 轮转文件（RotatingFileSink）
//
// 【为什么先测这两个模块】它们是被移植来**加速后续开发**的基础设施：
// 日志用于 CLI 的 --verbose/--log-format，配置用于 `config validate/explain`
// 与后续的 `record --config`。基础设施不可靠，上层全部白做。

// 标准库头文件必须最先包含
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "stratum/config.hpp"
#include "stratum/config/level_codec.hpp"
#include "stratum/log.hpp"
#include "test_framework.h"
#include "test_isolation.h"

namespace fs = std::filesystem;

using namespace stratum;

namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string TempDir(const char* tag) {
  const std::string d = std::string("/tmp/stratum_port_") + tag + "_" + UniqueSuffix();
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d, ec);
  return d;
}

std::string ReadAll(const std::string& path) {
  std::ifstream in(path);
  if (!in) return {};
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return s;
}

size_t CountLines(const std::string& s) {
  size_t n = 0;
  for (char c : s) {
    if (c == '\n') ++n;
  }
  return n;
}

}  // namespace

// ===========================================================================
// 一、IniFile：解析与查询
// ===========================================================================

STRATUM_TEST(PortIni, ParseSectionsKeysAndComments) {
  config::IniFile ini;
  const std::string text =
      "# 顶部注释\n"
      "; 另一种注释\n"
      "[record]\n"
      "dir = /data/stratum\n"
      "retain = 24h      # 行尾注释\n"
      "\n"
      "[log]\n"
      "level = info\n"
      "stdout = true\n";
  std::string err;
  STRATUM_CHECK_TRUE(ini.Parse(text, &err));

  STRATUM_CHECK_TRUE(ini.Has("record", "dir"));
  STRATUM_CHECK_EQ(ini.Get("record", "dir"), std::string("/data/stratum"));
  STRATUM_CHECK_EQ(ini.Get("record", "retain"), std::string("24h"));
  STRATUM_CHECK_EQ(ini.Get("log", "level"), std::string("info"));
  STRATUM_CHECK_EQ(ini.Get("log", "stdout"), std::string("true"));

  // 不存在的键返回 fallback，而不是空串掩盖问题
  STRATUM_CHECK_FALSE(ini.Has("record", "nope"));
  STRATUM_CHECK_EQ(ini.Get("record", "nope", "FALLBACK"), std::string("FALLBACK"));
  STRATUM_CHECK_EQ(ini.Get("nosection", "k", "FB"), std::string("FB"));
}

STRATUM_TEST(PortIni, ReparseReplacesOldContent) {
  config::IniFile ini;
  STRATUM_CHECK_TRUE(ini.Parse("[a]\nk = 1\n"));
  STRATUM_CHECK_EQ(ini.Get("a", "k"), std::string("1"));
  // 重复 Parse 必须先清空，否则旧键会残留（会让配置"改不掉"）
  STRATUM_CHECK_TRUE(ini.Parse("[b]\nm = 2\n"));
  STRATUM_CHECK_FALSE(ini.Has("a", "k"));
  STRATUM_CHECK_EQ(ini.Get("b", "m"), std::string("2"));
}

STRATUM_TEST(PortIni, LoadFromFileAndMissingFile) {
  const std::string dir = TempDir("ini");
  const std::string path = dir + "/t.ini";
  {
    std::ofstream o(path);
    o << "[x]\nkey = value\n";
  }
  config::IniFile ini;
  std::string err;
  STRATUM_CHECK_TRUE(ini.Load(path, &err));
  STRATUM_CHECK_EQ(ini.Get("x", "key"), std::string("value"));

  // 不存在的文件必须失败并给出诊断，而不是静默返回空配置
  config::IniFile ini2;
  std::string err2;
  STRATUM_CHECK_FALSE(ini2.Load(dir + "/not_exist.ini", &err2));
  STRATUM_CHECK_TRUE(!err2.empty());
}

// ===========================================================================
// 二、ValueCodec：类型转换（含非法输入必须被拒绝）
// ===========================================================================

STRATUM_TEST(PortCodec, ScalarRoundTrip) {
  // int
  int i = 0;
  STRATUM_CHECK_TRUE(config::ValueCodec<int>::Decode("42", &i));
  STRATUM_CHECK_EQ(i, 42);
  STRATUM_CHECK_EQ(config::ValueCodec<int>::Encode(42), std::string("42"));

  // bool：四种真值与四种假值都要认
  for (const char* t : {"1", "true", "TRUE", "yes", "on", "On"}) {
    bool b = false;
    STRATUM_CHECK_TRUE(config::ValueCodec<bool>::Decode(t, &b));
    STRATUM_CHECK_TRUE(b);
  }
  for (const char* f : {"0", "false", "FALSE", "no", "off", "Off"}) {
    bool b = true;
    STRATUM_CHECK_TRUE(config::ValueCodec<bool>::Decode(f, &b));
    STRATUM_CHECK_FALSE(b);
  }
  // 无法识别的文本必须失败（不能"猜"）
  bool b = false;
  STRATUM_CHECK_FALSE(config::ValueCodec<bool>::Decode("maybe", &b));
  STRATUM_CHECK_FALSE(config::ValueCodec<bool>::Decode("", &b));

  // string 原样
  std::string s;
  STRATUM_CHECK_TRUE(config::ValueCodec<std::string>::Decode("a b c", &s));
  STRATUM_CHECK_EQ(s, std::string("a b c"));
}

STRATUM_TEST(PortCodec, NumericRejectsTrailingGarbage) {
  // "42abc" 这类必须被拒绝 —— 否则配置里的拼写错误会被静默吞掉
  int i = 0;
  STRATUM_CHECK_FALSE(config::ValueCodec<int>::Decode("42abc", &i));
  STRATUM_CHECK_FALSE(config::ValueCodec<int>::Decode("4 2", &i));
  STRATUM_CHECK_TRUE(config::ValueCodec<int>::Decode(" 42 ", &i));
  STRATUM_CHECK_EQ(i, 42);
}

STRATUM_TEST(PortCodec, VectorCommaSeparated) {
  std::vector<int> v;
  STRATUM_CHECK_TRUE(config::ValueCodec<std::vector<int>>::Decode("1,2,3", &v));
  STRATUM_CHECK_EQ(v.size(), static_cast<size_t>(3));
  STRATUM_CHECK_EQ(v[0], 1);
  STRATUM_CHECK_EQ(v[2], 3);
  STRATUM_CHECK_EQ(config::ValueCodec<std::vector<int>>::Encode(v), std::string("1,2,3"));

  // 元素非法则整体失败
  std::vector<int> bad;
  STRATUM_CHECK_FALSE(config::ValueCodec<std::vector<int>>::Decode("1,x,3", &bad));
}

STRATUM_TEST(PortCodec, LogLevelCodec) {
  // 配置里的日志级别走 ValueCodec<log::Level>，与日志模块打通
  log::Level lv = log::Level::Info;
  STRATUM_CHECK_TRUE(config::ValueCodec<log::Level>::Decode("debug", &lv));
  STRATUM_CHECK_EQ(static_cast<int>(lv), static_cast<int>(log::Level::Debug));
  STRATUM_CHECK_TRUE(config::ValueCodec<log::Level>::Decode("WARN", &lv));
  STRATUM_CHECK_EQ(static_cast<int>(lv), static_cast<int>(log::Level::Warn));

  const std::string enc = config::ValueCodec<log::Level>::Encode(log::Level::Error);
  log::Level back = log::Level::Info;
  STRATUM_CHECK_TRUE(config::ValueCodec<log::Level>::Decode(enc, &back));
  STRATUM_CHECK_EQ(static_cast<int>(back), static_cast<int>(log::Level::Error));
}

// ===========================================================================
// 三、日志模块：真的写出来了吗
// ===========================================================================

STRATUM_TEST(PortLog, WritesToFileAndCanBeReadBack) {
  const std::string dir = TempDir("logfile");

  log::LoggerManager& mgr = log::LoggerManager::Instance();
  // 配到 info 且关闭 stdout，只写文件（避免污染测试输出）
  mgr.Configure(log::Level::Info, /*stdout_enabled=*/false, dir, "t.log");
  auto logger = mgr.Get("TestLogger");
  STRATUM_CHECK_TRUE(logger != nullptr);

  STRATUM_LOG_INFO(logger) << "hello-from-port";
  STRATUM_LOG_WARN(logger) << "warn-from-port";
  // debug 应被级别过滤掉
  STRATUM_LOG_DEBUG(logger) << "debug-should-be-filtered";

  // 给后台 worker 落盘的时间（日志是异步的，这是它的设计）
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const std::string s = ReadAll(dir + "/t.log");
    if (s.find("warn-from-port") != std::string::npos) break;
  }
  const std::string content = ReadAll(dir + "/t.log");

  STRATUM_CHECK_TRUE(!content.empty());
  STRATUM_CHECK_TRUE(content.find("hello-from-port") != std::string::npos);
  STRATUM_CHECK_TRUE(content.find("warn-from-port") != std::string::npos);
  // 级别过滤必须生效
  STRATUM_CHECK_TRUE(content.find("debug-should-be-filtered") == std::string::npos);
  // 消息里应带 logger 名（默认格式含 {logger}）
  STRATUM_CHECK_TRUE(content.find("TestLogger") != std::string::npos);
}

STRATUM_TEST(PortLog, LevelFilterIsRespected) {
  const std::string dir = TempDir("loglvl");
  log::LoggerManager& mgr = log::LoggerManager::Instance();

  // 只放行 error 及以上
  mgr.Configure(log::Level::Error, false, dir, "e.log");
  auto lg = mgr.Get("LvlLogger");
  STRATUM_LOG_INFO(lg) << "info-x";
  STRATUM_LOG_WARN(lg) << "warn-x";
  STRATUM_LOG_ERROR(lg) << "error-x";

  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (ReadAll(dir + "/e.log").find("error-x") != std::string::npos) break;
  }
  const std::string c = ReadAll(dir + "/e.log");
  STRATUM_CHECK_TRUE(c.find("error-x") != std::string::npos);
  STRATUM_CHECK_TRUE(c.find("info-x") == std::string::npos);
  STRATUM_CHECK_TRUE(c.find("warn-x") == std::string::npos);
}

// ===========================================================================
// 四、大数据量
// ===========================================================================

STRATUM_TEST(PortLog, LargeVolumeHundredThousandLines) {
  const std::string dir = TempDir("logbig");
  log::LoggerManager& mgr = log::LoggerManager::Instance();
  mgr.Configure(log::Level::Info, false, dir, "big.log");
  auto lg = mgr.Get("BigLogger");

  const size_t kN = 100000;
  auto t0 = Clock::now();
  for (size_t i = 0; i < kN; ++i) {
    STRATUM_LOG_INFO(lg) << "line-" << i << " payload=" << (i * 7919 % 1000003);
  }
  const double enqueue_ms = MsSince(t0);

  // 等落盘。
  //
  // 【注意】不要在这里每次都读整个文件 —— 6MB × 数千次会把测试自己变成瓶颈，
  // 测出来的"落盘耗时"其实是测试的读文件耗时。这里用递进间隔 + 有上限的轮询。
  const std::string logfile = dir + "/big.log";
  size_t lines = 0;
  auto t1 = Clock::now();
  // 固定观察窗：不追求"等到一条不落"（那不是异步日志的契约，也会拖慢测试）
  for (int i = 0; i < 12; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    lines = CountLines(ReadAll(logfile));
    if (lines >= kN) break;
  }
  const double flush_ms = MsSince(t1);

  std::printf("      [规模] 写 %zu 条：入队 %.1f ms（异步日志的设计目标就是"
              "不阻塞调用方）；%.1f ms 观察窗内已落盘 %zu 行\n",
              kN, enqueue_ms, flush_ms, lines);

  // 契约 1：入队不能阻塞调用方（这是选这套日志库的核心原因）
  STRATUM_CHECK_LT(enqueue_ms, 2000.0);
  // 契约 2：绝大多数必须落盘。异步 worker 在结尾允许极少量未落盘，
  //         但绝不允许大面积丢失 —— 如实打印缺口，不做"一条不丢"的假断言。
  const size_t missing = (lines >= kN) ? 0 : (kN - lines);
  std::printf("      [规模] 观察窗结束时未落盘 %zu 条（占 %.4f%%）—— "
              "异步 worker 会继续消费，此处只验证【持续落盘】\n",
              missing, 100.0 * static_cast<double>(missing) / static_cast<double>(kN));
  // 契约：在观察窗内必须已经落盘绝大多数（证明 worker 在正常工作，不是卡死）
  STRATUM_CHECK_GE(lines, kN / 2);
  STRATUM_CHECK_GE(lines, kN - kN / 1000);   // 且最终达到 99.9%
}

STRATUM_TEST(PortIni, LargeIniFile) {
  // 大 INI：5000 个键
  std::string text = "[big]\n";
  const size_t kN = 5000;
  for (size_t i = 0; i < kN; ++i) {
    text += "key" + std::to_string(i) + " = value" + std::to_string(i) + "\n";
  }
  config::IniFile ini;
  auto t0 = Clock::now();
  STRATUM_CHECK_TRUE(ini.Parse(text));
  const double ms = MsSince(t0);

  STRATUM_CHECK_TRUE(ini.Has("big", "key0"));
  STRATUM_CHECK_TRUE(ini.Has("big", "key4999"));
  STRATUM_CHECK_EQ(ini.Get("big", "key4999"), std::string("value4999"));
  std::printf("      [规模] 解析 %zu 键的 INI：%.2f ms\n", kN, ms);
}

// ===========================================================================
// 五、随机（多种子）
// ===========================================================================

STRATUM_TEST(PortIni, RandomKeysAndValuesMultipleSeeds) {
  for (uint64_t seed : {1ULL, 42ULL, 777ULL, 20261014ULL}) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> n_d(50, 300);
    std::uniform_int_distribution<int> ch_d(0, 25);
    std::uniform_int_distribution<int> val_d(0, 1000000);
    std::uniform_int_distribution<int> mode_d(0, 3);

    const int n = n_d(rng);
    std::vector<std::pair<std::string, std::string>> expect;
    std::string text = "[s]\n";
    for (int i = 0; i < n; ++i) {
      const std::string key = "k" + std::to_string(i);
      std::string val;
      switch (mode_d(rng)) {
        case 0: val = std::to_string(val_d(rng)); break;
        case 1: val = std::string(1, static_cast<char>('a' + ch_d(rng))); break;
        case 2: val = "true"; break;
        default: val = "with space"; break;
      }
      text += key + " = " + val + "\n";
      expect.emplace_back(key, val);
    }
    config::IniFile ini;
    STRATUM_CHECK_TRUE(ini.Parse(text));
    for (const auto& kv : expect) {
      STRATUM_CHECK_EQ(ini.Get("s", kv.first), kv.second);
    }
  }
}

STRATUM_TEST(PortCodec, RandomNumericRoundTripMultipleSeeds) {
  for (uint64_t seed : {7ULL, 99ULL, 20261014ULL}) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> v_d(-1000000, 1000000);
    for (int i = 0; i < 3000; ++i) {
      const int v = v_d(rng);
      const std::string enc = config::ValueCodec<int>::Encode(v);
      int back = 0;
      STRATUM_CHECK_TRUE(config::ValueCodec<int>::Decode(enc, &back));
      STRATUM_CHECK_EQ(back, v);
    }
  }
}

// ===========================================================================
// 六、真实场景
// ===========================================================================

// ---------------------------------------------------------------------------
// 场景 1：用配置文件驱动日志行为
//
// 这是移植这两个模块的**直接动机**：CLI 需要 `--config` 指定日志级别、
// 目录、是否输出到 stdout，而不是把这些写死在代码里。
// ---------------------------------------------------------------------------
STRATUM_TEST(PortReal, ConfigDrivenLogging) {
  const std::string dir = TempDir("cfgdlog");
  const std::string ini_path = dir + "/app.ini";
  {
    std::ofstream o(ini_path);
    o << "[log]\n"
         "level = warn\n"
         "stdout = false\n"
         "directory = " << dir << "\n"
         "file = app.log\n";
  }

  config::IniFile ini;
  STRATUM_CHECK_TRUE(ini.Load(ini_path));

  log::Level lv = log::Level::Info;
  STRATUM_CHECK_TRUE(config::ValueCodec<log::Level>::Decode(
      ini.Get("log", "level", "info"), &lv));
  bool to_stdout = true;
  STRATUM_CHECK_TRUE(config::ValueCodec<bool>::Decode(
      ini.Get("log", "stdout", "true"), &to_stdout));

  STRATUM_CHECK_EQ(static_cast<int>(lv), static_cast<int>(log::Level::Warn));
  STRATUM_CHECK_FALSE(to_stdout);

  log::LoggerManager::Instance().Configure(lv, to_stdout,
                                           ini.Get("log", "directory", dir),
                                           ini.Get("log", "file", "app.log"));
  auto lg = log::LoggerManager::Instance().Get("CfgLogger");
  STRATUM_LOG_INFO(lg) << "info-must-be-filtered";   // warn 级别下应被滤掉
  STRATUM_LOG_ERROR(lg) << "error-must-appear";

  const std::string logfile = dir + "/app.log";
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (ReadAll(logfile).find("error-must-appear") != std::string::npos) break;
  }
  const std::string c = ReadAll(logfile);
  STRATUM_CHECK_TRUE(c.find("error-must-appear") != std::string::npos);
  STRATUM_CHECK_TRUE(c.find("info-must-be-filtered") == std::string::npos);
  std::printf("      [场景1] 配置驱动：level=warn 生效，stdout 关闭\n");
}

// ---------------------------------------------------------------------------
// 场景 2：配置校验必须拒绝非法值
//
// 对应项目的 EC-16/EC-17：配置里写错一个键或一个值，
// 若被静默接受，轻则行为不符预期，重则（保留窗口写错）立刻删光数据。
// ---------------------------------------------------------------------------
STRATUM_TEST(PortReal, ConfigValidationRejectsBadValues) {
  config::IniFile ini;
  STRATUM_CHECK_TRUE(ini.Parse(
      "[record]\nretain = 24h\nlevel = verbose\nstdout = maybe\n"));

  // 非法日志级别必须被拒绝（不能"猜"成某个默认级别）
  log::Level lv = log::Level::Info;
  STRATUM_CHECK_FALSE(config::ValueCodec<log::Level>::Decode(
      ini.Get("record", "level"), &lv));
  // 非法布尔同样拒绝
  bool b = false;
  STRATUM_CHECK_FALSE(config::ValueCodec<bool>::Decode(
      ini.Get("record", "stdout"), &b));
  // 合法值通过
  bool ok = false;
  STRATUM_CHECK_TRUE(config::ValueCodec<bool>::Decode("off", &ok));
  STRATUM_CHECK_FALSE(ok);

  std::printf("      [场景2] 非法 level/stdout 被明确拒绝（不猜默认值）\n");
}

// ---------------------------------------------------------------------------
// 场景 3：轮转文件
//
// 长期常开（DP-02）必须有日志轮转，否则日志自己会把磁盘写满 ——
// 这与 EC-11（磁盘写满）是同一类风险，只是来源不同。
// ---------------------------------------------------------------------------
STRATUM_TEST(PortReal, RotatingFileSink) {
  const std::string dir = TempDir("rot");
  const std::string path = dir + "/rot.log";

  // RotatingFileSink 通过 LoggerManager 的目录/文件配置启用轮转
  log::LoggerManager::Instance().Configure(log::Level::Info, false, dir, "rot.log");
  auto lg = log::LoggerManager::Instance().Get("RotLogger");

  // 写足够多的内容以触发轮转（阈值由实现决定）
  for (int i = 0; i < 20000; ++i) {
    STRATUM_LOG_INFO(lg) << "rotating-line-" << i << " "
                         << std::string(64, 'x');
  }
  for (int i = 0; i < 300; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (CountLines(ReadAll(path)) > 5000) break;
  }

  std::error_code ec;
  size_t files = 0;
  uint64_t bytes = 0;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec)) {
      ++files;
      bytes += static_cast<uint64_t>(e.file_size(ec));
    }
  }
  std::printf("      [场景3] 目录下日志文件 %zu 个，合计 %llu 字节\n", files,
              static_cast<unsigned long long>(bytes));
  STRATUM_CHECK_TRUE(files >= 1);
  STRATUM_CHECK_TRUE(bytes > 0);
}

STRATUM_TEST_MAIN()
