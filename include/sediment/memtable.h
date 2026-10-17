#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "sediment/arena.h"
#include "sediment/internal_key.h"
#include "sediment/skiplist.h"

namespace sediment {

// In-memory write buffer: a skiplist of internal keys over an Arena.
// Entries are never modified or removed; an overwrite or delete adds a newer
// version, and Get picks the newest version visible at the given sequence.
// Single writer; not thread-safe.
class MemTable {
 public:
  enum class GetResult {
    kFound,     // a value is visible; *value was set
    kDeleted,   // a tombstone is the newest visible version
    kNotFound,  // no version of the key is visible here
  };

  MemTable();
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Requires: (sequence, key) was not added before. For kDeletion, `value`
  // is ignored in lookups (normally empty).
  void Add(SequenceNumber sequence, ValueType type, std::string_view key,
           std::string_view value);

  // Looks up the newest version of `key` with sequence <= `sequence`.
  // Sets *value only on kFound.
  GetResult Get(std::string_view key, SequenceNumber sequence,
                std::string* value) const;

  // Bytes held by the arena, including slack. Used to decide when to flush.
  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

 private:
  // Entry layout in the arena:
  //   varint32 internal_key_len | internal key | varint32 value_len | value
  // The skiplist stores a pointer to the start of each entry.
  struct EntryComparator {
    int operator()(const char* a, const char* b) const;
  };
  using Table = SkipList<const char*, EntryComparator>;

  Arena arena_;
  Table table_;
};

}  // namespace sediment
