// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 本文件移植自 Go2Cpp（MIT License）
//   Copyright (c) 2026 Go2Cpp contributors
//   Ported to Stratum on 2026-10-15
//   改动内容见 docs/10-porting-notes.md
//   上游许可全文见 THIRD_PARTY_NOTICES.md 第 5 节
//
// 注意：MIT 许可要求保留上述版权声明，请勿删除。

#include "stratum/config.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iostream>
#include <string_view>
#include <thread>

namespace stratum::config {
namespace {

/// 函数功能：完成 Trim 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] value 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

/// 函数功能：完成 ParseSize 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] text 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] value 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool ParseSize(const std::string& text, std::size_t* value) {
    if (!value) return false;
    const auto clean = Trim(text);
    if (clean.empty()) return false;
    std::size_t parsed = 0;
    const auto result = std::from_chars(clean.data(), clean.data() + clean.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != clean.data() + clean.size()) return false;
    *value = parsed;
    return true;
}

/// 函数功能：完成 ParseBool 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] text 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] value 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool ParseBool(const std::string& text, bool* value) {
    if (!value) return false;
    auto clean = Trim(text);
    std::transform(clean.begin(), clean.end(), clean.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    if (clean == "1" || clean == "true" || clean == "yes" || clean == "on") *value = true;
    else if (clean == "0" || clean == "false" || clean == "no" || clean == "off") *value = false;
    else return false;
    return true;
}

// 配置文件是外部输入。这里集中定义上限，避免某一个模块单独放宽
// 参数后造成过量线程、内存或超长等待。0 只在文档明确说明时表示自动。
constexpr std::size_t kMaxWorkers = 32U;
constexpr std::size_t kMaxLocalQueue = 1U << 20U;
constexpr std::size_t kMinFiberStack = 16U * 1024U;
constexpr std::size_t kMaxFiberStack = 64U * 1024U * 1024U;
constexpr std::size_t kMaxAffinityBudget = 1U << 20U;
constexpr auto kMaxDuration = std::chrono::hours(24);
constexpr std::size_t kMaxDurationMilliseconds =
    static_cast<std::size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(kMaxDuration).count());

bool ValidateSchedulerSize(const char* key, std::size_t value,
                           std::string* error) {
    const auto fail = [error, key](const std::string& reason) {
        if (error) *error = std::string("scheduler.") + key + reason;
        return false;
    };
    if (std::string_view(key) == "processor_count" ||
        std::string_view(key) == "min_workers" ||
        std::string_view(key) == "max_workers") {
        return value == 0 || value <= kMaxWorkers
                   ? true
                   : fail(" 不能超过 32");
    }
    if (std::string_view(key) == "local_queue_limit") {
        if (value == 0) return fail(" 必须大于 0");
        return value <= kMaxLocalQueue ? true : fail(" 不能超过 1048576");
    }
    if (std::string_view(key) == "fiber_stack_size") {
        if (value == 0) return true;
        if (value < kMinFiberStack) return fail(" 小于 16384 时可能无法建立安全栈");
        return value <= kMaxFiberStack ? true : fail(" 不能超过 67108864");
    }
    if (std::string_view(key) == "task_affinity_budget") {
        return value <= kMaxAffinityBudget ? true : fail(" 不能超过 1048576");
    }
    if (std::string_view(key) == "fiber_bin_capacity") {
        return value <= 4096U ? true : fail(" 不能超过 4096");
    }
    return true;
}

/// 函数功能：完成 ValidateDuration 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] key 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] value 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] error 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool ValidateDuration(const char* key, std::size_t value, std::string* error) {
    if (value == 0) {
        if (error) *error = std::string("scheduler.") + key + " 必须大于 0 毫秒";
        return false;
    }
    if (value > kMaxDurationMilliseconds) {
        if (error) *error = std::string("scheduler.") + key + " 不能超过 24 小时";
        return false;
    }
    return true;
}

