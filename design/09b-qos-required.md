# 修正 RTPS 移植策略：QoS 为必需能力

> 修正日期：2026-10
> 修正原因：用户指出"QoS 为啥不要，这个也要"——判断失误，QoS 是入口条件

---

## 修正前（错误判断）

| 能力 | 需要与否 |
|---|---|
| QoS 协商完整版 | ❌ 砍 |

## 修正后

| 能力 | 需要与否 | 理由 |
|---|---|---|
| **QoS 类型系统（37 个策略）** | ✅ 移植 | 数据结构的定义 |
| **QoS 编解码（QosPoliciesSerializer）** | ✅ 移植 | SEDP 消息里 Writer 的 QoS 公告要靠它解析 |
| **QoS 兼容性判定** | ✅ 移植 | **匹配的先决条件**——不兼容的 Writer 不会与 Reader 建立关联，录不到数据 |
| QoS 协商的完整状态机 | 部分 | 移植判定逻辑；砍掉"作为 Writer 的主动协商"路径 |

---

## 为什么 QoS 是入口条件（不是附加功能）

DDS 的发现 → 匹配流程：

```
SEDP 公告：Writer 发布 "话题 T + QoS W"
                    ↓
Stratum Reader 收到公告 → 解析 QoS W（需要编解码）
                    ↓
检查兼容性：Reader 请求的 QoS 与 W 是否相容（需要判定逻辑）
                    ↓
不相容 → 不建立关联 → 收不到任何数据
相容   → 建立关联 → 开始接收
```

**没有 QoS 解析与判定，Stratum 的 Reader 连"匹配"这关都过不去。**

## 对录制语义的额外意义

1. 录制的数据必须**同时记录其 QoS**（可靠性、持久性、截止时间等）——否则回放时无法还原数据的语义（比如"这条数据本来要求可靠传输"）
2. 这正是最初需求文档中 QoS 配置的来源（33 需求中的 QoS 相关项）
3. Fast DDS 学习阶段的 QoS 笔记（`LearnNotes/notes/03-qos-system.md`，37 策略 ID、推导规则）可以直接用于移植验证

## 更新的裁剪表

| 能力 | 需要与否 |
|---|---|
| Reader 路径 + DATA 解析 | ✅ |
| SPDP/SEDP 发现（消费侧） | ✅ |
| Heartbeat/ACKNACK | ✅ |
| **QoS 类型 + 编解码 + 兼容判定** | ✅ **必需** |
| 不砍：QoS 是匹配入口 | — |
| Writer/发布路径 | ❌ 砍 |
| Security/Persistence/Statistics | ❌ 砍 |
| 完整协商状态机（Writer 侧） | ❌ 砍 |

## 对里程碑的影响

- M1 线格式编解码：追加 QoS 策略编解码（QosPoliciesSerializer 移植）
- M2 SPDP 发现
- M3 SEDP：**含 Writer QoS 公告解析与兼容性判定**（新增验证点：QoS 不兼容的 Writer 必须正确拒绝）
- M4 可靠传输 + 互操作
- M5 端到端

## 验证新增项

1. 固定向量：SEDP 消息中携带的 QoS 参数（不同策略组合）能正确解析
2. 兼容性判定矩阵：Reliability/History/Durability 等关键策略的相容表对拍 Fast DDS
3. 录制元数据：录制的样本必须携带其 QoS 快照（回放时可用）
