# 参考实现深读笔记：LSMengine 内存层与键格式（ref-01）

- **对象**：`/UserData/LeaningLevelDB/LSMengine`（LevelDB 的完整翻版，命名空间 `LSMengine`）
- **范围**：`db/dbformat.{h,cc}`、`db/memtable.{h,cc}`、`db/skiplist.h`、`db/write_batch.cc`、`db/write_batch_internal.h`、`util/arena.{h,cc}`、`util/coding.{h,cc}`、`util/crc32c.{h,cc}`；辅助引用 `port/port_stdcxx.h`、`util/random.h`、`db/snapshot.h`、`db/db_impl.cc`（Write/Get 路径）、`include/LSMengine/comparator.h`、`include/LSMengine/write_batch.h`、`CMakeLists.txt`、`build/CMakeCache.txt`、`build/include/port/port_config.h`、`db/log_writer.cc`、`db/log_reader.cc`、`table/table_builder.cc`、`table/format.cc`
- **目标**：为"重新实现一个更强的 DB 层"（Stratum 时间轴需求）提供精确设计依据。只读源码，未修改任何参考实现文件。
- **行号约定**：所有 `文件:行` 均指本仓库该文件的物理行号；引用的代码注释保留仓库原文（原文即中文注释）。

---

## 1. InternalKey 格式

### 1.1 ValueType 与 tag 的位分配

```cpp
// db/dbformat.h:51-56
// 编码在 internal key 最后部分的 value 类型。
// 不要修改这些枚举值：它们嵌入磁盘上的数据结构。
enum ValueType { kTypeDeletion = 0x0, kTypeValue = 0x1 };
// kValueTypeForSeek 定义构造 ParsedInternalKey、定位到指定 sequence number 时
// 应传入的 ValueType。由于 sequence 按降序排列，且 value type 嵌入 internal key
// sequence 的低 8 位，因此必须使用编号最高的 ValueType，而不是最低的。
static const ValueType kValueTypeForSeek = kTypeValue;
```

```cpp
// db/dbformat.h:58-61
typedef uint64_t SequenceNumber;
// 底部保留 8 位，使 type 和 sequence# 可以打包到 64 位中。
static const SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);
```

- **tag = uint64**：`(sequence << 8) | type`。sequence 占 bit [8..63]（实际上限 56 位），type 占 bit [0..7]。见 `db/dbformat.cc:18-22`：

```cpp
// db/dbformat.cc:18-22
static uint64_t PackSequenceAndType(uint64_t seq, ValueType t) {
  assert(seq <= kMaxSequenceNumber);
  assert(t <= kValueTypeForSeek);
  return (seq << 8) | t;
}
```

- 因为 `kMaxSequenceNumber = (1<<56)-1`，合法 tag 的 bit [56..63] 恒为 0。
- **编码 = user_key ∥ fixed64(tag)**（定长 8 字节 tag 紧跟 user key 之后），`db/dbformat.cc:24-27`：

```cpp
void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}
```

- 编码后长度：`db/dbformat.h:75-77`

```cpp
inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
  return key.user_key.size() + 8;
}
```

- 解析 `db/dbformat.h:164-174`：取末尾 8 字节解码为 uint64，`type = num & 0xff`，`sequence = num >> 8`，user_key 是前缀；type 合法性检查为 `c <= kTypeValue`（即 0 或 1）：

```cpp
inline bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result) {
  const size_t n = internal_key.size();
  if (n < 8) return false;
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  uint8_t c = num & 0xff;
  result->sequence = num >> 8;
  result->type = static_cast<ValueType>(c);
  result->user_key = Slice(internal_key.data(), n - 8);
  return (c <= static_cast<uint8_t>(kTypeValue));
}
```

- 提取 user key：`db/dbformat.h:89-92`（`ExtractUserKey`：去掉末尾 8 字节，`assert(size >= 8)`）。
- 封装类 `InternalKey`（`db/dbformat.h:127-157`）：内部就是 `std::string rep_`；`DecodeFrom`/`Encode`/`user_key`/`SetFrom`/`Clear`；注释明确要求目录内模块不要直接拿字符串比较，避免误用普通字符串比较而非 `InternalKeyComparator`（`db/dbformat.h:125-126`）。

### 1.2 端序：固定小端（little-endian）

`EncodeFixed64` 是显式逐字节小端写入，与主机端序无关（`util/coding.h:61-73`）：

```cpp
inline void EncodeFixed64(char* dst, uint64_t value) {
  uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);
  // 较新的 clang 和 gcc 会将其优化为一条 mov / str 指令。
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  ...
  buffer[7] = static_cast<uint8_t>(value >> 56);
}
```

### 1.3 为什么"sequence 降序 = 字节序"成立（具体字节举例）

关键链条：

1. **单整数打包**：`(seq << 8) | type` 把 (sequence, type) 折叠成**一个** uint64，seq 在高位 → 该整数数值序 = 先比 seq、再比 type。
2. **小端 fixed64**：8 字节 tag 的**字节字典序（memcmp）== 数值序**（小端存储时低字节在前，多字节整数按字节递增排列）。因此"tag 数值比较"和"tag 字节比较"等价。
3. **比较器取反**：`InternalKeyComparator` 对 tag 数值比较结果取反（见 §2），于是"seq 数值降序"直接成为排序序 —— 新版本（seq 大）排在旧版本前面。

具体字节示例（user_key = `"foo"` = `66 6f 6f`）：

| 条目 | seq | type | tag 数值 | tag 小端字节 |
|---|---|---|---|---|
| A | 10 | Value(1) | `0x0a01` = 2561 | `01 0a 00 00 00 00 00 00` |
| B | 9  | Value(1) | `0x0901` = 2305 | `01 09 00 00 00 00 00 00` |
| C | 5  | Deletion(0) | `0x0500` = 1280 | `00 05 00 00 00 00 00 00` |

- 完整 internal key A = `66 6f 6f 01 0a 00 00 00 00 00 00`（3+8=11 字节）。
- 数值：2561 > 2305 > 1280；字节序：第 1 字节 `0a > 09`，第 0 字节 `01 > 00` —— memcmp 与数值比较结论一致。
- 比较器把"数值大"映射为"排序靠前"（`db/dbformat.cc:59-63`：`anum > bnum → r = -1`），于是 A（最新）在最前。
- 同 seq 的 type 决胜：`(s, Value=1) > (s, Deletion=0)`。`kValueTypeForSeek = kTypeValue = 1`（`db/dbformat.h:53-56`）正是利用这一点：**LookupKey 的 tag 取 (seq, 1)**，它排在"所有 seq 相同、type≤1 的真实条目"之前 → Seek 直接落在"seq ≤ 目标 seq 的第一条"上，见 §3.3 的 `MemTable::Get` 注释"无需再检查 sequence"。

### 1.4 LookupKey 布局

```cpp
// db/dbformat.h:197-206
// 构造如下形式的 char 数组：
//    klength  varint32               <-- start_
//    userkey  char[klength]          <-- kstart_
//    tag      uint64
//                                    <-- end_
// 该数组可作为 MemTable key；从 "userkey" 开始的后缀可作为 InternalKey。
```

