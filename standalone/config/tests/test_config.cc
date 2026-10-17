// ---------------------------------------------------------------------------
// tests/test_config.cc —— stratum-config 的独立测试
//
// **刻意不依赖任何外部测试框架**：一个"给别的项目用的库"如果不能独立验证，
// 那它就还不是独立的。这里用一个 30 行的 mini runner。
//
// 编译：cmake -DSTRATUM_CONFIG_BUILD_TESTS=ON && cmake --build build
// 运行：./build/test_config
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "stratum/config/config.hpp"

namespace cfg = stratum::config;

// ===========================================================================
// mini test runner
// ===========================================================================
namespace {

int g_failed = 0;
int g_passed = 0;
const char* g_current = "";

void Check(bool ok, const char* expr, const char* file, int line) {
  if (ok) {
    ++g_passed;
    return;
  }
  ++g_failed;
  std::printf("  [FAIL] %s\n         %s (%s:%d)\n", g_current, expr, file, line);
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) Check((a) == (b), #a " == " #b, __FILE__, __LINE__)

struct Case {
  const char* name;
  void (*fn)();
};

std::vector<Case>& Registry() {
  static std::vector<Case> r;
  return r;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

#define TEST(name)                                    \
  static void name();                                 \
  static Registrar reg_##name(#name, name);           \
  static void name()

// 临时文件路径（在 $HOME 下 —— /tmp 会被系统定期清理）
std::string TempPath(const char* tag) {
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/stratum_cfg_test_" + tag;
}

void WriteFile(const std::string& path, const std::string& content) {
  std::ofstream o(path, std::ios::out | std::ios::trunc);
  o << content;
}

}  // namespace

// ===========================================================================
// 一、INI 解析
// ===========================================================================

TEST(IniFileParsesSectionsAndKeys) {
  cfg::IniFile ini;
  std::string err;
  CHECK(ini.Parse("[server]\nport = 8080\nname = svc\n[pool]\nworkers = 4\n", &err));
  CHECK_EQ(ini.Get("server", "port"), std::string("8080"));
  CHECK_EQ(ini.Get("server", "name"), std::string("svc"));
  CHECK_EQ(ini.Get("pool", "workers"), std::string("4"));
  // 不存在的 key 返回 fallback
  CHECK_EQ(ini.Get("server", "missing", "dflt"), std::string("dflt"));
  CHECK(!ini.Has("server", "missing"));
}

TEST(IniFileHandlesCommentsAndWhitespace) {
  cfg::IniFile ini;
  std::string err;
  const char* text =
      "# 整行注释\n"
      "; 另一种注释\n"
      "\n"
      "[a]\n"
      "  k1 =   v1   \n"
      "k2=v2\n";
  CHECK(ini.Parse(text, &err));
  CHECK_EQ(ini.Get("a", "k1"), std::string("v1"));   // 前后空白被 trim
  CHECK_EQ(ini.Get("a", "k2"), std::string("v2"));
  CHECK_EQ(ini.Get("a", "k3", "none"), std::string("none"));
}

// ===========================================================================
// 二、类型编解码
// ===========================================================================

TEST(ValueCodecIntBoolString) {
  // int
  CHECK_EQ(cfg::ValueCodec<int>::Encode(42), std::string("42"));
  int i = 0;
  CHECK(cfg::ValueCodec<int>::Decode(" 42 ", &i));
  CHECK_EQ(i, 42);
  CHECK(!cfg::ValueCodec<int>::Decode("abc", &i));

  // bool 的多种写法
  bool b = false;
  for (const char* t : {"true", "TRUE", "1", "yes", "on"}) {
    CHECK(cfg::ValueCodec<bool>::Decode(t, &b));
    CHECK_EQ(b, true);
  }
  for (const char* f : {"false", "FALSE", "0", "no", "off"}) {
    CHECK(cfg::ValueCodec<bool>::Decode(f, &b));
    CHECK_EQ(b, false);
  }
  CHECK(!cfg::ValueCodec<bool>::Decode("maybe", &b));

  // string 原样
  std::string s;
  CHECK(cfg::ValueCodec<std::string>::Decode("  hello  ", &s));
  CHECK_EQ(s, std::string("  hello  "));   // string 不 trim
}

TEST(ValueCodecVector) {
  // 逗号分隔，逐元素解析
  auto enc = cfg::ValueCodec<std::vector<int>>::Encode({1, 2, 3});
  CHECK_EQ(enc, std::string("1,2,3"));
  std::vector<int> v;
  CHECK(cfg::ValueCodec<std::vector<int>>::Decode("4,8,16", &v));
  CHECK_EQ(v.size(), static_cast<size_t>(3));
  CHECK_EQ(v[0], 4);
  CHECK_EQ(v[2], 16);

  // 空字符串 → 空 vector（不是失败）
  std::vector<int> empty;
  CHECK(cfg::ValueCodec<std::vector<int>>::Decode("", &empty));
  CHECK_EQ(empty.size(), static_cast<size_t>(0));

  // 含非法元素 → 整体失败
  std::vector<int> bad;
  CHECK(!cfg::ValueCodec<std::vector<int>>::Decode("1,x,3", &bad));
}

// ===========================================================================
// 三、ConfigVar 的取值与监听
// ===========================================================================

TEST(ConfigVarDefaultAndSetValue) {
  auto& config = cfg::Config::Instance();
  auto var = config.Lookup<int>("test.basic", 100, "测试项");
  CHECK(var != nullptr);
  CHECK_EQ(var->GetValue(), 100);

  var->SetValue(200);
  CHECK_EQ(var->GetValue(), 200);
}

TEST(ConfigVarListenerFires) {
  auto& config = cfg::Config::Instance();
  auto var = config.Lookup<int>("test.listen", 1);

  int calls = 0;
  int last_old = -1, last_new = -1;
  var->AddListener([&](const int& o, const int& n) {
    ++calls;
    last_old = o;
    last_new = n;
  });

  var->SetValue(5);
  CHECK_EQ(calls, 1);
  CHECK_EQ(last_old, 1);
  CHECK_EQ(last_new, 5);

  var->SetValue(9);
  CHECK_EQ(calls, 2);
  CHECK_EQ(last_old, 5);
  CHECK_EQ(last_new, 9);

  // 设成相同的值：不应触发（避免无意义的回调）
  var->SetValue(9);
  CHECK_EQ(calls, 2);
}

TEST(LookupTypeConflictReturnsNull) {
  auto& config = cfg::Config::Instance();
  auto as_int = config.Lookup<int>("test.conflict", 1);
  CHECK(as_int != nullptr);
  // 同一 key 用不同类型再查 → 空指针（而不是静默给错值）
  auto as_double = config.Lookup<double>("test.conflict", 1.0);
  CHECK(as_double == nullptr);
}

// ===========================================================================
// 四、从 INI 加载
// ===========================================================================

TEST(LoadFromIniOverridesDefaults) {
  auto& config = cfg::Config::Instance();
  auto port = config.Lookup<int>("load.port", 8080);
  auto debug = config.Lookup<bool>("load.debug", false);
  auto workers = config.Lookup<std::vector<int>>("load.workers", {1});

  cfg::IniFile ini;
  std::string err;
  CHECK(ini.Parse("[load]\nport = 9000\ndebug = true\nworkers = 4,8\n", &err));

  CHECK(config.LoadFromIni(ini, &err));
  CHECK_EQ(port->GetValue(), 9000);
  CHECK_EQ(debug->GetValue(), true);
  CHECK_EQ(workers->GetValue().size(), static_cast<size_t>(2));
  CHECK_EQ(workers->GetValue()[1], 8);
}

TEST(LoadFromIniKeepsDefaultsForMissingKeys) {
  auto& config = cfg::Config::Instance();
  auto a = config.Lookup<int>("keep.a", 111);
  auto b = config.Lookup<int>("keep.b", 222);

  cfg::IniFile ini;
  std::string err;
  CHECK(ini.Parse("[keep]\na = 999\n", &err));   // 只给 a
  CHECK(config.LoadFromIni(ini, &err));

  CHECK_EQ(a->GetValue(), 999);
  // b 未出现在文件里 → 保持默认值（这是"部分覆盖"的关键语义）
  CHECK_EQ(b->GetValue(), 222);
}

TEST(LoadFromFileReportsMissingFile) {
  auto& config = cfg::Config::Instance();
  // 保证这个配置项已被注册，避免"文件不存在"与"类型未注册"混淆
  config.Lookup<int>("nonexistent.key", 1);

  std::string err;
  const bool ok = config.LoadFromFile(TempPath("definitely_absent_xyz.ini"), &err);
  CHECK(!ok);
  CHECK(!err.empty());
}

TEST(LoadFromFileSucceeds) {
  const std::string path = TempPath("real.ini");
  WriteFile(path, "[file]\nvalue = 12345\nflag = yes\n");

  auto& config = cfg::Config::Instance();
  auto v = config.Lookup<int>("file.value", 0);
  auto f = config.Lookup<bool>("file.flag", false);

  std::string err;
  CHECK(config.LoadFromFile(path, &err));
  CHECK_EQ(v->GetValue(), 12345);
  CHECK_EQ(f->GetValue(), true);

  std::remove(path.c_str());
}

// ===========================================================================
// 五、热重载与编辑锁
// ===========================================================================

TEST(WatcherAppliesFileChanges) {
  const std::string path = TempPath("watch.ini");
  WriteFile(path, "[w]\nv = 1\n");

  auto& config = cfg::Config::Instance();
  auto v = config.Lookup<int>("w.v", 0);

  std::string err;
  CHECK(config.LoadFromFile(path, &err));
  CHECK_EQ(v->GetValue(), 1);

  // 启动轮询（50ms 一次，让测试快一点）
  CHECK(config.StartWatcher(path, std::chrono::milliseconds(50), &err));
  CHECK(config.Watching());

  // ★ 同尺寸改动（v = 1 → v = 2，字节数完全相同），且**不 sleep**。
  //
  // 这是修复后的关键验证：只比 (mtime, size) 时会漏掉这次修改
  // （文件系统 mtime 精度通常只有秒级）。现在 stamp 含内容哈希，
  // 所以必然能发现。
  WriteFile(path, "[w]\nv = 2\n");
  bool applied = false;
  for (int i = 0; i < 40 && !applied; ++i) {   // 最多等 2 秒
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    applied = (v->GetValue() == 2);
  }
  config.StopWatcher();
  CHECK(applied);
  CHECK(!config.Watching());

  std::remove(path.c_str());
}

TEST(WatcherRefusesToStartWhileEditLocked) {
  const std::string path = TempPath("lock.ini");
  WriteFile(path, "[l]\nv = 1\n");

  auto& config = cfg::Config::Instance();
  auto v = config.Lookup<int>("l.v", 0);

  std::string err;
  CHECK(config.LoadFromFile(path, &err));
  CHECK_EQ(v->GetValue(), 1);

  // 放一个非空 .lock（模拟"编辑器正在写"）
  WriteFile(path + ".lock", "editing\n");

  // ★ 语义：锁存在时 StartWatcher **直接失败**（不是"启动后暂停"）。
  // 这样调用方立刻知道为什么热重载没开，而不是以为开了、实际一直不生效。
  err.clear();
  CHECK(!config.StartWatcher(path, std::chrono::milliseconds(50), &err));
  CHECK(!err.empty());
  CHECK(err.find("锁") != std::string::npos || err.find("lock") != std::string::npos);
  CHECK(!config.Watching());

  // 锁清空后重新调用 → 应成功，且随后的改动生效
  std::remove((path + ".lock").c_str());
  err.clear();
  CHECK(config.StartWatcher(path, std::chrono::milliseconds(50), &err));
  CHECK(config.Watching());

  WriteFile(path, "[l]\nv = 2\n");
  bool applied = false;
  for (int i = 0; i < 40 && !applied; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    applied = (v->GetValue() == 2);
  }
  config.StopWatcher();
  CHECK(applied);

  std::remove(path.c_str());
}

// ===========================================================================
// 六、List 与状态查询
// ===========================================================================

TEST(ListReturnsRegisteredVars) {
  auto& config = cfg::Config::Instance();
  config.Lookup<int>("list.a", 1);
  config.Lookup<std::string>("list.b", "x");
  config.Lookup<bool>("list.c", false);

  const auto all = config.List();
  CHECK(all.size() >= 3);
  // 至少能列出我们刚注册的三个
  bool has_a = false, has_b = false, has_c = false;
  for (const auto& v : all) {
    if (!v) continue;
    // ConfigVarBase 提供 name()
    const std::string n = v->name();
    if (n == "list.a") has_a = true;
    if (n == "list.b") has_b = true;
    if (n == "list.c") has_c = true;
  }
  CHECK(has_a);
  CHECK(has_b);
  CHECK(has_c);
}

// ===========================================================================
// main
// ===========================================================================

int main() {
  std::printf("stratum-config 测试\n");
  std::printf("----------------------------------------------------------\n");
  for (const auto& c : Registry()) {
    g_current = c.name;
    const int before = g_failed;
    c.fn();
    std::printf("  %s %s\n", (g_failed == before) ? "[PASS]" : "[FAIL]", c.name);
  }
  std::printf("----------------------------------------------------------\n");
  std::printf("result: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
