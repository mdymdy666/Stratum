# 参考实现精读：Version 机制与 DBImpl 生命周期（LSMengine）

> 源码位置：`/UserData/LeaningLevelDB/LSMengine`（LevelDB 的完整翻版，命名空间 `LSMengine`，本 repo 将上游的
> 全局常量 `kNumLevels` 等改为 `config::kNumLevels` 命名空间常量）。
> 本文所有行号均为该 repo 当前源码的实际行号；文件名以 repo 根为基准的相对路径。
> 已核对的事实用「文件:行号」标注；无法从代码直接确认的推断标注 **待确认**。

---

## 1. VersionEdit 的磁盘编码

### 1.1 Tag 数值表（写入磁盘、不得更改）

`db/version_edit.cc:16-26`：

```cpp
enum Tag {
  kComparator = 1,
  kLogNumber = 2,
  kNextFileNumber = 3,
  kLastSequence = 4,
  kCompactPointer = 5,
  kDeletedFile = 6,
  kNewFile = 7,
  // 数值 8 曾用于 large value reference。
  kPrevLogNumber = 9
};
```

| Tag | 数值 | payload 布局（tag 之后） |
|---|---|---|
| kComparator | 1 | length-prefixed string：`varint32(长度) + 字节` |
| kLogNumber | 2 | `varint64` |
| kPrevLogNumber | 9 | `varint64` |
| kNextFileNumber | 3 | `varint64` |
| kLastSequence | 4 | `varint64` |
| kCompactPointer | 5 | `varint32(level)` + length-prefixed InternalKey |
| kDeletedFile | 6 | `varint32(level)` + `varint64(文件号)` |
| kNewFile | 7 | `varint32(level)` + `varint64(文件号)` + `varint64(file_size)` + length-prefixed smallest InternalKey + length-prefixed largest InternalKey |

- **数值 8 已废弃**（历史上用于 large value reference），不可复用于新语义——与 LevelDB 兼容性相关，见 §13.3。
- InternalKey 的编码 = `user_key + 8 字节 (sequence<<8 | type)` 小端定长（`db/dbformat.h:164-174` 的 `ParseInternalKey` 逆运算；
  `kValueTypeForSeek = kTypeValue = 0x1`，`kMaxSequenceNumber = 2^56-1`，`db/dbformat.h:52-61`）。

### 1.2 EncodeTo 的输出顺序

`db/version_edit.cc:44-87`：

```cpp
void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_comparator_) { PutVarint32(dst, kComparator); PutLengthPrefixedSlice(dst, comparator_); }
  if (has_log_number_) { PutVarint32(dst, kLogNumber); PutVarint64(dst, log_number_); }
  if (has_prev_log_number_) { PutVarint32(dst, kPrevLogNumber); PutVarint64(dst, prev_log_number_); }
  if (has_next_file_number_) { PutVarint32(dst, kNextFileNumber); PutVarint64(dst, next_file_number_); }
  if (has_last_sequence_) { PutVarint32(dst, kLastSequence); PutVarint64(dst, last_sequence_); }
  for (...) { PutVarint32(dst, kCompactPointer); PutVarint32(dst, level); PutLengthPrefixedSlice(dst, key.Encode()); }
  for (const auto& deleted_file_kvp : deleted_files_) {   // std::set → 按 (level, number) 升序
    PutVarint32(dst, kDeletedFile); PutVarint32(dst, level); PutVarint64(dst, number); }
  for (...) { // new_files_ 保持插入顺序
    PutVarint32(dst, kNewFile); PutVarint32(dst, level); PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size); PutLengthPrefixedSlice(dst, f.smallest.Encode());
    PutLengthPrefixedSlice(dst, f.largest.Encode()); }
}
```

要点：**一个 edit 就是"一组字段 + 文件增量列表"**，不是完整快照；`deleted_files_` 是
`std::set<std::pair<int,uint64_t>>`（`db/version_edit.h:90,104`），因此删除项天然去重且按 (level, number) 排序输出。

### 1.3 DecodeFrom 的容错

`db/version_edit.cc:108-206`：

- 入口先 `Clear()`（108-109），**出错时返回 `Status::Corruption("VersionEdit", msg)`**（201-205），调用方不得部分应用。
- 每个字段解析失败给出**指名错误**：`"comparator name"` / `"log number"` / `"previous log number"` /
  `"next file number"` / `"last sequence number"` / `"compaction pointer"` / `"deleted file"` /
  `"new-file entry"` / 未知 tag → `"unknown tag"`（191-193）。
- **未知 tag 是致命错误**（`default: msg = "unknown tag"`）——没有"跳过未知字段"的向前兼容机制。这一点对 §13.3
  加新 tag 很重要：新旧二进制不兼容，必须同步升级编解码器。
- `GetLevel` 强制 `level < config::kNumLevels`（98-106），越界 level 视为损坏。
- `GetInternalKey`（89-96）：要求 length-prefixed slice 存在且 `InternalKey::DecodeFrom` 成功；
  `InternalKey::DecodeFrom` 对**空串返回 false**（`db/dbformat.h:137-140`），即空 InternalKey 被视为损坏。
- 循环出口：`GetVarint32` 读到输入耗尽（干净 EOF）正常结束；若输入还有残留但读不出完整 varint →
  `"invalid tag"`（197-199）。
- 边界行为：解析中途失败时 edit 可能已填充部分字段，但 `VersionSet::Recover` 只在 `s.ok()` 时才 `builder.Apply(&edit)`
  （`db/version_set.cc:909-911`），并且 Recover 遇错直接整体失败，**不存在"跳坏记录继续恢复"**（见 §6）。

---

## 2. MANIFEST 的物理格式

### 2.1 命名

`db/filename.cc:46-52`：

```cpp
std::string DescriptorFileName(const std::string& dbname, uint64_t number) {
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu", ...);
  return dbname + buf;
}
```

即 `MANIFEST-000002`、`MANIFEST-000003`…… 编号复用全局文件号空间（见 §4）。

### 2.2 一条 VersionEdit = 一条 log record

MANIFEST 复用 WAL 的 log writer。`VersionSet::LogAndApply` 中（`db/version_set.cc:809-812`）：

```cpp
std::string record;
edit->EncodeTo(&record);
s = descriptor_log_->AddRecord(record);
```

物理格式（`db/log_format.h:17-33`、`db/log_writer.cc:37-110`）：

