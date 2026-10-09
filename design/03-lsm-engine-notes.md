# LSM 引擎开发记录与缺陷分析

> 文件：`design/03-lsm-engine-notes.md`
> 记录自研 LSM 引擎的开发过程、设计决策，以及**实际踩到的三个 bug 及其根因**。

---

## 1 完成内容

### 1.1 模块

| 文件 | 行数 | 职责 |
|---|---|---|
| `include/stratum/storage/lsm/memtable.h` | ~160 | MemTable + WAL 接口、内部键契约 |
| `include/stratum/storage/lsm/skiplist.h` | ~120 | 跳表（备选索引实现，当前未启用） |
| `include/stratum/storage/lsm/lsm_engine.h` | ~200 | LSM 引擎与 SSTable 接口 |
| `src/storage/lsm/memtable.cc` | ~360 | MemTable、WAL（含 CRC32）、内部键编解码 |
| `src/storage/lsm/skiplist.cc` | ~140 | 跳表实现 |
| `src/storage/lsm/lsm_engine.cc` | ~740 | SSTable 读写、引擎主体、迭代器、Compaction |

### 1.2 架构

```
写入：PutBatch → WAL 追加（先持久化）→ MemTable（可见）
读取：MemTable → Immutable → L0（新→旧）→ L1 → L2
刷盘：MemTable 满（默认 4MB）→ 写 SSTable → 清空 WAL
合并：L0 文件数 ≥ 4 → 归并 L0+L1 → 新 L1
```

### 1.3 验证结果

```
100% tests passed, 0 tests failed out of 3

test_time_key        : 10 passed, 0 failed
test_timeline_basic  : 10 passed, 0 failed
test_storage_contract:  4 passed, 0 failed
    ├─ StoreContract.MemoryEngine         ← 参考实现
    ├─ StoreContract.LsmEngine            ← 自研引擎
    ├─ StoreContract.EngineBehaviorParity ← 两引擎行为一致
    └─ StoreContract.RangeScanParity      ← 范围扫描结果逐条一致
```

**关键意义**：`test_storage_contract` 是**同一套测试**跑两个后端。
LSM 引擎零改动通过，证明 `IStorageEngine` 抽象是成立的、插件式架构有效。

---

## 2 三个真实 bug（按发现顺序）

这三个都是**架构层面**的错误，不是笔误。记录下来避免重犯。

### Bug 1：比较函数与容器字节序不一致 → 静默的数据不可见

**症状**：写入成功，`MemTable::Count()` 也正确，但 `Get` 始终返回 NotFound。
诡异之处：`Close()` 后再 `Open()` 能读到（因为走了 SSTable 路径）。

**根因**：`CompareInternalKey` 的实现与 `std::string` 的字节序比较**不一致**。

实测证据：

```
actual < seek (std::string) = true      ← 自然的字节序比较
CompareInternalKey(actual, seek) = 1    ← 自定义函数说"大于"
```

而 `internal key` 存放在 `std::map<std::string, ...>` 中，容器用自定义比较函数排序，
`lower_bound` 也用它。**比较函数一旦与容器不变量不一致，红黑树结构就被破坏**，
查找会随机失败——而且是静默的，没有任何报错。

**修复**：
1. `CompareInternalKey` 改为与字节序完全一致（仅 `a < b` / `a > b`）
2. 更进一步：`MemTable::Get` 改为**遍历 + 取符合条件的最大 sequence**，
   不再依赖 `lower_bound` 与自定义比较函数的一致性

**教训**：
> 只要把自定义类型放进标准容器，比较函数就必须与容器的排序不变量严格一致。
> 这里有一个隐蔽点：internal key 的"按 sequence 降序"性质**应该由编码保证**
> （大端 tag 放在键尾），而**不应该**由比较函数额外翻转——两者混用必然出错。

---

### Bug 2：边界过滤分散在多处 → 范围扫描丢数据

**症状**：`MemTableCount() = 20`，但全范围扫描返回 **0 条**；
子范围（应 5 条）返回 **4 条**。

**根因**：边界逻辑（`begin` / `end` 的半开区间判断）分散在三个地方：
- `MemTable::CollectRange`
- `SSTable::CollectRange`
- `LsmIterator` 构造

每一处都"大致正确"，但组合起来就会出现：
起点定位偏一位、某层少收集一条、或某层多收集一条。

**修复**：**把边界过滤集中到唯一一处**（`LsmIterator` 构造）：
- `InternalCollectRange` 改为"不做边界过滤，只收集全部可见条目"
- `LsmIterator` 构造时统一执行：排序 → 去重 → 边界过滤 → 剔除 tombstone

