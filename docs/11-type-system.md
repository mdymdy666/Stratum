# 11 · 类型系统的决策与已知边界

> **本文的来历（先纠正一个错误）**
>
> 在 2026-10-15 的推进过程中，我（AI）曾多次引用一份
> `docs/03-open-questions.md`，并把「Q4 · 类型定义格式」当成既定编号
> 写进了任务说明与记录。**经过实际核对：这份文件与这个编号都不存在。**
>
> 本文是那次错误的修正 —— 把**真实**做的决策写下来，并给一个**真实**的
> 文档位置，避免继续引用虚构内容。
>
> 教训一并记下：**引用文档前必须核对它存在**，不要凭记忆编号。

---

## 1. 类型定义格式：选定「简化自描述（JSON）」

### 决策

`types::TypeDescriptor` 用 `definition_format` 字段区分格式。**当前唯一实现的
取值是 `"json"`**，即用 JSON 文本自描述负载结构：

```json
{"temperature_c": "double", "valid": "bool"}
```

### 为什么这么选

| 考量 | 说明 |
|---|---|
| **能立刻打通链路** | JSON 是自描述的，**不需要额外解析器**。`JsonSerializer` 直接编解码 `DynamicValue`，真解码路径当天就能端到端跑通并验证 |
| **跨语言友好** | 上层（Python 分析、Web、其它语言）都能读 JSON，不必引入 IDL 编译器 |
| **便于人读与调试** | `types show` 直接打印定义原文，出错时肉眼可查 |
| **不锁死将来** | `definition_format` 是**字符串字段**，加 `"xtypes"` / `"idl"` 分支即可，**不需要改动现有接口或存储格式** |
| **与项目原则一致** | 项目坚持"不猜"：宁可显式要求用户注册定义，也不用启发式猜测字节含义 |

### 放弃了什么（诚实记录）

* **不与 DDS 生态直接互通** —— 别人的 IDL / TypeObject 不能直接喂进来，
  需要一次转换。
* **表达力有限** —— 当前只覆盖 `null/bool/int32/int64/uint32/uint64/double/
  string/bytes/array/struct` 这 11 种 `DynamicValue::Kind`，
  不支持联合体（union）、位域、别名、继承等 IDL 特性。
* **不是标准** —— 自描述格式是本项目自定的，不具备规范背书。

### 将来接 DDS-XTypes 的路径

`TypeDescriptor` 的三个字段（`name` / `definition` / `definition_format`）
足以承载 XTypes：

1. **新增格式分支**：`JsonSerializerFactory::Create` 里按 `definition_format`
   分发。加 `"xtypes"` → 新的 `XTypesSerializer`，**不改动任何现有代码路径**。
2. **`definition` 承载类型对象**：XTypes 的 TypeObject 是二进制，`definition`
   是 `std::string`，可直接存（必要时 base64）。
3. **`DynamicValue` 不必改**：11 种 Kind 已能表达绝大多数实际负载；
   union 等可先降级为 `kStruct` + 标记字段。

**结论：接 XTypes 是"增加一个格式分支"，不是重构。**

---

## 2. 两处已知有损（格式本身的限制，不是实现缺陷）

这两条都已在 `tests/types/test_json_serializer.cc` 里写成**断言**，
目的是让将来改动时**立刻看到影响面**，而不是等下游发现数据变了型。

### 有损 1：`kBytes` 的前缀与普通字符串冲突

`kBytes` 序列化为 `"hex:0a0b"` 形式。**无法区分**"字节串"与"恰好以 `hex:`
开头、且后续为合法十六进制的字符串"。

* **取舍**：JSON 没有字节类型；二进制冒充文本会让人**误读**。
  改为 `{"$bytes": "..."}` 可无损，但所有二进制值都会变成嵌套对象，
  可读性大幅下降。
* **冲突面有限**：`"hex:0a0"`（奇数长度）、`"hex:zz"`、`"hex:"` 仍保持字符串。

### 有损 2：数字类型靠值范围推断

JSON 只有 `number`。`MakeUInt32(100)` 与 `MakeInt64(42)` 往返后都变成 `kInt32`。

* **数据本身无损**（数值完全正确），只是**类型标签**不再精确。

---

## 3. 元数据的独立 key 约定（已成型并复用三次）

新增元数据时，**不要改动既有编码格式**（会牵动版本兼容），
改用**独立 meta key + 追加式存储**。

已按此办理的三处：

| key | 用途 | 引入于 |
|---|---|---|
| `pruned/<topic_id>` | 主动裁剪的区间（使 verify 能区分"我们删的"与"数据丢了"） | `prune` |
| `typedef/<topic_name>` | 话题的类型定义（真解码路径的前提） | `types register` |
| `control_events` | 控制事件（使 verify 能区分"主动停止"与"发布者异常") | `ctl` |

配套做法：

* 对应的 `ITimeline` 接口**给默认实现返回 `NotSupported`**，
  只让 `TimelineImplV2` 覆盖 —— 保证向后兼容，不破坏既有实现类与测试替身。
* 解析时**容错**：单行格式不对就跳过该行继续，不因旁路信息损坏而让整个查询失败。
* 写入时**转义**会破坏行格式的字符（逗号、换行 → 空格）。

---

## 4. 端到端链路现状

```
DDS 负载（原始字节）
   │
   ├─ record ──────────────► 存储（payload 是不透明字节）
   │
   ├─ types register ──────► typedef/<topic_name>（JSON 自描述定义）
   │
   └─ query --decode ──────► 按定义解出结构化数据
                              四种情形可区分：
                                decoded:true              ── 成功
                                decoded:false, no_type_definition ── 无定义（降级）
                                decoded:false, decode_failed      ── 有定义但解不动
                                （未加 --decode）          ── 完全不变
```

**关键现实**：DDS 的 SEDP 不保证携带类型定义，所以 `record` 写下的
`type_definition` **常为空** —— **降级是常态，不是异常**。
因此 `--decode` 必须把"没定义"和"解不动"**明确区分开**，
否则使用者会以为"这条数据里没有内容"。

---

## 5. 相关文件

| 文件 | 内容 |
|---|---|
| `src/include/stratum/types/iserializer.h` | 接口：`TypeDescriptor` / `DynamicValue` / `ISerializer` / `ISerializerFactory` |
| `src/src/types/dynamic_value.cc` | 11 个工厂 + JSON 风格 `ToString` |
| `src/src/types/json_serializer.cc` | `PassthroughSerializer` / `JsonSerializer` / 工厂 + 内置 JSON 解析器 |
| `tests/types/test_dynamic_value.cc` | 12 用例 |
| `tests/types/test_json_serializer.cc` | 15 用例（含两处有损的断言） |
| `tests/unit/test_topic_type_definition.cc` | 15 用例（核心用例用 8 个话题逐话题校验） |
| `src/cli/cmd_types.cc` | `types register / list / show` |
