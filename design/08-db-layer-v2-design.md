# Stratum DB 层重设计（v2）：对齐 LevelDB 架构 + 时间轴扩展

> 状态：设计骨架，待 `ref-01/02/03` 三份源码精读报告完成后填充
> 目标：放弃现有简化 LSM 引擎的修补，按 LevelDB 架构重新实现一个**功能更强的 DB 层**

---

## 1 为什么重做

现有 LSM 引擎是"理解 LevelDB 后自己简化"的产物。三轮审查证明这条路不可行：
自己发明的简化（WAL 格式、无 MANIFEST、扫描 .sst 恢复、sequence 从 WAL 推导）
每一条都对应一个已实测复现的 Critical 缺陷（C1/C2/C4/H2/H9/N-16）。

**结论**：存储引擎的正确性依赖一批相互咬合的机制（键编码 → memtable → WAL →
SSTable → MANIFEST/Version → compaction → snapshot），单独简化任何一环都会
在其他环上炸开。**要么全套按成熟设计来，要么不要自己写存储层。**

## 2 对齐 LevelDB 的组件清单

| # | 组件 | LevelDB 对应 | 状态 |
|---|---|---|---|
| 1 | 键格式（InternalKey/LookupKey/Comparator） | `db/dbformat.*` | 待实现 |
| 2 | 跳表 + Arena | `db/skiplist.h` / `util/arena.*` | 待实现 |
| 3 | MemTable（refs + skiplist） | `db/memtable.*` | 待实现 |
| 4 | WAL（block 分片 + 4 种 record type + CRC） | `db/log_*` | ✅ 已完成（LevelDB 格式） |
| 5 | SSTable（block/restart/两段迭代/filter/footer） | `table/*` | 待实现 |
| 6 | **VersionEdit + MANIFEST + CURRENT** | `db/version_edit.*` | 待实现 |
| 7 | **VersionSet（Version 链表 + Builder）** | `db/version_set.*` | 待实现 |
| 8 | Compaction（Pick + 归并 + LogAndApply） | `db/version_set.cc` | 待实现 |
| 9 | SnapshotList + smallest_snapshot 保护 | `db/snapshot.h` | 待实现 |
| 10 | DBImpl（Open/Recover/写路径/后台线程） | `db/db_impl.*` | 待实现 |
| 11 | WriteBatch | `db/write_batch.*` | 待实现 |
| 12 | 文件命名与解析 | `db/filename.*` | 待实现 |
| 13 | varint/fixed 编码 | `util/coding.*` | 待实现 |
| 14 | CRC32C + Mask | `util/crc32c.*` | 部分（现有 CRC32 需换/对齐） |

## 3 时间轴扩展（"功能更强大"的部分）

LevelDB 是为"随机 key 空间 + 覆盖写 + 点查"设计的。Stratum 的负载是
**单调追加的 TimeKey + 范围扫描 + 时间点查询**。以下是计划中的扩展点：

### 3.1 TimeKey 直接作为 user_key

TimeKey（20 字节定长，字节序 = 时间序）直接嵌入 InternalKey 的 user_key 段。
LevelDB 的 `InternalKeyComparator` 可以**原样复用**（先比 user_key，再比 tag）。

收益：
- 范围查询 [t1, t2) 直接映射为 internal key 范围，零转换
- 前缀压缩在 TimeKey 上效果极佳（相邻 key 共享 topic + 时间戳前缀）

### 3.2 时间点查询 = 快照 + 反向定位

当前 `GetStateAt` 用正向扫描（1034 μs @ 5 万条）。新设计：
- 时间点 → 内部 sequence 的映射（或直接按时间戳 seek）
- 快照机制保证一致性：`Snapshot` 已经是"某一时刻的完整视图"
- 目标：**修复 SeekToLast 后（已修 H1），用反向迭代实现 O(log n) 定点查询**

### 3.3 保留点（用户设想："自动监控 A 事件发生前的数据"）

- `RetentionPoint` = 带名字的持久化快照
- 写入 MANIFEST（新增 VersionEdit tag），崩溃后仍有效
- Compaction 尊重保留点：`smallest_snapshot` 机制扩展为
  "小于最小保留点的版本不可丢弃"
- 上层 API：`CreateRetentionPoint(name, at_time)` → 之后任意时刻
  `QueryAt(retention_point)` 读回该时刻的完整状态

### 3.4 单调追加场景的 compaction 简化

TimeKey 唯一（时间戳 + seq_in_ts），compaction 几乎无版本丢弃。
保留 LevelDB 的框架（Pick/归并/LogAndApply），但：
- 允许 seek 触发禁用（范围扫描场景 seek 计数无意义）——保留为配置项
- 允许按 topic 做 key 空间分区（分层放置不同话题）——可选优化

## 4 与现有代码的关系

| 现有组件 | 处置 |
|---|---|
| `lsm/` 简化引擎 | 废弃，替换为 v2（保留到 v2 通过全部测试） |
| `memory_engine` | 保留（作为参考实现与测试基准） |
| `ITimeline`/Recorder/Player | 保留（它们依赖 `IStorageEngine` 接口，不感知后端） |
| WAL（LevelDB 格式） | ✅ 直接复用 |
| TimeKey 编解码 | ✅ 直接复用（作为 user_key） |
| 全部 11 个测试套件 | 保留，v2 必须**不改测试**全部通过 |

## 5 验收标准

1. 现有 11 套件零修改通过（含所有审查回归测试）
2. 崩溃恢复测试：kill -9 后数据不丢、无静默损坏
3. MANIFEST 损坏时 Open 报错且不破坏数据
4. 保留点跨重启有效
5. `GetStateAt` 从 1034 μs 降到微秒级
6. 性能不劣于现有 LSM（写入 ≥ 49 万 ops/s 基线）

## 6 实现顺序

```
第 1 步：util 层（coding/crc32c/arena）
第 2 步：键格式（dbformat）—— TimeKey 作为 user_key
第 3 步：跳表 + MemTable
第 4 步：SSTable（block/table/迭代器/filter）
第 5 步：VersionEdit + MANIFEST + CURRENT
第 6 步：VersionSet + 恢复
第 7 步：DBImpl 写路径 + compaction + snapshot
第 8 步：时间轴扩展（时间点查询 + 保留点）
第 9 步：全量测试 + 性能对比 + 崩溃测试
```
