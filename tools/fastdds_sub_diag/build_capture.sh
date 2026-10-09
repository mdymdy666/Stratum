#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 文件：tools/fastdds_sub_diag/build_capture.sh
# 用途：编译 capture_udp（原始 UDP 抓包器，复用 Stratum 的 SpdpAnnouncer）
#
# 依赖 Stratum 自身已构建的静态库 build/src/libstratum_rtps_base.a
# （SpdpAnnouncer / CDRMessage 均在其中）。若尚未构建，先：
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build
#
# 用法：bash tools/fastdds_sub_diag/build_capture.sh [输出路径]
# ---------------------------------------------------------------------------
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
OUT="${1:-/UserData/CodexWorkSpace/Stratum/build/tools/capture_udp}"
LIB="$ROOT/build/src/libstratum_rtps_base.a"

if [ ! -f "$LIB" ]; then
    echo "缺少 $LIB（先在项目根构建 Stratum）" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
g++ -std=c++17 -O2 -g -Wall -Wextra \
    -o "$OUT" \
    "$DIR/capture_udp.cc" \
    -I"$ROOT/src/include" \
    -I/opt/ros/jazzy/include/fastcdr \
    "$LIB" \
    -L/opt/ros/jazzy/lib -lfastcdr \
    -lpthread \
    -Wl,-rpath,/opt/ros/jazzy/lib

echo "built: $OUT"
