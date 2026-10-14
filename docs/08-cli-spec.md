# Stratum CLI 深化设计 · 第一部分：参数级规范与语义

> **这份文档的定位**：`docs/07-cli-design.md` 回答"要做哪些命令、为什么"，
> 本文档回答"**每条命令的精确行为是什么**" —— 目标是**可照着实现、可照着验收**。
>
> 设计原则：**任何歧义都是 bug**。因此每个参数都给出
> 类型、默认值、取值域、互斥/依赖关系、以及**失败时的行为**。
>
> 状态：设计提案，未实现。日期：2026-10-14。

---

## 0 三条元规则（本设计的总纲）

| 规则 | 含义 | 违反的后果 |
|---|---|---|
| **R1 无歧义** | 任何输入的解析结果唯一；有歧义则**报错**而不是猜 | 例如裸时间 `10:00:00` 在不同时区含义不同 → 必须要求 `--tz` |
| **R2 默认安全** | 破坏性操作的默认行为是"不破坏" | `prune/delete/repair` 默认 `--dry-run` |
| **R3 失败可区分** | 不同原因导致不同退出码，不合并成"出错了" | 完整性失败(4) ≠ 环境不满足(6) |

---

## 1 通用参数（所有子命令共有）

| 参数 | 类型 | 默认 | 取值域 | 说明 |
|---|---|---|---|---|
| `--format` | enum | `table`；**非 TTY 时 `json`** | `table,json,yaml,csv` | 输出格式 |
| `--quiet` / `-q` | flag | off | | 仅输出结果行（脚本友好） |
| `--verbose` / `-v` | count | 0 | 可重复 | `-v` 信息，`-vv` 调试，`-vvv` 含报文级 |
| `--log-format` | enum | `text` | `text,json` | 日志格式（`json` 用于采集） |
| `--log-file` | path | stderr | | 日志落盘（`DP-08` 远程运维） |
| `--tz` | IANA 名称 | 系统时区 | | **解析裸时间所必需**（R1） |
| `--config` | path | `~/.config/stratum/config.yaml` | | 配置文件 |
| `--no-config` | flag | off | | 忽略配置文件（用于排查配置问题） |
| `--socket` | path | `/run/stratum/<instance>.sock` | | 控制通道 |
| `--dry-run` | flag | **见 1.1** | | 预演 |
| `--yes` | flag | off | | 跳过交互确认 |
| `--color` | enum | `auto` | `auto,always,never` | |

### 1.1 `--dry-run` 的默认值（不是全局统一，而是按危险程度）

| 命令 | 默认 | 理由 |
|---|---|---|
| `prune` / `session delete` / `session prune` | **true** | `EC-17`：设错的窗口会立刻删光数据 |
| `session repair` / `doctor --recover` | **true** | `EC-12`：恢复动作可能截断数据 |
| `prune --vacuum` | **true** | 空间回收不可逆 |
| 其他（`record` `play` `query` `verify`） | false | 不改动已有数据 |

> 想真删必须显式 `--yes`。**"忘了加 dry-run 就删库"这种事故，必须由设计杜绝，不能靠纪律。**

### 1.2 退出码（全局，`DP-15` CI 依赖）

| 码 | 名称 | 何时 | 是否可重试 |
|---|---|---|---|
| 0 | OK | 成功 | — |
| 1 | ERROR | 一般错误 | 视情况 |
| 2 | USAGE | 参数/语法错误 | 否（改命令） |
| 3 | EXISTS | 目标已存在（`record` 拒绝写非空目录） | 否（换目录或 `--overwrite`） |
| **4** | **INTEGRITY** | **完整性校验失败 / 性能门禁未达** | 否（数据或性能问题） |
| 5 | PARTIAL | 部分成功（有数据但有缺口/丢弃） | 否 |
| **6** | **ENV** | **环境不满足**（权限、`rmem_max` 过小、磁盘不足） | **是**（修环境后重试） |
| 7 | BUSY | 被锁拒绝 / 另一实例在运行 | 是（稍后） |
| 8 | NOTFOUND | 会话/话题/文件不存在 | 否 |

