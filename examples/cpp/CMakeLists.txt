# Stratum C++ 示例
#
# 每个示例都是一个**独立的可执行文件**，展示如何把 Stratum 作为库嵌入自己的程序。
# 它们只依赖 stratum 的核心库，不依赖任何 Fast DDS 头文件。
#
# 用法（在仓库根目录）：
#   cmake -S . -B build -G Ninja -DSTRATUM_BUILD_EXAMPLES=ON
#   cmake --build build
#   ./build/examples/cpp/01_record_and_replay /tmp/my_timeline 0 5
#
# 或者独立构建本目录（需要先把 Stratum 库构建好）：
#   cmake -S examples/cpp -B build-examples -G Ninja \
#         -DSTRATUM_ROOT=<仓库根> -DSTRATUM_LIB_DIR=<仓库根>/build/src

cmake_minimum_required(VERSION 3.16)
project(stratum_examples_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 找到 Stratum 的头文件与静态库
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

# Stratum 的静态库（顺序有意义：被依赖者在前）
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
# 示例清单：每个源文件 = 一个可执行文件
# ---------------------------------------------------------------------------
set(STRATUM_EXAMPLES
    01_record_and_replay
    02_query_timeline
    03_playback_control
    04_custom_backend)

foreach(ex ${STRATUM_EXAMPLES})
  add_executable(${ex} ${ex}.cc)
  target_include_directories(${ex} PRIVATE
      "${STRATUM_INCLUDE_DIR}"
      "${STRATUM_ROOT}/build/generated"
      "${LSM_INC}")

  # 静态库需要 --start-group 才能解决循环依赖
  target_link_libraries(${ex} PRIVATE
      -Wl,--start-group ${STRATUM_LIBS} -Wl,--end-group
      "${LSM_LIB}"
      pthread)

  set_target_properties(${ex} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endforeach()

message(STATUS "Stratum C++ 示例将构建 ${CMAKE_ARRAY_LENGTH} 个可执行文件")
message(STATUS "  头文件: ${STRATUM_INCLUDE_DIR}")
message(STATUS "  库目录: ${STRATUM_LIB_DIR}")
