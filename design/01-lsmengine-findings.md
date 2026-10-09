# Stratum 设计发现：LSMengine 改造边界分析 v0.1

> 文件名：`design/01-lsmengine-findings.md`
> 状态：**草案（基于实读源码，未评审）**
> 目的：在动手改 LSMengine 之前，先确定**哪些必须改、哪些可以不改**。

---

## 1 结论速览

| 议题 | 结论 | 依据 |
|---|---|---|
| 同时间戳去重 | **不需要改跳表和比较器** | `dbformat.h` 的 internal key 机制已支持 |
| 定点查询（查历史某刻） | **不需要改 LSMengine 核心** | Snapshot 机制已支持，见第 4 节 |
| 时间轴 key 设计 | **不需要改比较器** | 时间戳放在 user_key 里即可 |
| 真正需要改的 | **Compaction 与保留策略的集成** | 见第 5 节 |
| 风险最高的一处 | **VersionEdit / MANIFEST 的线格式** | 改它要处理向后兼容 |

**总结**：这个改动的核心可以控制在**很少的几处**，而不是全面重构。

---

## 2 证据一：internal key 已经内建"版本倒序"

### 2.1 结构

`db/dbformat.h` 中的定义：

| 项 | 值 | 行号 |
|---|---|---|
| internal key = `user_key + 8 字节 tag` | — | `:75-77` |
| tag 编码 = `sequence << 8 \| type` | — | `:58-61`、`:164-174` |
| sequence 占 56 位，type 占低 8 位 | — | `:58-61` |
| 比较规则：**先比 user_key，再按 sequence 降序** | — | `:94-95` |

### 2.2 这带来什么

**"同一个 user_key 的多个版本，按时间倒序排列"是内建行为。**

`ValueType` 只有两个值（`:52`）：`kTypeDeletion = 0x0`、`kTypeValue = 0x1`。

### 2.3 对"同时间戳"问题的解法

用户提出的问题是：同一时间戳有多条消息时，key 会不会互相覆盖？

**答案：不会，而且不需要改跳表或比较器。**

**做法**：把用户 key 编码成

```
user_key = <topic_id> + <timestamp: 8字节大端> + <seq_in_ts: 4字节大端>
```

| 分量 | 作用 |
|---|---|
| `topic_id` | 话题隔离 |
| `timestamp` | 时间轴有序（大端保证字节序 == 数值序） |
| `seq_in_ts` | **同一时间戳内的去重因子** |

**为什么这样就够了**：
1. 同一时间戳的多条消息，因为 `seq_in_ts` 不同 → 不同的 user_key → **不会互相覆盖**
2. 时间戳大端编码 → 字节序排序 == 时间序 → **范围扫描天然可用**
3. 完全不触碰 `InternalKeyComparator`、跳表、`FindFile`、`SomeFileOverlapsRange`

**备选方案（不推荐）**：把比较器改成"复合并发比较"（composite comparator）。
**风险**：比较器影响 `Version::ForEachOverlapping`（`:277`）、`FindFile`（`:44`）、
`SomeFileOverlapsRange`（`:51`）、`PickLevelForMemTableOutput`（`:464`）、
`GetOverlappingInputs`（`:491`）等多处核心逻辑，改错会出现很难查的数据丢失。

---

## 3 证据二：写入模式与 LSM 的匹配问题

### 3.1 一个必须正视的矛盾

如果把时间戳作为 user_key 的**高位**（如 `<topic> + <timestamp>`）：

| 性质 | 效果 |
|---|---|
| ✅ 时间范围扫描 | 一次 seek + 顺序读，**最优** |
| ✅ 新数据写入 | 时间戳递增 → 新 key 总是在末尾 → **顺序写，LSM 最优情况** |
| ❌ 查"某实体的最新状态" | 需要反向扫描或额外索引 |
| ⚠️ Compaction | key 空间按时间滑动，**旧数据集中在底层且不再被访问** |

**这个模式（append-only + 时间范围读）恰好是 LSM 最擅长的负载**——这一点与我此前在需求文档中的判断一致。

### 3.2 但 Compaction 会出现新特征

因为是 append-only 写入，**同一 user_key 几乎不会重复出现**（`seq_in_ts` 保证唯一）。

后果：

| 影响 | 说明 |
|---|---|
| 旧版本清理（rule A）几乎不触发 | 因为每个 user_key 只有一条记录 |
| **Compaction 变成"搬运"而非"合并"** | 数据量不减少，只是从高层搬到低层 |
| 写放大 | 每条数据会在 compaction 中被重写多次（层数 × 每次搬迁） |

**这是时间序列负载在 LSM 上的固有问题**，不是这个实现独有的。
**可能的应对**（列为待评估，不急于实现）：
- 调整层级压缩比（`MaxBytesForLevel`，`version_set.cc:43`）
- 对历史层做"冷数据不参与 compaction"的特殊处理

⚠️ **这一条我没有实测数据**，属于待验证的推断。

---

## 4 证据三：定点查询可以完全复用 Snapshot 机制

### 4.1 机制原文

`db/snapshot.h:5-6` 的注释直接说明了设计意图：

> Snapshot 在 DBImpl mutex 保护下构成按 sequence 排序的环形链表；
> **最旧的 sequence 是 Compaction 清理历史版本时的保留边界。**

### 4.2 Compaction 的实际判定

`db/db_impl.cc` 中 `DoCompactionWork` 的逻辑：

```
if (snapshots_.empty()) {
    compact->smallest_snapshot = versions_->LastSequence();     // :907-908
} else {
    compact->smallest_snapshot = snapshots_.oldest()->sequence_number();   // :910
}
```

