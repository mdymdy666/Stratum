# ref-04：LSMengine DBImpl vs StratumDB 行级对比审查

> 审查方式：逐行精读 + 交叉引用 + 具体数字验证（脚本模拟 TimeKey 编码与 Seek/Prev 边界）。
> 约定：每条结论均附 `文件:行号` 证据；无法从代码确定的事项标注 **待确认**。
> 审查对象行数核对：`db_impl.cc` 实测 1578 行；`stratum_db.cc` 实测 469 行（任务所述"约 450"属实）。

## 0. 证据文件清单

| 文件 | 路径 | 行数 |
|---|---|---|
| DBImpl 实现 | `LSMengine/db/db_impl.cc` | 1578 |
| DBImpl 头 | `LSMengine/db/db_impl.h` | 213 |
| DB 公共接口 | `LSMengine/include/LSMengine/db.h` | 145 |
| WriteBatch | `LSMengine/db/write_batch.cc` | 153 |
| Options/ReadOptions/WriteOptions | `LSMengine/include/LSMengine/options.h` | 153 |
| Iterator 契约 | `LSMengine/include/LSMengine/iterator.h` | 98 |
| Snapshot 实现 | `LSMengine/db/snapshot.h` | 96 |
| MemTable | `LSMengine/db/memtable.cc` | — |
| WAL 恢复 | `LSMengine/db/log_reader.cc` | — |
| StratumDB 实现 | `Stratum/src/src/storage/lsm_v2/stratum_db.cc` | 469 |
| StratumDB 头 | `Stratum/src/include/stratum/storage/lsm_v2/stratum_db.h` | 169 |
| TimeKey 编解码 | `Stratum/src/src/storage/time_key.cc` / `.../include/.../time_key.h` | 151 / 177 |
| 比较器 | `Stratum/src/src/storage/lsm_v2/time_key_comparator.cc` | 31 |
| 核心类型 | `Stratum/src/include/stratum/core/types.h` | 198 |
| 时间线接口 | `Stratum/src/include/stratum/core/itimeline.h` | 170 |
| 需求文档 | `Stratum/01-requirements.md` | 556 |
| 项目已知缺口 | `Stratum/design/05-known-gaps.md` | 182 |
| v2 现有测试 | `Stratum/tests/review/test_lsm_v2_basic.cc` | 100 |

---

## A. 自动继承清单（机制 → 证据 → 结论）

StratumDB 通过持有 `LSMengine::DB* db_`（stratum_db.h:150）直接复用 DBImpl，以下机制**自动继承**：

| # | DBImpl 机制 | LSMengine 证据 | StratumDB 调用证据 | 结论 |
|---|---|---|---|---|
| A1 | writers_ 队列 / group commit | db_impl.cc:40-49（Writer 结构）、1203-1274（Write 排队、队首批量合并）、1278-1324（BuildBatchGroup 按 sync 分组） | stratum_db.cc:196、260、425 均调 `db_->Write()` | **完全继承**。多线程 AppendBatch 的写入原子性与顺序由底层保证 |
| A2 | write stall（写停顿/限速） | db_impl.cc:1328-1402（MakeRoomForWrite：bg_error 短路 1334-1337、L0 慢写 1338-1348、L0 停写 1359-1362、等 imm_ 1354-1358） | 同上，经 `db_->Write()` 继承 | **完全继承**。memtable 满/L0 堆积时写线程被自动限速 |
| A3 | bg_error_ fail-stop（写路径） | db_impl.cc:657-663（RecordBackgroundError）、1245-1250（sync 失败强制 fail-stop）、1334-1337（后续写返回 bg_error_） | stratum_db.cc:197-200、261-263、426-428：`db_->Write` 非 OK 翻译为 `Status::IOError` | **写路径继承**；**读路径有缺口**：GetStateAt 未检查 Iterator::status()（见 B-M1） |
| A4 | DeleteObsoleteFiles | db_impl.cc:222-287（实现）、573/759/1532（compaction 后与 Open 时调用） | 无 StratumDB 代码，由 db_ 内部触发 | **完全继承**。过期 SSTable/WAL 自动清理 |
| A5 | compaction 调度 | db_impl.cc:665-680（MaybeScheduleCompaction）、682-703（BackgroundCall 尾调度）、1156-1158（Get 触发） | 同上 | **完全继承**。memtable 满即自动 compact |
| A6 | smallest_snapshot 快照保护 | db_impl.cc:907-911（DoCompactionWork 取 snapshots_.oldest()）；snapshot.h:41-56（SnapshotList.oldest） | stratum_db.cc:439（CreateRetentionPoint 调 `db_->GetSnapshot()`） | **完全继承**——但仅在创建保留点之后才生效；StratumDB 未把快照用于读路径（见 B-H1），保护是"半截"的 |
| A7 | LOCK 文件（单实例互斥） | db_impl.cc:297（Recover 中 `env_->LockFile(LockFileName(dbname_))`）、158-160（析构 UnlockFile） | stratum_db.cc:81（`LSMengine::DB::Open`） | **完全继承**。两进程打开同一目录被拒 |
| A8 | WAL + MANIFEST 崩溃恢复 | db_impl.cc:289-380（Recover）、382-500（RecoverLogFile：429-445 整批重放、446-450 max_sequence、419 checksum reader）、1502-1543（DB::Open） | stratum_db.cc:81 | **完全继承**。Close 后 memtable 内容靠 WAL 重放恢复（不丢，见 B-M8 的 fsync 限制） |
| A9 | WriteBatch 原子性 | db_impl.cc:1222-1255（单批序列化进 WAL 1233、整批 InsertInto 1242）；log_reader.cc:237-245（校验和失败丢弃整条记录） | stratum_db.cc:191-194（RegisterTopic 双 Put 同批）、419-422（DeleteRange 批量 tombstone 同批） | **完全继承**。RegisterTopic 的"topic meta + next_topic_id"原子落盘 |
| A10 | 迭代器隐式快照（读一致性） | db_impl.cc:1165-1175（无 snapshot 时用 NewInternalIterator 时的 latest_snapshot 固定视图） | stratum_db.cc:277-278、325-326（ReadOptions 不带 snapshot） | **完全继承**。边录边放时单次迭代器视图稳定，新写入不影响进行中的扫描 |
| A11 | DB 级线程安全 | db.h:46（"多个线程可以并发访问同一个 DB，无需调用方额外加锁"） | 全部 db_ 调用 | **完全继承** |
| A12 | 键序一致性契约 | options.h:42-43（comparator 每次打开必须一致）；time_key_comparator.cc:13-27（memcmp 字典序） | stratum_db.cc:77（`options.comparator = &db->comparator_`） | **完全继承**。TimeKeyComparator 是纯 memcmp，跨重启稳定 |

