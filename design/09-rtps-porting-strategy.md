# RTPS 协议层：移植策略（v0 骨架，待 ref-05 依赖分析填充）

> 状态：依赖拆分分析进行中（ref-05-rtps-split.md）
> 用户决策（2026-10）：与 LevelDB 同样的处理方式 —— **拆分 Fast DDS 架构，
> 对于重复造轮子的地方，尽量把无依赖源码实现弄过来修改**。

---

## 1 策略：LSMengine 模式的复用

存储层的成功模式：

```
LSMengine（LevelDB 翻版）→ 构建为静态库 → StratumDB 业务层
                              ↑ 零重写，直接复用
```

RTPS 层应用同一模式：

```
Fast DDS 的 rtps 源码 → 筛选无依赖模块 → 移植进 Stratum → RTPS Backend 业务层
                         ↑ 整体搬 + 最小修改            ↑ 实现 IDdsBackend
```

**红线**：`/UserData/Fast-DDS` 是上游仓库，只读；移植 = 复制到 Stratum 后修改。

## 2 移植目标（预期，待分析确认）

| 类别 | 处理 | 预期模块 |
|---|---|---|
| 直接移植（无依赖） | 整体复制 + 改命名空间/头文件路径 | 线格式编解码、Submessage、基础类型（GUID/Locator/SequenceNumber） |
| 裁剪修改（依赖可替换） | 复制 + 替换依赖（Log/线程等） | SPDP/SEDP 消息、状态机 |
| 参考实现（强耦合） | 不移植，读源码后自己写 | Participant 创建链（与 DomainParticipant 深度绑定） |

## 3 Stratum 的裁剪空间（关键优势）

Stratum 只需要**只订阅不发布**的 RTPS 能力：

| 能力 | Fast DDS | Stratum 需要 |
|---|---|---|
| 发布（Writer）路径 | 完整实现 | **不需要** |
| 订阅（Reader）路径 | 完整实现 | ✅ 需要 |
| SPDP 发现 | 完整 | ✅ 需要（被动响应 + 主动探测） |
| SEDP 发现 | 完整 | ✅ 需要（只消费 Publication 公告） |
| 可靠传输 | Heartbeat/ACKNACK | ✅ 需要（作为 Reader 发 ACKNACK） |
| QoS 协商 | 完整 | 部分（录取端判定） |
| Security / Persistence / Statistics | 完整 | 不需要 |

裁剪后规模预计是 Fast DDS RTPS 的 **20-30%**。

## 4 验证方式（线格式兼容的黄金标准）

移植的正确性靠**固定测试向量**验证：

1. 用 Fast DDS 真实发送的消息抓包，保存为二进制测试向量
2. Stratum 的解码器必须能解析出相同的结果
3. 反向：Stratum 编码的 ACKNACK 必须被 Fast DDS 接受（互操作测试）

## 5 里程碑规划（待分析后细化）

- M1：线格式编解码移植 + 固定向量验证
- M2：SPDP 发现（能被 Fast DDS 参与者发现）
- M3：SEDP + 纯 Reader 数据接收（录到真实 DDS 数据）
- M4：可靠传输（ACKNACK）+ 互操作测试
- M5：接入 IDdsBackend + 端到端录制真实流量

## 6 当前状态

- 依赖拆分分析进行中（subagent）
- 待分析完成后细化移植清单与工作量
