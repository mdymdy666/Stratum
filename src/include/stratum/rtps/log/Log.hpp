// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 最小日志桩：替代 Fast DDS 的 fastdds/dds/log/Log.hpp
//
// 移植说明（ref-05）：Fast DDS 的日志系统依赖单例、线程、文件句柄等，
// 对 Stratum 的线格式编解码而言完全不需要。这里提供**语法兼容**的最小桩：
//   - 保留 EPROSIMA_LOG_* 宏名与流式用法
//   - 编译期丢弃（no-op），零运行时开销
// 需要真实日志时由 Stratum 自己的日志设施接管。

#ifndef STRATUM_RTPS_LOG_LOG_HPP_
#define STRATUM_RTPS_LOG_LOG_HPP_

#define EPROSIMA_LOG_ERROR(cat, msg) ((void)0)
#define EPROSIMA_LOG_WARNING(cat, msg) ((void)0)
#define EPROSIMA_LOG_INFO(cat, msg) ((void)0)
#define EPROSIMA_LOG_WARN(cat, msg) ((void)0)

// 少数代码使用 EPROSIMA_LOG_*_FILTER 变体
#define EPROSIMA_LOG_ERROR_FILTER(cat, ctx, msg) ((void)0)
#define EPROSIMA_LOG_WARNING_FILTER(cat, ctx, msg) ((void)0)

#endif  // STRATUM_RTPS_LOG_LOG_HPP_
