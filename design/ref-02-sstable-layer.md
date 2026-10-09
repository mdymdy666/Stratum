# 参考实现 LSMengine 之 SSTable 层设计分析（ref-02）

> 分析对象：WSL `/UserData/LeaningLevelDB/LSMengine`（LevelDB 1.x 的完整翻版，命名空间 `LSMengine`）。
> 本文件所有行号引用均相对 LSMengine 仓库根目录，格式为 `文件:行号`（如 `table/block.cc:165-223`）。
> 本文档只做源码分析，不修改任何源码文件。

---

## 1. SSTable 文件的完整布局

官方格式文档 `doc/table_format.md:4-15` 给出了顶层布局：

```
<beginning_of_file>
[data block 1]
[data block 2]
...
[data block N]
[meta block 1]          ← 目前只有 filter block（可选）
[metaindex block]
[index block]
[Footer]                (fixed size; starts at file_size - sizeof(Footer))
<end_of_file>
```

**字节级精确布局**（结合 `table/table_builder.cc` 的写入路径）：

```
offset 0
+---------------------------+------------------------------------+
| data block 1              |  BlockBuilder 输出（前缀压缩条目） |
|   + trailer (5 字节)      |  [type:1B][crc32c masked:4B]       |
+---------------------------+------------------------------------+
| data block 2 + trailer    |  ...                               |
+---------------------------+------------------------------------+
| ...                       |  ...                               |
+---------------------------+------------------------------------+
| data block N + trailer    |                                    |
+---------------------------+------------------------------------+
| filter block + trailer    |  仅当 options.filter_policy != null |
|                           |  且不压缩 (type = kNoCompression)  |
+---------------------------+------------------------------------+
| metaindex block + trailer |  内容：entry("filter.<Name>",      |
|                           |        BlockHandle(filter block))  |
+---------------------------+------------------------------------+
| index block    + trailer  |  每 data block 一条 entry          |
+---------------------------+------------------------------------+
| Footer（固定 48 字节）    |  metaindex_handle: ≤20B (padding)  |
|                           |  index_handle:     ≤20B (padding)  |
|                           |  magic: 8B (little-endian)         |
+---------------------------+------------------------------------+
<end_of_file>
```

**每个物理 block 的磁盘形态**（`table/table_builder.cc:189-206` `WriteRawBlock`）：

```cpp
// block_data: uint8[n]   ← BlockBuilder::Finish() 的输出（可能被压缩）
// type:       uint8      ← CompressionType（0/1/2，见 include/LSMengine/options.h:23-28）
// crc:        uint32     ← crc32c::Value(data, n) 再 Extend(type 1 字节) 后 Mask
trailer[0] = type;
uint32_t crc = crc32c::Value(block_contents.data(), block_contents.size());
crc = crc32c::Extend(crc, trailer, 1);  // compression type 也纳入 CRC
EncodeFixed32(trailer + 1, crc32c::Mask(crc));
```

`kBlockTrailerSize = 5`（`table/format.h:77`）。BlockHandle 的 `size()` **不含 trailer**（`table/format.h:34-35` 注释），读取时统一读 `n + 5` 字节（`table/format.cc:78-81`）。

**Finish() 的写入顺序**（`table/table_builder.cc:210-265`）：
1. `Flush()` —— 把最后一个未满的 data block 写盘（`table_builder.cc:212`）；
2. 写 filter block（`WriteRawBlock(..., kNoCompression, ...)`，`table_builder.cc:219-222`）；
3. 写 metaindex block（`table_builder.cc:225-238`）；
4. 写 index block（`table_builder.cc:241-250`）；
5. 写 48 字节 footer（`table_builder.cc:253-263`）。

因此 data blocks 在文件前部、footer 在尾部，metaindex/filter/index 在中间——这与 `doc/table_format.md` 的布局图完全一致。

---

## 2. Footer：48 字节精确结构

**长度**：`table/format.h:51`

```cpp
enum { kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8 };  // 20*2+8 = 48
```

**编码**（`table/format.cc:32-41` `Footer::EncodeTo`）：

```cpp
const size_t original_size = dst->size();
metaindex_handle_.EncodeTo(dst);   // 2 个 varint64（offset, size）
index_handle_.EncodeTo(dst);       // 2 个 varint64（offset, size）
dst->resize(2 * BlockHandle::kMaxEncodedLength);  // 补齐到 40 字节，padding 为 0
PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber & 0xffffffffu));  // magic 低 32 位
PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber >> 32));          // magic 高 32 位
```

字节布局：

```
byte 0  ──────────────┐
      │ metaindex_handle: varint64(offset) + varint64(size)   │ ≤ 20 字节
byte 20 ├─────────────┤ 不够 20 字节时右侧补 0x00（padding）
byte 20 ──────────────┐
      │ index_handle:     varint64(offset) + varint64(size)   │ ≤ 20 字节
byte 40 ├─────────────┤ 不够 20 字节时右侧补 0x00（padding）
byte 40 ──────────────┐
      │ magic: 8 字节 little-endian fixed64                    │
byte 48 ──────────────┘
```

**magic 值**（`table/format.h:71-74`）：

```cpp
// kTableMagicNumber 由以下命令产生：
//    echo http://code.google.com/p/leveldb/ | sha1sum
// 并取结果的前 64 位。
static const uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;
```

注意：`DecodeFrom` 用 `fixed32` 拼回 uint64（`table/format.cc:48-52`），因此**磁盘上 magic 是小端序**的 8 字节。校验逻辑在 `table/format.cc:43-67`：先查长度 ≥ 48，再比对 magic，然后从开头依次解两个 varint64 handle，最后把输入 Slice 推进到 magic 之后（容忍尾部多余字节，`table/format.cc:63-64`）。

---

## 3. BlockHandle：varint 编码

**结构**（`table/format.h:23-44`）：两个 `uint64_t`——`offset_`（block 在文件中的偏移）与 `size_`（block 内容长度，不含 5 字节 trailer）。未设置时初始化为 `~0`（`table/format.h:92-93`）。

**编码**（`table/format.cc:16-22`）：

```cpp
void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}
```

varint64 算法（`util/coding.cc:57-66`）：**每字节低 7 位携带数值，最高位（0x80）表示"后面还有字节"**，低位在前；uint64 最多占 10 字节：

```cpp
char* EncodeVarint64(char* dst, uint64_t v) {
  while (v >= B /*128*/) {
    *(ptr++) = v | B;   // 最高位置 1
    v >>= 7;
  }
  *(ptr++) = static_cast<uint8_t>(v);   // 最后一字节最高位为 0
}
```

最大编码长度 `kMaxEncodedLength = 10 + 10`（`table/format.h:26`）。解码端 `util/coding.cc:118-133` 循环移位 7 位，最高位为 0 的字节结束。

