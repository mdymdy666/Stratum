# Copyright 2026 Stratum Project
# SPDX-License-Identifier: Apache-2.0
#
# 测试构建（随开发逐步扩充）

add_library(stratum_test_framework INTERFACE)
target_include_directories(stratum_test_framework INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(stratum_test_framework INTERFACE stratum_core)
target_include_directories(stratum_test_framework INTERFACE
    "/opt/ros/jazzy/include/fastcdr")

function(stratum_add_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE
      stratum_all
      stratum_test_framework)
  add_test(NAME ${name} COMMAND ${name}
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(${name} PROPERTIES TIMEOUT 900 LABELS "${ARGN}")
endfunction()

stratum_add_test(test_storage_contract unit/test_storage_contract.cc unit storage)
stratum_add_test(test_time_key unit/test_time_key.cc unit storage)
stratum_add_test(test_ack_bitmap unit/test_ack_bitmap.cc unit storage)   # O10：ACKNACK 位图判定

stratum_add_test(test_timeline_basic integration/test_timeline_basic.cc integration)

stratum_add_test(test_record_replay integration/test_record_replay.cc integration)

stratum_add_test(test_random_ops random/test_random_ops.cc random)

stratum_add_test(test_stress_throughput stress/test_stress_throughput.cc stress)

stratum_add_test(test_real_sensor_data real/test_real_sensor_data.cc real)

stratum_add_test(test_review_findings review/test_review_findings.cc review)

stratum_add_test(test_recorder_queue review/test_recorder_queue.cc review)

stratum_add_test(test_wal_corruption review/test_wal_corruption.cc review)

stratum_add_test(test_recorder_concurrency review/test_recorder_concurrency.cc review)

stratum_add_test(test_lsm_v2_basic review/test_lsm_v2_basic.cc review lsm_v2)

stratum_add_test(test_lsm_v2_regression review/test_lsm_v2_regression.cc review)

stratum_add_test(test_lsm_v2_snapshot review/test_lsm_v2_snapshot.cc review)

stratum_add_test(test_lsm_v2_random random/test_lsm_v2_random.cc random)
stratum_add_test(test_lsm_v2_real real/test_lsm_v2_real.cc real)

stratum_add_test(test_timeline_v2 integration/test_timeline_v2.cc integration)

stratum_add_test(test_record_replay_v2 integration/test_record_replay_v2.cc integration)

stratum_add_test(test_quality_feedback review/test_quality_feedback.cc review)

stratum_add_test(test_retention_point_persist review/test_retention_point_persist.cc review)

stratum_add_test(test_c3_concurrent_iterate review/test_c3_concurrent_iterate.cc review)

# 回归：话题 type_name 跨"关闭-重开"存活（曾丢失 → SEDP Publication 静默不发）
stratum_add_test(test_topic_meta_type_name review/test_topic_meta_type_name.cc review)

# 回归：后端配置（domain）必须从 RecorderConfig/PlayerConfig 传到 DDS 后端
# （曾静默丢弃 → 回放永远落在 domain 0，互操作在非 0 域上完全不通）
stratum_add_test(test_backend_config_propagation review/test_backend_config_propagation.cc review)

stratum_add_test(test_rtps_types rtps/test_rtps_types.cc rtps)

stratum_add_test(test_cdr_message rtps/test_cdr_message.cc rtps)

stratum_add_test(test_submessage_parser rtps/test_submessage_parser.cc rtps)

stratum_add_test(test_spdp_parser rtps/test_spdp_parser.cc rtps)

# ---------------------------------------------------------------------------
# 测试数据向量：复制到构建目录
# ---------------------------------------------------------------------------
# 测试从 build/ 目录运行，相对路径 "tests/vectors/..." 会解析到
# build/tests/vectors/。若不复制，真实流量用例会因找不到文件而失败
# （早期版本曾静默跳过 —— 那是假通过，已修正为硬失败）。
file(GLOB STRATUM_TEST_VECTORS
    "${CMAKE_CURRENT_SOURCE_DIR}/vectors/*.bin")
if(STRATUM_TEST_VECTORS)
    file(COPY ${STRATUM_TEST_VECTORS}
         DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/vectors")
    message(STATUS "Stratum: 已复制 ${CMAKE_CURRENT_BINARY_DIR}/vectors 下的测试向量")
else()
    message(WARNING "Stratum: tests/vectors/ 为空 —— 真实流量测试将失败")
endif()

stratum_add_test(test_spdp_announcer rtps/test_spdp_announcer.cc rtps)

stratum_add_test(test_sedp_parser rtps/test_sedp_parser.cc rtps)

stratum_add_test(test_qos_matcher rtps/test_qos_matcher.cc rtps)

stratum_add_test(test_rtps_packet_utils rtps/test_rtps_packet_utils.cc rtps)

stratum_add_test(test_submsg_walk rtps/test_submsg_walk.cc rtps)

# 回归：SEDP Publication 构造失败必须显式报错（曾静默跳过 → 对端收不到数据）
stratum_add_test(test_publication_guard rtps/test_publication_guard.cc rtps)

# types 模块：DynamicValue / TypeDescriptor / ISerializer 契约
stratum_add_test(test_types_serializer types/test_types_serializer.cc types)

# protocol 模块：IDdsBackend / ISource / ISink 契约
stratum_add_test(test_protocol_backend protocol/test_protocol_backend.cc protocol)

# rtps 使用侧：报文构造/分片/配置契约（区别于 tests/rtps/ 的移植代码测试）
stratum_add_test(test_rtps_backend_usage rtps/test_rtps_backend_usage.cc rtps)

# 移植模块：日志（logging）+ 配置（config）
# 来源 /UserData/CodexWorkSpace/Go2Cpp，见 docs/10-porting-notes.md
stratum_add_test(test_log_config unit/test_log_config.cc unit config)

# 会话元数据的持久化与重载（曾因 Open 缺 LoadSession 导致 verify 假阳性）
stratum_add_test(test_session_metadata unit/test_session_metadata.cc unit storage)

# DeleteRange：CLI prune 的地基（删除是唯一不可逆操作，必须有回归）
stratum_add_test(test_delete_range unit/test_delete_range.cc unit storage)

# DynamicValue 工厂与自描述输出（把 KnownGap 变成已覆盖能力）
stratum_add_test(test_dynamic_value types/test_dynamic_value.cc types)

# JSON 序列化器 / 工厂 / 降级模式（含两处已知有损的断言）
stratum_add_test(test_json_serializer types/test_json_serializer.cc types)

# 已裁剪区间的读取与语义（补上"裁剪不是缺口"这条契约的自动化覆盖）
stratum_add_test(test_pruned_ranges unit/test_pruned_ranges.cc unit storage)

# 并发注册/追加/查询压力测试（排查 record 段的 core dump）
stratum_add_test(test_concurrent_register unit/test_concurrent_register.cc unit storage)

# 话题类型定义的存取（核心用例须 ≥2 话题：单话题曾掩盖 return 落进循环体的 bug）
stratum_add_test(test_topic_type_definition unit/test_topic_type_definition.cc unit storage)

# 控制事件的存取（校准：逗号/换行必须被转义，否则会破坏行格式）
stratum_add_test(test_control_events unit/test_control_events.cc unit storage)

# CLI 命令 io / doctor 的集成测试（通过 popen 调用真实二进制，
# 因为这两个命令的逻辑在 CLI 层，单元测试链接不到；
# 项目现有的 shell 场景脚本又不进 ctest，故用 C++ 包装以保证进质量门）
stratum_add_test(test_cli_io_doctor integration/test_cli_io_doctor.cc integration cli)

# 埋点配置：自动生成 / 热重载 / 双语义（全局 fail-safe vs 其余 fail-preserve）/ .lock
stratum_add_test(test_probe_config unit/test_probe_config.cc unit probe)

# 埋点库：环满覆盖、并发写、SCOPE(含异常路径)、PR-04 反汇编零开销
stratum_add_test(test_probe unit/test_probe.cc unit probe)
target_compile_definitions(test_probe PRIVATE
    PROJECT_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/src/include")

# 埋点端到端：业务(只含 probe.h) → ring → stratum probe → DDS → record → query
stratum_add_test(test_cli_probe integration/test_cli_probe.cc integration probe cli)
target_compile_definitions(test_cli_probe PRIVATE
    PROBE_CLI_PATH="${CMAKE_BINARY_DIR}/stratum"
    PROBE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# 这个用例会起真实进程并用 DDS 通信；与其余测试并行时抢 CPU 会让
# 「等待 N 秒」这类时序不可靠。独占跑，结果才有意义。
set_tests_properties(test_cli_probe PROPERTIES RUN_SERIAL TRUE)

# ring 的边界行为：最小容量 / 序号回绕 / ring 复用 / 长度恰好到上限
stratum_add_test(test_probe_boundary unit/test_probe_boundary.cc unit probe)

# probe 子命令的参数与错误处理（只测解析与退出码，不起重资源）
stratum_add_test(test_probe_cli_errors integration/test_probe_cli_errors.cc integration cli)
target_compile_definitions(test_probe_cli_errors PRIVATE
    PROBE_CLI_PATH="${CMAKE_BINARY_DIR}/stratum")

# 多实例：不同容量/交叉写/per-ring 开关/共享与独立配置/单环多线程
stratum_add_test(test_probe_instance unit/test_probe_instance.cc unit probe)

# 热重载时序：连续改/中间态恢复/锁暂停与恢复/删除保留现状/计数累计
stratum_add_test(test_probe_hot_reload unit/test_probe_hot_reload.cc unit probe)

# status 边界：空 ring/全未配置/声明未见/scan-limit/环覆盖/两种格式
stratum_add_test(test_probe_status_edge integration/test_probe_status_edge.cc integration cli)
target_compile_definitions(test_probe_status_edge PRIVATE
    PROBE_CLI_PATH="${CMAKE_BINARY_DIR}/stratum")

# soak：持续写入 20 秒（计数精确/内存不涨）+ 绕 10 圈覆盖后计数仍精确
stratum_add_test(test_probe_soak unit/test_probe_soak.cc unit probe)
# 这条本身要跑 20 秒以上，给足超时；与其它测试并行会拉长时序，独占跑
set_tests_properties(test_probe_soak PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

# verify 的业务层视角：分层 / 启发式信号 / ★不污染完整性判定 / RC-B1 / 前缀可配
stratum_add_test(test_verify_business integration/test_verify_business.cc integration cli)
target_compile_definitions(test_verify_business PRIVATE
    PROBE_CLI_PATH="${CMAKE_BINARY_DIR}/stratum"
    PROBE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_tests_properties(test_verify_business PROPERTIES TIMEOUT 400 RUN_SERIAL TRUE)

# stratum diff：相对对齐 / 分桶检出尾部丢失 / payload 三档 / 业务层 / 空会话 / 规模 / 随机 / 错误
stratum_add_test(test_diff integration/test_diff.cc integration cli)
target_compile_definitions(test_diff PRIVATE
    DIFF_CLI_PATH="${CMAKE_BINARY_DIR}/stratum")
set_tests_properties(test_diff PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

# 跨命令不变量：verify 与 diff 对同一份数据的结论必须自洽
# （diff.events == verify.events、探针分类一致、无差异⇒结论一致）
stratum_add_test(test_xcheck_verify_diff integration/test_xcheck_verify_diff.cc integration cli)
target_compile_definitions(test_xcheck_verify_diff PRIVATE
    XCHECK_CLI_PATH="${CMAKE_BINARY_DIR}/stratum")
set_tests_properties(test_xcheck_verify_diff PROPERTIES TIMEOUT 200 RUN_SERIAL TRUE)

# diff 的对称性与幂等性：自反/连比一致/方向互换后差异集合相同
stratum_add_test(test_diff_symmetry integration/test_diff_symmetry.cc integration cli)
target_compile_definitions(test_diff_symmetry PRIVATE
    SYM_CLI_PATH="${CMAKE_BINARY_DIR}/stratum")
set_tests_properties(test_diff_symmetry PROPERTIES TIMEOUT 200 RUN_SERIAL TRUE)

# stdout 纯净性：stdout 是数据出口，日志/警告/错误只能在 stderr
stratum_add_test(test_stdout_purity integration/test_stdout_purity.cc integration cli)
target_compile_definitions(test_stdout_purity PRIVATE
    PURITY_CLI_PATH="${CMAKE_BINARY_DIR}/stratum")
set_tests_properties(test_stdout_purity PROPERTIES TIMEOUT 200 RUN_SERIAL TRUE)

# diff 的算术边界：span=0 / 空左侧 / 单条 / bucket=1·1000 / 极端 payload / 双空
stratum_add_test(test_diff_edge integration/test_diff_edge.cc integration cli)
target_compile_definitions(test_diff_edge PRIVATE
    EDGE_CLI_PATH="${CMAKE_BINARY_DIR}/stratum")
set_tests_properties(test_diff_edge PROPERTIES TIMEOUT 200 RUN_SERIAL TRUE)

# metrics 的业务层指标：分层/与 verify 对账/两种输出一致/前缀可配/零值仍输出
stratum_add_test(test_metrics_business integration/test_metrics_business.cc integration cli)
target_compile_definitions(test_metrics_business PRIVATE
    MB_CLI_PATH="${CMAKE_BINARY_DIR}/stratum")
set_tests_properties(test_metrics_business PROPERTIES TIMEOUT 200 RUN_SERIAL TRUE)