- 每条逻辑 record 被拆成若干**物理 record**，7 字节 header + payload：
  `CRC32C(4B, masked) + length(2B, little-endian) + type(1B)`；
  type 为 `kFullType=1 / kFirstType=2 / kMiddleType=3 / kLastType=4`（`kZeroType=0` 保留给预分配）。
- 固定 **32KB block**（`kBlockSize = 32768`）；block 尾部不足 7 字节时填零 trailer（`db/log_writer.cc:48-56`）。
- CRC 覆盖 **type 字节 + payload**（`db/log_writer.cc:96-98`），每条物理 record 写入后 `Flush()`（105）。
- Reader 校验 CRC；损坏时**丢弃本 block 剩余部分、跳到下一个 32KB 边界重新同步**（`db/log_reader.cc:236-248`），
  并回调 `Reporter::Corruption`。对 MANIFEST，Recover 把任何 corruption 视为致命（见 §6）。

### 2.3 MANIFEST 的"首条记录"是全量快照

新建 MANIFEST 时先写一条 **WriteSnapshot** 记录（`db/version_set.cc:1051-1079`）：Comparator + 全部
compact pointer + 当前 Version 的全部文件（AddFile 列表）。之后的所有 edit 都是**增量**。
新 MANIFEST 的创建时机：`LogAndApply` 首次调用（descriptor_log_ 为空，`db/version_set.cc:792-802`）；
或 Recover 后 `ReuseManifest` 判定不满足复用条件（`db/version_set.cc:981-1010`：`reuse_logs` 关闭、
MANIFEST 大小 ≥ `options->max_file_size` 等）→ 下次 LogAndApply 以新编号建新 MANIFEST。

### 2.4 MANIFEST 写入的原子性（env 层）

`util/env_posix.cc` 的 `PosixWritableFile::Sync()`（328-343）：

```cpp
Status Sync() override {
  // 确保 manifest 引用的新文件已经存在于文件系统。
  // 必须在 manifest 文件刷到磁盘前完成，以免崩溃后 manifest 已引用尚未落盘的文件。
  Status status = SyncDirIfManifest();   // 367-381：仅 MANIFEST 文件：fsync 其所在目录
  if (!status.ok()) return status;
  status = FlushBuffer();                // 用户态 64KB 缓冲刷出（kWritableFileBufferSize=65536，line 62）
  if (!status.ok()) return status;
  return SyncFd(fd_, filename_);         // 386-405：fdatasync（无则 fsync）
}
```

- `is_manifest_` 由文件名 basename 前缀 `MANIFEST` 判定（437-439）。
- 先 fsync **目录**再 fsync 文件：保证 MANIFEST 引用的新 .ldb/.sst 文件的目录项已持久化——否则崩溃后可能出现
  "MANIFEST 引用了不存在的表文件"。
- 顺序总结：AddRecord（含 Flush）→ `Sync()`（目录 fsync → 数据 fsync）→ 才轮得到 CURRENT 切换（§3）。

---

## 3. CURRENT 文件

### 3.1 内容与 SetCurrentFile

`db/filename.cc:125-141`：

```cpp
Status SetCurrentFile(Env* env, const std::string& dbname, uint64_t descriptor_number) {
  // 移除开头的 "dbname/"，并为 manifest 文件名添加换行符。
  std::string manifest = DescriptorFileName(dbname, descriptor_number);
  Slice contents = manifest;
  contents.remove_prefix(dbname.size() + 1);
  std::string tmp = TempFileName(dbname, descriptor_number);      // dbname/NNNNNN.dbtmp
  Status s = WriteStringToFileSync(env, contents.ToString() + "\n", tmp);
  if (s.ok()) {
    s = env->RenameFile(tmp, CurrentFileName(dbname));            // rename → dbname/CURRENT
  }
  if (!s.ok()) env->RemoveFile(tmp);
  return s;
}
```

- **内容 = 一行相对文件名 + '\n'**，例如 `MANIFEST-000002\n`。
- `WriteStringToFileSync` = `NewWritableFile → Append → Sync → Close`，任一步失败删除文件
  （`util/env.cc:51-80`）。
- POSIX `RenameFile` 即 `std::rename`（`util/env_posix.cc:632-637`）。

### 3.2 原子性论证

- 先写**临时文件**（`.dbtmp`）并 **fsync**，再 `rename` 到 CURRENT：`rename(2)` 原子，读者永远看到
  旧 CURRENT 或新 CURRENT，**不会看到半截内容**。
- Recover 侧还会校验内容以 '\n' 结尾（`db/version_set.cc:862-864`），进一步防御 torn write。
- 崩溃窗口分析：LogAndApply 先 fsync MANIFEST 再切换 CURRENT（§7），所以无论 rename 是否持久，
  崩溃后要么旧 CURRENT+旧 MANIFEST（丢最新一次 edit，状态一致），要么新 CURRENT+新 MANIFEST（状态一致）。
- **待确认**：rename 之后没有对目录做 fsync（目录 fsync 只发生在 MANIFEST 的 `Sync()` 里，且早于 rename），
  理论上断电可能让 CURRENT 回退到旧内容——无害（回到旧的但一致的状态），这是 LevelDB 原版同样的取舍。

---

## 4. VersionSet 数据结构

`db/version_set.h:276-295`：

```cpp
WritableFile* descriptor_file_;   // 延迟打开
log::Writer* descriptor_log_;     // 延迟打开
Version dummy_versions_;          // Version 双向环形链表的头哨兵。
Version* current_;                // == dummy_versions_.prev_
std::string compact_pointer_[config::kNumLevels];
```

- **`dummy_versions_`**：Version 循环双向链表的哨兵头（`Version` 内 `next_/prev_`，`db/version_set.h:141-142`）。
  `AppendVersion` 把新 Version 追加到链表尾（dummy 的 prev 侧）并设为 current，同时 Unref 旧 current
  （`db/version_set.cc:748-763`）。旧 Version 只要还有外部 refs（迭代器/Compaction/Get）就留在链表里，
  见 §5、§8、§12。
- **Version 链表**的意义：`AddLiveFiles` 遍历**整条链表**收集活文件（`db/version_set.cc:1129-1139`），
  因此任何被读者引用的历史文件集合都不会被 GC。
