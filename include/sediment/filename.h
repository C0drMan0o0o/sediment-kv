#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sediment {

// Files in a database directory are named by a number, zero-padded to six
// digits, and an extension giving their type: 000123.wal, 000045.sst. Numbers
// come from one counter, so a WAL and a table never share one.
enum class FileType : uint8_t {
  kWal,
  kTable,
};

std::string WalFileName(const std::string& dbname, uint64_t number);
std::string TableFileName(const std::string& dbname, uint64_t number);

// The file DB::Open locks so that only one handle uses the directory.
std::string LockFileName(const std::string& dbname);

// Parse a numbered file name (without its directory), such as "000123.wal".
// Returns false for any other name, including LOCK.
bool ParseFileName(std::string_view name, uint64_t* number, FileType* type);

}  // namespace sediment
