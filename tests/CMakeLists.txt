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
