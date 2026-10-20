#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "sediment/file.h"
#include "sediment/status.h"

namespace sediment::wal {

// Each record is framed as
//
//   [masked crc32c: 4][length: 4][type: 1][payload: length]
//
// with the fixed-width fields little-endian. The CRC covers the type byte and
// the payload, so a flipped bit anywhere in a record other than the length
// field is caught; a corrupt length is caught because the CRC will not match
// the bytes it then selects. Records are length-prefixed rather than split
// across fixed-size blocks, so a record of any size is a single frame; see
// DESIGN.md.
inline constexpr size_t kHeaderSize = 4 + 4 + 1;
inline constexpr size_t kMaxPayloadSize = UINT32_MAX;

enum class RecordType : uint8_t {
  // Reserved so that a zero-filled tail (a common result of a torn write) is
  // never mistaken for a valid record.
  kZero = 0,
  kFull = 1,
};

// Appends records to a log file. Single-threaded.
class Writer {
 public:
  // `file` is owned by the writer and should be freshly created.
  explicit Writer(std::unique_ptr<WritableFile> file);

  // Append one record. If `sync` is true, force it to stable storage before
  // returning; otherwise it is durable only after a later Sync() or sync=true.
  // After a failed append the log may end in a partial record, so the writer
  // refuses further appends and returns the same error.
  Status AddRecord(std::string_view payload, bool sync = false);

  Status Sync();
  Status Close();

 private:
  std::unique_ptr<WritableFile> file_;
  Status error_;
};

}  // namespace sediment::wal