构造实现 `db/dbformat.cc:119-136`：

```cpp
LookupKey::LookupKey(const Slice& user_key, SequenceNumber s) {
  size_t usize = user_key.size();
  size_t needed = usize + 13;  // 保守估计
  char* dst;
  if (needed <= sizeof(space_)) {
    dst = space_;
  } else {
    dst = new char[needed];
  }
  start_ = dst;
  dst = EncodeVarint32(dst, usize + 8);
  kstart_ = dst;
  std::memcpy(dst, user_key.data(), usize);
  dst += usize;
  EncodeFixed64(dst, PackSequenceAndType(s, kValueTypeForSeek));
  dst += 8;
  end_ = dst;
}
```

要点：
- 布局 = `varint32(len(user_key)+8) ∥ user_key ∥ fixed64 tag(s, kValueTypeForSeek)`。
- `needed = usize + 13` 是保守估计：varint32 最坏 5 字节 + tag 8 字节。
- `space_[200]` 栈上快速路径，短 key 不分配堆（`db/dbformat.h:206`）；`~LookupKey` 只释放堆分配（`db/dbformat.h:209-211`）。
- 三个视图（`db/dbformat.h:187-194`）：`memtable_key()` = 全串（带长度前缀，给 MemTable 用）；`internal_key()` = 从 `kstart_` 起（无长度前缀，给 internal iterator / SST 用）；`user_key()` = 去掉末尾 8 字节。
- 拷贝构造被删除（`db/dbformat.h:182-183`），避免悬挂指针。

---

## 2. InternalKeyComparator

### 2.1 比较顺序与实现

```cpp
// db/dbformat.cc:50-66
int InternalKeyComparator::Compare(const Slice& akey, const Slice& bkey) const {
  // 排序顺序：
  //    user key 升序（按用户提供的 comparator）；
  //    sequence number 降序；
  //    type 降序（虽然 sequence# 应已足以消除歧义）。
  int r = user_comparator_->Compare(ExtractUserKey(akey), ExtractUserKey(bkey));
  if (r == 0) {
    const uint64_t anum = DecodeFixed64(akey.data() + akey.size() - 8);
    const uint64_t bnum = DecodeFixed64(bkey.data() + bkey.size() - 8);
    if (anum > bnum) {
      r = -1;
    } else if (anum < bnum) {
      r = +1;
    }
  }
  return r;
}
```

- 实现方式是"user key 比较 + tag 数值比较取反"，**不是**对整串 memcmp（tag 的小端布局使二者等价，但代码选择了显式解码+取反的写法）。
- `Name()` 固定为 `"leveldb.InternalKeyComparator"`（`db/dbformat.cc:46-48`）；持有 `user_comparator_`（`db/dbformat.h:96-111`）。
- 重载 `Compare(const InternalKey&, const InternalKey&)` 直接比较编码串（`db/dbformat.h:159-162`）。
- `FindShortestSeparator` / `FindShortSuccessor`（`db/dbformat.cc:68-100`）：只对 **user key 部分**调用 user comparator 的缩短函数；缩短成功（物理变短且逻辑变大）后**追加最小可能的 tag** `(kMaxSequenceNumber, kValueTypeForSeek)`，并用 `assert(Compare(start, tmp) < 0)` 保证不越界：

```cpp
// db/dbformat.cc:74-84（FindShortestSeparator 核心）
user_comparator_->FindShortestSeparator(&tmp, user_limit);
if (tmp.size() < user_start.size() &&
    user_comparator_->Compare(user_start, tmp) < 0) {
  // user key 的物理长度变短，但逻辑顺序变大；为缩短后的 user key 追加
  // 最小可能的编号。
  PutFixed64(&tmp, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
  assert(this->Compare(*start, tmp) < 0);
  assert(this->Compare(tmp, limit) < 0);
  start->swap(tmp);
}
```

### 2.2 为什么这样设计

- **同一 user key 的多个版本按 seq 降序紧邻**：最新可见版本总是排在最前。任何基于比较的定位（跳表 Seek、SST 二分）一次就能命中"最新版本或它的删除标记"。
- **type 降序**是防御性的：seq 相同仅可能在构造的 LookupKey 与真实条目之间发生；`kValueTypeForSeek=1` 保证 seek 目标（seq,1）排在真实条目（seq,0/1）之前，使"≤seq 的第一条"的语义严格成立。
- **对 Get 的影响**：`DBImpl::Get` 用 `LookupKey lkey(key, snapshot)` 直接 Seek（`db/db_impl.cc:1144`），MemTable 命中即返回（`db/memtable.cc:104-137`），无需扫描多个版本。
- **对迭代的影响**：迭代器看到的 key 流天然按"user key 升序、版本从新到旧"排列；上层（如 `db_iter.cc`，不在本次范围）负责跳过 seq 高于 snapshot 的版本。Compaction 同样依赖这个序做版本裁剪（`db/db_impl.cc:923-989`，`last_sequence_for_key` 过滤逻辑）。
- **对 Compaction/GC 的影响**：snapshot 链表中的最旧 sequence 是"能丢弃的历史版本边界"（`db/db_impl.cc:63-74, 907-910`），见 §9 对时间轴语义的讨论。

---

## 3. MemTable

### 3.1 数据结构：skiplist + refs 计数

```cpp
// db/memtable.h:24-81（节选）
class MemTable {
 public:
  // MemTable 使用引用计数。初始引用计数为 0，调用者必须至少调用一次 Ref()。
  explicit MemTable(const InternalKeyComparator& comparator);
  void Ref() { ++refs_; }
  void Unref() {
    --refs_;
    assert(refs_ >= 0);
    if (refs_ <= 0) { delete this; }
  }
  ...
 private:
  struct KeyComparator {
    const InternalKeyComparator comparator;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
    int operator()(const char* a, const char* b) const;
  };
  typedef SkipList<const char*, KeyComparator> Table;
  ~MemTable();  // 私有析构：只能通过 Unref() 删除
  KeyComparator comparator_;
  int refs_;
  Arena arena_;
  Table table_;
};
```

- 跳表的 key 类型是 `const char*`（指向 Arena 中编码好的 entry），比较器 `KeyComparator` 先把指针解成 length-prefixed slice 再走 `InternalKeyComparator`（`db/memtable.cc:31-37`）。
- 构造把 `comparator_` 与 `&arena_` 传入 `table_`（`db/memtable.cc:24-25`）；析构断言 `refs_ == 0`（`db/memtable.cc:27`）。
- `ApproximateMemoryUsage` = `arena_.MemoryUsage()`（`db/memtable.cc:29`）。

### 3.2 Add 的实现要点

