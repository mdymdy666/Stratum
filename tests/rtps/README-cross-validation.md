# RTPS 交叉验证

## 为什么需要它

`test_cdr_message` 的随机往返测试只能证明编解码**自洽**——
如果 add 与 read 对称地错，测试依然全绿。

本目录的交叉验证提供**独立证据**：
真实 Fast DDS 发出的网络字节 → Stratum 移植的解码器 →
断言 RTPS 头与子消息结构。

## 运行

```bash
bash tests/rtps/run_cross_validation.sh
```

需要：
- `libfastdds.so`（`/UserData/Fast-DDS/build-fastdds/src/cpp/`）
- `libfastcdr`（`/opt/ros/jazzy/lib`）
- 绑定 UDP 7400 的权限（组播接收；无权限时仍可收到单播回环）

## 已验证结果（2026-11）

```
收到 36 个真实 RTPS 包，36/36 含有效 RTPS 头
魔数=RTPS  版本=2.2  vendor=010f(eProsima)
guidPrefix=010f1b9bde11aa3f00000000
SPDP DATA 子消息: id=0x15 flags=0x05 octetsToNextHeader=536
子消息个数=3
检查项 6，失败 0
```

`flags=0x05` 印证了字节序语义：bit0=1 表示小端序（Submessage
通过 flags 自行声明字节序，而非全局固定大端）。