**StratumDB 有责任但未做（本清单的"反向"）**：

| # | 责任项 | 证据 | 详见 |
|---|---|---|---|
| A13 | seq_in_ts 计数持久化 | 计数器只在内存（stratum_db.cc:231 `seq_in_ts_state_`），Open 恢复路径（138-172）只重建 topic 表 | B-C1 |
| A14 | 读路径的 Iterator::status() 检查 | GetStateAt 全函数无 status 检查 | B-M1 |
| A15 | 保留点读路径集成 | ReadOptions.snapshot 从未被设置 | B-H1 |
| A16 | 参数透传 | Open 只固定 create_if_missing=true + comparator（stratum_db.cc:76-81） | B-M4 |
| A17 | 错误状态暴露的完整性 | Flush/Compact 走 void CompactRange，错误被吞 | B-M5 |

---

## B. StratumDB 问题清单

### Critical

#### B-C1：seq_in_ts 计数器不持久化 → 跨重启同 (topic, ts) 追加静默覆盖历史数据

- **位置**：stratum_db.cc:231（`auto& counter = seq_in_ts_state_[topic_id].second; return counter++;`）；对照恢复路径 stratum_db.cc:138-172（LoadTopics 只重建 `topic_ids_`/`next_topic_id_`，**不重建 seq_in_ts_state_**）。
- **证据**：
  ```cpp
  // stratum_db.cc:223-233（NextSeqInTs 全部状态在内存）
  (void)ts_ns;
  auto& counter = seq_in_ts_state_[topic_id].second;
  return counter++;
  ```
  ```cpp
  // stratum_db.cc:138-172（LoadTopics：topic 恢复完备，seq 计数缺失）
  topic_ids_.clear();
  next_topic_id_ = 1;
  ...
  topic_ids_[name] = id;
  ```
- **机理（数字验证，见附录 E-5）**：重启后 counter 从 0 重新分配。若对**已存在过的 (topic, ts)** 追加，新条目 key = 历史条目 key 完全相同（如 seq=0 → `53545231 00000007 0000000000000064 ffffffff`）。LSMengine 同 user key 的新 seq 在读取时优先（memtable.cc:104 `MemTable::Get` 按 LookupKey 取最新 seq），旧值不可见 → **静默覆盖，无任何报错**。
- **DBImpl 对照**：LSMengine 自身无此问题——它的 sequence number 由 MANIFEST 持久化并在恢复时重建（db_impl.cc:325-377：Recover 中 `versions_->SetLastSequence(max_sequence)`）。**StratumDB 本应仿照此机制把每 topic 的 seq_in_ts 高水位随元数据持久化，但没做**。
- **项目自认**：05-known-gaps.md:51-68 已记录同一缺陷（针对旧 Timeline 实现），lsm_v2 重写后**问题原样保留**。
- **触发条件**：跨重启 + 重复时间戳（时钟回拨、重录同源数据、同 ts 多条的分段写入）。正常连续录制不会触发（05-known-gaps.md:62 也承认）。
- **影响**：违反需求"批量追加：同 ts 多条"与"崩溃恢复"的组合语义；数据不可恢复。

#### B-C2：AppendBatch 丢弃 Event 的 reception_timestamp_ns / seq / instance_key → 双时间戳与来源区分永久丢失

- **位置**：stratum_db.cc:251-252（value 只存 payload）；对照 Event 结构 types.h:66-83。
- **证据**：
  ```cpp
  // stratum_db.cc:250-252
  const uint32_t seq_in_ts = NextSeqInTs(e.topic_id, e.source_timestamp_ns);
  batch.Put(EventKey(e.topic_id, e.source_timestamp_ns, seq_in_ts),
            e.payload);
  ```
  ```cpp
  // types.h:66-83（Event 完整字段）
  uint32_t seq = 0;                 // 话题内单调序号（Recorder 分配）
  uint32_t seq_in_ts = 0;
  TimestampNs source_timestamp_ns = ...;
  TimestampNs reception_timestamp_ns = ...;  // ← 未落盘
  std::string payload;
  std::string instance_key;                   // ← 未落盘
  ```
  仅 `topic_id`（在 key）、`source_timestamp_ns`（在 key）、`seq_in_ts`（在 key）、`payload`（在 value）落盘。`seq`、`reception_timestamp_ns`、`instance_key` **全部丢失且不可逆**。
