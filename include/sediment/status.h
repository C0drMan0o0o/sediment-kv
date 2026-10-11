#pragma once

#include <string>
#include <string_view>

namespace sediment {

// Result of an operation. Errors are returned, never thrown, so the public API
// stays exception-free. Cheap to pass around when OK (empty message).
class [[nodiscard]] Status {
 public:
  enum class Code {
    kOk,
    kNotFound,
    kCorruption,
    kIOError,
    kInvalidArgument,
  };

  Status() = default;

  static Status OK() { return Status(); }
  static Status NotFound(std::string_view msg = {}) {
    return Status(Code::kNotFound, msg);
  }
  static Status Corruption(std::string_view msg = {}) {
    return Status(Code::kCorruption, msg);
  }
  static Status IOError(std::string_view msg = {}) {
    return Status(Code::kIOError, msg);
  }
  static Status InvalidArgument(std::string_view msg = {}) {
    return Status(Code::kInvalidArgument, msg);
  }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  bool IsInvalidArgument() const { return code_ == Code::kInvalidArgument; }

  Code code() const { return code_; }
  std::string_view message() const { return message_; }

  // "OK", "NotFound", or "NotFound: <message>".
  std::string ToString() const;

  friend bool operator==(const Status&, const Status&) = default;

 private:
  Status(Code code, std::string_view msg) : code_(code), message_(msg) {}

  Code code_ = Code::kOk;
  std::string message_;
};

}  // namespace sediment