bool ReadSize(const IniFile& ini, const char* section, const char* key,
              std::size_t* target, std::string* error) {
    if (!ini.Has(section, key)) return true;
    if (!ParseSize(ini.Get(section, key), target)) {
        if (error) *error = std::string("配置项不是非负整数: ") + section + "." + key;
        return false;
    }
    // 静态配置和热更新必须使用同一套范围规则；这样错误会在读取具体字段时
    // 报告，而不是等到调度器已经部分初始化后才发现。
    //
    // 注意：ValidateSchedulerSize 只认识 scheduler 段的键名，其它段（例如
    // timer 的 warn_after_days / error_after_days）传入时既不在它的白名单里
    // 也不该被它拒绝。因此这里必须按段分流：只有 scheduler 段才走那套范围
    // 规则，其它段解析成功即成功。
    //
    // 修复前写成「if (section == "scheduler" && Validate(...)) return true;
    // return false;」——非 scheduler 段会跳过 return true 直落 return false，
    // 导致 [timer] 段的值永远无法被读取，而且失败时不设置 error（静默失败）。
    if (std::string_view(section) == "scheduler") {
        return ValidateSchedulerSize(key, *target, error);
    }
    return true;
}

// 文件被编辑器直接截断并重写时，监听线程可能恰好读到半个文件。用
// “元数据相同 + 内容相同”的两次快照确认稳定状态；确认失败只重试，绝不
// 把半文件提交给配置中心。生产环境仍建议写临时文件后 rename 替换。
struct FileStamp final {
    std::filesystem::file_time_type modified{};
    std::uintmax_t size{0};

    bool operator==(const FileStamp& other) const noexcept {
        return modified == other.modified && size == other.size;
    }
};

bool ReadFileStamp(const std::string& path, FileStamp* stamp,
                   std::string* error) {
    if (!stamp) return false;
    std::error_code ec;
    stamp->modified = std::filesystem::last_write_time(path, ec);
    if (ec) {
        if (error) *error = "无法读取配置文件时间戳: " + path;
        return false;
    }
    stamp->size = std::filesystem::file_size(path, ec);
    if (ec) {
        if (error) *error = "无法读取配置文件大小: " + path;
        return false;
    }
    return true;
}

/// 函数功能：完成 EditLockPath 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] path 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
std::string EditLockPath(const std::string& path) {
    return path + ".lock";
}

// 配置写入方通过 path + ".lock" 声明编辑事务：锁文件非空表示配置仍在
// 修改，空文件或不存在表示可以读取。这里只检查文件大小，不读取锁文件
// 内容，避免监听线程因为锁文件本身被截断而误判为已提交。
/// 函数功能：完成 IsEditLocked 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] path 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] locked 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] error 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool IsEditLocked(const std::string& path, bool* locked, std::string* error) {
    if (!locked) return false;
    const auto lock_path = EditLockPath(path);
    std::error_code ec;
    const bool exists = std::filesystem::exists(lock_path, ec);
    if (ec) {
        if (error) *error = "无法检查配置编辑锁: " + lock_path;
        return false;
    }
    if (!exists) {
        *locked = false;
        return true;
    }
    const auto size = std::filesystem::file_size(lock_path, ec);
    if (ec) {
        if (error) *error = "无法读取配置编辑锁大小: " + lock_path;
        return false;
    }
    *locked = size != 0;
    return true;
}

bool ReadFileContents(const std::string& path, std::string* contents,
                      std::string* error) {
    if (!contents) return false;
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream) {
        if (error) *error = "无法打开配置文件: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream.good() && !stream.eof()) {
        if (error) *error = "读取配置文件失败: " + path;
        return false;
    }
    *contents = buffer.str();
    return true;
}

