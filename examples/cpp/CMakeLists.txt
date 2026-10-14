# Stratum C++ 示例
#
# 五个模块，各两个文件：
#   <模块>_demo.cpp   简单用法（≥100 行），逐 API 注释用途与坑
#   <模块>_full.cpp   全面用法（≥300 行），演示 ≥3 个真实场景
#
# 这些示例只依赖 Stratum 的核心库，**不需要 Fast DDS 头文件**。
#
# 用法（在仓库根目录）：
#   cmake -S . -B build -G Ninja && cmake --build build     # 先构建 Stratum 库
#   cmake -S examples/cpp -B build-examples -G Ninja
#   cmake --build build-examples
#   ./build-examples/rtps_demo
#
# 也可以指定库位置：
#   cmake -S examples/cpp -B build-examples -G Ninja \
#         -DSTRATUM_ROOT=<仓库根> -DSTRATUM_LIB_DIR=<仓库根>/build/src

cmake_minimum_required(VERSION 3.16)
project(stratum_examples_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# 定位 Stratum 的头文件与静态库
# ---------------------------------------------------------------------------
if(NOT DEFINED STRATUM_ROOT)
  set(STRATUM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
endif()
if(NOT DEFINED STRATUM_LIB_DIR)
  set(STRATUM_LIB_DIR "${STRATUM_ROOT}/build/src")
endif()

set(STRATUM_INCLUDE_DIR "${STRATUM_ROOT}/src/include")
set(LSM_INC "/UserData/LeaningLevelDB/LSMengine/include")
set(LSM_LIB "/UserData/LeaningLevelDB/LSMengine/build-lib/libLSMengine.a")

if(NOT EXISTS "${STRATUM_INCLUDE_DIR}/stratum/core/recorder.h")
  message(FATAL_ERROR "找不到 Stratum 头文件：${STRATUM_INCLUDE_DIR}")
endif()

# 这些库之间存在循环依赖，链接时必须用 --start-group/--end-group
set(STRATUM_LIBS
    "${STRATUM_LIB_DIR}/libstratum_rtps_protocol.a"
    "${STRATUM_LIB_DIR}/libstratum_rtps_base.a"
    "${STRATUM_LIB_DIR}/libstratum_recorder.a"
    "${STRATUM_LIB_DIR}/libstratum_timeline.a"
    "${STRATUM_LIB_DIR}/libstratum_lsm_v2.a"
    "${STRATUM_LIB_DIR}/libstratum_storage_lsm.a"
    "${STRATUM_LIB_DIR}/libstratum_storage_memory.a"
    "${STRATUM_LIB_DIR}/libstratum_core.a")

# ---------------------------------------------------------------------------
# 示例清单：五个模块 × (demo, full)
# ---------------------------------------------------------------------------
set(STRATUM_EXAMPLES
    core_demo
    core_full
    protocol_demo
    protocol_full
    storage_demo
    storage_full
    types_demo
    types_full
    rtps_demo
    rtps_full)

foreach(ex ${STRATUM_EXAMPLES})
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${ex}.cpp")
    message(FATAL_ERROR "缺少源文件：${ex}.cpp")
  endif()

  add_executable(${ex} "${ex}.cpp")

  # examples/cpp 本身也要加进 include 路径，这样 common/*.h 能被找到
  target_include_directories(${ex} PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}"
      "${STRATUM_INCLUDE_DIR}"
      "${STRATUM_ROOT}/build/generated"
      "${LSM_INC}")

  target_link_libraries(${ex} PRIVATE
      -Wl,--start-group ${STRATUM_LIBS} -Wl,--end-group
      "${LSM_LIB}"
      pthread)

  set_target_properties(${ex} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endforeach()

list(LENGTH STRATUM_EXAMPLES _n)
message(STATUS "Stratum 示例：将构建 ${_n} 个可执行文件")
message(STATUS "  头文件目录: ${STRATUM_INCLUDE_DIR}")
message(STATUS "  静态库目录: ${STRATUM_LIB_DIR}")
