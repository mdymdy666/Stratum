/* Copyright 2026 Stratum Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * src/include/stratum/probe/probe.h
 *
 * 埋点库的**业务侧**接口。纯 C，零依赖。
 *
 * 需求依据：docs/13-probe-requirements.md
 *   PR-01 零依赖（纯 C，不链接 DDS、不引入 C++ 模板）
 *   PR-02 STRATUM_PROBE / _KV / _SCOPE 三个宏
 *   PR-03 写入路径无 malloc/mutex/syscall；满则丢最旧
 *   PR-04 -DSTRATUM_PROBE_DISABLE 时零开销（空语句）
 *
 * ---------------------------------------------------------------------------
 * 设计要点
 * ---------------------------------------------------------------------------
 *
 * 1. **固定大小记录**。ring 里每条记录同大小，所以任意位置都能用
 *    `(seq % capacity) * sizeof(record)` 直接索引 —— 不需要分配器。
 *    这是"无 malloc"的前提。
 *
 * 2. **单写者、多读者**。业务只写，读取器（stratum probe）只读。
 *    写入用一次 `__atomic_store_n(..., release)` 提交序号，
 *    读取器用 `acquire` —— 不需要 mutex。
 *
 * 3. **满则覆盖最旧、绝不阻塞**。刻意的取舍：
 *    **"绝不拖慢业务" 优先于 "不丢数据"**（PR-03）。
 *    丢弃累加到 `dropped_full`，可由 `stratum probe status` 看到。
 *
 * 4. **热路径零系统调用**。共享内存的创建/映射只在**首次**（懒初始化）发生；
 *    之后每次写入只是指针运算 + memcpy。
 *
 * 5. **编译期可关闭**。定义 `STRATUM_PROBE_DISABLE` 后三个宏全展开成
 *    `((void)0)`，连 `enabled()` 都不调用 —— 反汇编里不留痕迹（PR-04）。
 *
 * ---------------------------------------------------------------------------
 * 关于负载格式的一个务实决定
 * ---------------------------------------------------------------------------
 *
 * `STRATUM_PROBE_KV` **接受一个已经组织好的字符串**（如 `"id=1001,ok=1"`），
 * 而不是变参 `(key, value, key, value, ...)`。原因：
 *
 *   · C 的 varargs **无法判断参数类型** —— `_KV("t","k", 99.5)` 里 `99.5`
 *     在 varargs 中是 double，但被调用方拿不到这个信息，只能靠调用方声明。
 *   · 若为此引入 printf 风格格式串，热路径上就要跑格式化（比 memcpy 贵得多），
 *     违背 PR-03 的"纳秒级"目标。
 *
 * 所以：**格式化由业务在写入前完成**（通常只是一次 snprintf，或干脆用字面量），
 * 探针只负责搬字节。这也符合 docs/13 的非目标："探针不理解业务语义"。
 *
 * ---------------------------------------------------------------------------
 * 用法
 * ---------------------------------------------------------------------------
 *
 *   #include <stratum/probe/probe.h>
 *
 *   void submit_order(const Order& o) {
 *       STRATUM_PROBE_SCOPE("order/submit");          // C++：RAII 自动记耗时
 *       STRATUM_PROBE("order/detail", "id=1001");     // 固定字符串负载
 *       STRATUM_PROBE_KV("order/amount", "amount=99.5,cur=CNY");
 *   }
 *
 * 想关掉某个 topic：改配置文件里对应段落的 `enabled`（见 probe_config.h）。
 */

#ifndef STRATUM_PROBE_PROBE_H_
#define STRATUM_PROBE_PROBE_H_

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* 编译期开关（PR-04）                                                        */
/* ------------------------------------------------------------------------- */

#ifdef STRATUM_PROBE_DISABLE

/* 全部变成空语句。**连 enabled() 都不调用** —— 这样 -O2 下整段被彻底消除。 */
#define STRATUM_PROBE(topic, payload) ((void)0)
#define STRATUM_PROBE_KV(topic, kv_text) ((void)0)
#define STRATUM_PROBE_STR(topic, cstr) ((void)0)
#define STRATUM_PROBE_BEGIN(topic) ((void)0)
#define STRATUM_PROBE_END(handle) ((void)0)
#define STRATUM_PROBE_SCOPE(topic) ((void)0)