- **`next_file_number_`** 是文件号分配器，初始 2（1 号给首个 MANIFEST，`db/version_set.cc:721-739`）；
  `NewFileNumber()` 自增（184）、`MarkFileNumberUsed()`（1012-1016）、`ReuseFileNumber()`（188-192）。
- **`compact_pointer_[level]`**：每层"下次 compaction 从哪个 key 之后开始"，用于 size 触发的 compaction 轮转
  （`PickCompaction`，§8.1）；在 `SetupOtherInputs` 里**立即**更新并写入 edit（`db/version_set.cc:1410-1414`）。
- **`Builder`**（内部类，`db/version_set.cc:561-719`）：
  - 作用：把若干 edit 施加到 base Version 上，**不物化中间 Version**；每个 level 维护
    `deleted_files` 集合 + `added_files`（`FileSet`，按 smallest internal key 排序、文件号打破平局，564-576）。
  - `Apply(edit)`（621-658）：更新 compact_pointer_；收集删除；新增文件时 `new FileMetaData`、
    置 `refs=1`、按 `file_size/16384`（下限 100）设置 `allowed_seeks`（652-653）、并从 deleted 集合移除同号文件（655）。
  - `SaveTo(v)`（661-703）：把 base 文件与 added 文件按 smallest key 做 upper_bound 归并，跳过已删文件，
    `MaybeAddFile`（705-718）给每个文件 `refs++` 后放入 `v->files_[level]`；debug 构建下断言 level>0 不重叠（687-701）。
  - 构造函数 `base_->Ref()`（590-591），析构释放所有 added 文件引用与 base 引用（599-618）。
- `manifest_file_number_` / `log_number_` / `prev_log_number_` / `last_sequence_`：从 MANIFEST 恢复出的
  持久化元数据（§6）。

---

## 5. Version 类

`db/version_set.h:57-156`。

### 5.1 files_ 的表示与排序

```cpp
std::vector<FileMetaData*> files_[config::kNumLevels];  // version_set.h:146
```

- **所有层**（含 L0）在 `Builder::SaveTo` 后都按 **smallest internal key 升序**排列（平局按文件号），
  因为归并比较器 `BySmallestKey` 对每层统一使用（`db/version_set.cc:564-576, 661-703`）。
  - ⚠️ 与任务描述"level 0 按文件号排序"**不一致**：本实现 files_[0] 按 smallest key 排序；
    "按文件号"只出现在**点查路径**——`ForEachOverlapping` 把与 user_key 重叠的 L0 文件临时拷贝后按
    **文件号降序**（`NewestFirst`，`db/version_set.cc:273-275, 282-298`）排序，保证新文件优先被搜到。
- **level > 0 的约束**：同层文件 key 区间互不重叠（debug 断言 `db/version_set.cc:687-701`；
  归并时也有断言 710-714）。L0 允许重叠。
- `FileMetaData`（`db/version_edit.h:21-30`）：`refs`（该文件被多少个 Version 引用）、`allowed_seeks`
  （初始 `1<<30`，seek 触发 compaction 的预算）、`number`、`file_size`、`smallest`、`largest`。

### 5.2 refs 计数

```cpp
void Version::Ref() { ++refs_; }                 // 447
void Version::Unref() {                          // 449-456
  assert(this != &vset_->dummy_versions_);
  --refs_;
  if (refs_ == 0) delete this;
}
```

`~Version`（68-86）：从链表摘除；对每个被引用文件 `f->refs--`，归零即 `delete f`。
持有 ref 的方：`AppendVersion`（current 本身）、`NewInternalIterator`（`db/db_impl.cc:1097`，迭代器析构时
`CleanupIteratorState` 释放，1068-1076）、`Get`（1135/1161）、`GetApproximateSizes`（1471-1483）、
`PickCompaction/CompactRange` 的 `input_version_`（`db/version_set.cc:1263-1264, 1441-1442`）。

### 5.3 AddIterators 的合并逻辑

`db/version_set.cc:226-241`：

```cpp
void Version::AddIterators(const ReadOptions& options, std::vector<Iterator*>* iters) {
  // 合并所有 L0 文件，因为它们可能相互重叠。
  for (size_t i = 0; i < files_[0].size(); i++) {
    iters->push_back(vset_->table_cache_->NewIterator(options, files_[0][i]->number,
                                                      files_[0][i]->file_size));
  }
  // 对于大于 0 的层级，可以使用串接迭代器顺序遍历该层互不重叠的文件。
  for (int level = 1; level < config::kNumLevels; level++) {
    if (!files_[level].empty()) iters->push_back(NewConcatenatingIterator(options, level));
  }
}
```

- L0：**每个文件一个独立 table 迭代器**（必须全合并，因为互相重叠）。
- L>0：`NewConcatenatingIterator`（219-224）= `TwoLevelIterator`：一级迭代器是 `LevelFileNumIterator`
  （160-205，key()=文件 largest，value()=文件号+大小 16 字节，Seek 用 `FindFile` 二分，88-104），
  二级用 `GetFileIterator`（207-217）经 TableCache 惰性打开具体文件。
- 真正的归并发生在调用方：`DBImpl::NewInternalIterator` 把 memtable、imm、`versions_->current()->AddIterators`
  的全部迭代器交给 `NewMergingIterator(&internal_comparator_, ...)`（`db/db_impl.cc:1087-1096`）。

---

## 6. 恢复流程（VersionSet::Recover）

`db/version_set.cc:848-979`。步骤：

1. **读 CURRENT**：`ReadFileToString`（857-861）；文件不存在 → 直接返回该错误（由 `DBImpl::Recover` 处理
   create_if_missing，§11）；内容为空或末字符非 '\n' → `Corruption("CURRENT file does not end with newline")`
   （862-864）；去掉末尾 '\n' 得 manifest 相对名。
2. **打开 MANIFEST**：`dscname = dbname_ + "/" + current`（867）；`NewSequentialFile` 失败且是 NotFound →
   `Corruption("CURRENT points to a non-existent file")`（869-876）。
3. **逐条读 log record**（889-933）：`log::Reader` 带 checksum；每条 record `VersionEdit::DecodeFrom`；
   检查 comparator 名一致（901-907）；`builder.Apply(&edit)`（909-911）；**每条 edit 的
   log_number/prev_log_number/next_file_number/last_sequence 以"最后出现者"为准**累进记录（913-931）。
   注意 Builder 基于 `current_`（此时是构造时挂入的空 Version）。
