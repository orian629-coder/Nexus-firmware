#pragma once

#include <optional>
#include <string>
#include <utility>

#include "core/ErrorCodes.h"

namespace nexus::core {

// Status / Result<T> are the return type for anything that can fail. Errors never cross module
// boundaries as exceptions — callers inspect the returned status. This keeps fault handling
// explicit and the isolation guarantees enforceable.

class Status {
 public:
  Status() = default;  // ok
  Status(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}
  explicit Status(ErrorCode code) : code_(code), message_(std::string(describe(code))) {}

  static Status success() { return Status{}; }
  static Status error(ErrorCode code, std::string message) {
    return Status{code, std::move(message)};
  }

  bool ok() const { return code_ == ErrorCode::Ok; }
  explicit operator bool() const { return ok(); }

  ErrorCode code() const { return code_; }
  const std::string& message() const { return message_; }

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
};

// A value-or-error container. Holds a T on success, or a (non-ok) Status on failure.
template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}                          // NOLINT: implicit ok
  Result(Status status) : status_(std::move(status)) {}                  // NOLINT: implicit err
  Result(ErrorCode code, std::string message) : status_(code, std::move(message)) {}

  bool ok() const { return status_.ok(); }
  explicit operator bool() const { return ok(); }

  const Status& status() const { return status_; }
  ErrorCode code() const { return status_.code(); }

  T& value() { return *value_; }
  const T& value() const { return *value_; }
  T value_or(T fallback) const { return value_ ? *value_ : std::move(fallback); }

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace nexus::core
