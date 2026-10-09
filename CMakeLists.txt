# Copyright 2026 Stratum Project
# SPDX-License-Identifier: Apache-2.0
#
# Stratum 顶层构建
#
# 设计原则：
#   1. 零外部依赖（核心库只用标准库）—— 便于嵌入式/边缘部署
#   2. 插件式：每类插件是独立的 OBJECT/STATIC 库，可按需裁剪
#   3. 默认开启测试；测试可独立关闭以便快速构建产物

cmake_minimum_required(VERSION 3.16)

project(Stratum
        VERSION 0.1.0
        DESCRIPTION "面向 DDS 的可回溯数据平面"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 严格警告：本项目的代码质量要求较高，默认开启
if(NOT MSVC)
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# 选项
# ---------------------------------------------------------------------------
option(STRATUM_BUILD_TESTS "构建测试" ON)
option(STRATUM_BUILD_CLI "构建命令行工具" ON)
option(STRATUM_BUILD_BENCHMARKS "构建性能基准" ON)
option(STRATUM_ENABLE_SANITIZERS "启用 ASan/UBSan（仅调试用）" OFF)

if(STRATUM_ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# 版本信息（供代码内使用）
# ---------------------------------------------------------------------------
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stratum_version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/stratum/version.h
  @ONLY)

# ---------------------------------------------------------------------------
# 子目录
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(STRATUM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(STRATUM_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# 构建摘要
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "========== Stratum ${PROJECT_VERSION} 配置摘要 ==========")
message(STATUS "  构建类型      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ 标准      : ${CMAKE_CXX_STANDARD}")
message(STATUS "  编译器        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  测试          : ${STRATUM_BUILD_TESTS}")
message(STATUS "  CLI           : ${STRATUM_BUILD_CLI}")
message(STATUS "  基准          : ${STRATUM_BUILD_BENCHMARKS}")
message(STATUS "  Sanitizers    : ${STRATUM_ENABLE_SANITIZERS}")
message(STATUS "=====================================================")
message(STATUS "")