4. **校验必需元数据**（937-952）：缺 next_file / log_number / last_sequence → Corruption；
   prev_log_number 缺省 0；`MarkFileNumberUsed` 把 log 号占位（950-951）。
5. **物化最终 Version**（954-964）：`builder.SaveTo(v)` → `Finalize(v)`（算 compaction score，§8.1）→
   `AppendVersion(v)`；随后恢复 `manifest_file_number_=next_file`、`next_file_number_=next_file+1`、
   `last_sequence_/log_number_/prev_log_number_`。
6. **MANIFEST 复用判定**（966-971）：`ReuseManifest`（981-1010）满足（reuse_logs 开、文件小）→ 以
   `NewAppendableFile` + `Writer(file, size)` 继续追加；否则 `*save_manifest = true`，下次 LogAndApply
   新建 MANIFEST 并写全量快照。

### 错误处理（MANIFEST 损坏怎么办）

- Reader 遇到坏 record 时回调 `LogReporter::Corruption` → 设置 `s`（849-854）；读循环条件是
  `reader.ReadRecord(...) && s.ok()`（896），因此**任何一处 corruption 都使 Recover 立即以该错误失败**，
  日志只打印 `"Error recovering version set with %d records"`（973-975）→ `DB::Open` 失败。
- 这与 WAL 重放不同：WAL 中坏 record 可被跳过（§11 的 `RecoverLogFile`），MANIFEST 则 **fail-stop**，
  因为跳过一个 edit 会导致文件集合/序号状态错乱。
- `ReadRecord` 在 corruption 时会自动跳到下一个 32KB 边界（`db/log_reader.cc:150-156, 236-248`），
  但这只影响"损坏报告范围"，不改变 Recover 整体失败的结论。
- 外部保护：`DBImpl::Recover` 还会校验所有 live 文件实际存在（`db/db_impl.cc:341-358`：把目录里解析出的
  文件从 `expected`（=AddLiveFiles 结果）中划掉，非空即 `"N missing files; e.g."` Corruption）。

---

## 7. LogAndApply：精确顺序与失败语义

`db/version_set.cc:765-846`。前置条件：持 `*mu` 进入；无并发 LogAndApply（header 169-170 注释）。

```cpp
Status VersionSet::LogAndApply(VersionEdit* edit, port::Mutex* mu) {
  if (edit->has_log_number_) { assert(...); }
  else edit->SetLogNumber(log_number_);                        // 766-771
  if (!edit->has_prev_log_number_) edit->SetPrevLogNumber(prev_log_number_);  // 773-775
  edit->SetNextFile(next_file_number_);                        // 777：持久化文件号水位
  edit->SetLastSequence(last_sequence_);                       // 778：持久化 seq 水位

  Version* v = new Version(this);                              // 780：内存中构建新 Version
  { Builder builder(this, current_); builder.Apply(edit); builder.SaveTo(v); }
  Finalize(v);                                                 // 786：算 score

  if (descriptor_log_ == nullptr) {                            // 792-802：首次调用才建新 MANIFEST
    new_manifest_file = DescriptorFileName(dbname_, manifest_file_number_);
    s = env_->NewWritableFile(new_manifest_file, &descriptor_file_);
    if (s.ok()) { descriptor_log_ = new log::Writer(descriptor_file_);
                  s = WriteSnapshot(descriptor_log_); }        // 先写全量快照（未 fsync）
  }

  { mu->Unlock();                                              // 804：磁盘 I/O 期间放锁
    if (s.ok()) {
      std::string record; edit->EncodeTo(&record);
      s = descriptor_log_->AddRecord(record);                  // 809-812：追加增量 record
      if (s.ok()) s = descriptor_file_->Sync();                // 813-815：fsync MANIFEST（含目录 fsync，§2.4）
    }
    if (s.ok() && !new_manifest_file.empty()) {
      s = SetCurrentFile(env_, dbname_, manifest_file_number_); // 821-824：仅新 MANIFEST 才切 CURRENT
    }
    mu->Lock();                                                // 826
  }

  if (s.ok()) {                                                // 830-833：全部持久化成功后才安装
    AppendVersion(v);
    log_number_ = edit->log_number_;
    prev_log_number_ = edit->prev_log_number_;
  } else {                                                     // 834-842：失败回滚
    delete v;
    if (!new_manifest_file.empty()) { ... RemoveFile(new_manifest_file); }
  }
  return s;
}
```

**顺序结论**（回答"先写 MANIFEST 还是先更新 CURRENT"）：

1. 内存中先建好新 Version（Builder 应用 edit），但**不安装**；
2. MANIFEST 追加 record 并 **fsync**；
3. 若 MANIFEST 文件本身是新的，才 `SetCurrentFile`（tmp 写+fsync+rename）；
4. 重新加锁后 `AppendVersion` 安装新 Version，更新 log_number_/prev_log_number_。

**为什么这样排序**：崩溃一致性按"提交点"分层——MANIFEST 持久化是提交点；CURRENT 只是指向"已提交"文件的指针。
- 崩溃于 1 之后、2 之前：record 未落盘 → 恢复出旧状态（edit 丢失），一致；
- 崩溃于 2 之后、3 之前：新 record 已落盘但 CURRENT 还指向旧 MANIFEST → 恢复旧状态，一致（新 MANIFEST 成为孤儿文件，GC 清理，见 §12）；
- 崩溃于 3 之后：CURRENT 指向已含 record 的 MANIFEST → 恢复新状态，一致。
- 复用旧 MANIFEST（追加）时根本不动 CURRENT，所以 3 的条件是 `!new_manifest_file.empty()`。

**写失败怎么办**（834-842）：内存中的新 Version 直接 `delete v`（Builder 产物随 Version 析构释放引用）；
新 MANIFEST 关掉并 `RemoveFile`；descriptor_log_/descriptor_file_ 置空（下次重试会重建 MANIFEST + 快照）。
调用方把失败记为 `bg_error_`（`db/db_impl.cc:575, 744, 1048-1050`）→ 之后所有写被拒。
注意：已分配的 file number 不回滚（产生无害空洞）；`last_sequence_`/`next_file_number_` 内存值也不回滚，
但它们是"水位"语义，只会偏大不会破坏正确性。

---

## 8. Compaction 的完整生命周期

### 8.1 触发与选择（PickCompaction）

