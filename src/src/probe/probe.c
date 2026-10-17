/* Copyright 2026 Stratum Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * src/src/probe/probe.c
 *
 * 埋点库的业务侧实现：共享内存 ring，无锁、无 malloc、热路径无系统调用。
 *
 * 【并发模型：单写者 + 多读者，无锁】
 *
 *   业务侧（单写者）                     读取器（stratum probe，多读者）
 *   ─────────────────                    ──────────────────────────────
 *   1. seq = fetch_add(write_seq, 1)  ← 分配槽位（可能覆盖最旧）
 *   2. 把数据写进 records[seq % cap]
 *   3. store(committed_seq, seq+1)    ← 提交：此刻数据已完整
 *                                        4. 读 committed_seq
 *                                        5. 只消费本进程已提交的记录
 *
 * **为什么需要两个计数器**：
 * 单写者用环形覆盖时，"正在写的槽"与"读者想读的槽"可能是同一个。
 * 若只用一个 `write_seq`，读者会把半写状态的数据当成有效的。
 * 用 `committed_seq` 明确区分"已分配"与"已写完"，
 * 读者只需保证自己读的位置 `< committed_seq` 即可 —— 无需加锁。
 *
 * 【为什么不会阻塞业务】
 * 写入路径上没有任何等待：环满了就覆盖最旧的（`dropped_full++`），
 * 这是 PR-03 的刻意取舍 —— **绝不拖慢业务优先于不丢数据**。
 */

#include "stratum/probe/probe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */
/* ring 头部（放在共享内存起始处）                                            */
/* ------------------------------------------------------------------------- */

#define STRATUM_RING_MAGIC 0x53545250u /* 'STRP' */
#define STRATUM_RING_VERSION 1u

typedef struct stratum_ring_header {
  uint32_t magic;
  uint32_t version;
  uint32_t capacity;     /* 记录条数 */
  uint32_t record_size;  /* = sizeof(stratum_probe_record) */

  /* 已分配序号（写者递增）。注意：它可能大于已写完的序号。 */
  _Atomic uint64_t write_seq;
  /* 已提交序号（即"到这里的数据都是完整的"）。读者用它做上界。 */
  _Atomic uint64_t committed_seq;

  /* 全局开关。读取器/测试可写；写者只读。
   * 【为什么要放在共享内存里】这样"关闭探针"不需要业务重启，
   * 由 Stratum 侧或配置热重载直接改这一位即可（PR-06 的开关）。 */
  _Atomic uint32_t enabled;

  uint32_t reserved[6];
} stratum_ring_header;

/* ------------------------------------------------------------------------- */
/* 进程内状态（静态；热路径只读这些指针）                                     */
/* ------------------------------------------------------------------------- */

static stratum_ring_header* g_hdr = NULL;
static stratum_probe_record* g_records = NULL;
static size_t g_map_size = 0;
static int g_fd = -1;
static char g_shm_path[256];

/* 懒初始化只用一次；用原子标志避免重复 mmap */
static _Atomic int g_init_attempted = 0;
/* 打开失败后不再反复尝试（避免热路径上反复 syscall） */
static _Atomic int g_open_failed = 0;

static stratum_probe_stats g_stats;

/* ------------------------------------------------------------------------- */
/* 工具                                                                       */
/* ------------------------------------------------------------------------- */

static uint64_t now_realtime_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t stratum_probe_now_mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t current_thread_id(void) {
  /* 不用 pthread_self（会引入 pthread 依赖）；用 syscall 太重，
   * 这里退化为"进程内静态计数 + tid 的低位"。
   * 注意：gettid 是 syscall —— 但只在**开启时**调用一次，不在热路径。 */
  static __thread uint32_t cached = 0;
  if (cached == 0) {
    const long tid = (long)syscall(SYS_gettid);
    cached = (uint32_t)(tid & 0xffff);
    if (cached == 0) cached = 1;
  }
  return cached;
}

static void build_default_path(char* out, size_t out_len) {
  const char* env = getenv("STRATUM_PROBE_SHM");
  if (env != NULL && env[0] != '\0') {
    snprintf(out, out_len, "%s", env);
    return;
  }
  snprintf(out, out_len, "/dev/shm/stratum_probe.%ld", (long)getpid());
}

/* 容量：环境变量 STRATUM_PROBE_CAPACITY 可覆盖（测试用），
 * 但必须落在合理范围内，且向上取 2 的幂（取模更快）。 */