bool ReadStableFileContents(const std::string& path, std::string* contents,
                            std::string* error) {
    constexpr int kAttempts = 8;
    constexpr auto kRetryDelay = std::chrono::milliseconds(2);
    std::string last_error;
    bool lock_seen = false;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        bool edit_locked = false;
        if (!IsEditLocked(path, &edit_locked, &last_error)) {
            std::this_thread::sleep_for(kRetryDelay);
            continue;
        }
        if (edit_locked) {
            lock_seen = true;
            std::this_thread::sleep_for(kRetryDelay);
            continue;
        }
        FileStamp before;
        std::string first;
        FileStamp after;
        if (ReadFileStamp(path, &before, &last_error) &&
            ReadFileContents(path, &first, &last_error) &&
            ReadFileStamp(path, &after, &last_error) && before == after) {
            bool locked_after_read = false;
            if (!IsEditLocked(path, &locked_after_read, &last_error)) {
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            if (locked_after_read) {
                lock_seen = true;
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            FileStamp verify_before;
            std::string second;
            FileStamp verify_after;
            if (ReadFileStamp(path, &verify_before, &last_error) &&
                ReadFileContents(path, &second, &last_error) &&
                ReadFileStamp(path, &verify_after, &last_error) &&
                after == verify_before && verify_before == verify_after &&
                first == second) {
                bool locked_after_verify = false;
                if (!IsEditLocked(path, &locked_after_verify, &last_error)) {
                    std::this_thread::sleep_for(kRetryDelay);
                    continue;
                }
                if (locked_after_verify) {
                    lock_seen = true;
                    std::this_thread::sleep_for(kRetryDelay);
                    continue;
                }
                *contents = std::move(second);
                return true;
            }
        }
        std::this_thread::sleep_for(kRetryDelay);
    }
    if (error) {
        *error = lock_seen
                     ? "配置文件正在编辑，请先清空编辑锁: " + EditLockPath(path)
                     : "配置文件在读取期间仍在变化，请使用临时文件加 rename 原子替换: " +
                           path;
        if (!last_error.empty()) *error += "（" + last_error + "）";
    }
    return false;
}

}  // namespace

/// 函数功能：完成 Load 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] path 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] error 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool IniFile::Load(const std::string& path, std::string* error) {
    std::string contents;
    return ReadStableFileContents(path, &contents, error) &&
           Parse(contents, error);
}

/// 函数功能：完成 Parse 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] text 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] error 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool IniFile::Parse(const std::string& text, std::string* error) {
    // 先构造临时快照，只有完整解析成功才替换当前内容；语法错误不能
    // 让调用方看到“前半段已更新、后半段丢失”的半解析状态。
    decltype(m_sections) parsed_sections;
    std::string current = "global";
    std::istringstream stream(text);
    std::string line; std::size_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        auto clean = Trim(line);
        if (clean.empty() || clean.front() == '#' || clean.front() == ';') continue;
        if (clean.front() == '[') {
            if (clean.back() != ']') { if (error) *error = "节标题缺少 ]，行 " + std::to_string(line_number); return false; }
            current = Trim(clean.substr(1, clean.size() - 2));
            if (current.empty()) { if (error) *error = "节标题为空，行 " + std::to_string(line_number); return false; }
            continue;
        }
        const auto equal = clean.find('=');
        if (equal == std::string::npos) { if (error) *error = "配置项缺少 =，行 " + std::to_string(line_number); return false; }
        const auto key = Trim(clean.substr(0, equal));
        auto value = Trim(clean.substr(equal + 1));
        const auto comment = value.find_first_of("#;");
        if (comment != std::string::npos) value = Trim(value.substr(0, comment));
        for (std::size_t position = 0; (position = value.find("\\n", position)) != std::string::npos;) {
            value.replace(position, 2, "\n");
            ++position;
        }
        if (key.empty()) { if (error) *error = "配置键为空，行 " + std::to_string(line_number); return false; }
        parsed_sections[current][key] = value;
    }
    m_sections.swap(parsed_sections);
    return true;
}

std::string IniFile::Get(const std::string& section, const std::string& key,
                         std::string fallback) const {
    const auto section_it = m_sections.find(section);
    if (section_it == m_sections.end()) return fallback;
    const auto value_it = section_it->second.find(key);
    return value_it == section_it->second.end() ? fallback : value_it->second;
}

