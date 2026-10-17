// ---------------------------------------------------------------------------
// examples/basic.cc —— stratum-config 的最小可用示例
//
// 演示四件事（也是这个库的全部核心）：
//   1. 定义强类型配置项并读值
//   2. 从 INI 文件加载
//   3. 监听配置变更
//   4. 运行期热重载
//
// 编译：cmake -DSTRATUM_CONFIG_BUILD_EXAMPLES=ON
// 运行：./config_example [/path/to/app.ini]
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "stratum/config/config.hpp"

namespace cfg = stratum::config;

int main(int argc, char** argv) {
  auto& config = cfg::Config::Instance();

  // -------------------------------------------------------------------------
  // 1) 声明配置项（这一步只是"注册 + 给默认值"，不读文件）
  // -------------------------------------------------------------------------
  auto port = config.Lookup<int>("server.port", 8080, "监听端口");
  auto host = config.Lookup<std::string>("server.name", "localhost", "服务名");
  auto debug = config.Lookup<bool>("server.debug", false, "调试模式");
  auto workers = config.Lookup<std::vector<int>>("pool.workers", {1, 2, 3},
                                                "工作线程数");

  // Lookup 对同一 key 用不同类型会返回空 —— 这是刻意的，避免静默给错值
  auto conflict = config.Lookup<double>("server.port", 0.0);
  if (!conflict) {
    std::printf("[类型冲突] server.port 已有 int 定义，用 double 再查会返回空 —— 符合预期\n");
  }

  // -------------------------------------------------------------------------
  // 2) 从文件加载（可选）
  // -------------------------------------------------------------------------
  const std::string path = (argc > 1) ? argv[1] : "app.ini";
  std::string err;
  if (config.LoadFromFile(path, &err)) {
    std::printf("[加载] %s 读取成功\n", path.c_str());
  } else {
    std::printf("[加载] %s 未读到（%s）—— 用默认值继续，这在实际部署里很常见\n",
                path.c_str(), err.c_str());
  }

  // -------------------------------------------------------------------------
  // 3) 读值
  // -------------------------------------------------------------------------
  std::printf("\n当前配置:\n");
  std::printf("  server.port   = %d\n", port->GetValue());
  std::printf("  server.name   = %s\n", host->GetValue().c_str());
  std::printf("  server.debug  = %s\n", debug->GetValue() ? "true" : "false");
  std::printf("  pool.workers  = [");
  const auto w = workers->GetValue();
  for (size_t i = 0; i < w.size(); ++i) {
    std::printf("%s%d", i ? ", " : "", w[i]);
  }
  std::printf("]\n");

  // -------------------------------------------------------------------------
  // 4) 监听变更 —— 配置改了，业务马上能反应
  // -------------------------------------------------------------------------
  std::atomic<int> changes{0};
  port->AddListener([&changes](const int& old_v, const int& new_v) {
    ++changes;
    std::printf("[变更] server.port: %d -> %d（业务可以据此重建监听）\n", old_v,
                new_v);
  });

  // 改一次，验证监听生效
  port->SetValue(9000);
  std::printf("\n[设值] server.port 已设为 %d，监听器被调用 %d 次\n",
              port->GetValue(), changes.load());

  // -------------------------------------------------------------------------
  // 5) 热重载（可选：只在文件存在时演示）
  // -------------------------------------------------------------------------
  std::printf("\n热重载演示（3 秒）:\n");
  std::printf("  如果你在这 3 秒内修改 %s 里的 server.port，这里会打印新值\n",
              path.c_str());
  if (config.StartWatcher(path, std::chrono::milliseconds(500), &err)) {
    for (int i = 0; i < 6; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      std::printf("  [%d] port = %d\n", i + 1, port->GetValue());
    }
    config.StopWatcher();
  } else {
    std::printf("  无法监视文件（%s）—— 跳过热重载演示\n", err.c_str());
  }

  std::printf("\n完成。累计变更次数: %d\n", changes.load());
  return 0;
}