**为什么用 varint**：
- BlockHandle 出现的两处场景都是"小数值为主"：① index block 的 value 里每 data block 一条（`table/table_builder.cc:104-106`），相邻 block 的 offset 增量约等于 block_size（默认 4KB），用 1-2 字节即可表示，定长 16 字节会浪费 ~90%；② footer 里两个 handle。
- 同时 footer 又必须**定长**才能"从文件尾读固定 48 字节"（`table/table.cc:47-50`）。varint + `resize(2*kMaxEncodedLength)` 补零（`table/format.cc:36`）同时满足"常见值短"和"最坏情况定长"两个需求，是空间与简单性的折中。

---

## 4. Block 格式：restart 点机制与 BlockBuilder

### 4.1 条目格式（`table/block_builder.cc:5-23` 头注释）

```
每个 entry：
    shared_bytes:    varint32   ← 与前一个 key 的公共前缀字节数
    unshared_bytes:  varint32   ← key 差异部分长度
    value_length:    varint32
    key_delta:       char[unshared_bytes]
    value:           char[value_length]
restart 点处 shared_bytes == 0（存完整 key）。

block 尾部：
    restarts:        uint32[num_restarts]   ← 每个 restart 点在 block 内的偏移
    num_restarts:    uint32
```

value 不参与前缀压缩，直接跟在 key 后（`table/block_builder.cc:94-96`）。

### 4.2 为什么需要 restart 点

前缀压缩（第 i 个 key 只存与第 i-1 个 key 的差异）让**顺序读**很快，但**随机 Seek 必须从 block 起点逐条解码**才能重建任意 key（O(block) 条）。restart 点的作用是：
- 每 `block_restart_interval` 个 key（默认 **16**，`include/LSMengine/options.h:90`）强制存一个完整 key（`table/block_builder.cc:82-86`：`counter_` 达到 interval 时 `restarts_.push_back(buffer_.size()); counter_ = 0;`，本轮 shared=0）；
- 查找时先对 restart 数组二分（O(log n) 次 key 比较），再在单个 restart 区间内线性解码 ≤16 个 entry，把最坏扫描长度限制在 interval 内（详见第 5 节）。

构造时 `restarts_.push_back(0)`（`table/block_builder.cc:41`）保证偏移 0 处始终有一个 restart 锚点，空 block 也有合法结构，二分查找也有固定起点。

### 4.3 BlockBuilder 状态机（`table/block_builder.h:42-49` + `table/block_builder.cc`）

状态：`buffer_`（序列化输出）、`restarts_`（偏移数组）、`counter_`（自上次 restart 后的 entry 数）、`finished_`、`last_key_`（供前缀比较）。

**Add(key, value)**（`table/block_builder.cc:69-103`）：

```cpp
void BlockBuilder::Add(const Slice& key, const Slice& value) {
  Slice last_key_piece(last_key_);
  assert(!finished_);
  assert(counter_ <= options_->block_restart_interval);
  assert(buffer_.empty() || options_->comparator->Compare(key, last_key_piece) > 0);  // key 必须严格递增
  size_t shared = 0;
  if (counter_ < options_->block_restart_interval) {
    // 逐字节数公共前缀
    const size_t min_length = std::min(last_key_piece.size(), key.size());
    while ((shared < min_length) && (last_key_piece[shared] == key[shared])) shared++;
  } else {
    restarts_.push_back(buffer_.size());  // 新 restart 区间
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;
  PutVarint32(&buffer_, shared);
  PutVarint32(&buffer_, non_shared);
  PutVarint32(&buffer_, value.size());
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());
  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  counter_++;
}
```

**Finish()**（`table/block_builder.cc:59-67`）：在条目数据之后追加 restart 偏移数组 + 数组长度：

```cpp
Slice BlockBuilder::Finish() {
  for (size_t i = 0; i < restarts_.size(); i++) PutFixed32(&buffer_, restarts_[i]);
  PutFixed32(&buffer_, restarts_.size());   // num_restarts
  finished_ = true;
  return Slice(buffer_);
}
```

**Reset()**（`table/block_builder.cc:44-51`）：清空 buffer、restarts 重新置 `{0}`、counter=0、finished=false。

大小估计（`table/block_builder.cc:53-57`）：`CurrentSizeEstimate() = buffer_.size() + restarts_.size()*4 + 4`，TableBuilder 用它判断是否达 `block_size`（默认 4KB，`include/LSMengine/options.h:86`）。

### 4.4 解码端对 restart 元数据的校验（`table/block.cc:22-42`）

```cpp
inline uint32_t Block::NumRestarts() const {
  return DecodeFixed32(data_ + size_ - sizeof(uint32_t));   // 最后 4 字节
}
Block::Block(const BlockContents& contents) ... {
  if (size_ < sizeof(uint32_t)) { size_ = 0; /* 错误标记 */ }
  else {
    size_t max_restarts_allowed = (size_ - sizeof(uint32_t)) / sizeof(uint32_t);
    if (NumRestarts() > max_restarts_allowed) size_ = 0;   // 声明数与实际空间不符 → 损坏
    else restart_offset_ = size_ - (1 + NumRestarts()) * sizeof(uint32_t);  // restart 数组起点
  }
}
```

只有 restart 数组完全落在 block 范围内才信任元数据；损坏时 `size_==0`，`NewIterator` 返回错误迭代器（`table/block.cc:276-286`）。

---

## 5. Block 迭代器：二分定位 + 线性扫描

实现为 `Block::Iter`（`table/block.cc:78-274`），持有 `data_`、`restarts_`（restart 数组偏移）、`num_restarts_`、`current_`（当前条目在 block 内的偏移）、`restart_index_`。

### 5.1 Seek：二分 restart 数组（`table/block.cc:165-223`）

```cpp
void Seek(const Slice& target) override {
  uint32_t left = 0;
  uint32_t right = num_restarts_ - 1;
  int current_key_compare = 0;
  if (Valid()) {
    current_key_compare = Compare(key_, target);
    if (current_key_compare < 0)      left = restart_index_;   // 目标在当前位置之后：收窄左界
    else if (current_key_compare > 0) right = restart_index_;  // 目标在之前：收窄右界
    else return;                        // 已在目标上
  }
  while (left < right) {
    uint32_t mid = (left + right + 1) / 2;
    uint32_t region_offset = GetRestartPoint(mid);
    uint32_t shared, non_shared, value_length;
    const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_,
                                      &shared, &non_shared, &value_length);
    if (key_ptr == nullptr || (shared != 0)) { CorruptionError(); return; }  // restart 点必须 shared==0
    Slice mid_key(key_ptr, non_shared);
    if (Compare(mid_key, target) < 0) left = mid;       // 排除 mid 之前的区间
    else                              right = mid - 1;  // 排除 mid 及之后
  }
  // 若目标就在当前 restart 区间且位于当前 key 之后，直接从当前位置继续，省去回退重扫
  bool skip_seek = left == restart_index_ && current_key_compare < 0;
  if (!skip_seek) SeekToRestartPoint(left);
  // 区间内线性扫描：找第一个 key >= target
  while (true) {
    if (!ParseNextKey()) return;
    if (Compare(key_, target) >= 0) return;
  }
}
```