/// 函数功能：完成 Has 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] section 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] key 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool IniFile::Has(const std::string& section, const std::string& key) const {
    const auto section_it = m_sections.find(section);
    return section_it != m_sections.end() && section_it->second.find(key) != section_it->second.end();
}/// 函数功能：完成 Validate 调用，读取或更新相关运行时状态。/// 函数功能：完成 ApplyLogging 调用，读取或更新相关运行时状态。/// 函数功能：完成 LoadRuntimeConfig 调用，读取或更新相关运行时状态。/// 函数功能：完成 Instance 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
Config& Config::Instance() {
    static Config instance;
    return instance;
}

Config::~Config() { StopWatcher(); }

/// 函数功能：完成 LookupBase 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] name 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
ConfigVarBase::ptr Config::LookupBase(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_vars.find(name);
    return it == m_vars.end() ? nullptr : it->second;
}

/// 函数功能：完成 LoadFromIni 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] ini 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] error 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool Config::LoadFromIni(const IniFile& ini, std::string* error) {
    // 预检和提交必须处于同一事务临界区。否则两个监听线程可能分别
    // 通过预检，随后交错 ApplyString，破坏“整批配置一次发布”的约定。
    std::unique_lock<std::mutex> transaction_lock(m_transaction_mutex);
    const auto variables = List();
    // 先检查所有已注册项对应的文本能否转换；实际写入仍按变量顺序完成，
    // 这样错误能指出具体变量，同时不要求所有类型都暴露内部临时值。
    std::vector<std::pair<ConfigVarBase::ptr, std::string>> pending;
    std::map<std::string, std::string> candidates;
    for (const auto& variable : variables) {
        if (!variable) continue;
        candidates.emplace(variable->name(), variable->ToString());
        const auto separator = variable->name().find('.');
        const auto section = separator == std::string::npos ? "global" : variable->name().substr(0, separator);
        const auto key = separator == std::string::npos ? variable->name() : variable->name().substr(separator + 1);
        if (!ini.Has(section, key)) continue;
        const auto text = ini.Get(section, key);
        if (!variable->ValidateString(text, error)) return false;
        candidates[variable->name()] = text;
        pending.emplace_back(variable, text);
    }
    // 单字段校验只能检查类型和取值范围，跨字段关系必须基于本次文件的
    // 完整候选快照判断。校验发生在任何 ApplyString 之前，失败时不留下半套值。
    std::map<std::uint64_t, TransactionValidator> validators;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        validators = m_transaction_validators;
    }
    for (const auto& [id, validator] : validators) {
        (void)id;
        if (validator && !validator(candidates, error)) return false;
    }
    // 第二阶段只写入变量，不触发监听器。这样 scheduler/log 等模块不会
    // 看到“只更新了一半”的配置组合；全部变量写入成功后再统一广播。
    std::vector<ConfigVarBase::ptr> applied;
    applied.reserve(pending.size());
    try {
        for (const auto& [variable, text] : pending) {
            if (!variable->ApplyString(text, error)) {
                for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
                    (*it)->RollbackPending();
                }
                return false;
            }
            if (variable->HasPending()) applied.push_back(variable);
        }
        // 所有变量已经完成第一阶段提交；通知阶段不持有事务锁，监听器可以
        // 查询注册表或触发下一次配置加载，避免插件回调与事务锁形成死锁。
        transaction_lock.unlock();
        // 监听器读取其他变量时只能看到本次文件提交后的值，不会读到本次
        // ApplyString 顺序造成的半套状态。并发的新事务可能在此之后开始，
        // 这是有意的：配置值已经完整发布，回调不阻塞配置刷新线程。
        for (const auto& variable : applied) variable->NotifyPending();
        if (!applied.empty()) {
            std::map<std::uint64_t, CommitListener> listeners;
            try {
                std::lock_guard<std::mutex> lock(m_mutex);
                listeners = m_commit_listeners;
            } catch (...) {
                // 仅影响提交后的插件通知，不影响已经提交的配置值。
                listeners.clear();
            }
            for (const auto& [id, listener] : listeners) {
                (void)id;
                if (!listener) continue;
                try {
                    listener(applied);
                } catch (...) {
                    // 配置插件只能观察已提交快照；单个插件失败时继续
                    // 执行其余插件，绝不能让配置监听线程退出。
                }
            }
        }
        return true;
    } catch (...) {
        for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
            (*it)->RollbackPending();
        }
        throw;
    }
}