```cpp
// db/memtable.cc:78-102
void MemTable::Add(SequenceNumber s, ValueType type, const Slice& key,
                   const Slice& value) {
  // entry 由以下字段顺序拼接而成：
  //  key_size     : internal_key.size() 的 varint32
  //  key bytes    : char[internal_key.size()]
  //  tag          : uint64((sequence << 8) | type)
  //  value_size   : value.size() 的 varint32
  //  value bytes  : char[value.size()]
  size_t key_size = key.size();
  size_t val_size = value.size();
  size_t internal_key_size = key_size + 8;
  const size_t encoded_len = VarintLength(internal_key_size) +
                             internal_key_size + VarintLength(val_size) +
                             val_size;
  char* buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, internal_key_size);
  std::memcpy(p, key.data(), key_size);
  p += key_size;
  EncodeFixed64(p, (s << 8) | type);
  p += 8;
  p = EncodeVarint32(p, val_size);
  std::memcpy(p, value.data(), val_size);
  assert(p + val_size == buf + encoded_len);
  table_.Insert(buf);
}
```

- 实际字节布局（以代码为准，注释里 tag 单独列一行但实际计入 `internal_key_size`）：

```
entry := varint32(user_key_len + 8)
         ∥ user_key 字节
         ∥ fixed64((seq << 8) | type)   // 注意：这里直接 (s << 8) | type，没有走 PackSequenceAndType
         ∥ varint32(value_len)
         ∥ value 字节
```

- 一次 Arena 分配、一次跳表 Insert；entry 内 key 是 `user_key + tag`，与 §1 的 InternalKey 布局一致（这正是 iterator `key()` 能直接当 internal key 返回的原因）。
- 删除条目复用同一布局，`value` 为空（`db/memtable.cc:5-6` 注释）。

### 3.3 Get 的实现要点（一次 Seek，不检查 sequence）

```cpp
// db/memtable.cc:104-137
bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
  Slice memkey = key.memtable_key();
  Table::Iterator iter(&table_);
  iter.Seek(memkey.data());
  if (iter.Valid()) {
    // entry 格式如下：
    //    klength  varint32
    //    userkey  char[klength]
    //    tag      uint64
    //    vlength  varint32
    //    value    char[vlength]
    // 检查它是否属于同一个 user key。无需再检查 sequence，因为上面的 Seek()
    // 应当已经跳过 sequence 过大的所有 entry。
    const char* entry = iter.key();
    uint32_t key_length;
    const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);
    if (comparator_.comparator.user_comparator()->Compare(
            Slice(key_ptr, key_length - 8), key.user_key()) == 0) {
      // 找到了正确的 user key。
      const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
      switch (static_cast<ValueType>(tag & 0xff)) {
        case kTypeValue: {
          Slice v = GetLengthPrefixedSlice(key_ptr + key_length);
          value->assign(v.data(), v.size());
          return true;
        }
        case kTypeDeletion:
          *s = Status::NotFound(Slice());
          return true;
      }
    }
  }
  return false;
}
```

- Seek 目标是 LookupKey（tag = (seq, kValueTypeForSeek)）；由于排序序（§2），Seek 直接落在"user key 相同且 seq ≤ 目标 seq 的第一条"，**因此不需要再比对 sequence**（`db/memtable.cc:115-116` 注释）。
- user key 相等用 `user_comparator()->Compare` 判（`key_length - 8` 剥掉 tag）。
- 命中 `kTypeValue` → 返回 value；命中 `kTypeDeletion` → 返回 true + `Status::NotFound`；都不是（user key 不同）→ 返回 false，由调用方（`db/db_impl.cc:1145-1152`）继续查 imm / SST。
- `GetLengthPrefixedSlice`（`db/memtable.cc:17-22`）用 `GetVarint32Ptr(p, p+5, &len)`，`+5` 是因为 varint32 最长 5 字节、且"假定 p 未损坏"（内存内数据，可信）。

### 3.4 迭代器

`MemTableIterator`（`db/memtable.cc:48-76`）：包装 `Table::Iterator`；`Seek` 前用 `EncodeKey` 给目标 internal key 加长度前缀（`db/memtable.cc:41-46`）；`key()` 返回 internal key，`value()` 是 key 之后第二个 length-prefixed slice：

```cpp
// db/memtable.cc:63-67
Slice key() const override { return GetLengthPrefixedSlice(iter_.key()); }
Slice value() const override {
  Slice key_slice = GetLengthPrefixedSlice(iter_.key());
  return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
}
```

### 3.5 并发模型：为什么读不加锁

头文件注释即结论（`db/memtable.h:5-6`）："DBImpl 为修改操作提供同步，而读操作可以无锁执行"。完整依据链：

1. **单写者**：所有写通过 `DBImpl` 的 writers 队列串行化（`db/db_impl.cc:1209-1213`），同一时刻只有一个线程执行 `mem_->InsertInto`（`db/db_impl.cc:1227-1230` 注释：释放 mutex 期间 `&w` 是唯一 logger/memtable 写者）。
2. **节点不可变、永不删除**：跳表不变量 (1)(2)（`db/skiplist.h:19-26`，见 §4.5）。
3. **发布-读取内存序**：节点链入用 release-store，读者跟随指针用 acquire-load（`db/skiplist.h:144-153`）→ 读者看到的节点必定已完整初始化。
4. **内存不回收**：Arena 只追加、不单独 free（`util/arena.h:16-18`），读者持有的任何指针在 MemTable 存活期内都有效。
5. **生命周期由 refs 保证**：读路径在 mutex 内取指针并 `Ref()`，解锁后无锁读，读完 `Unref()`（`db/db_impl.cc:1130-1135, 1140-1144, 1159`）→ 读期间 MemTable 不会被析构（`db/memtable.h:36-42`）。
6. `MemTable::Get` 全程无锁、无原子操作（`db/memtable.cc:104-137`）；用到的 comparator 无状态（`db/memtable.cc:31-37`）。

---

## 4. 跳表（db/skiplist.h）

### 4.1 节点结构（next 数组、height）

```cpp
// db/skiplist.h:138-168
template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}
  Key const key;
  // link 的访问器/修改器。通过方法封装，以便按需加入正确的内存屏障。
  Node* Next(int n) {
    assert(n >= 0);
    // 使用 acquire load，确保看到返回 Node 的完整初始化版本。
    return next_[n].load(std::memory_order_acquire);
  }
  void SetNext(int n, Node* x) {
    assert(n >= 0);
    // 使用 release store，确保通过此指针读取的线程看到已完整初始化的插入节点。
    next_[n].store(x, std::memory_order_release);
  }
  ...
 private:
  // 长度等于节点高度的数组；next_[0] 是最低层链接。
  std::atomic<Node*> next_[1];
};
```

- `next_[1]` 是柔性数组：节点实际按 `height` 个指针分配；`key` 不可变。
- 还有无屏障变体 `NoBarrier_Next/NoBarrier_SetNext`，供"已由外部同步"的安全位置使用（`db/skiplist.h:155-163`）。

### 4.2 Arena 管理节点内存

```cpp
// db/skiplist.h:170-176
Node* NewNode(const Key& key, int height) {
  char* const node_memory = arena_->AllocateAligned(
      sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
  return new (node_memory) Node(key);
}
```