关键点：
- restart 点的 key 是**完整** key，所以 `DecodeEntry` 后必须校验 `shared == 0`（`block.cc:193`），否则视为损坏；
- 二分收敛后最多再线性解码一个 restart 区间的条目（≤ `block_restart_interval` = 16 个）；
- `skip_seek` 是"顺序扫描时 Seek"的优化：若目标仍在当前区间且更大，直接接着扫（`block.cc:210-213`）。

### 5.2 ParseNextKey：前缀重建 + restart_index_ 推进（`table/block.cc:246-273`）

```cpp
bool ParseNextKey() {
  current_ = NextEntryOffset();                 // 从上一 value 末尾继续
  const char* p = data_ + current_;
  const char* limit = data_ + restarts_;        // restart 数组紧跟在条目数据之后
  if (p >= limit) { current_ = restarts_; restart_index_ = num_restarts_; return false; }
  uint32_t shared, non_shared, value_length;
  p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
  if (p == nullptr || key_.size() < shared) { CorruptionError(); return false; }
  else {
    key_.resize(shared);                        // 复用前一个 key 的公共前缀
    key_.append(p, non_shared);                 // 追加差异部分
    value_ = Slice(p + non_shared, value_length);
    while (restart_index_ + 1 < num_restarts_ &&
           GetRestartPoint(restart_index_ + 1) < current_) ++restart_index_;
    return true;
  }
}
```

`DecodeEntry`（`table/block.cc:56-76`）有快速路径：当 `shared/non_shared/value_length` 三者都 < 128 时，各自只占一个字节，直接读 3 字节再前进（`block.cc:63-65`），避免三次 varint 解码调用。

### 5.3 其他定位

- `Next()`：直接 `ParseNextKey()`（`block.cc:139-142`）；
- `Prev()`（`block.cc:144-163`）：回退到 `current_` 之前的最近 restart 点（`GetRestartPoint(restart_index_) >= original` 则 `restart_index_--`），`SeekToRestartPoint` 后重扫到原位置前一条；
- `SeekToFirst/Last`（`block.cc:225-235`）：restart 0 / 最后一个 restart 点起扫。

---

## 6. 索引机制：index block 与 TwoLevelIterator

### 6.1 index entry 的生成（"每 N 个 data block 的最后一个 key"）

index block 本身也是用 BlockBuilder 构建的普通 block，其 entry 是 `(index_key, BlockHandle 编码)`，每个 **data block 一条**。

**延迟写入**（`table/table_builder.cc:53-57` 注释 + `101-108`）：

```cpp
// 看到下一个 data block 的第一个 key 后，才为前一个 block 写 index entry。
// 这样可利用相邻 block 的边界生成更短的 index key。例如边界两侧分别是
// "the quick brown fox" 和 "the who" 时，可以用 "the r" 作为前一 block 的
// index key，因为它 >= 前一 block 的所有 entry，且 < 后续 block 的所有 entry。
if (r->pending_index_entry) {
  assert(r->data_block.empty());
  r->options.comparator->FindShortestSeparator(&r->last_key, key);
  std::string handle_encoding;
  r->pending_handle.EncodeTo(&handle_encoding);
  r->index_block.Add(r->last_key, Slice(handle_encoding));
  r->pending_index_entry = false;
}
```

`Flush()` 写盘 data block 后置 `pending_index_entry = true` 并记下 `pending_handle`（`table/table_builder.cc:130-133`）。最后一个 block 在 `Finish()` 中用 `FindShortSuccessor(last_key)` 生成（`table/table_builder.cc:242-248`）。

**分隔键缩短算法**（`util/comparator.cc:33-54`）：找 `start` 与 `limit` 的公共前缀，在第一个差异字节处若 `start[diff]+1 < limit[diff]`，则把 start 截断为 `前缀 + (start[diff]+1)`；不能缩短时保持原样。内部比较器包装（`db/dbformat.cc:68-85`）保证缩短的是 user key 部分且结果仍大于原 key、小于 limit（追加 `PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek)` 即最小可能 tag）：

```cpp
void InternalKeyComparator::FindShortestSeparator(std::string* start, const Slice& limit) const {
  Slice user_start = ExtractUserKey(*start);
  Slice user_limit = ExtractUserKey(limit);
  std::string tmp(user_start.data(), user_start.size());
  user_comparator_->FindShortestSeparator(&tmp, user_limit);
  if (tmp.size() < user_start.size() && user_comparator_->Compare(user_start, tmp) < 0) {
    PutFixed64(&tmp, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    start->swap(tmp);
  }
}
```

**index block 的 restart interval = 1**（`table/table_builder.cc:38`：`index_block_options.block_restart_interval = 1;`）——相邻 index key 是"人工构造的分隔键"，通常无公共前缀，前缀压缩无收益，interval=1 使每个 index entry 都是完整 key（shared 恒 0），也避免了解码时跨 entry 依赖。

`doc/table_format.md:34-36` 对 index 的官方表述："key 大于等于该 data block 的最后一个 key，并小于后续 data block 的第一个 key；value 是该 data block 的 BlockHandle"。

### 6.2 TwoLevelIterator 的数据流

组合方式（`table/table.cc:207-211`）：

```cpp
Iterator* Table::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(
      rep_->index_block->NewIterator(rep_->options.comparator),  // 第一层：index iterator
      &Table::BlockReader, const_cast<Table*>(this), options);   // value→data iterator 的工厂
}
```

数据流（`table/two_level_iterator.cc:20-162`）：

```
TwoLevelIterator
 ├── index_iter_   (IteratorWrapper，遍历 index block；key=分隔键，value=BlockHandle 编码)
 └── data_iter_    (IteratorWrapper，当前 data block 的迭代器；可能为 null)
```

- **Seek(target)**（`two_level_iterator.cc:84-89`）：
  1. `index_iter_.Seek(target)` —— 二分定位到"第一个 index key ≥ target"的 index entry（即目标所在 data block 的 handle）；
  2. `InitDataBlock()` —— 取 `index_iter_.value()` 解码出 BlockHandle，调 `block_function_`（即 `Table::BlockReader`）创建该 block 的迭代器；
  3. `data_iter_.Seek(target)` —— 在 block 内二分 restart + 线性扫描；
  4. `SkipEmptyDataBlocksForward()` —— 若该 block 里没有 ≥ target 的 key（或 block 为空），`index_iter_.Next()` 到下一 block 并 `SeekToFirst()`，循环（`two_level_iterator.cc:117-128`）。