/// 函数功能：完成 LoadFromFile 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] path 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] error 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool Config::LoadFromFile(const std::string& path, std::string* error) {
    IniFile ini;
    return ini.Load(path, error) && LoadFromIni(ini, error);
}

/// 函数功能：完成 List 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
std::vector<ConfigVarBase::ptr> Config::List() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ConfigVarBase::ptr> result;
    result.reserve(m_vars.size());
    for (const auto& [name, variable] : m_vars) result.push_back(variable);
    return result;
}

/// 函数功能：注册配置事务提交后的扩展回调。
/// 执行流程：
/// 1. 校验回调对象；
/// 2. 加锁复制并登记回调；
/// 3. 返回稳定的监听器 ID，供调用方稍后注销。
/// @param[in] listener 事务完成后接收变更变量列表的回调。
/// @return 可传给 DelCommitListener 的唯一 ID。
std::uint64_t Config::AddCommitListener(CommitListener listener) {
    const auto id = m_next_commit_listener.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commit_listeners.emplace(id, std::move(listener));
    return id;
}

/// 函数功能：注销配置事务提交回调。
/// 执行流程：获取注册表锁并删除指定 ID；不存在时保持幂等。
/// @param[in] id AddCommitListener 返回的监听器 ID。
/// @return 无返回值。
void Config::DelCommitListener(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commit_listeners.erase(id);
}

/// 函数功能：注册配置文件事务候选快照校验器。
/// @param[in] validator 接收本次候选键值并返回校验结果的回调。
/// @return 可传给 DelTransactionValidator 的唯一 ID。
std::uint64_t Config::AddTransactionValidator(TransactionValidator validator) {
    const auto id = m_next_transaction_validator.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transaction_validators.emplace(id, std::move(validator));
    return id;
}

/// 函数功能：删除配置文件事务候选快照校验器。
/// @param[in] id AddTransactionValidator 返回的校验器 ID。
/// @return 无返回值；不存在的 ID 按幂等方式处理。
void Config::DelTransactionValidator(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transaction_validators.erase(id);
}

/// 函数功能：完成 StartWatcher 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] path 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] interval 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] error 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool Config::StartWatcher(const std::string& path, std::chrono::milliseconds interval, std::string* error) {
    if (path.empty() || interval <= std::chrono::milliseconds::zero()) {
        if (error) *error = "配置监听路径或周期无效";
        return false;
    }
    std::lock_guard<std::mutex> lifecycle_lock(m_watcher_lifecycle_mutex);
    stop_watcher_locked();
    bool edit_locked = false;
    if (!IsEditLocked(path, &edit_locked, error)) return false;
    if (edit_locked) {
        if (error) {
            *error = "配置文件正在编辑，请先清空编辑锁: " + EditLockPath(path);
        }
        return false;
    }
    FileStamp initial_stamp;
    if (!ReadFileStamp(path, &initial_stamp, error)) {
        return false;
    }
    if (!LoadFromFile(path, error)) return false;
    m_watching.store(true, std::memory_order_release);
    m_watcher = std::thread([this, path, interval, initial_stamp]() mutable {
        auto stamp = initial_stamp;
        // 锁从非空变为空时必须强制尝试一次，即使文件系统的时间戳和大小
        // 没有变化；这样同尺寸原子替换也不会被旧基准短路。
        bool retry_pending = false;
        while (m_watching.load(std::memory_order_acquire)) {
            // 条件变量让停止监听操作立即唤醒，不必等待一个完整刷新周期。
            std::unique_lock<std::mutex> wait_lock(m_watcher_mutex);
            m_watcher_cv.wait_for(wait_lock, interval, [this] {
                return !m_watching.load(std::memory_order_acquire);
            });
            wait_lock.unlock();
            if (!m_watching.load(std::memory_order_acquire)) break;
            bool edit_locked = false;
            std::string lock_error;
            if (!IsEditLocked(path, &edit_locked, &lock_error)) {
                std::cerr << "Stratum 配置编辑锁检查失败: " << lock_error << '\n';
                continue;
            }
            if (edit_locked) {
                retry_pending = true;
                continue;
            }
            FileStamp current;
            std::string stamp_error;
            if (!ReadFileStamp(path, &current, &stamp_error) ||
                (current == stamp && !retry_pending)) {
                continue;
            }
            std::string error;
            try {
                if (LoadFromFile(path, &error)) {
                    stamp = current;
                    retry_pending = false;
                } else {
                    std::cerr << "Stratum 配置热加载失败: " << error << '\n';
                    retry_pending = true;
                }
            } catch (const std::exception& exception) {
                // 监听器属于用户扩展点，异常不能逃出配置监听线程；
                // 记录本次失败后继续监视后续文件变化。
                std::cerr << "Stratum 配置监听器异常: " << exception.what()
                          << '\n';
                retry_pending = true;
            } catch (...) {
                std::cerr << "Stratum 配置监听器抛出未知异常\n";
                retry_pending = true;
            }
        }
    });
    return true;
}

