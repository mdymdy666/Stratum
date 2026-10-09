#!/usr/bin/env bash
# 交叉验证：真实 Fast DDS 流量 → Stratum 移植解码器
#
# ===========================================================================
# 这是什么
# ===========================================================================
#
# round-trip 测试只能证明编解码"自洽"（对称地错也会通过）。
# 本脚本提供**独立**的第二道验证：
#   1. 用真实 Fast DDS（libfastdds.so）创建 DomainParticipant
#      → 它发出真实的 SPDP 组播公告
#   2. Stratum 抓包并用**移植的**解码器解析
#   3. 断言：魔数/版本/vendorId/子消息结构
#
# 用法：bash tests/rtps/run_cross_validation.sh
set -u

PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
FD="${FASTDDS_ROOT:-/UserData/Fast-DDS}"
OUT=/tmp/stratum_spdp_capture.bin

echo "=== 1) 构建 Fast DDS 生产者（链接真实 libfastdds.so）==="
g++ -std=c++17 -o /tmp/xcheck_producer "$PROJ/tools/xcheck_producer.cc" \
    -I"$FD/include" -I"$FD/build-fastdds/include" \
    -I/opt/ros/jazzy/include/fastcdr \
    -L"$FD/build-fastdds/src/cpp" -lfastdds \
    -L/opt/ros/jazzy/lib -lfastcdr \
    -L"$FD/_deps/sysroot/usr/lib/x86_64-linux-gnu" -lfoonathan_memory \
    -Wl,-rpath,"$FD/build-fastdds/src/cpp" \
    -Wl,-rpath,/opt/ros/jazzy/lib \
    -Wl,-rpath,"$FD/_deps/sysroot/usr/lib/x86_64-linux-gnu" || exit 1

echo "=== 2) 构建 Stratum 抓包器（链接移植的解码器）==="
g++ -std=c++17 -o /tmp/xcheck_sniff "$PROJ/tools/xcheck_sniff.cc" \
    "$PROJ/src/rtps/messages/CDRMessage.cpp" \
    "$PROJ/src/rtps/common/Time_t.cpp" \
    "$PROJ/src/rtps/common/LocatorWithMask.cpp" \
    "$PROJ/src/rtps/utils/IPLocator_minimal.cpp" \
    "$PROJ/src/rtps/network/utils/network.cpp" \
    "$PROJ/src/rtps/fastdds/core/Time_t.cpp" \
    -I"$PROJ/src/include" -I/opt/ros/jazzy/include/fastcdr \
    -L/opt/ros/jazzy/lib -lfastcdr -Wl,-rpath,/opt/ros/jazzy/lib || exit 1

echo "=== 3) 抓包 + 发 SPDP ==="
SUDO=""
if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then SUDO="sudo"; fi
( $SUDO /tmp/xcheck_sniff 7 "$OUT" > /tmp/sniff.log 2>&1 & )
sleep 2
timeout 15 /tmp/xcheck_producer > /tmp/producer.log 2>&1 || true
sleep 6

cat /tmp/sniff.log

echo "=== 4) 归档测试向量 ==="
mkdir -p "$PROJ/tests/vectors"
if [ -f "$OUT" ]; then
    cp "$OUT" "$PROJ/tests/vectors/spdp_capture.bin"
    echo "已归档: tests/vectors/spdp_capture.bin ($(stat -c%s "$OUT") 字节)"
fi

grep -q "失败 0" /tmp/sniff.log && echo "=== 交叉验证 PASS ===" && exit 0
echo "=== 交叉验证 FAIL ===" && exit 1