- **需求对照**：
  - 01-requirements.md:52-70 FR-R-3【P0】"必须区分并**同时保存**源时间戳与接收时间戳" → 违反。
  - 01-requirements.md:39 FR-R-2"同一话题有多个发布者，都要录，且要能区分来源" → instance_key 丢失，无法区分。
  - 任务清单"质量统计：丢帧/倒退/延迟" → 丢帧检测依赖 `Event.seq`（sequence_gaps）、延迟依赖 reception−source，两者数据源均已丢失。
- **QueryRange 同样不回填**：stratum_db.cc:304-309 只填 topic_id/source_timestamp_ns/seq_in_ts/payload。
- **修复**：value 编码改为结构化记录（如 `[seq][reception_ns][payload_len][payload][instance_len][instance]`）并升格式版本（参照 05-known-gaps.md:64-66 的版本升级思路）。
- **待确认**：instance_key 是否属 DB 层职责（TimeKey 20 字节布局不含 instance 维度，time_key.h:18-27），若上层负责则至少 seq/reception 必须入 value。

### High

#### B-H1：CreateRetentionPoint 的快照未被任何读路径使用 → "时间点快照"形同虚设

- **位置**：stratum_db.cc:435-445（创建）、277-278 与 325-326（读路径 ReadOptions 均为默认，`snapshot == nullptr`）。
- **证据**：
  ```cpp
  // stratum_db.cc:439-443
  const LSMengine::Snapshot* snap = db_->GetSnapshot();
  ...
  out->reset(new RetentionPoint(snap, db_));
  ```
  ```cpp
  // stratum_db.cc:325-326（GetStateAt）；QueryRange:277-278 同样
  LSMengine::ReadOptions ro;   // ro.snapshot == nullptr
  std::unique_ptr<LSMengine::Iterator> it(db_->NewIterator(ro));
  ```
  RetentionPoint 只暴露 `raw()`（stratum_db.h:69），**没有任何 API 接受 RetentionPoint 或 Snapshot 做查询**。快照唯一作用是让 `snapshots_` 非空，使 compaction 的 smallest_snapshot（db_impl.cc:907-911）保活旧版本文件——但用户**无法从该时间点读数据**。
- **对照**：options.h:131-133（"snapshot 非空时按指定快照读取"）——DBImpl 支持，StratumDB 未接线。
- **影响**：任务需求"保留点：时间点快照"不成立；用户创建的保留点只是"防 GC 令牌"。

#### B-H2：保留点不跨重启

- **位置**：stratum_db.h:31（注释自认）、snapshot.h:20-39（SnapshotImpl 纯内存）、41-92（SnapshotList 内存链表）。
- **证据**：
  ```cpp
  // stratum_db.h:30-31
  // 保留点 = LevelDB Snapshot（后续可扩展为 MANIFEST 持久化）。
  ```
  LevelDB/LSMengine 快照无任何磁盘形态；重启后 `snapshots_` 为空（db_impl.h:187 成员为纯内存）。
- **影响**：任务需求"保留点：跨重启"**不满足**。按时间点表达保留点的持久化（如把 at_ns 写入 META）完全缺失。

#### B-H3：RetentionPoint 持 DB 裸指针 → DB 先销毁时析构 UAF

- **位置**：stratum_db.h:57-74（RetentionPoint 持有 `LSMengine::DB* db_`）；对照 LSMengine 契约 db.h:26-27。
- **证据**：
  ```cpp
  // stratum_db.h:59-65
  RetentionPoint(const LSMengine::Snapshot* snap, LSMengine::DB* db)
      : snap_(snap), db_(db) {}
  ~RetentionPoint() {
    if (snap_ != nullptr && db_ != nullptr) {
      db_->ReleaseSnapshot(snap_);   // ← db 若已 delete，UAF
    }
  }
  ```
  ```cpp
  // db.h:26-27
  // DB 返回的对象，尤其是 Iterator 和 Snapshot，必须先于 DB 本身销毁。
  ```
  LSMengine 明确要求快照先于 DB 销毁；StratumDB::Close（stratum_db.cc:99-106）直接 `delete db_`，不追踪存活 RetentionPoint，违反契约后 UAF。
- **影响**：正常关闭顺序（先释放保留点）安全，但无任何防护/文档约束。

#### B-H4：质量统计整体缺失（丢帧/倒退/延迟均无法产出）

- **位置**：stratum_db.cc:460-465（GetStats 只有 writes/bytes）；QualityStats 类型已定义但零引用（types.h:91-126）；ITimeline::GetQualityStats 无实现（itimeline.h:163）。
- **证据**：
  ```cpp
  // stratum_db.cc:460-465
  StratumDB::Stats StratumDB::GetStats() {
    Stats stats;
    stats.writes = writes_.load(...);
    stats.total_bytes = bytes_written_.load(...);
    return stats;   // total_keys 从未赋值（见 B-L1）
  }
  ```
  丢帧检测需要 Event.seq（B-C2 已丢失）；延迟需要 reception_ts（B-C2 已丢失）；倒退检测需要与上一事件比较——全部无实现。
- **对照**：db_impl.cc:1404-1466 GetProperty 提供了底层 stats 透传口（"leveldb.stats" 等），StratumDB 连这个也没透传。
- **影响**：任务需求"质量统计：丢帧/倒退/延迟"**不满足**；01-requirements.md:357-370 FR-Q-4 亦不满足。

