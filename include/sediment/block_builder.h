#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sediment {

// Builds one SSTable block of sorted key/value entries. The block layout is
//
//   entry 0 ... entry N-1
//   restart[0] ... restart[R-1]    fixed32 offsets of entries stored in full
//   num_restarts                   fixed32
//   masked crc32c                  fixed32, covers every preceding byte
//
// and each entry is
//
//   [shared: varint32][non_shared: varint32][value_length: varint32]
//   [key[shared..]: non_shared bytes][value: value_length bytes]
//
// where `shared` is the length of the prefix the key has in common with the
// previous key. Every `restart_interval` entries the prefix sharing is reset
// (shared = 0) and the entry's offset is recorded, so a reader can binary
// search the restart points and then scan at most `restart_interval` entries.
//
// The builder does not compare keys: the caller must add them in the order
// its comparator defines, and prefix compression is bytewise. Single-threaded.
class BlockBuilder {
 public:
  static constexpr int kDefaultRestartInterval = 16;

  // `restart_interval` must be at least 1.
  explicit BlockBuilder(int restart_interval = kDefaultRestartInterval);

  // Append an entry. Must not be called after Finish() until Reset().
  void Add(std::string_view key, std::string_view value);

  // Append the restart array and CRC and return the finished block. The view
  // stays valid until Reset() or the builder is destroyed.
  std::string_view Finish();

  // Return to the empty state so the builder can be reused.
  void Reset();

  // Size in bytes Finish() would return if called now.
  size_t CurrentSizeEstimate() const;

  bool empty() const { return buffer_.empty(); }

 private:
  const int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int entries_since_restart_ = 0;
  std::string last_key_;
  bool finished_ = false;
};

}  // namespace sediment
