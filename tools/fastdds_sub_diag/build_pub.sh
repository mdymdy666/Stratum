#!/usr/bin/env bash
# 编译 QoS 可配置的 Fast DDS 发布端（fastdds_pub_diag）
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FD="${FASTDDS_ROOT:-/UserData/Fast-DDS}"
OUT="${1:-/UserData/CodexWorkSpace/Stratum/build/tools/fastdds_pub_diag}"
mkdir -p "$(dirname "$OUT")"
g++ -std=c++17 -O2 -g -Wall -Wextra \
    -o "$OUT" \
    "$DIR/fastdds_pub_diag.cc" \
    "$DIR/types/HelloWorldPubSubTypes.cxx" \
    "$DIR/types/HelloWorldTypeObjectSupport.cxx" \
    -I"$DIR/types" -I"$FD/include" -I"$FD/build-fastdds/include" \
    -I/opt/ros/jazzy/include/fastcdr \
    -L"$FD/build-fastdds/src/cpp" -lfastdds \
    -L/opt/ros/jazzy/lib -lfastcdr \
    -L"$FD/_deps/sysroot/usr/lib/x86_64-linux-gnu" -lfoonathan_memory \
    -Wl,-rpath,"$FD/build-fastdds/src/cpp" \
    -Wl,-rpath,/opt/ros/jazzy/lib \
    -Wl,-rpath,"$FD/_deps/sysroot/usr/lib/x86_64-linux-gnu"
echo "built: $OUT"