#### B-H5：保留策略只有手动 DeleteRange，无自动调度（RetentionPolicy 未接入）

- **位置**：types.h:152-156（RetentionPolicy 定义，含 `check_interval_ns`）；itimeline.h:152-156（SetRetentionPolicy/GetRetentionPolicy/RunRetention 接口）；StratumDB 无对应方法（stratum_db.h:79-163）。
- **证据**：StratumDB 只有 `DeleteRange(topic_id, begin, end)`（stratum_db.cc:386-430），**无任何"按时间/空间自动淘汰"的执行路径**；`retain_duration_ns`、`max_bytes` 字段在整个 lsm_v2 目录零引用。
- **对照**：01-requirements.md:128-143 FR-R-7【P0，S1 黑匣子场景核心】"配置只保留最近 N 分钟/N GB，超出部分自动淘汰"。
- **影响**：任务需求"保留策略：按时间淘汰"半满足（有原子删除 API，无策略引擎）；且 DeleteRange 需枚举全 key（05-known-gaps.md:29-36 已知）。

#### B-H6：StratumDB 未实现 ITimeline 接口 → 上层契约断裂

- **位置**：itimeline.h:97-164（接口定义）vs stratum_db.h:79-163（StratumDB 方法集）。
- **证据（签名级差异）**：
  | ITimeline 契约 | StratumDB 现状 |
  |---|---|
  | `QueryRange(topic, TimeRange, EventIteratorPtr*)` 返回迭代器（itimeline.h:131-132） | 返回 `std::vector<Event>*`（stratum_db.h:99-100） |
  | `CreateRetentionPoint(TimestampNs at_ns, uint64_t* point_id)`（itimeline.h:146-147，语义=保护 at_ns 之前的**数据**） | `CreateRetentionPoint(unique_ptr<RetentionPoint>*)`（stratum_db.h:115，语义=LevelDB 当前快照，方向相反） |
  | `GetSessionMetadata/GetTopicMetadata/FindTopicByName`（itimeline.h:122-126） | 均无 |
  | `SetRetentionPolicy/GetRetentionPolicy/RunRetention`（itimeline.h:152-156） | 均无 |
  | `GetQualityStats`（itimeline.h:163） | 无（B-H4） |
  | `Append`（单条，itimeline.h:115） | 无（只有 AppendBatch） |
  | `ReleaseRetentionPoint(uint64_t)`（itimeline.h:149） | 无（RAII 替代，语义可映射） |
- **影响**：Recorder/Player 层无法按契约接入；itimeline.h:15 注释称"Timeline 层负责映射"，但 lsm_v2 这一层不满足该抽象。

### Medium

#### B-M1：GetStateAt 未检查 Iterator::status() → IO 错误伪装成 kNotFound

- **位置**：stratum_db.cc:320-371（全函数无 `it->status()` 调用）；对照同文件 QueryRange:311-313、DeleteRange:410-412、LoadTopics:168-170 均有检查。
- **证据**：iterator.h:61-62（"若发生过错误则返回该错误"）；DBImpl 读错误经 DBIter 传播（db_impl.cc:1026 对 compaction 输入迭代器显式查 status，说明错误可能延迟到 Valid()==false 才可见）。
- **影响**：SSTable 损坏/IO 错误时，GetStateAt 返回 kNotFound 而非错误，上层误判"无数据"。

#### B-M2：GetStateAt 的 `SeekToLast` 兜底分支错误 → 特定场景返回 as_of 之后的数据

- **位置**：stratum_db.cc:341-344。
- **证据**：
  ```cpp
  // stratum_db.cc:334-344
  if (it->Valid()) {
    ...
    if (DecodeEventKey(...) && (t != topic_id || ts > as_of_ns)) {
      it->Prev();
    }
  }
  if (!it->Valid()) {
    it->SeekToLast();   // ← 两个语义混在一起：Seek 超库尾（合理）与 Prev 越界（错误）
  }
  ```
- **机理**：若 Seek 命中的条目是库中**第一条 key 之前没有任何 key**（即该 topic 未注册、无 "META/topic/*" 元数据——B-M3 使该场景可达），且其 ts > as_of，则 Prev() 后 !Valid → SeekToLast 跳到**库尾**。若库尾条目恰属查询 topic，返回该条目（ts 远晚于 as_of）→ **错误返回未来数据**。正确行为应是 kNotFound。
- **可达性**：AppendBatch 不校验话题注册（B-M3），未注册 topic 的事件可先于任何元数据写入；此时该场景必现。
- **DBImpl 对照**：DBImpl 无此模式（它不做时间语义查询）；此逻辑为 StratumDB 自写，需修正为"Prev 后 !Valid → 直接 kNotFound；仅当从未 Seek 成功时（Seek 超库尾）才 SeekToLast"。

#### B-M3：AppendBatch 不校验 topic 已注册 → 可写入孤儿事件

- **位置**：stratum_db.cc:246-249。
- **证据**：
  ```cpp
  // stratum_db.cc:246-249
  auto tit = topic_ids_.find(
      "");  // 需要按 id 反查：注册时保证存在
  (void)tit;
  ```
  注释承认"注册时保证存在"，但代码**没有校验**（find("") 是恒失败的死代码）。没有任何路径检查 `e.topic_id` 是否在 `topic_ids_` 值域中。
- **影响**：未注册 topic 的事件可落盘且可被 QueryRange（按 id）查到，但 ListTopics 不可见 → 语义漏洞；同时是 B-M2 的触发条件之一。

#### B-M4：LSMengine Options 无透传