static uint32_t resolve_capacity(void) {
  uint32_t cap = STRATUM_PROBE_DEFAULT_CAPACITY;
  const char* env = getenv("STRATUM_PROBE_CAPACITY");
  if (env != NULL && env[0] != '\0') {
    const long v = strtol(env, NULL, 10);
    if (v >= 2 && v <= 1 << 20) cap = (uint32_t)v;
  }
  uint32_t p = 1;
  while (p < cap) p <<= 1;
  return p;
}

/* ------------------------------------------------------------------------- */
/* 打开 / 关闭                                                                */
/* ------------------------------------------------------------------------- */

int stratum_probe_open(const char* shm_path) {
  if (g_hdr != NULL) return 0; /* 已打开 */

  char path[256];
  if (shm_path != NULL && shm_path[0] != '\0') {
    snprintf(path, sizeof(path), "%s", shm_path);
  } else {
    build_default_path(path, sizeof(path));
  }
  snprintf(g_shm_path, sizeof(g_shm_path), "%s", path);

  const uint32_t cap = resolve_capacity();
  const size_t rec_size = sizeof(stratum_probe_record);
  const size_t need = sizeof(stratum_ring_header) + (size_t)cap * rec_size;

  const int fd = open(path, O_RDWR | O_CREAT, 0600);
  if (fd < 0) {
    g_open_failed = 1;
    return -1;
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    g_open_failed = 1;
    return -2;
  }

  int created = 0;
  if ((size_t)st.st_size < need) {
    if (ftruncate(fd, (off_t)need) != 0) {
      close(fd);
      g_open_failed = 1;
      return -3;
    }
    created = 1;
  }

  void* p = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    close(fd);
    g_open_failed = 1;
    return -4;
  }

  stratum_ring_header* hdr = (stratum_ring_header*)p;
  if (created || hdr->magic != STRATUM_RING_MAGIC) {
    /* 首次创建：初始化头部。用 memset 保证残留页不带来旧值。 */
    memset(p, 0, need);
    hdr->magic = STRATUM_RING_MAGIC;
    hdr->version = STRATUM_RING_VERSION;
    hdr->capacity = cap;
    hdr->record_size = (uint32_t)rec_size;
    __atomic_store_n(&hdr->write_seq, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&hdr->committed_seq, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&hdr->enabled, 1, __ATOMIC_RELEASE);
  } else {
    /* 复用已有段：沿用它的容量（可能与我们算的不同） */
    if (hdr->record_size != (uint32_t)rec_size || hdr->capacity == 0) {
      munmap(p, need);
      close(fd);
      g_open_failed = 1;
      return -5;
    }
  }

  g_fd = fd;
  g_hdr = hdr;
  g_records = (stratum_probe_record*)((char*)p + sizeof(stratum_ring_header));
  g_map_size = need;
  g_stats.capacity = g_hdr->capacity;
  return 0;
}

void stratum_probe_close(void) {
  if (g_hdr != NULL) {
    munmap((void*)g_hdr, g_map_size);
    g_hdr = NULL;
    g_records = NULL;
    g_map_size = 0;
  }
  if (g_fd >= 0) {
    close(g_fd);
    g_fd = -1;
  }
  g_init_attempted = 0;
  g_open_failed = 0;
  memset(&g_stats, 0, sizeof(g_stats));
}