> **4 与 6 的区分是本项目经验的直接沉淀**：
> * `rmem_max` 默认 208KB 导致突发丢包 → 这是**环境**问题(6)，不是数据问题
> * 录制确实有缺口 → 这是**数据**问题(4)
> 若合并成一个码，CI 无法判断"该修环境还是该查数据"。

---

## 2 选择器：完整 BNF 与语义

### 2.1 语法

```bnf
selector        ::= or_expr
or_expr         ::= and_expr ( "or" and_expr )*
and_expr        ::= unary ( "and" unary )*
unary           ::= [ "not" ] primary
primary         ::= "(" or_expr ")" | condition

condition       ::= topic_cond | time_cond | meta_cond
topic_cond      ::= "topic" ( "=" | "~" | "~re" | "!=" | "in" ) topic_value
time_cond       ::= ( "since" | "until" | "within" ) time_value
meta_cond       ::= ( "seq" | "instance" | "size" | "type" | "qos" "." ident )
                    compare_op literal

topic_value     ::= string | "(" string ( "," string )* ")"
time_value      ::= abs_time | rel_time | session_offset | interval
abs_time        ::= rfc3339 | "HH:MM:SS"
rel_time        ::= ( "+" | "-" ) duration
session_offset  ::= "@" ( session_id | "session" ) ( "+" | "-" ) duration
interval        ::= "[" time_value "," time_value ")"
duration        ::= number ( "ms" | "s" | "m" | "h" | "d" | "w" )
compare_op      ::= "=" | "!=" | "<" | "<=" | ">" | ">="
```

### 2.2 语义裁决（每条都必须明确，否则实现会各写各的）

| 问题 | 裁决 | 理由 |
|---|---|---|
| `since`/`until` 的开闭 | **半开 `[since, until)`** | 与 `TimeRange` 一致；相邻区间可直接拼接 |
| `HH:MM:SS` 无时区 | **报错**（除非 `--tz` 或配置给了默认） | R1 无歧义 |
| `-30m` 的基准 | **wall clock now（本地）** | 用户直觉 |
| `@session+5s` 的基准 | **该 session 的 `created_at_ns`** | 会话内相对时间 |
| `@session-5s` 为负 | **报错**（会话开始前不存在） | 避免静默截断 |
| `topic ~ "sensor/*"` 的 `*` | **匹配任意层**（`sensor/a/b` 也算） | 与项目现有实现一致；**但 `Q9` 未定，故写入 `--strict-glob` 可切换为单层** |
| `topic ~re "..."` | 正则，**区分大小写** | 显式即安全 |
| `size > 1KB` 的单位 | 十进制 KB=1000？二进制 KiB=1024？ | **裁决：`KB/MB/GB` 为 1000 进制，`KiB/MiB/GiB` 为 1024 进制**；不带单位的数字按**字节** |
| 空选择器 | **`prune`/`delete` 报错**（拒绝全删）；`query`/`export` 允许（等于全选） | R2 默认安全 |
| `not` 与 `!=` 的优先级 | `not` 只作用于紧随的 `primary` | 避免歧义 |
| 多个 `since` | **报错**（重复条件） | R1 |

### 2.3 选择器的可审计性（`SC-12` 要求）

**裁决**：选择器字符串**原样**记入审计日志与会话元数据，不做规范化改写。
理由：`SC-12` 要求证明"当时用的是什么条件"，规范化会丢失用户原意。

---

## 3 `record` 的完整参数规范

### 3.1 参数分组

**目标与身份**

| 参数 | 类型 | 默认 | 取值域 | 说明 |
|---|---|---|---|---|
| `--dir` | path | 必填 | | 会话根目录 |
| `--session-name` | string | `auto:YYYYMMDD-HHMMSS` | | 会话名；重名则**报错(3)** |
| `--instance-id` | string | `default` | | `DP-09` 多实例；影响锁与 socket 名 |
| `--overwrite` | flag | off | | 允许写非空目录（**危险**，需 `--yes`） |