- **位置**：stratum_db.cc:76-81（Open 只设置 comparator + create_if_missing）。
- **证据**：options.h:55（paranoid_checks）、59（Env）、61（info_log）、73（write_buffer_size）、77（max_open_files）、82（block_cache）、86（block_size）、119（filter_policy）等全部不可配置。db.h:52-53 的 Open 本来接受完整 Options。
- **影响**：录制性能/内存/压缩（compression、zstd level）无法按部署调整；FR-R-6（不干扰业务）的调优手段受限。**待确认**：上层 Recorder 是否计划另开通道配置。

#### B-M5：Flush/Compact 吞错误（bg_error 不可见）

- **位置**：stratum_db.cc:447-458；对照 db.h:128（`void CompactRange`，无返回）、db_impl.cc:640-655（TEST_CompactMemTable 的 bg_error 只经 `Write` 返回）。
- **证据**：
  ```cpp
  // stratum_db.cc:447-452
  Status StratumDB::Flush() {
    ...
    db_->CompactRange(nullptr, nullptr);   // 返回 void
    return Status::OK();                    // 恒 OK
  }
  ```
  db_impl.cc:590 `TEST_CompactMemTable()` 的失败（如 bg_error）在 CompactRange 内部被丢弃（该函数返回 void，db_impl.cc:579-594 无错误出口）。
- **影响**：磁盘满/后台错误时 Flush/Compact 静默"成功"；与 db_impl.cc:458-461 的设计意图（"reflect errors immediately"）相悖。

#### B-M6：ListTopics 不回填 type_name → 类型信息持久化但查询不可见

- **位置**：stratum_db.cc:162（LoadTopics 解出 type_name 后丢弃）、207-218（ListTopics 只填 topic_name/topic_id）。
- **证据**：
  ```cpp
  // stratum_db.cc:161-166
  if (DecodeTopicMeta(it->value().ToString(), &id, &type_name)) {
    topic_ids_[name] = id;   // type_name 被丢弃，topic_ids_ 只存 id
  ```
  ```cpp
  // stratum_db.cc:211-216
  TopicMetadata m;
  m.topic_name = kv.first;
  m.topic_id = kv.second;
  ```
  TopicMetadata 有 type_name/type_definition/qos 字段（types.h:41-50），全部不回填；也无 GetTopicMetadata/FindTopicByName（itimeline.h:123-126）。
- **影响**：任务需求"话题注册/查询：类型信息持久化"**持久化 ✓、查询 ✗**（半满足）。

#### B-M7：Close 无锁无生命周期防护 → 并发操作 UAF

- **位置**：stratum_db.cc:99-106（Close 不持 mutex_、不设防护位后置空）。
- **证据**：
  ```cpp
  // stratum_db.cc:99-106
  Status StratumDB::Close() {
    if (db_ != nullptr) {
      delete db_;
      db_ = nullptr;
    }
    opened_ = false;
  ```
  与 AppendBatch（243-260）、QueryRange（277-278）、CreateRetentionPoint（439）并发时，`delete db_` 与 `db_->Write/NewIterator/GetSnapshot` 竞态 → UAF。`opened_` 是非原子 bool，且检查与使用之间存在 TOCTOU。
- **对照**：DBImpl 析构有完整关闭协议（db_impl.cc:149-156：置 shutting_down_ 并等后台任务完成），但那只保护 DBImpl 内部，不保护 StratumDB 的调用方。

#### B-M8：Close 不 fsync → 正常退出后立刻断电会丢 sync=false 的尾部数据

- **位置**：stratum_db.cc:99-106（Close 直接 delete）；对照 db_impl.cc:149-176（析构不刷 memtable、不 sync WAL）。
- **机理**：
  - Close 时 memtable **不落盘**（db_impl.cc:163 `mem_->Unref()` 丢弃内存），依赖 WAL 恢复（db_impl.cc:382-500 RecoverLogFile）→ **同机重开不丢**（已验证测试 test_lsm_v2_basic.cc:79-93 关闭重开数据完整）。
  - 但 `sync=false` 的 batch（stratum_db.cc:258）从未 fsync（db_impl.cc:1235 仅 `options.sync` 为真才 Sync）；官方语义 options.h:140-148："为 false 时，**机器崩溃可能丢失最近写入**；仅进程崩溃而机器未重启则不丢"。
  - 因此：kill -9（进程级）→ 不丢（页缓存存活）；**机器断电 → 最后一次 fsync 之后的全部 batch 可丢（0~write_buffer 量级，默认 4MB，options.h:73）**，且最后一个 batch 若不完整，被 checksum 丢弃（log_reader.cc:237-245）。
- **需求对照**：01-requirements.md:190（FR-R-10 明确允许 kill -9/断电丢尾部未落盘数据）→ 该行为**符合项目需求**；但与任务清单"崩溃恢复：kill -9 不丢已提交数据"的严格语义存在口径差异——sync=false 的"已提交"仅对进程崩溃成立。**待确认**：任务需求是否要求机器级持久化（若是，AppendBatch 需改 sync=true 或提供 durability 分级，types.h:188-193 的 DurabilityLevel 枚举已预留该分级但未接入）。

### Low

#### B-L1：Stats.total_keys 恒为 0

- **位置**：stratum_db.cc:460-465（GetStats 只填 writes/total_bytes；`total_keys` 定义于 stratum_db.h:128，无任何赋值点）。
- **影响**：统计接口返回恒 0 字段，误导调用方。

#### B-L2：seq_in_ts 计数器 uint32 回绕