- **Next/Prev**（`two_level_iterator.cc:105-115`）：先在 data_iter 内移动，耗尽后跳过空 block 前进/后退到相邻 block。
- **InitDataBlock 的重复创建防护**（`two_level_iterator.cc:148-162`）：用 `data_block_handle_` 记住当前 data iterator 对应的 handle 编码，同一 handle 不重建（Prev/Next 在 block 边界抖动时避免重复 I/O）。
- **错误保存**：`SetDataIterator` 把被替换的 data 迭代器的错误存入 `status_`（`two_level_iterator.cc:143-146`），避免"跳空 block"时丢失错误。

### 6.3 Table::BlockReader（value → data iterator 工厂，`table/table.cc:153-205`）

```cpp
Iterator* Table::BlockReader(void* arg, const ReadOptions& options, const Slice& index_value) {
  Table* table = reinterpret_cast<Table*>(arg);
  Cache* block_cache = table->rep_->options.block_cache;
  Block* block = nullptr;
  Cache::Handle* cache_handle = nullptr;
  BlockHandle handle;
  Slice input = index_value;
  Status s = handle.DecodeFrom(&input);   // 解码 varint64 offset/size
  if (s.ok()) {
    BlockContents contents;
    if (block_cache != nullptr) {
      char cache_key_buffer[16];
      EncodeFixed64(cache_key_buffer, table->rep_->cache_id);          // 前 8 字节
      EncodeFixed64(cache_key_buffer + 8, handle.offset());            // 后 8 字节
      Slice key(cache_key_buffer, sizeof(cache_key_buffer));
      cache_handle = block_cache->Lookup(key);
      if (cache_handle != nullptr) {
        block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
      } else {
        s = ReadBlock(table->rep_->file, options, handle, &contents);
        if (s.ok()) {
          block = new Block(contents);
          if (contents.cachable && options.fill_cache)
            cache_handle = block_cache->Insert(key, block, block->size(), &DeleteCachedBlock);
        }
      }
    } else { /* 无 cache：直接 ReadBlock + new Block */ }
  }
  Iterator* iter;
  if (block != nullptr) {
    iter = block->NewIterator(table->rep_->options.comparator);
    if (cache_handle == nullptr) iter->RegisterCleanup(&DeleteBlock, block, nullptr);   // 迭代器销毁时 delete block
    else                         iter->RegisterCleanup(&ReleaseBlock, block_cache, cache_handle);  // 迭代器销毁时 Release handle
  } else {
    iter = NewErrorIterator(s);
  }
  return iter;
}
```

清理机制（`table/iterator.cc:11-41` + `include/LSMengine/iterator.h:64-87`）：`RegisterCleanup(func, arg1, arg2)` 把回调挂到迭代器内部的单链表，`~Iterator()` 统一执行——block 内存的释放时机与迭代器生命周期绑定，cache handle 在迭代器存活期间 pin 住底层 Block。

---

## 7. FilterBlock（布隆过滤器）

### 7.1 FilterPolicy 契约（`include/LSMengine/filter_policy.h:22-43`）

`KeyMayMatch` 返回 false 时**确定** key 不存在；返回 true 允许假阳性，**绝不能有假阴性**。`Name()` 用于 metaindex 的 key（`"filter.<Name>"`）。`bits_per_key=10` 约对应 1% 假阳性（`filter_policy.h:45-47`）。

### 7.2 生成算法（`util/bloom.cc`）

探测次数 k（`bloom.cc:21-26`）：

```cpp
explicit BloomFilterPolicy(int bits_per_key) : bits_per_key_(bits_per_key) {
  k_ = static_cast<size_t>(bits_per_key * 0.69);  // 0.69 ≈ ln(2)，最优 k = m/n·ln2
  if (k_ < 1) k_ = 1;
  if (k_ > 30) k_ = 30;
}
```

CreateFilter（`bloom.cc:30-54`）——位数组大小 `max(n*bits_per_key, 64)` bits，**双重哈希**生成 k 个位（Kirsch & Mitzenmacher 2006）：

```cpp
uint32_t h = BloomHash(keys[i]);              // Hash(key, seed=0xbc9f1d34)，bloom.cc:15-17
const uint32_t delta = (h >> 17) | (h << 15); // 循环右移 17 bit
for (size_t j = 0; j < k_; j++) {
  const uint32_t bitpos = h % bits;
  array[bitpos / 8] |= (1 << (bitpos % 8));
  h += delta;                                  // 第 j 个位置 = (h + j*delta) % bits
}
```

过滤器**最后一个字节写入 k**（`bloom.cc:42`），使读取端可以用编码在过滤器内的 k 解析（兼容不同参数构建的过滤器）；KeyMayMatch（`bloom.cc:56-78`）读出 k，重算同一序列的 k 个位，任一为 0 即返回 false；`k > 30` 视为"可能匹配"（保留扩展位）。

internal key 适配（`db/dbformat.cc:102-117`）：`InternalFilterPolicy` 在 `CreateFilter`/`KeyMayMatch` 前用 `ExtractUserKey` 剥掉 8 字节 sequence/type tag，bloom 只对 user key 生效。

### 7.3 FilterBlockBuilder 的格式

按 data block 的**文件偏移**每 2KB 一组生成一个 filter（`table/filter_block.cc:16-17`）：

```cpp
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;   // 2048
```

- `StartBlock(block_offset)`（`filter_block.cc:22-28`）：`filter_index = block_offset / 2048`，若中间跳过了分组就补生成空 filter（`GenerateFilter` 快路径只 push 一个偏移，`filter_block.cc:54-58`）；
- `AddKey(key)`（`filter_block.cc:30-34`）：key 平铺进 `keys_`，`start_` 记录起始下标；
- `GenerateFilter()`（`filter_block.cc:52-76`）：把 `start_` 转成 Slice 列表，调 `policy_->CreateFilter(...)` 追加到 `result_`，同时 `filter_offsets_.push_back(result_.size())`；
- `Finish()`（`filter_block.cc:36-50`）：追加 offset 数组、数组起点、编码参数：

```
[filter 0][filter 1]...[filter N-1]
[offset of filter 0]      : 4 字节 (fixed32)
[offset of filter 1]      : 4 字节
...
[offset of filter N-1]    : 4 字节
[offset of offset array]  : 4 字节   ← array_offset = result_.size()（追加前）
lg(base)                  : 1 字节   ← 11
```

（格式与 `doc/table_format.md:62-78` 一致；此处实现中 offset 数组的"结尾哨兵"就是 `array_offset` 本身，`FilterBlockReader` 用它截断最后一个 filter。）

