#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "sediment/file.h"
#include "sediment/internal_key.h"
#include "sediment/memtable.h"
#include "sediment/options.h"
#include "sediment/status.h"
#include "sediment/wal.h"

namespace sediment {

// A database stored in one directory. Only one DB may have a directory open at
// a time, across all processes. Not thread-safe.
class DB {
 public:
  // Open the database in directory `path`, creating the directory (but not its
  // parents) if it is missing. Replays every existing write-ahead log, oldest
  // first, then starts a new one. Fails with IOError if another DB holds the
  // directory, and with Corruption if a log is damaged anywhere but its tail.
  // On failure `*db` is null.
  static Status Open(const Options& options, const std::string& path,
                     std::unique_ptr<DB>* db);

  // Set `key` to `value`. The write is in the log before it is applied, and
  // is synced first if Options::sync_writes is set. Returns InvalidArgument if
  // the key is 4 GiB - 8 bytes or longer or the value is 4 GiB or longer. After
  // a failed log write every later write fails with the same error. A write
  // that returns an IOError may still be in the log and reappear on reopen.
  Status Put(std::string_view key, std::string_view value);

  // Remove `key`, if present. Logged and synced as Put is.
  Status Delete(std::string_view key);

  // Set `*value` to the newest value of `key`. Returns NotFound if the key was
  // never written or its newest write is a Delete.
  Status Get(std::string_view key, std::string* value) const;

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  DB(DB&&) = delete;
  DB& operator=(DB&&) = delete;
  ~DB() = default;

 private:
  DB(const Options& options, std::unique_ptr<FileLock> lock,
     std::unique_ptr<wal::Writer> log, std::unique_ptr<MemTable> mem,
     SequenceNumber last_sequence);

  Status Write(ValueType type, std::string_view key, std::string_view value);

  const Options options_;
  // Declared before the log so it is released after the log is closed.
  std::unique_ptr<FileLock> lock_;
  std::unique_ptr<wal::Writer> log_;
  std::unique_ptr<MemTable> mem_;
  // Sequence number of the newest write, 0 before the first.
  SequenceNumber last_sequence_;
  // Encoded log entry being written, kept to reuse its capacity.
  std::string entry_;
};

}  // namespace sediment