#else /* !STRATUM_PROBE_DISABLE */

/* ------------------------------------------------------------------------- */
/* 容量常量                                                                   */
/* ------------------------------------------------------------------------- */

#define STRATUM_PROBE_TOPIC_MAX 48    /* 含结尾 0；超长截断并计入 dropped_bad */
#define STRATUM_PROBE_PAYLOAD_MAX 192 /* 超过截断，置 flags bit0 */

/* ring 的默认容量（条）。可由 stratum_probe_open 的环境变量覆盖。 */
#define STRATUM_PROBE_DEFAULT_CAPACITY 4096

/* ------------------------------------------------------------------------- */
/* 记录（固定大小）                                                           */
/* ------------------------------------------------------------------------- */

typedef struct stratum_probe_record {
  uint64_t ts_realtime_ns;  /* CLOCK_REALTIME，用于与业务 DDS 数据对齐 */
  uint64_t ts_mono_ns;      /* 单调时钟，用于算耗时（不受系统时间调整影响） */
  uint32_t seq;             /* 全局写入序号（低 32 位） */
  uint32_t thread_id;       /* 写入线程（低 16 位有效） */
  uint32_t topic_len;       /* 不含结尾 0 */
  uint32_t payload_len;     /* 实际写入字节数 */
  uint32_t flags;           /* bit0 = 负载被截断 */
  uint32_t reserved;
  char topic[STRATUM_PROBE_TOPIC_MAX];
  char payload[STRATUM_PROBE_PAYLOAD_MAX];
} stratum_probe_record;

/* ------------------------------------------------------------------------- */
/* ring 头部（写侧与读侧共享同一布局）                                        */
/* ------------------------------------------------------------------------- */

#define STRATUM_RING_MAGIC 0x53545250u /* 'STRP' */
#define STRATUM_RING_VERSION 1u

/* 【为什么放在头文件里】读取器（stratum probe）在**另一个进程**里按同一
 * 布局解析这块共享内存。若两边各写一份结构，"字段顺序一改就静默错位" ——
 * 不报错，只读到垃圾。共享类型定义由编译器保证两侧一致。
 *
 * 布局说明（读取器的消费规则）：
 *   · write_seq      ：已分配的序号（写者 fetch_add；**可能包含未写完的**）
 *   · committed_seq  ：已写完的序号（到这里的记录一定完整）
 *   · 读取器必须用 committed_seq 做上界，且只消费 < 本进程上次读到的位置
 *     的新记录 —— 否则环形覆盖会重复读。 */
typedef struct stratum_ring_header {
  uint32_t magic;        /* STRATUM_RING_MAGIC */
  uint32_t version;      /* STRATUM_RING_VERSION */
  uint32_t capacity;     /* 记录条数（2 的幂） */
  uint32_t record_size;  /* = sizeof(stratum_probe_record) */

  uint64_t write_seq;     /* 原子：已分配序号 */
  uint64_t committed_seq; /* 原子：已提交序号 */
  uint32_t enabled;       /* 原子：全局开关（读取器可写，写侧只读） */
  uint32_t reserved[7];

  /* records[] 紧跟在这个头部之后 */
} stratum_ring_header;

/* 给定 ring 起始地址，返回记录数组的起始地址 */
static inline stratum_probe_record* stratum_ring_records(stratum_ring_header* h) {
  return (stratum_probe_record*)((char*)h + sizeof(stratum_ring_header));
}
static inline const stratum_probe_record* stratum_ring_records_const(
    const stratum_ring_header* h) {
  return (const stratum_probe_record*)((const char*)h + sizeof(stratum_ring_header));
}

/* 计算一个 ring 需要多少字节（供读取器校验文件大小） */
static inline size_t stratum_ring_bytes(uint32_t capacity) {
  return sizeof(stratum_ring_header) +
         (size_t)capacity * sizeof(stratum_probe_record);
}

/* ------------------------------------------------------------------------- */
/* 生命周期                                                                   */
/* ------------------------------------------------------------------------- */