Builder 调用时序由正则 `(StartBlock AddKey*)* Finish` 约束（`table/filter_block.h:26-27`）。TableBuilder 侧：构造时 `StartBlock(0)`（`table/table_builder.cc:68-71`），每次 `Flush()` 写盘后 `StartBlock(r->offset)`（`table_builder.cc:135-137`），每个 key `AddKey`（`table_builder.cc:110-112`）。

### 7.4 读取时的查找流程

FilterBlockReader 构造（`table/filter_block.cc:78-89`）：从尾部读 `base_lg_`（1B）与 `array_offset`（4B），校验 `array_offset <= n-5`，算出 `offset_` 与 `num_`。

KeyMayMatch（`table/filter_block.cc:91-105`）：

```cpp
bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
  uint64_t index = block_offset >> base_lg_;      // 2KB 分组的序号
  if (index < num_) {
    uint32_t start = DecodeFixed32(offset_ + index * 4);
    uint32_t limit = DecodeFixed32(offset_ + index * 4 + 4);
    if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
      Slice filter = Slice(data_ + start, limit - start);
      return policy_->KeyMayMatch(key, filter);
    } else if (start == limit) {
      return false;                               // 空 filter：确定不匹配
    }
  }
  return true;   // 损坏或越界按"可能匹配"处理，防止 false negative（filter_block.h:52-53）
}
```

接入点：`Table::InternalGet`（`table/table.cc:221-225`）在读取 data block **之前**调用：

```cpp
if (filter != nullptr && handle.DecodeFrom(&handle_value).ok() &&
    !filter->KeyMayMatch(handle.offset(), k)) {
  // 跳过 block 读取（点查路径的读放大优化）
} else {
  Iterator* block_iter = BlockReader(this, options, iiter->value());
  ...
}
```

filter block 的加载链：`Table::ReadMeta`（`table/table.cc:83-110`）读 metaindex，Seek `"filter.<Name>"`（用 BytewiseComparator），命中后 `ReadFilter`（`table/table.cc:112-132`）解码 BlockHandle 并 `ReadBlock`，常驻内存（`Rep::filter` + `filter_data`，`table/table.cc:22-37`）。filter block 写盘时**不压缩**（`table/table_builder.cc:220`）。

---

## 8. TableBuilder 状态机

状态保存在 `TableBuilder::Rep`（`table/table_builder.cc:24-64`）：`data_block`、`index_block`、`filter_block`、`last_key`、`pending_index_entry`、`pending_handle`、`offset`、`num_entries`。不变量：**仅当 data_block 为空时 pending_index_entry 才为 true**（`table_builder.cc:59`）。

### 8.1 Add 的分流（`table/table_builder.cc:93-122`）

```
Add(key, value)
 ├─ 断言：key > last_key（严格递增，:97-99）
 ├─ pending_index_entry? ──是──→ FindShortestSeparator(last_key, key)，
 │                               index_block.Add(last_key, pending_handle 编码)，清 pending  (:101-108)
 ├─ filter_block? ──是──→ filter_block->AddKey(key)                             (:110-112)
 ├─ last_key = key; num_entries++                                              (:114-115)
 ├─ data_block.Add(key, value)                                                 (:116)
 └─ CurrentSizeEstimate() >= block_size(4KB)? ──是──→ Flush()                  (:118-121)
```

### 8.2 Flush（`table/table_builder.cc:124-138`）

```
Flush()
 ├─ data_block 为空则直接返回（:128）
 ├─ 断言 !pending_index_entry
 ├─ WriteBlock(&data_block, &pending_handle)   ← 落盘 + 记下 handle
 ├─ pending_index_entry = true
 ├─ file->Flush()（OS 层）
 └─ filter_block->StartBlock(r->offset)        ← 开始下一个 2KB filter 分组
```

### 8.3 WriteBlock / WriteRawBlock（`table/table_builder.cc:140-206`）

`WriteBlock`：`Finish()` 取原始 block → 按 `options.compression` 压缩（snappy/zstd，**节省不足 12.5% 时退回不压缩**，`:157-182`）→ `WriteRawBlock` → `block->Reset()`。
`WriteRawBlock`：`handle->set_offset(r->offset); set_size(contents.size())` → Append 数据 → Append 5 字节 trailer（type + masked CRC32C）→ `r->offset += n + 5`（`:189-206`）。

### 8.4 Finish 的顺序（`table/table_builder.cc:210-265`）

```
Finish()
 1. Flush()                                  ← 剩余 data block 落盘
 2. filter block  → WriteRawBlock(Finish(), kNoCompression)   （:219-222）
 3. metaindex block：
       key "filter.<Name>" → value = filter_block_handle 编码
       WriteBlock(&meta_index_block)                          （:225-238）
 4. index block：
       pending_index_entry? → FindShortSuccessor(last_key) 后 Add
       WriteBlock(&index_block)                               （:241-250）
 5. Footer{metaindex_handle, index_handle} → EncodeTo → Append（:253-263）
```

`Abandon()` 只置 `closed = true` 丢弃内容（`:267-271`）。析构断言 `closed` 捕获调用方忘记 Finish/Abandon（`:73-77`）。

---

## 9. Table::Open 与读取路径

### 9.1 Open（`table/table.cc:40-81`）

```cpp
Status Table::Open(const Options& options, RandomAccessFile* file, uint64_t size, Table** table) {
  *table = nullptr;
  if (size < Footer::kEncodedLength) return Status::Corruption("file is too short to be an sstable");
  char footer_space[Footer::kEncodedLength];
  Slice footer_input;
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_input, footer_space);
  ...
  Footer footer;
  s = footer.DecodeFrom(&footer_input);        // 校验 magic + 解两个 handle
  ...
  BlockContents index_block_contents;
  ReadOptions opt;
  if (options.paranoid_checks) opt.verify_checksums = true;
  s = ReadBlock(file, opt, footer.index_handle(), &index_block_contents);   // 读 index block
  if (s.ok()) {
    Block* index_block = new Block(index_block_contents);
    Rep* rep = new Table::Rep;
    rep->options = options;
    rep->file = file;
    rep->metaindex_handle = footer.metaindex_handle();
    rep->index_block = index_block;             // index block 整个常驻内存（Rep 析构时 delete）
    rep->cache_id = (options.block_cache ? options.block_cache->NewId() : 0);
    rep->filter_data = nullptr; rep->filter = nullptr;
    *table = new Table(rep);
    (*table)->ReadMeta(footer);                 // 懒加载 filter
  }
  return s;
}
```

读路径：**footer（尾部 48B）→ index block（唯一强制读取的中间层，常驻内存）→ data block（按需，可走 block cache）**。filter 通过 metaindex 懒加载（`ReadMeta`，`table/table.cc:83-110`；读不到不报错——filter 只是优化，`table.cc:96`）。

### 9.2 点查 InternalGet（`table/table.cc:213-241`）