**话题选择（`FR-R-1/2` `EC-16`）**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--include` | selector | 空 = 全部（自动发现） | |
| `--exclude` | selector | 空 | **排除优先于包含** |
| `--on-missing-topic` | enum | `warn` | `fail` 启动即报错(2)；`warn` 记警告继续；`wait` 阻塞等待出现（配 `--wait-timeout`） |
| `--wait-timeout` | duration | `0`（无限） | 仅 `wait` 时有效 |
| `--auto-discover-timeout` | duration | `2s` | 等待发现完成后才开始计时 |

**DDS 连接（`FR-R-5` `DP-12`）**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--domain` | u32 | 0 | |
| `--participant-name` | string | `stratum-recorder-<instance>` | |
| `--advertised-address` | ip | 空 = 自动探测 | **跨网段时必须显式给**（本项目踩过：不设则 NAT 下对端不可达，症状是"发现成功、SEDP 正常、Writer 已匹配但收不到数据"） |
| `--initial-peers` | ip list | 空 = 组播 | 逗号分隔；不依赖组播（`WSL` 环境组播受限） |
| `--qos-profile` | path | 空 | 期望 QoS（`FR-R-5` 快照的对象） |

**保留与容量（`FR-R-7` `EC-11` `EC-17` `NFR-2.4`）**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--retain` | duration | `0`（不限） | 时间维度 |
| `--retain-bytes` | size | `0`（不限） | 空间维度 |
| `--on-full` | enum | `drop-newest` | **写缓冲满时**：`drop-newest`/`drop-oldest`/`block` |
| `--on-disk-full` | enum | `stop` | **磁盘满时**：`stop`/`drop-oldest`/`warn-and-continue` |
| `--min-free` | size | `1GB` | 低于此值触发 `--on-disk-full` |
| `--retention-check-interval` | duration | `1s` | |

> **裁决：`--on-full` 与 `--on-disk-full` 是两件事**
> * `--on-full`：**内存**缓冲满（消费者跟不上）→ 对应 `RC-1`、`Q10`
> * `--on-disk-full`：**磁盘**满 → 对应 `EC-11`
> 二者成因、后果、对质量报告的影响都不同，**不能合并成一个开关**。

**裁决：`--retain` 与 `--retain-bytes` 同时给定时**
取**先到者**触发淘汰；两者都是"保留至少这么旧/这么多"的下界语义 ——
即"只要任一维度未达上限，就不删"。

**分片（`FR-R-8`）**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--split-by-bytes` | size | `256MB` | 单个地层文件上限 |
| `--split-by-duration` | duration | `0` | |
| `--split-by-events` | count | `0` | |

**裁决**：三个条件**任一满足即切分**（OR 语义），这是 `FR-R-8` 的自然读法。

**性能预算（`FR-R-6` `NFR-1.2` `NFR-4`）**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--budget-cpu` | ratio | `0`（不限） | 如 `0.5` = 半个核 |
| `--budget-io` | rate | `0`（不限） | 如 `50MB/s` |
| `--budget-mem` | size | `256MB` | |
| `--on-budget-exceeded` | enum | `warn` | `warn`/`drop-newest`/`stop` |
| `--write-batch-size` | count | `256` | 越大吞吐越高、延迟越大 |
| `--durability` | enum | `balanced` | `relaxed`/`balanced`/`strict` |

**运行形态（`DP-01` ~ `DP-03` `FR-R-10`）**

| 参数 | 类型 | 默认 | 互斥/依赖 |
|---|---|---|---|
| `--continuous` | flag | off | 与 `--duration` **互斥** |
| `--duration` | duration | | 与 `--continuous` **互斥** |
| `--schedule` | cron(5 字段) | | **隐含 `--continuous`**；要求 `--tz` |
| `--window` | duration | | 依赖 `--schedule` 或 `--continuous` |
| `--window-keep` | count | | 依赖 `--window` |
| `--trigger-on` | selector | | 与 `--schedule` **互斥** |
| `--pre-buffer` | duration | `0` | 依赖 `--trigger-on` |
| `--post-buffer` | duration | `60s` | 依赖 `--trigger-on` |
| `--foreground` | flag | off | `DP-06` 容器（等价 `--log-file -`） |
| `--health-check` | path | | 暴露健康端点（`DP-06`） |

**时钟策略（`EC-01/02/04` `NFR-2.5`）**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--clock-policy` | enum | **`clamp`** | `reject`/`clamp`/`accept` |
| `--clock-skew-warn` | duration | `100ms` | 超过则告警 |