- 节点由外部注入的 Arena 分配（`db/skiplist.h:123-125`），对齐分配（见 §6.3）；注释明确"对象必须在 skiplist 整个生命周期内保持分配状态"（`db/skiplist.h:44-45`）。
- MemTable 场景下 Arena 与 MemTable 同生命周期（`db/memtable.h:79-80`），refs 保证读期间不析构（§3.5）。
- 头节点：构造时以 `kMaxHeight` 高度创建，key 随意（`db/skiplist.h:312-322`）：

```cpp
SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp),
      arena_(arena),
      head_(NewNode(0 /* 任意 key 均可 */, kMaxHeight)),
      max_height_(1),
      rnd_(0xdeadbeef) {
  for (int i = 0; i < kMaxHeight; i++) head_->SetNext(i, nullptr);
}
```

### 4.3 RandomHeight

```cpp
// db/skiplist.h:98 与 230-240
enum { kMaxHeight = 12 };
int SkipList<Key, Comparator>::RandomHeight() {
  // 以 1/kBranching 的概率增加高度。
  static const unsigned int kBranching = 4;
  int height = 1;
  while (height < kMaxHeight && rnd_.OneIn(kBranching)) {
    height++;
  }
  ...
  return height;
}
```

- 几何分布：`P(height = h) = (1/4)^(h-1) · 3/4`（h < 12 时），最大 12 层。
- 随机源是极简 LCG `Random`（`util/random.h:26-44`，`A=16807, M=2^31-1`），`OneIn(n)` = `Next() % n == 0`（`util/random.h:48-49`）；"并发修改不安全；引擎中的每个拥有者都使用自己的生成器"（`util/random.h:12-14`）——每个 SkipList 实例一个 `rnd_`（`db/skiplist.h:132-133`），且只有单写者访问。

### 4.4 FindGreaterOrEqual 实现要点

```cpp
// db/skiplist.h:249-269
Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) {
      // 继续在当前层级搜索。
      x = next;
    } else {
      if (prev != nullptr) prev[level] = x;
      if (level == 0) {
        return next;
      } else {
        // 切换到下一层链表。
        level--;
      }
    }
  }
}
```

- 自顶向下：`next` 为 null 视为正无穷（`KeyIsAfterNode`，`db/skiplist.h:243-246`）；每层记录前驱 `prev[level]`（供 Insert 使用）；层 0 返回"第一个 key ≥ 目标"的节点（不存在返回 nullptr）。
- 配套 `FindLessThan`（`db/skiplist.h:271-290`）与 `FindLast`（`db/skiplist.h:292-310`）同构。
- `Iterator::Prev` 没有 prev 指针，靠 `FindLessThan` 重新搜索（`db/skiplist.h:201-209`）→ **Prev 是 O(log n) 而不是 O(1)**（对设计时间轴迭代方向时有意义）。

### 4.5 Insert 与无锁读的前提

```cpp
// db/skiplist.h:324-353
void SkipList<Key, Comparator>::Insert(const Key& key) {
  // TODO(opt)：此处 Insert() 已由外部同步，因此可以使用无屏障版 FindGreaterOrEqual()。
  Node* prev[kMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);
  // 此数据结构不允许重复插入。
  assert(x == nullptr || !Equal(key, x->key));
  int height = RandomHeight();
  if (height > GetMaxHeight()) {
    for (int i = GetMaxHeight(); i < height; i++) {
      prev[i] = head_;
    }
    // 可以在不与并发 reader 同步的情况下修改 max_height_。看到新值的 reader
    // 可能看到 head_ 中新层指针的旧值（nullptr），也可能看到下面循环写入的新值。
    // 前一种情况下，reader 会立即降到下一层，因为 nullptr 排在所有 key 之后；
    // 后一种情况下，reader 会使用新节点。
    max_height_.store(height, std::memory_order_relaxed);
  }
  x = NewNode(key, height);
  for (int i = 0; i < height; i++) {
    // NoBarrier_SetNext() 已足够，因为在 prev[i] 中发布指向 "x" 的指针时
    // 会再加入内存屏障。
    x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
    prev[i]->SetNext(i, x);
  }
}
```

- 插入顺序：先完全初始化节点（key 写入、`next_` 用 NoBarrier 赋值），再自底向上用 **release-store** 把节点挂进每层链表（`prev[i]->SetNext(i, x)`）。读者要么看不到节点，要么看到完整节点。
- `max_height_` 是 `std::atomic<int>`，仅 Insert 修改、relaxed 读（`db/skiplist.h:129-130, 100-102`）；注释论证了读者读到旧高度也安全（会从 head 直接降层）。

无锁读的全部前提（`db/skiplist.h:11-27` 头注释）：

```
// 写操作需要外部同步，通常是 mutex。
// 读操作只要求保证读取期间 SkipList 不会被销毁；除此之外，读操作不使用
// 内部锁或同步机制。
// 不变量：
// (1) 已分配节点直到 SkipList 销毁前都不会删除。代码从不删除 SkipList 节点，因此自然满足此条件。
// (2) Node 链入 SkipList 后，除 next/prev 指针外的内容不可变。
// 只有 Insert() 会修改链表；它会先初始化节点，再使用 release-store 将节点发布到一个或多个层级链表中。
```

即：**外部同步的单写者 + 不可变节点 + 发布式插入（release/acquire）+ 内存永不归还（Arena）+ 存活期保证（refs）**，五者共同构成无锁读的合法性。

---

## 5. WriteBatch

### 5.1 完整编码布局

`db/write_batch.cc:8-17` 的权威注释：

```
// WriteBatch::rep_ :=
//    sequence: fixed64
//    count: fixed32
//    data: record[count]
// record :=
//    kTypeValue varstring varstring         |
//    kTypeDeletion varstring
// varstring :=
//    len: varint32
//    data: uint8[len]
```

对应代码：

- header 12 字节 = 8 字节 sequence + 4 字节 count（`db/write_batch.cc:29-30`）：

```cpp
static const size_t kHeader = 12;
```

- `Clear()` 只保留 header（`db/write_batch.cc:38-41`）；`ApproximateSize()` 即 `rep_.size()`（`db/write_batch.cc:43`）。
- `Put`/`Delete`（`db/write_batch.cc:101-112`）：count+1（`SetCount` 直接改写 `rep_[8..12]`），追加 `kTypeValue`/`kTypeDeletion` 单字节 tag，再追加 length-prefixed key（Delete 无 value）。
- 存取器（`db/write_batch.cc:85-99`）：`Count` 读 `rep_[8..12)`，`SetCount` 写 `rep_[8..12)`，`Sequence` 读 `rep_[0..8)`，`SetSequence` 写 `rep_[0..8)`。
- `Append`（`db/write_batch.cc:147-151`）：count 相加、直接拼接双方 `rep_` 的 data 区（跳过 12 字节 header），O(1) 拼接而非逐条 Iterate（公共注释见 `include/LSMengine/write_batch.h:62-66`）。
- `SetContents`（`db/write_batch.cc:142-145`）：恢复时把 WAL 记录整段放入 rep_。

### 5.2 Iterate 的解析与校验