然后在遍历 compaction 输入时：

```
if (last_sequence_for_key <= compact->smallest_snapshot) {
    drop = true;        // (A) 被更新的同 key 记录遮蔽，可以丢
}
```

**即：只要存在一个旧 Snapshot，比它更旧的**同 key 历史版本**就不会被丢弃。**

### 4.3 为 Stratum 所用的设计

```
上层（Stratum 存储层）维护：
    时间戳 T  →  sequence number S

当需要保留"T 时刻的数据"时：
    1. 记录 T 对应的 sequence S
    2. 创建并长期持有该 Snapshot（不 Release）
    3. 此后 T 之后的数据写入不受影响，但 T 之前的历史版本被保护
    4. 查询时用该 Snapshot 作为 ReadOptions.snapshot 读取 → 得到 T 时刻的一致视图
    5. 不再需要时 ReleaseSnapshot → 历史版本在后续 Compaction 中被正常清理
```

### 4.4 为什么这是"用 Version 扩展功能"的正确落点

用户的思路（"利用 Version 做定点查询"）与这个机制**方向一致，但实现不必改 Version 类本身**：

| 方案 | 做法 | 风险 |
|---|---|---|
| **A. 复用 Snapshot（推荐）** | 上层持有 Snapshot 控制保留边界 | **极低**，零源码改动 |
| B. 扩展 Version 携带时间范围 | 给 `Version`/`FileMetaData` 加时间字段 | 中：要改 `VersionEdit` 线格式（`:82-83`）与 MANIFEST 兼容 |
| C. 独立的版本历史结构 | 在 VersionSet 外加一层 | 中高：要处理并发与恢复 |

**建议路径**：**先用 A 验证功能**，如果发现 A 的能力不足（如需要"按时间范围批量管理快照"），
再考虑 B 或 C。这样能用最小代价先跑通。

### 4.5 需要注意的约束

| 约束 | 说明 |
|---|---|
| **Snapshot 会阻止空间回收** | 长期持有大量 Snapshot → 磁盘占用下不去。这正是我们想要的"保留历史"，但**必须有上限** |
| Snapshot 数量管理 | Stratum 需要自己的"保留策略"来决定何时 Release 哪个 Snapshot |
| 恢复后 Snapshot 不持久 | Snapshot 是内存对象，**重启后丢失**（`snapshot.h` 无持久化）。若要跨重启保留，需在 Stratum 侧持久化"要保留哪些时间点"，重启后重建 |

**最后一条是关键限制**，必须在设计文档中处理。

---

## 5 真正需要改动 LSMengine 的地方

基于以上分析，改动清单收敛为：

| # | 改动 | 位置 | 必要性 | 风险 |
|---|---|---|---|---|
| 1 | 保留策略与 Compaction 集成（让"该保留的"不被清理） | `db_impl.cc` `DoCompactionWork`（`:895`）、`BackgroundCompaction`（`:705`） | **必须**（可用 Snapshot 变通） | 中 |
| 2 | Compaction 触发与时间轴配合（避免历史数据被反复搬运） | `version_set.cc` `PickCompaction`（`:1229`）、`Finalize`（`:1018`）、`MaxBytesForLevel`（`:43`） | 视性能验证结果 | 中 |
| 3 | 层级压缩比调优 | `dbformat.h:26-46` 的 `config` 常量 | 视性能验证结果 | 低 |
| 4 | 时间范围删除接口（用于环形保留的批量淘汰） | 上层实现即可（范围 tombstone 或直接删文件） | 可选 | 低 |
| 5 | 若需跨重启保留历史 | 需扩展 MANIFEST 语义 | 视需求 | **高** |

### 5.1 关于"跳表设计"的结论

用户提到"跳表设计可能要修改来兼容上面的功能"。

**根据实读结论：跳表不需要改。**

理由：`db/skiplist.h`（367 行）实现的是"读无锁、单写者"的有序索引，
它只依赖传入的 Comparator。既然我们的去重因子放在 user_key 里（第 2.3 节），
跳表看到的仍是普通的有序 key。

**唯一需要跳表配合的场景**：如果要做"同一时间戳内按优先级排序"这类
**超出字节序的自定义排序**，才需要动比较器。当前设计不需要。

---

## 6 待验证清单

以下判断**我没有实测**，需要原型验证：

| # | 待验证项 | 验证方法 |
|---|---|---|
| V1 | 时间轴 key 设计的范围扫描性能 | 写入 1000 万条，测按时间范围读取的耗时 |
| V2 | append-only 负载下的写放大 | 观察 compaction 前后的数据量与耗时 |
| V3 | Snapshot 长期持有对空间回收的影响 | 持有 N 个快照，观察磁盘占用增长 |
| V4 | 同一时间戳高并发写入的表现 | 单时间戳写 1 万条，验证不丢失 |
| V5 | 重启后重建保留点的可行性 | 持久化时间点→sequence 映射并恢复 |

---

## 7 下一步

1. **继续摸清 LSMengine**：按 `BOTTOM_UP_READING_ORDER.md` 读完第 6、7、8 模块
   （`dbformat`、`skiplist`、`memtable`、`snapshot`、`version_*`、`db_impl`），
   以及第 5 模块（`table/`，因为 Block 层需确认其不可变性假设）
2. **整理 Fast DDS 的 RTPS 实现要点**（复用我已有的 7 篇笔记）
3. **产出 Stratum 设计文档**：分层架构 + 插件接口 + 上述改造方案的具体接口定义
4. **搭最小验证原型**：先做存储层（不涉及协议），验证 V1–V4
