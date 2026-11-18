#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "sediment/block_reader.h"
#include "sediment/file.h"
#include "sediment/internal_key.h"
#include "sediment/status.h"
#include "sediment/table_builder.h"

namespace sediment {

// Read-only access to a table file written by TableBuilder.
//
// Open() reads the footer and the whole index block into memory. A lookup
// binary searches that index for the one data block that may hold the key,
// reads just that block from the file and checks its CRC. Corrupt or truncated
// files give Corruption (or IOError for a failed read), never garbage.
//
// The comparator must order keys the way the table's writer did. Thread-safe
// after Open(): lookups only read shared state and each call has its own
// buffers.
class Table {
 public:
  // Outcome of an internal-key lookup; see Get(user_key, sequence, ...).
  enum class LookupResult {
    kFound,     // a value is visible; *value was set
    kDeleted,   // a tombstone is the newest visible version
    kNotFound,  // no version of the key is visible in this table
  };

  class Iterator;

  // `file_size` is the table file's length in bytes. Takes ownership of
  // `file` on success; on failure `*table` is untouched.
  static Status Open(std::unique_ptr<RandomAccessFile> file, uint64_t file_size,
                     KeyComparator comparator, std::unique_ptr<Table>* table);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  // Find the first entry whose key is >= `target` in comparator order and
  // copy it into `*key` and `*value`. Returns NotFound if every key is smaller.
  Status Seek(std::string_view target, std::string* key,
              std::string* value) const;

  // Look up an entry whose key equals `key` exactly. Returns OK and fills
  // `*value`, or NotFound if the table has no such key.
  Status Get(std::string_view key, std::string* value) const;

  // For a table of internal keys (opened with CompareInternalKeys): find the
  // newest version of `user_key` with sequence <= `sequence`. Sets `*result`,
  // and `*value` only on kFound. kNotFound means "ask the next, older source";
  // kDeleted means "stop, the key is gone". Returns non-OK only for Corruption
  // or IOError, in which case `*result` is not set.
  Status Get(std::string_view user_key, SequenceNumber sequence,
             std::string* value, LookupResult* result) const;

  // Ordered iterator over every entry. The table must outlive it.
  Iterator NewIterator() const;

 private:
  Table(std::unique_ptr<RandomAccessFile> file, uint64_t data_size,
        KeyComparator comparator, std::string index_contents);

  // Read a block, bounds-checked against the data area of the file.
  Status ReadBlock(const BlockHandle& handle, std::string* contents) const;

  const std::unique_ptr<RandomAccessFile> file_;
  // Bytes before the footer; every block must lie inside them.
  const uint64_t data_size_;
  const KeyComparator comparator_;
  const std::string index_contents_;
  // Views index_contents_, which is why Table is not copyable or movable.
  const BlockReader index_;
};

// Two-level iterator: an iterator over the index block picks each data block
// in turn, and a block iterator walks the entries inside it. Only one data
// block is held in memory at a time, read (and its CRC checked) when the
// iterator first enters it.
//
// key() and value() stay valid until the next call that moves the iterator.
// Check status() after Valid() turns false: a damaged block or a failed read
// stops iteration with Corruption or IOError, and the error is sticky.
// Movable, not copyable. Single-threaded, but a table may have any number of
// iterators at once.
class Table::Iterator {
 public:
  bool Valid() const { return data_ != nullptr && data_->it.Valid(); }
  const Status& status() const { return status_; }

  // Position at the first entry.
  void SeekToFirst();
  // Position at the first entry whose key is >= `target`.
  void Seek(std::string_view target);
  // Advance to the next entry. Requires Valid().
  void Next();

  // Requires: Valid().
  std::string_view key() const { return data_->it.key(); }
  // Requires: Valid().
  std::string_view value() const { return data_->it.value(); }

 private:
  friend class Table;
  explicit Iterator(const Table* table);

  // The current data block. Kept on the heap because the block iterator
  // points at the reader and the reader views the contents, so their
  // addresses must not change when Table::Iterator is moved.
  struct DataBlock {
    DataBlock(std::string block_contents, KeyComparator comparator)
        : contents(std::move(block_contents)),
          reader(contents, comparator),
          it(reader.NewIterator()) {}
    DataBlock(const DataBlock&) = delete;
    DataBlock& operator=(const DataBlock&) = delete;

    const std::string contents;
    const BlockReader reader;
    BlockReader::Iterator it;
  };

  // Load the data block the index iterator points at, leaving its iterator
  // unpositioned. Clears data_ at the end of the index or on an error.
  void LoadDataBlock();
  // While the current block is exhausted, move on to the next non-empty one.
  void SkipExhaustedBlocks();
  void Fail(Status status);

  const Table* table_;
  BlockReader::Iterator index_it_;
  std::unique_ptr<DataBlock> data_;
  Status status_;
};

}  // namespace sediment