```cpp
// db/write_batch.cc:45-83（节选）
Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeader) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }
  input.remove_prefix(kHeader);
  ...
  while (!input.empty()) {
    found++;
    char tag = input[0];
    input.remove_prefix(1);
    switch (tag) {
      case kTypeValue: ... handler->Put(key, value); ... break;
      case kTypeDeletion: ... handler->Delete(key); ... break;
      default: return Status::Corruption("unknown WriteBatch tag");
    }
  }
  if (found != WriteBatchInternal::Count(this)) {
    return Status::Corruption("WriteBatch has wrong count");
  } else { return Status::OK(); }
}
```

- 解析靠 `GetLengthPrefixedSlice`（返回 false 即 Corruption，见 §7 错误处理）；末尾校验 `found == Count`，防止截断/伪造记录。

### 5.3 InsertInto 流程与"批量 sequence"

```cpp
// db/write_batch.cc:118-140
class MemTableInserter : public WriteBatch::Handler {
 public:
  SequenceNumber sequence_;
  MemTable* mem_;
  void Put(const Slice& key, const Slice& value) override {
    mem_->Add(sequence_, kTypeValue, key, value);
    sequence_++;
  }
  void Delete(const Slice& key) override {
    mem_->Add(sequence_, kTypeDeletion, key, Slice());
    sequence_++;
  }
};

Status WriteBatchInternal::InsertInto(const WriteBatch* b, MemTable* memtable) {
  MemTableInserter inserter;
  inserter.sequence_ = WriteBatchInternal::Sequence(b);
  inserter.mem_ = memtable;
  return b->Iterate(&inserter);
}
```

- **一个 batch 一个起始 sequence，批内逐条 +1**。seq 的分配发生在 DBImpl 写路径（`db/db_impl.cc:1220-1254`）：

```cpp
// db/db_impl.cc:1223-1225
WriteBatch* write_batch = BuildBatchGroup(&last_writer);
WriteBatchInternal::SetSequence(write_batch, last_sequence + 1);
last_sequence += WriteBatchInternal::Count(write_batch);
...
// db/db_impl.cc:1242
status = WriteBatchInternal::InsertInto(write_batch, mem_);
// db/db_impl.cc:1254
versions_->SetLastSequence(last_sequence);
```

- 先落 WAL（`log_->AddRecord(WriteBatchInternal::Contents(write_batch))`，`db/db_impl.cc:1233`），后进 MemTable（`db/db_impl.cc:1242`），顺序与 WAL 一致 → 崩溃恢复重放后 seq 与原运行期一致（恢复路径 `db/db_impl.cc:430-450`：`SetContents` + `InsertInto` + 用 `Sequence(batch) + Count(batch) - 1` 推进 max_sequence）。
- 相邻 writer 可被合并成一个 group batch（`BuildBatchGroup`，`db/db_impl.cc:1278-1314`；上限默认 1MB，小写最多附带 128KB，`db/db_impl.cc:1290-1293`；sync 与非 sync 不混批，`db/db_impl.cc:1300-1303`），合并后仍是一次 WAL record + 一次连续 seq 分配。

**为什么用"批量 sequence"**：
1. **原子性**：batch 内操作共享同一 WAL record 与同一 seq 区间，恢复时整体重放；seq 连续性使"部分写入可见"不可能发生（崩溃后要么整批在、要么整批不在）。
2. **group commit**：多个 writer 的 batch 合并 → 一次 fsync、一次 seq 区间分配，写入吞吐随合并度提升。
3. **seq 分配开销从每操作降为每批**：只需两次 64 位读写（`db_impl.cc:1224-1225, 1254`）。
4. 公共注释明确了语义："batch 加入 DBImpl 的 writer 队列后，DBImpl 才为这些操作分配连续的 sequence number"（`include/LSMengine/write_batch.h:29-30`）。

---

## 6. Arena

### 6.1 分配策略

```cpp
// util/arena.h:16-18
// Arena 是单一所有者使用的指针递增分配器：单次分配的内存不会单独释放，
// 返回的所有指针在 Arena 析构前始终有效。分配操作本身不提供同步；只有
// MemoryUsage() 使用原子操作。
```

快速路径是纯指针递增（`util/arena.h:57-68`）：

```cpp
inline char* Arena::Allocate(size_t bytes) {
  assert(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}
```

慢路径与块管理（`util/arena.cc`）：

```cpp
// util/arena.cc:9-15
static const int kBlockSize = 4096;
// 小请求共享 4 KiB 内存块。大于块大小四分之一的请求使用独立内存，
// 避免一个大对象让共享块中剩余的大部分空间无法利用。

// util/arena.cc:23-38
char* Arena::AllocateFallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // 对象大于默认块大小的四分之一，单独分配以免浪费过多尾部空间。
    char* result = AllocateNewBlock(bytes);
    return result;
  }
  // 放弃当前块中剩余的空间。
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;
  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}
```

- 小请求（≤1 KiB）共享 4 KiB 块；大请求独占一块。块列表存于 `std::vector<char*> blocks_`（`util/arena.h:47-48`）。

### 6.2 内存释放模型

- **无 per-allocation free**：析构时整块 `delete[]`（`util/arena.cc:17-21`）。
- `MemoryUsage()` 是 atomic 估算值（`util/arena.h:35-37`），统计口径含块指针开销（`util/arena.cc:60-66`）：

```cpp
char* Arena::AllocateNewBlock(size_t block_bytes) {
  char* result = new char[block_bytes];
  blocks_.push_back(result);
  memory_usage_.fetch_add(block_bytes + sizeof(char*),
                          std::memory_order_relaxed);
  return result;
}
```

- `AllocateAligned`（`util/arena.cc:40-58`）：对齐到 8 字节（64 位平台 `sizeof(void*)==8`；注释说按 malloc 的保证），不足时补 `slop`；对齐溢出当前块时走 `AllocateFallback`（新块首地址天然对齐）。

### 6.3 为什么要避免 free

1. **性能**：bump 分配是"比较+指针加法"O(1)，无空闲链表、无头部元数据、无碎片整理；块级回收使每次分配成本趋近于零。
2. **无锁读的正确性前提**：跳表节点一旦发布就不可变、且读者可能持有任意节点的指针（§4.5）。任何单节点 free 都可能造成 use-after-free。Arena 把"释放"推迟到整体析构，与 refs 生命周期管理配合，从机制上杜绝悬垂。
3. **碎片可控**：小对象共享块最多浪费 <4 KiB/块；大对象独占块无浪费。MemTable 生命周期内产生的碎片随 MemTable 一次性归还。

---

## 7. varint / fixed 编码

### 7.1 定长（fixed）编码

小端、与主机无关，头注释（`util/coding.h:5-8`）：

```
// 与主机字节序无关的编码：
// * 定长整数按最低有效字节在前的顺序编码；
```

- `EncodeFixed32/64` 与 `DecodeFixed32/64` 是 inline 的显式逐字节移位（`util/coding.h:51-99`），注释指出编译器会优化为单条 mov/str 指令。
- `PutFixed32/64` 把结果 append 进 `std::string`（`util/coding.cc:11-21`）。

