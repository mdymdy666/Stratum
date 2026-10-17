// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// ValueCodec<log::Level> —— **Stratum 侧**为独立配置库提供的类型特化。
//
// stratum-config 是一个通用库，不认识 log::Level 这种业务枚举。
// 它的设计是：使用者为自己的类型特化 ValueCodec（见 standalone/config/README.md）。
// 本文件就是 Stratum 作为"使用者"提供的特化。
//
// 【为什么单独一个头文件，而不是塞回 config.hpp】
// 特化注入必须在**使用点之前可见**。放进 config.hpp 会让通用库又依赖 Stratum 的日志模块，
// 那等于把刚切断的耦合重新接上。
//
// 用法：在任何需要 `Lookup<log::Level>` 或 `ValueCodec<log::Level>` 的 TU 里
//
//     #include "stratum/config/config.hpp"
//     #include "stratum/config/level_codec.hpp"     // 必须在 config.hpp 之后
//
// 注意：**必须在模板被实例化之前 include**，否则仍会走主模板而编译失败
// （主模板要求 operator<< / operator>>，log::Level 没有）。

#ifndef STRATUM_CONFIG_LEVEL_CODEC_HPP_
#define STRATUM_CONFIG_LEVEL_CODEC_HPP_

#include <string>

#include "stratum/config/config.hpp"
#include "stratum/logging/log.hpp"

namespace stratum {
namespace config {

template <>
struct ValueCodec<log::Level> {
    /** @brief 编码日志级别为文本。 */
    static std::string Encode(log::Level value) { return log::ToString(value); }
    /** @brief 从文本解析日志级别；无法识别时返回 false（配置项保持旧值）。 */
    static bool Decode(const std::string& text, log::Level* value) {
        return log::ParseLevel(text, value);
    }
};

}  // namespace config
}  // namespace stratum

#endif  // STRATUM_CONFIG_LEVEL_CODEC_HPP_
