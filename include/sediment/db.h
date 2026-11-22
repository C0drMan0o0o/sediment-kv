#pragma once

#include <memory>
#include <string>

#include "sediment/file.h"
#include "sediment/options.h"
#include "sediment/status.h"
#include "sediment/wal.h"

namespace sediment {

// A database stored in one directory. Only one DB may have a directory open at
// a time, across all processes.
class DB {
 public:
  // Open the database in directory `path`, creating the directory (but not its
  // parents) if it is missing, and start a new write-ahead log. Fails with
  // IOError if another DB holds the directory. On failure `*db` is null.
  static Status Open(const Options& options, const std::string& path,
                     std::unique_ptr<DB>* db);

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  DB(DB&&) = delete;
  DB& operator=(DB&&) = delete;
  ~DB() = default;

 private:
  DB(std::unique_ptr<FileLock> lock, std::unique_ptr<wal::Writer> log);

  // Declared first so it is released last, after the log is closed.
  std::unique_ptr<FileLock> lock_;
  std::unique_ptr<wal::Writer> log_;
};

}  // namespace sediment