打分在 `Finalize`（`db/version_set.cc:1018-1049`）：L0 按**文件数** `score = files[0].size() / config::kL0_CompactionTrigger(=4)`；
L>0 按字节 `level_bytes / MaxBytesForLevel`（L1 10MB 起，每层 ×10，43-53）。选出最高分层写入
`compaction_level_/compaction_score_`。

`PickCompaction`（1229-1279）：

- **size 触发优先于 seek 触发**（1233-1235）：`size_compaction = (current_->compaction_score_ >= 1)`；
  `seek_compaction = (current_->file_to_compact_ != nullptr)`。
- size 触发下用 **compact_pointer 轮转**（1242-1254）：选 `compact_pointer_[level]` 之后第一个
  `largest > pointer` 的文件；没有则回绕取第一个文件。
- seek 触发：直接压缩 `file_to_compact_`（1255-1258）。`file_to_compact_` 由 `Version::UpdateStats`
  在 `allowed_seeks` 扣到 ≤0 时设置（`db/version_set.cc:398-409`；Get 后调用，`db/db_impl.cc:1156-1158`）。
- **L0 重叠扩展**（1266-1274）：取所选文件范围后，`GetOverlappingInputs(0, ...)` 把**所有**与之重叠的 L0
  文件一并纳入（`GetOverlappingInputs` 对 L0 在范围被扩大时**清空重扫**，`db/version_set.cc:515-528`）。
- `c->input_version_ = current_; c->input_version_->Ref();`（1263-1264）：**Compaction 期间钉住旧 Version**。

### 8.2 SetupOtherInputs（1356-1415）

- `AddBoundaryInputs`（1331-1354）：若某个文件的上界 user_key 等于另一个文件下界 user_key（即同 user key 被
  文件边界切开），必须一并纳入，否则按层搜索会返回错误版本（1320-1331 注释）。
- 取 level+1 重叠文件（1363-1365）；**扩展优化**（1371-1401）：若扩展 level 输入后 level+1 输入文件数不变且
  总量 < `25 × max_file_size`（`ExpandedCompactionByteSizeLimit`，39-41），则采用更大的输入集。
- 收集 grandparents（level+2 重叠文件，1405-1408）供输出切分使用。
- **立即更新 compact_pointer_** 并写进 edit（1410-1414）——注释说明：Compaction 失败后下一次尝试会换 key 范围。

### 8.3 执行（DoCompactionWork，`db/db_impl.cc:895-1054`）

1. `smallest_snapshot` 计算（907-911）：`snapshots_.empty() ? LastSequence() : snapshots_.oldest()->sequence_number()`（§9）。
2. `MakeInputIterator`（`db/version_set.cc:1197-1227`）：L0 每文件一个迭代器；更高层每层一个串联迭代器；
   `NewMergingIterator` 归并。
3. **放锁**执行归并（915-916）；期间若 `has_imm_`，先抢做 `CompactMemTable`（926-936）。
4. `ShouldStopBefore`（1504-1526）：与 grandparent 的重叠字节超过 `10 × max_file_size` 时切新输出文件。
5. **drop 决策**（947-981，见 §9）。
6. 输出：`OpenCompactionOutputFile`（803-826）——`NewFileNumber()` 并登记 `pending_outputs_`；
   `FinishCompactionOutputFile`（828-875）——Finish + `Sync` + `Close` + 开迭代器校验可用。
7. 失败路径：`CleanupCompaction`（786-801）放弃 builder、从 pending_outputs_ 移除输出、删输出文件。

### 8.4 安装结果（InstallCompactionResults，877-893）

```cpp
compact->compaction->AddInputDeletions(compact->compaction->edit());   // 885：删旧文件
for (outputs) compact->compaction->edit()->AddFile(level + 1, ...);     // 887-891：加新文件
return versions_->LogAndApply(compact->compaction->edit(), &mutex_);    // 892：原子安装
```

`AddInputDeletions`（`db/version_set.cc:1475-1481`）把 level 与 level+1 的全部输入文件写进 edit 的
deleted 集合。

### 8.5 快速路径：IsTrivialMove（1466-1473）

单个 level 输入、level+1 无重叠、grandparent 重叠 ≤ `10×max_file_size` → 不重写数据，只发一个
RemoveFile+AddFile 的 edit（`db/db_impl.cc:735-750`）。

### 8.6 收尾与旧 Version 释放

`BackgroundCompaction`（`db/db_impl.cc:705-784`）非平凡路径收尾：

```cpp
CleanupCompaction(compact);
c->ReleaseInputs();          // 758：input_version_->Unref()
RemoveObsoleteFiles();       // 759：立即 GC（§12）
...
delete c;                    // 761：~Compaction 兜底 Unref（version_set.cc:1460-1464）
```

旧 Version 何时释放：`AppendVersion` 安装新 Version 时 Unref 旧 current（`db/version_set.cc:753`）；
但只有当**所有读者/Compaction 的 ref 全部归还**时 `refs_` 才归零、Version 才析构并从链表摘除
（`db/version_set.cc:68-86, 449-456`）。因此"持长迭代器/长 Compaction"会推迟旧文件被 GC（§12）。

### 8.7 调度

`MaybeScheduleCompaction`（665-680）：无已调度任务、非关闭、无 bg_error、且
`imm_ || manual || NeedsCompaction()` 时 `env_->Schedule(&BGWork)`。
`BackgroundCall`（686-703）：跑完一次后**立即再评估**是否需要继续调度。

---

## 9. 快照与版本的关系

### 9.1 SnapshotList

`db/snapshot.h`：`SnapshotImpl` 只含 `sequence_number_` 与链表指针（20-39）；`SnapshotList` 是带头哨兵的
**双向循环链表**（41-92）：

```cpp
SnapshotList() : head_(0) { head_.prev_ = &head_; head_.next_ = &head_; }
SnapshotImpl* oldest() const { return head_.next_; }   // 49-52
SnapshotImpl* newest() const { return head_.prev_; }   // 53-56
SnapshotImpl* New(SequenceNumber sequence_number) { ... 追加到尾部 ... }  // 59-72
void Delete(const SnapshotImpl* snapshot) { ... 摘链并 delete ... }        // 80-87
```

DBImpl 侧：`GetSnapshot()` = `snapshots_.New(versions_->LastSequence())`（`db/db_impl.cc:1184-1187`）；
`ReleaseSnapshot`（1189-1192）。**快照只记录一个 sequence number，是纯内存对象，重启即失**（对 §13 重要）。