- **位置**：stratum_db.cc:223-233；注释 229 行声称"全局递增唯一性**天然成立**"——**不成立**：uint32_t 上限 42.9 亿，per-topic 计数到 0xFFFFFFFF 后 ++ 回绕为 0，key 与 seq=0 的历史条目相同 → 覆盖（与 B-C1 同机理，但无需重启）。
- **对照**：LSMengine 内部序列号是 uint64（db_impl.cc:1224 `last_sequence += Count`），无此问题。**待确认**：S1 黑匣子长期录制单话题 42.9 亿条是否在预算内（大概率不会，但注释断言过强）。

#### B-L3：Flush 以 CompactRange 实现 → 附带全库 compaction 开销

- **位置**：stratum_db.cc:447-452；db_impl.cc:579-594（CompactRange 先刷 memtable 再逐层 compact 到最深）。
- **影响**：Flush 语义正确（db_impl.cc:590 TEST_CompactMemTable 确实先落 memtable），但每次 Flush 触发整库逐层压缩，开销远超"刷 memtable"本意。

#### B-L4：范围查询逐 key ToString() + 全量 vector 拷贝；CountRange 载入全部 payload

- **位置**：stratum_db.cc:291-292（每次迭代两次 `it->key().ToString()`）、308（`it->value().ToString()` 拷贝）；376-384（CountRange 复用 QueryRange 只为计数）。
- **影响**：大范围查询/统计的 CPU 与内存开销线性放大；对比 itimeline.h:131-132 的迭代器契约（流式）更优。05-known-gaps.md:85-93 记录了同族问题（Player 全量载入）。

#### B-L5：死代码/死声明

- **位置**：stratum_db.cc:25（`kMetaSession` 未使用）、131-133（`MetaKey` 未使用）、246-249（`find("")` 死代码）；stratum_db.h:136（`RegisterTopicLocked` 声明但 .cc 无实现——若被调用将链接失败）。
- **影响**：无运行时影响，但 RegisterTopicLocked 的悬空声明是维护陷阱。

#### B-L6：DeleteRange 语义毛边

- **位置**：stratum_db.cc:416-417（范围为空返回 `Status::NotFound`，把"无事可做"当错误）；394-413（收集段无锁——收集窗口内新写入的范围内事件不会被删除，属语义窗口而非 bug，LSMengine 无 range delete 所致，05-known-gaps.md:29-36 已知）。
- **影响**：保留策略调用方需容忍 NotFound；并发窗口需文档化。

#### B-L7：RegisterTopic 幂等不校验类型冲突

- **位置**：stratum_db.cc:180-184（同名命中直接返回旧 id，不比较 meta.type_name）。
- **影响**：同名不同类型（如 DDS 域中同名话题改类型）静默复用旧 id，回放时类型错配。

#### B-L8：bytes_written_ 在写失败时已计入

- **位置**：stratum_db.cc:253（锁内 `fetch_add`，早于 260 行 `db_->Write`）。
- **影响**：Write 失败时 GetStats.total_bytes 虚高。

#### B-L9：seq 分配与 db_->Write 顺序错位（锁外 Write）

- **位置**：stratum_db.cc:243-260（seq 分配在锁内，Write 在锁外）。
- **机理**：两个并发 AppendBatch 的 seq 分配序与 LSMengine writers_ 入队序可能交叉：批次 A 分配 seq 0..9 后解锁，批次 B 分配 seq 10..19 并先完成 Write。GetStateAt 按"stored 最小"返回 seq 最大条目，即**分配序**最新——与"最后完成写"可能不一致。因 seq 序即全序定义，语义自洽；失败批次的 seq 形成空洞（不影响正确性，GetStateAt 不需要连续）。**待确认**：上层是否依赖 WAL 写入序与 seq 序一致（若依赖需把 Write 移入锁内）。
- 另：锁内分配时若 Write 失败，已分配 seq 不回滚（无害，见上）。

#### B-L10：GetProperty/GetApproximateSizes/DB::Get 未透传

- **位置**：db.h:109-119（底层属性/大小估算 API）；stratum_db.h:79-163 无对应方法。
- **影响**：运维侧无法取 compaction 统计与磁盘估算；空间淘汰（FR-R-7 按 N GB）缺少估算依据。

---

## C. 需求覆盖矩阵（任务清单 9 项）