**教训**：
> 边界条件（尤其是半开区间）必须在**唯一一处**实现。
> 分散在多层的"各自负责"必然出现不一致，且症状是"数据少了几条"，
> 极难定位——因为每一处单独看都是对的。

---

### Bug 3：修复 Bug 2 时引入的回归 → 范围删除误删

**症状**：范围删除 `[rd-0010, rd-0020)` 后，范围外的 `rd-0005` 也被删了。

**根因**：`DeleteRange` 原本依赖 `InternalCollectRange` 做边界过滤。
修 Bug 2 时把 `InternalCollectRange` 改成"不过滤"，
`DeleteRange` 的调用点却没同步修改 → 它收到了**全部 key** 并全部删除。

**修复**：`DeleteRange` 自己按边界过滤（并显式注释为什么不能依赖 `InternalCollectRange`）。

**教训**：
> 修改一个"共享的底层函数"语义时，**必须检查所有调用点**。
> 本例中 `InternalCollectRange` 有 2 个调用点（迭代器、范围删除），
> 语义变更只对其中一个有利。
>
> 更稳妥的做法是**不要让同一个函数服务两种语义需求**——
> 当时就应该拆成 `CollectAll` 和 `CollectRange` 两个函数。

---

## 3 设计决策记录

### 3.1 为什么 SSTable 用"读时建索引"

当前实现把整个 SSTable 载入内存建有序索引。取舍：

| 方面 | 现状 | 后续优化 |
|---|---|---|
| 实现复杂度 | 低 | 块索引 + 延迟加载 |
| 内存占用 | 与文件同量级 | 只加载索引 |
| 适用规模 | 单文件数百 MB | GB 级以上 |

**理由**：先保证正确性与可恢复性。**块编码（前缀压缩、restart 数组）是纯优化，
可以后续替换而不影响任何接口。**

### 3.2 为什么把跳表实现了却不用

`skiplist.h/.cc` 已完整实现（单写多读、原子发布、随机层高），但 `MemTable` 当前用 `std::map`。

**理由**：
1. `std::map` 的实现更容易验证正确性（Bug 1 正是在 `std::map` 上暴露的）
2. 跳表的优势在**无锁并发读**，而当前引擎的读路径已经持锁
3. 保持两个实现可以在后续做性能对比

**跳表是备选路径**，等并发读成为瓶颈时再切换。

### 3.3 时间轴场景对 Compaction 的简化

时间轴的 key 是 `(topic, timestamp, seq)` 且**单调递增写入**，因此：
- 每个 key 唯一，Compaction 几乎不需要丢弃旧版本
- Compaction 退化为"归并排序 + 去重"

因此当前采用**最简单的策略**：L0 满 4 个文件时，归并 L0+L1 全部文件成新 L1。
未实现 LevelDB 的 key 范围轮转——在顺序写入场景下收益有限。

### 3.4 DeleteRange 的实现取舍

当前实现：收集范围内全部 key，逐个写 tombstone。

**问题**：对时间轴的"按时间区间淘汰"而言，这需要枚举全部 key，代价高。

**应采用的优化方向**（记录未实现）：**range tombstone**——
记录一个区间而非逐 key 标记，读取时按区间做可见性判断。
这是 LevelDB 本身也没有的特性，需要改 MemTable 与 SSTable 的读路径。

**当前缓解**：时间轴的保留策略可以按"整个 topic 分段"执行，
避免高频的细粒度范围删除。

---

## 4 与设计文档的对照

`design/01-lsmengine-findings.md` 中我分析过 LSMengine（参考实现）的机制，
当时的结论是：

> "定点查询可以完全复用 Snapshot 机制，不需要改核心。"

**本引擎已实现该机制**（见 `LsmEngine::GetSnapshot` / `ReleaseSnapshot`，
以及 `MemTable::Get` 的 `snapshot` 参数）。
`test_storage_contract` 中的 `snapshot-isolation` 与 `snapshot-monotonic`
两个检查验证了它。

**尚未实现**（LSMengine 有、本引擎暂无）：
- Snapshot 对 Compaction 的保留边界作用（`smallest_snapshot` 机制）
- 当前 Compaction 不考虑活跃快照，因此**长期持有快照时旧版本可能被合并丢弃**

⚠️ **这是一个已知的功能缺口**，记录在此，避免被误认为已支持。

---

## 5 后续工作

| 优先级 | 项 | 说明 |
|---|---|---|
| 高 | Compaction 尊重活跃快照 | 补上第 4 节所述的缺口 |
| 高 | 录制/回放/查询层 | 端到端功能 |
| 中 | SSTable 块索引 | 支持更大规模 |
| 中 | range tombstone | 高效时间区间淘汰 |
| 低 | 跳表替换 std::map | 并发读优化 |
