#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "sediment/block_builder.h"
#include "sediment/block_reader.h"
#include "sediment/file.h"
#include "sediment/memtable.h"
#include "sediment/status.h"

namespace sediment {

// Location of a block inside a table file. `size` includes the block's own
// trailing CRC.
struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;

  friend bool operator==(const BlockHandle&, const BlockHandle&) = default;
};

// Fixed-size last bytes of every table file:
//
//   index handle   fixed64 offset, fixed64 size
//   version        fixed32
//   magic          fixed64
//
// A reader finds it at (file_size - kEncodedLength) and needs nothing else to
// locate the index block.
struct Footer {
  static constexpr size_t kEncodedLength = 28;
  static constexpr uint32_t kCurrentVersion = 1;
  // Spells "sediment" in the file, because fixed64 is little-endian.
  static constexpr uint64_t kMagic = 0x746e656d69646573;

  BlockHandle index_handle;
  uint32_t version = kCurrentVersion;

  void EncodeTo(std::string* dst) const;
  // `input` must be exactly kEncodedLength bytes. Returns Corruption on a
  // wrong length, magic number or unsupported version.
  static Status DecodeFrom(std::string_view input, Footer* footer);
};

struct TableBuilderOptions {
  // A data block is finished once it reaches this many bytes.
  size_t block_size = 4096;
  int restart_interval = BlockBuilder::kDefaultRestartInterval;
  // Must match the order in which keys are added.
  KeyComparator comparator = BytewiseCompare;
};

// Writes a sorted run of key/value pairs as an SSTable:
//
//   data block 0 ... data block N-1
//   index block                    one entry per data block
//   footer                         see Footer
//
// Each block is a BlockBuilder block. The index block maps the last key of a
// data block to that block's encoded handle (varint64 offset, varint64 size),
// so a reader can binary search it for the first block that may hold a key.
// The index uses the exact last key rather than a shortened separator.
//
// Keys must be added in strictly increasing comparator order (asserted in
// debug builds). Errors are sticky: after the first failed write every call
// is a no-op and status() reports it. The builder does not own the file and
// does not sync or close it. Single-threaded.
class TableBuilder {
 public:
  TableBuilder(const TableBuilderOptions& options, WritableFile* file);

  // Must not be called after Finish().
  void Add(std::string_view key, std::string_view value);

  // Flush the last data block, then write the index block and footer.
  // Returns status(). Must be called at most once.
  Status Finish();

  const Status& status() const { return status_; }
  uint64_t NumEntries() const { return num_entries_; }
  // Bytes written to the file so far; the final file size after Finish().
  uint64_t FileSize() const { return offset_; }
  // Empty until the first Add().
  std::string_view smallest_key() const { return smallest_key_; }
  std::string_view largest_key() const { return largest_key_; }

 private:
  // Append a finished block, recording its position in `*handle`.
  void WriteBlock(std::string_view block, BlockHandle* handle);
  void FlushDataBlock();

  const TableBuilderOptions options_;
  WritableFile* const file_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  Status status_;
  uint64_t offset_ = 0;
  uint64_t num_entries_ = 0;
  std::string smallest_key_;
  std::string largest_key_;
  bool finished_ = false;
};

// Summary of a table written by WriteMemTableToTable.
struct TableInfo {
  uint64_t file_size = 0;
  uint64_t num_entries = 0;
  std::string smallest_key;  // encoded internal keys
  std::string largest_key;
};

// Write every entry of `memtable`, in internal-key order, to a new table file
// at `path`, then sync and close it. Keys in the table are encoded internal
// keys, so `options.comparator` is ignored and CompareInternalKeys is used.
// On failure the partial file is left for the caller to remove.
Status WriteMemTableToTable(const MemTable& memtable,
                            const TableBuilderOptions& options,
                            const std::string& path, TableInfo* info);

}  // namespace sediment