### 7.2 PutVarint32/64（7 bit 分组）

```cpp
// util/coding.cc:23-49
char* EncodeVarint32(char* dst, uint32_t v) {
  // 按无符号字节处理字符。
  uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
  static const int B = 128;// 2 ^ 7   10000000
  if (v < (1 << 7)) {
    *(ptr++) = v;
  } else if (v < (1 << 14)) {
    *(ptr++) = v | B;
    *(ptr++) = v >> 7;
  } else if (v < (1 << 21)) {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = v >> 14;
  } else if (v < (1 << 28)) {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = (v >> 14) | B;
    *(ptr++) = v >> 21;
  } else {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = (v >> 14) | B;
    *(ptr++) = (v >> 21) | B;
    *(ptr++) = v >> 28;
  }
  return reinterpret_cast<char*>(ptr);
}
```

- 每字节低 7 位有效，最高位（0x80）为"还有后续字节"标志；**先写低位组**（小端序的 7-bit 分组）。32 位值 1~5 字节，用 5 个固定分支展开（避免循环）。
- 64 位用等价的循环（`util/coding.cc:57-66`）：`while (v >= B) { *ptr++ = v | B; v >>= 7; } *ptr++ = v;`，1~10 字节。
- `PutVarint32/64`：先写进 5/10 字节栈缓冲再按实际长度 append（`util/coding.cc:51-55, 68-72`）。
- `PutLengthPrefixedSlice` = varint32 长度 + 原始字节（`util/coding.cc:74-77`）。
- `VarintLength`（`util/coding.cc:79-86`）：循环 `while (v >= 128) { v >>= 7; len++; }`。MemTable::Add 用它精确计算 entry 总长（`db/memtable.cc:89-91`）。

### 7.3 GetVarint 与错误处理

- 快速路径：单字节 varint（`util/coding.h:104-114`）——首个字节无 MSB 直接返回，否则进 fallback：

```cpp
inline const char* GetVarint32Ptr(const char* p, const char* limit,
                                  uint32_t* value) {
  if (p < limit) {
    uint32_t result = *(reinterpret_cast<const uint8_t*>(p));
    if ((result & 128) == 0) {
      *value = result;
      return p + 1;
    }
  }
  return GetVarint32PtrFallback(p, limit, value);
}
```

- Fallback（`util/coding.cc:88-104`）：`shift` 从 0 步进 7 到 28；读到 MSB=0 的字节即结束；**若 5 个字节都用完仍带 MSB（或数据耗尽）返回 nullptr**：

```cpp
const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                   uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = *(reinterpret_cast<const uint8_t*>(p));
    p++;
    if (byte & 128) {
      // 后面还有字节。
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}
```

- 指针版契约："只读取 [p..limit-1] 范围内的字节"（`util/coding.h:35-38`），越界/截断返回 nullptr，**不抛异常**。
- Slice 版 `GetVarint32/64`（`util/coding.cc:106-116, 135-145`）：失败返回 false 且**不动 input**；成功才把 `*input` 收缩到剩余部分。
- `GetVarint64Ptr` 同构，`shift <= 63`，最多 10 字节（`util/coding.cc:118-133`）。
- `GetLengthPrefixedSlice`（`util/coding.cc:147-156`）：解出 len 后校验 `input->size() >= len`，不足返回 false——这是 WriteBatch::Iterate 判 Corruption 的最终防线（§5.2）。
- 边界细节（LevelDB 原样保留）：varint32 第 5 字节若 >0x0f（`shift=28` 时 `byte << 28` 按 uint32 回绕），解码器**不拒绝**，按无符号回绕取值；同样 varint64 第 10 字节同理。**待确认**：Stratum 若要求严格校验需自行加 `byte < (1 << (32-shift))` 类检查。

---

## 8. crc32c

### 8.1 用哪个实现（SSE4.2 / 软件查表）

三层结构：

1. **自带可移植软件实现**（`util/crc32c.cc`）：
   - 一张 256 项字节表 `kByteExtensionTable`（`util/crc32c.cc:23-66`）+ 4 张 stride 表 `kStrideExtensionTable0..3`（`util/crc32c.cc:68-246`）。
   - `Extend` 主体（`util/crc32c.cc:278-378`）：初始/终止值均与 `0xffffffff` 异或（`kCRC32Xor`，`util/crc32c.cc:248-249`）；先把指针按 4 字节对齐（`RoundUp<4>`，`util/crc32c.cc:259-264`）；主体用 4 条互不依赖的 stride 交错处理（每轮 16 字节，`STEP4/STEP16` 宏，`util/crc32c.cc:295-312`，可利用 CPU 流水线），尾部先 4 字节（stride 轮换合并，`util/crc32c.cc:350-359`）再逐字节（`STEP1`），最后 `l ^ kCRC32Xor`（`util/crc32c.cc:377`）。
2. **port 层可替换为硬件加速**（`port/port_stdcxx.h:208-218`）：

```cpp
inline uint32_t AcceleratedCRC32C(uint32_t crc, const char* buf, size_t size) {
#if HAVE_CRC32C
  return ::crc32c::Extend(crc, reinterpret_cast<const uint8_t*>(buf), size);
#else
  ...
  return 0;
#endif  // HAVE_CRC32C
}
```

   即链接外部 **google/crc32c 库**（该库在 x86 上运行时探测并使用 SSE4.2 的 `crc32` 指令 `_mm_crc32_u64`，无硬件时自动回退软件），磁盘上的 checksum 值不受替换影响（`util/crc32c.cc:20-21` 注释）。
3. **运行时选择**（`util/crc32c.cc:268-282`）：

```cpp
static bool CanAccelerateCRC32C() {
  // 无法加速时 port::AcceleretedCRC32C 返回 0。
  static const char kTestCRCBuffer[] = "TestCRCBuffer";
  static const char kBufSize = sizeof(kTestCRCBuffer) - 1;
  static const uint32_t kTestCRCValue = 0xdcbc59fa;
  return port::AcceleratedCRC32C(0, kTestCRCBuffer, kBufSize) == kTestCRCValue;
}
uint32_t Extend(uint32_t crc, const char* data, size_t n) {
  static bool accelerate = CanAccelerateCRC32C();
  if (accelerate) {
    return port::AcceleratedCRC32C(crc, data, n);
  }
  ...软件实现...
}
```

   用已知测试向量探测一次（static 缓存结果），探测失败即永久走软件路径。

- **本仓库当前构建的实际状态**：`CMakeLists.txt:41` 用 `check_library_exists(crc32c crc32c_value "" HAVE_CRC32C)` 探测外部库，找到才链接（`CMakeLists.txt:271-273`）；`build/CMakeCache.txt:650` 中 `HAVE_CRC32C:INTERNAL=` 为空 → **本机构建未启用外部 crc32c，运行时走自带软件查表实现**；`build/include/port/port_config.h:25-28` 模板默认 `HAVE_CRC32C 0`。结论：**实现架构上是"SSE4.2（经外部 google/crc32c）优先、软件查表兜底"，本构建实例实际运行软件路径**。

