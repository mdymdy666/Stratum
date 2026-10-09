// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 统一状态码。
//
// 设计原则：
//  - 不依赖任何第三方（不引入 LevelDB 的 Status、不用 std::error_code）。
//  - 值语义、可拷贝、零分配（除 message 外）。
//  - 存储层高频返回，必须便宜。

#ifndef STRATUM_CORE_STATUS_H_
#define STRATUM_CORE_STATUS_H_

#include <cstdint>
#include <string>
#include <utility>

namespace stratum {

enum class StatusCode : uint8_t {
  kOk = 0,
  kNotFound,           // 键不存在
  kCorruption,         // 数据损坏（校验失败、格式非法）
  kNotSupported,       // 后端不支持该操作
  kInvalidArgument,    // 调用方传参非法
  kIOError,            // 文件/磁盘错误
  kOutOfSpace,         // 空间不足
  kAlreadyExists,      // 目标已存在（如打开已打开的库）
  kSnapshotReleased,   // 引用了已释放的快照
  kShutdownInProgress, // 关闭中，拒绝新请求
  kTimeout,            // 操作超时
  kInternal,           // 内部不变量被破坏（bug）
};

const char* StatusCodeName(StatusCode code);

// ---------------------------------------------------------------------------
// Status：轻量结果类型
//
// 用法：
//   Status s = engine->Put(...);
//   if (!s.ok()) { ... s.ToString() ... }
//
// 性能说明：OK 状态不分配内存（message_ 为空）。
// ---------------------------------------------------------------------------
class Status {
 public:
  Status() = default;  // kOk

  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  static Status OK() { return Status(); }
  static Status NotFound(std::string msg = {}) {
    return Status(StatusCode::kNotFound, std::move(msg));
  }
  static Status Corruption(std::string msg) {
    return Status(StatusCode::kCorruption, std::move(msg));
  }
  static Status NotSupported(std::string msg) {
    return Status(StatusCode::kNotSupported, std::move(msg));
  }
  static Status InvalidArgument(std::string msg) {
    return Status(StatusCode::kInvalidArgument, std::move(msg));
  }
  static Status IOError(std::string msg) {
    return Status(StatusCode::kIOError, std::move(msg));
  }
  static Status OutOfSpace(std::string msg) {
    return Status(StatusCode::kOutOfSpace, std::move(msg));
  }
  static Status AlreadyExists(std::string msg) {
    return Status(StatusCode::kAlreadyExists, std::move(msg));
  }
  static Status SnapshotReleased(std::string msg = {}) {
    return Status(StatusCode::kSnapshotReleased, std::move(msg));
  }
  static Status ShutdownInProgress(std::string msg = {}) {
    return Status(StatusCode::kShutdownInProgress, std::move(msg));
  }
  static Status Timeout(std::string msg = {}) {
    return Status(StatusCode::kTimeout, std::move(msg));
  }
  static Status Internal(std::string msg) {
    return Status(StatusCode::kInternal, std::move(msg));
  }

  bool ok() const { return code_ == StatusCode::kOk; }
  bool IsNotFound() const { return code_ == StatusCode::kNotFound; }
  bool IsCorruption() const { return code_ == StatusCode::kCorruption; }
  bool IsNotSupported() const { return code_ == StatusCode::kNotSupported; }
  bool IsIOError() const { return code_ == StatusCode::kIOError; }
  bool IsOutOfSpace() const { return code_ == StatusCode::kOutOfSpace; }

  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }

  std::string ToString() const;

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

// 便于测试与日志的宏
#define STRATUM_RETURN_IF_ERROR(expr)      \
  do {                                     \
    ::stratum::Status _s = (expr);         \
    if (!_s.ok()) {                        \
      return _s;                           \
    }                                      \
  } while (0)

}  // namespace stratum

#endif  // STRATUM_CORE_STATUS_H_