> **裁决：默认 `clamp` 而非 `accept`**
> 因为源时间戳倒退会破坏 **"字节序 == 时间序"** 这一 LSM 不变量
> （`tests/unit/test_time_key.cc` 专门验证过它）。`accept` 会让范围扫描结果错乱，
> 且症状隐蔽（"结果差不多对但顺序不对"）。`reject` 又太激进（丢数据）。
> `clamp` 把倒退的时间戳夹到上一条之后，**并计数告警** —— 数据保留、事实留痕。

**其他**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--ignore-self` | flag | **on** | 防止录到自己（`EC-10`） |
| `--lock` | path | `<dir>/.stratum.lock` | `DP-09` |
| `--allow-multi-writer` | flag | off | `EC-15` 显式放行 |
| `--max-samples-per-sec` | rate | `0` | 限速（调试用） |
| `--debug-drop-send-percent` | u32 | `0` | 故障注入（仅测试） |

### 3.2 `record` 的状态机

```
                      ┌─────────┐
   argv 解析成功 ────▶ │ opening │  取锁、建目录、校配置
                      └────┬────┘
              失败(3/6/7)  │ 成功
                           ▼
                    ┌─────────────┐
                    │ discovering │  发 SPDP、等 SEDP；--auto-discover-timeout
                    └──────┬──────┘
         --on-missing=fail │ 发现完成（或缺话题但非 fail）
                           ▼
      ┌────────────────▶┌───────────┐◀────────────────┐
      │                 │ recording │                 │
      │   ctl resume    └─────┬─────┘   ctl pause     │
      │                       │                       │
      │                       │ rotate / window 切换   │
      │                       ▼                       │
      │                 ┌───────────┐                 │
      └─────────────────│ rotating  │─────────────────┘
                        └─────┬─────┘
                              │
        ctl stop / 期限到 / 磁盘满  │
                              ▼
                        ┌───────────┐
                        │ draining  │  把队列写完（--drain-timeout，默认 30s）
                        └─────┬─────┘
                              ▼
                        ┌───────────┐
                        │  closing  │  写元数据、释放锁
                        └─────┬─────┘
                              ▼
                        ┌───────────┐
                        │  closed   │  退出码 0 或 5（有缺口）
                        └───────────┘

  任意状态崩溃 ──▶ 下次启动检测到 stale lock + 未完元数据 ──▶ recovering ──▶ closed|recording
