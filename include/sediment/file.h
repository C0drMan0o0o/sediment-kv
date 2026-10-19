#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "sediment/status.h"

namespace sediment {

// Thin wrappers over POSIX file descriptors. Every operation returns a Status;
// nothing throws. Files close their descriptor on destruction, but only an
// explicit Close() reports a failure.

// A file opened for appending. Data goes straight to write(2): it reaches the
// operating system's page cache but is not durable until Sync() succeeds.
class WritableFile {
 public:
  virtual ~WritableFile() = default;

  virtual Status Append(std::string_view data) = 0;

  // Force everything appended so far to stable storage. On macOS this uses
  // fcntl(F_FULLFSYNC); see DESIGN.md.
  virtual Status Sync() = 0;

  virtual Status Close() = 0;
};

// A file read from start to end.
class SequentialFile {
 public:
  virtual ~SequentialFile() = default;

  // Read up to `n` bytes into `scratch`, which must hold at least `n` bytes.
  // `*result` views the bytes read (inside `scratch`); it is empty at end of
  // file. A short result is not an error.
  virtual Status Read(size_t n, char* scratch, std::string_view* result) = 0;

  // Skip `n` bytes. Skipping past the end of file is not an error.
  virtual Status Skip(uint64_t n) = 0;
};

// A file read at arbitrary offsets. Reads do not move any shared cursor.
class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;

  // Read up to `n` bytes starting at `offset` into `scratch`, which must hold
  // at least `n` bytes. `*result` views the bytes read (inside `scratch`); it
  // is shorter than `n` only at end of file.
  virtual Status Read(uint64_t offset, size_t n, char* scratch,
                      std::string_view* result) const = 0;
};

// Create (or truncate) `path` for writing.
Status NewWritableFile(const std::string& path,
                       std::unique_ptr<WritableFile>* result);

// Open the existing file `path` for sequential reading.
Status NewSequentialFile(const std::string& path,
                         std::unique_ptr<SequentialFile>* result);

// Open the existing file `path` for random-access reading.
Status NewRandomAccessFile(const std::string& path,
                           std::unique_ptr<RandomAccessFile>* result);

}  // namespace sediment
