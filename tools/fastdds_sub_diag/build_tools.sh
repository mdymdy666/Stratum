#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 文件：tools/fastdds_sub_diag/build_tools.sh
# 用途：重新链接 tools/ 下的协议诊断工具（它们未纳入 Stratum 的 CMake）
#
# 为什么必须重建：这些工具链接 build/src/*.a 的静态库。src/ 改动后若继续用
# /tmp 里的旧二进制做验证，结论会基于旧代码（本项目已吃过"验证工具与源码
# 不一致"的亏）。任何 src/ 改动后都应先跑本脚本。
#
# 用法：bash tools/fastdds_sub_diag/build_tools.sh
# 产物：/UserData/CodexWorkSpace/Stratum/build/tools/stratum_rtps_replay、/UserData/CodexWorkSpace/Stratum/build/tools/rtps_sink_diag2、/UserData/CodexWorkSpace/Stratum/build/tools/rtps_reader_diag
# ---------------------------------------------------------------------------
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
LSM="/UserData/LeaningLevelDB/LSMengine/build-lib/libLSMengine.a"
LSM_INC="/UserData/LeaningLevelDB/LSMengine/include"

build_one() {
    local src="$1"
    local out="$2"
    mkdir -p "$(dirname "$out")"
    g++ -std=c++17 -O2 -g \
        -o "$out" \
        "$src" \
        -I"$ROOT/src/include" \
        -I"$ROOT/build/generated" \
        -I"$LSM_INC" \
        -I/opt/ros/jazzy/include/fastcdr \
        -Wl,--start-group \
        "$ROOT/build/src/libstratum_rtps_protocol.a" \
        "$ROOT/build/src/libstratum_rtps_base.a" \
        "$ROOT/build/src/libstratum_recorder.a" \
        "$ROOT/build/src/libstratum_timeline.a" \
        "$ROOT/build/src/libstratum_lsm_v2.a" \
        "$ROOT/build/src/libstratum_storage_lsm.a" \
        "$ROOT/build/src/libstratum_storage_memory.a" \
        "$ROOT/build/src/libstratum_core.a" \
        -Wl,--end-group \
        "$LSM" \
        -L/opt/ros/jazzy/lib -lfastcdr \
        -lpthread \
        -Wl,-rpath,/opt/ros/jazzy/lib
    echo "built: $out"
}

build_one "$ROOT/tools/fastdds_sub_diag/make_timeline.cc" /UserData/CodexWorkSpace/Stratum/build/tools/make_timeline
build_one "$ROOT/tools/stratum_rtps_replay.cc" /UserData/CodexWorkSpace/Stratum/build/tools/stratum_rtps_replay
build_one "$ROOT/tools/stratum_rtps_record.cc" /UserData/CodexWorkSpace/Stratum/build/tools/stratum_rtps_record
build_one "$ROOT/tools/rtps_sink_diag2.cc" /UserData/CodexWorkSpace/Stratum/build/tools/rtps_sink_diag2
build_one "$ROOT/tools/rtps_reader_diag.cc" /UserData/CodexWorkSpace/Stratum/build/tools/rtps_reader_diag
