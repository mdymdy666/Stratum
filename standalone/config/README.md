# stratum-config

一个**独立、零依赖**的 C++17 配置库。从 [Stratum](https://github.com/mdymdy666/Stratum) 项目中提取。

- 依赖：**C++17 + pthread**（`std::thread` 需要）。仅此而已。
- 不依赖任何日志库、状态类型、第三方框架。
- 支持 INI 文件、类型化配置项、变更监听、**运行期热重载**、编辑锁。

---

## 快速开始

### 方式 ①：作为子目录（推荐）

```cmake
add_subdirectory(third_party/stratum-config)
target_link_libraries(myapp PRIVATE stratum_config)
```

### 方式 ②：独立构建并安装

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --target install
```

```cmake
find_package(stratum_config REQUIRED)
target_link_libraries(myapp PRIVATE stratum_config::stratum_config)
```

### 五行的最小用法

```cpp
#include "stratum/config/config.hpp"

auto& cfg = stratum::config::Config::Instance();
auto port = cfg.Lookup<int>("server.port", 8080, "监听端口");
std::printf("port = %d\n", port->GetValue());
```

---

## 核心概念

### 1. 配置项是**强类型对象**，不是字符串

```cpp
auto port    = cfg.Lookup<int>("server.port", 8080);        // int
auto name    = cfg.Lookup<std::string>("server.name", "svc");
auto debug   = cfg.Lookup<bool>("server.debug", false);     // 支持 true/false/1/0/yes/no/on/off
auto workers = cfg.Lookup<std::vector<int>>("pool.workers", {1, 2, 3});
```

**类型冲突会被发现**：同一个 key 用不同类型 `Lookup` 会返回空指针（而不是静默给出错误的值）。

### 2. 从 INI 加载，自动按类型解析

```ini
[server]
port = 9000
debug = true

[pool]
workers = 4,8,16
```

```cpp
auto& cfg = stratum::config::Config::Instance();
cfg.Lookup<int>("server.port", 8080);
cfg.Lookup<bool>("server.debug", false);
cfg.Lookup<std::vector<int>>("pool.workers", {});

std::string err;
if (!cfg.LoadFromFile("app.ini", &err)) {
    std::fprintf(stderr, "配置加载失败: %s\n", err.c_str());
}
// 现在 port == 9000（文件值覆盖了默认值）
```

### 3. 变更监听 —— 配置改了，业务立刻知道

```cpp
auto port = cfg.Lookup<int>("server.port", 8080);
port->AddListener([](const int& old_value, const int& new_value) {
    // 例如：重建监听 socket
    std::printf("port %d -> %d，需要重新绑定\n", old_value, new_value);
});
```

监听器在 `SetValue` 或热重载时被调用，**同步执行**（在调用者的线程里）。

### 4. 热重载 —— 改文件即生效，不重启进程

```cpp
std::string err;
if (!cfg.StartWatcher("app.ini", std::chrono::milliseconds(500), &err)) {
    std::fprintf(stderr, "无法监视配置文件: %s\n", err.c_str());
}

// ... 业务运行，文件被改动后配置自动生效 ...

cfg.StopWatcher();
```

**安全措施**：若配置文件旁边存在**非空的 `app.ini.lock`**，`StartWatcher`
会**直接失败**并说明原因：

```
配置文件正在编辑，请先清空编辑锁: app.ini.lock
```

（注意：是**启动失败**，不是"启动后暂停"。这样调用方能立刻知道为什么热重载没开，
而不是以为开了、实际一直不生效。锁清空后重新调用 `StartWatcher` 即可。）

**变更检测方式**：轮询比对 **mtime + size + 内容哈希**。
加入内容哈希是为了覆盖「同尺寸改动」—— 把 `port = 8080` 改成 `port = 9000`
而落在同一个 mtime 精度周期内时，只比 (mtime, size) 会漏掉这次修改。

### 5. 事务式批量修改

```cpp
// 一次性改多个配置项，全部校验通过才提交；任一失败则全部不生效
```

配合 `AddTransactionValidator` 可以在提交前做整体校验。

---

## 支持的类型

| 类型 | 解析规则 |
|---|---|
| `int` / `int64_t` / `size_t` / `double` | 标准数字，前后空白忽略 |
| `bool` | `true/false`、`1/0`、`yes/no`、`on/off`（大小写不敏感） |
| `std::string` | 原样 |
| `std::vector<T>` | 逗号分隔，逐元素按 `T` 解析 |
| **任意支持 `operator<<` / `operator>>` 的类型** | 走主模板 `ValueCodec<T>` |

### 为自定义枚举提供支持

枚举通常没有流式操作符，需要特化 `ValueCodec`：

```cpp
enum class Mode { kFast, kSafe };

template <>
struct stratum::config::ValueCodec<Mode> {
    static std::string Encode(Mode v) {
        return v == Mode::kFast ? "fast" : "safe";
    }
    static bool Decode(const std::string& s, Mode* v) {
        if (s == "fast") { *v = Mode::kFast; return true; }
        if (s == "safe") { *v = Mode::kSafe; return true; }
        return false;   // 解析失败：配置项保持旧值，错误可被上层看到
    }
};
```

**特化必须在使用之前可见**，通常放在自己的头文件里，在包含 config 之后 include 进来。

---

## 设计取舍（给使用者看的诚实说明）

**① 为什么单例？**

`Config::Instance()` 返回进程级单例。理由是配置天然是全局的，且多实例会让
"同一个 key 在不同实例里值不同"变成一类难查的 bug。

**如果你需要多实例**：`Config` 的构造函数是私有的；本库不提供多实例支持。
这是一个刻意的约束，不是遗漏。

**② 热重载是轮询，不是 inotify**

默认 1 秒轮询（可配）。理由是 inotify 跨平台差异大，而配置热重载对延迟不敏感。

代价：文件改动到生效最多滞后一个轮询周期。

**③ 解析失败时保留旧值**

任何一个配置项解析失败，**该项保持旧值**，其它项正常更新。
错误通过 `LoadFromFile` 的 `error` 参数返回。

这样"配置文件写错一个值"不会让整个进程的配置回到默认值。

**④ 没有 JSON/YAML 支持**

只支持 INI。理由是 INI 的解析规则简单到可以自信地全部覆盖，
而 JSON/YAML 需要引入依赖或自己写一个不完整的解析器。

---

## 目录结构

```
standalone/config/
├── include/stratum/config/config.hpp   ← 公共头文件（零依赖）
├── src/config.cpp                       ← 实现
├── CMakeLists.txt
├── examples/basic.cc                    ← 可运行的最小示例
└── tests/test_config.cc                 ← 独立测试（不依赖外部框架）
```

## 自己跑一下

```bash
# 独立构建 + 测试
cmake -S . -B build -DSTRATUM_CONFIG_BUILD_TESTS=ON -DSTRATUM_CONFIG_BUILD_EXAMPLES=ON
cmake --build build
./build/test_config          # 全部测试
./build/config_example       # 示例程序
```

---

## 许可

与 Stratum 项目一致：Apache-2.0。`config.hpp` 的头部另有 Go2Cpp（MIT）的移植声明。