### 9.2 快照如何约束读

`Get`（`db/db_impl.cc:1118-1163`）：`snapshot = options.snapshot ? snapshot->seq : LastSequence()`
（1122-1128）；`LookupKey(key, snapshot)`（1144）编码成 internal key 查询，**seq > snapshot 的版本天然不可见**
（internal key 比较中同 user key 按 seq 降序，`db/dbformat.h:96-111`）。迭代器同理（1165-1174）。

### 9.3 smallest snapshot 如何约束 Compaction 的 drop 决策

`DoCompactionWork`（`db/db_impl.cc:907-911`）：

```cpp
if (snapshots_.empty()) {
  compact->smallest_snapshot = versions_->LastSequence();
} else {
  compact->smallest_snapshot = snapshots_.oldest()->sequence_number();
}
```

归并循环中的两条 drop 规则（947-981，处理同一 user key 从新到旧的序列）：

```cpp
if (last_sequence_for_key <= compact->smallest_snapshot) {
  // 被同一 user key 的更新条目隐藏（rule A）
  drop = true;
} else if (ikey.type == kTypeDeletion &&
           ikey.sequence <= compact->smallest_snapshot &&
           compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
  // (1) 更高层没有该 key 的数据；(2) 更低层数据 seq 更大；
  // (3) 本层更旧版本会被 rule (A) 丢弃 → 删除标记作废
  drop = true;   // rule B
}
last_sequence_for_key = ikey.sequence;
```

**保留规则**（精确表述）：

