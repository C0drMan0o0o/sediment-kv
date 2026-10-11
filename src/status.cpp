#include "sediment/status.h"

namespace sediment {

namespace {

std::string_view CodeName(Status::Code code) {
  switch (code) {
    case Status::Code::kOk:
      return "OK";
    case Status::Code::kNotFound:
      return "NotFound";
    case Status::Code::kCorruption:
      return "Corruption";
    case Status::Code::kIOError:
      return "IOError";
    case Status::Code::kInvalidArgument:
      return "InvalidArgument";
  }
  return "Unknown";
}

}  // namespace

std::string Status::ToString() const {
  std::string result(CodeName(code_));
  if (!message_.empty()) {
    result += ": ";
    result += message_;
  }
  return result;
}

}  // namespace sediment