| 需求 | 是否满足 | 缺口与证据 |
|---|---|---|
| 话题注册/查询（幂等、类型信息持久化） | **部分满足** | 幂等 ✓（stratum_db.cc:180-184）；类型持久化 ✓（44-48 编码、191-194 同批原子落盘、LoadTopics 162 解码）；**查询不回填 type_name**（B-M6）；缺 GetTopicMetadata/FindTopicByName（itimeline.h:123-126） |
| 批量追加（乱序时间戳、同 ts 多条） | **部分满足** | 乱序 ✓（key 按 (topic,ts,seq) 排序，附录 E-1/E-2）；同 ts 多条 ✓（单进程内 seq_in_ts 递增唯一，test_lsm_v2_basic.cc:62-77 实测 50 条同 ts）；**跨重启同 ts 追加覆盖**（B-C1）；seq/reception/instance 字段丢失（B-C2）；未注册 topic 可写（B-M3） |
| 时间范围查询（[begin,end) 半开） | **满足** | begin=EventKey(topic,begin,0xFFFFFFFF)、end=EventKey(topic,end,0xFFFFFFFF)、`key>=end_key` break（stratum_db.cc:284-293）；数字验证附录 E-2：ts=end 整段排除 ✓；跨 topic break（301-303）✓；Seek 定位 O(log n)+顺序扫结果集，满足 FR-Q-2"不能全表扫描"（01-requirements.md:331） |
| 时间点查询（as_of 或之前最新值、O(log n)） | **部分满足** | 算法正确性：Seek((topic,as_of,stored=0)) 命中 ≥as_of 的最小 ts，Prev 一次必落在 ≤as_of 的最大 ts（附录 E-4 场景 c/d/e/f 全过）；O(log n) ✓（skiplist+索引 Seek）；**缺陷**：无 status 检查（B-M1）、Prev 越界兜底错误（B-M2）、dispose/out-of-range 语义未实现（itimeline.h:79-83 kDisposed/kOutOfRange，FR-Q-3 四种情况 01-requirements.md:346-352 只覆盖前两种） |
| 保留策略（按时间淘汰） | **部分满足** | DeleteRange 原子批量 tombstone+sync（stratum_db.cc:419-425）✓；**无自动调度**：RetentionPolicy/check_interval_ns/RunRetention 全缺失（B-H5）；DeleteRange 需枚举全 key（05-known-gaps.md:29-36）；空范围返回 NotFound 语义怪（B-L6） |
| 保留点（时间点快照、跨重启） | **不满足** | 快照可创建（stratum_db.cc:439）但读路径零使用（B-H1）；LevelDB 快照纯内存不跨重启（B-H2）；db 先销毁 UAF（B-H3）；与 ITimeline 的 at_ns 语义方向相反（B-H6） |
| 质量统计（丢帧/倒退/延迟） | **不满足** | GetStats 仅 writes/bytes（stratum_db.cc:460-465）；QualityStats 类型零引用（types.h:91-126）；GetQualityStats 未实现（itimeline.h:163）；检测所需数据（seq/reception_ts）已在写入时丢弃（B-C2） |
| 崩溃恢复（kill -9 不丢已提交数据） | **部分满足** | WAL+MANIFEST 恢复自动继承 ✓（A8）；sync=true 写入（注册/删除）fsync ✓；sync=false 批量追加官方语义"进程崩溃不丢、机器崩溃丢"（options.h:140-148）→ kill -9 满足、断电不满足（B-M8，符合 FR-R-10 的口径）；**恢复后继续写会覆盖历史**（B-C1，因 seq 计数不持久化） |
| 并发（边录边放） | **部分满足** | DB 线程安全 + 写队列 + 迭代器隐式快照自动继承 ✓（A1/A10/A11）；**Close 无防护 UAF**（B-M7）；seq 分配与 WAL 序错位（B-L9）；DeleteRange 收集窗口（B-L6）；多线程写从未测试（05-known-gaps.md:136 自认） |

**附：docs 中与本层相关但任务清单未列出的 P0 缺口**
- FR-R-3 双时间戳【P0】：reception_ts 未落盘（B-C2）。
- FR-R-2 多发布者区分来源：instance_key 未落盘（B-C2）。
- FR-R-7 环形保留【P0】：无自动淘汰（B-H5）。
- FR-Q-3 四种状态语义：kDisposed/kOutOfRange 未实现（C 表第 4 行）。

---

## D. 修复优先级排序

| 优先级 | 项 | 理由 | 修复要点 |
|---|---|---|---|
| **P0-1** | B-C2：value 编码补 seq/reception_ts/instance_key | 数据一旦写入即永久丢失，**不可逆**；P0 需求 FR-R-3/FR-R-2 直接违反 | 定义 value 结构（长度前缀+字段），升 kFormatVersion；写入/查询两端同步 |
| **P0-2** | B-C1：seq_in_ts 计数持久化 | 静默覆盖历史数据，无任何报错；项目已知未修（05-known-gaps.md:51-68） | 每 topic 高水位写入 META（如 `META/seq/<topic_id>`），**必须在 AppendBatch 的同一 WriteBatch 内更新**（同批原子，崩溃一致；否则崩溃时高水位落后仍会覆盖）；Open 时 LoadTopics 一并恢复。注意高水位无法从 key 空间反推（seq 全局递增但 ts 主导排序，topic 首条 key 的 seq 不一定是全局最大），必须显式持久化 |
| **P1-1** | B-H1+H2+H3：保留点全链路 | 需求"保留点"整体不成立 | 读 API 接受 RetentionPoint；保留点按 at_ns 持久化到 META；RetentionPoint 持 StratumDB 弱引用/由 StratumDB 登记并随 Close 失效 |
| **P1-2** | B-H4：质量统计 | 需求项整体缺失，且依赖 P0-1 的数据 | 实现 QualityStats 累积（AppendBatch 内比较 seq/ts），GetQualityStats 对接 |
| **P1-3** | B-H5：保留策略自动调度 | FR-R-7 为 P0（S1 黑匣子核心） | 后台线程按 check_interval_ns 调 DeleteRange；接入 max_bytes 空间淘汰（可用 db_->GetApproximateSizes，db.h:118） |
| **P1-4** | B-H6：对齐 ITimeline 接口 | 上层无法按契约接入 | 补齐方法签名/语义（或修订 itimeline.h 并同步上层） |
| **P2-1** | B-M1+M2：GetStateAt 状态检查与兜底修正 | 错误结果与错误掩盖，属正确性 | 补 it->status() 检查；Prev 越界直接 kNotFound |
| **P2-2** | B-M3：AppendBatch 校验话题注册 | 孤儿事件 + M2 触发条件 | 建立 id→name 反查表或在注册时缓存 topic_id 集合 |
| **P2-3** | B-M7：Close 生命周期防护 | UAF | 用共享互斥锁包裹 db_ 访问或引入引用计数/close 标志 |
| **P2-4** | B-M4/M5/M8：参数透传、Flush 错误暴露、关闭语义 | 运维与调优 | Open 增加 Options 参数；Flush/Compact 用 TEST 语义或 GetProperty 检查 bg 状态；明确 durability 分级（types.h:188-193 已有枚举） |
| **P3** | B-L1~L10 | 统计正确性、性能、死代码清理 | 按 B 清单逐项处理 |

