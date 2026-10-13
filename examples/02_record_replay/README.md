# 示例 02：录制 → 回放 → 校验

**验证什么**：Stratum 的核心用途 —— 把 DDS 域中流动的数据录下来，
再从录制产物回放出去，并检查**完整性**与**时序保真**。

## 运行

```bash
bash examples/02_record_replay/run.sh
```

## 流程

```
fastdds_pub_diag         发布 120 条 @ 30/s
      │
      ▼
stratum_rtps_record      录制到磁盘（domain=71）
      │  产出：一个不可变的 Session
      ▼
stratum_rtps_replay      按原速回放
      │
      ▼
fastdds_sub_diag         接收并统计
```

## 期望结果

```
录制:   hello_world_topic   ... 条
序号缺口: 0（缺 0 条）          ← 完整性
回放后订阅端收到 ≈ 录制条数      ← 时序保真
✅ PASS
```

## 可调环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `DOMAIN` | 71 | DDS domain |
| `COUNT` | 120 | 发布条数 |
| `RATE` | 30 | 发布速率（条/秒） |

## 要点

- 录制产物保存在 `build/tools/ex02_rec`，可以用 `stratum_rtps_replay` **反复回放**；
- **序号缺口**是判断"录制可不可信"的关键指标 —— 本项目的设计原则是
  "最危险的不是知道数据不全，而是以为录全了实际漏了"。

## 失败时看什么

| 现象 | 可能原因 |
|---|---|
| 录到 0 条 | 与发布端 domain 不一致；或发布端 type_name 为空导致公告失败 |
| 有缺口 | 录制侧接收缓冲不足（见 `docs/04-deployment.md` 的 `rmem_max`） |
| 回放后收得少 | 订阅端还没完成发现就开始回放了（可加大脚本里的 `sleep`） |
