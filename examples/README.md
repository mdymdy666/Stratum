# Stratum 示例

两层示例，按需要选：

| 目录 | 面向 | 内容 |
|---|---|---|
| **[`cpp/`](cpp/)** | **C++ 程序员**（要嵌入这个库） | 4 个 C++ 程序，演示全部对外 API 与扩展点 |
| **本目录的 `0x_*/`** | 运维 / 验证 | 4 个 shell 一键脚本，演示端到端的可运行链路 |

---

## C++ 示例（`cpp/`）

| 示例 | 演示的 API | 实测 |
|---|---|---|
| `01_record_and_replay` | `TimelineImplV2` + `RtpsBackend` + `Recorder` + `Query` + `Player` | 录制 78 条 → 回放 117 条（100%） |
| `02_query_timeline` | `Query::ListTopics / GetInfo / FindTopic / ExportRange / GetStateAt` | 4 个 API 全部正确 |
| `03_playback_control` | 区间/速率/进度/回调/停止 + 自定义 `ISink` | 区间回放 4/8 条、进度 71.3% |
| `04_custom_backend` | **`IDdsBackend` / `ISource` / `ISink` 扩展点** | 内存后端完整跑通 |

```bash
cmake -S . -B build -G Ninja && cmake --build build     # 先建库
cmake -S examples/cpp -B build-examples -G Ninja && cmake --build build-examples
./build-examples/04_custom_backend /tmp/tl 8            # 最独立，先看这个
```

详见 [`cpp/README.md`](cpp/README.md)。

---

## Shell 示例（端到端链路）

| 示例 | 验证内容 | 实测结果 |
|---|---|---|
| [`01_hello_pubsub`](01_hello_pubsub/) | Stratum ↔ Fast DDS 最小互通 | 回放 32 → 收到 **33** |
| [`02_record_replay`](02_record_replay/) | 录制 → 回放 → 完整性校验 | 录制 86 条、缺口 0 → 收到 **86 条** |
| [`03_large_payload`](03_large_payload/) | 大样本自动分片（DATA_FRAG） | 1200B/8000B/60000B **全 PASS** |
| [`04_cross_host`](04_cross_host/) | 跨主机双向（NAT + UDP 中继） | 正向 **51/50**、反向 **51/50** |
