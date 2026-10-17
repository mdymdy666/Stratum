# ---------------------------------------------------------------------------
# stratum-config —— 独立、零依赖的配置库
#
# 从 Stratum 提取。**唯一依赖是 C++17 标准库 + pthread**（std::thread 需要）。
#
# 两种使用方式：
#
#   ① 作为子目录（推荐，无需安装）
#        add_subdirectory(third_party/stratum-config)
#        target_link_libraries(myapp PRIVATE stratum_config)
#
#   ② 独立构建后安装
#        cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
#        cmake --build build --target install
#        # 然后 find_package(stratum_config REQUIRED)
#        #      target_link_libraries(myapp PRIVATE stratum_config::stratum_config)
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(stratum_config VERSION 1.0.0 LANGUAGES CXX)

# 作为子目录被引入时不覆盖父项目的标准
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

if(NOT DEFINED CMAKE_POSITION_INDEPENDENT_CODE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

add_library(stratum_config STATIC
    src/config.cpp
)
add_library(stratum_config::stratum_config ALIAS stratum_config)

target_include_directories(stratum_config PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# std::thread 需要 pthread
find_package(Threads REQUIRED)
target_link_libraries(stratum_config PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# 使用示例（默认不编译；独立构建时可用 -DSTRATUM_CONFIG_BUILD_EXAMPLES=ON 打开）
# ---------------------------------------------------------------------------
option(STRATUM_CONFIG_BUILD_EXAMPLES "构建示例程序" OFF)
if(STRATUM_CONFIG_BUILD_EXAMPLES)
    add_executable(config_example examples/basic.cc)
    target_link_libraries(config_example PRIVATE stratum_config)
endif()

# ---------------------------------------------------------------------------
# 测试（不依赖任何外部测试框架，用 assert 风格的 mini runner）
# ---------------------------------------------------------------------------
option(STRATUM_CONFIG_BUILD_TESTS "构建测试" OFF)
if(STRATUM_CONFIG_BUILD_TESTS)
    enable_testing()
    add_executable(test_config tests/test_config.cc)
    target_link_libraries(test_config PRIVATE stratum_config)
    add_test(NAME test_config COMMAND test_config)
endif()

# ---------------------------------------------------------------------------
# 安装规则（方式 ② 用）
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS stratum_config
        EXPORT stratum_configTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT stratum_configTargets
        FILE stratum_configTargets.cmake
        NAMESPACE stratum_config::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/stratum_config)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/stratum_configConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stratum_configConfig.cmake"
"include(\"${CMAKE_CURRENT_BINARY_DIR}/stratum_configTargets.cmake\")\n")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/stratum_configConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/stratum_configConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/stratum_config)