```

**每个状态下"允许什么"**：

| 状态 | `ctl stop` | `ctl pause` | `ctl add` | `ctl snapshot` | `query`（同会话） | `prune`（同会话） |
|---|---|---|---|---|---|---|
| `opening` | ✗ BUSY | ✗ | ✗ | ✗ | ✗ NOTFOUND | ✗ BUSY |
| `discovering` | ✅ | ✅ | ✅ | ✗ | ⚠️ 空结果 | ✗ BUSY |
| `recording` | ✅ | ✅ | ✅ | ✅ | ✅（已落盘部分） | ⚠️ 默认拒绝 |
| `rotating` | ✅ | ✅ | ✗ BUSY | ✅ | ✅ | ✗ BUSY |
| `draining` | ✅（幂等） | ✗ | ✗ | ✅ | ✅ | ✗ BUSY |
| `closing` | ✅（幂等） | ✗ | ✗ | ✗ | ✅ | ✅ |
| `closed` | ✗ NOTFOUND | ✗ | ✗ | ✗ | ✅ | ✅ |

> **"⚠️ 默认拒绝"** 指需要 `--allow-live-prune`：对正在写入的会话做删除，
> 语义（删掉的是不是还会有新数据进来？）必须由用户显式确认。

### 3.3 锁的语义（`DP-09` `EC-15`）

**锁文件**：`<dir>/.stratum.lock`，内容为 JSON：

```json
{
  "instance_id": "camera-a",
  "pid": 12345,
  "host": "line3-gateway",
  "started_at": "2026-10-14T10:00:00+08:00",
  "stratum_version": "1.0.0",
  "config_hash": "sha256:..."
}
```

**获取规则**：

| 情况 | 行为 |
|---|---|
| 锁不存在 | 创建并继续 |
| 锁存在，PID 存活且是本机 | **拒绝(7 BUSY)**，打印持有者信息 |
| 锁存在，PID 不存在（**stale**） | 打印"检测到陈旧锁"，**要求 `--break-lock`** 才抢占（不自动抢） |
| 锁存在，`host` 不是本机（共享存储） | 拒绝(7)，提示可能是另一台机器在写 |
| `--allow-multi-writer` | 改用 `.stratum.lock.<instance-id>`，允许多写者共存 |

> **裁决：stale lock 不自动抢占**
> 因为共享存储场景下"PID 不存在"可能只是本机视角（另一台机器在写）。
> 自动抢占会造成两个写入者——这正是 `EC-15` 要防的。

### 3.4 并发矩阵（`EC-21/22/23`）

| 操作 A | 操作 B | 允许？ | 语义 / 保护 |
|---|---|---|---|
| `record` | `query`（同会话） | ✅ | 查询看到**已落盘的快照**；正在写缓冲的数据**不可见**（诚实，不假装实时） |
| `record` | `snapshot` | ✅ | 快照共享底层数据，**引用计数**保护（否则 10GB 会话做快照要再花 10GB） |
| `record` | `verify`（同会话） | ✅ | 只校验已落盘部分，报告中标注"会话仍在写入" |
| `record` | `prune`（同会话） | ⚠️ | 需 `--allow-live-prune`；且**不得删除正在写入的地层** |
| `record` | `play`（同 domain 同话题） | ⚠️ | 需 `--allow-loopback`；默认拒绝（`EC-18`：下游会看到两份数据） |
| `record` | `record`（同目录） | ❌ | 锁拒绝(7)，除非 `--allow-multi-writer` |
| `play` | `play` | ✅ | 各自独立 |
| `query` | `query` | ✅ | |
| `play` | `ctl play pause` | ✅ | 控制通道按 `--control-socket` 区分实例 |

---

## 4 `play` 的完整参数规范

### 4.1 参数

**源与范围**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `session` | 位置参数 | 必填 | 会话名或路径 |
| `--where` | selector | 空 = 全部 | 选择要回放的数据子集 |
| `--from` | time_value | 会话起点 | 支持 `@session+5s` |
| `--to` | time_value | 会话终点 | **半开** |
| `--order` | enum | `source_ts` | `source_ts`/`reception_ts`/`seq` |

> **裁决：`--order` 默认 `source_ts` 而不是 `reception_ts`**
> 回放要还原**当时的时序**，源时间戳才是"当时"。`reception_ts` 只用于诊断。
> （项目实测：注入时钟异常后两种排序位置差异达 8.47%，选错会改变回放顺序。）

**速率**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--rate` | float | `1.0` | `0` = 尽可能快 |
| `--rate-mode` | enum | `ratio` | `ratio`（等比压缩所有间隔）/ `compress-gaps`（只压缩无数据的长空档） |
| `--max-rate` | rate | `0` | 硬上限（条/秒） |

> **裁决：`ratio` 与 `compress-gaps` 必须都支持**
> * `ratio`：10 倍速 → 1 小时的录制 6 分钟放完（**所有间隔等比缩小**）
> * `compress-gaps`：间隔超过 `--gap-threshold` 的部分才压缩，其余保持原速
>   → "10 倍速但仍需 1 小时"（只是把'没数据的等待'去掉）
> 两者用途完全不同：前者是"快速过一遍"，后者是"跳过无关时段但保持真实节奏"。
| `--gap-threshold` | duration | `2s` | 仅 `compress-gaps` 有效 |

**循环（`FR-P-6`）**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--loop` | flag | off | |
| `--loop-count` | count | `0`（无限） | |
| `--loop-gap` | duration | `0` | 两轮之间的间隔 |
| `--loop-until` | abs_time | | 与 `--loop-count` **互斥** |
| `--rewrite-ts` | flag | **off** | 是否重写源时间戳为当前时间 |

> **裁决：`--rewrite-ts` 默认 off**
> 循环回放同一段时间时，若不重写时间戳，下游会看到**时间倒流**（第二轮的时间戳比第一轮早）。
> 但重写会丢掉"当时"的语义。这是真实矛盾，必须让用户选，且**默认保持原时间戳**
> （因为"回放就是还原当时"，改写是特殊用法）。

**话题映射与冲突（`FR-P-4` `EC-18`）**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--remap` | `src=dst`（可重复） | | |
| `--require-idle` | selector | | 匹配的话题若有活跃发布者则**拒绝启动(7)** |
| `--on-topic-conflict` | enum | `abort` | `abort`/`warn`/`suffix`（自动加 `_replay`） |
| `--allow-loopback` | flag | off | 显式允许与实时发布者共存 |