/* 懒初始化：只在第一次写入时做一次（唯一会 syscall 的地方） */
static void ensure_open(void) {
  if (g_hdr != NULL) return;
  if (__atomic_load_n(&g_open_failed, __ATOMIC_ACQUIRE)) return;
  int expected = 0;
  if (!__atomic_compare_exchange_n(&g_init_attempted, &expected, 1, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    /* 别的线程正在/已经初始化；本次直接返回（这一条会被丢弃） */
    return;
  }
  (void)stratum_probe_open(NULL);
}

int stratum_probe_enabled(void) {
  if (g_hdr == NULL) {
    ensure_open();
    if (g_hdr == NULL) return 0;
  }
  return __atomic_load_n(&g_hdr->enabled, __ATOMIC_ACQUIRE) ? 1 : 0;
}

void stratum_probe_set_enabled(int enabled) {
  if (g_hdr == NULL) {
    ensure_open();
    if (g_hdr == NULL) return;
  }
  __atomic_store_n(&g_hdr->enabled, enabled ? 1u : 0u, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------------- */
/* 写入（热路径）                                                             */
/* ------------------------------------------------------------------------- */

void stratum_probe_write(const char* topic, const void* payload,
                         size_t payload_len) {
  if (topic == NULL || topic[0] == '\0') {
    ++g_stats.dropped_bad;
    return;
  }
  if (g_hdr == NULL) {
    ensure_open();
    if (g_hdr == NULL) {
      ++g_stats.dropped_bad;
      return;
    }
  }
  if (!__atomic_load_n(&g_hdr->enabled, __ATOMIC_ACQUIRE)) {
    ++g_stats.dropped_off;
    return;
  }

  const uint32_t cap = g_hdr->capacity;
  if (cap == 0) {
    ++g_stats.dropped_bad;
    return;
  }

  /* 分配槽位。若序号超过容量，说明发生了覆盖 —— 计入 dropped_full。 */
  const uint64_t seq =
      __atomic_fetch_add(&g_hdr->write_seq, 1, __ATOMIC_RELAXED);
  if (seq >= (uint64_t)cap) {
    /* 这条记录会覆盖 seq-cap 那一条，那一条已经丢过了，不重复计。 */
  }

  stratum_probe_record* r = &g_records[seq % cap];

  /* 填记录。注意顺序：先写数据，最后才提交 committed_seq。 */
  memset(r, 0, sizeof(*r));
  r->ts_realtime_ns = now_realtime_ns();
  r->ts_mono_ns = stratum_probe_now_mono_ns();
  r->seq = (uint32_t)seq;
  r->thread_id = current_thread_id();

  size_t tl = strlen(topic);
  uint32_t flags = 0;
  if (tl >= STRATUM_PROBE_TOPIC_MAX) {
    tl = STRATUM_PROBE_TOPIC_MAX - 1;
    flags |= 1u; /* 截断 */
  }
  memcpy(r->topic, topic, tl);
  r->topic[tl] = '\0';
  r->topic_len = (uint32_t)tl;

  if (payload != NULL && payload_len > 0) {
    size_t pl = payload_len;
    if (pl > STRATUM_PROBE_PAYLOAD_MAX) {
      pl = STRATUM_PROBE_PAYLOAD_MAX;
      flags |= 1u;
    }
    memcpy(r->payload, payload, pl);
    r->payload_len = (uint32_t)pl;
  } else {
    r->payload_len = 0;
  }
  r->flags = flags;

  /* 提交：此刻数据完整，读者可以从这里往后读。
   * 用 max() 保证单调（多线程下 fetch_add 顺序与提交顺序可能交错）。 */
  const uint64_t commit_to = seq + 1;
  uint64_t prev = __atomic_load_n(&g_hdr->committed_seq, __ATOMIC_RELAXED);
  while (prev < commit_to) {
    if (__atomic_compare_exchange_n(&g_hdr->committed_seq, &prev, commit_to, 1,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
      break;
    }
  }

  ++g_stats.written;

  /* 环满的统计：写满一轮之后，每次写入都意味着一条旧记录被覆盖。
   * 【诚实】判据是"序号 >= 容量"，因此第 cap+1 条开始计。 */
  if (seq >= (uint64_t)cap) ++g_stats.dropped_full;
}

void stratum_probe_write_scope(const char* topic, uint64_t dur_ns) {
  if (topic == NULL || topic[0] == '\0') return;
  char buf[96];
  const unsigned long long us = (unsigned long long)(dur_ns / 1000ull);
  const int n = snprintf(buf, sizeof(buf), "dur_us=%llu", us);
  if (n <= 0) return;
  const size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
  stratum_probe_write(topic, buf, len);
}

void stratum_probe_get_stats(stratum_probe_stats* out) {
  if (out == NULL) return;
  *out = g_stats;
  if (g_hdr != NULL) out->capacity = g_hdr->capacity;
}

/* ------------------------------------------------------------------------- */
/* 读取侧辅助（供 stratum probe 与测试使用）                                  */
/* ------------------------------------------------------------------------- */

/* 返回 ring 头部的只读视图（供读取器读 committed_seq）。内部接口。 */
const void* stratum_probe_ring_view(void) { return (const void*)g_hdr; }

/* 供测试：把最后一环的 committed_seq 读出来 */
uint64_t stratum_probe_committed(void) {
  if (g_hdr == NULL) return 0;
  return __atomic_load_n(&g_hdr->committed_seq, __ATOMIC_ACQUIRE);
}

uint64_t stratum_probe_allocated(void) {
  if (g_hdr == NULL) return 0;
  return __atomic_load_n(&g_hdr->write_seq, __ATOMIC_ACQUIRE);
}
