#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 文件：tools/fastdds_sub_diag/build.sh
# 用途：编译 Fast DDS 订阅诊断程序（fastdds_sub_diag）
#
# 为什么单独一个脚本 / 不并入 Stratum 的 CMake：
#   Stratum 主项目坚持"零外部依赖（核心库只用标准库）"，而这个诊断程序
#   必须链接真实 libfastdds.so。把它塞进主 CMake 会把外部依赖带进正常构建，
#   因此按项目已有的做法（tests/rtps/run_cross_validation.sh 也是这么编译
#   真实 Fast DDS 程序的）单独编译。
#
# 用法：bash tools/fastdds_sub_diag/build.sh [输出路径，默认 /UserData/CodexWorkSpace/Stratum/build/tools/fastdds_sub_diag]
# 环境：FASTDDS_ROOT 可覆盖上游 Fast DDS 根目录（默认 /UserData/Fast-DDS）
# ---------------------------------------------------------------------------
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FD="${FASTDDS_ROOT:-/UserData/Fast-DDS}"
OUT="${1:-/UserData/CodexWorkSpace/Stratum/build/tools/fastdds_sub_diag}"

if [ ! -d "$FD/build-fastdds/src/cpp" ]; then
    echo "找不到 $FD/build-fastdds/src/cpp（libfastdds 构建产物）" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
g++ -std=c++17 -O2 -g -Wall -Wextra \
    -o "$OUT" \
    "$DIR/fastdds_sub_diag.cc" \
    "$DIR/types/HelloWorldPubSubTypes.cxx" \
    "$DIR/types/HelloWorldTypeObjectSupport.cxx" \
    -I"$DIR/types" \
    -I"$FD/include" \
    -I"$FD/build-fastdds/include" \
    -I/opt/ros/jazzy/include/fastcdr \
    -L"$FD/build-fastdds/src/cpp" -lfastdds \
    -L/opt/ros/jazzy/lib -lfastcdr \
    -L"$FD/_deps/sysroot/usr/lib/x86_64-linux-gnu" -lfoonathan_memory \
    -Wl,-rpath,"$FD/build-fastdds/src/cpp" \
    -Wl,-rpath,/opt/ros/jazzy/lib \
    -Wl,-rpath,"$FD/_deps/sysroot/usr/lib/x86_64-linux-gnu"

echo "built: $OUT"
