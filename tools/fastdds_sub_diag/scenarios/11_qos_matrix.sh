#!/usr/bin/env bash
# A3 验证：QoS 兼容性矩阵（Stratum Reader 的 MatchWriterToReader 是否在真实对端上生效）
set -u

D=/UserData/CodexWorkSpace/Stratum
TOOLS="$D/build/tools"
LOGS="$TOOLS/logs"
DOMAIN=8
export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu
mkdir -p "$LOGS"

printf '%-26s %-24s %-10s %-9s %s\n' "writer(durability/rel)" "reader(requested)" "sedp_pub" "qos_rej" "samples"
printf '%.0s-' $(seq 1 88); echo

run_case() {
    local wdur="$1" wrel="$2" rdur="$3" rrel="$4" tag="$5"
    local rargs=""
    [ -n "$rdur" ] && rargs="$rargs --reader-durability=$rdur"
    [ -n "$rrel" ] && rargs="$rargs --reader-reliability=$rrel"

    timeout 30 "$TOOLS/rtps_reader_diag" 14 "$DOMAIN" $rargs > "$LOGS/a3_${tag}_rd.log" 2>&1 &
    local rd=$!
    sleep 5
    timeout 30 "$TOOLS/fastdds_pub_diag" --domain="$DOMAIN" --count=10 --rate=5 \
        --durability="$wdur" --reliability="$wrel" > "$LOGS/a3_${tag}_pub.log" 2>&1
    wait $rd 2>/dev/null

    local last
    last=$(grep -a "pkt=" "$LOGS/a3_${tag}_rd.log" | tail -1)
    local sedp rej samp
    sedp=$(echo "$last" | grep -o "sedp_pub=[0-9]*" | cut -d= -f2)
    rej=$(echo "$last" | grep -o "qos_rej=[0-9]*" | cut -d= -f2)
    samp=$(echo "$last" | grep -o "samples=[0-9]*" | cut -d= -f2)
    local rdesc="${rdur:-默认}/"
    rdesc="$rdesc${rrel:-默认}"
    printf '%-26s %-24s %-10s %-9s %s\n' "$wdur/$wrel" "$rdesc" "${sedp:-?}" "${rej:-?}" "${samp:-?}"
}

run_case volatile        reliable      ""              ""          c1
run_case transient_local reliable      ""              ""          c2
run_case volatile        reliable      transient_local ""          c3
run_case volatile        best_effort   ""              reliable    c4
run_case transient_local reliable      transient_local reliable    c5
run_case volatile        best_effort   ""              ""          c6

echo
echo "判据：qos_rej>0 且 samples=0 ⇒ 已按规则拒绝；samples>0 ⇒ 已接受并收到数据"