**QoS（`FR-P-5` `EC-19`）**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--qos-restore` | enum | `recorded` | `recorded`/`default`/`override` |
| `--qos-override` | `k=v`（可重复） | | 如 `reliability=best_effort` |
| `--on-qos-incompatible` | enum | `abort` | `abort`/`downgrade`/`ignore` |

> **裁决：`--on-qos-incompatible` 默认 `abort`**
> `EC-19` 是 Stratum 特有的坑：录制时是 RELIABLE、回放时对端只有 BEST_EFFORT 时，
> 静默降级会让用户以为"回放成功了"但实际丢包。**必须让失败显式**（R3）。

**故障注入（`SC-08`）**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--inject` | 表达式（可重复） | | `drop=5%`、`delay=200ms jitter=50ms`、`reorder=window:10` |
| `--inject-seed` | u64 | 随机 | **未指定时打印实际使用的种子** |

> **裁决：`--inject-seed` 未指定时随机，但必须打印出来**
> 否则 `SC-08`"录正常、回放异常"的对比实验无法复现。

**结束与控制（`Q2` `Q3`）**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--on-finish` | enum | `exit` | `exit`（结束就退出）/ `stay`（追平后持续跟随实时，`Q2`） |
| `--wait-for-ack` | flag | off | 结束前等确认 |
| `--ack-timeout` | duration | `5s` | |
| `--republish-unacked` | flag | **on** | 用新 SN 补发首条（`VOLATILE` 下必需） |
| `--interactive` | flag | off | 前台交互（`p` 暂停 / `s` 单步 / `q` 退出） |
| `--control-socket` | path | 继承全局 | |

**CI（`DP-15`）**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--expect-count` | `topic=N`（可重复） | | 不满足则退出码 **4** |
| `--expect-duration` | duration ±tolerance | | |
| `--timeout` | duration | `0` | 超时退出码 **1** |
| `--exit-on-mismatch` | flag | off | |

---

## 5 `query` 的完整参数规范

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `session` | 位置参数 | 必填 | |
| `--where` | selector | 空 = 全部 | |
| `--list-topics` | flag | | 只列话题摘要（`FR-Q-1`） |
| `--state-at` | time_value | | 定点状态（`FR-Q-3`）**与 `--where` 可共存**（先筛话题再定点） |
| `--all-topics` | flag | | 配 `--state-at` 使用 |
| `--stream` | flag | off | 流式输出（大结果必需） |
| `--limit` / `--offset` | count | `0` | 分页 |
| `--out` | path | stdout | |
| `--group-by` | enum | | `topic`/`minute`/`instance` |
| `--aggregate` | enum | | `count`/`bytes`/`rate` |
| `--percentiles` | list | | 如 `50,95,99` |

**定点查询的输出必须区分三种状态**（对应实现的 `StateAtResult::Kind`）：

```json
{"topic":"sensor/temp","state":"found","as_of":"2026-10-14T10:05:00.123+08:00","seq":8421}
{"topic":"sensor/temp","state":"disposed","as_of":"2026-10-14T10:05:00.123+08:00"}
{"topic":"sensor/temp","state":"out_of_range","requested":"2026-10-20T00:00:00+08:00",
 "session_range":["2026-10-14T10:00:00+08:00","2026-10-14T11:00:00+08:00"]}
```

> **裁决：`kNotFound` / `kDisposed` / `kOutOfRange` 必须是三个不同的值**
> 合并成"没有数据"会让 `SC-02` 的结论不可靠：
> * `not_found` = 那一刻之前确实没数据（发布者还没开始）
> * `disposed` = 那一刻之前被显式注销了
> * `out_of_range` = **查询本身问错了范围**（不是数据的问题）
> 三者对复盘的含义完全不同。

---

