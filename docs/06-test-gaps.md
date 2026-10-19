# 测试缺口清单（审计结果）

> **生成方式**（可复现）：脚本提取 `src/include/stratum/**`（排除移植的 `rtps/`）
> 的全部类名与方法名，与 `tests/**/*.cc` 的正文对照，找出从未出现的符号；
> 再逐项人工核实"是真缺口、还是间接覆盖、还是死接口"。
>
> 审计日期：2026-10-14 · 脚本输出：**19 个类 + 86 个方法**从未出现

---

## 一、真缺口（有实现、示例用过、但测试未直接覆盖）

| 优先级 | 项 | 现状 | 建议补法 |
|---|---|---|---|
| **中高** | **`Player` 控制面**：`GetProgress` / `IsPlaying` / `RequestStop` + `PlaybackProgress` | 实现见 `src/src/core/player.cc`；`examples/cpp/core_full.cpp` 用过；**测试里没有** | 新增 `tests/core/test_player_control.cc`：进度单调递增、发布回调计数、协作式停止（`RequestStop` 后 `IsPlaying` 转 false） |
| **中** | **`Query::GetInfo` + `SessionMetadata`** | 实现见 `src/src/core/query.cc:45`；`core_demo.cpp` 用过；**测试里没有** | 并入现有 timeline 测试：字段往返、空会话、`format_version` 语义 |
| **中** | **`MetaCodec`**（`EncodeSession/DecodeSession/EncodeTopic/DecodeTopic/EncodeRetention/DecodeRetention/MakeTopicKey`） | 实现见 `src/src/storage/meta_codec.cc`；被 `timeline_impl` 间接使用；**无直接测试** | 新增 `tests/storage/test_meta_codec.cc`：往返、5 类损坏输入、**高版本必须被拒绝** |
| **中** | **`IStorageEngine` 迭代器的 seek / 反向**：`SeekToFirst` / `SeekForPrev` / `Prev` | 契约测试（`tests/unit/test_storage_contract.cc`）**只覆盖正向 `Next`** | 扩充现有契约测试，让 MemoryEngine 与 LsmEngine 都跑一遍 |
| **低** | **`SplitPolicy`**（纯配置结构） | 仅 `examples/cpp/types_demo.cpp` 覆盖 | 并入配置类测试（默认值 + 字段语义） |
| **低** | **`Status` 的分类判定**：`IsIOError` / `IsNotSupported` / `IsOutOfSpace` / `SnapshotReleased` 等 | **无测试** | 新增 `tests/unit/test_status.cc`：各工厂方法与判定方法一一对应 |
| **低** | **工厂接口**：`IDdsBackendFactory` / `IStorageEngineFactory`（含 `LsmEngineFactory` / `MemoryEngineFactory`） | **无测试** | 新增工厂创建测试：`Create()` 返回非空且类型正确 |

---

## 二、间接覆盖（可接受，不必专门补）

| 项 | 为什么可接受 |
|---|---|
| `core/timeline_impl.h` 的迭代器类（`TimelineEventIterator`） | 该头文件被 **6 个测试** include（`random/` `real/` `stress/` `review/`），属间接覆盖 |
| `storage/lsm/memtable.h` 的内部方法（`EmitPhysicalRecord` / `ExtractUserKey` / `Sync` …） | LSM 内部实现细节，由 LSM 的契约与随机测试间接覆盖 |
| `LsmSnapshot` / `MemorySnapshot` / `SSTable` / `TimeKeyComparator` | 存储内部类型 |
| `lsm_v2/stratum_db.h` 的编解码函数（`EncodeEventKey` 等） | 内部实现，由 `lsm_v2` 的测试间接覆盖 |
| `IStorageEngine::WriteAmplification` / `ConfigSchema` | 可选诊断接口 |

---

## 三、死接口（应决策，而不是补测试）

### `src/include/stratum/protocol/itransport.h`

声明了 `ITransport` / `ITransportFactory` / `Datagram` / `TransportConfig`，
但审计显示：

```
src/ 下没有任何实现或使用者
没有任何文件 include 它
```

**建议二选一**：

1. **删除** —— 当前 RTPS 后端直接用 socket，不需要这一层抽象；
2. **保留并在文件头标注**"预留接口，尚未实现"，避免后来者以为可用。

> 注：`protocol/idds_backend.h` 里的 `IDdsBackend`/`ISource`/`ISink` 是**真正在用**的
> 扩展点（有 `RtpsBackend` 实现、有测试、有示例），与 `ITransport` 的情况不同。

---

## 四、已覆盖的部分（供对照）

| 层次 | 状态 |
|---|---|
| examples（C++） | **5/5 模块**：10 文件（demo **1345 行** + full **3094 行**），**15 个真实场景**，全部编译+实测，**连跑两次幂等**，CMake **10/10** |
| tests | **5/5 模块**，`ctest` **37/37**（**2026-10-19 更新：实际 70/70**），每模块含 **大数据量 / 随机 / 真实场景** 三类 |
| 对账 A | **200/200**（序号缺口 0） |
| 真实场景脚本 | **28 个**（**2026-10-19 更新：实际 29 个**；互通、录制回放、QoS、keyed、崩溃恢复、丢包、跨主机…） |

### 各模块测试的三类证据

| 模块 | 大数据量 | 随机 | 真实场景 |
|---|---|---|---|
| core | 14 个文件 | 2 个 | 14 个 |
| protocol | `test_protocol_backend.cc` 976 行 | 4 个种子 | 3 个 |
| storage | 14 个文件 | 6 个 | 15 个 |
| types | `test_types_serializer.cc` 763 行 | 4 个种子 | 3 个 |
| rtps | `test_rtps_backend_usage.cc` 563 行 | 4 个种子 | 3 个 |

---

## 五、另有一处「接口已定、实现未落」

> **2026-10-19 更新：本节的缺口已补齐。** `src/src/types/dynamic_value.cc` 已实现全部
> `Make*` 工厂与 `ToString`，`tests/types/test_dynamic_value.cc` 直接覆盖；
> `tests/types/test_types_serializer.cc` 里的 `KnownGap_*` 用例保留为历史记录。
> 以下为原始审计内容。

`types/iserializer.h` 声明了 `DynamicValue::MakeNull/MakeBool/MakeInt32/MakeInt64/
MakeUInt32/MakeUInt64/MakeDouble/MakeString/MakeBytes/MakeArray/MakeStruct`
与 `ToString(int) const`，但**没有实现**（`nm` 扫遍所有静态库均无符号），
直接调用会链接失败（`undefined reference`）。

`tests/types/test_types_serializer.cc` 里有 `KnownGap_*` 用例**显式记录**了这一点，
并自带等价助手以保证测试可跑。建议后续补实现，或先在头文件标注未实现。