```cpp
Status Table::InternalGet(const ReadOptions& options, const Slice& k, void* arg,
                          void (*handle_result)(void*, const Slice&, const Slice&)) {
  Iterator* iiter = rep_->index_block->NewIterator(rep_->options.comparator);
  iiter->Seek(k);                               // index 二分 → 目标 data block 的 handle
  if (iiter->Valid()) {
    Slice handle_value = iiter->value();
    FilterBlockReader* filter = rep_->filter;
    BlockHandle handle;
    if (filter != nullptr && handle.DecodeFrom(&handle_value).ok() &&
        !filter->KeyMayMatch(handle.offset(), k)) {
      /* 布隆判定不存在：跳过 block 读取 */
    } else {
      Iterator* block_iter = BlockReader(this, options, iiter->value());  // 见 6.3，cache 交互在此
      block_iter->Seek(k);
      if (block_iter->Valid()) (*handle_result)(arg, block_iter->key(), block_iter->value());
      s = block_iter->status();
      delete block_iter;
    }
  }
  ...
}
```

### 9.3 block cache 交互（`table/table.cc:167-184`）

- cache key = `cache_id(8B) + block offset(8B)` 共 16 字节（`table.cc:168-171`）；`cache_id` 由 `block_cache->NewId()` 分配（`table.cc:73`），防止多 DB 共享同一 cache 时文件号/偏移冲突；
- 命中 → 直接取 `Block*`（`table.cc:172-174`）；miss → `ReadBlock` → `Insert(key, block, block->size(), DeleteCachedBlock)`，charge 为 block 解压后大小（`table.cc:176-184`）；
- `fill_cache=false`（大扫描）时读但不插（`table.cc:179`）；`contents.cachable=false`（如底层文件直接返回 mmap 指针、非 heap 分配）时不插（`table/format.cc:103-116`）；
- 默认 block cache：未配置时 `NewLRUCache(8MB)`（`db/db_impl.cc:112-113`）。

### 9.4 ReadBlock 的 I/O 细节（`table/format.cc:69-162`）

读 `handle.size()+5` 字节 → 可选 CRC32C 校验（CRC 覆盖 data+type，`format.cc:93-101`）→ 按 type 分支：不压缩（`format.cc:104-119`）、snappy（`120-137`）、zstd（`138-155`）解压；输出 `BlockContents{data, cachable, heap_allocated}` 表明内存归属（堆上 buf 由 Block 析构释放，见 `table/block.cc:44-48`）。

### 9.5 ApproximateOffsetOf（`table/table.cc:243-266`）

index 二分 Seek 后取 handle.offset()；越界时返回 metaindex 偏移近似文件尾。供 compaction 估算层重叠/代价用。

---

## 10. builder.cc：memtable → SSTable 与 Compaction 的构建路径

### 10.1 BuildTable（`db/builder.cc:20-83`）

```cpp
Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  TableCache* table_cache, Iterator* iter, FileMetaData* meta) {
  iter->SeekToFirst();
  std::string fname = TableFileName(dbname, meta->number);
  if (iter->Valid()) {
    WritableFile* file; ...
    TableBuilder* builder = new TableBuilder(options, file);
    meta->smallest.DecodeFrom(iter->key());
    Slice key;
    for (; iter->Valid(); iter->Next()) { key = iter->key(); builder->Add(key, iter->value()); }
    if (!key.empty()) meta->largest.DecodeFrom(key);
    s = builder->Finish();
    ...
    s = file->Sync(); s = file->Close();
    if (s.ok()) {
      Iterator* it = table_cache->NewIterator(ReadOptions(), meta->number, meta->file_size);  // 验证可用
      s = it->status(); delete it;
    }
  }
  if (!iter->status().ok()) s = iter->status();
  if (!(s.ok() && meta->file_size > 0)) env->RemoveFile(fname);   // 失败清理
  return s;
}
```

BuildTable 本身**不决定丢弃**任何 key——它对传入迭代器全量倾倒；smallest/largest 从首尾 key 解码（`builder.cc:35, 41-43`）。

### 10.2 MemTable flush 用哪类迭代器（`db/db_impl.cc:502-544`）

```cpp
Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base) {
  ...
  Iterator* iter = mem->NewIterator();      // 单个 memtable 的有序跳表迭代器（无需 merge）
  s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
  ...
  if (base != nullptr) level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
  edit->AddFile(level, meta.number, meta.file_size, meta.smallest, meta.largest);
}
```

flush 时 memtable 内容**逐条全量写入**（memtable 内同一 user key 已按 sequence 合并过，见 MemTable 实现），不做 drop；输出层级由 `PickLevelForMemTableOutput` 决定（无重叠时可下推到 L2，`db/dbformat.h:38-41` 的 `kMaxMemCompactLevel=2`）。

### 10.3 Compaction 用哪类迭代器（`db/version_set.cc:1197-1227`）

```cpp
Iterator* VersionSet::MakeInputIterator(Compaction* c) {
  ReadOptions options;
  options.verify_checksums = options_->paranoid_checks;
  options.fill_cache = false;                 // 后台合并不污染 block cache
  // L0 文件必须合并在一起；其他层级则为每层创建一个串接迭代器。
  for (int which = 0; which < 2; which++) {
    if (!c->inputs_[which].empty()) {
      if (c->level() + which == 0) {
        for (每个 L0 文件) list[num++] = table_cache_->NewIterator(...);   // 每文件一个 Table 迭代器
      } else {
        list[num++] = NewTwoLevelIterator(
            new Version::LevelFileNumIterator(icmp_, &c->inputs_[which]),
            &GetFileIterator, table_cache_, options);   // 整层一个两级迭代器
      }
    }
  }
  Iterator* result = NewMergingIterator(&icmp_, list, num);   // k 路归并
}
```

L0 文件相互重叠，必须逐文件并入归并堆；L≥1 层内文件不重叠，用 `LevelFileNumIterator`（key=文件 smallest，value=文件号）做第一级的串接。调用点 `db/db_impl.cc:913`。

### 10.4 key 的保留/丢弃规则（`db/db_impl.cc:947-981`）

```cpp
bool drop = false;
if (!ParseInternalKey(key, &ikey)) { /* 错误 key 不隐藏 */ }
else {
  if (!has_current_user_key || user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) != 0) {
    current_user_key.assign(...); has_current_user_key = true;
    last_sequence_for_key = kMaxSequenceNumber;
  }
  if (last_sequence_for_key <= compact->smallest_snapshot) {
    drop = true;   // (A) 同一 user key 上已有更新的可见版本，且没有快照需要本版本
  } else if (ikey.type == kTypeDeletion && ikey.sequence <= compact->smallest_snapshot &&
             compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
    drop = true;   // (B) 删除墓碑：更高层无该 key 数据、更老版本随后会被 (A) 丢弃 → 墓碑作废
  }
  last_sequence_for_key = ikey.sequence;
}
```

