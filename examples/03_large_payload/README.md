# 示例 03：大样本自动分片（DATA_FRAG）

**验证什么**：超过路径 MTU 的样本能否被**自动切片**发送、被对端正确**重组**。

## 为什么需要它

MTU 1500 的路径上，不分片的 UDP 上限是 **1472 字节**。超过就要依赖 **IP 分片**，
而 IP 分片在 **NAT / UDP 中继**这类路径上会被丢弃 —— 表现为
"发送端显示已发布、对端一条都收不到"，且**没有任何报错**。

Stratum 用 RTPS 的 `DATA_FRAG` 把切分放到协议层，每个报文都不超过 MTU。

## 运行

```bash
bash examples/03_large_payload/run.sh
```

## 流程

对三组 payload 各跑一遍：`1200B ×20`、`8000B ×20`、`60000B ×8`

```
make_timeline           生成指定 payload 的时间线
      │
      ▼
stratum_rtps_replay     自动探测路径 MTU → 超限则切片（domain=72）
      │
      ▼
fastdds_sub_diag        接收并重组
```

## 期望结果

```
--- payload=8000B × 20 ---
  [writer] 自动探测路径 MTU=65535 -> 单报文上限 65507 字节
  已发布样本 = 20
  订阅端收到 = 21
  ✅ PASS
```

> **注意**：单机走 loopback，探测到的 MTU 很大（如 65535），因此**不会分片**。
> 跨主机时会探测到 1500 并自动切片 —— 见 `examples/04_cross_host`。
> 这正是"按对端探测 MTU"的意义：早期版本固定探测外部地址，导致单机也被强行分片。

## 可调环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `DOMAIN` | 72 | DDS domain |

## 强制分片以便观察

```bash
STRATUM_MAX_UDP_PAYLOAD=1472 bash examples/03_large_payload/run.sh
```

## 相关源码

| 位置 | 作用 |
|---|---|
| `src/rtps/protocol/rtps_packet_utils.cc` | `BuildDataFragPackets` 分片报文构造 |
| `src/rtps/protocol/rtps_writer.cc` | 按 MTU 决定是否分片、退避重传 |
| `src/rtps/protocol/rtps_reader.cc` | `HandleDataFrag` 缓存与重组 |
| `tests/manual/test_data_frag.cc` | 分片报文的字节级验证 |