### 8.2 Mask / Unmask 的用途

```cpp
// util/crc32c.h:23-38
static const uint32_t kMaskDelta = 0xa282ead8ul;
// 返回 crc 经 mask 后的表示。
inline uint32_t Mask(uint32_t crc) {
  // 循环右移 15 bit，再加上一个常量。
  return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}
// 返回 masked_crc 所对应的原始 crc。
inline uint32_t Unmask(uint32_t masked_crc) {
  uint32_t rot = masked_crc - kMaskDelta;
  return ((rot >> 17) | (rot << 15));
}
```

头注释给出的理由（`util/crc32c.h:14-17, 25-28`）："持久化 checksum 在落盘前会被 mask，避免被校验记录中嵌入的 checksum 形成有问题的自相似 CRC 模式"。

实际用法（本仓库实证）：

- **写入侧 Mask**：WAL 写头部前 mask（`db/log_writer.cc:96-97`：`crc = crc32c::Extend(type_crc_[t], ptr, length); crc = crc32c::Mask(crc);`）；SST block trailer 同样先 `Extend` 后 `Mask` 再落盘（`table/table_builder.cc:198-200`）。
- **读取侧 Unmask 比较**：WAL 读（`db/log_reader.cc:238-239`：`expected_crc = crc32c::Unmask(DecodeFixed32(header)); actual_crc = crc32c::Value(header + 6, 1 + length);`）；SST block 读（`table/format.cc:94-95` 同款模式）。
- 额外性质（LevelDB 公认，本仓库代码未直接使用整段常数比较）：`Mask` 与 `Unmask` 互逆（`Unmask(Mask(x)) = x`，可由 rot15+delta / 减 delta+rot17 验证），且 `crc32c(X ∥ Mask(crc32c(X))) == kMaskDelta`（常数）——即"数据+masked CRC"整段再算一次 CRC 恒等于 `kMaskDelta`，可用于整段校验。**待确认**：本实现读路径选择的是 Unmask 后与 `Value` 比较，而不是整段常数比较；两者都正确，常数比较是更强的优化点（省一次 Unmask）。
- `Extend` 的增量语义：`Extend(init_crc, data, n)` 返回 `concat(A, data)` 的 CRC，其中 `init_crc` 是 A 的 CRC（`util/crc32c.h:16-18`）；`Value(data, n) = Extend(0, data, n)`（`util/crc32c.h:21`）。WAL 利用这一点把 record type 单字节的 CRC 预计算进 `type_crc_[4]`，写 record 时 `Extend(type_crc_[t], ptr, length)` 免去重复计算（`db/log_writer.cc:22, 96`）。

---

## 9. 与 Stratum 时间轴需求的映射建议

### 9.1 TimeKey 直接作为 user_key 会得到什么

假设 TimeKey = (topic_id, timestamp_ns, seq_in_ts)，20 字节定长（**具体字段宽度与端序待确认**；最可能的布局是 8+8+4 字节；4+8+8 或 16+4 亦常见，需与 Stratum 规格核对）。InternalKey = TimeKey ∥ 8 字节 tag → **总长 28 字节**；MemTable entry = `varint32(28)=0x1c`（1 字节）∥ 28 字节 key ∥ varint32(val_len) ∥ value。

直接作为 user_key 的后果（按序）：

1. **排序即时间轴**：若三个字段都按**大端**编码，"字节字典序 == 数值序"（与 §1.3 同一原理）→ (topic, ts, seq) 天然形成"topic 升序 → 时间戳升序 → 同时间戳内 seq 升序"的全序。同 topic 的时间范围查询 = 一次跳表 Seek + 顺序遍历，**无需任何二级索引**。
2. **历史版本天然共存**：不同 (ts, seq) 是**不同 user key**，写入互不覆盖。同一个 TimeKey 的重复写入才产生多版本（由 internal sequence 区分，seq 大者在前，§2）。
3. **快照/版本链的角色被削弱**：MVCC 的"旧版本"不再依赖 internal seq 保留——每个时间点本身就是独立的 key。internal seq 退化为"同一 TimeKey 的覆盖顺序"与"GC 可见性边界"。
4. **代价**：(a) 数据只增不减，LevelDB 无原生 TTL → 旧时间点数据永久占用空间与 Compaction 带宽，必须有清理机制；(b) "同一时间戳最新一条"需要 `Seek((topic, ts, 0xFFFFFFFF))` + `Prev`（或按 §9.3 编码 seq_in_ts 反转），LevelDB 迭代器无 SeekForPrev（**待确认**：本仓库 `Iterator` 接口未提供该操作）；(c) 20 字节 key 相对字符串 key 更长，但定长比较（memcmp）比变长比较更快，抵消一部分。

### 9.2 哪些可以原样复用、哪些需要调整

**原样复用（零改动或仅换参数）**：

| 组件 | 依据 | 复用方式 |
|---|---|---|
| InternalKey 编码/解析 | `db/dbformat.h:75-92, 164-174` | 20B 定长 user_key 完全兼容，28B internal key |
| InternalKeyComparator | `db/dbformat.cc:50-66` | 内部实现不动，只换 `user_comparator_` |
| MemTable / SkipList / Arena | `db/memtable.*`、`db/skiplist.h`、`util/arena.*` | 整块搬走，键长增加不影响任何逻辑 |
| LookupKey + 单次 Seek 的 Get 语义 | `db/dbformat.cc:119-136`、`db/memtable.cc:104-137` | 原样；对指定 TimeKey 的读天然是 O(log n) |
| WriteBatch 编码与恢复 | `db/write_batch.cc` 全篇、`db/db_impl.cc:430-450` | batch 内 key 换 20B 即可，WAL 格式不变 |
| coding / crc32c / Random | `util/*` | 全部原样 |
| Snapshot 机制（sequence 阈值） | `db/snapshot.h`、`db/db_impl.cc:1122-1152` | 用于并发一致性，见 §9.4 |

**需要调整/新增**：

1. **user Comparator 必须自定义**（或确认直接沿用 BytewiseComparator）。Comparator 契约见 `include/LSMengine/comparator.h:21-49`：`Name()` 是持久化元数据，"以 leveldb. 开头的名称由本库保留，客户端不得使用"（`comparator.h:36`）→ 新引擎的 comparator 名称必须自定义（如 `"stratum.TimeKeyComparator"`），**换名称即换 schema**，避免与既有 LevelDB 库串库。定长大端 TimeKey 下 `Compare` 就是 20 字节 memcmp（见 §9.3 代码）。
2. **TTL / 时间范围清理**：LevelDB 没有原生 TTL 与 range delete。历史 TimeKey 永不自动删除 → 需要其一：
   - 按时间窗口做**物理分片**（topic 内按 ts 分 shard DB / column family，到期整片删文件）——最贴合 LSM 且无需改引擎核心；
   - 或给 Compaction 加过滤器钩子（**待确认**：本 LSMengine 未提供 CompactionFilter；需自行扩展 `db/db_impl.cc` 的 CompactMemTable / BackgroundCompaction 路径，参照 `db/db_impl.cc:923-989` 的版本裁剪逻辑插入时间判断）；
   - 或定时逐 key 写入 `kTypeDeletion`（成本高，不推荐）。