---

## E. 附录：seq_in_ts 反转编码与 Seek/Prev 边界的数字验证

以下结论由脚本按 TimeKeyCodec（time_key.cc:24-35）与 StratumDB::EventKey（stratum_db.cc:111-117）的精确语义模拟得出（magic=0x53545231，全字段大端，20 字节定长）。

**E-1 同 ts 排序**：topic=5, ts=100（0x64），seq_in_ts=0/1/2：
```
seq=0 → stored=0xffffffff → 53545231 00000005 0000000000000064 ffffffff
seq=1 → stored=0xfffffffe → ...64 fffffffe
seq=2 → stored=0xfffffffd → ...64 fffffffd
```
字节序排序后**第一条是 seq=2**（最大 seq 排最前）✓，与 stratum_db.cc:113-115 注释一致。

**E-2 QueryRange [100,200) 半开**：begin_key=(5,100,stored=0)，end_key=(5,200,stored=0)。遍历到 ts=200 的第一条（stored=0xffffffff ≥ 0）即 break → 结果恰为 ts=100 的两条，**ts=200 整段排除** ✓。end_ns==kInvalidTimestamp（=0，types.h:30）时不比较 end_key（stratum_db.cc:292）→ 无上界 ✓。

**E-3 GetStateAt(5,150) 回退**：库含 (5,100,seq0→stored 0xffffffff)、(5,100,seq1→0xfffffffe)、(5,200,seq0→0xffffffff)。Seek((5,150,0)) 命中 (5,200,stored=0xffffffff,seq=0)；Prev() 一次 → (5,100,stored=0xfffffffe,seq=1)，ts=100≤150 ✓ 返回 100 时刻最新值。严格论证：命中 ts = 最小有数据的 ts ≥ as_of；Prev 一次必落在 < 该 ts 的最大 ts，因此 ≤ as_of 或跨 topic/元数据 → **一次 Prev 足够，无"Prev 不足"缺陷**。

**E-4 边界矩阵**（模拟器输出）：
| 场景 | 结果 | 正确性 |
|---|---|---|
| 库空 | kNotFound | ✓ |
| 只有元数据（"META/..."，首字节 0x4D < magic 0x53，恒排在事件区之前） | SeekToLast 落到元数据 → Decode 失败 → kNotFound | ✓ |
| as_of 恰有数据 | 返回该时刻最大 seq | ✓ |
| as_of 无数据、前后有 | 返回 as_of 之前最大 ts | ✓ |
| as_of 早于全部数据 | kNotFound | ✓ |
| as_of 超库尾 | SeekToLast → 返回最新值 | ✓（与 FR-Q-3"T 晚于录制结束返回不存在"的差异属上层语义，**待确认**是否需 DB 层返回 kOutOfRange） |
| topic 不存在 | Prev 落到他 topic → kNotFound | ✓ |

**E-5 重启碰撞**：重启前后同 (topic=7,ts=100,seq=0) 的 key 完全相同（`53545231 00000007 ...64 ffffffff`）→ LSMengine 按新 seq 覆盖旧值 → 旧数据不可见（B-C1）。

**E-6 LoadTopics 前缀上限**："META/topic/" 的 limit="META/topic0"，第 11 字节 '/'（0x2F）< '0'（0x30），故任意 topic 名（含空串、'0' 开头）都 < limit；"META/topica" ≥ limit 越界停 ✓（stratum_db.cc:151-156 构造正确）。

**E-7 元数据 value**：topic_id=7、type="std_msgs/msg/String" → `00000007 00000013 7374645f6d7367732f6d73672f537472696e67`（27B）✓ 与 Encode/DecodeTopicMeta（stratum_db.cc:44-58）一致。

**E-8 WriteBatch 生命周期**：`batch.Put(std::string(...), ...)` 传入临时 std::string 的 Slice 安全——write_batch.cc:101-106 Put 立即 `PutLengthPrefixedSlice(&rep_, key/value)` 序列化复制，临时对象析构不影响 batch ✓。

**E-9 字符串比较与 comparator 一致性**：QueryRange 的 `it->key().ToString() >= end_key`（stratum_db.cc:292）用 std::string 字典序，与 TimeKeyComparator 的 memcmp（time_key_comparator.cc:13-27）同序 ✓，不会产生边界误判。

---

## F. 与项目既有记录的关系

- 05-known-gaps.md:51-68（seq_in_ts 未持久化）、29-36（DeleteRange 枚举全 key）为项目**已知**；本报告 B-C1/B-L6 独立复核确认，lsm_v2 重写未修复前者。
- 05-known-gaps.md:136（多线程写未测试）与本报告 B-L9/B-M7 互为印证。
- 05-known-gaps.md:70-79 描述的"GetStateAt 正向扫描"问题**在 lsm_v2 已不存在**（stratum_db.cc:329-341 已是 Seek+Prev，测试 test_lsm_v2_basic.cc:52-59 覆盖该路径），确认该项已解决。