`smallest_snapshot` = 最老存活快照的 sequence，无快照时为 `LastSequence()`（`db_impl.cc:908-910`）。MergingIterator 保证同一 user key 的版本按 **sequence 降序**依次出现（internal key 排序：user key 升序 + seq 降序，`db/dbformat.cc:50-66`），所以 (A) 规则只需跟踪 `last_sequence_for_key`。

`IsBaseLevelForKey`（`db/version_set.cc:1483-1502`）：检查 level+2 及更深层是否有文件的 [smallest.user_key, largest.user_key] 覆盖该 user key；无覆盖才允许丢墓碑。`ShouldStopBefore`（`db/version_set.cc:1504-1526`）：累计 grandparent 层重叠字节超过 `MaxGrandParentOverlapBytes` 时切分输出文件（配合 `db_impl.cc:939-945`）；另有 `FileSize() >= MaxOutputFileSize()` 切分（`db_impl.cc:1006-1013`）。

Compaction 输出不经过 `BuildTable`，而是 `OpenCompactionOutputFile`（`db/db_impl.cc:803-826`，直接 `new TableBuilder`）→ 循环 Add → `FinishCompactionOutputFile`（`db_impl.cc:828-862`，Finish/Sync/Close/验证）。

---

## 11. TableCache

### 11.1 key 的构成与 FindTable（`db/table_cache.cc:44-78`）

```cpp
Status TableCache::FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle** handle) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);            // key = 8 字节小端 file_number
  Slice key(buf, sizeof(buf));
  *handle = cache_->Lookup(key);
  if (*handle == nullptr) {
    std::string fname = TableFileName(dbname_, file_number);
    RandomAccessFile* file = nullptr; Table* table = nullptr;
    s = env_->NewRandomAccessFile(fname, &file);
    if (!s.ok()) {  // 兼容旧文件名
      std::string old_fname = SSTTableFileName(dbname_, file_number);
      if (env_->NewRandomAccessFile(old_fname, &file).ok()) s = Status::OK();
    }
    if (s.ok()) s = Table::Open(options_, file, file_size, &table);
    if (!s.ok()) { delete file; /* 不缓存错误结果 */ }
    else {
      TableAndFile* tf = new TableAndFile;    // {RandomAccessFile*, Table*}
      *handle = cache_->Insert(key, tf, 1, &DeleteEntry);   // charge=1 → 按"打开表数量"计容量
    }
  }
  return s;
}
```

value 是 `TableAndFile{file, table}`（`table_cache.cc:17-20`），被驱逐时 `DeleteEntry` 一并销毁（`:22-27`）。**key 只有 file_number**（cache 是每 DB 独立的）。容量 = `max_open_files - 10`（`db_impl.cc:118-121`），cache 用 `NewLRUCache(entries)`（`table_cache.cc:40`）。

### 11.2 Get / NewIterator 流程（`db/table_cache.cc:80-114`）

```cpp
Iterator* TableCache::NewIterator(...) {
  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) return NewErrorIterator(s);
  Table* table = ...->table;
  Iterator* result = table->NewIterator(options);
  result->RegisterCleanup(&UnrefEntry, cache_, handle);   // 迭代器存活期间 pin 住 TableAndFile
  if (tableptr != nullptr) *tableptr = table;
  return result;
}

Status TableCache::Get(...) {
  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (s.ok()) {
    Table* t = ...;
    s = t->InternalGet(options, k, arg, handle_result);   // 点查（含布隆）
    cache_->Release(handle);
  }
  return s;
}
```

`Evict(file_number)` = `cache_->Erase`（`table_cache.cc:116-120`）。被驱逐/Erase 的 Table 若仍有迭代器引用，由 handle pin 住（LRU 语义），真正的释放发生在最后一个迭代器析构时。

### 11.3 底层 LRU 实现（`util/cache.cc`）

- **16 路分片**：`kNumShardBits=4`（`cache.cc:332-333`），按 key 的 hash 高 4 位路由到分片（`cache.cc:345`），每分片独立 mutex，降低锁竞争；
- 每分片 = 手写链式哈希表 `HandleTable`（`cache.cc:65-140`，桶数 ≥ 元素数保证平均链长 ≤ 1）+ 双循环链表：
  - `lru_` 链表：`refs==1 && in_cache`，按最近使用排序（`lru.prev` 最新，`lru.next` 最旧，`cache.cc:183-185`）；
  - `in_use_` 链表：`refs>=2 && in_cache`，正被客户端引用的条目（`cache.cc:187-189`）；
- `Ref()`（`cache.cc:214-220`）：refs 1→2 时从 lru 移入 in_use；`Unref()`（`222-234`）：refs 2→1 时移回 lru，refs 1→0（且已出 cache）时调 deleter 释放；
- `Lookup` 命中即 Ref（`cache.cc:249-256`）；`Insert` 超容量时循环驱逐 `lru_.next`（最旧，`cache.cc:290-297`）——**只驱逐未被引用的条目**，被引用的在 in_use 链表里不受影响；
- `Erase`/`Prune`（`cache.cc:315-330`）从哈希表摘除后 `FinishErase`，实际释放延迟到引用归零。

---

## 12. 与 Stratum 时间轴需求的映射建议

### 12.1 单调追加写入下，前缀压缩对 TimeKey 的效果

- **结论：效果很好，且比 LevelDB 典型负载更好。** 前缀压缩收益取决于相邻 key 的公共前缀长度（`table/block_builder.cc:76-81`）。Stratum 写是单调追加：相邻 key 的时间戳部分（unix 秒/纳秒的高位十进制或大端二进制）连续，设备/源 id 相同，公共前缀可达 TimeKey 总长的 70-90%，`shared` 大、`non_shared` 小。
- **注意点 1**：LevelDB 的 internal key = `user_key + 8B tag(sequence<<8|type)`（`db/dbformat.cc:24-27`）。memtable flush 时相邻 entry 的 sequence 各不相同，tag 不同，共享前缀会在 user key 末尾处截断——user key 部分的压缩不受影响，但 8 字节 tag 无压缩。若 Stratum 保证一 key 一版本（时间戳本身唯一），**可以去掉 tag 或让 tag 固定**，前缀压缩将直达 key 末尾，还能让"同一时间戳的多版本"问题根本不存在。
- **注意点 2**：restart interval 默认 16（`include/LSMengine/options.h:90`）是"4KB block / ~250B entry"时代的经验值；TimeKey 若较长（如 24-32 字节），16 依然合理（restart 数组 4B/key 的开销远小于 key 本身）；若 TimeKey 很短（8 字节），可考虑加大 interval 减少 restart 数组开销。⚠️ **待确认**：Stratum 的 TimeKey 具体字节布局（定长/变长、含不含源 id）与平均 value 大小，这决定 block_size/restart_interval 的调参起点。
- **注意点 3**：索引键缩短（FindShortestSeparator/Successor，第 6 节）对定长二进制时间戳同样有效——"0x0000013A2B" 与下一块首 key 之间取 "0x0000013A2C" 只需保留前缀+递增 1 字节（`util/comparator.cc:33-54`）。TimeKey 用**大端二进制**编码时缩短收益最大；小端编码会导致公共前缀出现在 key 尾部，前缀压缩与分隔键缩短双双失效——TimeKey 应设计为"前缀即时间顺序"（即大端）。

