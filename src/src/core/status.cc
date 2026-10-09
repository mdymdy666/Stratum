// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// Status 实现

#include "stratum/core/status.h"

namespace stratum {

const char* StatusCodeName(StatusCode code) {
  switch (code) {
    case StatusCode::kOk:
      return "OK";
    case StatusCode::kNotFound:
      return "NotFound";
    case StatusCode::kCorruption:
      return "Corruption";
    case StatusCode::kNotSupported:
      return "NotSupported";
    case StatusCode::kInvalidArgument:
      return "InvalidArgument";
    case StatusCode::kIOError:
      return "IOError";
    case StatusCode::kOutOfSpace:
      return "OutOfSpace";
    case StatusCode::kAlreadyExists:
      return "AlreadyExists";
    case StatusCode::kSnapshotReleased:
      return "SnapshotReleased";
    case StatusCode::kShutdownInProgress:
      return "ShutdownInProgress";
    case StatusCode::kTimeout:
      return "Timeout";
    case StatusCode::kInternal:
      return "Internal";
  }
  return "Unknown";
}

std::string Status::ToString() const {
  std::string result = StatusCodeName(code_);
  if (!message_.empty()) {
    result += ": ";
    result += message_;
  }
  return result;
}

}  // namespace stratum