- 对同一 user key，**每个 key 的最新一条永远保留**；
- 旧条目 E 被丢弃当且仅当存在更新的条目 E' 且 `seq(E') <= smallest_snapshot`（此时所有现存快照都能看到 E'，
  E 对任何快照都不可见）；
- 因此 **`seq > smallest_snapshot` 的条目全部保留**（对现存快照不可见，但更旧的条目可能被某个快照看到）；
  它们会在快照全部释放后（smallest_snapshot 前移）被后续 Compaction 逐步清掉；
- 删除标记：`seq <= smallest_snapshot` 且该 key 在更高层无数据（`IsBaseLevelForKey`，`db/version_set.cc:1483-1502`）
  时整体丢弃。
- `IsBaseLevelForKey` 的检查正是为了防止"删标记被丢、深层老数据复活"。

**关键区分**（对 §13.1 很重要）：快照本身**不 pin 文件**，它只影响 Compaction 的条目保留规则；
文件保留靠 Version 引用计数（迭代器等，§5.2）。

---

## 10. DBImpl 的写路径

`DBImpl::Write`（`db/db_impl.cc:1203-1274`）：

1. **写者队列 + group commit**：每个写者是一个 `Writer{batch, sync, done, cv}`（40-49），入队
   `writers_`（deque），非队头者等待 `cv`（1210-1213）。队头成为 group leader。
2. `MakeRoomForWrite(force)`（1219；实现 1328-1402）：
   - `bg_error_` 非空 → 直接失败；
   - **软限流**：L0 文件数 ≥ `config::kL0_SlowdownWritesTrigger(=8)` → 睡 1ms 一次（1338-1349）；
   - memtable 未满（≤ `options.write_buffer_size`）→ 放行；
   - **imm_ 未排空** → 在 `background_work_finished_signal_` 上等待（1354-1358）；
   - **硬停写**：L0 ≥ `config::kL0_StopWritesTrigger(=12)` → 等待（1359-1362）；
   - 否则**切换 memtable**：`NewFileNumber()` 开新 .log → 关旧 logfile → `imm_ = mem_`、
     `has_imm_=true`、新 `mem_`，`MaybeScheduleCompaction()`（1363-1398）。
     （常量见 `db/dbformat.h:26-46`。）
3. **group commit 打包**：`BuildBatchGroup`（1278-1324）合并后续非 sync 写者的 batch，上限 1MB
   （头部 batch ≤128KB 时上限 128KB+头部）；**sync 写不并入非 sync 组**（1300-1303）。
4. **落盘**（1231-1251，放锁执行）：`log_->AddRecord(batch 序列化)` → 若 `options.sync` 则 `logfile_->Sync()`
   → `WriteBatchInternal::InsertInto(batch, mem_)`（逐条 `mem->Add(seq+i, type, k, v)`）。
   - **sync 失败特殊处理**（1245-1250）：WAL 状态不确定 → `RecordBackgroundError` → **此后所有写失败**
     （fail-stop 防脑裂）。
5. 加锁后 `versions_->SetLastSequence(last_sequence)`（1254）；唤醒队列中到 `last_writer` 为止的所有写者，
   下一个队头继续当 leader（1257-1271）。
6. `WriteBatchInternal::SetSequence(write_batch, last_sequence + 1)`（1224）：group 内 seq 连续分配；
   `Count`/`Sequence` 由 batch header 维护（`db/write_batch_internal.h:23-32`）。

> 回答"触发 compaction 的条件"：写路径不直接触发——memtable 满只做切换；compaction 由
> `MaybeScheduleCompaction` 依据 `imm_ != null || NeedsCompaction()` 调度（§8.7）。

---

## 11. DBImpl::Open / Recover 的精确顺序

`DB::Open`（`db/db_impl.cc:1502-1543`）→ `DBImpl::Recover`（289-380）：

1. **`CreateDir(dbname)`**（295，忽略失败：目录可能已存在）；
2. **锁文件**：`env_->LockFile(dbname/LOCK)`（297-300；POSIX 实现 = `fcntl(fd, F_SETLK, F_WRLCK)` 全文件写锁，
   `util/env_posix.cc:451-460, 639-654`）——进程级排他，防多开；
3. **读 CURRENT 前分支**（302-319）：CURRENT 不存在 → `create_if_missing` 则 `NewDB()`
   （178-211：MANIFEST-000001 写 Comparator/LogNumber=0/NextFile=2/LastSequence=0 快照 + Sync + Close +
   `SetCurrentFile(1)`）；否则 InvalidArgument。`error_if_exists` 时相反。
4. **`versions_->Recover`**（321；§6）：恢复 VersionSet。
5. **WAL 重放**（327-373）：
   - `min_log = LogNumber()`, `prev_log = PrevLogNumber()`（334-335）；
   - 扫目录收集 `number >= min_log || number == prev_log` 的 .log，按号升序（341-361）；
   - 先做**文件存在性校验**：`AddLiveFiles` 得到的集合必须都能在目录中找到，否则 `Corruption("... missing files")`
     （341-358）；
   - 逐 log `RecoverLogFile(logs[i], i==last, save_manifest, edit, &max_sequence)`（362-367），随后
     `MarkFileNumberUsed(logs[i])`（372）；
   - `RecoverLogFile`（382-500）：Reader **始终校验 checksum**（415-419 注释：坏 record 整个跳过而非传播坏数据）；
     每条 record 是一个 WriteBatch → 插进临时 memtable；memtable 超 `write_buffer_size` 就
     `WriteLevel0Table` 刷成 L0 并把 AddFile 记进 `edit`（452-463）；
     **末尾**：`reuse_logs && last_log && compactions==0` → 以 `NewAppendableFile` + `Writer(file, size)`
     **原地复用**最后一个 log 作为 `logfile_`，其 memtable 直接成为 `mem_`（469-488）；
     否则残余 memtable 也刷成 L0（490-497）——**"丢弃陈旧 memtable"在本实现里是"把重放数据物化为 L0 SST"
     而不是留在内存**。
   - `SetLastSequence(max(max_sequence))`（375-377）。
6. **建新 log + memtable**（1511-1525）：若 `mem_ == nullptr`（未复用），`NewFileNumber()` 开新 .log，
   `edit.SetLogNumber(new_log_number)`。
7. **若 `save_manifest`**（重放产生过 L0 文件或未复用旧 log，1526-1530）：
   `edit.SetPrevLogNumber(0); edit.SetLogNumber(logfile_number_); LogAndApply(&edit)`。
8. **`RemoveObsoleteFiles()`**（1532）+ **`MaybeScheduleCompaction()`**（1533）。
9. 失败路径：`delete impl`（1539-1541）——析构会 UnlockFile、删 VersionSet 等（149-176）。

---

## 12. DeleteObsoleteFiles

`db/db_impl.cc:222-287`：

```cpp
if (!bg_error_.ok()) return;                    // 225-229：背景出错后无法判断新版本是否已提交，禁止 GC
std::set<uint64_t> live = pending_outputs_;     // 232：正在写的输出文件
versions_->AddLiveFiles(&live);                 // 233：所有存活 Version（含被读者引用的旧 Version）的文件
env_->GetChildren(dbname_, &filenames);         // 236
for (filename in filenames) {
  if (ParseFileName(filename, &number, &type)) {
    switch (type) {
      case kLogFile:     keep = (number >= versions_->LogNumber()) || (number == versions_->PrevLogNumber()); break;
      case kDescriptorFile: keep = (number >= versions_->ManifestFileNumber()); break;   // 保留本代及更新代的 MANIFEST
      case kTableFile:   keep = (live.find(number) != live.end()); break;
      case kTempFile:    keep = (live.find(number) != live.end()); break;
      case kCurrentFile: case kDBLockFile: case kInfoLogFile: keep = true; break;
    }
    if (!keep) { files_to_delete.push_back(...); if (type == kTableFile) table_cache_->Evict(number); }
  }
}
mutex_.Unlock();
for (...) env_->RemoveFile(dbname_ + "/" + filename);   // 279-286：删文件时放锁（文件名唯一，不会撞新文件）
mutex_.Lock();
```

- **"不在任何活跃 Version 中的文件"** 由 `AddLiveFiles`（`db/version_set.cc:1129-1139`）精确给出：
  遍历 dummy_versions_ 整条链表。旧 Version 只要还被迭代器/Compaction 引用就留在链表里 → 其文件免删。
  这正是 **Snapshot+迭代器 → 时间回溯数据存活** 的底层机制（快照不直接参与，见 §9.3）。
- **删除时机**：`DB::Open` 恢复后（1532）、`CompactMemTable` 成功后（573）、每次 `BackgroundCompaction`
  收尾（759）——都是"版本状态已提交"之后才调。
- MANIFEST 的 GC：`number >= ManifestFileNumber()` 全部保留，旧 MANIFEST 才删（避免与其它进程实例竞争）；
- log 的 GC：只保留当前 log（≥LogNumber）和正在压缩的 prev log。

---

## 13. 与 Stratum 时间轴需求的映射建议

### 13.1 Snapshot + Version 引用计数如何支撑"自动监控 A 事件发生前的数据"

LevelDB 给出的**两层机制**，必须区分清楚：

1. **逻辑时间点**：`GetSnapshot()` 拿到的 sequence number（`db/db_impl.cc:1184-1187`）。
   所有 `seq > snapshot` 的写入对以该快照进行的读不可见（`LookupKey(key, snapshot)`，
   `db/db_impl.cc:1122-1144`）。这就是"时间点回溯"的**读语义**。
2. **物理数据存活**：快照只影响 Compaction 的条目 drop（§9.3），**文件是否被删由 Version 引用计数决定**。
   想要"监控 A 事件前的数据"长期可读，必须在快照之外再**钉住当时的 Version**——LevelDB 中现成的做法是
   **持有迭代器**（`NewInternalIterator` 对 `versions_->current()` `Ref()`，`db/db_impl.cc:1094-1100`，
   迭代器析构才归还，1068-1076）。只要迭代器活着，`AddLiveFiles`（§12）就保护该 Version 的全部文件。

**API 形态**（现状）：`DB::GetSnapshot() / DB::ReleaseSnapshot()` + `ReadOptions.snapshot`
（`include/LSMengine/db.h:92-96`；ReadOptions.snapshot 的消费见 `db/db_impl.cc:1123, 1170-1173`）。
监控器 = "snapshot + 常驻迭代器"组合。

**三个局限，Stratum 必须处理**：

- 快照/迭代器都是**进程内、易失**的：崩溃或重启后监控点消失，且恢复时的 `RemoveObsoleteFiles`
  可能立刻删掉"回溯所需"的文件（§11 步骤 8）。→ 需要**持久化保留点**（§13.3）。
- 快照语义只能"看到某个 seq 及更早"，LevelDB 没有"冻结文件集合"的公开 API；钉 Version 是内部行为
  （`Version::Ref`，`db/version_set.cc:447`）。Stratum 需要把它提升为公开 API（如
  `PinVersion() → Handle; Unpin(Handle)`），或让持久化保留点自动 pin。
- 由于 Compaction 只会按 `smallest_snapshot` 保留条目，若 Stratum 长期持有最早保留点，建议把
  `smallest_snapshot` 的语义扩展为 `min(最老快照, 最老保留点序列号)`，否则过期数据的物理回收会停滞。

### 13.2 Stratum 单调追加场景下 Version 机制的价值在哪

Stratum 的 key 唯一、无覆盖无删除 → Compaction 的 drop 规则几乎永远不触发，IsTrivialMove 路径占比高。
Version 机制的价值**不在重写/去重**，而在以下四点：

1. **文件集合的原子切换**：新一批 SST 只有经 `LogAndApply`（§7）才对外可见；
   读者在切换前后看到的是**完整的旧集合或完整的新集合**，绝无中间态。监控多文件跨度查询的一致性由它保证。
2. **崩溃一致性**：MANIFEST（先 fsync，含目录 fsync）→ CURRENT（rename）的两段式提交顺序（§7），
   保证崩溃后文件集合与元数据一致，且恢复时用 `AddLiveFiles` 校验文件存在（§11）。
3. **obsolete 文件管理**：refcounted Version 链表 + `AddLiveFiles` + `RemoveObsoleteFiles`（§12）给出
   "何时可安全删文件"的精确答案；Stratum 做保留点/过期数据物理回收必须复用这套判定，
   否则删错文件无法恢复。
4. **文件号命名空间与恢复水位**：`NewFileNumber` / `MarkFileNumberUsed` / `ReuseFileNumber`
   （`db/version_set.h:184-192`、`db/version_set.cc:1012-1016`）和 `last_sequence_` 水位（778, 962）
   是时间轴（sequence）与磁盘命名空间的桥梁。

另外注意：LevelDB 的 Compaction 是"整文件重写"，对纯追加场景建议直接复用 **trivial move** 逻辑
（`db/version_set.cc:1466-1473`、`db/db_impl.cc:735-750`）：零重写地把 L0 文件挪到更深层，只产生
RemoveFile+AddFile 的 MANIFEST edit——这正是 Stratum 最想要的 compaction 形态。

### 13.3 若要把"保留点"持久化到 MANIFEST，应该加什么 VersionEdit tag

**建议新增 tag：`kRetentionPoint = 10`**（理由：1-7 已占用，8 曾用于 large value reference 而保留，
9 = kPrevLogNumber，下一个空闲是 10；**不可复用 8**，见 `db/version_edit.cc:24` 注释）。

**payload 建议**：`varint64(sequence_number)` + `length-prefixed(name)`（name 可选，留空则匿名）。
即"保留点 = 名字 + 时间点（seq）"。

**必须同步修改的位置**（每个位置都有一处 switch/循环，漏一处即格式不兼容）：

1. `VersionEdit` 增加字段与 `SetRetentionPoint(...)`，`Clear()` 清空（`db/version_edit.h:92-105` 模式）；
2. `EncodeTo` 输出（`db/version_edit.cc:44-87` 末尾追加）；
3. `DecodeFrom` 新增 case（`db/version_edit.cc:121-195` switch）——**当前未知 tag 是致命错误**
   （191-193），意味着旧版本二进制读到新 MANIFEST 会直接 Corruption：发布时新旧必须同步升级，或先落
   "识别并忽略未知 tag"的兼容改造；
4. `Builder::Apply` 或 `VersionSet` 侧记录保留点（`db/version_set.cc:621-658` 模式）：
   在 `VersionSet` 增加 `retention_points_` 表（name → seq），并在 Recover 时重建；
5. **`WriteSnapshot` 必须带上保留点**（`db/version_set.cc:1051-1079`）：MANIFEST 会被压缩重写
   （§2.3、`ReuseManifest` 981-1010），快照不含保留点则 MANIFEST 压缩后保留点永久丢失；
6. Recover 侧（`db/version_set.cc:848-979`）：读到保留点记录时**重建被 pin 的 Version**——由于 MANIFEST
   是增量日志，重放到保留点记录位置即可重构当时文件集合；该 Version 保留在 dummy_versions_ 链表中
   （`Ref()` 后）即自动被 `AddLiveFiles` 保护 → 崩溃重启后保留点的文件不会被 GC。

**写入方式**：空 edit（只有 kRetentionPoint tag）走 `LogAndApply` —— 与版本切换原子共提交，保证
"保留点"与"文件集合"的先后关系精确记录。崩溃一致性沿用 §7 的两段式提交，无需新机制。

**与 Compaction 的交互**：Stratum 若要求"保留点之前的数据可被物理回收"，则 Compaction 的
`smallest_snapshot` 需取 `min(snapshots_.oldest, retention_floor)`（`db/db_impl.cc:907-911` 扩展）；
若要求"保留点之前的数据永不回收"，则只需 pin Version + 把保留点版本纳入 `AddLiveFiles`（已是现状机制），
并在 `RemoveObsoleteFiles` 判定中不需要改动。

**替代方案（不污染 MANIFEST 格式）**：保留点写独立文件（如 `RETENTION-*`）——简单但失去与版本切换的
原子性/顺序保证，且要多维护一套崩溃一致性协议；**推荐 MANIFEST 方案**。

---

## 附：待确认清单

1. CURRENT 的 `rename` 之后无目录 fsync（§3.2）：断电可能使 CURRENT 回退到旧内容；一致但丢失最新提交，
   与上游 LevelDB 行为一致。
2. §5.1 中 files_[0] 的排序：任务描述称 L0 按文件号排序；本实现 `Builder::SaveTo` 对所有层统一按
   smallest key 排序（平局按文件号），文件号降序只出现在点查的 `ForEachOverlapping` 临时排序中。
3. `allowed_seeks` 初始化为 `file_size/16384`（下限 100）是 Builder::Apply 的启发式（`db/version_set.cc:642-653`），
   具体数值来自上游注释中的性能估算，非持久化格式的一部分。
4. MANIFEST 损坏时 `Recover` 中 `has_*` 元数据可能被部分解码的 edit 污染，但由于 Recover 遇错即整体失败
   （§6），不影响正确性。

---

*文档由源码精读生成；所有行号对应 `/UserData/LeaningLevelDB/LSMengine` 当前工作树，未修改任何源码文件。*
