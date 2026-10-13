# 示例 01：最小发布订阅

**验证什么**：Stratum 作为 DataWriter 能否被标准 DDS 实现（Fast DDS）正常发现、
匹配并接收数据 —— 这是"能不能互通"的最小闭环。

## 运行

```bash
bash examples/01_hello_pubsub/run.sh
```

## 流程

```
make_timeline          生成 32 条样本的时间线
      │
      ▼
stratum_rtps_replay    作为 DataWriter 回放（domain=70）
      │
      ▼
fastdds_sub_diag       Fast DDS 订阅端（同一 domain）
```

## 期望结果

```
已发布样本 = 32
订阅端收到 samples = 33（32 + 1 条首条补救副本）
✅ PASS —— Stratum 与 Fast DDS 互通正常
```

## 可调环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `DOMAIN` | 70 | DDS domain |
| `COUNT` | 32 | 样本条数 |

## 失败时看什么

| 现象 | 可能原因 |
|---|---|
| `samples=0` 且 `发现话题 0 个` | 发现没成功（domain 不一致 / 组播被挡） |
| `发现话题 1 个` 但 `samples=0` | 匹配上了但数据没到 —— 检查 SEDP 里声明的地址是否可达 |
| 只有 1 条 | 首条补救副本之外的数据没到，检查 SN 与 ACKNACK |
