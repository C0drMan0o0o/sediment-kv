#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "sediment/internal_key.h"
#include "sediment/memtable.h"
#include "sediment/status.h"
#include "sediment/wal.h"

namespace sediment {

// A WAL record carries one write:
//
//   [sequence: fixed64][type: 1][key_len: varint32][key][value_len:
//   varint32][value]
//
// Append the encoding of one write to `dst`.
void EncodeLogEntry(std::string* dst, SequenceNumber sequence, ValueType type,
                    std::string_view key, std::string_view value);

// What replaying a log found.
struct RecoveryStats {
  uint64_t records = 0;              // Entries applied to the memtable.
  SequenceNumber last_sequence = 0;  // Highest sequence applied, 0 if none.
  uint64_t dropped_bytes = 0;        // Bytes discarded at a damaged tail.
  wal::StopReason stop_reason = wal::StopReason::kNone;
};

// Replay the log at `path` into `memtable`, in order. A damaged tail is not an
// error: replay stops at the first bad frame and reports it in `stats`.
// Returns non-OK if the file cannot be opened or read, or if an intact frame
// holds an undecodable entry (Corruption), since the CRC rules out a torn
// write as the cause.
Status RecoverFromLog(const std::string& path, MemTable* memtable,
                      RecoveryStats* stats);

}  // namespace sediment
