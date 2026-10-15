# Copyright 2026 Stratum Project
# SPDX-License-Identifier: Apache-2.0
#
# 构建：暂只包含已实现的模块，其余随开发逐步加入。
# 这样做的好处：任何时刻仓库都处于"可编译、可测试"状态。

set(STRATUM_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated)

add_library(stratum_options INTERFACE)
target_include_directories(stratum_options INTERFACE ${STRATUM_INCLUDE_DIRS})
target_compile_features(stratum_options INTERFACE cxx_std_17)

# ---------------------------------------------------------------------------
# 核心库
# ---------------------------------------------------------------------------
add_library(stratum_core STATIC
    src/core/status.cc
    src/storage/time_key.cc
    src/storage/meta_codec.cc
)
target_link_libraries(stratum_core PUBLIC stratum_options)
target_include_directories(stratum_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# 插件：内存存储引擎
# ---------------------------------------------------------------------------
add_library(stratum_storage_memory STATIC
    src/storage/memory/memory_engine.cc
)
target_link_libraries(stratum_storage_memory PUBLIC stratum_core)

# ---------------------------------------------------------------------------
# Timeline 层：把事件语义映射到 KV（唯一知道 TimeKey 编码的地方）
# ---------------------------------------------------------------------------
add_library(stratum_timeline STATIC
    src/core/timeline_impl.cc
)
target_link_libraries(stratum_timeline PUBLIC stratum_core)


# ---------------------------------------------------------------------------
# 插件：自研 LSM 存储引擎（时间轴优化）
# ---------------------------------------------------------------------------
add_library(stratum_storage_lsm STATIC
    src/storage/lsm/memtable.cc
    src/storage/lsm/lsm_engine.cc
)
target_link_libraries(stratum_storage_lsm PUBLIC stratum_core)


# ---------------------------------------------------------------------------
# 录制 / 回放 / 查询 / 质量报告
# ---------------------------------------------------------------------------
add_library(stratum_recorder STATIC
    src/core/recorder.cc
    src/core/player.cc
    src/core/query.cc
)
target_link_libraries(stratum_recorder PUBLIC stratum_timeline)


# ---------------------------------------------------------------------------
# v2 DB 层：基于 LSMengine（LevelDB 架构）的时间轴 DB
# ---------------------------------------------------------------------------
# RTPS 基础类型（M1 移植）
add_library(stratum_rtps_base STATIC
    rtps/common/Time_t.cpp
    rtps/messages/CDRMessage.cpp
    rtps/builtin/spdp_parser.cc
    rtps/builtin/sedp_parser.cc
    rtps/builtin/qos_matcher.cc
    rtps/builtin/spdp_announcer.cc
    rtps/messages/submessage_parser.cc
    rtps/messages/RTPSMessageCreator.cpp
    rtps/common/LocatorWithMask.cpp
    rtps/common/SerializedPayload.cpp
    rtps/utils/IPLocator_minimal.cpp
    rtps/network/utils/network.cpp
    rtps/fastdds/core/Time_t.cpp
)
target_include_directories(stratum_rtps_base PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    "/opt/ros/jazzy/include/fastcdr"
)

add_library(stratum_lsm_v2 STATIC
    src/storage/lsm_v2/time_key_comparator.cc
    src/storage/lsm_v2/stratum_db.cc
    src/core/timeline_impl_v2.cc
)
target_include_directories(stratum_lsm_v2 PUBLIC
    "/UserData/LeaningLevelDB/LSMengine/include"
)
target_link_libraries(stratum_lsm_v2 PUBLIC stratum_core)
# LSMengine 以 -fno-rtti -fno-exceptions 编译，本层必须一致
target_compile_options(stratum_lsm_v2 PRIVATE -fno-rtti -fno-exceptions)
target_link_libraries(stratum_lsm_v2 PUBLIC "/UserData/LeaningLevelDB/LSMengine/build-lib/libLSMengine.a")

# ---------------------------------------------------------------------------
# 汇总库（随插件增加而扩展）
# ---------------------------------------------------------------------------
add_library(stratum_all INTERFACE)
target_link_libraries(stratum_all INTERFACE
    stratum_core
    stratum_storage_memory
    stratum_storage_lsm
    stratum_timeline
    stratum_recorder
    stratum_lsm_v2
    stratum_rtps_base
    stratum_rtps_protocol
)

# RTPS 协议引擎（自研订阅端实现，依赖已移植的 rtps 基础层）
add_library(stratum_rtps_protocol STATIC
    rtps/protocol/rtps_packet_utils.cc
    rtps/protocol/rtps_reader.cc
    rtps/protocol/rtps_writer.cc
    rtps/protocol/rtps_backend.cc
)
target_include_directories(stratum_rtps_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    "/opt/ros/jazzy/include/fastcdr"
)
target_link_libraries(stratum_rtps_protocol PUBLIC
    stratum_rtps_base
    stratum_core
)

# ---------------------------------------------------------------------------
# stratum CLI —— 交付入口
#
# 设计依据：docs/07-cli-design.md（做哪些命令、32 场景）
#           docs/08-cli-spec.md（每条命令的精确参数与语义）
#           docs/09-cli-scenarios-deep.md（边界与失败行为）
#
# 本版本交付 session / verify / config 三组；
# 其余子命令在 main.cc 中明确报"未实现"（不静默忽略）。
# ---------------------------------------------------------------------------
add_executable(stratum
    cli/main.cc
    cli/cli_common.cc
    cli/cmd_session.cc
    cli/cmd_record.cc
    cli/cmd_query.cc
    cli/cmd_verify.cc
    cli/cmd_config.cc
)
target_link_libraries(stratum PRIVATE stratum_all)
set_target_properties(stratum PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ---------------------------------------------------------------------------
# 日志模块（移植自 /UserData/CodexWorkSpace/Go2Cpp，见 docs/10-porting-notes.md）
#
# 【为什么移植】原项目已有成熟实现：LogFilter -> LogItemWorker -> LogFormatter
# -> LogSink 四层可替换，支持轮转文件、级别过滤、模式化格式，且**零第三方依赖**。
# 自己重写一遍日志是纯粹的浪费。
#
# 移植改动：命名空间 go2cpp::log -> stratum::log；宏 GO2CPP_LOG_* -> STRATUM_LOG_*。
# ---------------------------------------------------------------------------
add_library(stratum_logging STATIC
    logging/log.cpp
)
target_include_directories(stratum_logging PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(stratum_logging PUBLIC pthread)

# ---------------------------------------------------------------------------
# 配置模块（同上移植）
#
# 保留：IniFile（INI 解析）、ValueCodec（类型转换）、ConfigVarBase / ConfigVar
#       （可监听变量）、Config（两阶段事务 + 热更新 + 校验器/监听器）
# 裁剪：RuntimeConfig 与 BindRuntimeConfig —— 它们绑定 Go2Cpp 的调度器，
#       而 Stratum 没有调度器。裁掉后本模块零外部依赖。
# ---------------------------------------------------------------------------
add_library(stratum_config STATIC
    config/config.cpp
)
target_include_directories(stratum_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(stratum_config PUBLIC
    stratum_logging
    pthread
)

# 并入汇总库，使 CLI 与测试可直接使用
target_link_libraries(stratum_all INTERFACE
    stratum_logging
    stratum_config
)
