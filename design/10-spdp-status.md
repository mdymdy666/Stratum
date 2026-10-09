# SPDP 状态报告（M2）

> 更新：2026-11 | 全量 25 套件 100% 通过

---

## 1 已完成：SPDP 解析（消费侧）

**文件**：`src/include/stratum/rtps/builtin/spdp_parser.h` + `src/rtps/builtin/spdp_parser.cc`

**实现方式（移植优先）**：
- ParameterList 遍历：**复用移植的模板** `ParameterList::readParameterListfromCDRMsg`
  （上游 `core/policy/ParameterList.hpp:73-150`，随 53 个 IDENTICAL 文件一并移植）
- DATA 子消息定位 + PID 处理器：自写（参照上游 `ParticipantProxyData.cpp`）

**可解析字段**：参与者 GUID、租约时长、内建端点集、协议版本、vendorId、
domainId、四类 locator（metatraffic/default × unicast/multicast）、参与者名称。

**验证（4 用例，全部通过）**：

| 测试 | 规模 | 结果 |
|---|---|---|
| ParseRealFastDdsAnnouncement | 36 个真实 Fast DDS 包 | GUID/租约/端点/locator 全解析，malformed=0 |
| RandomBytesStress | 30,000 次 | 不崩溃、不越界 |
| TruncationStress | 真实包逐字节截断 | <20 字节必无输出 |
| RepeatedParseStress | 2,000 轮 × 36 包 | 稳定 |

---

## 2 未完成：SPDP 主动探测（发送侧）

**文件**：`src/include/stratum/rtps/builtin/spdp_announcer.h` + `.cc`
**状态**：代码已写，**未纳入构建**（`src/CMakeLists.txt` 中已移除该 .cc）

**已完成部分**：
- 字段顺序照抄上游 `ParticipantProxyData::write_to_cdr_message`
  （PROTOCOL_VERSION → VENDORID → PRODUCT_VERSION → DOMAIN_ID →
   PARTICIPANT_GUID → NETWORK_CONFIGURATION_SET → locators →
   BUILTIN_ENDPOINT_SET → LEASE_DURATION → SENTINEL）
- 各 `ParameterXxx_t` 结构填充
- DATA 子消息 + RTPS 头组装（复用移植的 CDRMessage 原语）
- UDP 组播发送器（239.255.0.1:7400+domain×250，含组播回环）

**未收敛的三处**（恢复构建前必须解决）：

1. **`PL_CDR_LE` 常量来源**
   上游 `ParameterList::writeEncapsulationToCDRMsg` 用 `PL_CDR_LE - msg->msg_endian`；
   该宏在 Fast DDS 中定义位置未定位到（`ParameterTypes.hpp` 中无）。
   → 需定位或直接采用数值（PL_CDR_LE=0x0003, PL_CDR_BE=0x0002）并验证字节序关系。

2. **`Parameter_t` sentinel 的写法**
   `ParameterSerializer<Parameter_t>::add_parameter_sentinel(&msg)` 是正确调用
   （上游 `ParameterSerializer.hpp:136-147`），但当前 .cc 中仍残留旧写法。

3. **`SerializedPayload.hpp:46` 宏冲突**
   `expected unqualified-id before numeric constant`，疑与 PL_CDR 宏或
   `FASTDDS_IS_BIG_ENDIAN_TARGET` 相关，需定位。

---

## 3 重要教训：依赖链发散

移植 `ParameterList.cpp`（276 行）触发了一条**发散依赖链**：

```
ParameterList.cpp
  → QosPolicies.hpp
      → attributes/{PropertyPolicy, RTPSParticipantAttributes,
                    ResourceManagement, ThreadSettings,
                    RTPSParticipantAllocationAttributes}.hpp
          → transport/TransportInterface.hpp → SenderResource.hpp → ...
              → flowcontrol/FlowControllerDescriptor.hpp → ...
                  → transport/network/{NetworkInterface, NetmaskFilterKind,
                                       AllowedNetworkInterface}.hpp
```

**实测：自动补齐 19 个文件后仍未收敛**（第 7 轮才"齐备"，紧接着又报
`FlowControllerSchedulerPolicy.hpp` 缺失）。

**结论（ref-05 的正确性得到验证）**：
> 遇到"一个 .cpp 拖入半个框架"时，**不要跟进依赖链**，而应回到
> **按需提取目标函数**（与 IPLocator 的处理一致：1331 行 → 190 行）。

本例中 `writeEncapsulationToCDRMsg` 只有 **6 行**，已内联进 `spdp_announcer.cc`
（标注上游行号），`ParameterList.cpp` 已删除。

**副作用**：那 19 个被自动补齐的文件仍在树中（无害；若 SEDP/QoS 阶段需要可直接复用）。
建议在 M3（SEDP + QoS）开始前先做一次**裁剪审计**，剔除不会被引用的部分。

---

## 4 下一步

按优先级：

1. 解决上述三处 → SPDP 发送侧编译通过
2. **验证**：Stratum 发送公告 → 真实 Fast DDS 参与者能否发现它
   （用 Fast DDS 的 `on_participant_discovery` 回调或抓包观察 SEDP 响应）
3. 随机/压力测试（按约定）
4. M3：SEDP 端点发现 + QoS 兼容判定