## 6 `prune` 的完整参数规范（危险命令）

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `session` | 位置参数 | 必填 | |
| `--where` | selector | **必填** | **空选择器直接报错(2)** —— 拒绝"无条件的全删" |
| `--dry-run` | flag | **true** | |
| `--yes` | flag | off | 真删所必需 |
| `--to-quarantine` | path | | 移入隔离区而非删除（可恢复） |
| `--keep-metadata` | flag | **on** | 保留话题元数据并标 `empty` |
| `--max-delete-bytes` | size | `10GB` | **超过则拒绝(2)** —— 防手滑 |
| `--max-delete-percent` | percent | `50%` | **超过则拒绝(2)** |
| `--i-know-what-i-am-doing` | flag | off | 绕过上面两个上限 |
| `--vacuum` | flag | off | 回收空间（耗时；默认 dry-run） |
| `--allow-live-prune` | flag | off | 允许对正在录制的会话操作 |

### 6.1 `--dry-run` 的输出契约（可作黄金文件测试）

```
$ stratum prune s --where 'topic ~ "sensor/debug_*" and since -7d'
DRY RUN — 未修改任何数据
  会话:      s-20261014
  选择器:    topic ~ "sensor/debug_*" and since -7d
  将删除:
    话题:     3 个 (sensor/debug_a, sensor/debug_b, sensor/debug_c)
    条数:     1,284,551
    字节:     2.1 GB (占会话 18.3%)
    时间范围: 2026-10-07T10:00:00+08:00 ~ 2026-10-14T10:00:00+08:00
  保护检查:
    --max-delete-bytes   10GB   通过
    --max-delete-percent 50%    通过
  元数据:    保留（话题将标记为 empty）
  审计:      将写入 /data/stratum/.audit/20261014T101500-prune.json
  执行请加 --yes
```

> **这段输出是设计的一部分，不是实现细节。**
> 它必须包含：选了什么、删什么、多少、**保护检查是否通过**、元数据怎么办、审计写到哪。
> 缺任何一项，用户在按下 `--yes` 前就少了一个判断依据。

### 6.2 元数据保留的裁决

**裁决：删到一条不剩时，`TopicMetadata` 保留并标 `state: "empty"`。**

理由：`FR-R-11` 要求录制产物**自描述**。如果某话题被删空后元数据一起消失，
事后就无法回答"这个话题曾经存在过吗、它的类型是什么"。
`SC-12` 审计场景恰恰需要这个事实。

---

## 7 `ctl` 的完整参数规范（运行时控制）

### 7.1 子命令与参数

| 子命令 | 参数 | 语义 | 是否写入元数据 |
|---|---|---|---|
| `stop` | `--topic <sel>` `--reason <str>` | **从此不再录该话题**（时间轴留空白） | ✅ **必须** |
| `resume` | `--topic <sel>` | 恢复录制 | ✅ |
| `pause` | `--all` \| `--topic <sel>` | 暂时不录（时间轴连续，标记"无数据"） | ✅ |
| `add` | `--topic <name>` `--type <name>` | 动态加入新话题 | ✅ |
| `drop` | `--topic <sel>` | 移除话题（同 `stop` 但语义更强：不再关注） | ✅ |
| `set` | `--retain` `--budget-*` `--on-full` … | 动态调参 | ✅ |
| `snapshot` | `--name <str>` | 录制中固化 | ✅ |
| `rotate` | `--reason <str>` | 强制切分地层 | ✅ |
| `status` | `--json` | 查询状态 | ✗ |
| `metrics` | `--format prometheus` `--listen :9100` | 指标 | ✗ |
| `shutdown` | `--drain-timeout <dur>` | 优雅停止整个录制 | ✅ |

### 7.2 最关键的一条裁决：`stop` 必须留痕

**`ctl stop --reason` 是必填参数**（不是可选）。

理由（这是本设计里最重要的一条）：

> 假如没有留痕，事后 `verify` 看到"sensor/debug_* 在 10:05 之后没有数据"，
> 会把它报告为**时间轴空洞**（`RC-4`）或**发布者停止**（`RC-3`）。
> 但真实原因是**我们主动停了它**。
>
> 于是复盘结论会完全错：工程师去查发布者的日志，而问题其实在我们这边。
>
> **这正是项目第一设计原则要防的事** —— "最危险的不是知道数据不全，
> 而是以为录全了/以为知道原因，实际搞错了"。

因此：

