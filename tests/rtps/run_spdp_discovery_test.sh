#!/usr/bin/env bash
# M2 端到端验收：真实 Fast DDS 能否发现 Stratum 参与者
#
# ===========================================================================
# 这是什么
# ===========================================================================
#
# 单元测试（test_spdp_announcer）只能验证报文结构自洽与字段往返；
# 本脚本提供**独立的外部判据**：
#   1. 启动真实的 Fast DDS 参与者（DomainParticipant + Listener）
#   2. Stratum 周期性发送 SPDP 公告
#   3. 断言 Fast DDS 的 on_participant_discovery 报告发现了 Stratum
#
# 用独立 domain（默认 5）而非 domain 0：
#   domain 0 的 SPDP 端口 7400 常年被 ROS 2 daemon 占据
#   （实测 `ros2-daemon --ros-domain-id 0` 监听 7400），会干扰判定。
#
# 用法：
#   bash tests/rtps/run_spdp_discovery_test.sh [domain] [rounds]
set -u

DOMAIN="${1:-5}"
ROUNDS="${2:-3}"
PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
FD="${FASTDDS_ROOT:-/UserData/Fast-DDS}"

echo "=== 1) 构建 Stratum 发送器 ==="
g++ -std=c++17 -o /tmp/spdp_announce "$PROJ/tools/spdp_announce.cc" \
    "$PROJ/src/rtps/builtin/spdp_announcer.cc" \
    "$PROJ/src/rtps/builtin/spdp_parser.cc" \
    "$PROJ/src/rtps/messages/CDRMessage.cpp" \
    "$PROJ/src/rtps/messages/submessage_parser.cc" \
    "$PROJ/src/rtps/messages/RTPSMessageCreator.cpp" \
    "$PROJ/src/rtps/common/Time_t.cpp" \
    "$PROJ/src/rtps/common/LocatorWithMask.cpp" \
    "$PROJ/src/rtps/common/SerializedPayload.cpp" \
    "$PROJ/src/rtps/utils/IPLocator_minimal.cpp" \
    "$PROJ/src/rtps/network/utils/network.cpp" \
    "$PROJ/src/rtps/fastdds/core/Time_t.cpp" \
    -I"$PROJ/src/include" -I/opt/ros/jazzy/include/fastcdr \
    -L/opt/ros/jazzy/lib -lfastcdr -Wl,-rpath,/opt/ros/jazzy/lib || exit 1

echo "=== 2) 构建 Fast DDS 监听器 ==="
g++ -std=c++17 -o /tmp/xcheck_listener "$PROJ/tools/xcheck_listener.cc" \
    -I"$FD/include" -I"$FD/build-fastdds/include" \
    -I/opt/ros/jazzy/include/fastcdr \
    -L"$FD/build-fastdds/src/cpp" -lfastdds \
    -L/opt/ros/jazzy/lib -lfastcdr \
    -L"$FD/_deps/sysroot/usr/lib/x86_64-linux-gnu" -lfoonathan_memory \
    -Wl,-rpath,"$FD/build-fastdds/src/cpp" \
    -Wl,-rpath,/opt/ros/jazzy/lib \
    -Wl,-rpath,"$FD/_deps/sysroot/usr/lib/x86_64-linux-gnu" || exit 1

echo "=== 3) 连续 $ROUNDS 轮验收（domain $DOMAIN）==="
PASS=0
for i in $(seq 1 "$ROUNDS"); do
    LOG="/tmp/spdp_discovery_round${i}.log"
    ( /tmp/xcheck_listener 12 "$DOMAIN" > "$LOG" 2>&1 & )
    sleep 3
    /tmp/spdp_announce 4 "$DOMAIN" > /dev/null 2>&1
    sleep 6

    if grep -qa "DISCOVERED" "$LOG" && grep -qa "stratum" "$LOG"; then
        echo "  第 $i 轮: PASS"
        grep -a "DISCOVERED" "$LOG" | head -2 | sed 's/^/      /'
        PASS=$((PASS + 1))
    else
        echo "  第 $i 轮: FAIL"
        echo "      listener 输出："
        grep -aE "DISCOVERED|结果" "$LOG" | sed 's/^/      /' | head -5
    fi
done

echo
if [ "$PASS" -eq "$ROUNDS" ]; then
    echo "=== M2 端到端验收 PASS（$PASS/$ROUNDS）==="
    exit 0
fi
echo "=== M2 端到端验收 FAIL（$PASS/$ROUNDS）==="
exit 1