3. **"同时间戳多条"语义**：`seq_in_ts` 排序方向决定 `Seek((topic,ts,0))` 拿到的第一条是"最早"还是"最新"。若语义要求"读某时间点最新一条"，建议把 `seq_in_ts` 存成 `~seq_in_ts`（按位取反）使其降序排列，与 internal seq 的"最新在前"哲学一致，一次 Seek 即命中（§9.3 给出）。**待确认**：Stratum 是否需要"同一 ns 内多条"，还是 seq_in_ts 仅用于去重。
4. **snapshot 与时间点解耦**（§9.4）。
5. **key 中时间戳精度与单调性**：若多个写线程各自取系统时钟，同 topic 的 ts 可能乱序到达 MemTable —— 这**不影响正确性**（键空间有序即可），但影响"时间点读"的语义边界；如需严格单调，应在写路径统一打点（见 §9.4 方案 A）。

### 9.3 Comparator 的具体写法

```cpp
// 建议：20 字节定长、全字段大端；Compare 用 memcmp，实现为三路比较。
class TimeKeyComparator : public Comparator {
 public:
  int Compare(const Slice& a, const Slice& b) const override {
    // 定长 20B 大端编码：topic_id(8B) ∥ timestamp_ns(8B) ∥ seq_in_ts(4B)
    // 字节字典序 == 数值序，因此直接 memcmp。
    assert(a.size() == kTimeKeySize && b.size() == kTimeKeySize);
    const int r = std::memcmp(a.data(), b.data(), kTimeKeySize);
    return (r < 0) ? -1 : (r > 0) ? 1 : 0;
  }
  const char* Name() const override { return "stratum.TimeKeyComparator"; }

  // 以下两项允许 no-op（comparator.h:39-48 明确"什么都不做也是正确实现"）；
  // 要省 index block 空间的话做字节版即可：
  //   FindShortestSeparator: 找 start 与 limit 的最长公共前缀 p，若 p<20 且
  //       start[p]+1 < limit[p]，则 start = start[0..p] ++ (start[p]+1)（大端无进位问题）
  //   FindShortSuccessor: 类似处理末字节 +1。
  void FindShortestSeparator(std::string* start, const Slice& limit) const override {}
  void FindShortSuccessor(std::string* key) const override {}
};
```

配套注意点：

- `InternalKeyComparator` 的 `FindShortestSeparator/Successor`（`db/dbformat.cc:68-100`）会自动在缩短的 user key 后面拼 `(kMaxSequenceNumber, kValueTypeForSeek)` 的 tag——缩短逻辑必须保持"物理变短且逻辑变大"的前置条件，字节版实现天然满足。
- 若字段里有小端存量数据，**不要**复用字节序 comparator，需逐字段解码比较（慢）；强烈建议新格式统一大端。
- 若 `seq_in_ts` 需要"同 ts 最新在前"，写入时存 `~seq_in_ts`（uint32 取反），其他不变。
- `InternalFilterPolicy`（`db/dbformat.cc:104-117`）对 Bloom filter 只喂 user key（即 20B TimeKey），点查 (topic,ts,seq) 的 filter 假阳性率与普通 key 相同，可原样复用。

### 9.4 snapshot 如何映射为时间点

前提认知：`SnapshotImpl` 只保存一个 `SequenceNumber`（`db/snapshot.h:22-34`），Get 的可见性 = "seq ≤ snapshot_seq"（经 LookupKey 实现，`db/db_impl.cc:1122-1144`）。时间点 T 的读需要把 T 换算成 seq。两个方案：

**方案 A：ts → seq 单调索引（推荐给"严格时序"场景）**
- 条件：时间戳在**写路径统一打点**，且与 seq 分配顺序同向。DBImpl 的 writer 队列天然保证 seq 单调分配（`db/db_impl.cc:1224-1225`）；只需在 leader 分配 seq 区间的同一临界区内取一次时钟作为该批时间戳（或要求批内所有 key 携带的 ts 不早于此前已分配批次），即可保证 `ts 单调不减 ⟺ seq 单调不减`。
- 数据：维护内存有序映射 `ts → 该 ts 之前分配的最大 seq`（可直接复用本仓库的 `SkipList<...>`，或 btree；写入点与 `versions_->SetLastSequence` 同步更新）。
- 读：`Snapshot_Time(T)` = 查表取 `floor(T)` 的 seq，其余路径**完全不变**（还是 `LookupKey lkey(key, seq)`，`db/db_impl.cc:1144`）。
- 代价：仅当时间戳乱序（客户端带 ts、多机合并等）时此方案失效。

**方案 B：时间点由 key 空间表达（推荐给"时间轴本身"场景）**
- 因为 timestamp 已经在 user_key 里（§9.1），"读 T 时刻最新"本来就是键空间查询：`Seek((topic_id, T, seq_in_ts 上限))` 后取第一个 `≤ T` 者；对"截至某时间点的连续回放"，直接 `Seek((topic_id, T_begin, 0))` 顺序迭代即可。
- LevelDB 的 Snapshot（seq 阈值）此时**只承担并发一致性**：保证一次迭代过程中不被中途写入的半成品污染（`ReadOptions.snapshot` 用最新 seq，`db/db_impl.cc:1126-1127`），而不是表达"时间点"。
- 优点：无需 ts→seq 索引、无单调性假设、跨 topic 无需全局时钟对齐；缺点：不能直接回答"在 seq=K 那一刻，哪些 key 已存在"这类写序问题。

**混合建议（最贴合 Stratum）**：以方案 B 为主表达时间轴；保留 LevelDB Snapshot 机制用于事务/一致性读；仅当出现"按写入顺序的事件溯源"需求时，再加方案 A 的 ts→seq 索引。两种方案都不需要修改 MemTable/SkipList/WriteBatch 的任何代码——改动集中在 Comparator（§9.3）、TTL 清理（§9.2-2）与 snapshot 换算这一薄层。

### 9.5 遗留风险清单

- TimeKey 三个字段的宽度/端序未确认（直接影响 §9.1/§9.3 的字节布局结论）。
- 本 LSMengine 无 CompactionFilter/TTL 钩子，时间范围清理必须自行扩展或走物理分片（**待确认**）。
- 迭代器无 `SeekForPrev`；"某时间点最新一条"若采用方案 B 且 seq_in_ts 未反转，需要 Seek+Prev（Prev 是 O(log n)，`db/skiplist.h:201-209`），对热点 topic 建议反转编码。
- varint32 第 5 字节非严格校验（§7.3），若 Stratum 对 WAL 恢复的输入不可信，需自加检查。
- 本机构建实际未链接硬件 CRC32C（§8.1）；重实现时建议像上游一样保留"运行时探测 + 软件兜底"结构。

---

*文档由只读深读生成；所有 `文件:行` 引用指向 `/UserData/LeaningLevelDB/LSMengine` 的物理行号。*
