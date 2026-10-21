#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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

// Why a Reader stopped before the end of the file.
enum class StopReason : uint8_t {
  kNone,  // Not stopped, or the log ended cleanly on a frame boundary.
  kTruncatedHeader,   // The file ends partway through a frame header.
  kTruncatedPayload,  // The file ends partway through a frame payload.
  kBadType,      // Unknown or reserved type byte (including a zeroed tail).
  kBadChecksum,  // The CRC does not match the type and payload.
  kIOError,      // The underlying read failed; see Reader::status().
};

// Reads records back from a log file written by Writer.
//
// Recovery stops at the first damaged frame: a torn write can only damage the
// tail, and with length-prefixed frames there is no way to find the next valid
// one. Everything from that frame on is counted in dropped_bytes() and is never
// returned. Damage is not an error: ReadRecord() returns false and status()
// stays OK; status() is non-OK only if the file itself could not be read.
// Single-threaded.
class Reader {
 public:
  explicit Reader(std::unique_ptr<SequentialFile> file);

  // Read the next intact record into `*record`. Returns false at the end of
  // the log, at the first damaged frame, or on an I/O error.
  bool ReadRecord(std::string* record);

  StopReason stop_reason() const { return stop_reason_; }

  // Bytes at and after the first damaged frame, 0 if the log ended cleanly.
  uint64_t dropped_bytes() const { return dropped_bytes_; }

  const Status& status() const { return status_; }

 private:
  // Read until `n` bytes arrive or the file ends; returns the count read.
  // Records an I/O failure in status_.
  size_t ReadFully(size_t n, char* dst);
  bool Stop(StopReason reason, uint64_t consumed);

  std::unique_ptr<SequentialFile> file_;
  Status status_;
  StopReason stop_reason_ = StopReason::kNone;
  uint64_t dropped_bytes_ = 0;
  bool done_ = false;
};

}  // namespace sediment::wal