/* 打开（或创建）本进程的共享内存 ring。
 *
 * `shm_path` 为 NULL 时用默认路径 `/dev/shm/stratum_probe.<pid>`。
 * 返回 0 成功，非 0 失败。**失败不致命**：之后写入静默丢弃，
 * 业务代码不需要检查返回值（埋点不能影响业务）。
 *
 * 这是唯一做系统调用的函数；业务首次调用探针时会自动触发它。 */
int stratum_probe_open(const char* shm_path);

/* 解除映射。通常不需要显式调用（进程退出内核会清理）；提供它是为了测试。 */
void stratum_probe_close(void);

/* 当前是否可用（已打开 && 全局开关为 true）。宏内部已调用。 */
int stratum_probe_enabled(void);

/* 运行期设置全局开关。生产环境里由配置文件驱动，不由业务调用。 */
void stratum_probe_set_enabled(int enabled);

/* ------------------------------------------------------------------------- */
/* 写入（热路径：无 malloc / 无 mutex / 无 syscall）                          */
/* ------------------------------------------------------------------------- */

/* 写一条记录。payload 可以是任意字节（含 0）。
 * 内部做采样与 topic 开关判断。**永不阻塞、永不失败**（失败只累加计数）。 */
void stratum_probe_write(const char* topic, const void* payload,
                         size_t payload_len);

typedef struct stratum_probe_stats {
  uint64_t written;         /* 成功写入 */
  uint64_t dropped_full;    /* 因环满丢弃（覆盖最旧） */
  uint64_t dropped_off;     /* 因 topic 或全局关闭而跳过 */
  uint64_t dropped_sampled; /* 因采样跳过 */
  uint64_t dropped_bad;     /* 因参数非法（topic 为空 / 超长） */
  uint32_t capacity;        /* ring 容量（条） */
} stratum_probe_stats;

void stratum_probe_get_stats(stratum_probe_stats* out);

/* 读取器与测试用的计数视图。
 *
 * allocated：已分配的写入序号（写者递增，**可能包含尚未写完的记录**）
 * committed：已提交的写入序号（到这里的记录一定完整）
 *
 * 【为什么两个都要暴露】读取器必须用 committed 做上界，
 * 否则会读到"正在写"的槽位（环形覆盖时该槽可能正被重写）。 */
uint64_t stratum_probe_allocated(void);
uint64_t stratum_probe_committed(void);

/* ring 头部只读视图（供读取器实现；普通业务代码不需要）。 */
const void* stratum_probe_ring_view(void);

/* 作用域计时的底层：写一条 "topic=... dur_us=..." 记录 */
void stratum_probe_write_scope(const char* topic, uint64_t dur_ns);

/* 单调时钟（纳秒） */
uint64_t stratum_probe_now_mono_ns(void);

/* ------------------------------------------------------------------------- */
/* 三个宏（PR-02）                                                            */
/* ------------------------------------------------------------------------- */

/* 1) 固定字符串负载（编译期已知长度，无 strlen）
 *
 * 【注意】这里刻意用 `sizeof(payload) - 1`：要求 payload 是**字符串字面量**
 * 或字符数组。若传指针会导致 `sizeof` 得到指针大小 —— 所以文档必须说清。
 * 传指针请用 stratum_probe_write()。 */
#define STRATUM_PROBE(topic, payload)                                     \
  do {                                                                    \
    if (stratum_probe_enabled()) {                                        \
      stratum_probe_write((topic), (payload),                             \
                          (sizeof(payload) - 1 > STRATUM_PROBE_PAYLOAD_MAX) \
                              ? STRATUM_PROBE_PAYLOAD_MAX                  \
                              : sizeof(payload) - 1);                      \
    }                                                                     \
  } while (0)

/* 2) KV 负载：接受**已组织好的字符串**（见文件头"务实决定"） */
#define STRATUM_PROBE_KV(topic, kv_text) STRATUM_PROBE((topic), (kv_text))