/// 函数功能：完成 stop_watcher_locked 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void Config::stop_watcher_locked() noexcept {
    m_watching.store(false, std::memory_order_release);
    m_watcher_cv.notify_all();
    if (m_watcher.joinable()) {
        if (m_watcher.get_id() == std::this_thread::get_id()) m_watcher.detach();
        else m_watcher.join();
    }
}

/// 函数功能：完成 StopWatcher 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] 无；该函数仅使用所属对象或线程局部状态。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
void Config::StopWatcher() {
    std::lock_guard<std::mutex> lifecycle_lock(m_watcher_lifecycle_mutex);
    stop_watcher_locked();
}

/// 函数功能：完成 BindLoggingConfig 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] config 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] error 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
bool BindLoggingConfig(Config& config, std::string* error) {
    auto level = config.Lookup<log::Level>("log.level", log::Level::Warn, "日志最低级别");
    auto stdout_enabled = config.Lookup<bool>("log.stdout", false, "是否输出到 stdout");
    auto directory = config.Lookup<std::string>("log.directory", "log", "日志目录");
    auto file = config.Lookup<std::string>("log.file", "stratum.log", "日志文件名");
    auto format = config.Lookup<std::string>("log.format", "{time} [{level}] {logger} ({file}:{line}) {message}\n", "日志格式");
    if (!level || !stdout_enabled || !directory || !file || !format) {
        if (error) *error = "日志配置变量类型冲突";
        return false;
    }
    const std::weak_ptr<ConfigVar<log::Level>> weak_level = level;
    const std::weak_ptr<ConfigVar<bool>> weak_stdout = stdout_enabled;
    const std::weak_ptr<ConfigVar<std::string>> weak_directory = directory;
    const std::weak_ptr<ConfigVar<std::string>> weak_file = file;
    const std::weak_ptr<ConfigVar<std::string>> weak_format = format;
    const auto apply = [weak_level, weak_stdout, weak_directory, weak_file, weak_format]() {
        const auto current_level = weak_level.lock();
        const auto current_stdout = weak_stdout.lock();
        const auto current_directory = weak_directory.lock();
        const auto current_file = weak_file.lock();
        const auto current_format = weak_format.lock();
        if (!current_level || !current_stdout || !current_directory || !current_file || !current_format) return;
        log::LoggerManager::Instance().Configure(current_level->GetValue(), current_stdout->GetValue(),
                                                  current_directory->GetValue(), current_file->GetValue(), current_format->GetValue());
    };
    level->AddListener([apply](const log::Level&, const log::Level&) { apply(); });
    stdout_enabled->AddListener([apply](const bool&, const bool&) { apply(); });
    directory->AddListener([apply](const std::string&, const std::string&) { apply(); });
    file->AddListener([apply](const std::string&, const std::string&) { apply(); });
    format->AddListener([apply](const std::string&, const std::string&) { apply(); });
    apply();
    return true;
}}  // namespace stratum::config