### 12.2 时间范围查询 [begin_ts, end_ts) → internal key 范围

- SSTable 内排序 = user key 升序（时间戳升序）+ 同 key sequence 降序（`db/dbformat.cc:50-66`），所以范围查询就是一次有序扫描：
  1. **下界**：构造 internal key `begin_ts + PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek)`（`db/dbformat.cc:55-56` 的 `kValueTypeForSeek` 正为此设计），`iter->Seek(该 key)` 恰好落在 ≥ begin_ts 的第一个（其最新版本）；
  2. **上界（排他）**：迭代直到 `ExtractUserKey(key) >= end_ts`（即 `user_key.compare(end_ts) >= 0`）时停止；或直接 `Seek(end_ts + 最小 tag)` 后不再前进。
- 执行路径：DB 层合并迭代器（L0 归并 + 各层 TwoLevelIterator，见 10.3）→ 每张表内 `index_iter_.Seek(begin_ts)` 定位首个 data block → 跨 block 时 `SkipEmptyDataBlocksForward` 自动衔接（`table/two_level_iterator.cc:84-128`）。**整个范围查询不需要任何额外机制**，TwoLevelIterator 天然是"范围扫描友好"的结构。
- ⚠️ **待确认**：Stratum 是否需要多版本（同一 ts 多个写入方/多次覆盖）。若是"append-only 且时间戳唯一"，internal key 的 sequence/type 维度可整体省略，范围映射退化为纯字节序比较。

### 12.3 是否需要布隆过滤器？

- **参考实现的约束**：布隆只在**点查路径**生效——`Table::InternalGet` 在读 data block 前查 `filter->KeyMayMatch`（`table/table.cc:223-225`）；`NewIterator` 的扫描路径**完全不查布隆**（`table/table.cc:207-211`）。且 FilterBlockBuilder 按 **2KB 文件偏移**分组（`table/filter_block.cc:16-17`），一个 filter 覆盖 2KB 数据段，不是按 key 范围索引。
- **Stratum 的判断**：若查询负载以 `[begin_ts, end_ts)` 范围扫描为主，布隆**收益接近于零**（扫描路径根本不走 filter），反而付出：每个 key 一次 AddKey 的内存与 CPU（`table/table_builder.cc:110-112`）、每表一个不压缩的 filter block（`table_builder.cc:219-222`）约占数据 1-2%（bits_per_key=10）+ metaindex 一个 block。**建议：范围扫描为主的 Stratum 默认关闭 filter_policy。**
- **例外**：若存在"精确点查某 ts+源"的高频操作（如重复读取同一时间点数据、或上层对最近时间点做点查），布隆能把"miss 时的一次 4KB block 读"省掉；此时 `bits_per_key=10`（约 1% 假阳性，`filter_policy.h:45-47`）即可。⚠️ **待确认**：Stratum 的实际查询混合（点查占比），以及是否允许"范围扫描内嵌点查"模式。
- **可参考的替代**：若想加速"判断某时间区间内是否有数据"，LevelDB 的 filter 设计帮不上忙（它不索引 key 范围），更合适的是 index block 的 `ApproximateOffsetOf`（`table/table.cc:243-266`）式二分，或自行在 index key 中携带时间下界做区间剪枝。

### 12.4 其他可直接复用的设计要点（供重实现参考）

- 48 字节定长 footer + magic 校验是文件格式自描述的关键（`table/format.h:51,74`）；重实现时建议保留"定长尾部 + magic"模式（可换 magic 值）。
- 每 block 5 字节 trailer（type+CRC32C）使单 block 可独立校验，损坏不会扩散（`table/format.cc:93-101`）。
- block cache key = `cache_id + offset`（`table/table.cc:168-171`）与 TableCache key = `file_number`（`table_cache.cc:47-49`）两层 cache 解耦了"打开表状态"与"解压后 block 内容"；LRU 用 pin/refcount 解决"迭代器持有中被驱逐"（`util/cache.cc:214-234`）。
- 范围扫描应设 `fill_cache=false`（`db/version_set.cc:1200` 的后台合并即如此）以避免热数据被扫出 cache；Stratum 在线范围扫描是否要进 cache 需按工作集判定。⚠️ **待确认**。

---

### 附：行号引用速查（源码位置索引）

| 主题 | 文件:行号 |
|---|---|
| 文件布局（官方文档） | `doc/table_format.md:4-45` |
| block trailer / CRC | `table/table_builder.cc:189-206`, `table/format.cc:93-101` |
| Footer 48B + magic | `table/format.h:51,71-77`, `table/format.cc:32-67` |
| BlockHandle varint | `table/format.cc:16-30`, `util/coding.cc:57-72,118-133` |
| Block 格式注释 | `table/block_builder.cc:5-23` |
| BlockBuilder Add/Finish | `table/block_builder.cc:59-103` |
| restart 默认间隔 / block 大小 | `include/LSMengine/options.h:86,90` |
| Block 迭代器二分 | `table/block.cc:165-223,246-273` |
| index entry 生成（延迟+缩短） | `table/table_builder.cc:53-57,101-108,242-248` |
| 分隔键算法 | `util/comparator.cc:33-68`, `db/dbformat.cc:68-100` |
| TwoLevelIterator | `table/two_level_iterator.cc:84-162` |
| BlockReader + cache key | `table/table.cc:153-205` |
| Bloom 生成/查询 | `util/bloom.cc:15-78` |
| FilterBlock 格式/读 | `table/filter_block.cc:16-105`, `doc/table_format.md:47-79` |
| InternalFilterPolicy | `db/dbformat.cc:102-117` |
| TableBuilder Finish 顺序 | `table/table_builder.cc:210-265` |
| Table::Open | `table/table.cc:40-81` |
| InternalGet 布隆接入 | `table/table.cc:213-241` |
| BuildTable | `db/builder.cc:20-83` |
| flush 迭代器来源 | `db/db_impl.cc:502-544` |
| compaction 输入迭代器 | `db/version_set.cc:1197-1227`, `db/db_impl.cc:913` |
| key drop 规则 | `db/db_impl.cc:947-981`, `db/version_set.cc:1483-1526` |
| TableCache | `db/table_cache.cc:44-120`, `db/db_impl.cc:118-121` |
| LRU cache | `util/cache.cc:332-397` |