/* 1b) **运行时字符串指针**专用版本 —— 用 strlen，不依赖 sizeof。
 *
 * 【什么时候必须用它】负载不是字符串字面量，而是：
 *     · 变量        const char* msg = ...;
 *     · 三元表达式  cond ? "a" : "b"        ← 类型是 const char*，不是数组
 *     · 函数返回值  GetMessage()
 *
 * 【为什么需要单独一个宏】
 * `STRATUM_PROBE` 用 `sizeof(payload) - 1`，只对**数组**成立。
 * 传指针时 sizeof 得到 8（指针大小），于是只发出前 7 个字节 ——
 * **不报错、不崩溃，只是数据被悄悄截断**。
 *
 * 这个坑我自己踩过一次：测试代码写了
 *     STRATUM_PROBE_KV("t", (i < 16) ? "ok" : "err")
 * 结果只发出 7 字节，而头文件里其实已经写了警告。
 * **"在注释里提醒"防不住错误，所以要给一个显式的正确入口。**
 *
 * 代价：多一次 strlen 遍历（对 100 字节以内的负载可忽略）。
 * 能用字面量时优先用 STRATUM_PROBE —— 那是零开销的。 */
#define STRATUM_PROBE_STR(topic, cstr)                                   \
  do {                                                                   \
    if (stratum_probe_enabled()) {                                        \
      const char* _sp = (cstr);                                           \
      if (_sp != 0) {                                                     \
        size_t _n = strlen(_sp);                                          \
        if (_n > STRATUM_PROBE_PAYLOAD_MAX) _n = STRATUM_PROBE_PAYLOAD_MAX; \
        stratum_probe_write((topic), _sp, _n);                            \
      }                                                                   \
    }                                                                     \
  } while (0)

/* 3) 作用域计时（C 用 BEGIN/END） */
#define STRATUM_PROBE_BEGIN(topic)                                      \
  stratum_probe_scope _stratum_scope_ = {(topic),                        \
                                         stratum_probe_now_mono_ns(), 1}

typedef struct stratum_probe_scope {
  const char* topic;
  uint64_t start_mono_ns;
  int active;
} stratum_probe_scope;

#define STRATUM_PROBE_END(handle)                                       \
  do {                                                                   \
    if ((handle).active) {                                               \
      stratum_probe_write_scope((handle).topic,                          \
                                stratum_probe_now_mono_ns() -            \
                                    (handle).start_mono_ns);             \
      (handle).active = 0;                                               \
    }                                                                    \
  } while (0)

#endif /* STRATUM_PROBE_DISABLE */

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ------------------------------------------------------------------------- */
/* C++ 专用 RAII（STRATUM_PROBE_SCOPE）                                       */
/* ------------------------------------------------------------------------- */
#ifdef __cplusplus
#ifndef STRATUM_PROBE_DISABLE
namespace stratum {
namespace probe {
namespace detail {

/* RAII：构造记单调时钟，析构写一条耗时记录。
 *
 * 【为什么异常路径也要发】docs/13 S01 的边界分支：
 * 若只在正常返回时发，"最慢的那次调用"往往恰好是抛异常的那次 —— 恰恰查不到。
 * 析构在栈展开时也会执行，异常路径自然被覆盖。 */
class Scope {
 public:
  explicit Scope(const char* topic)
      : topic_(topic), start_(::stratum_probe_now_mono_ns()) {}
  ~Scope() {
    if (topic_ != nullptr) {
      ::stratum_probe_write_scope(topic_, ::stratum_probe_now_mono_ns() - start_);
    }
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  const char* topic_;
  uint64_t start_;
};

}  // namespace detail
}  // namespace probe
}  // namespace stratum

/* 用 __LINE__ 保证同一作用域内多次使用不重名 */
#define STRATUM_PROBE_SCOPE_2(topic, line) \
  ::stratum::probe::detail::Scope _stratum_scope_##line(topic)
#define STRATUM_PROBE_SCOPE_1(topic, line) STRATUM_PROBE_SCOPE_2(topic, line)
#define STRATUM_PROBE_SCOPE(topic) STRATUM_PROBE_SCOPE_1(topic, __LINE__)

#endif /* !STRATUM_PROBE_DISABLE */
#endif /* __cplusplus */

#endif /* STRATUM_PROBE_PROBE_H_ */
