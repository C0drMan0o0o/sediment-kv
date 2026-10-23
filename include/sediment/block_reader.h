#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "sediment/status.h"

namespace sediment {

// Three-way key comparison returning <0, 0, >0 like strcmp. It must order
// keys the same way the BlockBuilder's caller did when adding them.
using KeyComparator = int (*)(std::string_view a, std::string_view b);

// Bytewise lexicographic comparison; the default.
int BytewiseCompare(std::string_view a, std::string_view b);

// Read-only view of a block produced by BlockBuilder::Finish(). The block
// does not copy `contents`, which must outlive the reader and any iterator.
//
// Construction verifies the trailing CRC and the restart array bounds; if
// either is wrong, status() is Corruption and iterators are never valid.
class BlockReader {
 public:
  class Iterator;

  explicit BlockReader(std::string_view contents,
                       KeyComparator comparator = BytewiseCompare);

  const Status& status() const { return status_; }

  Iterator NewIterator() const;

 private:
  friend class Iterator;

  std::string_view contents_;  // entries only, trailer stripped
  const char* restarts_ = nullptr;
  uint32_t num_restarts_ = 0;
  KeyComparator comparator_;
  Status status_;
};

// Ordered iterator over a block. key() and value() stay valid until the next
// call that moves the iterator. Check status() after Valid() turns false: a
// malformed entry stops iteration with Corruption rather than reading out of
// bounds. Single-threaded.
class BlockReader::Iterator {
 public:
  bool Valid() const { return valid_; }
  const Status& status() const { return status_; }

  // Position at the first entry.
  void SeekToFirst();
  // Position at the first entry whose key is >= `target`.
  void Seek(std::string_view target);
  // Advance to the next entry. Requires Valid().
  void Next();

  std::string_view key() const { return key_; }
  std::string_view value() const { return value_; }

 private:
  friend class BlockReader;
  explicit Iterator(const BlockReader* block);

  uint32_t RestartOffset(uint32_t index) const;
  // Jump to the restart point; the next ParseNext() reads its entry.
  void SeekToRestart(uint32_t index);
  // Decode the entry at next_ into key_/value_. Returns false at the end of
  // the block or on corruption (which sets status_).
  bool ParseNext();
  void Invalidate(Status status);

  const BlockReader* block_;
  size_t next_ = 0;  // offset of the entry after the current one
  bool valid_ = false;
  std::string key_;
  std::string_view value_;
  Status status_;
};

}  // namespace sediment