* `ctl stop` 的 `--reason` **必填**，并写入会话元数据的 `control_events` 数组；
* `verify` 报告必须把"人为停止"与"被动空洞"**分类呈现**；
* `ctl resume` 同样记录，形成完整的时间线。

元数据形态：

```json
"control_events": [
  {"at":"2026-10-14T10:05:00+08:00","action":"stop","topics":["sensor/debug_a"],
   "reason":"调试话题，现场反馈占用带宽","operator":"ops-zhang"},
  {"at":"2026-10-14T14:00:00+08:00","action":"resume","topics":["sensor/debug_a"],
   "reason":"重新开启排查","operator":"ops-zhang"}
]
```

### 7.3 `set --retain` 调小的保护

`set --retain 1h`（从 24h 调到 1h）会**立即触发删除 23 小时的数据**。

**裁决**：此类"调小"必须：
1. 先执行 `--dry-run` 等价输出（即使没给 `--dry-run`）；
2. 要求 `--yes`；
3. 受 `--max-delete-bytes` / `--max-delete-percent` 约束。

即 **`ctl set` 在"参数变严格"时，按 `prune` 的危险等级对待**。

---

## 8 横切规范

### 8.1 错误消息格式（三段式）

```
错误: <症状>
  原因: <为什么>
  动作: <怎么办>
```

示例：

```
错误: 无法绑定接收套接字，实际生效缓冲 425984 字节（请求 8388608）
  原因: net.core.rmem_max = 208896 限制；高突发时会丢包（实测丢包率可达 87%）
  动作: 以 root 执行 `sysctl -w net.core.rmem_max=67108864`，
        或运行 `stratum doctor --fix-rmem` 查看具体命令
  退出码: 6 (环境不满足)
```

> 这条规范直接来自项目经验：`rmem_max` 问题排查花了很多时间，
> 而工具当时只说了一句"请求 8388608 实际生效 425984"，没有说"所以会怎样、该怎么办"。

### 8.2 配置文件的 schema 版本

```yaml
schema_version: 1        # 必填；更高的版本必须被拒绝（对应 FR-S-2）
instance:
  id: camera-a
record:
  dir: /data/stratum
  include: 'topic ~ "sensor/*"'
  retain: 24h
```

**裁决**：`schema_version` **高于**程序支持时**拒绝启动(2)**，
并明确说明"本程序支持到 v1，配置是 v2"。这与会话格式版本的处理一致（`FR-S-2`）。

### 8.3 审计日志

所有**改变数据**的命令（`prune` / `session delete` / `ctl stop|drop|set` / `repair`）
写一条 JSON Lines 记录到 `<dir>/.audit/`：

```json
{"ts":"2026-10-14T10:15:00.123+08:00","action":"prune","session":"s-20261014",
 "selector":"topic ~ \"sensor/debug_*\" and since -7d",
 "deleted":{"topics":3,"events":1284551,"bytes":2254857830},
 "operator":"ops-zhang","host":"line3-gateway","stratum_version":"1.0.0","result":"ok"}
```

**选择器原样记录**（见 2.3），因为 `SC-12` 要求证明"当时用的是什么条件"。

### 8.4 可测试性（设计即测试规格）

每个命令的 `--dry-run` 输出都是**稳定的文本契约**，
可直接作为黄金文件（golden file）测试：

* `prune --dry-run` → 6.1 节那段输出
* `record --dry-run` → 展开后的所有生效配置（含"最老数据会在 X 小时后被删"）
* `config explain` → 逐项解释（`EC-17` 防线）
* `play --dry-run` → 将发布多少条、映射到哪些话题、预计耗时

**这意味着 CLI 的测试不需要真跑数据** —— 大量设计可以直接被断言。

---

## 9 与 `docs/07-cli-design.md` 的关系

本文档是 07 的**下钻**：

| 07 回答 | 本文档回答 |
|---|---|
| 要做哪些命令 | 每条命令的**精确参数与语义** |
| 32 个场景是什么 | 场景背后的**状态机、锁、并发、失败行为** |
| 为什么需要这些 | 每个裁决的**依据（含项目踩过的坑）** |

**下一步**：第二部分将对 32 个场景逐个再发散
（边界分支、失败模式、场景交互），并从发散中**回收新的设计约束**到本文档。
