#include "sediment/filename.h"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <limits>

namespace sediment {
namespace {

constexpr std::string_view kWalSuffix = ".wal";
constexpr std::string_view kTableSuffix = ".sst";

std::string NumberedFileName(const std::string& dbname, uint64_t number,
                             std::string_view suffix) {
  // 20 digits hold any uint64_t.
  std::array<char, 21> digits{};
  std::snprintf(digits.data(), digits.size(), "%06" PRIu64, number);
  std::string name = dbname;
  name += '/';
  name += digits.data();
  name += suffix;
  return name;
}

}  // namespace

std::string WalFileName(const std::string& dbname, uint64_t number) {
  return NumberedFileName(dbname, number, kWalSuffix);
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  return NumberedFileName(dbname, number, kTableSuffix);
}

std::string LockFileName(const std::string& dbname) {
  return dbname + "/LOCK";
}

bool ParseFileName(std::string_view name, uint64_t* number, FileType* type) {
  FileType parsed_type{};
  if (name.ends_with(kWalSuffix)) {
    parsed_type = FileType::kWal;
    name.remove_suffix(kWalSuffix.size());
  } else if (name.ends_with(kTableSuffix)) {
    parsed_type = FileType::kTable;
    name.remove_suffix(kTableSuffix.size());
  } else {
    return false;
  }
  if (name.empty()) {
    return false;
  }
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
  uint64_t value = 0;
  for (const char c : name) {
    if (c < '0' || c > '9') {
      return false;
    }
    const auto digit = static_cast<uint64_t>(c - '0');
    if (value > (kMax - digit) / 10) {
      return false;  // Overflow.
    }
    value = (value * 10) + digit;
  }
  *number = value;
  *type = parsed_type;
  return true;
}

}  // namespace sediment
